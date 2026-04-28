#!/usr/bin/env bash
# bench-aerospike.sh — Interactive end-to-end Aerospike + SPTAG benchmark.
# Run from Google Cloud Shell (or anywhere with gcloud installed and an
# authenticated GCP account). The script orchestrates the full pipeline
# step by step, pausing between steps so you can review what just happened.
#
# Override defaults via environment variables, e.g.:
#   PROJECT_ID=my-proj ZONE=us-central1-a ./bench-aerospike.sh
#
# This is the "no UDF" path — SPTAG_AEROSPIKE_UDF_MODE defaults to 0 (Off),
# which uses the standard bulk-read code path. To run server-side UDF
# scoring (Packed/Pairs), follow §4 of readme2.md to compile and deploy
# avx_math.so + register sptag_posting.lua, then re-run the benchmark
# step manually with SPTAG_AEROSPIKE_UDF_MODE=1 (or 2).

set -euo pipefail

# ───────────────────────── colors / formatting ────────────────────────────
if [[ -t 1 ]]; then
  C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'; C_RESET=$'\033[0m'
  C_BLUE=$'\033[34m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
  C_RED=$'\033[31m'; C_CYAN=$'\033[36m'
else
  C_BOLD=""; C_DIM=""; C_RESET=""; C_BLUE=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_CYAN=""
fi

section() {
  echo ""
  echo "${C_BOLD}${C_BLUE}════════════════════════════════════════════════════════════${C_RESET}"
  echo "${C_BOLD}${C_BLUE}  $*${C_RESET}"
  echo "${C_BOLD}${C_BLUE}════════════════════════════════════════════════════════════${C_RESET}"
}
info() { echo "${C_CYAN}ℹ  $*${C_RESET}"; }
ok()   { echo "${C_GREEN}✓ $*${C_RESET}"; }
warn() { echo "${C_YELLOW}⚠  $*${C_RESET}"; }
err()  { echo "${C_RED}✗ $*${C_RESET}" >&2; }

pause() {
  local msg="${1:-Press Enter to continue to the next step...}"
  echo ""
  read -r -p "${C_BOLD}>>> ${msg}${C_RESET} " _ || true
}

ask() {
  local prompt="$1" default="${2:-}" reply
  if [[ -n "$default" ]]; then
    read -r -p "${prompt} [${default}]: " reply
    echo "${reply:-$default}"
  else
    read -r -p "${prompt}: " reply
    echo "$reply"
  fi
}

confirm() {
  local prompt="$1" default="${2:-Y}" reply
  if [[ "$default" =~ ^[Yy]$ ]]; then
    read -r -p "${prompt} [Y/n]: " reply; reply="${reply:-Y}"
  else
    read -r -p "${prompt} [y/N]: " reply; reply="${reply:-N}"
  fi
  [[ "$reply" =~ ^[Yy] ]]
}

# ───────────────────────── temp dir cleanup ──────────────────────────────
TMP_BENCH=""
cleanup_tmp() { [[ -n "$TMP_BENCH" && -d "$TMP_BENCH" ]] && rm -rf "$TMP_BENCH"; }
trap cleanup_tmp EXIT

# ─────────────────────────── default config ──────────────────────────────
PROJECT_ID="${PROJECT_ID:-}"
ZONE="${ZONE:-us-east4-b}"
AS_MACHINE_TYPE="${AS_MACHINE_TYPE:-n2-standard-4}"
SPTAG_MACHINE_TYPE="${SPTAG_MACHINE_TYPE:-c2-standard-8}"
IMAGE_FAMILY="${IMAGE_FAMILY:-ubuntu-2404-lts-amd64}"
IMAGE_PROJECT="${IMAGE_PROJECT:-ubuntu-os-cloud}"
CLUSTER_NAME="${CLUSTER_NAME:-sptag-cluster}"
NAMESPACE_NAME="${NAMESPACE_NAME:-sptag_data}"
NVME_DEVICE="${NVME_DEVICE:-/dev/disk/by-id/google-local-nvme-ssd-0}"
AS_NODE1="${AS_NODE1:-aerospike-node-1}"
AS_NODE2="${AS_NODE2:-aerospike-node-2}"
AS_NODE3="${AS_NODE3:-aerospike-node-3}"
SPTAG_NODE="${SPTAG_NODE:-sptag-node}"
AEROSPIKE_URL="${AEROSPIKE_URL:-https://download.aerospike.com/artifacts/aerospike-server-community/8.1.1.1/aerospike-server-community_8.1.1.1_tools-12.1.1_ubuntu24.04_x86_64.tgz}"
AEROSPIKE_DIR="${AEROSPIKE_DIR:-aerospike-server-community_8.1.1.1_tools-12.1.1_ubuntu24.04_x86_64}"
SPTAG_UDF_MODE="${SPTAG_AEROSPIKE_UDF_MODE:-0}"  # 0=Off (default), 1=Packed, 2=Pairs

AS_NODE1_IP=""; AS_NODE2_IP=""; AS_NODE3_IP=""

# ───────────────────────────── steps ─────────────────────────────────────

show_welcome() {
  clear || true
  section "Aerospike + SPTAG Benchmark — Interactive Workflow"
  cat <<'EOF'

This script walks you through the full Aerospike-backed SPTAG benchmark
on GCP, end to end.

What it does (in order):
  1.  Verify prerequisites
  2.  Collect config (project, zone, node names)
  3.  Create internal firewall rule for Aerospike (TCP 3000-3004)
  4.  Create 3 Aerospike VMs with auto-install startup script
  5.  Update node 1 & 2 configs to include the full 3-node mesh
  6.  Verify the Aerospike cluster is healthy
  7.  Create the SPTAG benchmark VM (separate machine, c2-standard-8)
  8.  Install Docker + build the SPTAG image on the benchmark VM
  9.  Format and mount NVMe on the SPTAG VM
  10. Download SIFT1B dataset (~119 GB) on the SPTAG VM
  11. (Optional) create 100M-vector subset
  12. Run the SPFresh benchmark in Docker against Aerospike — UDF OFF
  13. Show results
  14. Cleanup (delete all 4 VMs + firewall) — opt-in

After each major step the script will pause and wait for you to press
Enter before continuing, so you can read what's about to happen and
inspect intermediate output.

Why "UDF OFF" by default?
  Server-side UDF scoring (Packed/Pairs) needs avx_math.so compiled and
  copied to every Aerospike node, plus a Lua module registered with
  aql. That deployment chain is fragile (see readme2.md §3). The
  baseline "Off" mode is the reliable path. To enable UDF later, follow
  readme2.md §4 then re-run with SPTAG_AEROSPIKE_UDF_MODE=1 or 2.

You can override defaults via env vars, e.g.
  PROJECT_ID=my-proj ZONE=us-central1-a ./bench-aerospike.sh
EOF
  pause "Press Enter to start..."
}

check_prerequisites() {
  section "Step 1 / Prerequisites"
  if ! command -v gcloud >/dev/null 2>&1; then
    err "gcloud CLI not found. Install it or run from Google Cloud Shell."
    exit 1
  fi
  ok "gcloud found"

  cat <<EOF

Before continuing, please confirm:
  • You have a Google Cloud project with billing enabled.
  • The Compute Engine API is enabled
      gcloud services enable compute.googleapis.com
  • You have at least 20 vCPUs of CPUS quota in the chosen region:
      3 × $AS_MACHINE_TYPE (= 12 vCPU) + 1 × $SPTAG_MACHINE_TYPE (= 8 vCPU)
    Check IAM & Admin → Quotas → CPUS (all regions).
  • You're running this from Cloud Shell, NOT from one of the VMs you
    created earlier (default VM service accounts lack the API scopes
    needed to create more VMs).

Why Aerospike for vector serving?
  Vector search is read-dominated. Aerospike keeps the primary index
  in DRAM and uses direct I/O on raw NVMe, so a lookup is one memory
  hit + (typically) one storage read with stable tail latency. This
  fits the ANN serving workload well.
EOF
  if ! confirm "Ready to proceed?" "Y"; then
    info "Aborted."
    exit 0
  fi
}

collect_config() {
  section "Step 2 / Configuration"

  local detected
  detected="$(gcloud config get-value project 2>/dev/null || true)"
  if [[ -z "$PROJECT_ID" ]]; then
    if [[ -n "$detected" && "$detected" != "(unset)" ]]; then
      PROJECT_ID="$(ask "GCP project ID" "$detected")"
    else
      PROJECT_ID="$(ask "GCP project ID")"
    fi
  fi
  [[ -z "$PROJECT_ID" ]] && { err "Project ID is required."; exit 1; }

  ZONE="$(ask "Zone" "$ZONE")"
  AS_MACHINE_TYPE="$(ask "Aerospike node machine type" "$AS_MACHINE_TYPE")"
  SPTAG_MACHINE_TYPE="$(ask "SPTAG node machine type" "$SPTAG_MACHINE_TYPE")"
  AS_NODE1="$(ask "Aerospike node 1 name" "$AS_NODE1")"
  AS_NODE2="$(ask "Aerospike node 2 name" "$AS_NODE2")"
  AS_NODE3="$(ask "Aerospike node 3 name" "$AS_NODE3")"
  SPTAG_NODE="$(ask "SPTAG benchmark node name" "$SPTAG_NODE")"

  echo ""
  info "Configuration summary:"
  cat <<EOF
  PROJECT_ID         = $PROJECT_ID
  ZONE               = $ZONE
  AS_MACHINE_TYPE    = $AS_MACHINE_TYPE
  SPTAG_MACHINE_TYPE = $SPTAG_MACHINE_TYPE
  CLUSTER_NAME       = $CLUSTER_NAME
  NAMESPACE_NAME     = $NAMESPACE_NAME
  AS_NODE1           = $AS_NODE1
  AS_NODE2           = $AS_NODE2
  AS_NODE3           = $AS_NODE3
  SPTAG_NODE         = $SPTAG_NODE
  UDF mode           = $SPTAG_UDF_MODE  $([[ "$SPTAG_UDF_MODE" == "0" ]] && echo "(Off)" || echo "(non-Off — make sure UDF is deployed!)")
EOF
  echo ""
  if ! confirm "Use this configuration?" "Y"; then
    info "Re-run the script to change values, or set them via env vars."
    exit 0
  fi

  gcloud config set project "$PROJECT_ID" >/dev/null
  ok "gcloud active project set to $PROJECT_ID"

  TMP_BENCH="$(mktemp -d -t bench-aerospike.XXXXXX)"
}

create_firewall() {
  section "Step 3 / Internal firewall rule"
  cat <<EOF
Creating a firewall rule allowing Aerospike traffic (TCP 3000-3004)
between VMs tagged 'aerospike'. The SPTAG node reaches Aerospike via
the project's default-allow-internal rule, so it doesn't need this tag.
EOF
  pause

  if gcloud compute firewall-rules describe aerospike-internal &>/dev/null; then
    ok "Firewall rule 'aerospike-internal' already exists, leaving it alone."
  else
    gcloud compute firewall-rules create aerospike-internal \
      --network=default \
      --allow tcp:3000-3004 \
      --source-tags=aerospike \
      --target-tags=aerospike
    ok "Firewall rule created."
  fi
}

# Generate the Aerospike server config given an array of mesh seed IPs.
# Echoes the config to stdout.
aerospike_config() {
  local seeds=("$@")
  local seed_lines=""
  for ip in "${seeds[@]}"; do
    seed_lines+="        mesh-seed-address-port ${ip} 3003"$'\n'
  done

  cat <<CONF
service {
    cluster-name ${CLUSTER_NAME}
    proto-fd-max 15000
}

logging {
    file /var/log/aerospike/aerospike.log {
        context any info
    }
}

network {
    service {
        address any
        port 3000
    }

    fabric {
        address any
        port 3001
    }

    heartbeat {
        mode mesh
        address any
        port 3003
        interval 150
        timeout 10
${seed_lines}    }

    admin {
        address any
        port 3004
    }
}

namespace ${NAMESPACE_NAME} {
    replication-factor 2
    default-ttl 0
    indexes-memory-budget 4G

    storage-engine device {
        device ${NVME_DEVICE}
    }
}
CONF
}

# Write the Aerospike VM startup script (used as --metadata-from-file).
write_startup_script() {
  cat > "$TMP_BENCH/startup-aerospike.sh" <<'STARTUP'
#!/bin/bash
set -euxo pipefail
exec > /var/log/startup-aerospike.log 2>&1
export DEBIAN_FRONTEND=noninteractive

CLUSTER_NAME="$(curl -fs -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/attributes/cluster-name)"
NAMESPACE_NAME="$(curl -fs -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/attributes/namespace-name)"
SEED_IPS_RAW="$(curl -fs -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/attributes/seed-ips || true)"
NVME_DEVICE="$(curl -fs -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/attributes/nvme-device)"
AEROSPIKE_URL="$(curl -fs -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/attributes/aerospike-url)"
AEROSPIKE_DIR="$(curl -fs -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/attributes/aerospike-dir)"

apt-get update
apt-get install -y wget curl python3 tar util-linux

if [ -b "$NVME_DEVICE" ]; then
    blkdiscard "$NVME_DEVICE" || true
fi

if [ ! -d "/opt/aerospike/bin" ]; then
    mkdir -p /opt/aerospike
    wget "$AEROSPIKE_URL" -O /tmp/aerospike.tgz
    tar -xzf /tmp/aerospike.tgz -C /tmp
    cd "/tmp/$AEROSPIKE_DIR"
    ./asinstall
fi

mkdir -p /etc/aerospike /var/log/aerospike
chown -R aerospike:aerospike /var/log/aerospike || true

if [ -b "$NVME_DEVICE" ]; then
    chown aerospike:aerospike "$(readlink -f "$NVME_DEVICE")" || true
    usermod -aG disk aerospike || true
fi

SEED_LINES=""
if [[ -n "${SEED_IPS_RAW}" ]]; then
  IFS=',' read -ra SEEDS <<< "${SEED_IPS_RAW}"
  for ip in "${SEEDS[@]}"; do
    [[ -n "${ip}" ]] && SEED_LINES="${SEED_LINES}        mesh-seed-address-port ${ip} 3003"$'\n'
  done
fi

cat > /etc/aerospike/aerospike.conf <<CONF
service {
    cluster-name ${CLUSTER_NAME}
    proto-fd-max 15000
}

logging {
    file /var/log/aerospike/aerospike.log {
        context any info
    }
}

network {
    service {
        address any
        port 3000
    }

    fabric {
        address any
        port 3001
    }

    heartbeat {
        mode mesh
        address any
        port 3003
        interval 150
        timeout 10
${SEED_LINES}    }

    admin {
        address any
        port 3004
    }
}

namespace ${NAMESPACE_NAME} {
    replication-factor 2
    default-ttl 0
    indexes-memory-budget 4G

    storage-engine device {
        device ${NVME_DEVICE}
    }
}
CONF

systemctl daemon-reload
systemctl enable aerospike
systemctl start aerospike
STARTUP
  chmod +x "$TMP_BENCH/startup-aerospike.sh"
}

create_aerospike_nodes() {
  section "Step 4 / Create 3 Aerospike VMs"
  cat <<EOF
About to create 3 Aerospike VMs in $ZONE:
  • $AS_NODE1, $AS_NODE2, $AS_NODE3
  • Machine type: $AS_MACHINE_TYPE (4 vCPU, 16 GB RAM)
  • 30 GB boot disk + 1 local NVMe SSD each (NVMe is the storage engine)
  • Aerospike Community 8.1.1.1 auto-installs via startup script

Cluster topology is built incrementally:
  1. Node 1 starts with no seeds (it's the founding member)
  2. Node 2 starts with node 1 as seed
  3. Node 3 starts with nodes 1 & 2 as seeds
  4. After all are up, node 1 & 2 configs are rewritten with the full
     mesh and Aerospike is restarted on them.

Total time: ~3-5 minutes (apt + Aerospike download dominate).
EOF
  pause

  local existing=()
  for N in "$AS_NODE1" "$AS_NODE2" "$AS_NODE3"; do
    if gcloud compute instances describe "$N" --zone="$ZONE" &>/dev/null; then
      existing+=("$N")
    fi
  done

  if (( ${#existing[@]} > 0 )); then
    warn "Already exist: ${existing[*]}"
    if confirm "Skip Aerospike VM creation and reuse these?" "Y"; then
      info "Skipping VM creation."
      AS_NODE1_IP=$(get_ip "$AS_NODE1")
      AS_NODE2_IP=$(get_ip "$AS_NODE2")
      AS_NODE3_IP=$(get_ip "$AS_NODE3")
      print_as_ips
      return 0
    fi
    err "Refusing to recreate over existing VMs. Delete them first or pick different names."
    exit 1
  fi

  write_startup_script

  local common_meta="cluster-name=${CLUSTER_NAME},namespace-name=${NAMESPACE_NAME},nvme-device=${NVME_DEVICE},aerospike-url=${AEROSPIKE_URL},aerospike-dir=${AEROSPIKE_DIR}"

  info "Creating $AS_NODE1 (no seeds — founding member)..."
  gcloud compute instances create "$AS_NODE1" \
    --project="$PROJECT_ID" --zone="$ZONE" --machine-type="$AS_MACHINE_TYPE" \
    --image-family="$IMAGE_FAMILY" --image-project="$IMAGE_PROJECT" \
    --boot-disk-size=30GB --boot-disk-type=pd-balanced \
    --local-ssd=interface=nvme --tags=aerospike \
    --metadata="${common_meta},seed-ips=" \
    --metadata-from-file="startup-script=$TMP_BENCH/startup-aerospike.sh"

  info "Waiting for $AS_NODE1 to get an internal IP..."
  AS_NODE1_IP=$(wait_for_ip "$AS_NODE1")
  ok "$AS_NODE1: $AS_NODE1_IP"

  info "Creating $AS_NODE2 (seed: $AS_NODE1_IP)..."
  gcloud compute instances create "$AS_NODE2" \
    --project="$PROJECT_ID" --zone="$ZONE" --machine-type="$AS_MACHINE_TYPE" \
    --image-family="$IMAGE_FAMILY" --image-project="$IMAGE_PROJECT" \
    --boot-disk-size=30GB --boot-disk-type=pd-balanced \
    --local-ssd=interface=nvme --tags=aerospike \
    --metadata="${common_meta},seed-ips=$AS_NODE1_IP" \
    --metadata-from-file="startup-script=$TMP_BENCH/startup-aerospike.sh"

  AS_NODE2_IP=$(wait_for_ip "$AS_NODE2")
  ok "$AS_NODE2: $AS_NODE2_IP"

  # node 3's seed list contains a comma, which conflicts with the
  # --metadata "key=value,key=value" syntax — use a file for that key.
  echo "${AS_NODE1_IP},${AS_NODE2_IP}" > "$TMP_BENCH/node3-seeds.txt"

  info "Creating $AS_NODE3 (seeds: $AS_NODE1_IP,$AS_NODE2_IP)..."
  gcloud compute instances create "$AS_NODE3" \
    --project="$PROJECT_ID" --zone="$ZONE" --machine-type="$AS_MACHINE_TYPE" \
    --image-family="$IMAGE_FAMILY" --image-project="$IMAGE_PROJECT" \
    --boot-disk-size=30GB --boot-disk-type=pd-balanced \
    --local-ssd=interface=nvme --tags=aerospike \
    --metadata="${common_meta}" \
    --metadata-from-file="startup-script=$TMP_BENCH/startup-aerospike.sh,seed-ips=$TMP_BENCH/node3-seeds.txt"

  AS_NODE3_IP=$(wait_for_ip "$AS_NODE3")
  ok "$AS_NODE3: $AS_NODE3_IP"

  echo ""
  print_as_ips
}

get_ip() {
  gcloud compute instances describe "$1" \
    --project="$PROJECT_ID" --zone="$ZONE" \
    --format='get(networkInterfaces[0].networkIP)' 2>/dev/null
}

wait_for_ip() {
  local node="$1" ip="" tries=0
  while (( tries < 30 )); do
    ip=$(get_ip "$node")
    [[ -n "$ip" ]] && { echo "$ip"; return 0; }
    sleep 5
    tries=$((tries + 1))
  done
  err "Timed out waiting for IP of $node"
  return 1
}

print_as_ips() {
  echo "Aerospike node IPs:"
  echo "  $AS_NODE1: $AS_NODE1_IP"
  echo "  $AS_NODE2: $AS_NODE2_IP"
  echo "  $AS_NODE3: $AS_NODE3_IP"
}

update_mesh_configs() {
  section "Step 5 / Update node 1 & 2 configs for full 3-node mesh"
  cat <<EOF
Node 3 already started with both other IPs as seeds, but nodes 1 and
2 only knew about the nodes that existed when they booted. Rewriting
their /etc/aerospike/aerospike.conf to include the full mesh and
restarting Aerospike on each.

Waiting first ~60s for the apt + Aerospike install on all 3 nodes to
finish before we touch them.
EOF
  pause

  info "Waiting 60s for Aerospike install to finish on all 3 nodes..."
  sleep 60

  # Wait for SSH + aerospike service to actually be up before pushing config.
  for N in "$AS_NODE1" "$AS_NODE2" "$AS_NODE3"; do
    info "Checking Aerospike service on $N..."
    local tries=0 ok_seen=0
    while (( tries < 24 )); do
      if gcloud compute ssh "$N" --zone="$ZONE" --command 'systemctl is-active --quiet aerospike' &>/dev/null; then
        ok "$N: aerospike is active"
        ok_seen=1
        break
      fi
      sleep 10
      tries=$((tries + 1))
    done
    if (( ok_seen == 0 )); then
      warn "$N: aerospike service did not come up within 4 min — check"
      warn "    gcloud compute ssh $N --zone $ZONE --command 'sudo cat /var/log/startup-aerospike.log | tail -50'"
    fi
  done

  push_mesh_config "$AS_NODE1" "$AS_NODE2_IP" "$AS_NODE3_IP"
  push_mesh_config "$AS_NODE2" "$AS_NODE1_IP" "$AS_NODE3_IP"
  ok "Node 1 & 2 mesh configs updated and Aerospike restarted."
}

push_mesh_config() {
  local node="$1" seed1="$2" seed2="$3"
  info "Pushing full-mesh config to $node (seeds: $seed1, $seed2)..."
  aerospike_config "$seed1" "$seed2" > "$TMP_BENCH/aerospike.conf"
  gcloud compute scp "$TMP_BENCH/aerospike.conf" "${node}:/tmp/aerospike.conf" --zone="$ZONE" >/dev/null
  gcloud compute ssh "$node" --zone="$ZONE" --command \
    'sudo mv /tmp/aerospike.conf /etc/aerospike/aerospike.conf && sudo systemctl restart aerospike'
}

verify_aerospike_cluster() {
  section "Step 6 / Verify Aerospike cluster health"
  info "Giving the restarted nodes 15s to rejoin the mesh..."
  sleep 15

  info "Running 'asadm -e info' on $AS_NODE1 — expect 3 nodes listed:"
  gcloud compute ssh "$AS_NODE1" --zone="$ZONE" --command 'asadm -e "info"' || \
    warn "asadm failed — check 'sudo journalctl -u aerospike' on the node."
  echo ""
  warn "Cluster should show 3 members. If you see fewer, check"
  warn "    sudo tail -f /var/log/aerospike/aerospike.log"
  warn "    sudo cat /var/log/startup-aerospike.log"
}

create_sptag_node() {
  section "Step 7 / Create the SPTAG benchmark VM"
  cat <<EOF
About to create 1 SPTAG VM:
  • $SPTAG_NODE
  • Machine type: $SPTAG_MACHINE_TYPE (compute-optimized; 8 vCPU, 32 GB)
  • 250 GB boot disk + 1 local NVMe SSD (for the SIFT1B dataset)

This VM holds the dataset and runs the SPTAG benchmark inside Docker
against the Aerospike cluster.
EOF
  pause

  if gcloud compute instances describe "$SPTAG_NODE" --zone="$ZONE" &>/dev/null; then
    warn "$SPTAG_NODE already exists."
    if confirm "Skip SPTAG VM creation and reuse it?" "Y"; then
      info "Skipping SPTAG VM creation."
      return 0
    fi
    err "Refusing to recreate over the existing VM. Delete it first or pick a different name."
    exit 1
  fi

  gcloud compute instances create "$SPTAG_NODE" \
    --project="$PROJECT_ID" --zone="$ZONE" --machine-type="$SPTAG_MACHINE_TYPE" \
    --subnet=default \
    --tags=http-server,https-server \
    --create-disk="auto-delete=yes,boot=yes,size=250GB,type=pd-standard,image-family=$IMAGE_FAMILY,image-project=$IMAGE_PROJECT" \
    --local-ssd=interface=NVME \
    --scopes=default

  info "Waiting 30s for $SPTAG_NODE to finish booting..."
  sleep 30
  ok "SPTAG VM ready."
}

setup_sptag_node() {
  section "Step 8 / Install Docker + build SPTAG image on $SPTAG_NODE"
  cat <<EOF
On the SPTAG VM:
  • apt-get install build deps + Docker + axel
  • git clone --recursive RAG_StormX
  • docker build -t sptag .  (≈ 10-15 min on first build)

Don't worry if the docker build prints red warning lines — those come
from the SPTAG codebase (and a couple of unused-method warnings we
added). They don't affect the binary.
EOF
  pause

  gcloud compute ssh "$SPTAG_NODE" --zone="$ZONE" --command "
    set -e
    sudo apt-get update -qq
    sudo apt-get install -y -qq git build-essential cmake apt-utils docker.io pkg-config libssl-dev libaio-dev python3-pip curl unzip wget axel

    sudo systemctl enable docker
    sudo systemctl start docker

    cd ~
    if [ ! -d RAG_StormX ]; then
      git clone --recursive https://github.com/BU-EC528-Spring-2026/RAG_StormX.git
    else
      echo 'RAG_StormX already cloned, skipping clone.'
    fi

    cd ~/RAG_StormX/SPTAG
    sudo docker build -t sptag .
  "
  ok "SPTAG image built on $SPTAG_NODE."
}

format_nvme_sptag() {
  section "Step 9 / Format and mount NVMe on $SPTAG_NODE"
  cat <<EOF
Formatting /dev/nvme0n1 as ext4 (no journal, max throughput) and
mounting at /mnt/nvme. Skipped if already mounted.

Note: the SPTAG node's NVMe is at /dev/nvme0n1 (kernel name); the
Aerospike nodes use /dev/disk/by-id/google-local-nvme-ssd-0 because
Aerospike binds at install time and that path is stable across reboots.
EOF
  pause

  gcloud compute ssh "$SPTAG_NODE" --zone="$ZONE" --command "
    if mountpoint -q /mnt/nvme; then
      echo '/mnt/nvme already mounted — skipping mkfs.'
    else
      sudo mkfs.ext4 -O ^has_journal /dev/nvme0n1
      sudo mkdir -p /mnt/nvme
      sudo mount /dev/nvme0n1 /mnt/nvme
      sudo chown -R \$USER:\$USER /mnt/nvme
    fi
    mkdir -p /mnt/nvme/sift1b /mnt/nvme/sptag_bench
  "
  ok "NVMe ready."
}

download_dataset() {
  section "Step 10 / Download SIFT1B dataset on $SPTAG_NODE"
  cat <<EOF
Files we need (from the billion-scale-ann-benchmarks bigann dataset):
  • query.public.10K.u8bin   (~1.3 MB)
  • GT.public.1B.ibin        (~7.7 MB)
  • base.1B.u8bin            (~119 GB)  ← takes ~10 min at 250+ MB/s

The big file is downloaded with axel using 16 parallel connections.
Files already present at the right size are skipped automatically.

⚠  This step downloads ~119 GB. If your Cloud Shell connection drops
   during the download, just re-run the script — axel resumes;
   complete files are skipped.
EOF
  if ! confirm "Start dataset download?" "Y"; then
    warn "Skipping dataset download. The benchmark will fail without it."
    return
  fi

  gcloud compute ssh "$SPTAG_NODE" --zone="$ZONE" --command 'bash -s' <<'REMOTE'
set -e
mkdir -p /mnt/nvme/sift1b
cd /mnt/nvme/sift1b

dl() {
  local url="$1" out="$2" min_size="$3"
  if [[ -f "$out" ]] && [[ "$(stat -c %s "$out")" -ge "$min_size" ]]; then
    echo "  $out already present ($(stat -c %s "$out") bytes), skipping."
    return
  fi
  echo "  Downloading $out ..."
  if [[ "$out" == "base.1B.u8bin" ]] && command -v axel >/dev/null; then
    axel -n 16 -o "$out" "$url"
  else
    wget -O "$out" "$url"
  fi
}

dl https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/query.public.10K.u8bin query.public.10K.u8bin 1000000
dl https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/GT.public.1B.ibin       GT.public.1B.ibin       7000000
dl https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/base.1B.u8bin           base.1B.u8bin           100000000000

echo ""
echo "Files in /mnt/nvme/sift1b/:"
ls -lh /mnt/nvme/sift1b/
REMOTE
  ok "Dataset ready on $SPTAG_NODE."
}

maybe_create_100m_subset() {
  section "Step 11 / Optional: create 100M-vector subset"
  cat <<EOF
The default benchmark runs at 1M-vector scale (~5 min per run).
If you want to also run 100M scale (~12+ HOURS, needs ~64 GB RAM),
we can build base.100M.u8bin from the first 100M vectors of base.1B.u8bin.

⚠  100M scale is too big for $SPTAG_MACHINE_TYPE — only 32 GB RAM.
   Skip unless you've sized up the SPTAG node and plan to run inside
   tmux on the VM.
EOF
  if ! confirm "Create 100M subset now?" "N"; then
    info "Skipping 100M subset."
    return
  fi

  gcloud compute ssh "$SPTAG_NODE" --zone="$ZONE" --command "$(cat <<'REMOTE'
python3 <<'PYEOF'
import os, struct
src = '/mnt/nvme/sift1b/base.1B.u8bin'
dst = '/mnt/nvme/sift1b/base.100M.u8bin'
topk = 100_000_000
chunk = 64 * 1024 * 1024
with open(src, 'rb') as fsrc:
    header = fsrc.read(8)
    n, d = struct.unpack('II', header)
    need = topk * d
    with open(dst, 'wb') as fdst:
        fdst.write(struct.pack('II', topk, d))
        remaining = need
        while remaining:
            r = min(chunk, remaining)
            buf = fsrc.read(r)
            if len(buf) != r:
                raise SystemExit('short read')
            fdst.write(buf)
            remaining -= r
print('wrote', dst, 'bytes', os.path.getsize(dst))
PYEOF
ls -lh /mnt/nvme/sift1b/
REMOTE
)"
  ok "100M subset created."
}

run_benchmark() {
  section "Step 12 / Run the SPFresh benchmark in Docker"

  # Make sure we know the AS host IP (e.g. when reusing existing VMs).
  if [[ -z "$AS_NODE1_IP" ]]; then
    AS_NODE1_IP=$(get_ip "$AS_NODE1")
  fi

  cat <<EOF
Benchmark settings:
  SPTAG_AEROSPIKE_HOST       = $AS_NODE1_IP  (any AS node IP works)
  SPTAG_AEROSPIKE_PORT       = 3000
  SPTAG_AEROSPIKE_NAMESPACE  = $NAMESPACE_NAME
  SPTAG_AEROSPIKE_SET        = sptag
  SPTAG_AEROSPIKE_BIN        = value
  SPTAG_AEROSPIKE_UDF_MODE   = $SPTAG_UDF_MODE  $([[ "$SPTAG_UDF_MODE" == "0" ]] && echo "(Off — bulk read path)" || echo "(REQUIRES UDF DEPLOYED!)")
  BENCHMARK_CONFIG           = /work/benchmarks/benchmark.aerospike.nvme.ini
  BENCHMARK_OUTPUT           = /mnt/nvme/sptag_bench/output_aerospike.json

Running inside the sptag Docker container with:
  -v ~/RAG_StormX/SPTAG:/work
  -v /mnt/nvme:/mnt/nvme
  --net=host  (so the client can reach Aerospike on the VPC)

⚠ Default 1M-vector scale takes ~5 min; ground-truth computation adds
  extra time on the first run.
EOF
  pause

  gcloud compute ssh "$SPTAG_NODE" --zone="$ZONE" --command "
    set -e
    cd ~/RAG_StormX/SPTAG
    rm -f perftest_vector.bin perftest_meta.bin perftest_metaidx.bin \
      perftest_addvector.bin perftest_addmeta.bin perftest_addmetaidx.bin \
      perftest_query.bin perftest_batchtruth.* 2>/dev/null || true
    rm -rf proidx/spann_index_aero proidx/spann_index_aero_* 2>/dev/null || true
    mkdir -p /mnt/nvme/sptag_bench

    sudo docker run --rm --net=host \
      -e BENCHMARK_CONFIG=/work/benchmarks/benchmark.aerospike.nvme.ini \
      -e BENCHMARK_OUTPUT=/mnt/nvme/sptag_bench/output_aerospike.json \
      -e SPTAG_AEROSPIKE_HOST=$AS_NODE1_IP \
      -e SPTAG_AEROSPIKE_PORT=3000 \
      -e SPTAG_AEROSPIKE_NAMESPACE=$NAMESPACE_NAME \
      -e SPTAG_AEROSPIKE_SET=sptag \
      -e SPTAG_AEROSPIKE_BIN=value \
      -e SPTAG_AEROSPIKE_UDF_MODE=$SPTAG_UDF_MODE \
      -v ~/RAG_StormX/SPTAG:/work \
      -v /mnt/nvme:/mnt/nvme \
      sptag bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig --log_level=test_suite'
  "
  ok "Benchmark complete."
}

view_results() {
  section "Step 13 / Results"
  gcloud compute ssh "$SPTAG_NODE" --zone="$ZONE" --command "
    echo '--- output_aerospike.json ---'
    cat /mnt/nvme/sptag_bench/output_aerospike.json
  "
  cat <<EOF

What to expect (UDF Off / bulk-read baseline at 1M scale):
  • Mean search latency: ~2-3 ms
  • Recall@10:           ~0.81 → ~0.84 (rises with each insert batch)
  • QPS:                 depends heavily on $SPTAG_MACHINE_TYPE network/CPU

Comparing with the project's checked-in artifacts (results/output_aerospike_no_udf.json):
  • Filtered-read UDF: ~10× latency regression (per-key UDF dispatch).
  • Merge UDF:         broken — recall collapses to ~0.04. Don't use.
  • Server-side scoring (Off vs Pairs/Packed): see readme2.md §3.
EOF
}

cleanup_prompt() {
  section "Step 14 / Cleanup"
  cat <<EOF
You now have 4 VMs accruing GCP cost ($AS_NODE1, $AS_NODE2, $AS_NODE3,
$SPTAG_NODE) plus 1 firewall rule. Recommended: delete them now if you're done.

Will delete:
  • $AS_NODE1, $AS_NODE2, $AS_NODE3, $SPTAG_NODE  (in $ZONE)
  • Firewall rule 'aerospike-internal'
EOF
  if ! confirm "Delete all 4 VMs + firewall now?" "N"; then
    info "Skipping cleanup. To clean up later, run:"
    echo "  gcloud compute instances delete $AS_NODE1 $AS_NODE2 $AS_NODE3 $SPTAG_NODE --zone $ZONE --quiet"
    echo "  gcloud compute firewall-rules delete aerospike-internal --quiet"
    return
  fi
  local reply
  read -r -p "${C_RED}Type 'yes' to confirm deletion: ${C_RESET}" reply
  if [[ "$reply" != "yes" ]]; then
    info "Cleanup aborted."
    return
  fi

  gcloud compute instances delete "$AS_NODE1" "$AS_NODE2" "$AS_NODE3" "$SPTAG_NODE" --zone "$ZONE" --quiet \
    || warn "(some instance deletions may have failed — check the console)"
  gcloud compute firewall-rules delete aerospike-internal --quiet \
    || warn "(firewall rule may already be gone)"
  ok "Cleanup complete."
}

main() {
  show_welcome
  check_prerequisites
  collect_config

  create_firewall;          pause
  create_aerospike_nodes;   pause
  update_mesh_configs;      pause
  verify_aerospike_cluster; pause
  create_sptag_node;        pause
  setup_sptag_node;         pause
  format_nvme_sptag;        pause
  download_dataset;         pause
  maybe_create_100m_subset; pause
  run_benchmark;            pause
  view_results;             pause
  cleanup_prompt

  echo ""
  ok "All done. Workflow finished."
}

main "$@"
