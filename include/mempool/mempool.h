#ifndef MEMPOOL_MEMPOOL_H
#define MEMPOOL_MEMPOOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Export macro --- */
#ifdef _WIN32
    #ifdef MEMPOOL_BUILDING
        #define MP_API __declspec(dllexport)
    #else
        #define MP_API __declspec(dllimport)
    #endif
#else
    #ifdef MEMPOOL_BUILDING
        #define MP_API __attribute__((visibility("default")))
    #else
        #define MP_API
    #endif
#endif

/* --- Opaque types --- */
typedef struct mp_arena_s mp_arena_t;

/* --- Configuration --- */
typedef struct mp_config {
    size_t arena_max_size;   /* 0 = unlimited */
    int    enable_stats;     /* 1 = enable statistics */
    int    debug_fill;       /* 1 = fill alloc/free memory with patterns */
} mp_config_t;

/* --- Statistics --- */
typedef struct mp_stats {
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t alloc_bytes;
    uint64_t free_bytes;
    uint64_t active_bytes;
    uint64_t arena_count;
    uint64_t chunk_count;
    uint64_t page_alloc_count;
    uint64_t page_free_count;
    uint64_t fast_path_hits;
    uint64_t slow_path_hits;
} mp_stats_t;

/* --- Core API --- */
MP_API int         mp_init(const mp_config_t* config);
MP_API void        mp_shutdown(void);

MP_API void*       mp_malloc(size_t size);
MP_API void        mp_free(void* ptr);
MP_API void*       mp_calloc(size_t count, size_t size);
MP_API void*       mp_realloc(void* ptr, size_t new_size);

/* --- Arena API --- */
MP_API mp_arena_t* mp_arena_create(const char* name, size_t max_size);
MP_API void        mp_arena_destroy(mp_arena_t* arena);
MP_API void*       mp_arena_malloc(mp_arena_t* arena, size_t size);

/* --- Thread API --- */
MP_API void        mp_thread_attach(mp_arena_t* arena);
MP_API void        mp_thread_detach(void);

/* --- Stats API --- */
MP_API void        mp_stats_get(mp_stats_t* out);
MP_API void        mp_stats_print(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMPOOL_MEMPOOL_H */
