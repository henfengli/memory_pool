#include <mempool/mempool.h>
#include "internal.h"
#include "platform.h"
#include "size_class.h"
#include "arena.h"
#include "tlc.h"
#include "stats.h"

#include <mutex>
#include <atomic>
#include <cstring>
#include <new>

namespace mp {

/* --- Global state --- */
Arena* g_default_arena = nullptr;
Arena* g_arena_list = nullptr;
std::mutex g_arena_list_mutex;

static std::mutex g_init_mutex;
static std::atomic<bool> g_initialized{false};
static std::atomic<uint32_t> g_arena_id_counter{0};
static mp_config_t g_config = {0, 0, 0};

static void do_init(const mp_config_t* config) {
    if (config) {
        g_config = *config;
    } else {
        g_config = {0, 0, 0};
    }

    g_default_arena = arena_create(g_arena_id_counter.fetch_add(1), "default", g_config.arena_max_size);

    {
        std::lock_guard<std::mutex> lock(g_arena_list_mutex);
        g_default_arena->next_arena = g_arena_list;
        g_arena_list = g_default_arena;
    }

    g_initialized.store(true, std::memory_order_release);
}

// Ensure initialized (lazy init on first use)
static inline void ensure_init() {
    if (MP_UNLIKELY(!g_initialized.load(std::memory_order_acquire))) {
        std::lock_guard<std::mutex> lock(g_init_mutex);
        if (!g_initialized.load(std::memory_order_relaxed)) {
            do_init(nullptr);
        }
    }
}

// Get the TLC for the current thread (auto-creating if needed). Marked
// noinline+cold so the fast paths in mp_malloc / mp_free do not bring in
// the init machinery (mutex, do_init) and end up emitting a stack frame
// just to satisfy a branch they almost never take.
__attribute__((noinline, cold))
static TLC* get_tlc() {
    ensure_init();
    TLC* tlc = tlc_current();
    if (MP_LIKELY(tlc != nullptr && tlc->arena != nullptr)) return tlc;
    return tlc_get_or_create(g_default_arena);
}

} // namespace mp

/* ========== C API Implementation ========== */

