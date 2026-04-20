#!/usr/bin/env bash
# ------------------------------------------------------------------
# Aerospike UDF A/B benchmark driver.
#
# Runs the SPFresh benchmark binary against an Aerospike-backed SPANN
# index for each AerospikeUDFMode (Off / Packed / Pairs) and captures
# one JSON output per mode so latency, QPS, and recall can be compared
# side-by-side.
#
# For PQ-quantized indexes, Pairs is skipped (PQ guard in
# ExtraDynamicSearcher clamps it to Off or Packed depending on
# SPTAG_AEROSPIKE_UDF_ALLOW_PACKED_PQ). Passing --pq re-runs only the
# modes that are valid under PQ.
#
# Usage:
#   ./run_aerospike_udf_ab.sh                 # non-PQ: Off, Packed, Pairs
#   ./run_aerospike_udf_ab.sh --pq            # PQ: Off (+ Packed if allow=1)
#   ./run_aerospike_udf_ab.sh --pq --allow-packed-pq
#
# Required env / CLI:
#   SPFRESH_BINARY     path to spfresh test binary (defaults to
#                      ../SPTAG/build/build-aero/Test/spfresh)
#   BENCHMARK_CONFIG   benchmark ini (defaults to ./benchmark.aerospike.nvme.ini)
#   RESULTS_DIR        where to write per-mode JSON (defaults to ../results)
#   SPTAG_AEROSPIKE_HOST / _PORT / _NAMESPACE / _SET / _BIN
# ------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SPFRESH_BINARY="${SPFRESH_BINARY:-${ROOT}/SPTAG/build/build-aero/Test/spfresh}"
BENCHMARK_CONFIG="${BENCHMARK_CONFIG:-${ROOT}/benchmarks/benchmark.aerospike.nvme.ini}"
RESULTS_DIR="${RESULTS_DIR:-${ROOT}/results}"
TOPN="${SPTAG_AEROSPIKE_UDF_TOPN:-32}"

PQ=0
ALLOW_PACKED_PQ=0
for arg in "$@"; do
    case "$arg" in
        --pq) PQ=1 ;;
        --allow-packed-pq) ALLOW_PACKED_PQ=1 ;;
        *) echo "unknown arg: $arg" >&2; exit 1 ;;
    esac
done

if [ ! -x "$SPFRESH_BINARY" ]; then
    echo "spfresh binary not found / not executable: $SPFRESH_BINARY" >&2
    exit 1
fi
if [ ! -f "$BENCHMARK_CONFIG" ]; then
    echo "benchmark config not found: $BENCHMARK_CONFIG" >&2
    exit 1
fi

mkdir -p "$RESULTS_DIR"

if [ "$PQ" -eq 1 ]; then
    if [ "$ALLOW_PACKED_PQ" -eq 1 ]; then
        MODES=(0 1)  # Off, Packed
        export SPTAG_AEROSPIKE_UDF_ALLOW_PACKED_PQ=1
    else
        MODES=(0)    # Off only — Packed and Pairs clamped to Off under PQ
        export SPTAG_AEROSPIKE_UDF_ALLOW_PACKED_PQ=0
    fi
    SUFFIX="pq"
else
    MODES=(0 1 2)    # Off, Packed, Pairs
    export SPTAG_AEROSPIKE_UDF_ALLOW_PACKED_PQ=0
    SUFFIX="nopq"
fi

export SPTAG_AEROSPIKE_UDF_TOPN="$TOPN"
export BENCHMARK_CONFIG

declare -A MODE_NAME=( [0]=Off [1]=Packed [2]=Pairs )

echo "=== Aerospike UDF A/B benchmark ==="
echo "  binary:      $SPFRESH_BINARY"
echo "  config:      $BENCHMARK_CONFIG"
echo "  results_dir: $RESULTS_DIR"
echo "  PQ:          $PQ  (allow_packed_pq=$SPTAG_AEROSPIKE_UDF_ALLOW_PACKED_PQ)"
echo "  top_n:       $TOPN"
echo "  modes:       ${MODES[*]}"
echo

for m in "${MODES[@]}"; do
    name="${MODE_NAME[$m]}"
    out="${RESULTS_DIR}/benchmark_aerospike_udf_${name,,}_${SUFFIX}.json"
    echo "--- Running mode=$m ($name) -> $out"
    export SPTAG_AEROSPIKE_UDF_MODE="$m"
    BENCHMARK_OUTPUT="$out" \
        "$SPFRESH_BINARY" --run_test=SPFreshTest/BenchmarkFromConfig 2>&1 \
        | tee "${RESULTS_DIR}/benchmark_aerospike_udf_${name,,}_${SUFFIX}.log"
    echo "--- Done mode=$m ($name)"
    echo
done

echo "All runs complete. JSON outputs in $RESULTS_DIR/*.json"
