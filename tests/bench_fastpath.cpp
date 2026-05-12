// real_tests/bench_fastpath.cpp
// Thesis §6.2 scenario 6 / §6.3.4: fast path hit rate via mp_stats_*.
// REQUIRES the library to be built with -DMEMPOOL_STATS=ON.
// If MEMPOOL_STATS is off the counters stay zero — this binary then
// just reports zeros (and a warning), it does not abort.

#include <mempool/mempool.h>
#include "bench_common.h"
#include <atomic>
#include <thread>

using namespace bench;

static void run_single_size(size_t sz, long ops, int K) {
    std::vector<void*> ring(K);
    for (int i = 0; i < K; i++) {
        ring[i] = mp_malloc(sz);
        *static_cast<volatile uint64_t*>(ring[i]) = (uint64_t)i;
    }
    for (long i = 0; i < ops - K; i++) {
        int slot = (int)(i % K);
        mp_free(ring[slot]);
        ring[slot] = mp_malloc(sz);
        *static_cast<volatile uint64_t*>(ring[slot]) = (uint64_t)i;
    }
    for (int i = 0; i < K; i++) mp_free(ring[i]);
}

static void run_batch(size_t sz, long ops) {
    std::vector<void*> ptrs(ops);
    for (long i = 0; i < ops; i++) ptrs[i] = mp_malloc(sz);
    for (long i = 0; i < ops; i++) mp_free(ptrs[i]);
}

// Workers signal `done_count` after finishing work, then spin on
// `release` so the caller can read mp_stats_get() while every
// worker's TLC is still attached to its arena. mp_thread_detach /
// the pthread-key destructor unlinks the TLC and drops its
// per-thread counters, so we must read stats before workers exit.
struct ThreadSync {
    std::atomic<int> go{0};
    std::atomic<int> done_count{0};
    std::atomic<int> release{0};
};

// Park-until-released worker pool. Caller pattern:
//   ThreadSync sync;
//   std::vector<std::thread> ths;
//   spawn_parked(n, ops, sz, K, sync, ths);
//   // workers now parked — read stats here
//   sync.release.store(1, std::memory_order_release);
//   for (auto& t : ths) t.join();
static void spawn_parked(int n, long ops_per_thread, size_t sz, int K,
                         ThreadSync& sync, std::vector<std::thread>& ths) {
    ths.reserve(n);
    for (int t = 0; t < n; t++) {
        ths.emplace_back([t, n, &sync, ops_per_thread, sz, K] {
            pin_to_cpu_if_room(t, n);
            while (!sync.go.load(std::memory_order_acquire)) {}
            std::vector<void*> ring(K);
            for (int i = 0; i < K; i++) {
                ring[i] = mp_malloc(sz);
                *static_cast<volatile uint64_t*>(ring[i]) = (uint64_t)i;
            }
            for (long i = 0; i < ops_per_thread - K; i++) {
                int slot = (int)(i % K);
                mp_free(ring[slot]);
                ring[slot] = mp_malloc(sz);
                *static_cast<volatile uint64_t*>(ring[slot]) = (uint64_t)i;
            }
            for (int i = 0; i < K; i++) mp_free(ring[i]);

            sync.done_count.fetch_add(1, std::memory_order_release);
            while (!sync.release.load(std::memory_order_acquire)) {}
            mp_thread_detach();
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sync.go.store(1, std::memory_order_release);
    while (sync.done_count.load(std::memory_order_acquire) < n) {}
}

struct Snap { uint64_t alloc, fast, slow; };
static Snap snap() {
    mp_stats_t s{};
    mp_stats_get(&s);
    return {s.alloc_count, s.fast_path_hits, s.slow_path_hits};
}
static void report(const char* label, Snap a, Snap b) {
    uint64_t da = b.alloc - a.alloc;
    uint64_t df = b.fast  - a.fast;
    uint64_t ds = b.slow  - a.slow;
    double pct = da > 0 ? 100.0 * df / da : 0.0;
    printf("  %-28s alloc=%-9llu fast=%-9llu slow=%-9llu hit=%6.2f%%\n",
           label, (unsigned long long)da, (unsigned long long)df,
           (unsigned long long)ds, pct);
}

int main() {
    print_env_banner("bench_fastpath (requires MEMPOOL_STATS=ON)");
    mp_init(nullptr);

    // Sanity: if STATS is disabled, alloc_count stays 0 forever.
    {
        void* p = mp_malloc(64); mp_free(p);
        mp_stats_t s{}; mp_stats_get(&s);
        if (s.alloc_count == 0) {
            printf("\n[!] mp_stats_get returned alloc_count=0 after a real alloc.\n"
                   "    Build with -DMEMPOOL_STATS=ON to enable counters.\n"
                   "    Continuing — all rows below will read zeros.\n");
        }
    }

    printf("\n[Scenario 6] Fast-path direct-hit rate\n");

    // Warmup so first slow paths from page allocation don't dominate.
    run_single_size(16, 10000, 64);

    {
        auto a = snap(); run_batch(16, 100000);             auto b = snap();
        report("batch_16B (100k+100k)", a, b);
    }
    {
        auto a = snap(); run_single_size(16, 100000, 64);   auto b = snap();
        report("churn_16B_K64", a, b);
    }
    {
        auto a = snap(); run_single_size(16, 100000, 512);  auto b = snap();
        report("churn_16B_K512", a, b);
    }
    {
        auto a = snap(); run_single_size(4096, 100000, 64); auto b = snap();
        report("churn_4096B_K64", a, b);
    }
    {
        auto a = snap(); run_single_size(4096, 100000, 4);  auto b = snap();
        report("churn_4096B_K4", a, b);
    }
    auto run_mt_with_snap = [](const char* label, int n, long ops, size_t sz, int K) {
        ThreadSync sync;
        std::vector<std::thread> ths;
        auto a = snap();
        spawn_parked(n, ops, sz, K, sync, ths);
        auto b = snap();
        sync.release.store(1, std::memory_order_release);
        for (auto& t : ths) t.join();
        report(label, a, b);
    };
    run_mt_with_snap("8threads_64B_K64", 8, 100000, 64, 64);
    run_mt_with_snap("8threads_64B_K16", 8, 100000, 64, 16);

    mp_shutdown();
    return 0;
}
