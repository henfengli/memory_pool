#include "tlc.h"
#include <cstring>
#include <new>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace mp {

// --- Platform TLS for TLC pointer ---
#ifdef _WIN32

static DWORD g_tlc_tls_index = TLS_OUT_OF_INDEXES;

static void init_tls_slot() {
    if (g_tlc_tls_index == TLS_OUT_OF_INDEXES) {
        g_tlc_tls_index = TlsAlloc();
    }
}

TLC* tlc_current() {
    if (g_tlc_tls_index == TLS_OUT_OF_INDEXES) return nullptr;
    return static_cast<TLC*>(TlsGetValue(g_tlc_tls_index));
}

void tlc_set_current(TLC* tlc) {
    init_tls_slot();
    TlsSetValue(g_tlc_tls_index, tlc);
}

#else // POSIX

static pthread_key_t g_tlc_key;
static pthread_once_t g_tlc_key_once = PTHREAD_ONCE_INIT;

static void make_tlc_key() {
    pthread_key_create(&g_tlc_key, nullptr);
}

TLC* tlc_current() {
    pthread_once(&g_tlc_key_once, make_tlc_key);
    return static_cast<TLC*>(pthread_getspecific(g_tlc_key));
}

void tlc_set_current(TLC* tlc) {
    pthread_once(&g_tlc_key_once, make_tlc_key);
    pthread_setspecific(g_tlc_key, tlc);
}

#endif

static void on_thread_exit(void* /*arg*/) {
    TLC* tlc = tlc_current();
    if (tlc) {
        tlc_flush(tlc);
        tlc_destroy(tlc);
        tlc_set_current(nullptr);
    }
}

// --- PageRange helper ---
static PageRange* page_range_alloc(void* ptr, uint32_t count, PageRange* next) {
    auto* pr = static_cast<PageRange*>(malloc(sizeof(PageRange)));
    pr->ptr = ptr;
    pr->count = count;
    pr->next = next;
    return pr;
}

static void page_range_free_list(PageRange* list) {
    while (list) {
        PageRange* next = list->next;
        free(list);
        list = next;
    }
}

TLC* tlc_get_or_create(Arena* arena) {
    TLC* tlc = tlc_current();
    if (tlc && tlc->arena == arena) return tlc;

    tlc = new(std::nothrow) TLC();
    if (!tlc) return nullptr;

    tlc->arena = arena;
    tlc->thread_id = platform::get_thread_id();
    memset(&tlc->stats, 0, sizeof(tlc->stats));

    for (uint32_t i = 0; i < MP_NUM_SIZE_CLASSES; i++) {
        Bucket& b = tlc->buckets[i];
        b.free_head = nullptr;
        b.local_free_head = nullptr;
        b.thread_free_head.store(nullptr, std::memory_order_relaxed);
        b.bucket_idx = (uint16_t)i;
        b.block_size = (uint16_t)sc_block_size(i);
        b.collect_count = 0;
        b.bump_ptr = nullptr;
        b.bump_limit = nullptr;
        b.page_list = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(arena->tlc_mutex);
        tlc->next_in_arena = arena->tlc_head;
        tlc->prev_in_arena = nullptr;
        if (arena->tlc_head) {
            arena->tlc_head->prev_in_arena = tlc;
        }
        arena->tlc_head = tlc;
    }

    tlc_set_current(tlc);
    platform::register_thread_exit_callback(on_thread_exit, tlc);

    return tlc;
}

// Calculate how many pages to request in one batch for a given size class.
static uint32_t calc_batch_pages(uint32_t blk_per_page) {
    if (blk_per_page >= MP_REFILL_BLOCKS) return 1;
    uint32_t pages = (MP_REFILL_BLOCKS + blk_per_page - 1) / blk_per_page;
    if (pages > 64) pages = 64;
    return pages;
}

// Check if a block belongs to a given page range [page_ptr, page_ptr + count*PAGE_SIZE)
static inline bool block_in_page(void* blk, void* page_ptr, uint32_t page_count) {
    uintptr_t a = reinterpret_cast<uintptr_t>(blk);
    uintptr_t base = reinterpret_cast<uintptr_t>(page_ptr);
    return a >= base && a < base + page_count * MP_PAGE_SIZE;
}

