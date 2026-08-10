cmake_minimum_required(VERSION 3.16)
project(distributed_kv_store CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Usage: cmake -B build -DTSAN_BUILD=ON   (catches data races at runtime)
option(TSAN_BUILD "Build with ThreadSanitizer" OFF)
if(TSAN_BUILD)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# Located via pkg-config rather than CMake's Config-mode find_package.
# Homebrew's grpc/protobuf ship both, but Debian/Ubuntu's apt packages
# (used in Dockerfile) only ship .pc files, not *Config.cmake -- pkg-config
# is the one discovery mechanism both platforms support, so building this
# way works identically on a Mac and inside the Docker image.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)

find_program(PROTOC_EXECUTABLE NAMES protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin REQUIRED)

# --- Core KV store library (no networking) ---
add_library(kv_store src/kv_store.cpp)
target_include_directories(kv_store PUBLIC include)
target_link_libraries(kv_store PUBLIC Threads::Threads)

# --- Generate C++ code from proto/kv.proto ---
# protoc reads kv.proto and, via the grpc_cpp_plugin, emits four files:
#   kv.pb.h/.cc       -- message classes (GetRequest, PutResponse, ...)
#   kv.grpc.pb.h/.cc  -- service base classes (KeyValueStore::Service, ::Stub)
set(PROTO_FILE ${CMAKE_SOURCE_DIR}/proto/kv.proto)
set(PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_SRCS ${PROTO_GEN_DIR}/kv.pb.cc)
set(PROTO_HDRS ${PROTO_GEN_DIR}/kv.pb.h)
set(GRPC_SRCS ${PROTO_GEN_DIR}/kv.grpc.pb.cc)
set(GRPC_HDRS ${PROTO_GEN_DIR}/kv.grpc.pb.h)

add_custom_command(
  OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
  COMMAND ${PROTOC_EXECUTABLE}
  ARGS --cpp_out=${PROTO_GEN_DIR}
       --grpc_out=${PROTO_GEN_DIR}
       --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
       -I ${CMAKE_SOURCE_DIR}/proto
       ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
  COMMENT "Generating gRPC code from kv.proto")

add_library(kv_proto ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS})
target_include_directories(kv_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(kv_proto PUBLIC PkgConfig::PROTOBUF PkgConfig::GRPCPP)

# --- gRPC server: wraps KVStore behind the KeyValueStore service ---
add_executable(kv_server src/kv_server.cpp)
target_link_libraries(kv_server PRIVATE kv_store kv_proto)

# --- gRPC client: interactive REPL that talks to kv_server over the network ---
add_executable(kv_client src/kv_client.cpp)
target_link_libraries(kv_client PRIVATE kv_proto)

# --- Concurrency stress test: many threads hitting kv_server over gRPC at once ---
add_executable(kv_stress_test src/kv_stress_test.cpp)
target_link_libraries(kv_stress_test PRIVATE kv_proto)

# --- Consistent hashing ring, used by the coordinator to place keys on nodes ---
add_library(kv_hash src/consistent_hash.cpp)
target_include_directories(kv_hash PUBLIC include)

# --- Coordinator: routes requests across multiple kv_server nodes ---
add_executable(kv_coordinator src/kv_coordinator.cpp)
target_link_libraries(kv_coordinator PRIVATE kv_proto kv_hash)

# --- Standalone demo: modulo hashing vs consistent hashing key movement ---
add_executable(consistent_hash_demo src/consistent_hash_demo.cpp)
target_link_libraries(consistent_hash_demo PRIVATE kv_hash)

# --- Benchmark suite: throughput/latency vs. concurrency ---
add_executable(kv_benchmark src/kv_benchmark.cpp)
target_link_libraries(kv_benchmark PRIVATE kv_proto)
