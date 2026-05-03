#ifndef MEMPOOL_TLC_H
#define MEMPOOL_TLC_H

#include "internal.h"
#include "arena.h"
#include "stats.h"
#include <atomic>

namespace mp {

/* --- FreeBlock: intrusive node in the free list --- */
struct FreeBlock {
    FreeBlock* next;
};

/* --- PageRange: tracks a batch of contiguous pages owned by a bucket --- */
struct PageRange {
    void*      ptr;    // start of the contiguous pages
    uint32_t   count;  // number of pages
    PageRange* next;   // linked list
};

/* --- Bucket: manages free blocks for one size class.
 * alignas(64) keeps each Bucket on its own cache line so the hot hot fields
 * (free_head, local_free_head) of Bucket[i] never share a line with any
 * other size class's hot fields. Without this, a multi-thread bench where
 * 2 threads both churn 64B (or any single size) sees no false sharing
 * between threads (each has its own TLC), but mp_malloc still reads
 * b.free_head via offset (i * sizeof(Bucket)) — having Bucket be a clean
 * power-of-2 also lets the compiler emit shl in place of imul (sizeof
 * 64 = 1<<6).
 */
struct alignas(64) Bucket {
    FreeBlock*               free_head;         // fast path: pop here
    FreeBlock*               local_free_head;   // same-thread free pushes here
    std::atomic<FreeBlock*>  thread_free_head;  // cross-thread CAS pushes here

    uint16_t  bucket_idx;       // which size class
    uint16_t  block_size;       // cached block size (avoids table lookup in bump path)
    uint16_t  collect_count;    // collect counter for amortized page reclaim

    // Bump pointer for lazy initialization (snmalloc/LevelDB style)
    uint8_t*  bump_ptr;         // next block to allocate via bump
    uint8_t*  bump_limit;       // end of current bump region

    PageRange* page_list;       // all pages owned by this bucket
};
static_assert(sizeof(Bucket) == 64,
              "Bucket should be exactly one cache line (64 bytes)");

/* --- TLC: Thread-Local Cache.
 * alignas(64) avoids two separately-allocated TLCs (one per thread) sharing
 * a cache line at their boundary, which can hurt multi-thread scaling on
 * workloads where threads alternately touch their respective Buckets.
 */
struct alignas(64) TLC {
    Arena*   arena;
    Bucket   buckets[MP_NUM_SIZE_CLASSES];
    TLCStats stats;

    TLC*     next_in_arena;  // linked list of TLCs in arena
    TLC*     prev_in_arena;
};

TLC* tlc_get_or_create(Arena* arena);
void* tlc_alloc_slow(TLC* tlc, uint32_t bucket_idx);
void tlc_free_remote(Bucket* bucket, void* ptr);
void tlc_flush(TLC* tlc);
void tlc_destroy(TLC* tlc);
TLC* tlc_current();
void tlc_set_current(TLC* tlc);

// Hot-path alloc: pop one block from b.free_head, or swap local_free into
// free_head and pop. Falls through to tlc_alloc_slow only when both lists
// are empty (must allocate new pages from arena, or drain thread_free).
//
// Why swap local_free → free_head here instead of in tlc_alloc_slow:
// in a tight churn loop (alloc-free-alloc-free with K-ring), each free pushes
// to local_free and each alloc pops from free_head. Without the swap on the
// fast path, free_head is empty by iteration 1, so every iteration jumps to
// tlc_alloc_slow (perf showed 36% of time there). Inlining the swap keeps
// the hot path at a single function call from mp_malloc.
//
// Reclaim: amortized page reclaim previously fired off this swap path
// (every 64 collects). It's now triggered only from tlc_alloc_slow when we
// fall there for new pages — for churn-only workloads no pages can be
// reclaimed anyway (working set is fully resident), so this loses nothing.
MP_ALWAYS_INLINE inline void* tlc_alloc(TLC* tlc, uint32_t bucket_idx) {
    Bucket& b = tlc->buckets[bucket_idx];
    if (MP_LIKELY(b.free_head != nullptr)) {
        FreeBlock* blk = b.free_head;
        b.free_head = blk->next;
        if (blk->next) __builtin_prefetch(blk->next, 0, 3);
#ifdef MEMPOOL_STATS
        tlc->stats.alloc_count++;
#endif
        return blk;
    }
    if (MP_LIKELY(b.local_free_head != nullptr)) {
        FreeBlock* blk = b.local_free_head;
        b.free_head = blk->next;
        b.local_free_head = nullptr;
#ifdef MEMPOOL_STATS
        tlc->stats.alloc_count++;
#endif
        return blk;
    }
    return tlc_alloc_slow(tlc, bucket_idx);
}

// Hot-path free: push to b.local_free_head. Header-inlined for the same
// reason as tlc_alloc above.
MP_ALWAYS_INLINE inline void tlc_free(TLC* tlc, void* ptr, PageMeta* /*pm*/, uint32_t bucket_idx) {
    Bucket& b = tlc->buckets[bucket_idx];
    FreeBlock* blk = static_cast<FreeBlock*>(ptr);
    blk->next = b.local_free_head;
    b.local_free_head = blk;
#ifdef MEMPOOL_STATS
    tlc->stats.free_count++;
#endif
}

} // namespace mp

#endif /* MEMPOOL_TLC_H */
