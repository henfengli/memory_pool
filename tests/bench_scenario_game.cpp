// real_tests/bench_scenario_game.cpp
// Thesis §6.5 production scenario A: game particle emitter.
// 256 spawns/frame, ~32 frame avg lifetime, 2000 frames.
// Steady-state live ≈ 8000 particles of 96B each.
// Single-thread + 4-thread variants. Speedup column for direct compare.

#include <mempool/mempool.h>
#include "bench_common.h"
#include <atomic>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace bench;

struct Particle {
    float pos[3];
    float vel[3];
    float color[4];
    uint32_t lifetime_left;
    uint32_t flags;
    uint8_t  pad[48];
};
static_assert(sizeof(Particle) == 96, "Particle should be 96B");

static const int kFrames    = 2000;
static const int kSpawn     = 256;
static const int kAvgLife   = 32;

template <typename Alloc, typename Free>
double run_emitter(uint32_t seed, Alloc alloc, Free freefn) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> life_dist(kAvgLife - 8, kAvgLife + 8);
    std::vector<Particle*> live;
    live.reserve(kSpawn * (kAvgLife + 16));

    auto t0 = Clock::now();
    for (int f = 0; f < kFrames; f++) {
        for (int i = 0; i < kSpawn; i++) {
            auto* p = (Particle*)alloc(sizeof(Particle));
            p->lifetime_left = (uint32_t)life_dist(rng);
            p->pos[0] = (float)f; p->pos[1] = 0; p->pos[2] = 0;
            p->vel[0] = 1; p->vel[1] = 1; p->vel[2] = 0;
            live.push_back(p);
        }
        size_t w = 0;
        for (size_t r = 0; r < live.size(); r++) {
            Particle* p = live[r];
            p->pos[0] += p->vel[0];
            p->pos[1] += p->vel[1];
            p->pos[2] += p->vel[2];
            if (--p->lifetime_left == 0) freefn(p);
            else                          live[w++] = p;
        }
        live.resize(w);
        touch(live.empty() ? nullptr : live[0]);
    }
    for (auto* p : live) freefn(p);
    auto t1 = Clock::now();
    return elapsed_ms(t0, t1);
}

template <typename Alloc, typename Free>
double run_emitter_mt(int nth, Alloc alloc, Free freefn) {
    std::atomic<int> go{0};
    std::vector<std::thread> ths;
    ths.reserve(nth);
    std::vector<double> per(nth, 0);
    for (int t = 0; t < nth; t++) {
        ths.emplace_back([&, t] {
            pin_to_cpu_if_room(t, nth);
            while (!go.load(std::memory_order_acquire)) {}
            per[t] = run_emitter(0xBEEFu + (uint32_t)t, alloc, freefn);
            mp_thread_detach();
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto t0 = Clock::now();
    go.store(1, std::memory_order_release);
    for (auto& th : ths) th.join();
    auto t1 = Clock::now();
    return elapsed_ms(t0, t1);
}

static double ops_per_ms(double ms, long ops) { return ms > 0 ? ops / ms : 0.0; }

int main() {
    print_env_banner("bench_scenario_game (96B particles, 2000 frames)");

    auto mp_a = [](size_t s) { return mp_malloc(s); };
    auto mp_f = [](void* p)  { mp_free(p); };
    auto sy_a = [](size_t s) { return ::malloc(s); };
    auto sy_f = [](void* p)  { ::free(p); };

    constexpr int kRuns = 5;
    long ops_st = (long)kFrames * kSpawn * 2;

    // ---- single-threaded ----
    printf("\n[Production A] game_particles single-thread (%ld ops, median of %d)\n", ops_st, kRuns);
    printf("  %-10s | %12s %12s | %8s\n", "allocator", "ms", "ops/ms", "speedup");
    printf("  -----------+--------------------------+---------\n");

    mp_init(nullptr);
    double mp_ms = median_ms(kRuns, [&] { run_emitter(0xBEEF, mp_a, mp_f); });
    mp_shutdown();
    double sy_ms = median_ms(kRuns, [&] { run_emitter(0xBEEF, sy_a, sy_f); });
    printf("  %-10s | %10.2f   %10.0f   |\n", "mempool", mp_ms, ops_per_ms(mp_ms, ops_st));
    printf("  %-10s | %10.2f   %10.0f   | x%6.2f\n",
           "system",  sy_ms, ops_per_ms(sy_ms, ops_st), sy_ms / mp_ms);

    // ---- 4-thread ----
    long ops_mt = (long)4 * kFrames * kSpawn * 2;
    printf("\n[Production A] game_particles 4-thread (%ld ops, median of %d)\n", ops_mt, kRuns);
    printf("  %-10s | %12s %12s | %8s\n", "allocator", "ms", "ops/ms", "speedup");
    printf("  -----------+--------------------------+---------\n");

    mp_init(nullptr);
    double mp_ms2 = median_ms(kRuns, [&] { run_emitter_mt(4, mp_a, mp_f); });
    mp_shutdown();
    double sy_ms2 = median_ms(kRuns, [&] { run_emitter_mt(4, sy_a, sy_f); });
    printf("  %-10s | %10.2f   %10.0f   |\n", "mempool", mp_ms2, ops_per_ms(mp_ms2, ops_mt));
    printf("  %-10s | %10.2f   %10.0f   | x%6.2f\n",
           "system",  sy_ms2, ops_per_ms(sy_ms2, ops_mt), sy_ms2 / mp_ms2);

    return 0;
}
