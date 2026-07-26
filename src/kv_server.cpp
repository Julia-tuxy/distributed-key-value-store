#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kv.grpc.pb.h"
#include "kv_store.h"

// Implements the KeyValueStore RPC service by delegating to our step-1
// KVStore. gRPC dispatches incoming RPCs from a thread pool, so multiple
// clients' Get/Put/Delete calls can genuinely execute on different threads
// at the same time. This is exactly why KVStore's shared_mutex locking
// exists -- without it, this server would corrupt data_ under concurrent
// requests.
class KeyValueStoreServiceImpl final : public kv::KeyValueStore::Service {
 public:
  grpc::Status Get(grpc::ServerContext* /*context*/, const kv::GetRequest* request,
                    kv::GetResponse* response) override {
    auto value = store_.Get(request->key());
    if (value.has_value()) {
      response->set_found(true);
      response->set_value(*value);
    } else {
      response->set_found(false);
    }
    return grpc::Status::OK;
  }

  grpc::Status Put(grpc::ServerContext* /*context*/, const kv::PutRequest* request,
                    kv::PutResponse* response) override {
    store_.Put(request->key(), request->value());
    response->set_success(true);
    return grpc::Status::OK;
  }

  grpc::Status Delete(grpc::ServerContext* /*context*/, const kv::DeleteRequest* request,
                       kv::DeleteResponse* response) override {
    response->set_existed(store_.Delete(request->key()));
    return grpc::Status::OK;
  }

 private:
  kv::KVStore store_;
};

int main(int argc, char** argv) {
  std::string address = "0.0.0.0:50051";
  if (argc > 1) {
    address = argv[1];
  }

  KeyValueStoreServiceImpl service;

  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  std::cout << "kv_server listening on " << address << "\n";
  server->Wait();

  return 0;
}
