// real_tests/perf_focused.cpp
// Minimal program for perf record. Runs ONLY one allocator's 16B K=256 churn
// for many iters so perf samples concentrate on the alloc/free hot path.
//
// Usage:
//   perf_focused mp     # mempool only
//   perf_focused sys    # glibc only

#include <mempool/mempool.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s mp|sys\n", argv[0]); return 1; }
    bool use_mp = std::strcmp(argv[1], "mp") == 0;

    if (use_mp) mp_init(nullptr);

    const int K = 256;
    const long iters = 1L << 25;  // 33M iters - plenty of samples
    const size_t sz = 16;

    std::vector<void*> ring(K);
    for (int i = 0; i < K; i++) {
        ring[i] = use_mp ? mp_malloc(sz) : malloc(sz);
        *static_cast<volatile uint64_t*>(ring[i]) = (uint64_t)i;
    }
    for (long i = 0; i < iters; i++) {
        int slot = (int)(i & (K - 1));
        if (use_mp) {
            mp_free(ring[slot]);
            ring[slot] = mp_malloc(sz);
        } else {
            free(ring[slot]);
            ring[slot] = malloc(sz);
        }
        *static_cast<volatile uint64_t*>(ring[slot]) = (uint64_t)i;
    }
    for (int i = 0; i < K; i++) {
        if (use_mp) mp_free(ring[i]); else free(ring[i]);
    }

    if (use_mp) mp_shutdown();
    printf("done %s, iters=%ld\n", argv[1], iters);
    return 0;
}
