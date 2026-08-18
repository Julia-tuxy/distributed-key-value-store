#!/usr/bin/env bash
# Scalability sweep: launches NUM_CLIENTS independent kv_benchmark
# *processes* (each its own OS process, its own Coordinator, its own gRPC
# connections to the cluster - a real multi-client scenario, not just
# threads inside one process) concurrently at each thread-count level, then
# merges their raw per-op latencies and totals into one global
# throughput/tail-latency figure per level. This shows how the cluster
# scales as concurrent load increases.
set -euo pipefail

NODES="${1:?usage: scalability_sweep.sh <nodes_csv> [num_clients] [\"thread levels\"] [ops_per_thread]}"
NUM_CLIENTS="${2:-4}"
THREAD_LEVELS="${3:-1 2 4 8 16}"
OPS_PER_THREAD="${4:-2000}"

BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build"
RESULT_ROOT="/tmp/kv_project_sweep_$$"
mkdir -p "$RESULT_ROOT"

percentile() {  # $1 = sorted numeric file, $2 = percentile fraction
  local file=$1 p=$2 n idx
  n=$(wc -l < "$file")
  if [[ "$n" -eq 0 ]]; then echo 0; return; fi
  idx=$(awk -v n="$n" -v p="$p" 'BEGIN{i=int(p*(n-1)); if(i<0)i=0; print i+1}')
  awk -v idx="$idx" 'NR==idx{print; exit}' "$file"
}

printf "%-9s %-9s %-13s %-16s %-10s %-10s %-10s\n" "clients" "threads" "concurrency" "throughput" "p50(ms)" "p90(ms)" "p99(ms)"

for threads in $THREAD_LEVELS; do
  level_dir="$RESULT_ROOT/t${threads}"
  mkdir -p "$level_dir"

  pids=()
  for c in $(seq 1 "$NUM_CLIENTS"); do
    "$BUILD_DIR/kv_benchmark" --nodes="$NODES" --threads="$threads" \
      --ops-per-thread="$OPS_PER_THREAD" --client-id="$c" --result-dir="$level_dir" \
      > "$level_dir/client_${c}.log" 2>&1 &
    pids+=($!)
  done
  for pid in "${pids[@]}"; do wait "$pid"; done

  # Wall time for the level is the slowest client (they ran concurrently),
  # not the sum - matches how you'd measure real concurrent throughput.
  total_ops=0
  max_wall="0"
  for f in "$level_dir"/client_*_summary.txt; do
    IFS=, read -r succ err wall < "$f"
    total_ops=$((total_ops + succ + err))
    max_wall=$(awk -v a="$max_wall" -v b="$wall" 'BEGIN{print (a>b)?a:b}')
  done

  cat "$level_dir"/client_*_latencies.txt | sort -n > "$level_dir/merged_latencies.txt"

  throughput=$(awk -v ops="$total_ops" -v wall="$max_wall" 'BEGIN{ if(wall>0) printf "%.1f", ops/wall; else print 0 }')
  p50_ms=$(awk -v v="$(percentile "$level_dir/merged_latencies.txt" 0.50)" 'BEGIN{printf "%.3f", v/1000}')
  p90_ms=$(awk -v v="$(percentile "$level_dir/merged_latencies.txt" 0.90)" 'BEGIN{printf "%.3f", v/1000}')
  p99_ms=$(awk -v v="$(percentile "$level_dir/merged_latencies.txt" 0.99)" 'BEGIN{printf "%.3f", v/1000}')

  concurrency=$((NUM_CLIENTS * threads))
  printf "%-9s %-9s %-13s %-16s %-10s %-10s %-10s\n" \
    "$NUM_CLIENTS" "$threads" "$concurrency" "${throughput} ops/s" "$p50_ms" "$p90_ms" "$p99_ms"
done

echo ""
echo "raw per-client logs and latency dumps kept at: $RESULT_ROOT"
