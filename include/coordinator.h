#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "consistent_hash.h"
#include "kv.grpc.pb.h"

namespace kv {

struct NodeConfig {
  std::string id;       // e.g. "node1"
  std::string address;  // e.g. "127.0.0.1:50051"
};

struct CoordinatorOptions {
  int replication_factor = 3;
  int virtual_nodes_per_physical = 100;
  int write_quorum = -1;             // -1 => majority of replication_factor
  int rpc_deadline_ms = 300;         // per-RPC deadline for Get/Put/Delete
  int health_check_interval_ms = 1500;
  int health_check_deadline_ms = 400;
  int failure_threshold = 3;         // consecutive failed pings -> mark down
  int recovery_threshold = 2;        // consecutive good pings -> mark back up
};

// Client-side "brain" of the cluster: owns the consistent-hash ring, holds a
// gRPC channel+stub per storage node, replicates writes, fails reads/writes
// over to the next live replica, and runs a background thread that pings
// every node to detect failure/recovery. A single Coordinator instance is
// safe to call concurrently from many threads (see comments below), which
// is what lets the benchmark suite hammer it with a thread pool.
class Coordinator {
 public:
  Coordinator(std::vector<NodeConfig> nodes, CoordinatorOptions options = {});
  ~Coordinator();

  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  // Returns true if the value was written to at least the write quorum.
  bool Put(const std::string& key, const std::string& value);

  // Returns true if a value was found; *value_out is only set in that case.
  // Returns false both when the key doesn't exist and when every replica
  // was unreachable (check LastOperationHadError() to tell those apart).
  bool Get(const std::string& key, std::string* value_out);

  // Returns true if the delete reached write quorum. *existed_out reports
  // whether any replica reported the key as present beforehand.
  bool Delete(const std::string& key, bool* existed_out);

  // Node ids currently considered healthy by the background health checker.
  std::vector<std::string> HealthyNodes() const;
  std::vector<std::string> AllNodes() const;

 private:
  struct NodeHandle {
    std::string id;
    std::string address;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<KVStore::Stub> stub;
    std::atomic<bool> healthy{true};
    std::atomic<int> consecutive_failures{0};
    std::atomic<int> consecutive_successes{0};
  };

  void HealthCheckLoop();
  // Full ring order for `key`, filtered to currently-healthy nodes, capped
  // at replication_factor_. This is where automatic failover happens: a
  // down primary is simply absent from this list, so the next node in ring
  // order takes its place.
  std::vector<NodeHandle*> LiveTargets(const std::string& key);

  CoordinatorOptions options_;
  ConsistentHashRing ring_;
  std::vector<std::unique_ptr<NodeHandle>> node_storage_;
  std::unordered_map<std::string, NodeHandle*> nodes_by_id_;

  std::thread health_thread_;
  std::atomic<bool> stop_{false};
};

}  // namespace kv
