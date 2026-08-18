#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "coordinator.h"

namespace kv {

// Parses "--nodes=node1@127.0.0.1:50051,node2@127.0.0.1:50052,..." (or bare
// "host:port" entries, which get auto-named node1, node2, ...) into the
// NodeConfig list the Coordinator expects. This is the cluster's static
// membership list - simple by design, since the resume project's failure
// model is "detect and route around a down node", not "grow/shrink the
// membership list itself".
inline std::vector<NodeConfig> ParseNodeList(const std::string& csv) {
  std::vector<NodeConfig> nodes;
  std::stringstream ss(csv);
  std::string entry;
  int auto_index = 1;
  while (std::getline(ss, entry, ',')) {
    if (entry.empty()) continue;
    auto at = entry.find('@');
    NodeConfig cfg;
    if (at == std::string::npos) {
      cfg.id = "node" + std::to_string(auto_index++);
      cfg.address = entry;
    } else {
      cfg.id = entry.substr(0, at);
      cfg.address = entry.substr(at + 1);
    }
    nodes.push_back(cfg);
  }
  return nodes;
}

}  // namespace kv
