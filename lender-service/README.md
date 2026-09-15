# lender-service

A gRPC service over MySQL, Arrow/Parquet and HDFS, running as six containers.

The interesting code is in **[`server/`](server/)** and that directory has the detailed
README. This one explains what's around it and what came from where.

## What's mine and what isn't

I should be straight about this, because it changes how you'd read the code.

**Not mine** — these came from a UW–Madison CS 544 project and are the environment I built
against:

| File | What it is |
|---|---|
| `lender.proto` | The service contract. Three RPCs, and I implement them as given. |
| `docker-compose.yml` | The topology: one server, one MySQL, one NameNode, three DataNodes. |
| `Dockerfile.hdfs` | Ubuntu + JDK 11 + Hadoop 3.3.6, the base for the two below. |
| `Dockerfile.namenode` | Formats and runs the NameNode as `boss:9000`. |
| `Dockerfile.datanode` | Runs a DataNode against `boss`. Three replicas of this. |
| `Dockerfile.mysql` | MySQL 8.4 that loads a dump on first boot. |
| `client.py` | A Python reference client. Still works against my server. |

**Mine** — everything in [`server/`](server/), written in 2026: the C++ server, a native
client, the unit and mutation tests, the two-stage runtime image, the end-to-end script with
fault injection, and the dataset generator.

The original project had no server implementation in this repo at all. `docker-compose.yml`
referenced a `${PROJECT}-server` image whose build context didn't exist. That's the gap this
fills, which is why `docker-compose.yml` needed no edits.

## The topology

```
        client.py / lender_client
                  |
                  v  :5000
            +-----------+
            |  server   |   C++20, gRPC, non-root
            +-----------+
             /         \
   :3306    /           \  :9870 WebHDFS
   +-------+         +-------+
   | mysql |         |  nn   |  "boss", NameNode
   +-------+         +-------+
                     /   |   \
                 +----+ +----+ +----+
                 | dn | | dn | | dn |   3 replicas
                 +----+ +----+ +----+
```

The server holds no state. It reads MySQL per call and talks to HDFS over REST, so
restarting it loses nothing.

## Storage layout

| Path | Replication | Block size | Why |
|---|---|---|---|
| `/hdma-wi-2021.parquet` | 2 | 1 MB | Survives losing one DataNode. The 1 MB blocks are what spread it across several nodes instead of landing as one block on one. |
| `/partitions/<county>.parquet` | 1 | 1 MB | A cache. Deliberately doesn't survive, which is the whole point. |

That asymmetry is the design. See [`server/README.md`](server/README.md) for what the server
does when the second one disappears.

## Running it

Full setup is in [`server/README.md`](server/README.md). The short version, from this
directory:

```bash
docker build -f server/Dockerfile.builder -t lender-cpp-builder server
python3 server/scripts/gen_dataset.py
export PROJECT=lender
for img in hdfs namenode datanode mysql; do
  case $img in namenode) tag=nn;; datanode) tag=dn;; *) tag=$img;; esac
  docker build -f Dockerfile.$img -t $PROJECT-$tag .
done
docker build -f server/Dockerfile.server -t $PROJECT-server .
docker compose up -d
bash server/scripts/e2e.sh
```

`Dockerfile.hdfs` must build before the namenode and datanode images — both are `FROM
lender-hdfs`.

## A note on the data

`Dockerfile.mysql` expects `hdma-wi-2021.sql.gz`, which is **not in this repo** and is
gitignored. It was a course-provided dataset. I don't have it, and I wouldn't republish it
if I did, so `server/scripts/gen_dataset.py` generates a schema-compatible substitute at
that exact path with a fixed seed.

The rows are made up. The schema, the Wisconsin FIPS county codes and the HMDA loan-type
codes are real, so the shape of the thing is right, but no number that comes out of this
tells you anything about actual mortgage lending.
