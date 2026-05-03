// real_tests/probe_k_sweep.cpp
// Verify the hypothesis that K=batch_size is a pathological dip for
// mempool's K-ring churn benchmark.
//
// At 16B size class:
//   blocks_per_page = 4096/16 = 256, batch_pages = 1 (since 256 >= 64)
//   one refill = 256 blocks via bump pointer.
//
// Predictions:
//   K << 256 : bump pointer covers many iters → fast path 2 (bump) hits.
//              Fast.
//   K == 256 : bump exhausted at end of prime; every steady iter triggers
//              bucket_collect_local + recursive tlc_alloc. Slowest case.
//   K >> 256 : local_free grows large between collects, so each
//              collect_local serves many subsequent allocs. Fast.
//
// We measure both same-thread free path (mempool) and glibc tcache to
// ground each measurement.

#include <mempool/mempool.h>
#include "bench_common.h"
#include <cstdlib>

using namespace bench;

template <typename Alloc, typename Free>
double run_churn(int K, long iters, size_t sz, Alloc alloc, Free freefn) {
    // ring slot array allocated via system malloc, NOT via the allocator
    // under test (would otherwise blow past mempool's 4096B max for K>=512).
    std::vector<void*> ring(K);
    for (int i = 0; i < K; i++) {
        ring[i] = alloc(sz);
        *static_cast<volatile uint64_t*>(ring[i]) = (uint64_t)i;
    }
    auto t0 = Clock::now();
    for (long i = 0; i < iters; i++) {
        int slot = (int)(i & (K - 1));  // K must be power of 2
        freefn(ring[slot]);
        ring[slot] = alloc(sz);
        *static_cast<volatile uint64_t*>(ring[slot]) = (uint64_t)i;
    }
    auto t1 = Clock::now();
    for (int i = 0; i < K; i++) freefn(ring[i]);
    return elapsed_ms(t0, t1);
}

template <typename F>
static double median_of(int n, F&& body) {
    std::vector<double> v;
    v.reserve(n);
    for (int i = 0; i < n; i++) v.push_back(body());
    std::sort(v.begin(), v.end());
    return v[n / 2];
}

int main() {
    print_env_banner("probe_k_sweep (verify K=batch pathology, 16B)");

    const long iters = 1 << 18;  // 262144 churn iters
    const int Ks[] = {16, 32, 64, 128, 256, 512, 1024, 2048};

    auto mp_a = [](size_t s) { return mp_malloc(s); };
    auto mp_f = [](void* p)  { mp_free(p); };
    auto sy_a = [](size_t s) { return ::malloc(s); };
    auto sy_f = [](void* p)  { ::free(p); };

    printf("\nchurn 16B, %ld iters per run, median of 5\n", iters);
    printf("\n  %-5s | %12s %12s | %12s %12s | %8s\n",
           "K", "mp ms", "mp ns/cycle", "sys ms", "sys ns/cycle", "speedup");
    printf("  ------+--------------------------+--------------------------+---------\n");

    for (int K : Ks) {
        mp_init(nullptr);
        double mp_ms = median_of(5, [&] { return run_churn(K, iters, 16, mp_a, mp_f); });
        mp_shutdown();
        double sy_ms = median_of(5, [&] { return run_churn(K, iters, 16, sy_a, sy_f); });

        double mp_ns = mp_ms * 1e6 / iters;  // alloc+free counted as 1 cycle
        double sy_ns = sy_ms * 1e6 / iters;
        printf("  %5d | %10.2f   %10.1f   | %10.2f   %10.1f   | x%6.2f\n",
               K, mp_ms, mp_ns, sy_ms, sy_ns, sy_ms / mp_ms);
        fflush(stdout);
    }
    return 0;
}
