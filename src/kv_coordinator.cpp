#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "consistent_hash.h"
#include "kv.grpc.pb.h"

// Routes requests across multiple storage nodes, replicating each key onto
// `replication_factor` of them.
//
// The coordinator implements the *same* KeyValueStore service that a plain
// kv_server does, so a kv_client can point at either one without knowing
// the difference. For each key, ConsistentHashRing::GetNodes gives an
// ordered replica set (element 0 is the primary); PUT/DELETE go to every
// node in that set, GET reads from just the primary.
//
// Reading only the primary means a key becomes unreachable if the primary
// happens to be down, even though a replica holding the same data is
// healthy -- the replicas exist but nothing here knows to fall back to
// them yet. That gap is exactly what step 7 (health checks + failover)
// closes.
class CoordinatorServiceImpl final : public kv::KeyValueStore::Service {
 public:
  CoordinatorServiceImpl(const std::vector<std::string>& node_addresses, int replication_factor)
      : replication_factor_(replication_factor) {
    for (const auto& address : node_addresses) {
      ring_.AddNode(address);
      auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
      node_stubs_[address] = kv::KeyValueStore::NewStub(channel);
    }
  }

  grpc::Status Get(grpc::ServerContext* context, const kv::GetRequest* request,
                    kv::GetResponse* response) override {
    auto replicas = ReplicasFor(request->key());
    grpc::ClientContext node_ctx;
    return node_stubs_[replicas.front()]->Get(&node_ctx, *request, response);
  }

  grpc::Status Put(grpc::ServerContext* context, const kv::PutRequest* request,
                    kv::PutResponse* response) override {
    auto replicas = ReplicasFor(request->key());
    for (const auto& address : replicas) {
      kv::PutResponse node_response;
      grpc::ClientContext node_ctx;
      grpc::Status status = node_stubs_[address]->Put(&node_ctx, *request, &node_response);
      if (!status.ok()) {
        return status;  // fail the whole write if any replica couldn't take it
      }
    }
    response->set_success(true);
    return grpc::Status::OK;
  }

  grpc::Status Delete(grpc::ServerContext* context, const kv::DeleteRequest* request,
                       kv::DeleteResponse* response) override {
    auto replicas = ReplicasFor(request->key());
    bool existed = false;
    for (const auto& address : replicas) {
      kv::DeleteResponse node_response;
      grpc::ClientContext node_ctx;
      grpc::Status status = node_stubs_[address]->Delete(&node_ctx, *request, &node_response);
      if (!status.ok()) {
        return status;
      }
      existed = existed || node_response.existed();
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

  int replication_factor_;
  kv::ConsistentHashRing ring_;
  std::unordered_map<std::string, std::unique_ptr<kv::KeyValueStore::Stub>> node_stubs_;
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
