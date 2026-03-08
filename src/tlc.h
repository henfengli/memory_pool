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

/* --- Bucket: manages free blocks for one size class --- */
struct Bucket {
    FreeBlock*               free_head;         // fast path: pop here
    FreeBlock*               local_free_head;   // same-thread free pushes here
    std::atomic<FreeBlock*>  thread_free_head;  // cross-thread CAS pushes here

    uint32_t  free_count;       // blocks in free_head
    uint32_t  local_free_count; // blocks in local_free_head
    uint32_t  bucket_idx;       // which size class

    // Page tracking: the page(s) this bucket pulls from
    void*     page_ptr;         // start of the current page
    uint32_t  page_count;       // number of pages allocated (usually 1)
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

// Get or create the TLC for the current thread, bound to the given arena.
TLC* tlc_get_or_create(Arena* arena);

// Allocate a block from the given bucket in this TLC.
void* tlc_alloc(TLC* tlc, uint32_t bucket_idx);

// Free a block. Detects same-thread vs cross-thread automatically.
void tlc_free(TLC* tlc, void* ptr, PageMeta* pm, uint32_t bucket_idx);

// Free a block from a different thread via CAS on thread_free_head.
void tlc_free_remote(Bucket* bucket, void* ptr);

// Flush all buckets in this TLC (called on thread exit or detach).
void tlc_flush(TLC* tlc);

// Destroy a TLC and remove it from its arena's TLC list.
void tlc_destroy(TLC* tlc);

// Get the current thread's TLC (nullptr if not attached).
TLC* tlc_current();

// Set the current thread's TLC.
void tlc_set_current(TLC* tlc);

} // namespace mp

#endif /* MEMPOOL_TLC_H */
