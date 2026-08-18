#!/usr/bin/env bash
set -euo pipefail

PID_FILE="/tmp/kv_project_pids"

if [[ ! -f "$PID_FILE" ]]; then
  echo "no pid file found at $PID_FILE"
  exit 0
fi

while read -r pid; do
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid"
    echo "stopped pid $pid"
  fi
done < "$PID_FILE"

rm -f "$PID_FILE"
