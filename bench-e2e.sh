#!/bin/bash
# =============================================================================
# End-to-end Aerospike benchmark workflow (master script)
# Runs entirely from Google Cloud Shell.
# Interactive demo mode: press Enter to advance each step.
#
#   1. deploy-aerospike.sh          → 3-node Aerospike cluster
#   2. setup-sptag.sh               → SPTAG VM + deps + NVMe + dataset + Docker
#   3. run_aerospike_policy_sweep.sh → benchmark sweep inside Docker on SPTAG VM
# =============================================================================
set -euo pipefail

# ── colors ──
CYAN='\033[1;36m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
MAGENTA='\033[1;35m'
BOLD='\033[1m'
RESET='\033[0m'

banner() {
    echo
    echo -e "${CYAN}══════════════════════════════════════════════${RESET}"
    echo -e "${CYAN}  $1${RESET}"
    echo -e "${CYAN}══════════════════════════════════════════════${RESET}"
}

pause() {
    echo
    echo -ne "${YELLOW}▶ $1  ${BOLD}[Press Enter to continue]${RESET} "
    read -r
    echo
}

info() {
    echo -e "${GREEN}  ✓ $1${RESET}"
}

# =========================
# PROJECT DETECTION
# =========================
export PROJECT_ID="${GOOGLE_CLOUD_PROJECT:-$(gcloud config get-value project 2>/dev/null)}"
if [[ -z "$PROJECT_ID" || "$PROJECT_ID" == "(unset)" ]]; then
    echo "ERROR: Could not detect GCP project. Run: gcloud config set project YOUR_PROJECT_ID"
    exit 1
fi

export ZONE="${ZONE:-us-east4-b}"
export SPTAG_VM="${SPTAG_VM:-sptag-node}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

sptag_ssh() {
    gcloud compute ssh "$SPTAG_VM" \
        --project="$PROJECT_ID" \
        --zone="$ZONE" \
        --command "$1"
}

banner "Aerospike E2E Benchmark Demo"
info "Project:  $PROJECT_ID"
info "Zone:     $ZONE"
info "SPTAG VM: $SPTAG_VM"

# =============================================================================
# STEP 1
# =============================================================================
pause "Step 1/3 — Deploy 3-node Aerospike cluster"
banner "STEP 1/3  deploy-aerospike.sh"
bash "${SCRIPT_DIR}/deploy-aerospike.sh"

export AEROSPIKE_IP=$(gcloud compute instances describe aerospike-node-1 \
    --project="$PROJECT_ID" \
    --zone="$ZONE" \
    --format="get(networkInterfaces[0].networkIP)")
info "Aerospike node-1 IP: $AEROSPIKE_IP"

echo -e "${YELLOW}Waiting 60s for cluster to stabilize...${RESET}"
sleep 60
gcloud compute ssh aerospike-node-1 \
    --project="$PROJECT_ID" --zone="$ZONE" \
    --command 'asadm -e "info"' || echo "WARNING: cluster check failed, continuing..."

# =============================================================================
# STEP 2
# =============================================================================
pause "Step 2/3 — Setup SPTAG VM (bridging)"
banner "STEP 2/3  setup-sptag.sh"
bash "${SCRIPT_DIR}/setup-sptag.sh"

# =============================================================================
# STEP 3
# =============================================================================
pause "Step 3/3 — Run Aerospike policy sweep benchmark"
banner "STEP 3/3  run_aerospike_policy_sweep.sh"

AEROSPIKE_PORT="${AEROSPIKE_PORT:-3000}"
AEROSPIKE_NAMESPACE="${AEROSPIKE_NAMESPACE:-sptag_data}"
AEROSPIKE_SET="${AEROSPIKE_SET:-sptag}"
AEROSPIKE_BIN="${AEROSPIKE_BIN:-posting}"

sptag_ssh "sudo docker run --rm --net=host \
    -v /home/\$USER/RAG_StormX:/work \
    -v /mnt/nvme:/mnt/nvme \
    -e SPTAG_AEROSPIKE_HOST=${AEROSPIKE_IP} \
    -e SPTAG_AEROSPIKE_PORT=${AEROSPIKE_PORT} \
    -e SPTAG_AEROSPIKE_NAMESPACE=${AEROSPIKE_NAMESPACE} \
    -e SPTAG_AEROSPIKE_SET=${AEROSPIKE_SET} \
    -e SPTAG_AEROSPIKE_BIN=${AEROSPIKE_BIN} \
    -e SPFRESH_BINARY=/app/Release/SPTAGTest \
    -e BENCHMARK_CONFIG=/work/SPTAG/benchmarks/benchmark.aerospike.nvme.ini \
    -e RESULTS_DIR=/work/results \
    sptag:latest bash -lc '
        mkdir -p /work/results
        ulimit -n 65535
        cd /app
        /work/SPTAG/benchmarks/run_aerospike_policy_sweep.sh
    '
"

# =============================================================================
# Collect results
# =============================================================================
banner "Collecting results..."

RESULTS_LOCAL="./results_$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$RESULTS_LOCAL"

gcloud compute scp --recurse \
    "${SPTAG_VM}:~/RAG_StormX/results/" \
    "$RESULTS_LOCAL/" \
    --project="$PROJECT_ID" \
    --zone="$ZONE"

echo
echo -e "${GREEN}══════════════════════════════════════════════${RESET}"
echo -e "${GREEN}  DONE. Results in: $RESULTS_LOCAL${RESET}"
echo -e "${GREEN}══════════════════════════════════════════════${RESET}"
