#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# Per-mode profiled A/B runner for the Aerospike UDF benchmark.
#
# Runs the existing `sptag:latest` Docker image (produced by SPTAG/Dockerfile)
# once per mode (Off / Pairs [/ Packed]) and snapshots Aerospike latency
# histograms + namespace counters around each individual mode so we can
# attribute per-mode latency deltas to server-side buckets (batch-sub-read
# vs batch-sub-udf, udf, etc.).
#
# Required env (export before calling):
#   SPTAG_AEROSPIKE_HOST, _PORT, _NAMESPACE, _SET, _BIN
#   BENCHMARK_CONFIG_IN_CONTAINER  (defaults to /work/benchmarks/benchmark.aerospike.nvme.ini)
#
# Usage:
#   ./run_aerospike_udf_ab_profiled.sh <run-tag> [--with-packed]
# -----------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
H="${SPTAG_AEROSPIKE_HOST:-10.150.0.34}"
NS="${SPTAG_AEROSPIKE_NAMESPACE:-sptag_data}"
PORT="${SPTAG_AEROSPIKE_PORT:-3000}"
SET="${SPTAG_AEROSPIKE_SET:-sptag}"
BIN="${SPTAG_AEROSPIKE_BIN:-value}"
RESULTS_DIR="${RESULTS_DIR:-${ROOT}/results}"
OUT="${RESULTS_DIR}/profile"
BENCHMARK_CONFIG_IN_CONTAINER="${BENCHMARK_CONFIG_IN_CONTAINER:-/work/SPTAG/benchmarks/benchmark.aerospike.nvme.ini}"
TOPN="${SPTAG_AEROSPIKE_UDF_TOPN:-32}"
IMAGE="${SPTAG_DOCKER_IMAGE:-sptag:latest}"

TAG="${1:-run}"
shift || true

WITH_PACKED=0
for arg in "$@"; do
    case "$arg" in
        --with-packed) WITH_PACKED=1 ;;
        *) echo "unknown arg: $arg" >&2; exit 1 ;;
    esac
done

mkdir -p "$OUT" "$RESULTS_DIR"

if [ "$WITH_PACKED" -eq 1 ]; then
    MODES=(0 1 2)
else
    MODES=(0 2)
fi

declare -A MODE_NAME=( [0]=Off [1]=Packed [2]=Pairs )

snap_ns_raw() {
    local label="$1"
    {
        echo "=== namespace/$NS @ $label $(date -u +%FT%TZ) ==="
        asinfo -h "$H" -v "namespace/$NS" | tr ';' '\n' | \
            grep -E '^(udf_|batch_|client_udf_|client_read|client_write|re_repl|fail_|from_proxy)'
        echo
    } >> "$OUT/${TAG}_ns_counters.txt"
}

snap_latencies() {
    local label="$1"
    {
        echo "=== latencies @ $label $(date -u +%FT%TZ) ==="
        asinfo -h "$H" -v "latencies:" | tr ';' '\n'
        echo
    } >> "$OUT/${TAG}_latencies.txt"
}

snap_all() {
    snap_ns_raw "$1"
    snap_latencies "$1"
}

snap_all "baseline"

for m in "${MODES[@]}"; do
    name="${MODE_NAME[$m]}"
    out_json_host="${RESULTS_DIR}/benchmark_aerospike_udf_${name,,}_nopq.json"
    out_log_host="${RESULTS_DIR}/benchmark_aerospike_udf_${name,,}_nopq.log"
    out_json_ctr="/work/SPTAG/results/benchmark_aerospike_udf_${name,,}_nopq.json"
    echo
    echo "=============================================================="
    echo "--- Mode $m ($name) -> $out_json_host"
    echo "=============================================================="

    snap_all "${name}_pre"

    sudo docker run --rm --net=host \
        -e BENCHMARK_CONFIG="$BENCHMARK_CONFIG_IN_CONTAINER" \
        -e BENCHMARK_OUTPUT="$out_json_ctr" \
        -e SPTAG_AEROSPIKE_HOST="$H" \
        -e SPTAG_AEROSPIKE_PORT="$PORT" \
        -e SPTAG_AEROSPIKE_NAMESPACE="$NS" \
        -e SPTAG_AEROSPIKE_SET="$SET" \
        -e SPTAG_AEROSPIKE_BIN="$BIN" \
        -e SPTAG_AEROSPIKE_UDF_MODE="$m" \
        -e SPTAG_AEROSPIKE_UDF_TOPN="$TOPN" \
        -e SPTAG_AEROSPIKE_UDF_ALLOW_PACKED_PQ=0 \
        -v /home/Zhakh/RAG_StormX:/work \
        -v /mnt/nvme:/mnt/nvme \
        -v /mnt/nvme0:/mnt/nvme0 \
        -v /mnt/nvme1:/mnt/nvme1 \
        "$IMAGE" bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig' 2>&1 \
        | tee "$out_log_host"

    snap_all "${name}_post"
    echo "--- Done $name"
done

echo
echo "All runs complete. Per-mode JSONs in $RESULTS_DIR/, histograms in $OUT/"
