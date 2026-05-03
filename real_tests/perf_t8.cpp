#include <mempool/mempool.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s mp|sys\n", argv[0]); return 1; }
    bool use_mp = !std::strcmp(argv[1], "mp");
    if (use_mp) mp_init(nullptr);

    constexpr int K = 256;
    constexpr int T = 8;
    constexpr long ops = 1L << 22;  // 4M ops/thread
    const size_t sz = 64;

    std::atomic<int> go{0}, ready{0};
    std::vector<std::thread> ths;
    for (int t = 0; t < T; t++) {
        ths.emplace_back([&, t] {
            void* ring[K];
            for (int i = 0; i < K; i++) {
                ring[i] = use_mp ? mp_malloc(sz) : malloc(sz);
                *(volatile uint64_t*)ring[i] = i;
            }
            ready.fetch_add(1);
            while (!go.load()) {}
            for (long i = 0; i < ops; i++) {
                int s = i & (K-1);
                if (use_mp) { mp_free(ring[s]); ring[s] = mp_malloc(sz); }
                else        { free(ring[s]);    ring[s] = malloc(sz); }
                *(volatile uint64_t*)ring[s] = i;
            }
            for (int i = 0; i < K; i++) (use_mp ? mp_free(ring[i]) : free(ring[i]));
            if (use_mp) mp_thread_detach();
        });
    }
    while (ready.load() < T) {}
    go.store(1);
    for (auto& th : ths) th.join();
    if (use_mp) mp_shutdown();
    return 0;
}
