#include "tlc.h"
#include <cstring>
#include <new>
#include <cstdlib>
#include <mutex>

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
static std::once_flag g_tlc_tls_once;

static void init_tls_slot() {
    std::call_once(g_tlc_tls_once, []() {
        g_tlc_tls_index = TlsAlloc();
    });
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
    memset(&tlc->stats, 0, sizeof(tlc->stats));

    for (uint32_t i = 0; i < MP_NUM_SIZE_CLASSES; i++) {
        Bucket& b = tlc->buckets[i];
        b.free_head = nullptr;
        b.local_free_head = nullptr;
        b.thread_free_head.store(nullptr, std::memory_order_relaxed);
        b.bucket_idx = (uint16_t)i;
        b.block_size = (uint16_t)sc_info(i).block_size;
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

// Check if a block belongs to a given page range [page_ptr, page_ptr + count*PAGE_SIZE)
static inline bool block_in_page(void* blk, void* page_ptr, uint32_t page_count) {
    uintptr_t a = reinterpret_cast<uintptr_t>(blk);
    uintptr_t base = reinterpret_cast<uintptr_t>(page_ptr);
    return a >= base && a < base + page_count * MP_PAGE_SIZE;
}

// Count blocks of a free list that fall within [page_ptr, page_ptr+count*PAGE_SIZE)
// Used by reclaim's per-PageRange occupancy check.
static inline uint32_t count_blocks_in_range(FreeBlock* head, void* page_ptr, uint32_t page_count) {
    uint32_t n = 0;
    for (FreeBlock* cur = head; cur; cur = cur->next) {
        if (block_in_page(cur, page_ptr, page_count)) ++n;
    }
    return n;
}

// Try to reclaim fully-freed pages from this bucket's page_list.
//
// Plan D (v1.20.0): instead of maintaining a per-page freed_count
// counter on the hot path, reclaim itself counts how many of each page's
// blocks currently sit on the free / local_free / thread_free lists.
// A PageRange is reclaimed iff every page within it has block count
// equal to blocks_per_page (no block held by any user thread).
//
// Why this is safe under concurrent cross-thread free:
//   The reclaim pulls thread_free_head via atomic exchange before
//   counting. Any cross-thread free that loses the race (i.e. push
//   happens after the exchange) means the user thread *still held*
//   that block during the count window, so the page was not fully
//   freed → reclaim correctly skips it. Late-arriving thread_free
//   pushes accumulate on the new (post-exchange) thread_free_head and
//   are processed by the next collect_thread_free.
//
// Cost: hot path zero. Reclaim path walks free_head once per
// PageRange (already done by the existing free_head filter), so the
// added counting reuses that traversal — no asymptotic regression.
// Reclaim is amortized to once every 64 collect_local invocations.
static void bucket_try_reclaim_pages(Bucket* b, Arena* arena) {
    // Step 1: drain thread_free into free_head so all known frees are
    // visible to the count below. (This subsumes collect_thread_free
    // for this round — callers won't need to repeat it.)
    FreeBlock* tf = b->thread_free_head.exchange(nullptr, std::memory_order_acquire);
    if (tf) {
        // Append tf to the end of free_head. Order does not matter for
        // correctness; we splice tf at head for O(1).
        if (b->free_head) {
            FreeBlock* tail = tf;
            while (tail->next) tail = tail->next;
            tail->next = b->free_head;
        }
        b->free_head = tf;
    }

    PageRange** pp = &b->page_list;
    while (*pp) {
        PageRange* pr = *pp;

        // Don't reclaim if bump_ptr is still pointing into this range
        if (b->bump_ptr != nullptr) {
            uintptr_t bp = reinterpret_cast<uintptr_t>(b->bump_ptr);
            uintptr_t base = reinterpret_cast<uintptr_t>(pr->ptr);
            if (bp >= base && bp < base + pr->count * MP_PAGE_SIZE) {
                pp = &pr->next;
                continue;
            }
        }

        // Count blocks of free_head that lie within this range.
        // A range is fully freed iff (count == blocks_per_page * pages_in_range).
        uint32_t blocks_seen = count_blocks_in_range(b->free_head, pr->ptr, pr->count);
        // All pages in pr share the same size class as this Bucket.
        uint32_t expected = sc_info(b->bucket_idx).blocks_per_page * pr->count;

        if (blocks_seen < expected) {
            pp = &pr->next;
            continue;
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
//
// Plan D (v1.20.0): there is no per-page freed_count to compensate.
// We just splice the thread_free list into free_head; reclaim later
// will count blocks per page directly.
static bool bucket_collect_thread_free(Bucket* b) {
    FreeBlock* head = b->thread_free_head.exchange(nullptr, std::memory_order_acquire);
    if (!head) return false;

    // free_head is empty when this is called, so just set it directly
    b->free_head = head;
    return true;
}

// Slow path: allocate a batch of pages from the arena.
static bool bucket_alloc_new_pages(TLC* tlc, Bucket* b) {
    uint32_t idx = b->bucket_idx;
    uint32_t batch_pages = sc_info(idx).batch_pages;

    void* pages = arena_alloc_pages(tlc->arena, idx, batch_pages);
    if (!pages) {
        if (batch_pages > 1) {
            batch_pages = 1;
            pages = arena_alloc_pages(tlc->arena, idx, 1);
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

void tlc_free(TLC* tlc, void* ptr, PageMeta* /*pm*/, uint32_t bucket_idx) {
    Bucket& b = tlc->buckets[bucket_idx];

    FreeBlock* blk = static_cast<FreeBlock*>(ptr);
    blk->next = b.local_free_head;
    b.local_free_head = blk;
    // No PageMeta write on the hot path: reclaim counts blocks per page
    // directly when triggered (see bucket_try_reclaim_pages, Plan D).

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

        // Drain thread_free so no other thread holds a stale pointer
        // back into this bucket after we return the pages. The contents
        // are discarded — every block lives inside one of b.page_list's
        // pages and will be wiped when arena_free_pages reclaims them.
        (void)b.thread_free_head.exchange(nullptr, std::memory_order_acquire);

        b.free_head = nullptr;
        b.local_free_head = nullptr;
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
