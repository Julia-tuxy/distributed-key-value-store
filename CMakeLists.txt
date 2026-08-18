cmake_minimum_required(VERSION 3.16)
project(kv_project CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(APPLE AND EXISTS "/opt/homebrew")
  list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

find_package(Threads REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# --- Generate C++ sources from proto/kv.proto -------------------------------
set(PROTO_FILE "${CMAKE_SOURCE_DIR}/proto/kv.proto")
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GENERATED_DIR})

set(PROTO_SRCS "${GENERATED_DIR}/kv.pb.cc")
set(PROTO_HDRS "${GENERATED_DIR}/kv.pb.h")
set(GRPC_SRCS "${GENERATED_DIR}/kv.grpc.pb.cc")
set(GRPC_HDRS "${GENERATED_DIR}/kv.grpc.pb.h")

add_custom_command(
  OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
  COMMAND protobuf::protoc
  ARGS --grpc_out=${GENERATED_DIR}
       --cpp_out=${GENERATED_DIR}
       --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
       -I ${CMAKE_SOURCE_DIR}/proto
       ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
  COMMENT "Generating protobuf/gRPC sources from kv.proto"
)

add_library(kv_proto STATIC ${PROTO_SRCS} ${GRPC_SRCS})
target_link_libraries(kv_proto PUBLIC protobuf::libprotobuf gRPC::grpc++ gRPC::grpc)
target_include_directories(kv_proto PUBLIC ${GENERATED_DIR})

# --- Shared library: consistent hashing + coordinator -----------------------
add_library(kv_common STATIC src/consistent_hash.cpp src/coordinator.cpp)
target_include_directories(kv_common PUBLIC include)
target_link_libraries(kv_common PUBLIC kv_proto Threads::Threads)

# --- Binaries ----------------------------------------------------------------
add_executable(storage_node src/storage_node_server.cpp)
target_include_directories(storage_node PRIVATE include)
target_link_libraries(storage_node PRIVATE kv_proto Threads::Threads)

add_executable(kv_client src/kv_client.cpp)
target_link_libraries(kv_client PRIVATE kv_common)

add_executable(kv_benchmark src/benchmark.cpp)
target_link_libraries(kv_benchmark PRIVATE kv_common)
