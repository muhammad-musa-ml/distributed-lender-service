# The Lender server

This is the part I wrote. A C++20 gRPC service implementing the three RPCs in
[`../lender.proto`](../lender.proto), built against the MySQL + HDFS topology in
[`../docker-compose.yml`](../docker-compose.yml).

```
DbToHdfs(Empty)            -> StatusString          MySQL -> Parquet -> HDFS
BlockLocations(path)       -> map<string,int32>     which DataNode holds how many blocks
CalcAvgLoan(county_code)   -> {avg_loan, source}    per-county average, with a cache signal
```

---

## Setup, start to finish

You need **Docker** (I used 28.4.0) and **Python 3** for the fixture generator. Nothing
else — no local compiler, no protoc, no Arrow. All of that lives in a build image.

Run everything below from the `lender-service/` directory, not from here.

### 1. Build the toolchain image

```bash
docker build -f server/Dockerfile.builder -t lender-cpp-builder server
```

Takes a few minutes the first time and then basically never changes. I split it out from
the server image on purpose: it's ~1.5 GB of compiler, protoc, gRPC, Arrow and MySQL
headers, and I didn't want to re-resolve apt on every edit-compile cycle.

The last layer of that Dockerfile verifies itself — it prints the version of everything and
`test -f`s every header the server needs. If the build succeeds, the toolchain is good. On
mine it prints:

```
g++ 13.3.0 / cmake 3.28.3 / libprotoc 3.21.12 / /usr/bin/grpc_cpp_plugin
grpc++ 1.51.1  protobuf 3.21.12  arrow 25.0.1  parquet 25.0.1
```

### 2. Generate the MySQL fixture

```bash
python3 server/scripts/gen_dataset.py
```

`Dockerfile.mysql` copies `hdma-wi-2021.sql.gz` into the MySQL entrypoint directory, and
**that file isn't in this repo.** It was a course-provided dataset; I don't have it and
wouldn't redistribute it. So I wrote a generator that produces a schema-compatible
substitute at exactly that path, which means `Dockerfile.mysql` needs no changes.

It's deterministic (seed 544), so you get the same file I did. Mine printed:

```
rows generated            : 450000
rows inside 30k..800k     : 441244  (98.05%)
rows DbToHdfs will exclude: 8756
gzipped size              : 3449846 bytes
```

**The rows are fabricated.** The schema, the Wisconsin FIPS county codes and the HMDA
loan-type codes are real. The loans are not. Any average you see below describes this
generated fixture and says nothing about real lending.

### 3. Build the rest of the images

```bash
export PROJECT=lender
docker build -f Dockerfile.hdfs     -t $PROJECT-hdfs  .
docker build -f Dockerfile.namenode -t $PROJECT-nn    .
docker build -f Dockerfile.datanode -t $PROJECT-dn    .
docker build -f Dockerfile.mysql    -t $PROJECT-mysql .
docker build -f server/Dockerfile.server -t $PROJECT-server .
```

`hdfs` has to come before `nn` and `dn` — they're both `FROM lender-hdfs`. It downloads
Hadoop 3.3.6, so it's the slow one.

`docker-compose.yml` needed no edits. It already referenced a `${PROJECT}-server` image
whose build context didn't exist in the original project; this supplies it.

### 4. Run it

```bash
docker compose up -d
docker compose ps          # expect 6 containers: server, mysql, nn, dn x3
```

Give MySQL a minute or two — it's importing 450k rows on first boot. The server itself
starts immediately and prints what it resolved:

```
lender-server listening on 0.0.0.0:5000
  mysql   : root@mysql:3306/CS544
  webhdfs : http://boss:9870
  big file: /hdma-wi-2021.parquet (replication 2, block 1048576)
  parts   : /partitions (replication 1)
```

### 5. Call it

A client ships inside the image, so you don't need Python or generated stubs:

```bash
docker compose exec server lender_client localhost:5000 DbToHdfs
docker compose exec server lender_client localhost:5000 BlockLocations /hdma-wi-2021.parquet
docker compose exec server lender_client localhost:5000 CalcAvgLoan 55025
```

The original Python client still works too, and speaks the same protocol:
`python3 client.py CalcAvgLoan -c 55025` from a container on the compose network.

### 6. Or just run the whole check

```bash
bash server/scripts/e2e.sh
```

That brings the topology up, waits for MySQL and for three live DataNodes, and walks the
entire flow including the failure case. It exits non-zero on any failure. Takes about five
minutes, most of it waiting for the MySQL import and for the NameNode to declare a node
dead.

