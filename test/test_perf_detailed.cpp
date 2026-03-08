#include <mempool/mempool.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <vector>
#include <thread>
#include <cstdlib>
#include <atomic>
#include <algorithm>
#include <numeric>

using Clock = std::chrono::high_resolution_clock;

static double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// ============================================================
// 1. Single-thread throughput: mempool vs system malloc
//    Tests: alloc-only, free-only, alloc+free total
// ============================================================

struct BenchResult {
    double alloc_ms;
    double free_ms;
    double total_ms;
    double ops_per_ms;  // total ops (alloc+free) / total_ms
};

static BenchResult bench_mempool_single(size_t block_size, int count) {
    mp_init(nullptr);
    std::vector<void*> ptrs(count);

    auto t0 = Clock::now();
    for (int i = 0; i < count; i++) {
        ptrs[i] = mp_malloc(block_size);
    }
    auto t1 = Clock::now();
    for (int i = 0; i < count; i++) {
        mp_free(ptrs[i]);
    }
    auto t2 = Clock::now();

    BenchResult r;
    r.alloc_ms = elapsed_ms(t0, t1);
    r.free_ms = elapsed_ms(t1, t2);
    r.total_ms = elapsed_ms(t0, t2);
    r.ops_per_ms = (count * 2.0) / r.total_ms;

    mp_shutdown();
    return r;
}

static BenchResult bench_system_single(size_t block_size, int count) {
    std::vector<void*> ptrs(count);

    auto t0 = Clock::now();
    for (int i = 0; i < count; i++) {
        ptrs[i] = malloc(block_size);
    }
    auto t1 = Clock::now();
    for (int i = 0; i < count; i++) {
        free(ptrs[i]);
    }
    auto t2 = Clock::now();

    BenchResult r;
    r.alloc_ms = elapsed_ms(t0, t1);
    r.free_ms = elapsed_ms(t1, t2);
    r.total_ms = elapsed_ms(t0, t2);
    r.ops_per_ms = (count * 2.0) / r.total_ms;
    return r;
}

// ============================================================
// 2. Multi-thread scalability: mempool vs system malloc
// ============================================================

struct MTResult {
    double total_ms;
    double ops_per_ms;
    int total_ops;
};

static MTResult bench_mempool_mt(int num_threads, size_t block_size, int allocs_per_thread) {
    mp_init(nullptr);

    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([block_size, allocs_per_thread]() {
            std::vector<void*> ptrs(allocs_per_thread);
            for (int i = 0; i < allocs_per_thread; i++) {
                ptrs[i] = mp_malloc(block_size);
            }
            for (int i = 0; i < allocs_per_thread; i++) {
                mp_free(ptrs[i]);
            }
            mp_thread_detach();
        });
    }
    for (auto& t : threads) t.join();
    auto t1 = Clock::now();

    MTResult r;
    r.total_ops = num_threads * allocs_per_thread * 2;
    r.total_ms = elapsed_ms(t0, t1);
    r.ops_per_ms = r.total_ops / r.total_ms;

    mp_shutdown();
    return r;
}

static MTResult bench_system_mt(int num_threads, size_t block_size, int allocs_per_thread) {
    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([block_size, allocs_per_thread]() {
            std::vector<void*> ptrs(allocs_per_thread);
            for (int i = 0; i < allocs_per_thread; i++) {
                ptrs[i] = malloc(block_size);
            }
            for (int i = 0; i < allocs_per_thread; i++) {
                free(ptrs[i]);
            }
        });
    }
    for (auto& t : threads) t.join();
    auto t1 = Clock::now();

    MTResult r;
    r.total_ops = num_threads * allocs_per_thread * 2;
    r.total_ms = elapsed_ms(t0, t1);
    r.ops_per_ms = r.total_ops / r.total_ms;
    return r;
}

// ============================================================
// 3. Alloc-free interleaved (realistic pattern)
// ============================================================

static BenchResult bench_mempool_interleaved(size_t block_size, int count) {
    mp_init(nullptr);

    auto t0 = Clock::now();
    for (int i = 0; i < count; i++) {
        void* p = mp_malloc(block_size);
        mp_free(p);
    }
    auto t1 = Clock::now();

    BenchResult r;
    r.alloc_ms = 0;
    r.free_ms = 0;
    r.total_ms = elapsed_ms(t0, t1);
    r.ops_per_ms = (count * 2.0) / r.total_ms;

    mp_shutdown();
    return r;
}

