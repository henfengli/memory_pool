#include "stats.h"
#include "arena.h"
#include "tlc.h"
#include <cstdio>
#include <cstring>

namespace mp {

// These are defined in mempool.cpp
extern Arena* g_default_arena;
extern Arena* g_arena_list;
extern std::mutex g_arena_list_mutex;

void stats_aggregate(mp_stats_t* out) {
    memset(out, 0, sizeof(*out));

    std::lock_guard<std::mutex> lock(g_arena_list_mutex);

    for (Arena* arena = g_arena_list; arena; arena = arena->next_arena) {
        out->arena_count++;
        out->chunk_count += arena->stat_chunk_count.load(std::memory_order_relaxed);
        out->page_alloc_count += arena->stat_page_alloc.load(std::memory_order_relaxed);
        out->page_free_count += arena->stat_page_free.load(std::memory_order_relaxed);

        // Aggregate TLC stats
        std::lock_guard<std::mutex> tlc_lock(arena->tlc_mutex);
        for (TLC* tlc = arena->tlc_head; tlc; tlc = tlc->next_in_arena) {
            out->alloc_count   += tlc->stats.alloc_count;
            out->free_count    += tlc->stats.free_count;
            out->alloc_bytes   += tlc->stats.alloc_bytes;
            out->free_bytes    += tlc->stats.free_bytes;
            out->fast_path_hits += tlc->stats.fast_path_hits;
            out->slow_path_hits += tlc->stats.slow_path_hits;
        }
    }

    out->active_bytes = out->alloc_bytes - out->free_bytes;
}

void stats_print() {
    mp_stats_t s;
    stats_aggregate(&s);

    printf("=== mempool statistics ===\n");
    printf("  Arenas:       %llu\n", (unsigned long long)s.arena_count);
    printf("  Chunks:       %llu\n", (unsigned long long)s.chunk_count);
    printf("  Pages alloc:  %llu\n", (unsigned long long)s.page_alloc_count);
    printf("  Pages freed:  %llu\n", (unsigned long long)s.page_free_count);
    printf("  Alloc count:  %llu\n", (unsigned long long)s.alloc_count);
    printf("  Free count:   %llu\n", (unsigned long long)s.free_count);
    printf("  Alloc bytes:  %llu\n", (unsigned long long)s.alloc_bytes);
    printf("  Free bytes:   %llu\n", (unsigned long long)s.free_bytes);
    printf("  Active bytes: %llu\n", (unsigned long long)s.active_bytes);
    printf("  Fast path:    %llu\n", (unsigned long long)s.fast_path_hits);
    printf("  Slow path:    %llu\n", (unsigned long long)s.slow_path_hits);
    printf("==========================\n");
}

bool stats_check_leaks() {
    mp_stats_t s;
    stats_aggregate(&s);

    if (s.alloc_count != s.free_count) {
        fprintf(stderr, "[mempool] LEAK DETECTED: alloc_count=%llu, free_count=%llu, diff=%llu\n",
                (unsigned long long)s.alloc_count,
                (unsigned long long)s.free_count,
                (unsigned long long)(s.alloc_count - s.free_count));
        return true;
    }
    return false;
}

} // namespace mp
