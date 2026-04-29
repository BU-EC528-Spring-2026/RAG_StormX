#!/bin/bash
# =============================================================================
# Bridging script: create SPTAG VM, install deps, prepare NVMe, download
# SIFT1B dataset, clone repo, build Docker image, and rebuild SPTAG with
# the Aerospike node IP baked in.
#
# Expects these env vars (set by the caller / bench-e2e.sh):
#   PROJECT_ID, ZONE, AEROSPIKE_IP
# =============================================================================
set -euo pipefail

: "${PROJECT_ID:?PROJECT_ID is required}"
: "${ZONE:?ZONE is required}"
: "${AEROSPIKE_IP:?AEROSPIKE_IP is required}"

SPTAG_VM="${SPTAG_VM:-sptag-node}"
SPTAG_MACHINE_TYPE="${SPTAG_MACHINE_TYPE:-c2-standard-8}"
GITHUB_REPO="${GITHUB_REPO:-https://github.com/BU-EC528-Spring-2026/RAG_StormX.git}"
GITHUB_BRANCH="${GITHUB_BRANCH:-aerospike-udf}"

# ── colors ──
CYAN='\033[1;36m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
MAGENTA='\033[1;35m'
BOLD='\033[1m'
RESET='\033[0m'

step() {
    echo
    echo -e "${MAGENTA}──────────────────────────────────────────────${RESET}"
    echo -e "${MAGENTA}  $1${RESET}"
    echo -e "${MAGENTA}──────────────────────────────────────────────${RESET}"
}

pause() {
    echo
    echo -ne "${YELLOW}▶ $1  ${BOLD}[Press Enter]${RESET} "
    read -r
    echo
}

info() {
    echo -e "${GREEN}  ✓ $1${RESET}"
}

sptag_ssh() {
    gcloud compute ssh "$SPTAG_VM" \
        --project="$PROJECT_ID" \
        --zone="$ZONE" \
        --command "$1"
}

# =============================================================================
# 2a. Create the SPTAG VM
# =============================================================================
pause "2a — Create SPTAG VM ($SPTAG_VM, $SPTAG_MACHINE_TYPE)"
step "2a  Create SPTAG VM"

if gcloud compute instances describe "$SPTAG_VM" --project="$PROJECT_ID" --zone="$ZONE" &>/dev/null; then
    info "SPTAG VM ($SPTAG_VM) already exists, skipping."
else
    gcloud compute instances create "$SPTAG_VM" \
        --project="$PROJECT_ID" \
        --zone="$ZONE" \
        --machine-type="$SPTAG_MACHINE_TYPE" \
        --subnet=default \
        --tags=http-server,https-server \
        --create-disk=auto-delete=yes,boot=yes,size=250GB,type=pd-standard,image-family=ubuntu-2404-lts-amd64,image-project=ubuntu-os-cloud \
        --local-ssd=interface=NVME \
        --scopes=default
    info "VM created."
fi

echo -e "${YELLOW}Waiting for SSH on $SPTAG_VM...${RESET}"
for i in $(seq 1 20); do
    if sptag_ssh "echo ready" 2>/dev/null; then break; fi
    sleep 10
done
info "SSH ready."

# =============================================================================
# 2b. Install dependencies
# =============================================================================
pause "2b — Install dependencies (git, docker, cmake, axel, ...)"
step "2b  Install dependencies"

sptag_ssh 'sudo apt-get update && sudo apt-get install -y \
    git build-essential cmake apt-utils docker.io pkg-config \
    libssl-dev libaio-dev python3-pip curl unzip wget axel tmux && \
    sudo systemctl enable docker && sudo systemctl start docker'
info "Dependencies installed."

# =============================================================================
# 2c. Prepare NVMe storage
# =============================================================================
pause "2c — Format & mount NVMe SSD"
step "2c  Prepare NVMe storage"

sptag_ssh 'set -e
if mountpoint -q /mnt/nvme; then
    echo "/mnt/nvme already mounted, skipping format."
