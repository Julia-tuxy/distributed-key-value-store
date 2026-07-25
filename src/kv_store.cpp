#include "kv_store.h"

namespace kv {

std::optional<std::string> KVStore::Get(const std::string& key) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  auto it = data_.find(key);
  if (it == data_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void KVStore::Put(const std::string& key, const std::string& value) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  data_[key] = value;
}

bool KVStore::Delete(const std::string& key) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  return data_.erase(key) > 0;
}

size_t KVStore::Size() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return data_.size();
}

}  // namespace kv
