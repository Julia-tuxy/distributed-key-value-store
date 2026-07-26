#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kv.grpc.pb.h"

// Interactive command-line client for kv_server. Unlike the old in-process
// REPL, every command here is a real network call: it serializes a request
// message, sends it over a TCP connection to the server, and deserializes
// the response. The data now lives in the *server* process's memory, so
// multiple kv_client instances (or terminals) talking to the same server
// all see the same data.
//
// Commands:
//   PUT <key> <value>
//   GET <key>
//   DELETE <key>
//   QUIT
class KeyValueStoreClient {
 public:
  explicit KeyValueStoreClient(const std::shared_ptr<grpc::Channel>& channel)
      : stub_(kv::KeyValueStore::NewStub(channel)) {}

  void Put(const std::string& key, const std::string& value) {
    kv::PutRequest request;
    request.set_key(key);
    request.set_value(value);
    kv::PutResponse response;
    grpc::ClientContext context;

    grpc::Status status = stub_->Put(&context, request, &response);
    if (!status.ok()) {
      std::cout << "RPC failed: " << status.error_message() << "\n";
      return;
    }
    std::cout << "OK\n";
  }

  void Get(const std::string& key) {
    kv::GetRequest request;
    request.set_key(key);
    kv::GetResponse response;
    grpc::ClientContext context;

    grpc::Status status = stub_->Get(&context, request, &response);
    if (!status.ok()) {
      std::cout << "RPC failed: " << status.error_message() << "\n";
      return;
    }
    std::cout << (response.found() ? response.value() : "(not found)") << "\n";
  }

  void Delete(const std::string& key) {
    kv::DeleteRequest request;
    request.set_key(key);
    kv::DeleteResponse response;
    grpc::ClientContext context;

    grpc::Status status = stub_->Delete(&context, request, &response);
    if (!status.ok()) {
      std::cout << "RPC failed: " << status.error_message() << "\n";
      return;
    }
    std::cout << (response.existed() ? "OK\n" : "(not found)\n");
  }

 private:
  std::unique_ptr<kv::KeyValueStore::Stub> stub_;
};

int main(int argc, char** argv) {
  std::string target = "localhost:50051";
  if (argc > 1) {
    target = argv[1];
  }

  KeyValueStoreClient client(
      grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));

  std::cout << "kv_client connected to " << target
            << " - commands: PUT <key> <value> | GET <key> | DELETE <key> | QUIT\n";

  std::string line;
  while (true) {
    std::cout << "> ";
    if (!std::getline(std::cin, line)) break;

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd.empty()) {
      continue;
    } else if (cmd == "PUT") {
      std::string key, value;
      iss >> key;
      std::getline(iss, value);
      if (!value.empty() && value[0] == ' ') value.erase(0, 1);
      if (key.empty() || value.empty()) {
        std::cout << "usage: PUT <key> <value>\n";
        continue;
      }
      client.Put(key, value);
    } else if (cmd == "GET") {
      std::string key;
      iss >> key;
      client.Get(key);
    } else if (cmd == "DELETE") {
      std::string key;
      iss >> key;
      client.Delete(key);
    } else if (cmd == "QUIT" || cmd == "EXIT") {
      break;
    } else {
      std::cout << "unknown command: " << cmd << "\n";
    }
  }

  return 0;
}
