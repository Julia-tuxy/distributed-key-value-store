// Multithreaded benchmark suite: N worker threads hammer one shared
// Coordinator instance with a configurable mix of GET/PUT operations,
// and we report throughput plus p50/p90/p99 latency. Because every worker
// thread shares the same Coordinator (and thus the same gRPC channels to
// every storage node), this doubles as a concurrency stress test of the
// coordinator's failover logic and the storage nodes' sharded locking.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "cluster_config.h"
#include "coordinator.h"

namespace {

struct BenchmarkConfig {
  std::string nodes_csv;
  int threads = 8;
  int ops_per_thread = 5000;
  double read_ratio = 0.8;
  int keyspace = 10000;
  int value_size = 64;
  // When set, this run represents one independent client process (its own
  // Coordinator, its own gRPC connections) among possibly several launched
  // in parallel by scripts/scalability_sweep.sh. Its raw latencies and
  // summary get written to result_dir so the sweep script can merge many
  // client processes' results into one global percentile/throughput figure.
  std::string client_id;
  std::string result_dir;
};

BenchmarkConfig ParseArgs(int argc, char** argv) {
  BenchmarkConfig cfg;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto value_after = [&](const std::string& prefix) { return arg.substr(prefix.size()); };
    if (arg.rfind("--nodes=", 0) == 0) {
      cfg.nodes_csv = value_after("--nodes=");
    } else if (arg.rfind("--threads=", 0) == 0) {
      cfg.threads = std::stoi(value_after("--threads="));
    } else if (arg.rfind("--ops-per-thread=", 0) == 0) {
      cfg.ops_per_thread = std::stoi(value_after("--ops-per-thread="));
    } else if (arg.rfind("--read-ratio=", 0) == 0) {
      cfg.read_ratio = std::stod(value_after("--read-ratio="));
    } else if (arg.rfind("--keyspace=", 0) == 0) {
      cfg.keyspace = std::stoi(value_after("--keyspace="));
    } else if (arg.rfind("--value-size=", 0) == 0) {
      cfg.value_size = std::stoi(value_after("--value-size="));
    } else if (arg.rfind("--client-id=", 0) == 0) {
      cfg.client_id = value_after("--client-id=");
    } else if (arg.rfind("--result-dir=", 0) == 0) {
      cfg.result_dir = value_after("--result-dir=");
    }
  }
  return cfg;
}

std::string RandomValue(std::mt19937& rng, int size) {
  static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  std::uniform_int_distribution<int> dist(0, sizeof(kAlphabet) - 2);
  std::string out(size, '0');
  for (auto& c : out) c = kAlphabet[dist(rng)];
  return out;
}

struct WorkerResult {
  std::vector<double> latencies_us;
  long successes = 0;
  long errors = 0;
};

WorkerResult RunWorker(kv::Coordinator& coordinator, const BenchmarkConfig& cfg, int worker_id) {
  WorkerResult result;
  result.latencies_us.reserve(cfg.ops_per_thread);

  std::mt19937 rng(std::random_device{}() + worker_id);
  std::uniform_int_distribution<int> key_dist(0, cfg.keyspace - 1);
  std::uniform_real_distribution<double> op_dist(0.0, 1.0);

  // Pre-seed a value so early reads aren't all misses.
  coordinator.Put("bench:0", RandomValue(rng, cfg.value_size));

  for (int i = 0; i < cfg.ops_per_thread; ++i) {
    std::string key = "bench:" + std::to_string(key_dist(rng));
    bool is_read = op_dist(rng) < cfg.read_ratio;

    auto start = std::chrono::steady_clock::now();
    bool ok;
    if (is_read) {
      std::string value_out;
      coordinator.Get(key, &value_out);
      ok = true;  // a miss is still a successful round trip
    } else {
      ok = coordinator.Put(key, RandomValue(rng, cfg.value_size));
    }
    auto end = std::chrono::steady_clock::now();

    double micros = std::chrono::duration<double, std::micro>(end - start).count();
    result.latencies_us.push_back(micros);
    if (ok) {
      ++result.successes;
    } else {
      ++result.errors;
    }
  }
  return result;
}

