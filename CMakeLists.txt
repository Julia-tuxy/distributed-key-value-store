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

# --- Core KV store library (no networking yet) ---
add_library(kv_store src/kv_store.cpp)
target_include_directories(kv_store PUBLIC include)
target_link_libraries(kv_store PUBLIC Threads::Threads)
