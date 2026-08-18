#include "consistent_hash.h"

#include <algorithm>
#include <set>

namespace kv {

// FNV-1a 64-bit. Simple, dependency-free, and deterministic across runs so
// every coordinator/client instance builds the exact same ring for the
// same node list.
uint64_t ConsistentHashRing::Hash(const std::string& s) {
  uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
  for (unsigned char c : s) {
    hash ^= c;
    hash *= 1099511628211ULL;  // FNV prime
  }
  return hash;
}

ConsistentHashRing::ConsistentHashRing(int virtual_nodes_per_physical)
    : virtual_nodes_per_physical_(virtual_nodes_per_physical) {}

void ConsistentHashRing::AddNode(const std::string& node_id) {
  for (int i = 0; i < virtual_nodes_per_physical_; ++i) {
    uint64_t point = Hash(node_id + "#" + std::to_string(i));
    ring_[point] = node_id;
  }
  physical_nodes_.push_back(node_id);
}

void ConsistentHashRing::RemoveNode(const std::string& node_id) {
  for (auto it = ring_.begin(); it != ring_.end();) {
    if (it->second == node_id) {
      it = ring_.erase(it);
    } else {
      ++it;
    }
  }
  physical_nodes_.erase(
      std::remove(physical_nodes_.begin(), physical_nodes_.end(), node_id),
      physical_nodes_.end());
}

std::vector<std::string> ConsistentHashRing::GetPreferenceList(const std::string& key,
                                                                int count) const {
  std::vector<std::string> result;
  if (ring_.empty()) return result;

  uint64_t point = Hash(key);
  auto it = ring_.lower_bound(point);

  std::set<std::string> seen;
  size_t scanned = 0;
  size_t ring_size = ring_.size();

  // Walk clockwise around the ring, wrapping at the end, collecting
  // distinct physical node ids until we have `count` of them or we've
  // scanned every point on the ring (fewer physical nodes than requested).
  while (result.size() < static_cast<size_t>(count) && scanned < ring_size) {
    if (it == ring_.end()) it = ring_.begin();
    const std::string& node_id = it->second;
    if (seen.insert(node_id).second) {
      result.push_back(node_id);
    }
    ++it;
    ++scanned;
  }
  return result;
}

}  // namespace kv
