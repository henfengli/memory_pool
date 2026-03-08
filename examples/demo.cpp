#include <mempool/mempool.h>
#include <cstdio>
#include <cstring>

int main() {
    printf("=== mempool demo ===\n\n");

    // 1. Initialize
    mp_config_t cfg = {0, 1, 0}; // unlimited size, enable stats
    mp_init(&cfg);
    printf("[1] Initialized mempool\n");

    // 2. Basic allocation
    void* p1 = mp_malloc(64);
    printf("[2] Allocated 64 bytes at %p\n", p1);
    memset(p1, 'A', 64);

    // 3. Calloc (zero-initialized)
    int* arr = (int*)mp_calloc(10, sizeof(int));
    printf("[3] Calloc 10 ints at %p (first value: %d)\n", arr, arr[0]);

    // 4. Realloc
    void* p2 = mp_malloc(32);
    memset(p2, 'B', 32);
    void* p3 = mp_realloc(p2, 256);
    printf("[4] Realloc 32->256 bytes: %p -> %p\n", p2, p3);

    // 5. Arena-based allocation
    mp_arena_t* arena = mp_arena_create("game_objects", 0);
    void* obj1 = mp_arena_malloc(arena, 128);
    void* obj2 = mp_arena_malloc(arena, 256);
    printf("[5] Arena allocs: %p, %p\n", obj1, obj2);

    // 6. Free everything
    mp_free(p1);
    mp_free(arr);
    mp_free(p3);
    mp_free(obj1);
    mp_free(obj2);
    printf("[6] Freed all allocations\n");

    // 7. Statistics
    printf("\n[7] Statistics:\n");
    mp_stats_print();

    // 8. Cleanup
    mp_arena_destroy(arena);
    mp_shutdown();
    printf("\n[8] Shutdown complete\n");

    return 0;
}
