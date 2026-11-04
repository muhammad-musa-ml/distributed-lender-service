// spsc_ring.hpp -- bounded lock-free single-producer / single-consumer ring buffer.
//
// WHY THIS EXISTS
// ---------------
// The ring is a building block for handing buffers from an I/O thread (producer) to a
// worker thread (consumer) without a mutex. The server in this folder runs one event loop
// and does not use it; only bench_ring.cpp and test_ring.cpp do. A mutex in such a handoff
// is not slow because locking costs much in the uncontended case -- it is slow because a
// CONTENDED futex parks the thread, and a parked thread costs microseconds, not
// nanoseconds. That would show up in p99.9 and nowhere else, which is exactly the
// percentile this project exists to talk about.
//
// THE CORRECTNESS ARGUMENT, stated because "lock-free" is easy to claim and hard to justify:
//
//   * Exactly ONE thread calls push() and exactly ONE calls pop(). With two indices this
//     is the classic Lamport ring and needs no CAS at all -- each index has a single
//     writer, so a plain store suffices. CAS is required only for multi-producer, and
//     paying for it here would be cargo-culting.
//
//   * head is written only by the producer, tail only by the consumer. Each side loads
//     the OTHER index with memory_order_acquire and stores its OWN with
//     memory_order_release. The release store on head publishes the slot write that
//     precedes it; the matching acquire load in pop() is what makes that slot write
//     visible. Reversing either one is the standard way this structure silently corrupts
//     -- and on x86 the hardware often hides the bug the compiler already introduced by
//     reordering, so it survives testing and fails on arm64.
//
//   * Capacity is a power of two so the modulo is a mask. Not micro-optimisation theatre:
//     an integer division in the hot path is ~20-40 cycles and would dominate the enqueue.
//
//   * The ring holds Capacity-1 usable slots. One slot is deliberately wasted so "empty"
//     (head == tail) and "full" (next(head) == tail) are distinguishable without a count
//     -- a count would be a THIRD shared variable written by BOTH sides, which is exactly
//     what we are avoiding.
//
//   * head and tail sit on separate cache lines. Without that padding they share a line
//     and every producer store invalidates the consumer's copy -- false sharing. This is
//     worth more on this workload than every other choice in this file combined, which is
//     why bench_ring.cpp MEASURES it (PadIndices=false) instead of asserting it.
//
// Single header. Depends only on <atomic>, <cstddef>, <type_traits>, <utility>.

#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace lowlat {

// A FIXED constant, deliberately, not std::hardware_destructive_interference_size.
// GCC 13 warns on that spelling in a header (-Winterference-size): its value varies with
// -mtune/-mcpu, so baking it into a type's layout makes the ABI depend on flags, and two
// TUs compiled differently would disagree about where `tail` lives. The compiler's own
// advice is "use a constant variable you define" -- this is that constant. 64 is correct
// for every x86-64; on Apple silicon's 128-byte pairs it is merely conservative (padding
// to one line instead of two), which costs a little space and breaks nothing.
inline constexpr std::size_t kCacheLine = 64;

namespace detail {

// Two index layouts, selected at compile time. The PadIndices=false form exists ONLY so
// the benchmark can measure the cost of false sharing rather than claim it.
template <bool Pad>
struct Indices;

template <>
struct Indices<true> {
    alignas(kCacheLine) std::atomic<std::size_t> head{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail{0};
};

template <>
struct Indices<false> {
    // Deliberately adjacent: head and tail land in the same 64-byte line.
    std::atomic<std::size_t> head{0};
    std::atomic<std::size_t> tail{0};
};

}  // namespace detail

template <typename T, std::size_t Capacity, bool PadIndices = true>
class SpscRing {
    static_assert(Capacity >= 2, "capacity must be at least 2 (one slot is reserved)");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

  public:
    using value_type = T;

    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // PRODUCER THREAD ONLY. False when full. Never blocks, never allocates.
    bool push(T value) noexcept {
        const std::size_t head = idx_.head.load(std::memory_order_relaxed);  // our own
        const std::size_t next = (head + 1) & kMask;
        if (next == idx_.tail.load(std::memory_order_acquire)) {
            return false;  // full
        }
        slots_[head] = std::move(value);
        idx_.head.store(next, std::memory_order_release);  // publishes the slot write
        return true;
    }

    // CONSUMER THREAD ONLY. False when empty. Never blocks.
    bool pop(T& out) noexcept {
        const std::size_t tail = idx_.tail.load(std::memory_order_relaxed);  // our own
        if (tail == idx_.head.load(std::memory_order_acquire)) {
            return false;  // empty
        }
        out = std::move(slots_[tail]);
        idx_.tail.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    // APPROXIMATE when called concurrently -- the other side's index can move between the
    // two loads. Exact only on a thread that owns both. Named so no caller mistakes them
    // for synchronisation.
    std::size_t size_approx() const noexcept {
        const std::size_t h = idx_.head.load(std::memory_order_acquire);
        const std::size_t t = idx_.tail.load(std::memory_order_acquire);
        return (h - t) & kMask;
    }
    bool empty_approx() const noexcept { return size_approx() == 0; }

    static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

  private:
    static constexpr std::size_t kMask = Capacity - 1;

    detail::Indices<PadIndices> idx_{};
    T slots_[Capacity]{};
};

}  // namespace lowlat
