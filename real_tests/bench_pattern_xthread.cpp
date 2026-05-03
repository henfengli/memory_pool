// real_tests/bench_pattern_xthread.cpp
// Thesis §6.2 scenario 5: cross-thread free.
// 1/2/4 producer-consumer pairs, fixed 64B, SPSC ring per pair.
// Producer alloc + push, consumer pop + free.

#include <mempool/mempool.h>
#include "bench_common.h"
#include <atomic>
#include <thread>
#include <vector>

using namespace bench;

static const long kOpsPerPair = 100000;
static const int  kRuns       = 5;
static const int  kPairCounts[] = {1, 2, 4};

struct SPSC {
    static constexpr size_t N = 4096;
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};
    void* slots[N];

    bool push(void* p) {
        size_t t = tail.load(std::memory_order_relaxed);
        size_t h = head.load(std::memory_order_acquire);
        if (t - h >= N) return false;
        slots[t & (N - 1)] = p;
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
    void* pop() {
        size_t h = head.load(std::memory_order_relaxed);
        size_t t = tail.load(std::memory_order_acquire);
        if (h == t) return nullptr;
        void* p = slots[h & (N - 1)];
        head.store(h + 1, std::memory_order_release);
        return p;
    }
};

template <typename Alloc, typename Free>
double run_xthread(int pairs, size_t sz, Alloc alloc, Free freefn) {
    return median_ms(kRuns, [&] {
        std::vector<SPSC> rings(pairs);
        std::atomic<int> go{0};

        std::vector<std::thread> ths;
        ths.reserve(pairs * 2);
        int total_threads = pairs * 2;
        for (int p = 0; p < pairs; p++) {
            ths.emplace_back([&, p] {
                pin_to_cpu_if_room(p * 2, total_threads);
                while (!go.load(std::memory_order_acquire)) {}
                for (long i = 0; i < kOpsPerPair; i++) {
                    void* ptr = alloc(sz);
                    while (!rings[p].push(ptr)) {} // spin
                }
                mp_thread_detach();
            });
            ths.emplace_back([&, p] {
                pin_to_cpu_if_room(p * 2 + 1, total_threads);
                while (!go.load(std::memory_order_acquire)) {}
                long got = 0;
                while (got < kOpsPerPair) {
                    void* ptr = rings[p].pop();
                    if (ptr) { freefn(ptr); ++got; }
                }
                mp_thread_detach();
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        go.store(1, std::memory_order_release);
        for (auto& th : ths) th.join();
    });
}

static double total_ops_per_ms(int pairs, double total_ms) {
    return total_ms > 0 ? (pairs * kOpsPerPair * 2.0) / total_ms : 0.0;
}

int main() {
    print_env_banner("bench_pattern_xthread (cross-thread free, 64B)");

    auto mp_a = [](size_t s) { return mp_malloc(s); };
    auto mp_f = [](void* p)  { mp_free(p); };
    auto sy_a = [](size_t s) { return ::malloc(s); };
    auto sy_f = [](void* p)  { ::free(p); };

    printf("\n[Scenario 5] Cross-thread free, 64B, 1/2/4 pairs\n");
    printf("  %-5s | %12s %12s | %12s %12s | %8s\n",
           "pairs", "mp ms", "mp ops/ms", "sys ms", "sys ops/ms", "speedup");
    printf("  ------+--------------------------+--------------------------+---------\n");

    mp_init(nullptr);
    for (int p : kPairCounts) {
        double mp_ms = run_xthread(p, 64, mp_a, mp_f);
        double sy_ms = run_xthread(p, 64, sy_a, sy_f);
        printf("  %5d | %10.2f   %10.0f   | %10.2f   %10.0f   | x%6.2f\n",
               p, mp_ms, total_ops_per_ms(p, mp_ms),
               sy_ms, total_ops_per_ms(p, sy_ms), sy_ms / mp_ms);
    }
    mp_shutdown();
    return 0;
}
