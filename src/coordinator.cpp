#include "coordinator.h"

#include <chrono>
#include <future>
#include <iostream>

namespace kv {

Coordinator::Coordinator(std::vector<NodeConfig> nodes, CoordinatorOptions options)
    : options_(options), ring_(options.virtual_nodes_per_physical) {
  if (options_.write_quorum <= 0) {
    options_.write_quorum = options_.replication_factor / 2 + 1;
  }

  for (const auto& cfg : nodes) {
    auto handle = std::make_unique<NodeHandle>();
    handle->id = cfg.id;
    handle->address = cfg.address;
    handle->channel = grpc::CreateChannel(cfg.address, grpc::InsecureChannelCredentials());
    handle->stub = KVStore::NewStub(handle->channel);
    ring_.AddNode(cfg.id);
    nodes_by_id_[cfg.id] = handle.get();
    node_storage_.push_back(std::move(handle));
  }

  health_thread_ = std::thread(&Coordinator::HealthCheckLoop, this);
}

Coordinator::~Coordinator() {
  stop_.store(true);
  if (health_thread_.joinable()) health_thread_.join();
}

std::vector<Coordinator::NodeHandle*> Coordinator::LiveTargets(const std::string& key) {
  // Ask the ring for every physical node in preference order, then keep the
  // first `replication_factor_` that are currently healthy. A node marked
  // down is simply skipped, so writes/reads transparently land on the next
  // replica in the ring - this is the failover path.
  auto ordered = ring_.GetPreferenceList(key, static_cast<int>(nodes_by_id_.size()));

  std::vector<NodeHandle*> targets;
  for (const auto& node_id : ordered) {
    NodeHandle* node = nodes_by_id_.at(node_id);
    if (node->healthy.load(std::memory_order_relaxed)) {
      targets.push_back(node);
      if (static_cast<int>(targets.size()) >= options_.replication_factor) break;
    }
  }
  return targets;
}

bool Coordinator::Put(const std::string& key, const std::string& value) {
  auto targets = LiveTargets(key);
  if (targets.empty()) return false;

  // Fan out to every replica in parallel; a slow/unreachable node shouldn't
  // make the others wait. Each call carries its own deadline so a hung node
  // can't block the write indefinitely.
  std::vector<std::future<bool>> futures;
  futures.reserve(targets.size());
  for (NodeHandle* node : targets) {
    futures.push_back(std::async(std::launch::async, [this, node, &key, &value]() {
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() +
                        std::chrono::milliseconds(options_.rpc_deadline_ms));
      PutRequest req;
      req.set_key(key);
      req.set_value(value);
      PutResponse resp;
      return node->stub->Put(&ctx, req, &resp).ok() && resp.success();
    }));
  }

  int acks = 0;
  for (auto& f : futures) {
    if (f.get()) ++acks;
  }
  return acks >= options_.write_quorum;
}

bool Coordinator::Get(const std::string& key, std::string* value_out) {
  auto targets = LiveTargets(key);
  // Try replicas in ring order (primary first); the first one that answers
  // successfully wins. If a replica is unreachable despite being marked
  // healthy (e.g. it just failed), we fail over to the next one in the list
  // rather than failing the whole request.
  for (NodeHandle* node : targets) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                      std::chrono::milliseconds(options_.rpc_deadline_ms));
    GetRequest req;
    req.set_key(key);
    GetResponse resp;
    if (!node->stub->Get(&ctx, req, &resp).ok()) continue;
    if (resp.found()) *value_out = resp.value();
    return resp.found();
  }
  return false;
}

bool Coordinator::Delete(const std::string& key, bool* existed_out) {
  auto targets = LiveTargets(key);
  if (existed_out) *existed_out = false;
  if (targets.empty()) return false;

  std::vector<std::future<std::pair<bool, bool>>> futures;
  futures.reserve(targets.size());
  for (NodeHandle* node : targets) {
    futures.push_back(std::async(std::launch::async, [this, node, &key]() {
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() +
                        std::chrono::milliseconds(options_.rpc_deadline_ms));
      DeleteRequest req;
      req.set_key(key);
      DeleteResponse resp;
      bool ok = node->stub->Delete(&ctx, req, &resp).ok() && resp.success();
      return std::make_pair(ok, ok && resp.existed());
    }));
  }

  int acks = 0;
  bool existed_anywhere = false;
  for (auto& f : futures) {
    auto [ok, existed] = f.get();
    if (ok) ++acks;
    existed_anywhere = existed_anywhere || existed;
  }
  if (existed_out) *existed_out = existed_anywhere;
  return acks >= options_.write_quorum;
}

void Coordinator::HealthCheckLoop() {
  while (!stop_.load()) {
    for (auto& node_ptr : node_storage_) {
      NodeHandle* node = node_ptr.get();
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() +
                        std::chrono::milliseconds(options_.health_check_deadline_ms));
      PingRequest req;
      PingResponse resp;
      bool ok = node->stub->Ping(&ctx, req, &resp).ok();

      if (ok) {
        node->consecutive_failures.store(0, std::memory_order_relaxed);
        int successes = node->consecutive_successes.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!node->healthy.load(std::memory_order_relaxed) &&
            successes >= options_.recovery_threshold) {
          node->healthy.store(true, std::memory_order_relaxed);
          std::cerr << "[coordinator] node " << node->id << " is back UP\n";
        }
      } else {
        node->consecutive_successes.store(0, std::memory_order_relaxed);
        int failures = node->consecutive_failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (node->healthy.load(std::memory_order_relaxed) &&
            failures >= options_.failure_threshold) {
          node->healthy.store(false, std::memory_order_relaxed);
          std::cerr << "[coordinator] node " << node->id << " marked DOWN\n";
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(options_.health_check_interval_ms));
  }
}

std::vector<std::string> Coordinator::HealthyNodes() const {
  std::vector<std::string> result;
  for (const auto& node_ptr : node_storage_) {
    if (node_ptr->healthy.load(std::memory_order_relaxed)) result.push_back(node_ptr->id);
  }
  return result;
}

std::vector<std::string> Coordinator::AllNodes() const {
  std::vector<std::string> result;
  for (const auto& node_ptr : node_storage_) result.push_back(node_ptr->id);
  return result;
}

}  // namespace kv
