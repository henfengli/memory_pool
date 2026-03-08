#ifndef MEMPOOL_STATS_H
#define MEMPOOL_STATS_H

#include "internal.h"
#include <mempool/mempool.h>

namespace mp {

struct Arena; // forward declaration
struct TLC;   // forward declaration

/* --- Thread-level statistics (non-atomic, only accessed by owning thread) --- */
struct TLCStats {
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t alloc_bytes;
    uint64_t free_bytes;
    uint64_t fast_path_hits;
    uint64_t slow_path_hits;
};

/* --- Statistics functions --- */

// Aggregate stats across all arenas and TLCs into `out`.
void stats_aggregate(mp_stats_t* out);

// Print formatted stats to stdout.
void stats_print();

// Check for leaks (alloc_count != free_count). Returns true if leaks detected.
bool stats_check_leaks();

} // namespace mp

#endif /* MEMPOOL_STATS_H */
