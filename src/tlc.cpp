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
        b.bucket_idx = i;
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

static bool bucket_collect_local(Bucket* b) {
    if (!b->local_free_head) return false;

    FreeBlock* tail = b->local_free_head;
    while (tail->next) tail = tail->next;
    tail->next = b->free_head;
    b->free_head = b->local_free_head;
    b->local_free_head = nullptr;
    return true;
}

static bool bucket_collect_thread_free(Bucket* b) {
    FreeBlock* head = b->thread_free_head.exchange(nullptr, std::memory_order_acquire);
    if (!head) return false;

    FreeBlock* tail = head;
    while (tail->next) tail = tail->next;
    tail->next = b->free_head;
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
        tlc->stats.alloc_count++;
        tlc->stats.alloc_bytes += sc_block_size(bucket_idx);
        return blk;
    }

    // Fast path 2: bump pointer allocation
    if (MP_LIKELY(b.bump_ptr != nullptr)) {
        size_t blk_size = sc_block_size(bucket_idx);
        uint8_t* ptr = b.bump_ptr;
        uint8_t* next = ptr + blk_size;
        if (MP_LIKELY(next <= b.bump_limit)) {
            b.bump_ptr = next;
            tlc->stats.alloc_count++;
            tlc->stats.alloc_bytes += blk_size;
            return ptr;
        }
        b.bump_ptr = nullptr;
        b.bump_limit = nullptr;
    }

    // Slow path 1: collect local_free
    if (bucket_collect_local(&b)) {
        tlc->stats.slow_path_hits++;
        return tlc_alloc(tlc, bucket_idx);
    }

    // Slow path 2: collect thread_free
    if (bucket_collect_thread_free(&b)) {
        tlc->stats.slow_path_hits++;
        return tlc_alloc(tlc, bucket_idx);
    }

    // Slow path 3: allocate new batch of pages from arena
    if (bucket_alloc_new_pages(tlc, &b)) {
        tlc->stats.slow_path_hits++;
        return tlc_alloc(tlc, bucket_idx);
    }

    return nullptr;
}

void tlc_free(TLC* tlc, void* ptr, PageMeta* /*pm*/, uint32_t bucket_idx) {
    Bucket& b = tlc->buckets[bucket_idx];

    FreeBlock* blk = static_cast<FreeBlock*>(ptr);
    blk->next = b.local_free_head;
    b.local_free_head = blk;

    tlc->stats.free_count++;
    tlc->stats.free_bytes += sc_block_size(bucket_idx);
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

        bucket_collect_local(&b);
        bucket_collect_thread_free(&b);

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
