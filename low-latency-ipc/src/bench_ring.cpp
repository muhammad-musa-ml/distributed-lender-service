// bench_ring.cpp -- measure the cost of FALSE SHARING on the ring's two indices.
//
// This is the project's worked "before/after of one optimisation", and it is deliberately
// the smallest possible change: the ONLY difference between the two runs is whether
// SpscRing's head and tail indices are alignas(64) onto separate cache lines. Same code
// path, same instructions, same compiler flags -- only the byte offsets of two members.
//
// WHY IT COSTS ANYTHING. head is written only by the producer and tail only by the
// consumer, so there is no logical sharing at all. But cache coherence works on LINES,
// not variables. With both indices in one 64-byte line, every producer store to head
// invalidates the consumer's copy of that line, so the consumer's next load of tail --
// a variable the producer never touched -- misses and must re-fetch from the coherence
// fabric. Two threads that share no data still ping-pong a line between cores. The fix
// is 56 bytes of padding and no change to the algorithm.
//
// WHAT IS REPORTED. Wall time and ns/item for a fixed number of items pushed through a
// fixed-capacity ring by two pinned-apart threads, for both layouts. The ratio is the
// finding. Absolute numbers depend on the host and are not the point; the RATIO between
// two runs that differ only in padding is.
//
// HONESTY NOTE, because a benchmark that flatters itself is worse than none: this is a
// throughput microbenchmark of an otherwise-empty ring. It deliberately maximises
// contention -- both threads do nothing but push and pop -- so it is an UPPER bound on
// the effect, not the effect a real server would see with work between operations. The
// server benchmark (bench_client) measures the end-to-end number that actually matters.

// THREAD PINNING, and why it is not optional here.
//
// The first version of this benchmark did not pin. Replicated five times at identical
// parameters it reported 3.26x, 2.24x, 3.12x, 1.69x and 2.97x -- the PADDED cost was
// stable (10.5-13.1 ns/item) while the UNPADDED cost swung between 22 and 37 ns/item.
// That is not noise in the measurement, it is a real difference in what was measured:
// with no affinity the scheduler decides whether the producer and consumer land on two
// SIBLING HYPERTHREADS (sharing L1/L2, so a shared line costs almost nothing) or on
// separate physical cores (where it costs a cross-core coherence round trip). Unpinned,
// the benchmark reports a blend of those two experiments in unknown proportion.
//
// Pinning makes the question well-posed: --cpu-a and --cpu-b select the placement, so the
// number is reproducible and the reader knows which arrangement produced it.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <sched.h>

#include "spsc_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kRingCapacity = 1024;

int g_cpu_a = 0;  // producer core
int g_cpu_b = 1;  // consumer core

// Returns false if the pin failed, so the report can say so rather than silently
// reverting to the unpinned experiment the pinning exists to eliminate.
bool pin_to(int cpu) {
    if (cpu < 0) return true;  // pinning explicitly disabled
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu), &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

std::atomic<int> g_pin_failures{0};

// One run: push `n` sequential values through a ring of the given layout, verifying the
// sequence as it goes (a benchmark that does not check its own output can be measuring
// a ring that dropped everything).
template <bool Pad>
double run_once(std::uint64_t n, std::uint64_t& checksum_out, std::uint64_t& mismatches_out) {
    lowlat::SpscRing<std::uint64_t, kRingCapacity, Pad> ring;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> checksum{0};
    std::atomic<std::uint64_t> mismatches{0};

    const auto t0 = std::chrono::steady_clock::now();

    std::thread consumer([&] {
        if (!pin_to(g_cpu_b)) g_pin_failures.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t expect = 0;
        std::uint64_t local_sum = 0;
        std::uint64_t local_bad = 0;
        std::uint64_t v = 0;
        while (true) {
            if (ring.pop(v)) {
                if (v != expect) ++local_bad;
                ++expect;
                local_sum += v;
            } else if (done.load(std::memory_order_acquire) && ring.empty_approx()) {
                break;
            }
        }
        checksum.store(local_sum, std::memory_order_relaxed);
        mismatches.store(local_bad, std::memory_order_relaxed);
    });

    if (!pin_to(g_cpu_a)) g_pin_failures.fetch_add(1, std::memory_order_relaxed);
    for (std::uint64_t i = 0; i < n; ++i) {
        while (!ring.push(i)) {
            // spin: keeps the line hot, which is the effect under measurement
        }
    }
    done.store(true, std::memory_order_release);
    consumer.join();

    const auto t1 = std::chrono::steady_clock::now();
    checksum_out = checksum.load();
    mismatches_out = mismatches.load();
    return std::chrono::duration<double>(t1 - t0).count();
}

