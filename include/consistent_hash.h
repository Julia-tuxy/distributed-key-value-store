#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kv {

// Maps keys to nodes using consistent hashing with virtual nodes.
//
// The idea: instead of `hash(key) % num_nodes` (where changing num_nodes
// reshuffles almost every key), place both nodes and keys on a fixed
// circular hash space [0, 2^64). A key belongs to the first node found by
// walking clockwise from the key's position. Adding or removing a node
// then only affects the keys between that node and its predecessor on the
// ring -- everything else is untouched.
//
// Each physical node is inserted at many points on the ring ("virtual
// nodes"), not just one. With only one point per node, one node could end
// up owning a disproportionate arc of the ring just by chance. Spreading
// each node across ~100+ points averages that out so load balances evenly
// across physical nodes.
class ConsistentHashRing {
 public:
  explicit ConsistentHashRing(int virtual_nodes_per_node = 150);

  void AddNode(const std::string& node_address);
  void RemoveNode(const std::string& node_address);

  // Returns the address of the node responsible for `key`.
  // Precondition: at least one node has been added.
  std::string GetNode(const std::string& key) const;

  // Returns up to `count` distinct physical node addresses for `key`, in
  // ring order starting from the key's position (so element 0 is exactly
  // what GetNode would return). Used to build a key's replica set: the
  // first address is the primary, the rest are replicas. Returns fewer
  // than `count` addresses if there aren't that many distinct nodes.
  std::vector<std::string> GetNodes(const std::string& key, int count) const;

  size_t NodeCount() const { return nodes_.size(); }

 private:
  static uint64_t Hash(const std::string& s);

  int virtual_nodes_per_node_;
  std::map<uint64_t, std::string> ring_;  // hash position -> node address
  std::vector<std::string> nodes_;        // distinct physical nodes in the ring
};

}  // namespace kv
