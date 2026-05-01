#include "arena.h"
#include <cstring>

namespace mp {

Arena* arena_create(uint32_t id, const char* name, size_t max_size) {
    auto* arena = new Arena();
    arena->id = id;
    if (name) {
        strncpy(arena->name, name, sizeof(arena->name) - 1);
        arena->name[sizeof(arena->name) - 1] = '\0';
    } else {
        snprintf(arena->name, sizeof(arena->name), "arena_%u", id);
    }
    arena->chunk_head = nullptr;
    arena->max_size = max_size;
    arena->total_allocated = 0;
    arena->tlc_head = nullptr;
    arena->stat_page_alloc.store(0, std::memory_order_relaxed);
    arena->stat_page_free.store(0, std::memory_order_relaxed);
    arena->stat_chunk_count.store(0, std::memory_order_relaxed);
    arena->next_arena = nullptr;
    return arena;
}

void arena_destroy(Arena* arena) {
    if (!arena) return;
    ChunkHeader* chunk = arena->chunk_head;
    while (chunk) {
        ChunkHeader* next = chunk->next;
        platform::chunk_free(chunk, MP_CHUNK_SIZE);
        chunk = next;
    }
    arena->chunk_head = nullptr;
    arena->total_allocated = 0;
    arena->stat_chunk_count.store(0, std::memory_order_relaxed);
    delete arena;
}

// Caller must hold arena->mutex.
static ChunkHeader* arena_alloc_chunk(Arena* arena) {
    if (arena->max_size > 0 && arena->total_allocated + MP_CHUNK_SIZE > arena->max_size) {
        return nullptr;
    }

    void* mem = platform::chunk_alloc(MP_CHUNK_SIZE, MP_CHUNK_ALIGN);
    if (!mem) return nullptr;

    auto* chunk = static_cast<ChunkHeader*>(mem);
    chunk->magic = MP_CHUNK_MAGIC;
    chunk->arena = arena;
    chunk->next_free_hint = 0;
    memset(chunk->page_bitmap, 0, sizeof(chunk->page_bitmap));
    memset(chunk->pages, 0, sizeof(chunk->pages));

    chunk->next = arena->chunk_head;
    chunk->prev = nullptr;
    if (arena->chunk_head) {
        arena->chunk_head->prev = chunk;
    }
    arena->chunk_head = chunk;

    arena->total_allocated += MP_CHUNK_SIZE;
    arena->stat_chunk_count.fetch_add(1, std::memory_order_relaxed);

    return chunk;
}

// Platform-specific CTZ (Count Trailing Zeros) intrinsic
#ifdef _MSC_VER
#include <intrin.h>
static inline int ctz64(uint64_t x) {
    unsigned long index;
    return _BitScanForward64(&index, x) ? (int)index : 64;
}
#else
static inline int ctz64(uint64_t x) {
    return x ? __builtin_ctzll(x) : 64;
}
#endif

// CAS-based lock-free bitmap operations (inspired by mimalloc)
// Try to atomically find and claim `count` consecutive free bits in a single word.
// Returns the starting bit index on success, -1 on failure.
static int bitmap_try_claim_in_word(std::atomic<uint64_t>* word, uint32_t count, uint32_t bit_offset) {
    if (count == 0 || count > 64) return -1;

    uint64_t map = word->load(std::memory_order_relaxed);

    // Quick check: if word is full, skip
    if (map == ~0ULL) return -1;

    const uint64_t mask_template = (count >= 64) ? ~0ULL : ((1ULL << count) - 1);
    const uint32_t max_bitidx = 64 - count;

    // Start from bit_offset
    uint32_t bitidx = bit_offset;

#ifdef MEMPOOL_USE_CTZ
    // Use CTZ to quickly find first zero bit
    if (map != 0) {
        int first_zero = ctz64(~map);
        if (first_zero >= (int)bit_offset) {
            bitidx = first_zero;
        }
    }
#endif

    // Scan for free bit sequence
    while (bitidx <= max_bitidx) {
        uint64_t mask = mask_template << bitidx;

        // Check if these bits are free
        if ((map & mask) == 0) {
            // Try to claim with CAS
            uint64_t newmap = map | mask;
            if (word->compare_exchange_weak(map, newmap,
                                           std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
                // Success!
                return (int)bitidx;
            }
            // CAS failed, map is updated to current value, retry
            continue;
        }

        // These bits are occupied, move to next position
#ifdef MEMPOOL_USE_CTZ
        // Use CTZ to skip to next potential free bit
        uint64_t occupied = map & mask;
        if (occupied != 0) {
            int skip = 64 - ctz64(occupied) - bitidx;
            bitidx += (skip > 0) ? skip : 1;
        } else {
            bitidx++;
        }
#else
        bitidx++;
#endif
    }

    return -1;
}

// Try to atomically claim `count` consecutive pages starting from `start_page`.
// Returns true on success. This is used when we know exactly which pages to claim.
static bool bitmap_try_claim_at(ChunkHeader* chunk, uint32_t start_page, uint32_t count) {
    uint32_t end = start_page + count;
    uint32_t word_start = start_page / 64;
    uint32_t word_end = (end - 1) / 64;

    if (word_start == word_end) {
        // All bits in same word: single CAS
        uint32_t bit_lo = start_page % 64;
        uint64_t mask = ((count >= 64) ? ~0ULL : ((1ULL << count) - 1)) << bit_lo;

        uint64_t old = chunk->page_bitmap[word_start].load(std::memory_order_relaxed);
        // Check if bits are free
        if ((old & mask) != 0) return false;

        uint64_t newval = old | mask;
        return chunk->page_bitmap[word_start].compare_exchange_strong(
            old, newval, std::memory_order_acquire, std::memory_order_relaxed);
    } else {
        // Cross-word: not supported in lock-free mode, return false
        return false;
    }
}

// Atomically release `count` pages starting from `start_page`.
static void bitmap_release_pages(ChunkHeader* chunk, uint32_t start_page, uint32_t count) {
    uint32_t end = start_page + count;
    uint32_t word_start = start_page / 64;
    uint32_t word_end = (end - 1) / 64;

    if (word_start == word_end) {
        // Single word: atomic AND
        uint32_t bit_lo = start_page % 64;
        uint64_t mask = ((count >= 64) ? ~0ULL : ((1ULL << count) - 1)) << bit_lo;
        chunk->page_bitmap[word_start].fetch_and(~mask, std::memory_order_release);
    } else {
        // Multi-word: atomic AND on each word
        uint32_t first_bit = start_page % 64;
        chunk->page_bitmap[word_start].fetch_and(~(~0ULL << first_bit), std::memory_order_release);

        for (uint32_t w = word_start + 1; w < word_end; w++) {
            chunk->page_bitmap[w].store(0, std::memory_order_release);
        }

        uint32_t last_bits = end % 64;
        if (last_bits > 0) {
            chunk->page_bitmap[word_end].fetch_and(~((1ULL << last_bits) - 1), std::memory_order_release);
        } else {
            chunk->page_bitmap[word_end].store(0, std::memory_order_release);
        }
    }

    // Update hint atomically
    uint32_t old_hint = chunk->next_free_hint.load(std::memory_order_relaxed);
    while (start_page < old_hint) {
        if (chunk->next_free_hint.compare_exchange_weak(old_hint, start_page,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
            break;
        }
    }
}

// Fast bitmap scan using ctzll to skip entire words of all-ones.
// Starts from chunk->next_free_hint to avoid rescanning already-full regions.
// NOTE: This is now a read-only scan for atomic bitmap
#ifdef MEMPOOL_USE_CTZ
// CTZ-optimized version: use ctz64 to quickly find next free bit
static int find_free_pages(ChunkHeader* chunk, uint32_t count) {
    uint32_t hint = chunk->next_free_hint.load(std::memory_order_relaxed);
    if (hint >= MP_USABLE_PAGES) hint = 0;

    uint32_t run = 0;
    uint32_t start = hint;

    for (uint32_t i = hint; i < MP_USABLE_PAGES; ) {
        uint32_t word = i / 64;
        uint32_t bit = i % 64;
        uint64_t w = chunk->page_bitmap[word].load(std::memory_order_relaxed);

        // Fast skip: entire word is full
        if (bit == 0 && w == ~0ULL) {
            run = 0;
            i += 64;
            start = i;
            continue;
        }

        // Check current bit
        if (w & (1ULL << bit)) {
            // Allocated bit: use CTZ to find next free bit
            uint64_t mask = ~((1ULL << bit) - 1);  // Mask from current bit onwards
            uint64_t remaining = w & mask;

            if (remaining == mask) {
                // Rest of word is all 1s, skip to next word
                i = (word + 1) * 64;
            } else {
                // Find next 0 bit using CTZ
                int next_zero = ctz64(~remaining);
                i = word * 64 + next_zero;
            }
            run = 0;
            start = i;
        } else {
            // Free bit
            run++;
            if (run == count) {
                return (int)start;
            }
            i++;
        }
    }

    // Wrap around: scan [0, hint)
    if (hint > 0) {
        run = 0;
        start = 0;
        uint32_t limit = hint < MP_USABLE_PAGES ? hint : MP_USABLE_PAGES;
        for (uint32_t i = 0; i < limit; ) {
            uint32_t word = i / 64;
            uint32_t bit = i % 64;
            uint64_t w = chunk->page_bitmap[word].load(std::memory_order_relaxed);

            if (bit == 0 && w == ~0ULL) {
                run = 0;
                i += 64;
                start = i;
                continue;
            }

            if (w & (1ULL << bit)) {
                uint64_t mask = ~((1ULL << bit) - 1);
                uint64_t remaining = w & mask;

                if (remaining == mask) {
                    i = (word + 1) * 64;
                } else {
                    int next_zero = ctz64(~remaining);
                    i = word * 64 + next_zero;
                }
                run = 0;
                start = i;
            } else {
                run++;
                if (run == count) {
                    return (int)start;
                }
                i++;
            }
        }
    }

    return -1;
}
#else
// Original version: bit-by-bit scan
static int find_free_pages(ChunkHeader* chunk, uint32_t count) {
    uint32_t hint = chunk->next_free_hint.load(std::memory_order_relaxed);
    if (hint >= MP_USABLE_PAGES) hint = 0;

    // Scan starting from hint
    uint32_t run = 0;
    uint32_t start = hint;

    for (uint32_t i = hint; i < MP_USABLE_PAGES; ) {
        uint32_t word = i / 64;
        uint32_t bit = i % 64;

        uint64_t w = chunk->page_bitmap[word].load(std::memory_order_relaxed);

        // Fast skip: if we're at bit 0 and the entire word is full, skip 64 pages at once
        if (bit == 0 && count <= 64 && w == ~0ULL) {
            run = 0;
            i += 64;
            start = i;
            continue;
        }

        if (w & (1ULL << bit)) {
            run = 0;
            start = i + 1;
        } else {
            run++;
            if (run == count) {
                return (int)start;
            }
        }
        i++;
    }

    // Wrap around: scan [0, hint) if hint was not 0
    if (hint > 0) {
        run = 0;
        start = 0;
        uint32_t limit = hint < MP_USABLE_PAGES ? hint : MP_USABLE_PAGES;
        for (uint32_t i = 0; i < limit; ) {
            uint32_t word = i / 64;
            uint32_t bit = i % 64;

            uint64_t w = chunk->page_bitmap[word].load(std::memory_order_relaxed);

            if (bit == 0 && count <= 64 && w == ~0ULL) {
                run = 0;
                i += 64;
                start = i;
                continue;
            }

            if (w & (1ULL << bit)) {
                run = 0;
                start = i + 1;
            } else {
                run++;
                if (run == count) {
                    return (int)start;
                }
            }
            i++;
        }
    }

    return -1;
}
#endif

// Lock-free arena_alloc_pages: CAS bitmap + mutex only for new chunk allocation
void* arena_alloc_pages(Arena* arena, uint32_t bucket_idx, uint32_t count) {
    size_t blk_size = sc_info(bucket_idx).block_size;

    // Phase 1 (lock-free): try to CAS-claim pages in existing chunks
    for (ChunkHeader* chunk = arena->chunk_head; chunk; chunk = chunk->next) {
        // Find candidate position (read-only scan)
        int start = find_free_pages(chunk, count);
        if (start < 0) continue;

        // Try to atomically claim these pages via CAS
        if (bitmap_try_claim_at(chunk, (uint32_t)start, count)) {
            // CAS succeeded — we own these pages, no lock needed for PageMeta
            chunk->next_free_hint.store((uint32_t)start + count, std::memory_order_relaxed);

            for (uint32_t i = 0; i < count; i++) {
                PageMeta& pm = chunk->pages[start + i];
                pm.block_size = (uint16_t)blk_size;
                pm.bucket_idx = (uint16_t)bucket_idx;
                pm.owner_tlc = nullptr;
            }

            arena->stat_page_alloc.fetch_add(count, std::memory_order_relaxed);
            return page_start(chunk, (uint32_t)start);
        }
        // CAS failed — another thread claimed these pages, retry scan on same chunk
        // (find_free_pages will find a different position next time due to updated bitmap)
        start = find_free_pages(chunk, count);
        if (start >= 0 && bitmap_try_claim_at(chunk, (uint32_t)start, count)) {
            chunk->next_free_hint.store((uint32_t)start + count, std::memory_order_relaxed);

            for (uint32_t i = 0; i < count; i++) {
                PageMeta& pm = chunk->pages[start + i];
                pm.block_size = (uint16_t)blk_size;
                pm.bucket_idx = (uint16_t)bucket_idx;
                pm.owner_tlc = nullptr;
            }

            arena->stat_page_alloc.fetch_add(count, std::memory_order_relaxed);
            return page_start(chunk, (uint32_t)start);
        }
    }

    // Phase 2 (locked): no space in existing chunks, allocate a new one
    {
        std::lock_guard<std::mutex> lock(arena->mutex);

        // Double-check: another thread may have added a chunk while we waited
        for (ChunkHeader* chunk = arena->chunk_head; chunk; chunk = chunk->next) {
            int start = find_free_pages(chunk, count);
            if (start >= 0 && bitmap_try_claim_at(chunk, (uint32_t)start, count)) {
                chunk->next_free_hint.store((uint32_t)start + count, std::memory_order_relaxed);

                for (uint32_t i = 0; i < count; i++) {
                    PageMeta& pm = chunk->pages[start + i];
                    pm.block_size = (uint16_t)blk_size;
                    pm.bucket_idx = (uint16_t)bucket_idx;
                    pm.owner_tlc = nullptr;
                }

                arena->stat_page_alloc.fetch_add(count, std::memory_order_relaxed);
                return page_start(chunk, (uint32_t)start);
            }
        }

        // Allocate new chunk (under lock)
        ChunkHeader* new_chunk = arena_alloc_chunk(arena);
        if (!new_chunk) return nullptr;

        int start = find_free_pages(new_chunk, count);
        if (start < 0) return nullptr;

        // New chunk is not visible to other threads yet (just added to list under lock),
        // so direct claim is safe
        bitmap_try_claim_at(new_chunk, (uint32_t)start, count);
        new_chunk->next_free_hint.store((uint32_t)start + count, std::memory_order_relaxed);

        for (uint32_t i = 0; i < count; i++) {
            PageMeta& pm = new_chunk->pages[start + i];
            pm.block_size = (uint16_t)blk_size;
            pm.bucket_idx = (uint16_t)bucket_idx;
            pm.owner_tlc = nullptr;
        }

        arena->stat_page_alloc.fetch_add(count, std::memory_order_relaxed);
        return page_start(new_chunk, (uint32_t)start);
    }
}

// Lock-free arena_free_pages: atomic bitmap release, no mutex needed
void arena_free_pages(Arena* arena, void* ptr, uint32_t count) {
    ChunkHeader* chunk = chunk_of(ptr);
    if (chunk->magic != MP_CHUNK_MAGIC || chunk->arena != arena) {
        return;
    }

    uint32_t start = page_index_of(chunk, ptr);

    // Clear PageMeta (page is owned by caller, no contention)
    for (uint32_t i = 0; i < count; i++) {
        PageMeta& pm = chunk->pages[start + i];
        pm.block_size = 0;
        pm.bucket_idx = 0;
        pm.owner_tlc = nullptr;
    }

    // Atomic bitmap release (lock-free)
    bitmap_release_pages(chunk, start, count);
    arena->stat_page_free.fetch_add(count, std::memory_order_relaxed);

    // Note: we intentionally do NOT decommit pages here.
    // Keeping them committed avoids costly VirtualAlloc(MEM_COMMIT) on reuse.
    // Physical memory is returned to OS when the entire chunk is freed.
}

PageMeta* resolve_block_ptr(void* ptr, ChunkHeader** out_chunk) {
    if (!ptr) return nullptr;

    ChunkHeader* chunk = chunk_of(ptr);
    if (chunk->magic != MP_CHUNK_MAGIC) return nullptr;

    uintptr_t chunk_base = reinterpret_cast<uintptr_t>(chunk);
    uintptr_t usable_base = chunk_base + MP_HEADER_PAGES * MP_PAGE_SIZE;
    uintptr_t usable_end = chunk_base + MP_CHUNK_SIZE;

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < usable_base || addr >= usable_end) return nullptr;

    uint32_t page_idx = (uint32_t)((addr - usable_base) / MP_PAGE_SIZE);
    if (page_idx >= MP_USABLE_PAGES) return nullptr;

    PageMeta& pm = chunk->pages[page_idx];
    if (pm.block_size == 0) return nullptr;

    if (out_chunk) *out_chunk = chunk;
    return &pm;
}

} // namespace mp
