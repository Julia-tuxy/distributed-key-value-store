#!/usr/bin/env bash
# Kills everything started by start_cluster.sh.
PID_FILE="/tmp/kv_cluster_pids"

if [[ ! -f "$PID_FILE" ]]; then
  echo "no running cluster found ($PID_FILE missing)"
  exit 0
fi

while read -r pid; do
  kill "$pid" 2>/dev/null || true
done < "$PID_FILE"

rm -f "$PID_FILE"
echo "cluster stopped"
