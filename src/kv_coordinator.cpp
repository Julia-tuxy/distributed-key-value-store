#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "consistent_hash.h"
#include "kv.grpc.pb.h"

// Routes requests across multiple storage nodes.
//
// The coordinator implements the *same* KeyValueStore service that a plain
// kv_server does, so a kv_client can point at either one without knowing
// the difference. Internally, instead of storing data itself, it picks a
// backend node for each key (via ConsistentHashRing) and forwards the
// request there.
class CoordinatorServiceImpl final : public kv::KeyValueStore::Service {
 public:
  explicit CoordinatorServiceImpl(const std::vector<std::string>& node_addresses) {
    for (const auto& address : node_addresses) {
      ring_.AddNode(address);
      auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
      node_stubs_[address] = kv::KeyValueStore::NewStub(channel);
    }
  }

  grpc::Status Get(grpc::ServerContext* context, const kv::GetRequest* request,
                    kv::GetResponse* response) override {
    grpc::ClientContext node_ctx;
    return StubFor(request->key())->Get(&node_ctx, *request, response);
  }

  grpc::Status Put(grpc::ServerContext* context, const kv::PutRequest* request,
                    kv::PutResponse* response) override {
    grpc::ClientContext node_ctx;
    return StubFor(request->key())->Put(&node_ctx, *request, response);
  }

  grpc::Status Delete(grpc::ServerContext* context, const kv::DeleteRequest* request,
                       kv::DeleteResponse* response) override {
    grpc::ClientContext node_ctx;
    return StubFor(request->key())->Delete(&node_ctx, *request, response);
  }

 private:
  kv::KeyValueStore::Stub* StubFor(const std::string& key) {
    std::string address = ring_.GetNode(key);
    std::cout << "route: key='" << key << "' -> " << address << "\n";
    return node_stubs_[address].get();
  }

  kv::ConsistentHashRing ring_;
  std::unordered_map<std::string, std::unique_ptr<kv::KeyValueStore::Stub>> node_stubs_;
};

int main(int argc, char** argv) {
  // argv: kv_coordinator <listen_address> <node1_addr> <node2_addr> ...
  if (argc < 3) {
    std::cerr << "usage: " << argv[0] << " <listen_address> <node_addr>... (at least one node)\n";
    return 1;
  }

  std::string listen_address = argv[1];
  std::vector<std::string> node_addresses;
  for (int i = 2; i < argc; ++i) {
    node_addresses.emplace_back(argv[i]);
  }

  CoordinatorServiceImpl service(node_addresses);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  std::cout << "kv_coordinator listening on " << listen_address << ", routing across "
            << node_addresses.size() << " node(s)\n";
  server->Wait();

  return 0;
}
