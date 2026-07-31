#!/usr/bin/env bash
# Starts 3 kv_server storage nodes plus 1 kv_coordinator in front of them.
# PIDs are recorded so stop_cluster.sh can clean everything up.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
PID_FILE="/tmp/kv_cluster_pids"

NODE_PORTS=(50051 50052 50053)
COORD_PORT=60000

: > "$PID_FILE"

for port in "${NODE_PORTS[@]}"; do
  "$BUILD_DIR/kv_server" "0.0.0.0:$port" > "/tmp/kv_node_$port.log" 2>&1 &
  echo $! >> "$PID_FILE"
  echo "started kv_server on port $port (pid $!)"
done

sleep 1  # let nodes finish binding their ports before the coordinator connects

node_args=()
for port in "${NODE_PORTS[@]}"; do
  node_args+=("localhost:$port")
done

"$BUILD_DIR/kv_coordinator" "0.0.0.0:$COORD_PORT" "${node_args[@]}" > "/tmp/kv_coordinator.log" 2>&1 &
echo $! >> "$PID_FILE"
echo "started kv_coordinator on port $COORD_PORT (pid $!)"

echo "cluster up. connect a client with: ./build/kv_client localhost:$COORD_PORT"
