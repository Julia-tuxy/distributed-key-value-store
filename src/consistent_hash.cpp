#include "consistent_hash.h"

#include <algorithm>

namespace kv {

// FNV-1a 64-bit. Chosen over std::hash<std::string> because std::hash's
// implementation is unspecified per-standard-library -- it isn't
// guaranteed to be stable across platforms, and some implementations
// randomize the seed per process for DoS resistance. Consistent hashing
// depends on the same key always mapping to the same ring position, so we
// want a fixed, deterministic function we control.
uint64_t ConsistentHashRing::Hash(const std::string& s) {
  uint64_t hash = 1469598103934665603ULL;  // FNV offset basis
  for (unsigned char c : s) {
    hash ^= c;
    hash *= 1099511628211ULL;  // FNV prime
  }
  return hash;
}

ConsistentHashRing::ConsistentHashRing(int virtual_nodes_per_node)
    : virtual_nodes_per_node_(virtual_nodes_per_node) {}

void ConsistentHashRing::AddNode(const std::string& node_address) {
  if (std::find(nodes_.begin(), nodes_.end(), node_address) != nodes_.end()) {
    return;  // already present
  }
  nodes_.push_back(node_address);
  for (int i = 0; i < virtual_nodes_per_node_; ++i) {
    uint64_t position = Hash(node_address + "#" + std::to_string(i));
    ring_[position] = node_address;
  }
}

void ConsistentHashRing::RemoveNode(const std::string& node_address) {
  auto it = std::find(nodes_.begin(), nodes_.end(), node_address);
  if (it == nodes_.end()) {
    return;
  }
  nodes_.erase(it);
  for (int i = 0; i < virtual_nodes_per_node_; ++i) {
    uint64_t position = Hash(node_address + "#" + std::to_string(i));
    ring_.erase(position);
  }
}

std::string ConsistentHashRing::GetNode(const std::string& key) const {
  uint64_t position = Hash(key);
  // First ring entry at or after `position`; wrap around to the smallest
  // entry if we ran off the end -- the ring is circular.
  auto it = ring_.lower_bound(position);
  if (it == ring_.end()) {
    it = ring_.begin();
  }
  return it->second;
}

}  // namespace kv
