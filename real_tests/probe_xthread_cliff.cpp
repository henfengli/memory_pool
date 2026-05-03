// real_tests/probe_xthread_cliff.cpp
// Bisect the network-bench cliff. Suspects:
//   (a) 2 producer-consumer pairs (vs 1 pair)
//   (b) mixed size classes per worker (vs single size)
//   (c) kRuns >= 2 — arena state accumulating across runs
//
// Measures wall time per run for a sweep over (pairs, ops_per_pair, run_idx).
// Prints each run individually so we can see if run_idx > 1 degrades.

#include <mempool/mempool.h>
#include "bench_common.h"
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace bench;

struct SPSC {
    static constexpr size_t N = 1 << 16;
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

// One run: `pairs` producer-consumer pairs, each pushes/pops `ops` items.
// Producer alloc(prod_sz) [+ alloc(prod_sz2)] → push.
// Consumer pop → [optionally alloc/free 3 same-thread intermediates] → mp_free pushed item.
// Producers wait for all consumers to finish before mp_thread_detach.
static double one_run(int pairs, long ops, size_t prod_sz, size_t prod_sz2,
                      bool consumer_intermediates) {
    int rings_per_pair = prod_sz2 ? 2 : 1;
    std::vector<SPSC> rings(pairs * rings_per_pair);
    std::atomic<int> go{0};
    std::atomic<int> consumers_done{0};
    std::vector<std::thread> ths;
    ths.reserve(pairs * 2);

    auto t0 = Clock::now();
    for (int p = 0; p < pairs; p++) {
        ths.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire)) {}
            for (long i = 0; i < ops; i++) {
                void* a = mp_malloc(prod_sz);
                while (!rings[p * rings_per_pair].push(a)) {}
                if (prod_sz2) {
                    void* b = mp_malloc(prod_sz2);
                    while (!rings[p * rings_per_pair + 1].push(b)) {}
                }
            }
            while (consumers_done.load(std::memory_order_acquire) < pairs) {}
            mp_thread_detach();
        });
        ths.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire)) {}
            for (long i = 0; i < ops; i++) {
                if (consumer_intermediates) {
                    // Mimic bench_scenario_network's worker: alloc 3 same-thread
                    // intermediates and free them right back, between cross-thread
                    // frees. This stresses the consumer's OWN TLC buckets while
                    // it's also pumping cross-thread frees to producer's TLC.
                    void* h = mp_malloc(64);
                    void* p1 = mp_malloc(192);
                    void* r = mp_malloc(512);
                    *(volatile char*)h = 0; *(volatile char*)p1 = 0; *(volatile char*)r = 0;
                    mp_free(h); mp_free(p1); mp_free(r);
                }
                // pop and cross-thread free first ring
                for (;;) {
                    void* x = rings[p * rings_per_pair].pop();
                    if (!x) continue;
                    mp_free(x);
                    break;
                }
                if (prod_sz2) {
                    for (;;) {
                        void* x = rings[p * rings_per_pair + 1].pop();
                        if (!x) continue;
                        mp_free(x);
                        break;
                    }
                }
            }
            consumers_done.fetch_add(1, std::memory_order_release);
            mp_thread_detach();
        });
    }
    go.store(1, std::memory_order_release);
    for (auto& t : ths) t.join();
    auto t1 = Clock::now();
    return elapsed_ms(t0, t1);
}

static void scenario(const char* tag, int pairs, long ops, size_t s1, size_t s2,
                     bool intermediates, int n_runs) {
    printf("\n[%s]  pairs=%d ops=%ld size=%zu%s%zu  intermediates=%s  runs=%d\n",
           tag, pairs, ops, s1,
           s2 ? "+" : "",
           s2,
           intermediates ? "yes" : "no",
           n_runs);
    printf("  run | wall ms\n");
    printf("  ----+--------\n");
    mp_init(nullptr);
    for (int r = 0; r < n_runs; r++) {
        double ms = one_run(pairs, ops, s1, s2, intermediates);
        printf("  %3d | %8.2f\n", r, ms);
        fflush(stdout);
        if (ms > 30000) { printf("  ↑ timeout, abort\n"); break; }
    }
    mp_shutdown();
}

int main() {
    print_env_banner("probe_xthread_cliff");

    // Baseline reproduction (already known not to cliff)
    scenario("B.1 2pair-224+1024-no_intermediates",   2, 10000, 224, 1024, false, 3);

    // Suspect: consumer's intermediate alloc/free triggers the cliff
    scenario("X.1 2pair-224+1024-with_intermediates", 2, 5000,  224, 1024, true, 3);
    scenario("X.2 2pair-224+1024-with_intermediates", 2, 10000, 224, 1024, true, 3);

    // If X triggers, narrow further: which intermediate size?
    scenario("Y.1 2pair-224+1024-no_intermediates-runs8", 2, 5000, 224, 1024, false, 8);
    scenario("Y.2 2pair-224+1024-with_intermediates-runs8", 2, 5000, 224, 1024, true, 8);

    return 0;
}
