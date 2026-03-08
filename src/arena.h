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
};

/* --- ChunkHeader: header at the start of each 4MB chunk --- */
struct ChunkHeader {
    uint64_t     magic;           // MP_CHUNK_MAGIC for validation
    struct Arena* arena;          // owning arena
    ChunkHeader* next;            // linked list of chunks in arena
    ChunkHeader* prev;

    // Bitmap: 1 bit per usable page (MP_USABLE_PAGES bits).
    // bit=1 means page is allocated, bit=0 means free.
    // We use 16 uint64_t = 1024 bits, enough for 1008 usable pages.
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

// Given any pointer within a chunk, get the ChunkHeader via alignment mask.
inline ChunkHeader* chunk_of(void* ptr) {
    return reinterpret_cast<ChunkHeader*>(
        reinterpret_cast<uintptr_t>(ptr) & ~(MP_CHUNK_SIZE - 1));
}

// Given a pointer within a chunk, get the page index (0-based from first usable page).
inline uint32_t page_index_of(ChunkHeader* chunk, void* ptr) {
    uintptr_t base = reinterpret_cast<uintptr_t>(chunk) + MP_HEADER_PAGES * MP_PAGE_SIZE;
    uintptr_t offset = reinterpret_cast<uintptr_t>(ptr) - base;
    return (uint32_t)(offset / MP_PAGE_SIZE);
}

// Get pointer to the start of a page given its index.
inline void* page_start(ChunkHeader* chunk, uint32_t page_idx) {
    return reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(chunk) + (MP_HEADER_PAGES + page_idx) * MP_PAGE_SIZE);
}

/* --- Arena functions --- */

// Create a new arena. `max_size` of 0 means unlimited.
Arena* arena_create(uint32_t id, const char* name, size_t max_size);

// Destroy an arena and free all its chunks.
void arena_destroy(Arena* arena);

// Allocate `count` contiguous pages from the arena for a given size class.
// Returns pointer to the first page, or nullptr on failure.
// Sets up PageMeta for each allocated page.
void* arena_alloc_pages(Arena* arena, uint32_t bucket_idx, uint32_t count, uint64_t thread_id);

// Free `count` contiguous pages starting at `ptr`.
void arena_free_pages(Arena* arena, void* ptr, uint32_t count);

// Resolve a block pointer: find its chunk, page meta, and validate.
// Returns the PageMeta for the page containing `ptr`, or nullptr if invalid.
PageMeta* resolve_block_ptr(void* ptr, ChunkHeader** out_chunk);

} // namespace mp

#endif /* MEMPOOL_ARENA_H */