### Developing without the cluster

The unit tests need neither MySQL nor HDFS:

```bash
docker run --rm -v "$PWD:/work" lender-cpp-builder bash -c \
  'cmake -S /work/server -B /tmp/build -DCMAKE_BUILD_TYPE=Release &&
   cmake --build /tmp/build -j$(nproc) &&
   /tmp/build/lender_tests'
```

**On Windows / Git Bash**, prefix docker commands with `MSYS_NO_PATHCONV=1`. Otherwise MSYS
rewrites `-w /tmp` into a Windows path and docker fails with "the working directory is
invalid, it needs to be an absolute path". That one cost me a while to figure out.

---

## The design, and the one interesting bit

The two storage policies are deliberately asymmetric:

| | replication | survives a DataNode loss? |
|---|---|---|
| `/hdma-wi-2021.parquet` | 2 | yes |
| `/partitions/<county>.parquet` | 1 | **no** |

The per-county file is a cache that's *supposed* to be losable. When a DataNode goes down,
the NameNode still has the partition's metadata — so as far as it's concerned the file
exists — but no replica can serve the bytes. That disagreement is the entire signal for
`recreate`.

Which is why [`cache_policy.hpp`](src/cache_policy.hpp) takes **two** observations instead
of one boolean:

```
                cache_usable = true   cache_usable = false
exists = kNo    reuse                 create        cold cache
exists = kYes   reuse                 RECREATE      a DataNode is down, or the bytes are corrupt
exists = kError reuse                 create        can't show anything was ever written
```

`cache_usable` is false unless the bytes came back **and** decoded as Parquet, because a
file that reads back as garbage is corrupt and the spec treats corrupt the same as lost.

I pulled this out into a pure function for one reason: otherwise the only way to test the
middle-right cell is to kill a container. As a function of two inputs, every cell above is a
unit test that runs in milliseconds. I still kill the container — see the e2e script — but
I don't have to do that to know the logic is right.

The `kError` row is the one I went back and forth on. If the metadata probe itself failed, I
can't show that anything was ever written there, so calling it `recreate` would be asserting
a history I have no evidence for. `create` is the weaker and more honest claim.

---

## Decisions I'd defend

**WebHDFS over REST, not `libhdfs`.** `libhdfs` is a JNI shim. Linking it means the runtime
image carries a JVM and a working CLASSPATH so that a program which makes four kinds of call
can make them. The cost of REST is that every operation is a round trip and large reads
stream through memory. At this dataset size that's fine, and it's in the limits section
rather than hidden.

**A real JSON parser, but a hand-written filter and sum.** These look inconsistent and I
think they're the same rule applied twice.

`nlohmann/json` parses the `GETFILEBLOCKLOCATIONS` response because that's *somebody else's
wire format*. Matching nested JSON with hand-rolled string searching is a well-known way to
ship something subtly wrong about escaping that looks fine on the one sample you tested.

The county filter and the sum are hand-written because Arrow 25 ships its compute kernels in
a separate `libarrow-compute` that `libarrow-dev` doesn't pull in. I found this the direct
way: `CallFunction("equal", ...)` returned

```
Key error: No function registered with name: equal
```

and `nm -D` on `libarrow.so` shows it exports `GetFunctionRegistry` and zero kernel
registration symbols. So the choice was: add a large kernel library to the *runtime* image,
or write an int32 equality filter and an int64 sum. There's no external format to
misinterpret in adding up numbers I produced myself, and the semantics are pinned by tests,
so I wrote them. A library earns its place where the problem is interpreting a format I
didn't define. It doesn't where the problem is arithmetic.

**Integer division for the average, never a float that gets floored.** `total / n` on an
int64 accumulator. Integer division of a non-negative total *is* the floor, so the classic
off-by-one where a true mean of 250000 shows up as 249999.99999999997 can't happen. The
accumulator is 64-bit on purpose: 440k rows bounded by 800,000 sum to about 3.4×10¹¹, well
past int32.

**CMake here, plain `make` in `low-latency-ipc`.** That project's Makefile explains its own
choice — four translation units, no dependency beyond libstdc++ and pthreads, so a build
system would've been the biggest file in it. The reasoning flips here: six libraries and two
codegen steps that need real dependency tracking.

**Locks striped by county, not one global lock.** gRPC dispatches on a thread pool, so two
clients can ask for the same county at the same instant. Both find no cache, both rebuild
it, both PUT to the same path. A global lock would fix that and serialise unrelated counties
too, which matters because the slow path is reading the whole big file.

