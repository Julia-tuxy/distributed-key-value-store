# Distributed Key-Value Store

A multithreaded distributed key-value store in C++ using gRPC. Storage nodes
are independent OS processes that each own an in-memory shard of the
keyspace; a client-side coordinator library uses consistent hashing to route
requests, replicates writes across nodes, and automatically fails over
around dead nodes using a background health checker.

## Components

- `proto/kv.proto` — the gRPC service (`Get`, `Put`, `Delete`, `Ping`).
- `include/sharded_store.h` / storage node — thread-safe in-memory store, one
  process per node.
- `include/consistent_hash.h` — hash ring with virtual nodes; decides which
  physical nodes own a key.
- `include/coordinator.h` / `src/coordinator.cpp` — the client-side brain:
  replication, quorum, failover, and the background health-check thread.
- `src/kv_client.cpp` — interactive/one-shot CLI.
- `src/benchmark.cpp` — multithreaded load generator with latency/throughput
  reporting; can also run as one of several independent client *processes*
  (`--client-id`/`--result-dir`) for multi-client load tests.
- `scripts/start_cluster.sh`, `scripts/stop_cluster.sh` — launch/stop N
  storage node processes on localhost.
- `scripts/scalability_sweep.sh` — launches several independent
  `kv_benchmark` client processes at increasing concurrency levels and
  reports how throughput and tail latency scale.

## Build

Requires cmake, protobuf, and grpc (`brew install cmake protobuf grpc abseil`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

```bash
# Start a 5-node cluster (ports 50051-50055)
./scripts/start_cluster.sh 5

NODES="node1@127.0.0.1:50051,node2@127.0.0.1:50052,node3@127.0.0.1:50053,node4@127.0.0.1:50054,node5@127.0.0.1:50055"

./build/kv_client --nodes=$NODES put hello world
./build/kv_client --nodes=$NODES get hello
./build/kv_client --nodes=$NODES del hello
./build/kv_client --nodes=$NODES status   # which nodes the coordinator sees as healthy

# Kill a node and confirm reads/writes still succeed (automatic failover):
kill -9 $(pgrep -f "storage_node node1")
./build/kv_client --nodes=$NODES get hello

# Benchmark (single client process, multiple worker threads)
./build/kv_benchmark --nodes=$NODES --threads=8 --ops-per-thread=5000 --read-ratio=0.8 --keyspace=10000

# Scalability sweep: 4 independent client processes, increasing threads/client
./scripts/scalability_sweep.sh "$NODES" 4 "1 2 4 8 16" 2000

./scripts/stop_cluster.sh
```

## Design notes

- **Partitioning**: consistent hashing with ~100 virtual nodes per physical
  node (`ConsistentHashRing`), so adding/removing a node only reshuffles a
  small slice of keys instead of the whole keyspace.
- **Replication**: each key's full ring order gives an ordered preference
  list of nodes; the coordinator writes to the first `replication_factor`
  *healthy* nodes in that list in parallel and requires a write quorum
  (majority by default) to consider the write successful.
- **Failover**: reads try replicas in ring order and move to the next one on
  RPC failure; writes simply skip down nodes when building the target list.
  A background thread pings every node every 1.5s; 3 consecutive failures
  marks a node down, 2 consecutive successes marks it back up.
- **Thread safety**: each storage node shards its map into 16
  independently-locked buckets (`ShardedStore`) so concurrent requests to
  different keys don't contend on one global lock. The coordinator itself
  is safe to call from many threads concurrently (gRPC stubs/channels are
  thread-safe, and per-node health state uses atomics), which is what lets
  the benchmark hit it with a thread pool directly.
