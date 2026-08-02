#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "consistent_hash.h"

// Quantifies the exact problem consistent hashing solves: adding a node
// to a 3-node cluster, how many of 10,000 keys have to move to a
// different node under naive `hash(key) % N` versus under a
// ConsistentHashRing?

namespace {

std::vector<std::string> GenerateKeys(int count) {
  std::vector<std::string> keys;
  keys.reserve(count);
  for (int i = 0; i < count; ++i) {
    keys.push_back("key-" + std::to_string(i));
  }
  return keys;
}

// Same "hash(key) % N" scheme kv_coordinator used before this step.
size_t ModuloNode(const std::string& key, size_t num_nodes) {
  return std::hash<std::string>{}(key) % num_nodes;
}

}  // namespace

int main() {
  constexpr int kNumKeys = 10000;
  std::vector<std::string> keys = GenerateKeys(kNumKeys);

  std::vector<std::string> nodes_before = {"node-A", "node-B", "node-C"};
  std::vector<std::string> nodes_after = {"node-A", "node-B", "node-C", "node-D"};

  // --- Naive modulo hashing ---
  int modulo_moved = 0;
  for (const auto& key : keys) {
    size_t before = ModuloNode(key, nodes_before.size());
    size_t after = ModuloNode(key, nodes_after.size());
    if (nodes_before[before] != nodes_after[after]) {
      ++modulo_moved;
    }
  }

  // --- Consistent hashing ---
  kv::ConsistentHashRing ring_before;
  for (const auto& n : nodes_before) ring_before.AddNode(n);

  kv::ConsistentHashRing ring_after;
  for (const auto& n : nodes_after) ring_after.AddNode(n);

  int consistent_moved = 0;
  for (const auto& key : keys) {
    if (ring_before.GetNode(key) != ring_after.GetNode(key)) {
      ++consistent_moved;
    }
  }

  auto pct = [&](int moved) { return 100.0 * moved / kNumKeys; };

  std::cout << "Adding a 4th node to a 3-node cluster, " << kNumKeys << " keys:\n\n";
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "  hash(key) % N       : " << modulo_moved << " keys moved (" << pct(modulo_moved)
            << "%)\n";
  std::cout << "  consistent hashing  : " << consistent_moved << " keys moved ("
            << pct(consistent_moved) << "%)\n\n";
  std::cout << "Theoretical ideal for consistent hashing when going from N to N+1 nodes "
               "is about 1/(N+1) = "
            << pct(kNumKeys / static_cast<int>(nodes_after.size())) << "%.\n";

  return 0;
}