struct Result {
    double best_seconds;
    double ns_per_item;
};

template <bool Pad>
Result best_of(std::uint64_t n, int reps) {
    Result best{1e30, 0.0};
    const std::uint64_t expected_sum = (n >= 1) ? (n - 1) * n / 2 : 0;
    for (int i = 0; i < reps; ++i) {
        std::uint64_t sum = 0, bad = 0;
        const double s = run_once<Pad>(n, sum, bad);
        if (bad != 0 || sum != expected_sum) {
            std::printf("  !! run %d produced WRONG DATA (mismatches=%llu, checksum %llu != %llu)"
                        " -- timing discarded\n",
                        i, static_cast<unsigned long long>(bad),
                        static_cast<unsigned long long>(sum),
                        static_cast<unsigned long long>(expected_sum));
            continue;
        }
        if (s < best.best_seconds) best.best_seconds = s;
    }
    best.ns_per_item = best.best_seconds * 1e9 / static_cast<double>(n);
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    // BEST-OF-N, not mean: on a loaded desktop the mean measures the scheduler as much as
    // the code, and the minimum is the closest available estimate of the uncontended cost.
    // Reported alongside the rep count so the reader can judge that choice.
    std::uint64_t n = 20'000'000;
    int reps = 5;
    if (argc > 1) n = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) reps = std::atoi(argv[2]);
    if (argc > 3) g_cpu_a = std::atoi(argv[3]);   // -1 disables pinning
    if (argc > 4) g_cpu_b = std::atoi(argv[4]);

    std::printf("SpscRing false-sharing benchmark\n");
    std::printf("items/run = %llu   reps = %d   ring capacity = %zu   cache line = %zu\n",
                static_cast<unsigned long long>(n), reps, kRingCapacity, lowlat::kCacheLine);
    std::printf("hardware_concurrency = %u   pinning: producer=cpu%d consumer=cpu%d%s\n\n",
                std::thread::hardware_concurrency(), g_cpu_a, g_cpu_b,
                (g_cpu_a < 0 || g_cpu_b < 0) ? "  (DISABLED -- results will vary run to run)" : "");

    std::printf("running UNPADDED (head and tail share a cache line)...\n");
    const Result unpadded = best_of<false>(n, reps);
    std::printf("  best: %.4f s   %.2f ns/item\n\n", unpadded.best_seconds, unpadded.ns_per_item);

    std::printf("running PADDED (head and tail on separate cache lines)...\n");
    const Result padded = best_of<true>(n, reps);
    std::printf("  best: %.4f s   %.2f ns/item\n\n", padded.best_seconds, padded.ns_per_item);

    const double speedup = unpadded.best_seconds / padded.best_seconds;
    std::printf("RESULT  padding is %.2fx %s  (%.2f -> %.2f ns/item)\n",
                speedup >= 1.0 ? speedup : 1.0 / speedup,
                speedup >= 1.0 ? "FASTER" : "SLOWER",
                unpadded.ns_per_item, padded.ns_per_item);
    std::printf("\nsizeof(unpadded ring) = %zu B   sizeof(padded ring) = %zu B   (cost of the fix)\n",
                sizeof(lowlat::SpscRing<std::uint64_t, kRingCapacity, false>),
                sizeof(lowlat::SpscRing<std::uint64_t, kRingCapacity, true>));
    const int pf = g_pin_failures.load();
    if (pf != 0) {
        std::printf("\n** WARNING: %d thread pin(s) FAILED -- this run measured whatever placement\n"
                    "** the scheduler chose, which is the unpinned experiment. Do not quote it.\n", pf);
        return 1;
    }
    return 0;
}
