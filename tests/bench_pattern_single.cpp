// real_tests/bench_pattern_single.cpp
// Thesis Tables 6-1 (batch) and 6-2 (interleaved K=256).
// 9 size classes × {batch, interleaved} × {mempool, system} × 5 runs.
//
// Methodology choices (matches test/test_perf_detailed.cpp, the historical
// baseline used by docs/性能指标测试.md):
//   * Per-call fresh ptrs vector. Reusing a single vector across runs
//     leaves the pointer array hot in L1/L2 — that artificially boosts
//     glibc's measured throughput (we observed ~3× inflation for sys
//     batch 16B with the reused-vector approach).
//   * mp_init/mp_shutdown PER measurement. Cold-start each time so the
//     mempool's first chunk allocation cost is amortized identically
//     to real one-shot allocator usage.
//   * Median of 5 (matches thesis §6.1).
//   * mp and sys runs interleaved per size, not grouped, so cache
//     warm-up bias is symmetric.

#include <mempool/mempool.h>
#include "bench_common.h"
#include <cstring>

using namespace bench;

static const long kOps  = 100000;
static const int  kRuns = 5;
static const size_t kSizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

// ---------- batch ----------
static double bench_mp_batch(size_t sz) {
    mp_init(nullptr);
    std::vector<void*> ptrs(kOps);
    auto t0 = Clock::now();
    for (long i = 0; i < kOps; i++) ptrs[i] = mp_malloc(sz);
    for (long i = 0; i < kOps; i++) mp_free(ptrs[i]);
    auto t1 = Clock::now();
    mp_shutdown();
    return elapsed_ms(t0, t1);
}
static double bench_sys_batch(size_t sz) {
    std::vector<void*> ptrs(kOps);
    auto t0 = Clock::now();
    for (long i = 0; i < kOps; i++) ptrs[i] = ::malloc(sz);
    for (long i = 0; i < kOps; i++) ::free(ptrs[i]);
    auto t1 = Clock::now();
    return elapsed_ms(t0, t1);
}

// ---------- interleaved K=256 ----------
// Steady-state churn: 256-slot ring, free-then-alloc each iter, write 8 bytes.
// 100k iterations -> 200k ops counted (alloc + free).
static double bench_mp_interleaved(size_t sz) {
    mp_init(nullptr);
    constexpr int K = 256;
    void* ring[K];
    for (int i = 0; i < K; i++) {
        ring[i] = mp_malloc(sz);
        *static_cast<volatile uint64_t*>(ring[i]) = (uint64_t)i;
    }
    auto t0 = Clock::now();
    for (long i = 0; i < kOps - K; i++) {
        int slot = (int)(i & (K - 1));
        mp_free(ring[slot]);
        ring[slot] = mp_malloc(sz);
        *static_cast<volatile uint64_t*>(ring[slot]) = (uint64_t)i;
    }
    auto t1 = Clock::now();
    for (int i = 0; i < K; i++) mp_free(ring[i]);
    mp_shutdown();
    return elapsed_ms(t0, t1);
}
static double bench_sys_interleaved(size_t sz) {
    constexpr int K = 256;
    void* ring[K];
    for (int i = 0; i < K; i++) {
        ring[i] = ::malloc(sz);
        *static_cast<volatile uint64_t*>(ring[i]) = (uint64_t)i;
    }
    auto t0 = Clock::now();
    for (long i = 0; i < kOps - K; i++) {
        int slot = (int)(i & (K - 1));
        ::free(ring[slot]);
        ring[slot] = ::malloc(sz);
        *static_cast<volatile uint64_t*>(ring[slot]) = (uint64_t)i;
    }
    auto t1 = Clock::now();
    for (int i = 0; i < K; i++) ::free(ring[i]);
    return elapsed_ms(t0, t1);
}

template <typename F>
double median_of(int n, F&& body) {
    std::vector<double> v;
    v.reserve(n);
    for (int i = 0; i < n; i++) v.push_back(body());
    std::sort(v.begin(), v.end());
    return v[n / 2];
}

static long batch_ops_count() { return kOps * 2; }
// Interleaved counts (kOps - K) free + (kOps - K) alloc inside the timed
// region. Prime/drain are outside.
static long interleaved_ops_count() { return (kOps - 256) * 2; }

int main() {
    print_env_banner("bench_pattern_single (Tables 6-1 + 6-2)");

    // ===== Table 6-1: batch =====
    printf("\n[Table 6-1] Single-thread batch alloc-then-free, %ld ops/run, median of %d\n",
           batch_ops_count(), kRuns);
    printf("  size  | mp ms      mp ops/ms | sys ms     sys ops/ms | speedup\n");
    printf("  ------+----------------------+-----------------------+--------\n");
    for (size_t sz : kSizes) {
        double mp_ms = median_of(kRuns, [&] { return bench_mp_batch(sz); });
        double sy_ms = median_of(kRuns, [&] { return bench_sys_batch(sz); });
        double mp_ops = batch_ops_count() / mp_ms;
        double sy_ops = batch_ops_count() / sy_ms;
        printf("  %5zuB | %8.2f   %10.0f | %8.2f   %10.0f | x%6.2f\n",
               sz, mp_ms, mp_ops, sy_ms, sy_ops, sy_ms / mp_ms);
        fflush(stdout);
    }

    // ===== Table 6-2: interleaved K=256 =====
    printf("\n[Table 6-2] Single-thread interleaved K=256, %ld ops/run, median of %d\n",
           interleaved_ops_count(), kRuns);
    printf("  size  | mp ms      mp ops/ms | sys ms     sys ops/ms | speedup\n");
    printf("  ------+----------------------+-----------------------+--------\n");
    for (size_t sz : kSizes) {
        double mp_ms = median_of(kRuns, [&] { return bench_mp_interleaved(sz); });
        double sy_ms = median_of(kRuns, [&] { return bench_sys_interleaved(sz); });
        double mp_ops = interleaved_ops_count() / mp_ms;
        double sy_ops = interleaved_ops_count() / sy_ms;
        printf("  %5zuB | %8.2f   %10.0f | %8.2f   %10.0f | x%6.2f\n",
               sz, mp_ms, mp_ops, sy_ms, sy_ops, sy_ms / mp_ms);
        fflush(stdout);
    }
    return 0;
}
