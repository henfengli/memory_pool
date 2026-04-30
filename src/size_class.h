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

// Per-size-class derived properties packed into one compile-time table.
// Inspired by tcmalloc's SizeMap and mimalloc's _mi_bin metadata: all
// derived attributes (block_size, blocks_per_page, batch_pages) are
// computed once at compile time, so the slow path performs a single load
// instead of redundant divisions / branches.
struct SizeClassInfo {
    uint16_t block_size;       // 16 ~ 4096
    uint16_t blocks_per_page;  // MP_PAGE_SIZE / block_size
    uint8_t  batch_pages;      // pages requested per refill (slow path)
    uint8_t  _pad;             // align to 8 bytes
};

namespace detail {
    // Compute pages-per-refill so that batch_pages * blocks_per_page >= MP_REFILL_BLOCKS.
    // Capped at 64 because a single bitmap word covers 64 pages (see arena.cpp).
    constexpr uint8_t compute_batch_pages(uint16_t blocks_per_page) {
        if (blocks_per_page >= MP_REFILL_BLOCKS) return 1;
        uint32_t pages = (MP_REFILL_BLOCKS + blocks_per_page - 1) / blocks_per_page;
        return (uint8_t)(pages > 64 ? 64 : pages);
    }

    struct SCInfoTable {
        SizeClassInfo data[MP_NUM_SIZE_CLASSES];
        constexpr SCInfoTable() : data{} {
            for (uint32_t i = 0; i < MP_NUM_SIZE_CLASSES; i++) {
                uint16_t bs  = (uint16_t)kSizeClasses[i];
                uint16_t bpp = (uint16_t)(MP_PAGE_SIZE / bs);
                data[i] = SizeClassInfo{ bs, bpp, compute_batch_pages(bpp), 0 };
            }
        }
    };
    static constexpr SCInfoTable sc_info_table{};
} // namespace detail

inline const SizeClassInfo& sc_info(uint32_t idx) {
    return detail::sc_info_table.data[idx];
}

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

} // namespace mp

#endif /* MEMPOOL_SIZE_CLASS_H */
