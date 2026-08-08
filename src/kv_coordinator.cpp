#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "consistent_hash.h"
#include "kv.grpc.pb.h"

namespace {
constexpr auto kHealthCheckInterval = std::chrono::seconds(1);
constexpr auto kRpcTimeout = std::chrono::seconds(2);
}  // namespace

// Routes requests across multiple storage nodes, replicating each key onto
// `replication_factor` of them, and routing around nodes a background
// health checker has marked down.
//
// The coordinator implements the *same* KeyValueStore service that a plain
// kv_server does, so a kv_client can point at either one without knowing
// the difference. For each key, ConsistentHashRing::GetNodes gives an
// ordered replica set (element 0 is the primary):
//   - GET tries replicas in order and returns the first successful
//     response, so a down primary transparently falls back to a replica.
//   - PUT/DELETE skip down replicas and write to whichever are healthy,
//     succeeding as long as at least one write lands. This favors
//     availability over strict consistency: if a replica was down during
//     a write, it simply doesn't have that update -- there is no
//     mechanism here to catch it back up when it returns (that's a
//     separate problem, "anti-entropy", outside this project's scope).
class CoordinatorServiceImpl final : public kv::KeyValueStore::Service {
 public:
  CoordinatorServiceImpl(const std::vector<std::string>& node_addresses, int replication_factor)
      : replication_factor_(replication_factor) {
    for (const auto& address : node_addresses) {
      ring_.AddNode(address);
      auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
      node_stubs_[address] = kv::KeyValueStore::NewStub(channel);
      healthy_[address] = true;  // assume healthy until the checker says otherwise
    }
    health_check_thread_ = std::thread(&CoordinatorServiceImpl::HealthCheckLoop, this);
  }

  ~CoordinatorServiceImpl() override {
    stop_.store(true, std::memory_order_relaxed);
    if (health_check_thread_.joinable()) {
      health_check_thread_.join();
    }
  }

  grpc::Status Get(grpc::ServerContext* context, const kv::GetRequest* request,
                    kv::GetResponse* response) override {
    grpc::Status last_error(grpc::StatusCode::UNAVAILABLE, "no healthy replica for key");
    for (const auto& address : ReplicasFor(request->key())) {
      if (!IsHealthy(address)) {
        std::cout << "  skip " << address << " (marked down)\n";
        continue;
      }
      grpc::ClientContext node_ctx;
      SetDeadline(&node_ctx);
      grpc::Status status = node_stubs_[address]->Get(&node_ctx, *request, response);
      if (status.ok()) {
        return status;
      }
      std::cout << "  " << address << " failed: " << status.error_message() << "\n";
      SetHealthy(address, false);
      last_error = status;
    }
    return last_error;
  }

  grpc::Status Put(grpc::ServerContext* context, const kv::PutRequest* request,
                    kv::PutResponse* response) override {
    int successes = 0;
    grpc::Status last_error(grpc::StatusCode::UNAVAILABLE, "no healthy replica for key");
    for (const auto& address : ReplicasFor(request->key())) {
      if (!IsHealthy(address)) {
        std::cout << "  skip " << address << " (marked down)\n";
        continue;
      }
      kv::PutResponse node_response;
      grpc::ClientContext node_ctx;
      SetDeadline(&node_ctx);
      grpc::Status status = node_stubs_[address]->Put(&node_ctx, *request, &node_response);
      if (status.ok()) {
        ++successes;
      } else {
        std::cout << "  " << address << " failed: " << status.error_message() << "\n";
        SetHealthy(address, false);
        last_error = status;
      }
    }
    if (successes == 0) {
      return last_error;
    }
    response->set_success(true);
    return grpc::Status::OK;
  }