// Try to reclaim fully-freed pages from this bucket's page_list.
// A page is reclaimable when freed_count == block_count (all blocks returned).
// Called on slow path only (after collect), with frequency control.
static void bucket_try_reclaim_pages(Bucket* b, Arena* arena) {
    PageRange** pp = &b->page_list;
    while (*pp) {
        PageRange* pr = *pp;
        ChunkHeader* chunk = chunk_of(pr->ptr);
        uint32_t pi_start = page_index_of(chunk, pr->ptr);

        // Check if ALL pages in this range are fully freed
        bool all_freed = true;
        for (uint32_t i = 0; i < pr->count; i++) {
            PageMeta& pm = chunk->pages[pi_start + i];
            if (pm.freed_count < pm.block_count) {
                all_freed = false;
                break;
            }
        }

        if (!all_freed) {
            pp = &pr->next;
            continue;
        }

        // Don't reclaim if bump_ptr is still pointing into this range
        if (b->bump_ptr != nullptr) {
            uintptr_t bp = reinterpret_cast<uintptr_t>(b->bump_ptr);
            uintptr_t base = reinterpret_cast<uintptr_t>(pr->ptr);
            if (bp >= base && bp < base + pr->count * MP_PAGE_SIZE) {
                pp = &pr->next;
                continue;
            }
        }

        // Filter blocks belonging to this page range out of free_head
        FreeBlock* new_head = nullptr;
        FreeBlock* new_tail = nullptr;
        FreeBlock* cur = b->free_head;
        while (cur) {
            FreeBlock* nxt = cur->next;
            if (!block_in_page(cur, pr->ptr, pr->count)) {
                cur->next = nullptr;
                if (new_tail) { new_tail->next = cur; new_tail = cur; }
                else { new_head = cur; new_tail = cur; }
            }
            cur = nxt;
        }
        b->free_head = new_head;

        // Return pages to arena
        arena_free_pages(arena, pr->ptr, pr->count);

        // Remove this PageRange from list
        *pp = pr->next;
        free(pr);
    }
}

// Collect local_free into free_head. Called only when free_head is empty,
// so no need to traverse to find tail — just swap the list pointer.
// Every 64 collects, try to reclaim fully-freed pages (amortized cost).
static bool bucket_collect_local(Bucket* b, Arena* arena) {
    if (!b->local_free_head) return false;
    b->free_head = b->local_free_head;
    b->local_free_head = nullptr;

    // Amortized page reclaim: check every 64 collects
    if (++b->collect_count >= 64) {
        b->collect_count = 0;
        bucket_try_reclaim_pages(b, arena);
    }
    return true;
}

// Collect thread_free (cross-thread frees) into free_head.
// Called only when free_head is empty.
// Walk the list to update freed_count per page (these frees bypassed tlc_free).
static bool bucket_collect_thread_free(Bucket* b) {
    FreeBlock* head = b->thread_free_head.exchange(nullptr, std::memory_order_acquire);
    if (!head) return false;

    // Count freed blocks per page for remote frees
    FreeBlock* cur = head;
    while (cur) {
        ChunkHeader* chunk = chunk_of(cur);
        uint32_t pi = page_index_of(chunk, cur);
        chunk->pages[pi].freed_count++;
        cur = cur->next;
    }

    // free_head is empty when this is called, so just set it directly
    b->free_head = head;
    return true;
}

// Slow path: allocate a batch of pages from the arena.
static bool bucket_alloc_new_pages(TLC* tlc, Bucket* b) {
    uint32_t idx = b->bucket_idx;
    uint32_t blk_per_page = sc_blocks_per_page(idx);
    uint32_t batch_pages = calc_batch_pages(blk_per_page);

    void* pages = arena_alloc_pages(tlc->arena, idx, batch_pages, tlc->thread_id);
    if (!pages) {
        if (batch_pages > 1) {
            batch_pages = 1;
            pages = arena_alloc_pages(tlc->arena, idx, 1, tlc->thread_id);
        }
        if (!pages) return false;
    }

    // Set up bump pointer region
    b->bump_ptr = static_cast<uint8_t*>(pages);
    b->bump_limit = b->bump_ptr + batch_pages * MP_PAGE_SIZE;

    // Set owner_tlc on all allocated pages for O(1) cross-thread free
    ChunkHeader* chunk = chunk_of(pages);
    uint32_t pi_start = page_index_of(chunk, pages);
    for (uint32_t i = 0; i < batch_pages; i++) {
        chunk->pages[pi_start + i].owner_tlc = tlc;
    }

    b->page_list = page_range_alloc(pages, batch_pages, b->page_list);
    return true;
}

