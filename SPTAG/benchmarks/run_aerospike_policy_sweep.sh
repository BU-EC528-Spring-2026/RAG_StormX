#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# Aerospike client policy sweep for SPTAG SPFresh benchmarks.
#
# Runs the Dockerized SPTAG benchmark once per Aerospike client-policy profile
# and writes one JSON/log pair per profile for side-by-side comparison.
# -----------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${SPTAG_DOCKER_IMAGE:-sptag:latest}"
RESULTS_ROOT="${RESULTS_DIR:-${ROOT}/results/aerospike_policy_sweep}"
RUN_TAG="${RUN_TAG:-$(date -u +%Y%m%dT%H%M%SZ)}"
OUT_DIR="${RESULTS_ROOT}/${RUN_TAG}"

BENCHMARK_CONFIG_IN_CONTAINER="${BENCHMARK_CONFIG_IN_CONTAINER:-/work/SPTAG/benchmarks/benchmark.aerospike.nvme.ini}"
WORKSPACE_HOST="${WORKSPACE_HOST:-/home/Zhakh/RAG_StormX}"
WORKSPACE_CONTAINER="${WORKSPACE_CONTAINER:-/work}"

SPTAG_AEROSPIKE_HOST="${SPTAG_AEROSPIKE_HOST:-10.150.0.36}"
SPTAG_AEROSPIKE_PORT="${SPTAG_AEROSPIKE_PORT:-3000}"
SPTAG_AEROSPIKE_NAMESPACE="${SPTAG_AEROSPIKE_NAMESPACE:-sptag_data}"
SPTAG_AEROSPIKE_SET="${SPTAG_AEROSPIKE_SET:-sptag}"
SPTAG_AEROSPIKE_BIN="${SPTAG_AEROSPIKE_BIN:-posting}"

mkdir -p "$OUT_DIR"
if [[ "$OUT_DIR" == "$WORKSPACE_HOST"* ]]; then
    OUT_DIR_IN_CONTAINER="${WORKSPACE_CONTAINER}${OUT_DIR#"$WORKSPACE_HOST"}"
else
    OUT_DIR_IN_CONTAINER="$OUT_DIR"
fi

# name|max_conns|conn_pools|thread_pool|min_conns|max_socket_idle
PROFILES=(
    "conservative|256|2|8|32|60"
    "current|1000|4|16|100|60"
    "high_concurrency|2000|8|32|200|60"
    "larger_pool|1000|8|16|100|60"
    "low_idle|1000|4|16|100|15"
)

echo "=== Aerospike policy sweep ==="
echo "  image:        $IMAGE"
echo "  host:         $SPTAG_AEROSPIKE_HOST:$SPTAG_AEROSPIKE_PORT"
echo "  namespace:    $SPTAG_AEROSPIKE_NAMESPACE"
echo "  set/bin:      $SPTAG_AEROSPIKE_SET/$SPTAG_AEROSPIKE_BIN"
echo "  config:       $BENCHMARK_CONFIG_IN_CONTAINER"
echo "  results:      $OUT_DIR"
echo

for profile in "${PROFILES[@]}"; do
    IFS='|' read -r name max_conns conn_pools thread_pool min_conns max_socket_idle <<< "$profile"
    out_json_host="${OUT_DIR}/benchmark_aerospike_policy_${name}.json"
    out_log_host="${OUT_DIR}/benchmark_aerospike_policy_${name}.log"
    out_json_ctr="${OUT_DIR_IN_CONTAINER}/benchmark_aerospike_policy_${name}.json"

    echo "=============================================================="
    echo "--- Profile: $name"
    echo "    max_conns_per_node=$max_conns conn_pools_per_node=$conn_pools thread_pool_size=$thread_pool"
    echo "    min_conns_per_node=$min_conns max_socket_idle=$max_socket_idle"
    echo "    output=$out_json_host"
    echo "=============================================================="

    sudo docker run --rm --net=host \
        -e BENCHMARK_CONFIG="$BENCHMARK_CONFIG_IN_CONTAINER" \
        -e BENCHMARK_OUTPUT="$out_json_ctr" \
        -e SPTAG_AEROSPIKE_HOST="$SPTAG_AEROSPIKE_HOST" \
        -e SPTAG_AEROSPIKE_PORT="$SPTAG_AEROSPIKE_PORT" \
        -e SPTAG_AEROSPIKE_NAMESPACE="$SPTAG_AEROSPIKE_NAMESPACE" \
        -e SPTAG_AEROSPIKE_SET="$SPTAG_AEROSPIKE_SET" \
        -e SPTAG_AEROSPIKE_BIN="$SPTAG_AEROSPIKE_BIN" \
        -e SPTAG_AEROSPIKE_MAX_CONNS_PER_NODE="$max_conns" \
        -e SPTAG_AEROSPIKE_CONN_POOLS_PER_NODE="$conn_pools" \
        -e SPTAG_AEROSPIKE_THREAD_POOL_SIZE="$thread_pool" \
        -e SPTAG_AEROSPIKE_MIN_CONNS_PER_NODE="$min_conns" \
        -e SPTAG_AEROSPIKE_MAX_SOCKET_IDLE="$max_socket_idle" \
        -v "${WORKSPACE_HOST}:${WORKSPACE_CONTAINER}" \
        -v /mnt/nvme:/mnt/nvme \
        -v /mnt/nvme0:/mnt/nvme0 \
        -v /mnt/nvme1:/mnt/nvme1 \
        "$IMAGE" bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig' 2>&1 \
        | tee "$out_log_host"

    echo "--- Done profile: $name"
    echo
done

python3 - "$OUT_DIR" <<'PY' | tee "$OUT_DIR/summary.txt"
import json
import pathlib
import sys

out_dir = pathlib.Path(sys.argv[1])
rows = []

for path in sorted(out_dir.glob("benchmark_aerospike_policy_*.json")):
    profile = path.stem.replace("benchmark_aerospike_policy_", "")
    with path.open() as f:
        data = json.load(f)

    results = data.get("results", {})
    search = results.get("benchmark0_query_before_insert", {})
    insert = results.get("benchmark1_insert")
    if isinstance(insert, dict) and insert:
        last_batch = sorted(insert.keys(), key=lambda k: int(k.split("_", 1)[1]))[-1]
        search = insert[last_batch].get("search", search)

    rows.append({
        "profile": profile,
        "qps": float(search.get("qps", 0.0)),
        "p99": float(search.get("p99", 0.0)),
        "mean": float(search.get("meanLatency", 0.0)),
    })

print("=== Aerospike policy sweep summary ===")
if not rows:
    print("No JSON result files found.")
    sys.exit(0)

for row in rows:
    print(f"{row['profile']:>16}  qps={row['qps']:.2f}  p99_ms={row['p99']:.4f}  mean_ms={row['mean']:.4f}")

best_qps = max(rows, key=lambda r: r["qps"])
best_p99 = min(rows, key=lambda r: r["p99"])
print()
print(f"Best throughput: {best_qps['profile']} ({best_qps['qps']:.2f} qps)")
print(f"Lowest p99:      {best_p99['profile']} ({best_p99['p99']:.4f} ms)")
PY

echo "All policy sweep runs complete. Results are in $OUT_DIR"
