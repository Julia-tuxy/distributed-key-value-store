#!/usr/bin/env bash
# Launches N storage node processes on localhost, each its own OS process
# bound to its own port. PIDs are written to /tmp so stop_cluster.sh can
# find them, and each node's stdout/stderr goes to its own log file.
set -euo pipefail

NUM_NODES="${1:-5}"
BASE_PORT="${2:-50051}"
BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build"
LOG_DIR="/tmp/kv_project_logs"
PID_FILE="/tmp/kv_project_pids"

mkdir -p "$LOG_DIR"
: > "$PID_FILE"

for i in $(seq 1 "$NUM_NODES"); do
  port=$((BASE_PORT + i - 1))
  node_id="node${i}"
  "$BUILD_DIR/storage_node" "$node_id" "127.0.0.1:${port}" \
    > "$LOG_DIR/${node_id}.log" 2>&1 &
  echo $! >> "$PID_FILE"
  echo "started $node_id on 127.0.0.1:${port} (pid $!)"
done

echo ""
echo "cluster node list for --nodes flag:"
nodes=""
for i in $(seq 1 "$NUM_NODES"); do
  port=$((BASE_PORT + i - 1))
  nodes+="node${i}@127.0.0.1:${port},"
done
echo "${nodes%,}"
