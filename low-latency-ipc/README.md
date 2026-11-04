# low-latency-ipc

A lock-free SPSC ring buffer and an edge-triggered `epoll` TCP server, in C++20.

I didn't build this because the world needs another echo server. I built it because I wanted
a place where every performance claim I make has a command next to it that reproduces it —
and because I wanted to find out which of the things I "knew" about low-latency networking
would survive being measured. One of them didn't.

No dependencies. `g++` and a 30-line Makefile.

```
make            # build everything into bin/
make test       # ring correctness tests
make tsan       # the same tests under ThreadSanitizer
make bench      # the false-sharing microbenchmark
```

## Setup

You need `g++` with C++20 support (I used 13.3.0) and `make`. That's it — no libraries, no
cmake, no package installs.

```bash
cd low-latency-ipc
make
make test
```

On Windows I run this through WSL:

```bash
wsl -e bash -lc 'cd /path/to/low-latency-ipc && make all && make test'
```

To drive the server by hand, run it in one shell and the harness in another:

```bash
./bin/server --port=9099 --nodelay=1
./bin/bench_client --port=9099 --nodelay=1 --requests=50000
```

`--split-send=1` on the client is the flag that matters; see Result 2.

## What's here

| File | What it is |
|---|---|
| `src/spsc_ring.hpp` | Bounded lock-free single-producer/single-consumer ring buffer |
| `src/test_ring.cpp` | Correctness tests, including a concurrent exact-sequence check |
| `src/bench_ring.cpp` | Measures the cost of false sharing on the ring's two indices |
| `src/server.cpp` | Edge-triggered `epoll` TCP server, length-prefixed binary protocol |
| `src/bench_client.cpp` | Closed-loop latency harness reporting p50/p90/p99/p99.9/max |

---

## Result 1 — false sharing costs between 1.1x and 3.8x, and the spread is the honest answer

`head` is written only by the producer and `tail` only by the consumer, so the two threads
share no data. But cache coherence works on **lines**, not variables. With both indices in
one 64-byte line, every producer store invalidates the consumer's copy, and the consumer's
next load of `tail` — a variable the producer never touches — misses.

The only difference between the two arms is `alignas(64)` on two members.

```
$ make bench                       # 20,000,000 items, best-of-5, threads pinned
UNPADDED (head and tail share a cache line)   35.45 ns/item
PADDED   (separate cache lines)                9.27 ns/item
RESULT  padding is 3.82x FASTER
sizeof(unpadded) = 8208 B   sizeof(padded) = 8320 B
```

I nearly wrote "3.82x faster" in this README and stopped, because one run isn't a
measurement. Nine runs, five of them (3.26x, 2.24x, 3.12x, 1.69x and 2.97x) on the first
version of the benchmark, before I added thread pinning:

| | ns/item |
|---|---|
| unpadded | 17.7 – 36.9 |
| padded | 9.3 – 15.9 |
| **ratio** | **1.12x, 1.69x, 2.24x, 2.41x, 2.97x, 3.12x, 3.26x, 3.43x, 3.82x** |

I added `sched_setaffinity` pinning (`--cpu-a/--cpu-b`) expecting it to collapse that
spread. It didn't. The reason is environmental: this runs on **WSL2**, a VM, where pinning
binds a thread to a *virtual* CPU whose mapping to a physical core isn't stable, and where
host load and frequency scaling are both outside the benchmark's control.

So here's what I think is actually supported:

- **Padding was faster in 9 of 9 runs.** That direction is solid.
- **The padded arm is much more stable** (9.3–15.9) than the unpadded one (17.7–36.9),
  which is consistent with the unpadded case being sensitive to where the two threads land
  — exactly the mechanism under test.
- **The magnitude isn't reproducible here.** Quoting "3.26x" would mean picking the run that
  flatters the change. A bare-metal Linux box with `isolcpus` and a fixed governor is what
  would make a single number quotable.

The benchmark verifies a checksum and the full sequence every run and throws the timing away
if either is wrong, because a throughput number from a ring that dropped items is worse than
no number at all. It also exits non-zero if a thread pin fails, so a run that quietly
reverted to unpinned placement can't be mistaken for a pinned one.

This is a microbenchmark of an otherwise-empty ring, so it's an **upper bound**: both threads
do nothing but push and pop, which maximises contention. Real work between operations sees
less.

---

## Result 2 — TCP_NODELAY does nothing here, until it does 973x

This is the one that corrected me.

My first version of the comment in `server.cpp` asserted that Nagle's algorithm costs a
latency cliff on request/response traffic. I'd read that a hundred times and writing it down
didn't feel like making a claim. **The measurement says it's false** for a strict ping-pong:

