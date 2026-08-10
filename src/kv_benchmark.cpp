#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "kv.grpc.pb.h"

// Benchmark suite: runs a mixed read/write workload against a target
// (a single kv_server, or a kv_coordinator fronting a whole cluster) at
// increasing client concurrency, and reports throughput plus latency
// percentiles at each level. The point of sweeping concurrency instead of
// running once is to see *how the system degrades* under load -- e.g.
// throughput flattening out while p99 latency climbs is the signature of
// a system approaching a bottleneck (lock contention, a single node's
// CPU, network saturation), which raw "ops/sec" alone would hide.
//
// Usage: kv_benchmark [target] [thread_counts_csv] [ops_per_thread]
//   target            default "localhost:60000" (a kv_coordinator)
//   thread_counts_csv default "1,2,4,8,16,32"
//   ops_per_thread    default 3000

namespace {

constexpr int kKeyPoolSize = 200;   // operations sample from this fixed key set
constexpr double kWriteRatio = 0.2;  // 20% PUT, 80% GET -- a read-heavy mix

struct RunResult {
  int thread_count = 0;
  long total_ops = 0;
  double seconds = 0;
  double throughput = 0;
  double p50_us = 0, p95_us = 0, p99_us = 0, max_us = 0;
  long errors = 0;
};

std::vector<int> ParseThreadCounts(const std::string& csv) {
  std::vector<int> result;
  std::stringstream ss(csv);
  std::string token;
  while (std::getline(ss, token, ',')) {
    if (!token.empty()) result.push_back(std::stoi(token));
  }
  return result;
}

// Nearest-rank percentile over an already-sorted vector.
double Percentile(const std::vector<double>& sorted_us, double p) {
  if (sorted_us.empty()) return 0.0;
  size_t idx = static_cast<size_t>(p * (sorted_us.size() - 1));
  return sorted_us[idx];
}

void SeedKeys(const std::shared_ptr<grpc::Channel>& channel) {
  auto stub = kv::KeyValueStore::NewStub(channel);
  for (int i = 0; i < kKeyPoolSize; ++i) {
    kv::PutRequest request;
    request.set_key("bench-key-" + std::to_string(i));
    request.set_value("seed");
    kv::PutResponse response;
    grpc::ClientContext context;
    stub->Put(&context, request, &response);
  }
}

RunResult RunLevel(const std::shared_ptr<grpc::Channel>& channel, int thread_count,
                    int ops_per_thread) {
  std::vector<std::vector<double>> latencies_us(thread_count);
  std::atomic<long> errors{0};

  auto worker = [&](int thread_id) {
    auto stub = kv::KeyValueStore::NewStub(channel);  // one stub per client thread
    std::mt19937 rng(thread_id * 7919 + 1);
    std::uniform_int_distribution<int> key_dist(0, kKeyPoolSize - 1);
    std::uniform_real_distribution<double> op_dist(0.0, 1.0);
    latencies_us[thread_id].reserve(ops_per_thread);

    for (int i = 0; i < ops_per_thread; ++i) {
      std::string key = "bench-key-" + std::to_string(key_dist(rng));
      bool is_write = op_dist(rng) < kWriteRatio;

      grpc::ClientContext context;
      grpc::Status status;
      auto op_start = std::chrono::steady_clock::now();
      if (is_write) {
        kv::PutRequest request;
        request.set_key(key);
        request.set_value("v" + std::to_string(i));
        kv::PutResponse response;
        status = stub->Put(&context, request, &response);
      } else {
        kv::GetRequest request;
        request.set_key(key);
        kv::GetResponse response;
        status = stub->Get(&context, request, &response);
      }
      auto op_end = std::chrono::steady_clock::now();

      if (!status.ok()) {
        errors.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      latencies_us[thread_id].push_back(
          std::chrono::duration<double, std::micro>(op_end - op_start).count());
    }
  };

  std::vector<std::thread> threads;
  auto wall_start = std::chrono::steady_clock::now();
  for (int t = 0; t < thread_count; ++t) threads.emplace_back(worker, t);
  for (auto& th : threads) th.join();
  auto wall_end = std::chrono::steady_clock::now();

  std::vector<double> all_latencies;
  for (auto& v : latencies_us) all_latencies.insert(all_latencies.end(), v.begin(), v.end());
  std::sort(all_latencies.begin(), all_latencies.end());

  RunResult result;
  result.thread_count = thread_count;
  result.total_ops = static_cast<long>(thread_count) * ops_per_thread;
  result.seconds = std::chrono::duration<double>(wall_end - wall_start).count();
  result.throughput = result.total_ops / result.seconds;
  result.p50_us = Percentile(all_latencies, 0.50);
  result.p95_us = Percentile(all_latencies, 0.95);
  result.p99_us = Percentile(all_latencies, 0.99);
  result.max_us = all_latencies.empty() ? 0.0 : all_latencies.back();
  result.errors = errors.load();
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  std::string target = argc > 1 ? argv[1] : "localhost:60000";
  std::string thread_counts_csv = argc > 2 ? argv[2] : "1,2,4,8,16,32";
  int ops_per_thread = argc > 3 ? std::atoi(argv[3]) : 3000;

  std::vector<int> thread_counts = ParseThreadCounts(thread_counts_csv);
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  if (!channel->WaitForConnected(
          std::chrono::system_clock::now() + std::chrono::seconds(3))) {
    std::cerr << "Could not connect to " << target << "\n";
    return 1;
  }

  std::cout << "Seeding " << kKeyPoolSize << " keys on " << target << "...\n";
  SeedKeys(channel);

  std::cout << "\nWorkload: " << static_cast<int>(kWriteRatio * 100) << "% PUT / "
            << static_cast<int>((1 - kWriteRatio) * 100) << "% GET, " << ops_per_thread
            << " ops/thread\n\n";

  std::cout << std::left << std::setw(9) << "threads" << std::right << std::setw(12) << "ops/sec"
            << std::setw(10) << "p50(us)" << std::setw(10) << "p95(us)" << std::setw(10)
            << "p99(us)" << std::setw(10) << "max(us)" << std::setw(9) << "errors" << "\n";

  for (int tc : thread_counts) {
    RunResult r = RunLevel(channel, tc, ops_per_thread);
    std::cout << std::left << std::setw(9) << r.thread_count << std::right << std::setw(12)
               << std::fixed << std::setprecision(0) << r.throughput << std::setw(10) << r.p50_us
               << std::setw(10) << r.p95_us << std::setw(10) << r.p99_us << std::setw(10)
               << r.max_us << std::setw(9) << r.errors << "\n";
  }

  return 0;
}
