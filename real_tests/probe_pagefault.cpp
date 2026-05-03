// real_tests/probe_pagefault.cpp
// Test the page-fault hypothesis: does Linux's lazy-commit mmap make
// mempool's free phase look disproportionately slow because the first
// write to each block (in mp_free's `blk->next = ...`) triggers the
// page fault?
//
// Two modes per size:
//   "raw"  : alloc loop, free loop. mp_free does the first write.
//   "warm" : alloc loop + pre-touch (write 1 byte per block right after
//            alloc, before timing free). Free now writes to a hot page.
//
// If the page-fault hypothesis holds:
//   - raw alloc fast, raw free slow
//   - warm alloc slow (eats fault cost), warm free fast
//   - raw total ≈ warm total (just shuffled)
//
// If hypothesis wrong:
//   - warm total != raw total → some other effect dominates

#include <mempool/mempool.h>
#include "bench_common.h"
#include <cstring>

using namespace bench;

static const long N = 100000;

struct Phase { double alloc_ms, fault_ms, free_ms, total_ms; };

static Phase run_raw(size_t sz) {
    mp_init(nullptr);
    std::vector<void*> ptrs(N);

    auto t0 = Clock::now();
    for (long i = 0; i < N; i++) ptrs[i] = mp_malloc(sz);
    auto t1 = Clock::now();
    for (long i = 0; i < N; i++) mp_free(ptrs[i]);
    auto t2 = Clock::now();

    Phase r{
        elapsed_ms(t0, t1),
        0.0,
        elapsed_ms(t1, t2),
        elapsed_ms(t0, t2)
    };
    mp_shutdown();
    return r;
}

static Phase run_warm(size_t sz) {
    mp_init(nullptr);
    std::vector<void*> ptrs(N);

    auto t0 = Clock::now();
    for (long i = 0; i < N; i++) ptrs[i] = mp_malloc(sz);
    auto t1 = Clock::now();
    // Pre-touch each block to force page commit before the timed free phase.
    for (long i = 0; i < N; i++) *(volatile char*)ptrs[i] = 0;
    auto t2 = Clock::now();
    for (long i = 0; i < N; i++) mp_free(ptrs[i]);
    auto t3 = Clock::now();

    Phase r{
        elapsed_ms(t0, t1),
        elapsed_ms(t1, t2),
        elapsed_ms(t2, t3),
        elapsed_ms(t0, t3) - elapsed_ms(t1, t2)  // exclude the artificial pre-touch from total
    };
    mp_shutdown();
    return r;
}

int main() {
    print_env_banner("probe_pagefault");
    printf("\nbatch %ld ops/run, single sample (no median, just to see direction)\n", N * 2);
    printf("\n  size  | mode  | alloc ms | pretouch ms | free ms | total ms (excl pretouch)\n");
    printf("  ------+-------+----------+-------------+---------+--------------------------\n");

    for (size_t sz : {16, 64, 256, 1024, 4096}) {
        // run each twice, second is more honest (page tables warm in kernel)
        run_raw(sz); run_warm(sz);

        Phase r = run_raw(sz);
        Phase w = run_warm(sz);

        printf("  %5zuB | raw   | %8.2f |          -- | %7.2f | %8.2f\n",
               sz, r.alloc_ms, r.free_ms, r.total_ms);
        printf("  %5zuB | warm  | %8.2f | %11.2f | %7.2f | %8.2f\n",
               sz, w.alloc_ms, w.fault_ms, w.free_ms, w.total_ms);
        printf("  ------+-------+----------+-------------+---------+--------------------------\n");
    }
    return 0;
}
