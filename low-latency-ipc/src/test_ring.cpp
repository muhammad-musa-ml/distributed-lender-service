// test_ring.cpp -- correctness tests for SpscRing. Run BEFORE any benchmark: a ring that
// is fast and wrong is worthless, and the failure mode of a mis-ordered acquire/release is
// silent corruption rather than a crash.
//
// The concurrent test is the load-bearing one. It pushes a strictly increasing sequence
// and asserts the consumer observes EXACTLY that sequence, which simultaneously catches:
//   * lost items      (a gap in the sequence)
//   * duplicated items (a repeat)
//   * reordered items  (out-of-order value)
//   * torn publishes   (a slot read before its write was visible -> wrong value)
// A pure count check would catch only the first two, and it is the check most people write.

#include "spsc_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    } else {
        std::printf("  ok    %s\n", what);
    }
}

void test_single_threaded_boundaries() {
    std::printf("[single-threaded boundaries]\n");
    lowlat::SpscRing<int, 8> r;  // 8 slots -> 7 usable

    check(r.capacity() == 7, "capacity() reports Capacity-1 (one slot reserved)");
    check(r.empty_approx(), "starts empty");

    int out = -1;
    check(!r.pop(out), "pop on empty returns false");

    for (int i = 0; i < 7; ++i) {
        if (!r.push(i)) {
            check(false, "push failed before reaching capacity");
            return;
        }
    }
    check(r.size_approx() == 7, "size_approx == 7 when full");
    check(!r.push(999), "push on full returns false (does NOT overwrite)");

    for (int i = 0; i < 7; ++i) {
        if (!r.pop(out) || out != i) {
            check(false, "FIFO order violated while draining");
            return;
        }
    }
    check(true, "drained 7 items in FIFO order");
    check(r.empty_approx(), "empty again after full drain");
    check(!r.pop(out), "pop on drained ring returns false");
}

void test_wraparound() {
    std::printf("[wraparound]\n");
    lowlat::SpscRing<int, 4> r;  // 3 usable -- forces many wraps
    int out = -1;
    bool ok = true;
    for (int i = 0; i < 10000; ++i) {
        if (!r.push(i)) { ok = false; break; }
        if (!r.pop(out) || out != i) { ok = false; break; }
    }
    check(ok, "10000 push/pop cycles through a 4-slot ring preserve value and order");
}

void test_move_only_payload() {
    std::printf("[move-only payload]\n");
    // A realistic payload: the server enqueues owning buffers, not ints.
    lowlat::SpscRing<std::vector<std::uint8_t>, 4> r;
    std::vector<std::uint8_t> in{1, 2, 3, 4, 5};
    const auto* addr = in.data();
    check(r.push(std::move(in)), "push accepts a move-only-ish payload");
    std::vector<std::uint8_t> out;
    check(r.pop(out), "pop returns it");
    check(out.size() == 5 && out[4] == 5, "payload contents survive the round trip");
    check(out.data() == addr, "buffer was MOVED, not copied (same heap allocation)");
}

// The real test. Two threads, a strictly increasing sequence, exact-match assertion.
void test_concurrent_sequence(std::uint64_t n) {
    std::printf("[concurrent producer/consumer, n=%llu]\n",
                static_cast<unsigned long long>(n));
    lowlat::SpscRing<std::uint64_t, 1024> r;

    std::atomic<bool> producer_done{false};
    std::atomic<std::uint64_t> mismatches{0};
    std::atomic<std::uint64_t> consumed{0};

    std::thread consumer([&] {
        std::uint64_t expect = 0;
        std::uint64_t v = 0;
        while (true) {
            if (r.pop(v)) {
                if (v != expect) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
                ++expect;
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else if (producer_done.load(std::memory_order_acquire) && r.empty_approx()) {
                break;
            }
        }
    });

    for (std::uint64_t i = 0; i < n; ++i) {
        while (!r.push(i)) {
            // Ring full: spin. A real server would apply backpressure here; for the test
            // the spin keeps the producer hot so wraparound is exercised continuously.
        }
    }
    producer_done.store(true, std::memory_order_release);
    consumer.join();

    check(consumed.load() == n, "consumer saw EXACTLY n items (no loss, no duplication)");
    check(mismatches.load() == 0, "every item arrived in strict producer order");
}

}  // namespace

int main(int argc, char** argv) {
    // n is settable because ThreadSanitizer runs 20-50x slower than -O2. The sanitizer
    // build uses a smaller n: TSan detects a mis-ordered acquire/release from the
    // happens-before graph, not by brute force, so a few hundred thousand items through
    // a 1024-slot ring already exercises every wraparound edge many times over. The
    // large n in the plain build is there to shake out timing-dependent loss instead.
    std::uint64_t n = 5'000'000;
    if (argc > 1) {
        n = std::strtoull(argv[1], nullptr, 10);
    }

    std::printf("SpscRing correctness\n");
    std::printf("cache line assumed: %zu bytes\n\n", lowlat::kCacheLine);

    test_single_threaded_boundaries();
    std::printf("\n");
    test_wraparound();
    std::printf("\n");
    test_move_only_payload();
    std::printf("\n");
    test_concurrent_sequence(n);

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