double Percentile(std::vector<double>& sorted_latencies, double p) {
  if (sorted_latencies.empty()) return 0.0;
  size_t idx = static_cast<size_t>(p * (sorted_latencies.size() - 1));
  return sorted_latencies[idx];
}

}  // namespace

int main(int argc, char** argv) {
  BenchmarkConfig cfg = ParseArgs(argc, argv);
  if (cfg.nodes_csv.empty()) {
    std::cerr << "usage: kv_benchmark --nodes=node1@127.0.0.1:50051,... "
                 "[--threads=8] [--ops-per-thread=5000] [--read-ratio=0.8] "
                 "[--keyspace=10000] [--value-size=64] "
                 "[--client-id=ID --result-dir=DIR]\n"
                 "  --client-id/--result-dir mark this run as one of several\n"
                 "  independent client processes (see scripts/scalability_sweep.sh)\n"
                 "  and dump raw results to DIR for cross-process aggregation.\n";
    return 1;
  }

  std::string label = cfg.client_id.empty() ? "" : ("[client " + cfg.client_id + "] ");

  kv::Coordinator coordinator(kv::ParseNodeList(cfg.nodes_csv));
  // Give the health-check thread one round trip before we start timing so a
  // node that's already down at boot doesn't eat into request latency.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  std::cout << label << "starting benchmark: " << cfg.threads << " threads x " << cfg.ops_per_thread
            << " ops, read_ratio=" << cfg.read_ratio << ", keyspace=" << cfg.keyspace << "\n";

  std::vector<std::thread> workers;
  std::vector<WorkerResult> results(cfg.threads);

  auto wall_start = std::chrono::steady_clock::now();
  for (int t = 0; t < cfg.threads; ++t) {
    workers.emplace_back([&, t]() { results[t] = RunWorker(coordinator, cfg, t); });
  }
  for (auto& w : workers) w.join();
  auto wall_end = std::chrono::steady_clock::now();

  std::vector<double> all_latencies;
  long total_successes = 0, total_errors = 0;
  for (auto& r : results) {
    all_latencies.insert(all_latencies.end(), r.latencies_us.begin(), r.latencies_us.end());
    total_successes += r.successes;
    total_errors += r.errors;
  }
  std::sort(all_latencies.begin(), all_latencies.end());

  double wall_seconds = std::chrono::duration<double>(wall_end - wall_start).count();
  long total_ops = total_successes + total_errors;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "\n" << label << "--- results ---\n";
  std::cout << label << "total ops:      " << total_ops << "\n";
  std::cout << label << "successes:      " << total_successes << "\n";
  std::cout << label << "errors:         " << total_errors << "\n";
  std::cout << label << "wall time:      " << wall_seconds << " s\n";
  std::cout << label << "throughput:     " << (total_ops / wall_seconds) << " ops/sec\n";
  std::cout << label << "latency p50:    " << Percentile(all_latencies, 0.50) / 1000.0 << " ms\n";
  std::cout << label << "latency p90:    " << Percentile(all_latencies, 0.90) / 1000.0 << " ms\n";
  std::cout << label << "latency p99:    " << Percentile(all_latencies, 0.99) / 1000.0 << " ms\n";
  std::cout << label << "healthy nodes:  ";
  for (auto& n : coordinator.HealthyNodes()) std::cout << n << " ";
  std::cout << "\n";

  // Dump raw per-op latencies plus a summary line so an external script can
  // merge results from several independent client processes (each with its
  // own connections to the cluster) into one global throughput/percentile
  // figure - see scripts/scalability_sweep.sh.
  if (!cfg.result_dir.empty() && !cfg.client_id.empty()) {
    std::ofstream latencies_file(cfg.result_dir + "/client_" + cfg.client_id + "_latencies.txt");
    for (double us : all_latencies) latencies_file << us << "\n";

    std::ofstream summary_file(cfg.result_dir + "/client_" + cfg.client_id + "_summary.txt");
    summary_file << total_successes << "," << total_errors << "," << wall_seconds << "\n";
  }
  return 0;
}
