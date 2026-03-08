#include <mempool/mempool.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <vector>
#include <thread>
#include <cstdlib>

using Clock = std::chrono::high_resolution_clock;

static double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// Benchmark: single-thread alloc/free throughput
static void bench_single_thread(const char* label, size_t block_size, int count) {
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

    double alloc_ms = elapsed_ms(t0, t1);
    double free_ms = elapsed_ms(t1, t2);
    double total_ms = elapsed_ms(t0, t2);

    printf("  %-30s alloc: %8.2f ms  free: %8.2f ms  total: %8.2f ms  (%d ops, %.0f ops/ms)\n",
           label, alloc_ms, free_ms, total_ms, count * 2, (count * 2) / total_ms);

    mp_shutdown();
}

// Benchmark: system malloc comparison
static void bench_system_malloc(const char* label, size_t block_size, int count) {
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

    double alloc_ms = elapsed_ms(t0, t1);
    double free_ms = elapsed_ms(t1, t2);
    double total_ms = elapsed_ms(t0, t2);

    printf("  %-30s alloc: %8.2f ms  free: %8.2f ms  total: %8.2f ms  (%d ops, %.0f ops/ms)\n",
           label, alloc_ms, free_ms, total_ms, count * 2, (count * 2) / total_ms);
}

// Benchmark: multi-thread scalability
static void bench_multi_thread(int num_threads, size_t block_size, int allocs_per_thread) {
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

    int total_ops = num_threads * allocs_per_thread * 2;
    double ms = elapsed_ms(t0, t1);
    printf("  %d threads x %d allocs (size %zu): %8.2f ms  (%.0f ops/ms)\n",
           num_threads, allocs_per_thread, block_size, ms, total_ops / ms);

    mp_shutdown();
}

int main() {
    const int N = 100000;

    printf("=== Performance Benchmarks ===\n\n");

    printf("--- Single-thread mempool ---\n");
    bench_single_thread("16 bytes", 16, N);
    bench_single_thread("64 bytes", 64, N);
    bench_single_thread("256 bytes", 256, N);
    bench_single_thread("1024 bytes", 1024, N);
    bench_single_thread("4096 bytes", 4096, N);

    printf("\n--- Single-thread system malloc ---\n");
    bench_system_malloc("16 bytes (system)", 16, N);
    bench_system_malloc("64 bytes (system)", 64, N);
    bench_system_malloc("256 bytes (system)", 256, N);
    bench_system_malloc("1024 bytes (system)", 1024, N);
    bench_system_malloc("4096 bytes (system)", 4096, N);

    printf("\n--- Multi-thread scalability ---\n");
    bench_multi_thread(1, 64, N);
    bench_multi_thread(2, 64, N);
    bench_multi_thread(4, 64, N);
    bench_multi_thread(8, 64, N);

    printf("\n--- Stats ---\n");
    mp_init(nullptr);
    for (int i = 0; i < 10000; i++) {
        void* p = mp_malloc(64);
        mp_free(p);
    }
    mp_stats_print();
    mp_shutdown();

    printf("\nAll benchmarks complete.\n");
    return 0;
}
