#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kv {

// A hash ring with virtual nodes: each physical node is hashed to many
// points on a 64-bit ring so that adding/removing a node only reshuffles a
// small, even slice of the keyspace instead of the whole thing.
class ConsistentHashRing {
 public:
  explicit ConsistentHashRing(int virtual_nodes_per_physical = 100);

  void AddNode(const std::string& node_id);
  void RemoveNode(const std::string& node_id);

  // Returns up to `count` distinct physical node ids, in ring order
  // starting from the point `key` hashes to. This is the full preference
  // list (primary first, then replicas) regardless of node liveness -
  // the coordinator filters out down nodes itself.
  std::vector<std::string> GetPreferenceList(const std::string& key, int count) const;

  size_t PhysicalNodeCount() const { return physical_nodes_.size(); }

  static uint64_t Hash(const std::string& s);

 private:
  int virtual_nodes_per_physical_;
  std::map<uint64_t, std::string> ring_;          // point -> physical node id
  std::vector<std::string> physical_nodes_;        // for PhysicalNodeCount()
};

}  // namespace kv