```
$ ./bin/server --port=9099 --nodelay=1 &   # then bench_client --nodelay=1
p50 45.73 us   p99 111.14 us   p99.9 166.92 us   (50,000 requests)

$ ./bin/server --port=9100 --nodelay=0 &   # then bench_client --nodelay=0
p50 46.54 us   p99 117.24 us   p99.9 183.44 us   (50,000 requests)
```

Indistinguishable. The precondition that "always set TCP_NODELAY" usually leaves out is that
Nagle may only withhold a small segment while an earlier one is **unacknowledged**, and a
ping-pong never has two in flight. There's nothing to withhold.

The cliff is real and it needs **write-write-read**. `--split-send=1` writes the 4-byte
length header and the payload as two separate `send()` calls, so the second small write waits
on the ACK of the first and the peer's delayed-ACK timer supplies the delay:

```
$ ./bin/bench_client --split-send=1 --nodelay=1
p50     45.20 us   max    272.39 us    18,550 req/s

$ ./bin/bench_client --split-send=1 --nodelay=0
p50 43,999.88 us   max 47,891.08 us        23 req/s
```

**~973x on p50** — and unlike Result 1, this replicates. Three independent runs of the
`--nodelay=0` arm:

```
p50  43,996.32 us
p50  43,996.63 us
p50  43,997.27 us      control (--nodelay=1, same shape): p50 52.23 us
```

Under one microsecond of spread across three runs on a figure of ~44 ms. That
reproducibility **is** the evidence: contention varies run to run (see Result 1, which swings
3.4x), whereas a fixed kernel timer lands on the same value every time. It isn't just a big
number, it's the *same* big number repeatedly, which is what separates "we hit the
delayed-ACK timer" from "the machine was busy".

The two experiments together are the real finding. The effect is **conditional**, and the
condition is the part that usually gets dropped.

---

## Result 3 — the ring is correct, and I check that rather than claim it

```
$ make test
[single-threaded boundaries]  8 checks
[wraparound]                  10,000 cycles through a 4-slot ring
[move-only payload]           buffer is MOVED, not copied (same heap allocation)
[concurrent producer/consumer, n=5000000]
  ok    consumer saw EXACTLY n items (no loss, no duplication)
  ok    every item arrived in strict producer order
ALL TESTS PASSED (0 failures)
```

The concurrent test pushes a strictly increasing sequence and asserts the consumer sees
exactly that sequence. A count check alone catches loss and duplication; the sequence check
also catches reordering and torn publishes, which are what a mis-ordered `acquire`/`release`
actually produces.

```
$ make tsan
ALL TESTS PASSED (0 failures)      # 0 ThreadSanitizer warnings
```

**This caveat matters more than the result.** On WSL2, ThreadSanitizer aborts with `FATAL:
ThreadSanitizer: unexpected memory mapping` — and it aborts *before running anything*, while
still printing zero warnings. A naive `grep -c WARNING` on that output reports "0 races" for
a check that never executed. The Makefile runs it under `setarch -R` (ASLR off), and the exit
code is what tells a real pass from an aborted one. Check the exit code, not the warning
count.

---

## Honest limits

- Measured on **WSL2** (Ubuntu 24.04, kernel 6.6.87.2) over **loopback**, on a 24-thread
  desktop. Loopback skips the NIC entirely, so the absolute microsecond figures aren't
  comparable to a real network path. The *ratios* are the transferable part.
- **Single connection, single event loop.** No `SO_REUSEPORT` sharding, no multi-threaded
  acceptor. Throughput figures are one core's, not the machine's.
- **No `io_uring` path.** The kernel supports it (`io_uring_setup` is present) but
  `liburing` isn't installed here, so I didn't build it and I'm not claiming it. The event
  loop is structured so a second backend could sit behind the same interface.
- **No TLS**, no auth. The 4-byte length header is attacker-controlled and bounded at 1 MiB
  (`kMaxFrame`); that bound is the only hardening here.
- I report `best-of-N` rather than a mean, because on a loaded desktop the mean measures the
  scheduler. The rep count is printed so you can judge that choice yourself.

## Protocol

```
request   [u32 big-endian length][length bytes payload]
response  [u32 big-endian length=16][8 bytes FNV-1a hash][8 bytes echo of request[0..8]]
```

The echo carries a sequence number so the harness can match each reply to its request.
`bench_client` verifies every one and reports `echo mismatches`. A latency harness that
doesn't check its replies can post excellent numbers for a server that's mis-framing its
stream.