else
    NVME_DEV=$(lsblk -dno NAME,TYPE | grep nvme | head -1 | awk "{print \"/dev/\" \$1}")
    if [ -z "$NVME_DEV" ]; then echo "ERROR: no NVMe device found"; exit 1; fi
    echo "Formatting $NVME_DEV..."
    sudo mkfs.ext4 -O ^has_journal "$NVME_DEV"
    sudo mkdir -p /mnt/nvme
    sudo mount "$NVME_DEV" /mnt/nvme
    sudo chown -R "$USER":"$USER" /mnt/nvme
fi
mkdir -p /mnt/nvme/sift1b /mnt/nvme/sptag_bench
echo "NVMe ready at /mnt/nvme"
'
info "NVMe ready."

# =============================================================================
# 2d. Download SIFT1B dataset
# =============================================================================
pause "2d — Download SIFT1B dataset (~119 GB, takes ~10 min)"
step "2d  Download SIFT1B dataset"

sptag_ssh 'set -e
cd /mnt/nvme/sift1b
[ -f query.public.10K.u8bin ] || wget -q -O query.public.10K.u8bin  https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/query.public.10K.u8bin
[ -f GT.public.1B.ibin ]      || wget -q -O GT.public.1B.ibin       https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/GT.public.1B.ibin
[ -f base.1B.u8bin ]           || axel -n 16 -o base.1B.u8bin        https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/base.1B.u8bin
echo "Download complete."
ls -lh /mnt/nvme/sift1b/
'
info "Dataset downloaded."

# =============================================================================
# 2e. Create 100M subset
# =============================================================================
pause "2e — Extract first 100M vectors (~12 GB subset)"
step "2e  Create 100M subset"

sptag_ssh 'if [ -f /mnt/nvme/sift1b/base.100M.u8bin ]; then echo "100M subset already exists, skipping."; exit 0; fi
python3 - <<'"'"'PY'"'"'
import os, struct
src = "/mnt/nvme/sift1b/base.1B.u8bin"
dst = "/mnt/nvme/sift1b/base.100M.u8bin"
topk = 100_000_000
chunk = 64 * 1024 * 1024
with open(src, "rb") as fsrc:
    header = fsrc.read(8)
    n, d = struct.unpack("II", header)
    need = topk * d
    with open(dst, "wb") as fdst:
        fdst.write(struct.pack("II", topk, d))
        remaining = need
        while remaining:
            r = min(chunk, remaining)
            buf = fsrc.read(r)
            if len(buf) != r:
                raise SystemExit("short read")
            fdst.write(buf)
            remaining -= r
print("wrote", dst, "bytes", os.path.getsize(dst))
PY
'
info "100M subset ready."

# =============================================================================
# 2f. Clone repo + build Docker image
# =============================================================================
pause "2f — Clone repo & build SPTAG Docker image"
step "2f  Clone repo + Docker build"

sptag_ssh "set -e
if [ ! -d ~/RAG_StormX ]; then
    git clone --recursive -b ${GITHUB_BRANCH} ${GITHUB_REPO}
else
    cd ~/RAG_StormX
    git fetch origin
    git reset --hard origin/${GITHUB_BRANCH}
    git submodule update --recursive
fi
cd ~/RAG_StormX/SPTAG
sudo docker build -t sptag .
"
info "Docker image built."

# =============================================================================
# 2g. Rebuild SPTAG with Aerospike IP
# =============================================================================
pause "2g — Rebuild SPTAG in Docker with Aerospike IP (${AEROSPIKE_IP})"
step "2g  cmake + build inside Docker"

sptag_ssh "sudo docker run --rm --net=host \
    -v /home/\$USER/RAG_StormX:/work \
    -v /mnt/nvme:/mnt/nvme \
    sptag:latest bash -lc '
        cd /app
        rm -rf build/build-aero
        cmake -S . -B build/build-aero -DAEROSPIKE=ON \
            -DAEROSPIKE_INCLUDE_DIR=/usr/include \
            -DAEROSPIKE_CLIENT_LIBRARY=/lib/libaerospike.so \
            -DAEROSPIKE_DEFAULT_HOST=${AEROSPIKE_IP}
        cmake --build build/build-aero -j8
    '
"
info "SPTAG rebuilt with Aerospike."

echo
echo -e "${GREEN}══════════════════════════════════════════════${RESET}"
echo -e "${GREEN}  setup-sptag.sh complete.${RESET}"
echo -e "${GREEN}══════════════════════════════════════════════${RESET}"