**A failed cache write is logged, not returned.** By the time the partition gets written I
already have the right answer. Refusing to return it because the cache couldn't be updated
would turn a degraded cluster into a broken RPC. This one is also a compatibility thing:
`client.py` does `if resp.error:` and prints the message *instead of* the answer, so putting
a warning in that field would hide the number the caller asked for. `resp.error` means "this
RPC did not answer your question". Everything else goes to the log.

---

## Testing

Three layers.

**Unit** — 65 checks, no MySQL, no HDFS. This also runs as a layer in `Dockerfile.server`,
so an image that can't pass its own tests can't be built.

**Mutation** — a green suite isn't evidence until you've watched it go red. I plant three
mutants in the load-bearing logic and require each to fail the suite:

| mutant | result |
|---|---|
| DataNode-failure branch returns `create` instead of `recreate` | killed |
| average uses `(double)total/n + 0.5` instead of integer division | killed |
| block counter records presence instead of tallying per host | killed |

The second one is why I bother. On the first run it **survived**. My "rounds down"
assertions used data averaging 20.33, which floors to 20 and also rounds-to-nearest to 20 —
so the test passed against exactly the wrong implementation it was written to catch. The
fixtures now include 62/3 = 20.667 and an exact 3/2, where floor and round disagree. A
rounding test whose fractional part is under .5 tests nothing.

**End-to-end** — [`scripts/e2e.sh`](scripts/e2e.sh). The last phase is the point:

```
PHASE 1  DbToHdfs   wrote 441244 rows (2562630 bytes) to /hdma-wi-2021.parquet
                    at replication 2, block size 1048576
PHASE 2  BlockLocations on the big file    3 datanodes, 6 block replicas
PHASE 3  CalcAvgLoan, cold cache           252113   create
PHASE 4  CalcAvgLoan again                 252113   reuse
PHASE 5  the partition lives on exactly 1 DataNode -> stop that container
PHASE 6  CalcAvgLoan                       252113   recreate
```

441,244 is the generator's own count of rows inside the 30k–800k window, so the filter is
doing what it claims. 6 block replicas over 3 blocks is the 2x replication, visible from
the outside.

It asks the cluster which node holds the partition, stops *that* one, waits for the NameNode
to mark it dead (~30–45s, set by `stale.datanode.interval` in `Dockerfile.namenode`), and
re-issues the call. The answer has to be unchanged, and it is.

---

## Two things that each cost me a build

**`-std=c++20` is required, not preferred.** Arrow 25's `arrow/buffer.h` uses `std::span`.
My first attempt used C++17 and failed with `error: 'span' in namespace 'std' does not name
a template type`.

**Arrow and Parquet aren't in Ubuntu 24.04's archive.** `apt-cache policy` inside
`ubuntu:24.04` reports no candidate for `libarrow-dev` or `libparquet-dev` while everything
else resolves. They come from Apache's own apt source. The first build of that layer failed
with `apache-arrow-apt-source : Depends: gnupg but it is not installable` — the message
names gnupg, but the actual cause is that the preceding layer ends in
`rm -rf /var/lib/apt/lists/*`, so the layer starts with no package index at all. The fix is
the `apt-get update` at the head of the layer, not just adding gnupg.

A third, smaller one: in CMake, don't quote the plugin path in
`--plugin=protoc-gen-grpc=...`. Under `VERBATIM` the quotes get passed through as literal
characters and protoc reports `"/usr/bin/grpc_cpp_plugin": program not found`, which reads
exactly like a missing plugin when the file is right there and executable.

---

## Limits

- **Whole-file reads.** A cold `CalcAvgLoan` pulls the entire big Parquet file into memory
  and filters it. Parquet row-group statistics would let me skip row groups whose county
  range excludes the target. At this size it isn't worth the complexity yet, and the striped
  locks mean one rebuild doesn't block other counties.
- **No auth.** WebHDFS is called with `user.name=` and gRPC listens on insecure credentials,
  which matches the topology. Neither is OK outside it.
- **`BlockLocations` takes a caller-supplied path**, so it's validated against a strict
  allowlist (`[A-Za-z0-9/._-]`, absolute, no `..`) before it's concatenated into a URL. The
  tests cover traversal and query-string smuggling.
- The server runs as a non-root user and drains in-flight RPCs on SIGTERM, which is about as
  far as the production-readiness goes.