extern "C" {

MP_API int mp_init(const mp_config_t* config) {
    std::lock_guard<std::mutex> lock(mp::g_init_mutex);
    if (!mp::g_initialized.load(std::memory_order_relaxed)) {
        mp::do_init(config);
    }
    return 0;
}

MP_API void mp_shutdown(void) {
    if (!mp::g_initialized.load(std::memory_order_acquire)) return;

    // Mark as not initialized first to prevent new allocations
    mp::g_initialized.store(false, std::memory_order_release);

    // Clear calling thread's TLC
    mp::tlc_set_current(nullptr);

#ifdef MEMPOOL_DEBUG
    mp::stats_check_leaks();
#endif

    // Destroy all arenas and their TLCs
    std::lock_guard<std::mutex> lock(mp::g_arena_list_mutex);
    mp::Arena* arena = mp::g_arena_list;
    while (arena) {
        mp::Arena* next = arena->next_arena;
        // Destroy all TLCs in this arena
        {
            std::lock_guard<std::mutex> tlc_lock(arena->tlc_mutex);
            mp::TLC* tlc = arena->tlc_head;
            while (tlc) {
                mp::TLC* tlc_next = tlc->next_in_arena;
                delete tlc;
                tlc = tlc_next;
            }
            arena->tlc_head = nullptr;
        }
        mp::arena_destroy(arena);
        arena = next;
    }
    mp::g_arena_list = nullptr;
    mp::g_default_arena = nullptr;
}

MP_API void* mp_malloc(size_t size) {
    // Combine zero and overflow check into single comparison
    if (MP_UNLIKELY(size - 1 >= mp::kMaxBlockSize)) {
        if (size == 0) size = 1; else return nullptr;
    }

    // Inline fast path: skip ensure_init() if TLC already exists
    mp::TLC* tlc = mp::tlc_current();
    if (MP_UNLIKELY(!tlc || !tlc->arena)) {
        tlc = mp::get_tlc();
        if (MP_UNLIKELY(!tlc)) return nullptr;
    }

    uint32_t idx = mp::sc_index_of(size);
    void* ptr = mp::tlc_alloc(tlc, idx);

#ifdef MEMPOOL_DEBUG
    if (ptr && mp::g_config.debug_fill) {
        memset(ptr, MP_ALLOC_FILL, mp::sc_info(idx).block_size);
    }
#endif

    return ptr;
}

MP_API void mp_free(void* ptr) {
    if (MP_UNLIKELY(!ptr)) return;

    // Inline resolve: chunk_of + page_index
    mp::ChunkHeader* chunk = mp::chunk_of(ptr);
    uint32_t page_idx = mp::page_index_of(chunk, ptr);
    mp::PageMeta* pm = &chunk->pages[page_idx];

#ifdef MEMPOOL_DEBUG
    // Safety checks only in debug mode
    if (MP_UNLIKELY(chunk->magic != MP_CHUNK_MAGIC)) return;
    if (MP_UNLIKELY(page_idx >= MP_USABLE_PAGES)) return;
    if (MP_UNLIKELY(pm->block_size == 0)) return;
#endif

#ifdef MEMPOOL_DEBUG
    // Double-free detection
    mp::FreeBlock* blk = static_cast<mp::FreeBlock*>(ptr);
    if (reinterpret_cast<uintptr_t>(blk->next) == MP_FREE_TAG) {
        fprintf(stderr, "[mempool] DOUBLE FREE DETECTED at %p\n", ptr);
        return;
    }
    if (mp::g_config.debug_fill) {
        memset(ptr, MP_FREE_FILL, pm->block_size);
    }
    // Set free tag for double-free detection
    blk->next = reinterpret_cast<mp::FreeBlock*>(MP_FREE_TAG);
#endif

    uint32_t bucket_idx = pm->bucket_idx;

    // Fast path: check if current thread owns this page via TLC pointer comparison
    // This avoids get_thread_id() system call entirely
    mp::TLC* tlc = mp::tlc_current();
    if (MP_LIKELY(tlc != nullptr && pm->owner_tlc == tlc)) {
        mp::tlc_free(tlc, ptr, pm, bucket_idx);
        return;
    }

    // Cross-thread free: O(1) via owner_tlc pointer
    mp::TLC* owner_tlc = pm->owner_tlc;
    if (MP_LIKELY(owner_tlc != nullptr)) {
        mp::tlc_free_remote(&owner_tlc->buckets[bucket_idx], ptr);
#ifdef MEMPOOL_STATS
        if (tlc) {
            tlc->stats.free_count++;
        }
#endif
        return;
    }
}

MP_API void* mp_calloc(size_t count, size_t size) {
    size_t total;
    if (mp_mul_overflow(count, size, &total)) return nullptr;

    void* ptr = mp_malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

MP_API void* mp_realloc(void* ptr, size_t new_size) {
    if (!ptr) return mp_malloc(new_size);
    if (new_size == 0) {
        mp_free(ptr);
        return nullptr;
    }
    if (new_size > mp::kMaxBlockSize) {
        // Can't handle sizes above max block size
        return nullptr;
    }

    // Check if the current block is already large enough
    mp::ChunkHeader* chunk = nullptr;
    mp::PageMeta* pm = mp::resolve_block_ptr(ptr, &chunk);
    if (!pm) return nullptr;

    uint32_t old_idx = pm->bucket_idx;
    uint32_t new_idx = mp::sc_index_of(new_size);

    // Same size class: no-op
    if (old_idx == new_idx) return ptr;

    // Need to reallocate
    void* new_ptr = mp_malloc(new_size);
    if (!new_ptr) return nullptr;

    size_t old_size = mp::sc_info(old_idx).block_size;
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_size);
    mp_free(ptr);

    return new_ptr;
}

MP_API mp_arena_t* mp_arena_create(const char* name, size_t max_size) {
    mp::ensure_init();
    uint32_t id = mp::g_arena_id_counter.fetch_add(1);
    mp::Arena* arena = mp::arena_create(id, name, max_size);
    if (!arena) return nullptr;

    {
        std::lock_guard<std::mutex> lock(mp::g_arena_list_mutex);
        arena->next_arena = mp::g_arena_list;
        mp::g_arena_list = arena;
    }

    return reinterpret_cast<mp_arena_t*>(arena);
}

MP_API void mp_arena_destroy(mp_arena_t* arena) {
    if (!arena) return;
    auto* a = reinterpret_cast<mp::Arena*>(arena);

    // Remove from global arena list
    {
        std::lock_guard<std::mutex> lock(mp::g_arena_list_mutex);
        mp::Arena** pp = &mp::g_arena_list;
        while (*pp) {
            if (*pp == a) {
                *pp = a->next_arena;
                break;
            }
            pp = &(*pp)->next_arena;
        }
    }

    mp::arena_destroy(a);
}

MP_API void* mp_arena_malloc(mp_arena_t* arena, size_t size) {
    if (!arena) return mp_malloc(size);
    if (size == 0) size = 1;
    if (size > mp::kMaxBlockSize) return nullptr;

    auto* a = reinterpret_cast<mp::Arena*>(arena);
    mp::TLC* tlc = mp::tlc_get_or_create(a);
    if (!tlc) return nullptr;

    uint32_t idx = mp::sc_index_of(size);
    return mp::tlc_alloc(tlc, idx);
}

MP_API void mp_thread_attach(mp_arena_t* arena) {
    mp::ensure_init();
    auto* a = arena ? reinterpret_cast<mp::Arena*>(arena) : mp::g_default_arena;
    mp::tlc_get_or_create(a);
}

MP_API void mp_thread_detach(void) {
    mp::TLC* tlc = mp::tlc_current();
    if (tlc) {
        mp::tlc_flush(tlc);
        mp::tlc_destroy(tlc);
        mp::tlc_set_current(nullptr);
    }
}

MP_API void mp_stats_get(mp_stats_t* out) {
    if (!out) return;
    mp::ensure_init();
    mp::stats_aggregate(out);
}

MP_API void mp_stats_print(void) {
    mp::ensure_init();
    mp::stats_print();
}

} // extern "C"
