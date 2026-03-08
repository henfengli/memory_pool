#ifndef MEMPOOL_SIZE_CLASS_H
#define MEMPOOL_SIZE_CLASS_H

#include "internal.h"

namespace mp {

// 25 size classes: 16B to 4096B
static constexpr size_t kSizeClasses[MP_NUM_SIZE_CLASSES] = {
    16,   32,   48,   64,   80,   96,  112,  128,   // 0-7:   step 16
    160,  192,  224,  256,                            // 8-11:  step 32
    320,  384,  448,  512,                            // 12-15: step 64
    640,  768, 1024,                                  // 16-18: step 128/256
    1280, 1536, 2048,                                 // 19-21
    2560, 3072, 4096                                  // 22-24
};

static constexpr size_t kMaxBlockSize = 4096;

// Fast lookup: given a requested size, return the size class index.
// Assumes size > 0 && size <= kMaxBlockSize.
inline uint32_t sc_index_of(size_t size) {
    if (size <= 128) {
        // step 16: index = ceil(size/16) - 1
        return (uint32_t)((size + 15) / 16) - 1;
    }
    // Binary search in the remaining classes
    uint32_t lo = 8, hi = MP_NUM_SIZE_CLASSES - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (kSizeClasses[mid] < size)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// Return the block size for a given size class index.
inline size_t sc_block_size(uint32_t idx) {
    return kSizeClasses[idx];
}

// Return how many blocks fit in one 4KB page for a given size class.
inline uint32_t sc_blocks_per_page(uint32_t idx) {
    return (uint32_t)(MP_PAGE_SIZE / kSizeClasses[idx]);
}

} // namespace mp

#endif /* MEMPOOL_SIZE_CLASS_H */
