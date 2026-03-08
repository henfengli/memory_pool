#include <mempool/mempool.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>

static int g_pass = 0, g_fail = 0;
static std::atomic<int> g_thread_errors{0};

#define RUN_TEST(fn) do { \
    printf("  %-40s ", #fn); fflush(stdout); \
    g_thread_errors.store(0); \
    fn(); \
    if (g_thread_errors.load() == 0) { \
        printf("PASS\n"); g_pass++; \
    } else { \
        printf("FAIL\n"); g_fail++; \
    } \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; return; \
    } \
} while(0)

#define THREAD_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "THREAD FAIL at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_thread_errors.fetch_add(1); return; \
    } \
} while(0)

void test_two_threads_simple() {
    mp_init(nullptr);
    std::thread t1([]() {
        void* p = mp_malloc(64);
        THREAD_ASSERT(p != nullptr);
        mp_free(p);
        mp_thread_detach();
    });
    t1.join();
    std::thread t2([]() {
        void* p = mp_malloc(64);
        THREAD_ASSERT(p != nullptr);
        mp_free(p);
        mp_thread_detach();
    });
    t2.join();
    mp_shutdown();
}

void test_concurrent_alloc_free() {
    mp_init(nullptr);
    const int NT = 4, N = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < NT; t++) {
        threads.emplace_back([N]() {
            std::vector<void*> ptrs;
            for (int i = 0; i < N; i++) {
                void* p = mp_malloc(64);
                THREAD_ASSERT(p != nullptr);
                memset(p, 0xAA, 64);
                ptrs.push_back(p);
            }
            for (void* p : ptrs) mp_free(p);
            mp_thread_detach();
        });
    }
    for (auto& t : threads) t.join();
    mp_shutdown();
}

void test_concurrent_mixed_sizes() {
    mp_init(nullptr);
    const int NT = 4;
    size_t sizes[] = {16, 64, 128, 256, 512, 1024, 2048, 4096};
    std::vector<std::thread> threads;
    for (int t = 0; t < NT; t++) {
        threads.emplace_back([&sizes, t]() {
            std::vector<void*> ptrs;
            for (int i = 0; i < 100; i++) {
                size_t sz = sizes[(t * 31 + i) % 8];
                void* p = mp_malloc(sz);
                THREAD_ASSERT(p != nullptr);
                memset(p, 0xBB, sz);
                ptrs.push_back(p);
            }
            for (void* p : ptrs) mp_free(p);
            mp_thread_detach();
        });
    }
    for (auto& t : threads) t.join();
    mp_shutdown();
}

void test_cross_thread_free() {
    mp_init(nullptr);
    const int N = 50;
    std::vector<void*> ptrs(N);

    std::thread t1([&]() {
        for (int i = 0; i < N; i++) {
            ptrs[i] = mp_malloc(128);
            THREAD_ASSERT(ptrs[i] != nullptr);
            memset(ptrs[i], 0xCC, 128);
        }
        // Don't detach: leave TLC alive for cross-thread free
    });
    t1.join();

    std::thread t2([&]() {
        for (int i = 0; i < N; i++) mp_free(ptrs[i]);
        mp_thread_detach();
    });
    t2.join();

    mp_shutdown();
}

void test_thread_exit_cleanup() {
    mp_init(nullptr);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([]() {
            for (int i = 0; i < 50; i++) {
                void* p = mp_malloc(32);
                THREAD_ASSERT(p != nullptr);
                mp_free(p);
            }
            // No mp_thread_detach: rely on DllMain thread exit callback
        });
    }
    for (auto& t : threads) t.join();
    mp_shutdown();
}

int main() {
    printf("=== test_thread ===\n");
    RUN_TEST(test_two_threads_simple);
    RUN_TEST(test_concurrent_alloc_free);
    RUN_TEST(test_concurrent_mixed_sizes);
    RUN_TEST(test_cross_thread_free);
    RUN_TEST(test_thread_exit_cleanup);
    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
