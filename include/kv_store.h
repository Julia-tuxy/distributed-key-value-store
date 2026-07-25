#pragma once

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace kv {

// Thread-safe in-memory key-value store.
//
// Concurrency model: a single std::shared_mutex protects the whole map.
//   - Get()    takes a shared (read) lock -> many readers run concurrently.
//   - Put/Delete take a unique (write) lock -> exclusive access.
// This is coarse-grained locking (one lock for the entire map, not per-key
// or sharded). It's the simplest correct design and a fine baseline to
// benchmark later against a sharded-lock version.
class KVStore {
 public:
  KVStore() = default;

  // Returns the value for `key`, or std::nullopt if not present.
  std::optional<std::string> Get(const std::string& key) const;

  // Inserts or overwrites `key` -> `value`.
  void Put(const std::string& key, const std::string& value);

  // Removes `key`. Returns true if the key existed.
  bool Delete(const std::string& key);

  // Number of keys currently stored.
  size_t Size() const;

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::string> data_;
};

}  // namespace kv
