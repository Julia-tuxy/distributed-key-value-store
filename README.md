# Distributed Key-Value Store

A distributed key-value store in C++ from scratch, using gRPC for the network layer. It started as a single-threaded map with a lock around it and grew, one piece at a time, into something with sharding, replication, and failover. Every step below was actually run against a live cluster before I moved to the next one — the numbers in this README aren't made up.

## What it does

- In-memory store with GET/PUT/DELETE, safe for concurrent access (`shared_mutex`, readers don't block each other)
- Talks over gRPC — the store is wrapped in a service, so it's reachable over the network instead of just being a library
- A coordinator sits in front of a set of storage nodes and figures out which node owns which key, using consistent hashing with virtual nodes instead of plain `hash % N`
- Each key gets written to more than one node (replication factor is configurable)
- The coordinator pings every node once a second and also notices failed RPCs immediately, so a dead node gets routed around instead of taking down reads/writes for keys it was holding
- Runs in Docker — `docker compose up` gets you a 3-node cluster + coordinator
- Has an actual benchmark tool that ramps up concurrency and reports latency percentiles, not just one throughput number

## Running it

Local build (needs CMake 3.16+, C++17, gRPC + Protobuf with pkg-config files — Homebrew's `grpc`/`protobuf` on macOS just work):

```bash
cmake -B build
cmake --build build -j
```

Start a 3-node cluster (replication factor 2) and talk to it:

```bash
./scripts/start_cluster.sh
./build/kv_client localhost:60000
> PUT hello world
OK
> GET hello
world
```

```bash
./scripts/stop_cluster.sh
```

Or with Docker:

```bash
docker compose up -d --build
./build/kv_client localhost:60000
docker compose down
```

Kill a node and watch it fail over:

```bash
docker compose stop node1
docker compose logs coordinator -f
```

## Benchmark

```bash
./build/kv_benchmark localhost:60000 "1,2,4,8,16" 1500
```

This is from a run against a local 3-node cluster:

```
threads       ops/sec   p50(us)   p95(us)   p99(us)   max(us)   errors
1                4222       210       330       347       438        0
2                7181       254       399       451       545        0
4               10796       342       542       626       867        0
8               13458       560       861      1014      1829        0
16              15407       998      1480      1763      4879        0
```

Throughput keeps climbing but p99 climbs faster than p50 — that's the system starting to feel the load. A single ops/sec number wouldn't show that.

## Layout

```
proto/kv.proto             gRPC service + messages
include/kv_store.h         the thread-safe map
include/consistent_hash.h  hash ring + replica lookup
src/kv_server.cpp          storage node
src/kv_coordinator.cpp     routing, replication, health checks, failover
src/kv_client.cpp          REPL client
src/kv_stress_test.cpp     concurrent-client correctness check
src/kv_benchmark.cpp       throughput/latency sweep
src/consistent_hash_demo.cpp   modulo vs. consistent hashing comparison
scripts/                   start/stop a local cluster
Dockerfile, docker-compose.yml
```
