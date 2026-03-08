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

/* --- Bucket: manages free blocks for one size class --- */
struct Bucket {
    FreeBlock*               free_head;         // fast path: pop here
    FreeBlock*               local_free_head;   // same-thread free pushes here
    std::atomic<FreeBlock*>  thread_free_head;  // cross-thread CAS pushes here

    uint32_t  free_count;       // blocks in free_head
    uint32_t  local_free_count; // blocks in local_free_head
    uint32_t  bucket_idx;       // which size class

    PageRange* page_list;       // all pages owned by this bucket
};

/* --- TLC: Thread-Local Cache --- */
struct TLC {
    Arena*   arena;
    uint64_t thread_id;
    Bucket   buckets[MP_NUM_SIZE_CLASSES];
    TLCStats stats;

    TLC*     next_in_arena;  // linked list of TLCs in arena
    TLC*     prev_in_arena;
};

TLC* tlc_get_or_create(Arena* arena);
void* tlc_alloc(TLC* tlc, uint32_t bucket_idx);
void tlc_free(TLC* tlc, void* ptr, PageMeta* pm, uint32_t bucket_idx);
void tlc_free_remote(Bucket* bucket, void* ptr);
void tlc_flush(TLC* tlc);
void tlc_destroy(TLC* tlc);
TLC* tlc_current();
void tlc_set_current(TLC* tlc);

} // namespace mp

#endif /* MEMPOOL_TLC_H */
