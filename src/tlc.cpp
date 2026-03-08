#include "tlc.h"
#include <cstring>
#include <new>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace mp {

// --- Platform TLS for TLC pointer (avoids thread_local in DLL on MinGW) ---
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

// Thread exit callback - cleans up the TLC
static void on_thread_exit(void* /*arg*/) {
    TLC* tlc = tlc_current();
    if (tlc) {
        tlc_flush(tlc);
        tlc_destroy(tlc);
        tlc_set_current(nullptr);
    }
}

TLC* tlc_get_or_create(Arena* arena) {
    TLC* tlc = tlc_current();
    if (tlc && tlc->arena == arena) return tlc;

    // Create new TLC
    tlc = new(std::nothrow) TLC();
    if (!tlc) return nullptr;

    tlc->arena = arena;
    tlc->thread_id = platform::get_thread_id();
    memset(&tlc->stats, 0, sizeof(tlc->stats));

    // Initialize all buckets
    for (uint32_t i = 0; i < MP_NUM_SIZE_CLASSES; i++) {
        Bucket& b = tlc->buckets[i];
        b.free_head = nullptr;
        b.local_free_head = nullptr;
        b.thread_free_head.store(nullptr, std::memory_order_relaxed);
        b.free_count = 0;
        b.local_free_count = 0;
        b.bucket_idx = i;
        b.page_ptr = nullptr;
        b.page_count = 0;
    }

    // Add to arena's TLC list
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

    // Register thread exit callback
    platform::register_thread_exit_callback(on_thread_exit, tlc);

    return tlc;
}

// Initialize a page as a free list of blocks.
static FreeBlock* init_page_freelist(void* page, size_t block_size, uint32_t block_count) {
    FreeBlock* head = nullptr;
    uint8_t* base = static_cast<uint8_t*>(page);

    for (int i = (int)block_count - 1; i >= 0; i--) {
        FreeBlock* blk = reinterpret_cast<FreeBlock*>(base + i * block_size);
        blk->next = head;
        head = blk;
    }
    return head;
}

// Slow path: merge local_free into free_head
static bool bucket_collect_local(Bucket* b) {
    if (!b->local_free_head) return false;

    FreeBlock* tail = b->local_free_head;
    while (tail->next) tail = tail->next;
    tail->next = b->free_head;
    b->free_head = b->local_free_head;
    b->free_count += b->local_free_count;
    b->local_free_head = nullptr;
    b->local_free_count = 0;
    return true;
}

// Slow path: collect thread_free via atomic exchange
static bool bucket_collect_thread_free(Bucket* b) {
    FreeBlock* head = b->thread_free_head.exchange(nullptr, std::memory_order_acquire);
    if (!head) return false;

    uint32_t count = 1;
    FreeBlock* tail = head;
    while (tail->next) {
        tail = tail->next;
        count++;
    }
    tail->next = b->free_head;
    b->free_head = head;
    b->free_count += count;
    return true;
}

// Slow path: allocate a new page from the arena
static bool bucket_alloc_new_page(TLC* tlc, Bucket* b) {
    uint32_t idx = b->bucket_idx;
    size_t blk_size = sc_block_size(idx);
    uint32_t blk_per_page = sc_blocks_per_page(idx);

    void* page = arena_alloc_pages(tlc->arena, idx, 1, tlc->thread_id);
    if (!page) return false;

    FreeBlock* head = init_page_freelist(page, blk_size, blk_per_page);
    b->free_head = head;
    b->free_count = blk_per_page;
    b->page_ptr = page;
    b->page_count = 1;
    return true;
}

void* tlc_alloc(TLC* tlc, uint32_t bucket_idx) {
    Bucket& b = tlc->buckets[bucket_idx];

    // Fast path: pop from free_head
    if (MP_LIKELY(b.free_head != nullptr)) {
        FreeBlock* blk = b.free_head;
        b.free_head = blk->next;
        b.free_count--;

        // Update page used count
        ChunkHeader* chunk = chunk_of(blk);
        uint32_t pi = page_index_of(chunk, blk);
        chunk->pages[pi].used_count.fetch_add(1, std::memory_order_relaxed);

        tlc->stats.alloc_count++;
        tlc->stats.alloc_bytes += sc_block_size(bucket_idx);
        tlc->stats.fast_path_hits++;
        return blk;
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

    // Slow path 3: allocate new page from arena
    if (bucket_alloc_new_page(tlc, &b)) {
        tlc->stats.slow_path_hits++;
        return tlc_alloc(tlc, bucket_idx);
    }

    return nullptr; // out of memory
}

void tlc_free(TLC* tlc, void* ptr, PageMeta* pm, uint32_t bucket_idx) {
    Bucket& b = tlc->buckets[bucket_idx];

    FreeBlock* blk = static_cast<FreeBlock*>(ptr);
    blk->next = b.local_free_head;
    b.local_free_head = blk;
    b.local_free_count++;

    pm->used_count.fetch_sub(1, std::memory_order_relaxed);

    tlc->stats.free_count++;
    tlc->stats.free_bytes += sc_block_size(bucket_idx);

    if (MP_UNLIKELY(b.local_free_count >= MP_HIGH_WATERMARK)) {
        bucket_collect_local(&b);
    }
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
        b.free_count = 0;
        b.local_free_count = 0;

        if (b.page_ptr && b.page_count > 0) {
            arena_free_pages(tlc->arena, b.page_ptr, b.page_count);
            b.page_ptr = nullptr;
            b.page_count = 0;
        }
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

    delete tlc;
}

} // namespace mp
