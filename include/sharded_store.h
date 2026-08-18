#pragma once

#include <array>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace kv {

// Thread-safe in-memory key-value store used by each storage node process.
//
// Instead of one giant mutex around one big map (which would serialize
// every single request through a single lock), the keyspace is split into
// N independent shards, each with its own std::shared_mutex. Two requests
// that land on different shards run fully in parallel; two reads on the
// same shard run in parallel via the shared (read) lock; only a write
// needs the exclusive lock, and only for its own shard.
class ShardedStore {
 public:
  static constexpr size_t kShardCount = 16;

  bool Get(const std::string& key, std::string* value_out) const {
    const Shard& shard = ShardFor(key);
    std::shared_lock lock(shard.mutex);
    auto it = shard.map.find(key);
    if (it == shard.map.end()) return false;
    *value_out = it->second;
    return true;
  }

  void Put(const std::string& key, const std::string& value) {
    Shard& shard = ShardFor(key);
    std::unique_lock lock(shard.mutex);
    shard.map[key] = value;
  }

  // Returns true if the key existed and was removed.
  bool Delete(const std::string& key) {
    Shard& shard = ShardFor(key);
    std::unique_lock lock(shard.mutex);
    return shard.map.erase(key) > 0;
  }

 private:
  struct Shard {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, std::string> map;
  };

  Shard& ShardFor(const std::string& key) {
    return shards_[std::hash<std::string>{}(key) % kShardCount];
  }
  const Shard& ShardFor(const std::string& key) const {
    return shards_[std::hash<std::string>{}(key) % kShardCount];
  }

  std::array<Shard, kShardCount> shards_;
};

}  // namespace kv
