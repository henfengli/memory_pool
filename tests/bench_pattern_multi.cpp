// real_tests/bench_pattern_multi.cpp
// Thesis §6.2 scenarios 3 (scalability_64) and 4 (mixed_sc).
// Each worker independently runs the K=256 interleaved pattern.
// No cross-thread free here — that is bench_pattern_xthread.

#include <mempool/mempool.h>
#include "bench_common.h"
#include <atomic>
#include <random>
#include <thread>

using namespace bench;

static const long kOpsPerThread = 100000;
static const int  kRuns         = 5;
static const int  kThreadCounts[] = {1, 2, 4, 8};

template <typename Alloc, typename Free>
void worker_fixed(int tid, int nthreads, size_t sz, Alloc alloc, Free freefn,
                  std::atomic<int>* go) {
    pin_to_cpu_if_room(tid, nthreads);
    while (!go->load(std::memory_order_acquire)) {}
    constexpr int K = 256;
    void* ring[K];
    for (int i = 0; i < K; i++) {
        ring[i] = alloc(sz);
        *static_cast<volatile uint64_t*>(ring[i]) = (uint64_t)i;
    }
    for (long i = 0; i < kOpsPerThread - K; i++) {
        int slot = (int)(i & (K - 1));
        freefn(ring[slot]);
        ring[slot] = alloc(sz);
        *static_cast<volatile uint64_t*>(ring[slot]) = (uint64_t)i;
    }
    for (int i = 0; i < K; i++) freefn(ring[i]);
    mp_thread_detach();  // no-op for system path
}

template <typename Alloc, typename Free>
void worker_mixed(int tid, int nthreads, Alloc alloc, Free freefn,
                  std::atomic<int>* go) {
    pin_to_cpu_if_room(tid, nthreads);
    while (!go->load(std::memory_order_acquire)) {}
    static const size_t mix[] = {16, 64, 128, 256, 512, 1024, 2048};
    constexpr int N = sizeof(mix) / sizeof(mix[0]);
    constexpr int K = 256;

    void* ring[K];
    size_t ring_sz[K];
    std::mt19937 rng(0xC0FFEEu ^ (uint32_t)tid);
    std::uniform_int_distribution<int> dist(0, N - 1);

    for (int i = 0; i < K; i++) {
        ring_sz[i] = mix[dist(rng)];
        ring[i] = alloc(ring_sz[i]);
        *static_cast<volatile uint64_t*>(ring[i]) = (uint64_t)i;
    }
    for (long i = 0; i < kOpsPerThread - K; i++) {
        int slot = (int)(i & (K - 1));
        freefn(ring[slot]);
        ring_sz[slot] = mix[dist(rng)];
        ring[slot] = alloc(ring_sz[slot]);
        *static_cast<volatile uint64_t*>(ring[slot]) = (uint64_t)i;
    }
    for (int i = 0; i < K; i++) freefn(ring[i]);
    mp_thread_detach();
}

// Suppress mp_thread_detach link error for system path: we wrap workers in
// templates that include the call; mp_thread_detach is safe to call when
// no TLC exists for the calling thread (it just no-ops). For the system
// allocator workers we still call it — it is harmless and keeps the two
// code paths textually identical inside the timed region.

// Time only the work region: spawn threads, wait until they're parked at
// the go barrier, then start the timer, fire `go`, join, stop timer.
// The 5ms parking sleep used to live inside the timed region — that cost
// landed entirely on every measurement and dominated runs <50ms wide.
template <typename WorkerFn>
double run_threaded(int n, WorkerFn fn) {
    std::vector<double> samples;
    samples.reserve(kRuns);
    for (int r = 0; r < kRuns; r++) {
        std::atomic<int> go{0};
        std::atomic<int> ready{0};
        std::vector<std::thread> ths;
        ths.reserve(n);
        for (int t = 0; t < n; t++) {
            ths.emplace_back([t, n, &fn, &go, &ready] {
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) {}
                fn(t, n, &go);
            });
        }
        while (ready.load(std::memory_order_acquire) < n) {}
        auto t0 = Clock::now();
        go.store(1, std::memory_order_release);
        for (auto& th : ths) th.join();
        auto t1 = Clock::now();
        samples.push_back(elapsed_ms(t0, t1));
    }
    std::sort(samples.begin(), samples.end());
    return samples[kRuns / 2];
}

static double total_ops_per_ms(int n, double total_ms) {
    return total_ms > 0 ? (n * kOpsPerThread * 2.0) / total_ms : 0.0;
}

int main() {
    print_env_banner("bench_pattern_multi (scalability_64 + mixed_sc, K=256/thread)");

    auto mp_a = [](size_t s) { return mp_malloc(s); };
    auto mp_f = [](void* p)  { mp_free(p); };
    auto sy_a = [](size_t s) { return ::malloc(s); };
    auto sy_f = [](void* p)  { ::free(p); };

    // ---- Scenario 3: fixed 64B ----
    printf("\n[Scenario 3] Multi-thread 64B interleaved (each thread runs K=256, %ld ops)\n", kOpsPerThread * 2);
    printf("  %-3s | %12s %12s | %12s %12s | %8s\n",
           "T", "mp ms", "mp ops/ms", "sys ms", "sys ops/ms", "speedup");
    printf("  ----+--------------------------+--------------------------+---------\n");

    mp_init(nullptr);
    for (int n : kThreadCounts) {
        double mp_ms = run_threaded(n, [&](int tid, int nt, std::atomic<int>* go) {
            worker_fixed(tid, nt, 64, mp_a, mp_f, go);
        });
        double sy_ms = run_threaded(n, [&](int tid, int nt, std::atomic<int>* go) {
            worker_fixed(tid, nt, 64, sy_a, sy_f, go);
        });
        printf("  %3d | %10.2f   %10.0f   | %10.2f   %10.0f   | x%6.2f\n",
               n, mp_ms, total_ops_per_ms(n, mp_ms),
               sy_ms, total_ops_per_ms(n, sy_ms), sy_ms / mp_ms);
    }
    mp_shutdown();

    // ---- Scenario 4: mixed size classes ----
    printf("\n[Scenario 4] Multi-thread mixed sizes (16,64,128,256,512,1024,2048; K=256/thread)\n");
    printf("  %-3s | %12s %12s | %12s %12s | %8s\n",
           "T", "mp ms", "mp ops/ms", "sys ms", "sys ops/ms", "speedup");
    printf("  ----+--------------------------+--------------------------+---------\n");

    mp_init(nullptr);
    for (int n : kThreadCounts) {
        double mp_ms = run_threaded(n, [&](int tid, int nt, std::atomic<int>* go) {
            worker_mixed(tid, nt, mp_a, mp_f, go);
        });
        double sy_ms = run_threaded(n, [&](int tid, int nt, std::atomic<int>* go) {
            worker_mixed(tid, nt, sy_a, sy_f, go);
        });
        printf("  %3d | %10.2f   %10.0f   | %10.2f   %10.0f   | x%6.2f\n",
               n, mp_ms, total_ops_per_ms(n, mp_ms),
               sy_ms, total_ops_per_ms(n, sy_ms), sy_ms / mp_ms);
    }
    mp_shutdown();
    return 0;
}
