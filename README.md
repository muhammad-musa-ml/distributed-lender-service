# distributed-lender-service

Two C++ systems projects I built to have something real behind the claims I make about
low-level and distributed work.

- **[`lender-service/`](lender-service/)** — a C++20 gRPC service that pulls ~440k mortgage
  records out of MySQL, writes them to HDFS as Parquet, and answers per-county aggregate
  queries through a cache it deliberately stores at replication 1 so it can lose it and
  rebuild it when a DataNode dies.
- **[`low-latency-ipc/`](low-latency-ipc/)** — a lock-free SPSC ring buffer and an
  edge-triggered `epoll` TCP server, written mainly as a place to measure things properly.
  One of the two performance claims in it was wrong until I measured it.

Both build and run. Neither is a toy that only compiles.

## Why these exist

I kept writing "C/C++" and "distributed systems" on a resume and then having nothing to
point at. Coursework doesn't count for much — nobody can see it, and I can't link to it.
So I picked the two things I'd actually be asked about in an interview and built them,
with the rule that I don't get to claim anything I haven't run.

That rule turned out to matter. In `low-latency-ipc/` I wrote a comment asserting that
Nagle's algorithm hurts request/response latency, measured it, and found no difference at
all (45.73 µs vs 46.54 µs p50). The effect is real but needs a write-write-read pattern,
which I then built a flag to isolate — and got a 973x difference that reproduces to within
a microsecond across three runs. The corrected version is in the README there, including
the wrong first draft, because the correction is the interesting part.

In `lender-service/` the same rule caught a bad test. I'd written assertions pinning that
an average "rounds down", they passed, and a mutation run showed they'd also pass against a
round-to-nearest implementation. My test data averaged 20.33, which floors *and* rounds to
20. Fixed by adding a case where the two disagree.

## Where to start

If you only want to look at one thing, read
[`lender-service/server/src/cache_policy.hpp`](lender-service/server/src/cache_policy.hpp).
It's about forty lines and it's the whole idea of the project: the big Parquet file is
stored at replication 2 and survives a node loss, the per-county cache is stored at
replication 1 and doesn't, and the RPC has to notice the difference and say which of
create / reuse / recreate it did.

The thing I like about it is that "a DataNode is down" shows up as a *disagreement between
two observations* — the NameNode still has the file's metadata, so it exists, but no replica
can serve the bytes. Keeping that as a pure function of two inputs instead of an `if` buried
in the handler means I can unit-test the failure path on a laptop instead of only by killing
containers.

I did also kill the containers. See below.

## Running them

Each project has its own README with full setup. Short version:

```bash
# low-latency-ipc: needs g++ and make, nothing else
cd low-latency-ipc && make && make test && make bench

# lender-service: needs Docker
cd lender-service
docker build -f server/Dockerfile.builder -t lender-cpp-builder server
python3 server/scripts/gen_dataset.py          # generates the MySQL fixture
export PROJECT=lender
docker build -f Dockerfile.hdfs     -t $PROJECT-hdfs  .
docker build -f Dockerfile.namenode -t $PROJECT-nn    .
docker build -f Dockerfile.datanode -t $PROJECT-dn    .
docker build -f Dockerfile.mysql    -t $PROJECT-mysql .
docker build -f server/Dockerfile.server -t $PROJECT-server .
docker compose up -d
bash server/scripts/e2e.sh                     # the whole thing, including fault injection
```

## What I actually verified

I don't want to list features, so here is what I ran and what it printed.

**`lender-service`**

| | |
|---|---|
| Compiler warnings | 0, under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion` |
| Unit suite | 65 checks, 0 failures — no MySQL and no HDFS needed |
| Mutation run | 3 of 3 planted mutants killed |
| End-to-end | create → reuse → **kill the DataNode holding the only replica** → recreate, all returning 252113 |
| Server image | 293 MB (the toolchain image it's built in is 1.09 GB) |

The end-to-end script doesn't fake the failure. It asks the running cluster which DataNode
holds `/partitions/55025.parquet`, stops that specific container, waits for the NameNode to
mark it dead, and re-issues the RPC. The answer has to come back unchanged, and it does.

**`low-latency-ipc`**

| | |
|---|---|
| Ring correctness | 5,000,000 items, exact-sequence check (catches loss, duplication, reordering, torn publishes) |
| ThreadSanitizer | clean — judged by exit code, because on WSL2 TSan aborts *before running* while still printing 0 warnings |
| False sharing | padded faster in 9 of 9 runs; ratio 1.12x–3.82x, and I explain why I won't quote a single number |
| Nagle / delayed ACK | no effect on ping-pong; ~973x on write-write-read, replicated to <1 µs across 3 runs |

## Things I'd want asked about

- Why `lender-service` talks to HDFS over WebHDFS REST instead of linking `libhdfs` (short
  answer: `libhdfs` is JNI, and I didn't want a JVM in the runtime image for four kinds of
  call).
- Why I used a real JSON library for one thing and hand-wrote the filter and the sum for
  another. Those look inconsistent and I think they're the same rule.
- Why the average is integer division and not a float that gets floored.
- Why the false-sharing number is a range and the Nagle number isn't.

## Caveats, up front

The loan rows are **synthetic**. The schema, the Wisconsin FIPS county codes and the HMDA
loan-type codes are real; the rows come from a seeded generator I wrote, because the
original dataset was a course-provided file I don't have and wouldn't redistribute anyway.
Every count in these READMEs describes that generated fixture. There's a generator script
so anyone can reproduce it exactly.

The `lender.proto` contract, the `docker-compose.yml` topology and the four infrastructure
Dockerfiles in `lender-service/` come from a UW–Madison CS 544 project. That's the
environment I built against. Everything in `lender-service/server/` and all of
`low-latency-ipc/` is mine, written in 2026, well after that course ended and in a language
it didn't use.
