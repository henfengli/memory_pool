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

// Pre-computed lookup table: size -> size class index (tcmalloc/rpmalloc style).
// Table indexed by (size + 15) / 16, covering 0..256 (for sizes 0..4096).
// Generated at compile time to replace binary search with O(1) table lookup.
namespace detail {
    constexpr uint8_t compute_sc_index(size_t slot) {
        size_t size = slot * 16; // slot = ceil(size/16), size = slot*16 is the upper bound
        if (size == 0) return 0;
        for (uint8_t i = 0; i < MP_NUM_SIZE_CLASSES; i++) {
            if (kSizeClasses[i] >= size) return i;
        }
        return MP_NUM_SIZE_CLASSES - 1;
    }

    // Build table at compile time
    struct SCLookupTable {
        uint8_t data[257]; // indices 0..256 for sizes 0..4096
        constexpr SCLookupTable() : data{} {
            for (size_t i = 0; i <= 256; i++) {
                data[i] = compute_sc_index(i);
            }
        }
    };
    static constexpr SCLookupTable sc_table{};
} // namespace detail

// Fast lookup: O(1) table lookup, no branches (tcmalloc/rpmalloc style).
// Assumes size > 0 && size <= kMaxBlockSize.
inline uint32_t sc_index_of(size_t size) {
    return detail::sc_table.data[(size + 15) / 16];
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
