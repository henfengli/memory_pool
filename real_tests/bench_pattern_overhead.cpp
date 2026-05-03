// real_tests/bench_pattern_overhead.cpp
// Thesis Table 6-7: internal fragmentation + resident memory overhead.
//
// Workload:
//   Maintain live_n=8192 outstanding allocations, churn 200,000 iterations
//   total. Two size distributions:
//     uniform_1_4096  : uniform on [1, 4096]
//     weighted_small  : 80% in [1, 128], 20% in [128, 4096]
//
// Metrics:
//   live request bytes      : sum of requested sizes currently held
//   size-class alloc bytes  : sum of size-class block sizes held
//   internal fragmentation  : (alloc - request) / alloc
//   resident bytes          : chunk_count * MP_CHUNK_SIZE (4MB chunks)
//   resident overhead       : resident / live_request - 1
//
// Note: requires MEMPOOL_STATS=ON to read chunk_count via mp_stats_get.

#include <mempool/mempool.h>
#include "bench_common.h"
#include <cstring>
#include <random>

using namespace bench;

static const int  kLiveN = 8192;
static const long kIters = 200000;
static const size_t kChunkSize = 4 * 1024 * 1024;  // matches MP_CHUNK_SIZE

// Table-driven size class roundup. We can't include src/size_class.h
// directly (private header), so reproduce the kSizeClasses table here
// — kept in sync with src/size_class.h.
static const size_t kSizeClassTable[] = {
    16,   32,   48,   64,   80,   96,  112,  128,
    160,  192,  224,  256,
    320,  384,  448,  512,
    640,  768,  1024,
    1280, 1536, 2048,
    2560, 3072, 4096
};
static constexpr int kNumSC = sizeof(kSizeClassTable) / sizeof(kSizeClassTable[0]);

static size_t roundup_sc(size_t sz) {
    if (sz == 0) return kSizeClassTable[0];
    for (int i = 0; i < kNumSC; i++) {
        if (kSizeClassTable[i] >= sz) return kSizeClassTable[i];
    }
    return kSizeClassTable[kNumSC - 1];  // saturated
}

template <typename SizeFn>
static void run_overhead(const char* dist_name, SizeFn&& gen) {
    mp_init(nullptr);

    std::vector<void*>  live(kLiveN, nullptr);
    std::vector<size_t> live_sz(kLiveN, 0);
    std::mt19937 rng(0xCAFE0001u);

    // Prime: fill live[] to live_n.
    uint64_t live_req = 0;
    uint64_t live_alloc = 0;
    for (int i = 0; i < kLiveN; i++) {
        size_t sz = gen(rng);
        live[i] = mp_malloc(sz);
        live_sz[i] = sz;
        live_req += sz;
        live_alloc += roundup_sc(sz);
    }

    // Churn: each iter free a random slot, alloc fresh into it.
    std::uniform_int_distribution<int> slot_dist(0, kLiveN - 1);
    for (long it = 0; it < kIters; it++) {
        int slot = slot_dist(rng);
        live_req   -= live_sz[slot];
        live_alloc -= roundup_sc(live_sz[slot]);
        mp_free(live[slot]);

        size_t sz = gen(rng);
        live[slot] = mp_malloc(sz);
        live_sz[slot] = sz;
        live_req   += sz;
        live_alloc += roundup_sc(sz);
    }

    // Snapshot resident state.
    mp_stats_t st{};
    mp_stats_get(&st);
    uint64_t resident = (uint64_t)st.chunk_count * kChunkSize;

    double internal_frag = (live_alloc > 0)
        ? 100.0 * (double)(live_alloc - live_req) / (double)live_alloc
        : 0.0;
    double resident_overhead = (live_req > 0)
        ? 100.0 * (double)resident / (double)live_req - 100.0
        : 0.0;

    auto fmt_mb = [](uint64_t b) { return b / (1024.0 * 1024.0); };

    printf("  %-20s | %12.2f MB | %12.2f MB | %12.2f MB | %7.2f%% | %8.0f%%\n",
           dist_name,
           fmt_mb(live_req), fmt_mb(live_alloc), fmt_mb(resident),
           internal_frag, resident_overhead);
    fflush(stdout);

    // Drain.
    for (int i = 0; i < kLiveN; i++) mp_free(live[i]);
    mp_shutdown();
}

int main() {
    print_env_banner("bench_pattern_overhead (Table 6-7)");

    if (mp_init(nullptr) == 0) {
        // Probe whether STATS is on: do an alloc/free, see if chunk_count > 0.
        void* p = mp_malloc(64); mp_free(p);
        mp_stats_t st{}; mp_stats_get(&st);
        if (st.chunk_count == 0) {
            printf("\n[!] mp_stats_get returned chunk_count=0 after a real alloc.\n"
                   "    Build with -DMEMPOOL_STATS=ON to enable counters.\n"
                   "    Continuing — resident column will read 0.\n");
        }
        mp_shutdown();
    }

    printf("\n[Table 6-7] Memory overhead (live_n=%d, %ld iters)\n", kLiveN, kIters);
    printf("  %-20s | %15s | %15s | %15s | %8s | %9s\n",
           "distribution", "live request", "size-class alloc", "resident",
           "internal", "resident");
    printf("  %-20s + %15s + %15s + %15s + %8s + %9s\n",
           "                   ", "    (bytes)    ", "    (bytes)    ",
           "    (bytes)    ", "  frag  ", " overhead");
    printf("  ---------------------+-----------------+-----------------+-----------------+----------+----------\n");

    // uniform on [1, 4096]
    {
        std::uniform_int_distribution<int> d(1, 4096);
        run_overhead("uniform_1_4096", [&](std::mt19937& g) -> size_t { return (size_t)d(g); });
    }
    // weighted_small: 80% in [1,128], 20% in [128, 4096]
    {
        std::uniform_real_distribution<double> bias(0.0, 1.0);
        std::uniform_int_distribution<int> small(1, 128);
        std::uniform_int_distribution<int> big(128, 4096);
        run_overhead("weighted_small", [&](std::mt19937& g) -> size_t {
            return bias(g) < 0.8 ? (size_t)small(g) : (size_t)big(g);
        });
    }
    return 0;
}
