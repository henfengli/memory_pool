// real_tests/bench_common.h
// Shared timing/printing helpers. Used by every bench_*.cpp here.
//
// Design choices that differ from test_by_other_ai/:
//   * Both allocators are passed as lambdas with identical types so the
//     compiler can instantiate the inner loop symmetrically (no fn-ptr
//     vs lambda asymmetry across mempool/system runs).
//   * pin_to_cpu is gated on nproc — pinning to non-existent CPUs on a
//     2 vCPU box is a no-op on Linux but signals nothing useful, and the
//     resulting thread placement is undefined. We just skip pinning when
//     threads > nproc.
//   * Inner loop never allocates std::vector etc. inside the timed
//     region — the vector is a sibling cost that pollutes per-op stats.

#ifndef MEMPOOL_REAL_TESTS_BENCH_COMMON_H
#define MEMPOOL_REAL_TESTS_BENCH_COMMON_H

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace bench {

using Clock = std::chrono::high_resolution_clock;

inline double elapsed_ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

template <typename F>
double median_ms(int runs, F&& body) {
    std::vector<double> samples;
    samples.reserve(runs);
    for (int i = 0; i < runs; i++) {
        auto t0 = Clock::now();
        body();
        auto t1 = Clock::now();
        samples.push_back(elapsed_ms(t0, t1));
    }
    std::sort(samples.begin(), samples.end());
    return samples[runs / 2];
}

inline int num_cpus() {
#if defined(__linux__) && !defined(__ANDROID__)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#else
    return 1;
#endif
}

inline void pin_to_cpu_if_room(int cpu, int total_threads) {
#if defined(__linux__) && !defined(__ANDROID__)
    if (total_threads > num_cpus()) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu % num_cpus(), &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu; (void)total_threads;
#endif
}

extern volatile void* g_sink;
inline void touch(void* p) { g_sink = p; }

// Banner used by every bench main().
inline void print_env_banner(const char* test_name) {
    fprintf(stdout, "# %s   nproc=%d\n", test_name, num_cpus());
}

} // namespace bench

#endif
