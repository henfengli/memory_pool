#ifndef MEMPOOL_ARENA_H
#define MEMPOOL_ARENA_H

#include "internal.h"
#include "platform.h"
#include "size_class.h"
#include <mutex>
#include <atomic>

namespace mp {

struct TLC; // forward declaration

/* --- PageMeta: metadata for one 4KB page within a chunk --- */
struct PageMeta {
    uint16_t block_size;     // size class block size (0 = unused)
    uint16_t bucket_idx;     // size class index
    uint16_t block_count;    // total blocks in this page
    std::atomic<uint16_t> used_count; // currently allocated blocks
    uint64_t owner_thread;   // thread that owns this page
    TLC*     owner_tlc;      // direct pointer to owner TLC (for O(1) cross-thread free)
};

/* --- ChunkHeader: header at the start of each 4MB chunk --- */
struct ChunkHeader {
    uint64_t     magic;           // MP_CHUNK_MAGIC for validation
    struct Arena* arena;          // owning arena
    ChunkHeader* next;            // linked list of chunks in arena
    ChunkHeader* prev;

    // Hint: next page index to start scanning from (avoids O(N²) bitmap scan)
    uint32_t     next_free_hint;

    // Bitmap: 1 bit per usable page (MP_USABLE_PAGES bits).
    // bit=1 means page is allocated, bit=0 means free.
    uint64_t     page_bitmap[16];

    // Page metadata array for usable pages
    PageMeta     pages[MP_USABLE_PAGES];
};

// Ensure ChunkHeader fits within the reserved header pages
static_assert(sizeof(ChunkHeader) <= MP_HEADER_PAGES * MP_PAGE_SIZE,
              "ChunkHeader too large for reserved header pages");

/* --- Arena --- */
struct Arena {
    uint32_t     id;
    char         name[64];
    std::mutex   mutex;
    ChunkHeader* chunk_head;      // linked list of chunks
    size_t       max_size;        // 0 = unlimited
    size_t       total_allocated; // total bytes committed to chunks

    // TLC list for this arena
    std::mutex   tlc_mutex;
    TLC*         tlc_head;

    // Statistics
    std::atomic<uint64_t> stat_page_alloc;
    std::atomic<uint64_t> stat_page_free;
    std::atomic<uint64_t> stat_chunk_count;

    Arena*       next_arena;      // global arena list
};

/* --- Inline helpers --- */

inline ChunkHeader* chunk_of(void* ptr) {
    return reinterpret_cast<ChunkHeader*>(
        reinterpret_cast<uintptr_t>(ptr) & ~(MP_CHUNK_SIZE - 1));
}

inline uint32_t page_index_of(ChunkHeader* chunk, void* ptr) {
    uintptr_t base = reinterpret_cast<uintptr_t>(chunk) + MP_HEADER_PAGES * MP_PAGE_SIZE;
    uintptr_t offset = reinterpret_cast<uintptr_t>(ptr) - base;
    return (uint32_t)(offset / MP_PAGE_SIZE);
}

inline void* page_start(ChunkHeader* chunk, uint32_t page_idx) {
    return reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(chunk) + (MP_HEADER_PAGES + page_idx) * MP_PAGE_SIZE);
}

/* --- Arena functions --- */

Arena* arena_create(uint32_t id, const char* name, size_t max_size);
void arena_destroy(Arena* arena);
void* arena_alloc_pages(Arena* arena, uint32_t bucket_idx, uint32_t count, uint64_t thread_id);
void arena_free_pages(Arena* arena, void* ptr, uint32_t count);
PageMeta* resolve_block_ptr(void* ptr, ChunkHeader** out_chunk);

} // namespace mp

#endif /* MEMPOOL_ARENA_H */
