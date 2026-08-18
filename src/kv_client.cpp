// Interactive/one-shot CLI client. Links the Coordinator directly (no
// separate "coordinator process" - the coordinator is a library, and any
// number of client processes can each run their own instance against the
// same cluster).
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "cluster_config.h"
#include "coordinator.h"

namespace {

void PrintUsage() {
  std::cout << "commands: get <key> | put <key> <value> | del <key> | status | quit\n";
}

void RunOneShot(kv::Coordinator& coord, const std::vector<std::string>& args) {
  const std::string& cmd = args[0];
  if (cmd == "get" && args.size() == 2) {
    std::string value;
    if (coord.Get(args[1], &value)) {
      std::cout << value << "\n";
    } else {
      std::cout << "(not found)\n";
    }
  } else if (cmd == "put" && args.size() == 3) {
    std::cout << (coord.Put(args[1], args[2]) ? "OK\n" : "FAILED (no write quorum)\n");
  } else if (cmd == "del" && args.size() == 2) {
    bool existed = false;
    bool ok = coord.Delete(args[1], &existed);
    std::cout << (ok ? "OK" : "FAILED") << " existed=" << (existed ? "true" : "false") << "\n";
  } else if (cmd == "status") {
    std::cout << "healthy nodes:";
    for (auto& n : coord.HealthyNodes()) std::cout << " " << n;
    std::cout << "\n";
  } else {
    PrintUsage();
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string nodes_csv;
  std::vector<std::string> command_args;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--nodes=", 0) == 0) {
      nodes_csv = arg.substr(8);
    } else {
      command_args.push_back(arg);
    }
  }

  if (nodes_csv.empty()) {
    std::cerr << "usage: kv_client --nodes=node1@127.0.0.1:50051,node2@127.0.0.1:50052,... "
                 "[get <key> | put <key> <value> | del <key>]\n";
    return 1;
  }

  kv::Coordinator coordinator(kv::ParseNodeList(nodes_csv));

  if (!command_args.empty()) {
    RunOneShot(coordinator, command_args);
    return 0;
  }

  std::cout << "connected. type 'help' for commands, 'quit' to exit.\n";
  std::string line;
  while (true) {
    std::cout << "> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    std::istringstream iss(line);
    std::vector<std::string> args{std::istream_iterator<std::string>{iss},
                                   std::istream_iterator<std::string>{}};
    if (args.empty()) continue;
    if (args[0] == "quit" || args[0] == "exit") break;
    if (args[0] == "help") {
      PrintUsage();
      continue;
    }
    RunOneShot(coordinator, args);
  }
  return 0;
}
