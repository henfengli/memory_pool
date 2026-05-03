#ifndef MEMPOOL_INTERNAL_H
#define MEMPOOL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <cassert>
#include <cstdio>

/* --- Constants --- */
static constexpr size_t MP_PAGE_SIZE   = 4096;
static constexpr size_t MP_CHUNK_SIZE  = 4u * 1024 * 1024;  // 4 MB
static constexpr size_t MP_CHUNK_ALIGN = MP_CHUNK_SIZE;
static constexpr uint64_t MP_CHUNK_MAGIC = 0x4D454D504F4F4CUL; // "MEMPOOL"

static constexpr size_t MP_NUM_SIZE_CLASSES = 25;

/* TLC watermarks */
static constexpr uint32_t MP_HIGH_WATERMARK  = 256;
static constexpr uint32_t MP_REFILL_BLOCKS   = 64;

/* Debug fill patterns */
static constexpr uint8_t MP_ALLOC_FILL = 0xCD;
static constexpr uint8_t MP_FREE_FILL  = 0xDD;
static constexpr uint64_t MP_FREE_TAG = 0xDEADDEADDEADDEADULL;

/* Max pages per chunk: (CHUNK_SIZE - header_reserved) / PAGE_SIZE
   Reserve first 16 pages (64KB) for ChunkHeader+PageMeta, leaves ~1008 usable pages.
   We use 1008 pages = ceil((4MB - 64KB) / 4KB). Use 1008 for simplicity. */
static constexpr size_t MP_HEADER_PAGES   = 16;
static constexpr size_t MP_TOTAL_PAGES    = MP_CHUNK_SIZE / MP_PAGE_SIZE;       // 1024
static constexpr size_t MP_USABLE_PAGES   = MP_TOTAL_PAGES - MP_HEADER_PAGES;  // 1008

/* --- Compiler helpers --- */
#if defined(__GNUC__) || defined(__clang__)
    #define MP_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define MP_UNLIKELY(x) __builtin_expect(!!(x), 0)
    #define MP_ALWAYS_INLINE __attribute__((always_inline))
#elif defined(_MSC_VER)
    #define MP_LIKELY(x)   (x)
    #define MP_UNLIKELY(x) (x)
    #define MP_ALWAYS_INLINE __forceinline
#else
    #define MP_LIKELY(x)   (x)
    #define MP_UNLIKELY(x) (x)
    #define MP_ALWAYS_INLINE
#endif

/* --- Bit operations (cross-platform) --- */
#if defined(_MSC_VER)
    #include <intrin.h>
    static inline int mp_ctzll(uint64_t x) {
        unsigned long idx;
        #if defined(_M_X64) || defined(_M_ARM64)
            _BitScanForward64(&idx, x);
        #else
            // 32-bit MSVC fallback
            if (_BitScanForward(&idx, (unsigned long)x)) return (int)idx;
            _BitScanForward(&idx, (unsigned long)(x >> 32));
            return (int)(idx + 32);
        #endif
        return (int)idx;
    }
    static inline int mp_popcountll(uint64_t x) {
        #if defined(_M_X64) || defined(_M_ARM64)
            return (int)__popcnt64(x);
        #else
            return (int)(__popcnt((unsigned int)x) + __popcnt((unsigned int)(x >> 32)));
        #endif
    }
#else
    // GCC / Clang (including MinGW)
    static inline int mp_ctzll(uint64_t x) {
        return __builtin_ctzll(x);
    }
    static inline int mp_popcountll(uint64_t x) {
        return __builtin_popcountll(x);
    }
#endif

/* --- Alignment helpers --- */
static inline size_t mp_align_up(size_t value, size_t align) {
    return (value + align - 1) & ~(align - 1);
}

static inline bool mp_is_aligned(const void* ptr, size_t align) {
    return ((uintptr_t)ptr & (align - 1)) == 0;
}

/* --- Safe multiply with overflow check --- */
static inline bool mp_mul_overflow(size_t a, size_t b, size_t* result) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, result);
#else
    if (a != 0 && b > SIZE_MAX / a) return true;
    *result = a * b;
    return false;
#endif
}

#endif /* MEMPOOL_INTERNAL_H */
