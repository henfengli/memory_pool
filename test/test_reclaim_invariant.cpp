// Reproducer for the freed_count vs reclaim invariant bug
// (discovered after v1.19.0, fixed in v1.20.0)
//
// Bug: freed_count is monotonically incremented and never decremented when
// alloc pops a block from free_head. Under sustained churn on the same size
// class, freed_count grows past blocks_per_page → reclaim wrongly judges the
// page as "fully freed" and returns it to arena while live blocks still
// reference it. Subsequent free of those live blocks corrupts memory.
//
// This reproducer pins a large set of live blocks while churning a small
// hot subset, forcing freed_count to climb without the live set ever being
// released. Should crash (or produce corrupted state) on v1.19.0;
// should pass on v1.20.0 with the freed_count-- fix.

#include "mempool/mempool.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// 96B size class: 4096 / 96 = 42 blocks per page.
// Choose a size that is NOT a power of two so it falls into a distinct
// size class with relatively small blocks_per_page (faster trigger).
static constexpr size_t kBlockSize = 96;

// LIVE_BLOCKS large enough to span many pages so reclaim has work to do.
// At 42 blocks/page, 8000 live blocks ≈ 191 pages.
static constexpr size_t kLiveBlocks = 8000;

// CHURN_ITERS large enough that:
//   - freed_count on hot pages climbs past 42 (blocks_per_page)
//   - collect_local triggers >= 64 times (reclaim threshold)
// Each free → freed_count++. Each alloc → (with bug) freed_count unchanged.
// So 1 churn iter = 1 free that bumps freed_count by 1 net.
static constexpr size_t kChurnIters = 200000;

void fill_pattern(void* p, size_t n, uint8_t tag) {
    std::memset(p, tag, n);
}

bool verify_pattern(const void* p, size_t n, uint8_t tag) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        if (b[i] != tag) return false;
    }
    return true;
}

int run() {
    mp_init(nullptr);

    // Step 1: allocate a large set of LIVE blocks and fill each with a
    // unique pattern derived from its index. We will keep these alive for
    // the entire test.
    std::vector<void*> live(kLiveBlocks, nullptr);
    for (size_t i = 0; i < kLiveBlocks; ++i) {
        live[i] = mp_malloc(kBlockSize);
        if (!live[i]) {
            std::fprintf(stderr, "[FAIL] live alloc %zu returned null\n", i);
            return 1;
        }
        // Pattern: low byte of index, repeated. Allows detection of
        // overwrite by an unrelated allocation.
        fill_pattern(live[i], kBlockSize, static_cast<uint8_t>(i & 0xFF));
    }

    std::printf("[STEP 1] allocated %zu live blocks of %zu bytes each\n",
                kLiveBlocks, kBlockSize);

    // Step 2: churn — repeatedly alloc-then-free a single hot block.
    // Under the bug, every free increments freed_count of whatever page
    // the hot block lands on, while alloc does NOT decrement it.
    // freed_count climbs unboundedly.
    for (size_t i = 0; i < kChurnIters; ++i) {
        void* p = mp_malloc(kBlockSize);
        if (!p) {
            std::fprintf(stderr, "[FAIL] churn alloc %zu returned null\n", i);
            return 2;
        }
        // Touch the block lightly — write a sentinel.
        *static_cast<uint32_t*>(p) = 0xDEADBEEF;
        mp_free(p);
    }

    std::printf("[STEP 2] churned %zu alloc-free cycles\n", kChurnIters);

    // Step 3: verify all live blocks still hold their original pattern.
    // If reclaim wrongly returned a live page to arena and arena handed it
    // out to the churn loop, the churn writes (0xDEADBEEF) will have
    // overwritten the live data → pattern mismatch.
    size_t corrupted = 0;
    for (size_t i = 0; i < kLiveBlocks; ++i) {
        if (!verify_pattern(live[i], kBlockSize, static_cast<uint8_t>(i & 0xFF))) {
            ++corrupted;
            if (corrupted <= 5) {
                std::fprintf(stderr,
                             "[CORRUPT] live[%zu]=%p first byte=%02x expected=%02x\n",
                             i, live[i],
                             static_cast<unsigned>(static_cast<uint8_t*>(live[i])[0]),
                             static_cast<unsigned>(i & 0xFF));
            }
        }
    }

    if (corrupted) {
        std::fprintf(stderr, "[FAIL] %zu / %zu live blocks were corrupted\n",
                     corrupted, kLiveBlocks);
        return 3;
    }

    std::printf("[STEP 3] all %zu live blocks verified intact\n", kLiveBlocks);

    // Step 4: free everything. If the bug already corrupted free-list links
    // via the cross-bucket page reuse path, this is where we typically
    // segfault. So if we reach here without crashing AND all patterns are
    // intact, the system is clean.
    for (size_t i = 0; i < kLiveBlocks; ++i) {
        mp_free(live[i]);
    }
    std::printf("[STEP 4] freed all live blocks without crash\n");

    mp_shutdown();
    std::printf("[PASS] reclaim invariant holds under sustained churn\n");
    return 0;
}

} // namespace

int main() {
    return run();
}
