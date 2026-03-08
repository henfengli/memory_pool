#include <mempool/mempool.h>
#include <cstdio>
#include <cstring>

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

void test_arena_create_destroy() {
    mp_init(nullptr);
    mp_arena_t* arena = mp_arena_create("test_arena", 0);
    ASSERT(arena != nullptr);
    mp_arena_destroy(arena);
    mp_shutdown();
}

void test_arena_malloc() {
    mp_init(nullptr);
    mp_arena_t* arena = mp_arena_create("test_arena", 0);
    ASSERT(arena != nullptr);
    void* p = mp_arena_malloc(arena, 128);
    ASSERT(p != nullptr);
    memset(p, 0xBB, 128);
    mp_free(p);
    mp_arena_destroy(arena);
    mp_shutdown();
}

void test_arena_multiple() {
    mp_init(nullptr);
    mp_arena_t* a1 = mp_arena_create("arena1", 0);
    mp_arena_t* a2 = mp_arena_create("arena2", 0);
    ASSERT(a1 != nullptr);
    ASSERT(a2 != nullptr);
    ASSERT(a1 != a2);
    void* p1 = mp_arena_malloc(a1, 64);
    void* p2 = mp_arena_malloc(a2, 64);
    ASSERT(p1 != nullptr);
    ASSERT(p2 != nullptr);
    mp_free(p1);
    mp_free(p2);
    mp_arena_destroy(a1);
    mp_arena_destroy(a2);
    mp_shutdown();
}

void test_arena_max_size() {
    mp_init(nullptr);
    mp_arena_t* arena = mp_arena_create("tiny", 1 * 1024 * 1024);
    ASSERT(arena != nullptr);
    void* p = mp_arena_malloc(arena, 64);
    ASSERT(p == nullptr);
    mp_arena_destroy(arena);
    mp_shutdown();
}

void test_arena_many_allocs() {
    mp_init(nullptr);
    mp_arena_t* arena = mp_arena_create("many", 0);
    ASSERT(arena != nullptr);
    void* ptrs[500];
    for (int i = 0; i < 500; i++) {
        ptrs[i] = mp_arena_malloc(arena, 256);
        ASSERT(ptrs[i] != nullptr);
        memset(ptrs[i], (unsigned char)i, 256);
    }
    for (int i = 0; i < 500; i++) mp_free(ptrs[i]);
    mp_arena_destroy(arena);
    mp_shutdown();
}

int main() {
    printf("=== test_arena ===\n");
    RUN_TEST(test_arena_create_destroy);
    RUN_TEST(test_arena_malloc);
    RUN_TEST(test_arena_multiple);
    RUN_TEST(test_arena_max_size);
    RUN_TEST(test_arena_many_allocs);
    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