static BenchResult bench_system_interleaved(size_t block_size, int count) {
    auto t0 = Clock::now();
    for (int i = 0; i < count; i++) {
        void* p = malloc(block_size);
        free(p);
    }
    auto t1 = Clock::now();

    BenchResult r;
    r.alloc_ms = 0;
    r.free_ms = 0;
    r.total_ms = elapsed_ms(t0, t1);
    r.ops_per_ms = (count * 2.0) / r.total_ms;
    return r;
}

// ============================================================
// 4. Multi-thread mixed size classes
// ============================================================

static MTResult bench_mempool_mt_mixed(int num_threads, int allocs_per_thread) {
    mp_init(nullptr);
    static const size_t sizes[] = {16, 32, 48, 64, 96, 128, 256, 512, 1024, 2048, 4096};
    static const int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([allocs_per_thread, t]() {
            std::vector<void*> ptrs(allocs_per_thread);
            for (int i = 0; i < allocs_per_thread; i++) {
                size_t sz = sizes[(t * 7 + i * 13) % nsizes];
                ptrs[i] = mp_malloc(sz);
            }
            for (int i = 0; i < allocs_per_thread; i++) {
                mp_free(ptrs[i]);
            }
            mp_thread_detach();
        });
    }
    for (auto& t : threads) t.join();
    auto t1 = Clock::now();

    MTResult r;
    r.total_ops = num_threads * allocs_per_thread * 2;
    r.total_ms = elapsed_ms(t0, t1);
    r.ops_per_ms = r.total_ops / r.total_ms;
    mp_shutdown();
    return r;
}

static MTResult bench_system_mt_mixed(int num_threads, int allocs_per_thread) {
    static const size_t sizes[] = {16, 32, 48, 64, 96, 128, 256, 512, 1024, 2048, 4096};
    static const int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([allocs_per_thread, t]() {
            std::vector<void*> ptrs(allocs_per_thread);
            for (int i = 0; i < allocs_per_thread; i++) {
                size_t sz = sizes[(t * 7 + i * 13) % nsizes];
                ptrs[i] = malloc(sz);
            }
            for (int i = 0; i < allocs_per_thread; i++) {
                free(ptrs[i]);
            }
        });
    }
    for (auto& t : threads) t.join();
    auto t1 = Clock::now();

    MTResult r;
    r.total_ops = num_threads * allocs_per_thread * 2;
    r.total_ms = elapsed_ms(t0, t1);
    r.ops_per_ms = r.total_ops / r.total_ms;
    return r;
}

// ============================================================
// 5. Fast path hit rate measurement
// ============================================================

static void bench_fast_path_stats() {
    mp_init(nullptr);

    // Warm up: trigger initial slow path
    void* warm = mp_malloc(64);
    mp_free(warm);

    // Reset by shutdown and reinit
    mp_shutdown();
    mp_init(nullptr);

    const int N = 100000;
    for (int i = 0; i < N; i++) {
        void* p = mp_malloc(64);
        mp_free(p);
    }

    mp_stats_t st;
    memset(&st, 0, sizeof(st));
    mp_stats_get(&st);

    printf("\n=== Fast Path Statistics (64B, %d alloc+free cycles) ===\n", N);
    printf("  Alloc count:     %llu\n", (unsigned long long)st.alloc_count);
    printf("  Free count:      %llu\n", (unsigned long long)st.free_count);
    printf("  Fast path hits:  %llu\n", (unsigned long long)st.fast_path_hits);
    printf("  Slow path hits:  %llu\n", (unsigned long long)st.slow_path_hits);
    if (st.fast_path_hits + st.slow_path_hits > 0) {
        double rate = 100.0 * st.fast_path_hits / (st.fast_path_hits + st.slow_path_hits);
        printf("  Fast path rate:  %.2f%%\n", rate);
    }
    printf("  Chunks used:     %llu\n", (unsigned long long)st.chunk_count);
    printf("  Pages alloc'd:   %llu\n", (unsigned long long)st.page_alloc_count);
    printf("  Pages freed:     %llu\n", (unsigned long long)st.page_free_count);

    mp_shutdown();
}

// ============================================================
// 6. Cross-thread free benchmark
// ============================================================

