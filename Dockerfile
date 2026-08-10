# Multi-stage build: compile in a full image with all the dev toolchain,
# then copy just the two binaries this project actually ships (kv_server,
# kv_coordinator) into a stage that doesn't carry the compiler/CMake
# around. The same image serves both roles -- docker-compose.yml just
# passes a different `command` to pick which binary runs.

FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      protobuf-compiler \
      protobuf-compiler-grpc \
      libprotobuf-dev \
      libgrpc++-dev \
      libgrpc-dev \
      pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY proto/ proto/
COPY include/ include/
COPY src/ src/

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j --target kv_server kv_coordinator

FROM debian:bookworm-slim AS runtime

# Just the shared libraries kv_server/kv_coordinator actually link against
# at runtime -- no headers, no static libs, no compiler. Names/versions
# are Debian bookworm-specific (confirmed via apt-cache search); a newer
# base image would need updated version suffixes here.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libprotobuf32 \
      libgrpc++1.51 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/kv_server /usr/local/bin/kv_server
COPY --from=builder /src/build/kv_coordinator /usr/local/bin/kv_coordinator

ENTRYPOINT ["/bin/sh", "-c", "exec \"$0\" \"$@\""]
