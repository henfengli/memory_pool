// real_tests/bench_scenario_pipeline.cpp
// Thesis Table 6-8 production C: 3-stage pipeline.
//
// parse  thread: read input → alloc Record(256B) → push to filter queue
// filter thread: pop Record → write Summary(32B) into pre-allocated slot,
//                push Summary to aggregate queue, free Record (cross-thread,
//                Record was alloc'd by parse)
// aggregate thread: pop Summary → fold into running totals → free Summary
//                   (cross-thread, Summary was alloc'd by filter)
//
// Per record: 1 alloc(Record) + 1 alloc(Summary) + 1 same-thread free of
//             Summary (in aggregate, after consume) + 1 cross-thread free
//             of Record (in filter) + 1 cross-thread free of Summary
//             (in aggregate, after fold).
// Per record: 2 alloc + 2 free (counting cross-thread free as 1 op).
//
// 200,000 records → 400,000 ops (matching thesis §6.5).
// Median of 5 runs.

#include <mempool/mempool.h>
#include "bench_common.h"
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace bench;

struct Record {
    uint8_t payload[256 - sizeof(uint64_t)];
    uint64_t id;
};
static_assert(sizeof(Record) == 256, "Record must be 256B");

struct Summary {
    uint64_t id;
    uint64_t value;
    uint64_t pad[2];
};
static_assert(sizeof(Summary) == 32, "Summary must be 32B");

struct SPSC {
    static constexpr size_t N = 1 << 16;
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};
    void* slots[N];

    bool push(void* p) {
        size_t t = tail.load(std::memory_order_relaxed);
        size_t h = head.load(std::memory_order_acquire);
        if (t - h >= N) return false;
        slots[t & (N - 1)] = p;
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
    void* pop() {
        size_t h = head.load(std::memory_order_relaxed);
        size_t t = tail.load(std::memory_order_acquire);
        if (h == t) return nullptr;
        void* p = slots[h & (N - 1)];
        head.store(h + 1, std::memory_order_release);
        return p;
    }
};

static const long kRecords = 200000;
static const int  kRuns    = 5;

template <typename Alloc, typename Free>
static double run_pipeline(Alloc alloc, Free freefn) {
    SPSC q_parse_to_filter;
    SPSC q_filter_to_aggregate;
    std::atomic<int> go{0};
    std::atomic<int> filter_done{0};
    std::atomic<int> aggregate_done{0};

    auto t0 = Clock::now();

    std::thread t_parse([&] {
        while (!go.load(std::memory_order_acquire)) {}
        for (long i = 0; i < kRecords; i++) {
            auto* r = (Record*)alloc(sizeof(Record));
            r->id = (uint64_t)i;
            r->payload[0] = (uint8_t)i;
            while (!q_parse_to_filter.push(r)) {}
        }
        // Stay alive until cross-thread frees from filter complete, then detach.
        while (filter_done.load(std::memory_order_acquire) == 0) {}
        mp_thread_detach();
    });

    std::thread t_filter([&] {
        while (!go.load(std::memory_order_acquire)) {}
        long got = 0;
        while (got < kRecords) {
            auto* r = (Record*)q_parse_to_filter.pop();
            if (!r) continue;
            auto* s = (Summary*)alloc(sizeof(Summary));
            s->id = r->id;
            s->value = (uint64_t)r->payload[0];
            while (!q_filter_to_aggregate.push(s)) {}
            freefn(r);  // cross-thread: Record was alloc'd by t_parse
            got++;
        }
        filter_done.store(1, std::memory_order_release);
        // Stay alive until aggregate finishes cross-free of our Summaries.
        while (aggregate_done.load(std::memory_order_acquire) == 0) {}
        mp_thread_detach();
    });

    std::thread t_aggregate([&] {
        while (!go.load(std::memory_order_acquire)) {}
        uint64_t total = 0;
        long got = 0;
        while (got < kRecords) {
            auto* s = (Summary*)q_filter_to_aggregate.pop();
            if (!s) continue;
            total += s->value;
            freefn(s);  // cross-thread: Summary was alloc'd by t_filter
            got++;
        }
        touch((void*)(uintptr_t)total);
        aggregate_done.store(1, std::memory_order_release);
        mp_thread_detach();
    });

    go.store(1, std::memory_order_release);
    t_parse.join();
    t_filter.join();
    t_aggregate.join();
    auto t1 = Clock::now();
    return elapsed_ms(t0, t1);
}

template <typename F>
static double median_of(int n, F&& body) {
    std::vector<double> v;
    v.reserve(n);
    for (int i = 0; i < n; i++) v.push_back(body());
    std::sort(v.begin(), v.end());
    return v[n / 2];
}

int main() {
    print_env_banner("bench_scenario_pipeline (Table 6-8 data_pipeline)");

    auto mp_a = [](size_t s) { return mp_malloc(s); };
    auto mp_f = [](void* p)  { mp_free(p); };
    auto sy_a = [](size_t s) { return ::malloc(s); };
    auto sy_f = [](void* p)  { ::free(p); };

    long ops = kRecords * 4;  // 2 alloc + 2 free per record

    printf("\n[Production C] data_pipeline (3 stages, %ld records, %ld ops, median of %d)\n",
           kRecords, ops, kRuns);
    printf("  %-10s | %10s %12s | %8s\n", "allocator", "ms", "ops/ms", "speedup");
    printf("  -----------+-----------------------+---------\n");

    mp_init(nullptr);
    double mp_ms = median_of(kRuns, [&] { return run_pipeline(mp_a, mp_f); });
    mp_shutdown();
    double sy_ms = median_of(kRuns, [&] { return run_pipeline(sy_a, sy_f); });

    printf("  %-10s | %8.2f   %10.0f   |\n", "mempool", mp_ms, ops / mp_ms);
    printf("  %-10s | %8.2f   %10.0f   | x%6.2f\n",
           "system",  sy_ms, ops / sy_ms, sy_ms / mp_ms);
    return 0;
}