static void bench_cross_thread_free(int count) {
    mp_init(nullptr);

    std::vector<void*> ptrs(count);

    // Thread A: allocate
    std::thread producer([&ptrs, count]() {
        for (int i = 0; i < count; i++) {
            ptrs[i] = mp_malloc(64);
        }
        mp_thread_detach();
    });
    producer.join();

    // Thread B: free (cross-thread)
    auto t0 = Clock::now();
    std::thread consumer([&ptrs, count]() {
        for (int i = 0; i < count; i++) {
            mp_free(ptrs[i]);
        }
        mp_thread_detach();
    });
    consumer.join();
    auto t1 = Clock::now();

    double ms = elapsed_ms(t0, t1);
    printf("  Cross-thread free %d blocks (64B): %.2f ms  (%.0f frees/ms)\n",
           count, ms, count / ms);

    mp_shutdown();
}

// ============================================================
// Main: run all benchmarks and collect results
// ============================================================

int main() {
    const int N = 100000;  // per-size-class count
    const int MT_N = 100000;  // per-thread count

    // Run each bench 3 times, take median
    auto median3 = [](double a, double b, double c) -> double {
        double arr[3] = {a, b, c};
        std::sort(arr, arr + 3);
        return arr[1];
    };

    printf("================================================================\n");
    printf("  mempool Detailed Performance Benchmark\n");
    printf("  Operations per test: %d alloc + %d free = %d ops\n", N, N, N * 2);
    printf("  Each test run 3 times, median selected\n");
    printf("================================================================\n");

    // --- 1. Single-thread: batch alloc then batch free ---
    printf("\n[1] Single-thread batch alloc+free (mempool vs system malloc)\n");
    printf("--------------------------------------------------------------\n");
    printf("  %-8s | %-10s %-10s %-10s %-12s | %-10s %-10s %-10s %-12s | %-8s\n",
           "Size", "mp alloc", "mp free", "mp total", "mp ops/ms",
           "sys alloc", "sys free", "sys total", "sys ops/ms", "Speedup");
    printf("  %-8s-+-%-10s-%-10s-%-10s-%-12s-+-%-10s-%-10s-%-10s-%-12s-+-%-8s\n",
           "--------", "----------", "----------", "----------", "------------",
           "----------", "----------", "----------", "------------", "--------");

    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < nsizes; s++) {
        BenchResult mp[3], sy[3];
        for (int r = 0; r < 3; r++) {
            mp[r] = bench_mempool_single(sizes[s], N);
            sy[r] = bench_system_single(sizes[s], N);
        }
        double mp_alloc = median3(mp[0].alloc_ms, mp[1].alloc_ms, mp[2].alloc_ms);
        double mp_free  = median3(mp[0].free_ms,  mp[1].free_ms,  mp[2].free_ms);
        double mp_total = median3(mp[0].total_ms, mp[1].total_ms, mp[2].total_ms);
        double mp_ops   = (N * 2.0) / mp_total;

        double sy_alloc = median3(sy[0].alloc_ms, sy[1].alloc_ms, sy[2].alloc_ms);
        double sy_free  = median3(sy[0].free_ms,  sy[1].free_ms,  sy[2].free_ms);
        double sy_total = median3(sy[0].total_ms, sy[1].total_ms, sy[2].total_ms);
        double sy_ops   = (N * 2.0) / sy_total;

        double speedup = sy_total / mp_total;

        char label[16];
        snprintf(label, sizeof(label), "%zuB", sizes[s]);
        printf("  %-8s | %8.2f   %8.2f   %8.2f   %10.0f   | %8.2f   %8.2f   %8.2f   %10.0f   | x%.2f\n",
               label, mp_alloc, mp_free, mp_total, mp_ops,
               sy_alloc, sy_free, sy_total, sy_ops, speedup);
    }

    // --- 2. Single-thread: interleaved alloc+free ---
    printf("\n[2] Single-thread interleaved alloc+free (alloc-free-alloc-free...)\n");
    printf("--------------------------------------------------------------\n");
    printf("  %-8s | %-10s %-12s | %-10s %-12s | %-8s\n",
           "Size", "mp total", "mp ops/ms", "sys total", "sys ops/ms", "Speedup");
    printf("  %-8s-+-%-10s-%-12s-+-%-10s-%-12s-+-%-8s\n",
           "--------", "----------", "------------", "----------", "------------", "--------");

    for (int s = 0; s < nsizes; s++) {
        double mp_t[3], sy_t[3];
        for (int r = 0; r < 3; r++) {
            auto mpr = bench_mempool_interleaved(sizes[s], N);
            auto syr = bench_system_interleaved(sizes[s], N);
            mp_t[r] = mpr.total_ms;
            sy_t[r] = syr.total_ms;
        }
        double mp_total = median3(mp_t[0], mp_t[1], mp_t[2]);
        double sy_total = median3(sy_t[0], sy_t[1], sy_t[2]);
        double mp_ops = (N * 2.0) / mp_total;
        double sy_ops = (N * 2.0) / sy_total;
        double speedup = sy_total / mp_total;

        char label[16];
        snprintf(label, sizeof(label), "%zuB", sizes[s]);
        printf("  %-8s | %8.2f   %10.0f   | %8.2f   %10.0f   | x%.2f\n",
               label, mp_total, mp_ops, sy_total, sy_ops, speedup);
    }

    // --- 3. Multi-thread scalability (fixed size 64B) ---
    printf("\n[3] Multi-thread scalability (64B, %d allocs/thread)\n", MT_N);
    printf("--------------------------------------------------------------\n");
    printf("  %-8s | %-10s %-12s | %-10s %-12s | %-8s\n",
           "Threads", "mp ms", "mp ops/ms", "sys ms", "sys ops/ms", "Speedup");
    printf("  %-8s-+-%-10s-%-12s-+-%-10s-%-12s-+-%-8s\n",
           "--------", "----------", "------------", "----------", "------------", "--------");

    int thread_counts[] = {1, 2, 4, 8};
    double mp_1t_ops = 0;
    double sy_1t_ops = 0;

    for (int tc = 0; tc < 4; tc++) {
        int nt = thread_counts[tc];
        double mp_ms[3], sy_ms[3];
        for (int r = 0; r < 3; r++) {
            auto mpr = bench_mempool_mt(nt, 64, MT_N);
            auto syr = bench_system_mt(nt, 64, MT_N);
            mp_ms[r] = mpr.total_ms;
            sy_ms[r] = syr.total_ms;
        }
        double mp_total = median3(mp_ms[0], mp_ms[1], mp_ms[2]);
        double sy_total = median3(sy_ms[0], sy_ms[1], sy_ms[2]);
        int total_ops = nt * MT_N * 2;
        double mp_ops = total_ops / mp_total;
        double sy_ops = total_ops / sy_total;
        double speedup = sy_total / mp_total;

        if (nt == 1) { mp_1t_ops = mp_ops; sy_1t_ops = sy_ops; }

        printf("  %-8d | %8.2f   %10.0f   | %8.2f   %10.0f   | x%.2f\n",
               nt, mp_total, mp_ops, sy_total, sy_ops, speedup);
    }

    // --- 4. Multi-thread mixed size classes ---
    printf("\n[4] Multi-thread mixed sizes (16B~4096B, %d allocs/thread)\n", MT_N);
    printf("--------------------------------------------------------------\n");
    printf("  %-8s | %-10s %-12s | %-10s %-12s | %-8s\n",
           "Threads", "mp ms", "mp ops/ms", "sys ms", "sys ops/ms", "Speedup");
    printf("  %-8s-+-%-10s-%-12s-+-%-10s-%-12s-+-%-8s\n",
           "--------", "----------", "------------", "----------", "------------", "--------");

    for (int tc = 0; tc < 4; tc++) {
        int nt = thread_counts[tc];
        double mp_ms[3], sy_ms[3];
        for (int r = 0; r < 3; r++) {
            auto mpr = bench_mempool_mt_mixed(nt, MT_N);
            auto syr = bench_system_mt_mixed(nt, MT_N);
            mp_ms[r] = mpr.total_ms;
            sy_ms[r] = syr.total_ms;
        }
        double mp_total = median3(mp_ms[0], mp_ms[1], mp_ms[2]);
        double sy_total = median3(sy_ms[0], sy_ms[1], sy_ms[2]);
        int total_ops = nt * MT_N * 2;
        double mp_ops = total_ops / mp_total;
        double sy_ops = total_ops / sy_total;
        double speedup = sy_total / mp_total;

        printf("  %-8d | %8.2f   %10.0f   | %8.2f   %10.0f   | x%.2f\n",
               nt, mp_total, mp_ops, sy_total, sy_ops, speedup);
    }

    // --- 5. Cross-thread free ---
    printf("\n[5] Cross-thread free performance\n");
    printf("--------------------------------------------------------------\n");
    bench_cross_thread_free(50000);

    // --- 6. Fast path statistics ---
    bench_fast_path_stats();

    printf("\n================================================================\n");
    printf("  All benchmarks complete.\n");
    printf("================================================================\n");

    return 0;
}
