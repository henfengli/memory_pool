#include <mempool/mempool.h>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_pass = 0, g_fail = 0;

#define RUN_TEST(fn) do { \
    printf("  %-40s ", #fn); \
    fn(); \
    printf("PASS\n"); \
    g_pass++; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
        return; \
    } \
} while(0)

void test_init_shutdown() {
    mp_config_t cfg = {0, 1, 0};
    ASSERT(mp_init(&cfg) == 0);
    mp_shutdown();
}

void test_malloc_free_small() {
    mp_init(nullptr);
    void* p = mp_malloc(16);
    ASSERT(p != nullptr);
    mp_free(p);
    mp_shutdown();
}

void test_malloc_all_size_classes() {
    mp_init(nullptr);
    size_t sizes[] = {1, 16, 32, 48, 64, 80, 96, 112, 128,
                      160, 192, 224, 256, 320, 384, 448, 512,
                      640, 768, 1024, 1280, 1536, 2048, 2560, 3072, 4096};
    std::vector<void*> ptrs;
    for (size_t sz : sizes) {
        void* p = mp_malloc(sz);
        ASSERT(p != nullptr);
        memset(p, 0xAA, sz);
        ptrs.push_back(p);
    }
    for (void* p : ptrs) mp_free(p);
    mp_shutdown();
}

void test_calloc_zero_init() {
    mp_init(nullptr);
    void* p = mp_calloc(10, 16);
    ASSERT(p != nullptr);
    unsigned char* bytes = (unsigned char*)p;
    for (int i = 0; i < 160; i++) ASSERT(bytes[i] == 0);
    mp_free(p);
    mp_shutdown();
}

void test_calloc_overflow() {
    mp_init(nullptr);
    void* p = mp_calloc(SIZE_MAX, 2);
    ASSERT(p == nullptr);
    mp_shutdown();
}

void test_realloc_grow() {
    mp_init(nullptr);
    void* p = mp_malloc(16);
    ASSERT(p != nullptr);
    memset(p, 0x42, 16);
    void* p2 = mp_realloc(p, 256);
    ASSERT(p2 != nullptr);
    unsigned char* bytes = (unsigned char*)p2;
    for (int i = 0; i < 16; i++) ASSERT(bytes[i] == 0x42);
    mp_free(p2);
    mp_shutdown();
}

void test_realloc_same_class() {
    mp_init(nullptr);
    void* p = mp_malloc(10);
    ASSERT(p != nullptr);
    void* p2 = mp_realloc(p, 15);
    ASSERT(p2 == p);
    mp_free(p2);
    mp_shutdown();
}

void test_realloc_null() {
    mp_init(nullptr);
    void* p = mp_realloc(nullptr, 64);
    ASSERT(p != nullptr);
    mp_free(p);
    mp_shutdown();
}

void test_realloc_zero() {
    mp_init(nullptr);
    void* p = mp_malloc(64);
    ASSERT(p != nullptr);
    void* p2 = mp_realloc(p, 0);
    ASSERT(p2 == nullptr);
    mp_shutdown();
}

void test_free_null() {
    mp_init(nullptr);
    mp_free(nullptr);
    mp_shutdown();
}

void test_malloc_too_large() {
    mp_init(nullptr);
    void* p = mp_malloc(5000);
    ASSERT(p == nullptr);
    mp_shutdown();
}

void test_many_allocs_frees() {
    mp_init(nullptr);
    std::vector<void*> ptrs;
    for (int i = 0; i < 1000; i++) {
        void* p = mp_malloc(64);
        ASSERT(p != nullptr);
        memset(p, (unsigned char)i, 64);
        ptrs.push_back(p);
    }
    for (void* p : ptrs) mp_free(p);
    mp_shutdown();
}

void test_stats_basic() {
    mp_config_t cfg = {0, 1, 0};
    mp_init(&cfg);
    void* p1 = mp_malloc(32);
    void* p2 = mp_malloc(64);
    mp_stats_t stats;
    mp_stats_get(&stats);
    ASSERT(stats.alloc_count >= 2);
    ASSERT(stats.alloc_bytes > 0);
    mp_free(p1);
    mp_free(p2);
    mp_shutdown();
}

int main() {
    printf("=== test_basic ===\n");
    RUN_TEST(test_init_shutdown);
    RUN_TEST(test_malloc_free_small);
    RUN_TEST(test_malloc_all_size_classes);
    RUN_TEST(test_calloc_zero_init);
    RUN_TEST(test_calloc_overflow);
    RUN_TEST(test_realloc_grow);
    RUN_TEST(test_realloc_same_class);
    RUN_TEST(test_realloc_null);
    RUN_TEST(test_realloc_zero);
    RUN_TEST(test_free_null);
    RUN_TEST(test_malloc_too_large);
    RUN_TEST(test_many_allocs_frees);
    RUN_TEST(test_stats_basic);
    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
