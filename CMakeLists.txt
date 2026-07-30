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
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

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
  COMMAND protobuf::protoc
  ARGS --cpp_out=${PROTO_GEN_DIR}
       --grpc_out=${PROTO_GEN_DIR}
       --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
       -I ${CMAKE_SOURCE_DIR}/proto
       ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
  COMMENT "Generating gRPC code from kv.proto")

add_library(kv_proto ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS})
target_include_directories(kv_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(kv_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

# --- gRPC server: wraps KVStore behind the KeyValueStore service ---
add_executable(kv_server src/kv_server.cpp)
target_link_libraries(kv_server PRIVATE kv_store kv_proto)

# --- gRPC client: interactive REPL that talks to kv_server over the network ---
add_executable(kv_client src/kv_client.cpp)
target_link_libraries(kv_client PRIVATE kv_proto)

# --- Concurrency stress test: many threads hitting kv_server over gRPC at once ---
add_executable(kv_stress_test src/kv_stress_test.cpp)
target_link_libraries(kv_stress_test PRIVATE kv_proto)
