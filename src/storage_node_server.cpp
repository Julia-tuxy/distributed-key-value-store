// Storage node: a standalone gRPC server process holding one shard of the
// cluster's data in memory. Run several of these on different ports to form
// a cluster; the coordinator (see coordinator.h) decides which node(s) own
// which keys and talks to them over gRPC.
#include <csignal>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "kv.grpc.pb.h"
#include "sharded_store.h"

namespace {

std::unique_ptr<grpc::Server> g_server;

void HandleShutdown(int) {
  if (g_server) g_server->Shutdown();
}

}  // namespace

class KVStoreServiceImpl final : public kv::KVStore::Service {
 public:
  explicit KVStoreServiceImpl(std::string node_id) : node_id_(std::move(node_id)) {}

  // grpc's sync server dispatches each incoming RPC onto a worker thread
  // from its internal thread pool, so Get/Put/Delete for different keys
  // (and even the same key) can run concurrently here. Correctness under
  // that concurrency comes entirely from ShardedStore's per-shard locking.
  grpc::Status Get(grpc::ServerContext*, const kv::GetRequest* request,
                    kv::GetResponse* response) override {
    std::string value;
    bool found = store_.Get(request->key(), &value);
    response->set_found(found);
    if (found) response->set_value(value);
    return grpc::Status::OK;
  }

  grpc::Status Put(grpc::ServerContext*, const kv::PutRequest* request,
                    kv::PutResponse* response) override {
    store_.Put(request->key(), request->value());
    response->set_success(true);
    return grpc::Status::OK;
  }

  grpc::Status Delete(grpc::ServerContext*, const kv::DeleteRequest* request,
                       kv::DeleteResponse* response) override {
    bool existed = store_.Delete(request->key());
    response->set_success(true);
    response->set_existed(existed);
    return grpc::Status::OK;
  }

  grpc::Status Ping(grpc::ServerContext*, const kv::PingRequest*,
                     kv::PingResponse* response) override {
    response->set_node_id(node_id_);
    return grpc::Status::OK;
  }

 private:
  std::string node_id_;
  kv::ShardedStore store_;
};

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: storage_node <node_id> <listen_address:port>\n"
              << "example: storage_node node1 0.0.0.0:50051\n";
    return 1;
  }
  std::string node_id = argv[1];
  std::string listen_address = argv[2];

  KVStoreServiceImpl service(node_id);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  g_server = builder.BuildAndStart();
  if (!g_server) {
    std::cerr << "failed to start server on " << listen_address << "\n";
    return 1;
  }

  std::signal(SIGINT, HandleShutdown);
  std::signal(SIGTERM, HandleShutdown);

  std::cout << "[" << node_id << "] listening on " << listen_address << std::endl;
  g_server->Wait();
  std::cout << "[" << node_id << "] shut down" << std::endl;
  return 0;
}