void* tlc_alloc(TLC* tlc, uint32_t bucket_idx) {
    Bucket& b = tlc->buckets[bucket_idx];

    // Fast path 1: pop from free_head (recycled blocks)
    if (MP_LIKELY(b.free_head != nullptr)) {
        FreeBlock* blk = b.free_head;
        b.free_head = blk->next;
        // Prefetch next-next block for the NEXT alloc call (rpmalloc/smmalloc trick)
        // Hides linked-list traversal latency by preloading into L1 cache
        if (blk->next) __builtin_prefetch(blk->next, 0, 3);
#ifdef MEMPOOL_STATS
        tlc->stats.alloc_count++;
#endif
        return blk;
    }

    // Fast path 2: bump pointer allocation (page-boundary safe)
    if (MP_LIKELY(b.bump_ptr != nullptr)) {
        uint8_t* ptr = b.bump_ptr;
        uint8_t* next = ptr + b.block_size;
        // Skip page boundary if block would straddle two pages
        // (only triggers for sizes that don't evenly divide 4096, e.g. 48, 80, 96...)
        if (MP_UNLIKELY(((uintptr_t)ptr ^ (uintptr_t)(next - 1)) >> 12 !=0)) {
            ptr = reinterpret_cast<uint8_t*>(((uintptr_t)ptr + MP_PAGE_SIZE - 1) & ~(uintptr_t)(MP_PAGE_SIZE - 1));
            next = ptr + b.block_size;
        }
        if (MP_LIKELY(next <= b.bump_limit)) {
            b.bump_ptr = next;
#ifdef MEMPOOL_STATS
            tlc->stats.alloc_count++;
#endif
            return ptr;
        }
        b.bump_ptr = nullptr;
        b.bump_limit = nullptr;
    }

    // Slow path 1: collect local_free (+ amortized page reclaim)
    if (bucket_collect_local(&b, tlc->arena)) {
#ifdef MEMPOOL_STATS
        tlc->stats.slow_path_hits++;
#endif
        return tlc_alloc(tlc, bucket_idx);
    }

    // Slow path 2: collect thread_free (+ count remote frees)
    if (bucket_collect_thread_free(&b)) {
#ifdef MEMPOOL_STATS
        tlc->stats.slow_path_hits++;
#endif
        return tlc_alloc(tlc, bucket_idx);
    }

    // Slow path 3: allocate new batch of pages from arena
    if (bucket_alloc_new_pages(tlc, &b)) {
#ifdef MEMPOOL_STATS
        tlc->stats.slow_path_hits++;
#endif
        return tlc_alloc(tlc, bucket_idx);
    }

    return nullptr;
}

void tlc_free(TLC* tlc, void* ptr, PageMeta* pm, uint32_t bucket_idx) {
    Bucket& b = tlc->buckets[bucket_idx];

    FreeBlock* blk = static_cast<FreeBlock*>(ptr);
    blk->next = b.local_free_head;
    b.local_free_head = blk;
    pm->freed_count++;  // LevelDB Arena 式: 积累释放计数，达到整页时批量回收

#ifdef MEMPOOL_STATS
    tlc->stats.free_count++;
#endif
}

void tlc_free_remote(Bucket* bucket, void* ptr) {
    FreeBlock* blk = static_cast<FreeBlock*>(ptr);
    FreeBlock* old_head = bucket->thread_free_head.load(std::memory_order_relaxed);
    do {
        blk->next = old_head;
    } while (!bucket->thread_free_head.compare_exchange_weak(
        old_head, blk,
        std::memory_order_release,
        std::memory_order_relaxed));
}

void tlc_flush(TLC* tlc) {
    for (uint32_t i = 0; i < MP_NUM_SIZE_CLASSES; i++) {
        Bucket& b = tlc->buckets[i];

        // No need to collect — just discard all lists
        b.free_head = nullptr;
        b.local_free_head = nullptr;
        b.thread_free_head.store(nullptr, std::memory_order_relaxed);
        b.bump_ptr = nullptr;
        b.bump_limit = nullptr;

        PageRange* pr = b.page_list;
        while (pr) {
            PageRange* next = pr->next;
            arena_free_pages(tlc->arena, pr->ptr, pr->count);
            free(pr);
            pr = next;
        }
        b.page_list = nullptr;
    }
}

void tlc_destroy(TLC* tlc) {
    if (!tlc) return;

    Arena* arena = tlc->arena;
    if (arena) {
        std::lock_guard<std::mutex> lock(arena->tlc_mutex);
        if (tlc->prev_in_arena) {
            tlc->prev_in_arena->next_in_arena = tlc->next_in_arena;
        } else {
            arena->tlc_head = tlc->next_in_arena;
        }
        if (tlc->next_in_arena) {
            tlc->next_in_arena->prev_in_arena = tlc->prev_in_arena;
        }
    }

    for (uint32_t i = 0; i < MP_NUM_SIZE_CLASSES; i++) {
        page_range_free_list(tlc->buckets[i].page_list);
    }

    delete tlc;
}

} // namespace mp
