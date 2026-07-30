#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "kv.grpc.pb.h"

// Network-level version of kv_store_demo.cpp's stress test. Instead of
// threads inside one process calling KVStore directly, this spawns threads
// that each act like an independent client, sending real gRPC requests
// over a TCP connection to a running kv_server. This exercises the whole
// path -- gRPC's server-side thread pool dispatching concurrent RPCs into
// KeyValueStoreServiceImpl, which calls into the same shared_mutex-guarded
// KVStore -- the thing step 2 could only assert, not prove.
//
// Usage: kv_stress_test [server_address]
//   (kv_server must already be running and listening on that address)

namespace {

constexpr int kNumWriterThreads = 4;
constexpr int kNumReaderThreads = 4;
constexpr int kOpsPerThread = 2000;

class Client {
 public:
  explicit Client(const std::shared_ptr<grpc::Channel>& channel)
      : stub_(kv::KeyValueStore::NewStub(channel)) {}

  bool Put(const std::string& key, const std::string& value) {
    kv::PutRequest request;
    request.set_key(key);
    request.set_value(value);
    kv::PutResponse response;
    grpc::ClientContext context;
    return stub_->Put(&context, request, &response).ok();
  }

  bool Get(const std::string& key, std::string* value_out, bool* found_out) {
    kv::GetRequest request;
    request.set_key(key);
    kv::GetResponse response;
    grpc::ClientContext context;
    grpc::Status status = stub_->Get(&context, request, &response);
    if (!status.ok()) return false;
    *found_out = response.found();
    if (response.found()) *value_out = response.value();
    return true;
  }

 private:
  std::unique_ptr<kv::KeyValueStore::Stub> stub_;
};

}  // namespace

int main(int argc, char** argv) {
  std::string target = "localhost:50051";
  if (argc > 1) target = argv[1];

  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  if (!channel->WaitForConnected(
          std::chrono::system_clock::now() + std::chrono::seconds(3))) {
    std::cerr << "Could not connect to kv_server at " << target
              << " -- start it first with ./build/kv_server\n";
    return 1;
  }

  std::atomic<long> total_ops{0};
  std::atomic<long> rpc_errors{0};
  std::atomic<long> total_gets{0};
  std::atomic<long> total_hits{0};

  auto writer_fn = [&](int thread_id) {
    Client client(channel);  // each thread gets its own stub (shared channel)
    for (int i = 0; i < kOpsPerThread; ++i) {
      std::string key = "key-" + std::to_string(thread_id) + "-" + std::to_string(i % 100);
      if (!client.Put(key, "value-" + std::to_string(i))) {
        rpc_errors.fetch_add(1, std::memory_order_relaxed);
      }
      total_ops.fetch_add(1, std::memory_order_relaxed);
    }
  };

  auto reader_fn = [&](int thread_id) {
    Client client(channel);
    for (int i = 0; i < kOpsPerThread; ++i) {
      std::string key = "key-" + std::to_string(thread_id % kNumWriterThreads) + "-" +
                         std::to_string(i % 100);
      std::string value;
      bool found = false;
      if (!client.Get(key, &value, &found)) {
        rpc_errors.fetch_add(1, std::memory_order_relaxed);
      } else {
        total_gets.fetch_add(1, std::memory_order_relaxed);
        if (found) total_hits.fetch_add(1, std::memory_order_relaxed);
      }
      total_ops.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  auto start = std::chrono::steady_clock::now();

  for (int t = 0; t < kNumWriterThreads; ++t) threads.emplace_back(writer_fn, t);
  for (int t = 0; t < kNumReaderThreads; ++t) threads.emplace_back(reader_fn, t);
  for (auto& th : threads) th.join();

  auto end = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(end - start).count();

  std::cout << "Target: " << target << "\n";
  std::cout << "Threads: " << kNumWriterThreads << " writers + " << kNumReaderThreads
            << " readers\n";
  std::cout << "Total ops: " << total_ops.load() << " in " << seconds << "s ("
            << (total_ops.load() / seconds) << " ops/sec)\n";
  std::cout << "GETs: " << total_gets.load() << ", hits: " << total_hits.load() << "\n";
  std::cout << "RPC errors: " << rpc_errors.load() << "\n";

  if (rpc_errors.load() > 0) {
    std::cerr << "FAIL: " << rpc_errors.load() << " RPCs failed\n";
    return 1;
  }
  std::cout << "PASS: no RPC errors under concurrent load\n";
  return 0;
}