  grpc::Status Delete(grpc::ServerContext* context, const kv::DeleteRequest* request,
                       kv::DeleteResponse* response) override {
    int successes = 0;
    bool existed = false;
    grpc::Status last_error(grpc::StatusCode::UNAVAILABLE, "no healthy replica for key");
    for (const auto& address : ReplicasFor(request->key())) {
      if (!IsHealthy(address)) {
        std::cout << "  skip " << address << " (marked down)\n";
        continue;
      }
      kv::DeleteResponse node_response;
      grpc::ClientContext node_ctx;
      SetDeadline(&node_ctx);
      grpc::Status status = node_stubs_[address]->Delete(&node_ctx, *request, &node_response);
      if (status.ok()) {
        ++successes;
        existed = existed || node_response.existed();
      } else {
        std::cout << "  " << address << " failed: " << status.error_message() << "\n";
        SetHealthy(address, false);
        last_error = status;
      }
    }
    if (successes == 0) {
      return last_error;
    }
    response->set_existed(existed);
    return grpc::Status::OK;
  }

 private:
  std::vector<std::string> ReplicasFor(const std::string& key) {
    auto replicas = ring_.GetNodes(key, replication_factor_);
    std::cout << "route: key='" << key << "' -> [";
    for (size_t i = 0; i < replicas.size(); ++i) {
      std::cout << (i == 0 ? "" : ", ") << replicas[i] << (i == 0 ? " (primary)" : "");
    }
    std::cout << "]\n";
    return replicas;
  }

  static void SetDeadline(grpc::ClientContext* context) {
    context->set_deadline(std::chrono::system_clock::now() + kRpcTimeout);
  }

  bool IsHealthy(const std::string& address) {
    std::shared_lock<std::shared_mutex> lock(health_mutex_);
    return healthy_.at(address);
  }

  void SetHealthy(const std::string& address, bool healthy) {
    std::unique_lock<std::shared_mutex> lock(health_mutex_);
    bool changed = healthy_[address] != healthy;
    healthy_[address] = healthy;
    if (changed) {
      std::cout << "health: " << address << " is now " << (healthy ? "UP" : "DOWN") << "\n";
    }
  }

  // Runs on a dedicated background thread for the coordinator's whole
  // lifetime, pinging every node every kHealthCheckInterval. This is the
  // *proactive* half of failure detection -- catching a down node even
  // before any client happens to send it a request. Get/Put/Delete above
  // add the *reactive* half: if an RPC fails despite the node having been
  // marked healthy, they mark it down immediately rather than waiting for
  // the next tick.
  void HealthCheckLoop() {
    while (!stop_.load(std::memory_order_relaxed)) {
      for (const auto& [address, stub] : node_stubs_) {
        kv::PingRequest request;
        kv::PingResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
        grpc::Status status = stub->Ping(&context, request, &response);
        SetHealthy(address, status.ok());
      }
      std::this_thread::sleep_for(kHealthCheckInterval);
    }
  }

  int replication_factor_;
  kv::ConsistentHashRing ring_;
  std::unordered_map<std::string, std::unique_ptr<kv::KeyValueStore::Stub>> node_stubs_;

  std::shared_mutex health_mutex_;
  std::unordered_map<std::string, bool> healthy_;

  std::atomic<bool> stop_{false};
  std::thread health_check_thread_;
};

int main(int argc, char** argv) {
  // argv: kv_coordinator <listen_address> <replication_factor> <node1_addr> <node2_addr> ...
  if (argc < 4) {
    std::cerr << "usage: " << argv[0]
              << " <listen_address> <replication_factor> <node_addr>... (at least one node)\n";
    return 1;
  }

  std::string listen_address = argv[1];
  int replication_factor = std::atoi(argv[2]);
  if (replication_factor < 1) {
    std::cerr << "replication_factor must be >= 1\n";
    return 1;
  }

  std::vector<std::string> node_addresses;
  for (int i = 3; i < argc; ++i) {
    node_addresses.emplace_back(argv[i]);
  }

  CoordinatorServiceImpl service(node_addresses, replication_factor);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  std::cout << "kv_coordinator listening on " << listen_address << ", routing across "
            << node_addresses.size() << " node(s) with replication factor "
            << replication_factor << "\n";
  server->Wait();

  return 0;
}
