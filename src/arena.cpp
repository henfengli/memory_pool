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

// Fast bitmap scan using ctzll to skip entire words of all-ones.
// Starts from chunk->next_free_hint to avoid rescanning already-full regions.
static int find_free_pages(ChunkHeader* chunk, uint32_t count) {
    uint32_t hint = chunk->next_free_hint;
    if (hint >= MP_USABLE_PAGES) hint = 0;

    // Scan starting from hint
    uint32_t run = 0;
    uint32_t start = hint;

    for (uint32_t i = hint; i < MP_USABLE_PAGES; ) {
        uint32_t word = i / 64;
        uint32_t bit = i % 64;

        // Fast skip: if we're at bit 0 and the entire word is full, skip 64 pages at once
        if (bit == 0 && count <= 64 && chunk->page_bitmap[word] == ~0ULL) {
            run = 0;
            i += 64;
            start = i;
            continue;
        }

        if (chunk->page_bitmap[word] & (1ULL << bit)) {
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

            if (bit == 0 && count <= 64 && chunk->page_bitmap[word] == ~0ULL) {
                run = 0;
                i += 64;
                start = i;
                continue;
            }

            if (chunk->page_bitmap[word] & (1ULL << bit)) {
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

static void mark_pages_allocated(ChunkHeader* chunk, uint32_t start, uint32_t count) {
    for (uint32_t i = start; i < start + count; i++) {
        uint32_t word = i / 64;
        uint32_t bit = i % 64;
        chunk->page_bitmap[word] |= (1ULL << bit);
    }
}

static void mark_pages_free(ChunkHeader* chunk, uint32_t start, uint32_t count) {
    for (uint32_t i = start; i < start + count; i++) {
        uint32_t word = i / 64;
        uint32_t bit = i % 64;
        chunk->page_bitmap[word] &= ~(1ULL << bit);
    }
    // Update hint: freed pages are good candidates for next allocation
    if (start < chunk->next_free_hint) {
        chunk->next_free_hint = start;
    }
}

void* arena_alloc_pages(Arena* arena, uint32_t bucket_idx, uint32_t count, uint64_t thread_id) {
    std::lock_guard<std::mutex> lock(arena->mutex);

    size_t blk_size = sc_block_size(bucket_idx);
    uint32_t blk_per_page = sc_blocks_per_page(bucket_idx);

    // Try existing chunks first
    for (ChunkHeader* chunk = arena->chunk_head; chunk; chunk = chunk->next) {
        int start = find_free_pages(chunk, count);
        if (start >= 0) {
            mark_pages_allocated(chunk, (uint32_t)start, count);
            // Advance hint past allocated region
            chunk->next_free_hint = (uint32_t)start + count;

            for (uint32_t i = 0; i < count; i++) {
                PageMeta& pm = chunk->pages[start + i];
                pm.block_size = (uint16_t)blk_size;
                pm.bucket_idx = (uint16_t)bucket_idx;
                pm.block_count = (uint16_t)blk_per_page;
                pm.used_count.store(0, std::memory_order_relaxed);
                pm.owner_thread = thread_id;
            }

            void* ptr = page_start(chunk, (uint32_t)start);
            arena->stat_page_alloc.fetch_add(count, std::memory_order_relaxed);
            return ptr;
        }
    }

    // No space in existing chunks, allocate a new one
    ChunkHeader* new_chunk = arena_alloc_chunk(arena);
    if (!new_chunk) return nullptr;

    int start = find_free_pages(new_chunk, count);
    if (start < 0) return nullptr;

    mark_pages_allocated(new_chunk, (uint32_t)start, count);
    new_chunk->next_free_hint = (uint32_t)start + count;

    for (uint32_t i = 0; i < count; i++) {
        PageMeta& pm = new_chunk->pages[start + i];
        pm.block_size = (uint16_t)blk_size;
        pm.bucket_idx = (uint16_t)bucket_idx;
        pm.block_count = (uint16_t)blk_per_page;
        pm.used_count.store(0, std::memory_order_relaxed);
        pm.owner_thread = thread_id;
    }

    arena->stat_page_alloc.fetch_add(count, std::memory_order_relaxed);
    return page_start(new_chunk, (uint32_t)start);
}

void arena_free_pages(Arena* arena, void* ptr, uint32_t count) {
    ChunkHeader* chunk = chunk_of(ptr);
    if (chunk->magic != MP_CHUNK_MAGIC || chunk->arena != arena) {
        return;
    }

    uint32_t start = page_index_of(chunk, ptr);

    std::lock_guard<std::mutex> lock(arena->mutex);

    for (uint32_t i = 0; i < count; i++) {
        PageMeta& pm = chunk->pages[start + i];
        pm.block_size = 0;
        pm.bucket_idx = 0;
        pm.block_count = 0;
        pm.used_count.store(0, std::memory_order_relaxed);
        pm.owner_thread = 0;
    }

    mark_pages_free(chunk, start, count);
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
