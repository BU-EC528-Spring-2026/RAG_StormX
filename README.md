# Benchmark on FileIO, RocksDB, TiKV(single node)
This README explains how to run benchmarks for FileIO, RocksDB, single-node TiKV, and a three-physical-node TiKV cluster. The first part covers the single-node benchmark setup, and the second part covers the three-node TiKV setup.

## Provision the GCP Instance

> [!IMPORTANT]
> The following command should be ran in the Google Shell, not the aerospike node shell.

In the Google Cloud Shell (press G and S in the google cloud VM-instance window):

**Run in Google Cloud Shell:**

```bash
# 1. Provision the GCP Instance
gcloud compute instances create sptag-node \
    --zone=us-east4-b \
    --machine-type=c2-standard-8 \
    --subnet=default \
    --tags=http-server,https-server \
    --create-disk=auto-delete=yes,boot=yes,size=250GB,type=pd-standard,image-family=ubuntu-2404-lts-amd64,image-project=ubuntu-os-cloud \
    --local-ssd=interface=NVME \
    --scopes=default \
    --labels=goog-ops-agent=v2-x86-template
```

---

## Prepare the SPTAG VM and Docker Environment

Then SSH into the provisioned VM, and run the following:

> [!NOTE]
> When you get to the Docker Build Phase, do not worry about the red text - those are warnings that came along with the SPTAG codebase. We also have unused methods which cause warnings, but they do not affect the ability of the code to run.

**Run on the provisioned SPTAG VM after SSHing in:**

```bash
# 1. Install all of the required tools (git, Docker, etc.):
sudo apt-get update
sudo apt-get install -y \
    git \
    build-essential \
    cmake \
    apt-utils \
    docker.io \
    docker-compose-v2 \
    pkg-config \
    libssl-dev \
    libaio-dev \
    python3-pip \
    curl \
    unzip \
    wget \
    axel

sudo systemctl enable docker
sudo systemctl start docker

# 2. Clone the Repository (with submodules)
# You will need to authenticate with GitHub!!!!! We will that as an excercise to the reader :) 
# (Just add an ssh key :^) )
GIT_LFS_SKIP_SMUDGE=1 git clone --recursive https://github.com/BU-EC528-Spring-2026/RAG_StormX.git
cd RAG_StormX/SPTAG

# 3. Build and Run the Docker Image (It takes around 10 mins)
sudo docker build -t sptag .
```
---

## Prepare NVMe Storage

The SPTAG node you provisioned earlier includes a local NVMe SSD. Format and mount it for use by the dataset and benchmark artifacts.

> [!NOTE]
> The NVMe device name may vary. Run `lsblk | grep nvme` to identify yours (e.g. `/dev/nvme0n1`).

**Run on the SPTAG VM (not inside Docker):**

```bash
# Identify your NVMe device
lsblk | grep nvme

# Format with ext4 (no journal for benchmark performance)
sudo mkfs.ext4 -O ^has_journal /dev/nvme0n1

# Mount
sudo mkdir -p /mnt/nvme
sudo mount /dev/nvme0n1 /mnt/nvme
sudo chown -R "$USER":"$USER" /mnt/nvme

# Create directories for the dataset and benchmark artifacts
mkdir -p /mnt/nvme/sift1b
mkdir -p /mnt/nvme/sptag_bench
```

## Download the SIFT1B (BigANN) Dataset

Source: [https://big-ann-benchmarks.com/](https://big-ann-benchmarks.com/)

> [!WARNING]
> The base vector file is **119 GB**. Using `axel` with 16 parallel connections, this takes roughly **10 minutes** at ~250 MB/s on GCP. On slower connections it will take significantly longer. The query and ground-truth files are small (under 10 MB each).

**Run on the SPTAG VM (not inside Docker):**

```bash
cd /mnt/nvme/sift1b

# Query vectors (1.3 MB) and ground truth (7.7 MB)
wget -O query.public.10K.u8bin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/query.public.10K.u8bin
wget -O GT.public.1B.ibin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/GT.public.1B.ibin

# Base vectors (119 GB) - axel uses parallel segmented download with resume support
axel -n 16 -o base.1B.u8bin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/base.1B.u8bin
```
### Create 100M Subset

The full 1B base is too large for feasible benchmarks. Extract the first 100M vectors (~12 GB) (this will take some time):

```bash
python3 - <<'PY'
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
PY
```

### Verify the Dataset

```bash
ls -lh /mnt/nvme/sift1b/
# Expected:
#   base.1B.u8bin          120G   (1 billion vectors, dim=128, uint8)
#   base.100M.u8bin         12G   (100 million vectors, dim=128, uint8)
#   query.public.10K.u8bin 1.3M   (10,000 query vectors)
#   GT.public.1B.ibin      7.7M   (ground truth, 100 nearest neighbors per query)
```
File format: `u8bin` = 8-byte header (uint32 num_vectors, uint32 dimension) followed by row-major uint8 vectors.

## Run Benchmark
> [!WARNING]
> Each benchmark run at the default 1M-vector scale (100k base + 9 batches of 100k inserts) takes approximately **5 minutes**. Ground truth computation adds some overhead on the first run.

> [!IMPORTANT]
> Before any benchmark rerun, clear stale `perftest_*` cache files from `~/RAG_StormX/SPTAG` (and optionally `~/RAG_StormX`) to avoid mixed-scale cache reuse.

```bash
cd ~/RAG_StormX/SPTAG
rm -f perftest_vector.bin perftest_meta.bin perftest_metaidx.bin \
  perftest_addvector.bin perftest_addmeta.bin perftest_addmetaidx.bin \
  perftest_query.bin perftest_truth.* perftest_addtruth.* \
  perftest_batchtruth.* perftest_quanvectors.bin
```

## 1) Run FileIO Benchmark
```bash
cd ~/RAG_StormX/SPTAG
sudo docker run --rm --net=host \
  -e BENCHMARK_CONFIG=/work/benchmarks/benchmark.fileio.nvme.ini \
  -e BENCHMARK_OUTPUT=/mnt/nvme/sptag_bench/output_fileio.json \
  -v ~/RAG_StormX:/work \
  -v /mnt/nvme:/mnt/nvme \
  sptag bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig'
```

## 2) RocksDB on NVMe

```bash
cd ~/RAG_StormX/SPTAG
sudo docker run --rm --net=host \
  -e BENCHMARK_CONFIG=/work/benchmarks/benchmark.rocksdb.nvme.ini \
  -e BENCHMARK_OUTPUT=/mnt/nvme/sptag_bench/output_rocksdb.json \
  -v ~/RAG_StormX:/work \
  -v /mnt/nvme:/mnt/nvme \
  sptag bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig'
```

## 3) single-node TiKV (Shared NVMe Local Cluster)
This section adds a local TiKV benchmark path that uses a **single NVMe device** with sub-directories shared by a 3PD + 3TiKV cluster.

> [!NOTE]
> This setup is designed for local benchmark convenience on one host, not for production fault isolation. All TiKV nodes share one physical NVMe mount and use different sub-directories under the same root.

If `docker compose` is missing, install Compose v2 first:

```bash
sudo apt-get update
# Ubuntu default repositories (recommended for this guide)
sudo apt-get install -y docker-compose-v2
docker compose version
```

```bash
# Start TiKV cluster on shared NVMe
cd ~/RAG_StormX/benchmarks
NVME_ROOT=/mnt/nvme/tikv_shared ./start_tikv_shared_nvme.sh

# Verify PD can see TiKV stores before running benchmark
curl http://127.0.0.1:2379/pd/api/v1/stores

# Run benchmark in the Docker image (works even when host has no ./Release/SPTAGTest)
cd ~/RAG_StormX
sudo docker run --rm --net=host \
  -e BENCHMARK_CONFIG=/work/benchmarks/benchmark.tikv.nvme.ini \
  -e BENCHMARK_OUTPUT=/mnt/nvme/sptag_bench/output_tikv.json \
  -v ~/RAG_StormX:/work \
  -v /mnt/nvme:/mnt/nvme \
  sptag bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig --log_level=message'

# Stop TiKV cluster after run
cd ~/RAG_StormX/benchmarks
NVME_ROOT=/mnt/nvme/tikv_shared ./stop_tikv_shared_nvme.sh
```
---

## Where to Find Results

Each benchmark writes a JSON output file to the path specified by `BENCHMARK_OUTPUT`. After running all benchmarks, you will find:

| Backend | Output File |
| --- | --- |
| FileIO | `/mnt/nvme/sptag_bench/output_fileio.json` |
| RocksDB | `/mnt/nvme/sptag_bench/output_rocksdb.json` |
| TiKV | `/mnt/nvme/sptag_bench/output_tikv.json` |

To inspect results:

```bash
cat /mnt/nvme/sptag_bench/output_fileio.json
cat /mnt/nvme/sptag_bench/output_rocksdb.json
cat /mnt/nvme/sptag_bench/output_tikv.json
```

The benchmark config files used for each run are in the [`benchmarks/`](benchmarks/) directory. You can adjust parameters there (e.g. vector counts, thread counts, distance method) and re-run.

## Scaling Up

To benchmark at larger scale, edit the config files in `benchmarks/` and adjust the parameters below. Keep in mind that larger runs require significantly more RAM and time:

| Scale | BaseVectorCount | InsertVectorCount | BatchNum | Expected RAM | Expected Time |
| --- | --- | --- | --- | --- | --- |
| 1M (default) | 100,000 | 900,000 | 9 | ~4 GB | ~5 min |
| 10M | 1,000,000 | 9,000,000 | 9 | ~16 GB | ~1 hour |
| 100M | 10,000,000 | 90,000,000 | 9 | ~64 GB | ~12+ hours |

> [!WARNING]
> At the 100M scale, expect runs to take **12+ hours** and require at least **64 GB of RAM**. Make sure your SPTAG VM has enough resources and that the benchmark is running inside a `tmux` or `screen` session.


# Benchmark on TiKV(3 physical nodes)
## PART 1: TiKV 3-Node Distributed Cluster on GCP Setup Guidance

This guide walks through how to deploy a **truly distributed** 3-node TiKV cluster on Google Cloud, where each node runs on a separate physical VM with its own NVMe SSD. This contrasts with the [shared-NVMe local cluster](#run-tikv-benchmark-on-shared-nvme), which simulates 3 nodes on a single machine.

This setup will:

- Create 3 Google Compute Engine VMs (each with local NVMe SSD)
- Install Docker on all 3 nodes
- Deploy a 3-node PD (Placement Driver) cluster across all VMs
- Deploy a 3-node TiKV cluster (one TiKV per VM, each with its own NVMe)
- Set up the SPTAG benchmark environment on node 1
- Open the required internal firewall ports for PD and TiKV traffic

#### Why distribute across 3 physical nodes?

The [shared-NVMe setup](#run-tikv-benchmark-on-shared-nvme) runs 3 PD + 3 TiKV processes on a single machine using `127.0.0.1` with different ports. While convenient, this means:

- All TiKV nodes **share one NVMe SSD** (I/O contention)
- Raft replication traffic stays on **localhost** (zero network latency)
- No real fault isolation

The distributed setup uses **real internal IPs** for Raft consensus and data replication, giving production-realistic latency and I/O characteristics. Each TiKV node gets a **dedicated NVMe SSD**, eliminating I/O contention.

#### Prerequisites

Before starting, ensure:

- You have access to a Google Cloud project with billing enabled.
- Compute Engine API is enabled: `gcloud services enable compute.googleapis.com`
- You have at least **12 vCPUs** of quota available (3 × n2-standard-4). Check via **IAM & Admin → Quotas → CPUS (all regions)**.
- You are using **Google Cloud Shell** or have `gcloud` installed locally.

> [!NOTE]
> If you also need a separate SPTAG node running simultaneously, you will need additional CPU quota (request an increase to 20+ via the Quotas page). In this guide, SPTAG benchmarks run directly on tikv-node-1.

### Deployment Steps

### 1. Find and Set Your Project

```bash
gcloud config set project YOUR_PROJECT_ID
gcloud config get-value project
```

### 2. Create the Deployment Script

Create a file named `deploy-tikv.sh` and paste the following bash script into it.

> [!IMPORTANT]
> Make sure to edit the `PROJECT_ID` at the top of the file to match your environment.
> The below script should be run in Google Cloud Shell.

```bash
#!/bin/bash
set -euo pipefail

# =========================
# USER-EDITABLE SETTINGS
# =========================
PROJECT_ID="YOUR-PROJECT-NAME"
ZONE="us-east4-b"
MACHINE_TYPE="n2-standard-4"
IMAGE_FAMILY="ubuntu-2404-lts-amd64"
IMAGE_PROJECT="ubuntu-os-cloud"
NODE1="tikv-node-1"
NODE2="tikv-node-2"
NODE3="tikv-node-3"

# Set the active gcloud project
gcloud config set project "${PROJECT_ID}"

# Create firewall rule for TiKV/PD internal traffic
gcloud compute firewall-rules create tikv-internal \
  --network=default \
  --allow tcp:2379-2580,tcp:20160-20182 \
  --source-tags=tikv \
  --target-tags=tikv || true

# =========================
# CREATE 3 VMs
# =========================
# Node 1: TiKV + PD + SPTAG benchmark (250GB disk for dataset)
echo "Creating ${NODE1} (benchmark node + TiKV)..."
gcloud compute instances create "${NODE1}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --machine-type="${MACHINE_TYPE}" \
  --image-family="${IMAGE_FAMILY}" \
  --image-project="${IMAGE_PROJECT}" \
  --boot-disk-size=250GB \
  --boot-disk-type=pd-standard \
  --local-ssd=interface=nvme \
  --tags=tikv \
  --scopes=default

echo "Creating ${NODE2}..."
gcloud compute instances create "${NODE2}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --machine-type="${MACHINE_TYPE}" \
  --image-family="${IMAGE_FAMILY}" \
  --image-project="${IMAGE_PROJECT}" \
  --boot-disk-size=30GB \
  --boot-disk-type=pd-balanced \
  --local-ssd=interface=nvme \
  --tags=tikv \
  --scopes=default

echo "Creating ${NODE3}..."
gcloud compute instances create "${NODE3}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --machine-type="${MACHINE_TYPE}" \
  --image-family="${IMAGE_FAMILY}" \
  --image-project="${IMAGE_PROJECT}" \
  --boot-disk-size=30GB \
  --boot-disk-type=pd-balanced \
  --local-ssd=interface=nvme \
  --tags=tikv \
  --scopes=default

echo "Waiting for VMs to boot..."
sleep 30

# Get internal IPs
NODE1_IP=$(gcloud compute instances describe "${NODE1}" \
  --project="${PROJECT_ID}" --zone="${ZONE}" \
  --format='get(networkInterfaces[0].networkIP)')
NODE2_IP=$(gcloud compute instances describe "${NODE2}" \
  --project="${PROJECT_ID}" --zone="${ZONE}" \
  --format='get(networkInterfaces[0].networkIP)')
NODE3_IP=$(gcloud compute instances describe "${NODE3}" \
  --project="${PROJECT_ID}" --zone="${ZONE}" \
  --format='get(networkInterfaces[0].networkIP)')

echo "Node IPs:"
echo "  ${NODE1}: ${NODE1_IP}"
echo "  ${NODE2}: ${NODE2_IP}"
echo "  ${NODE3}: ${NODE3_IP}"

# =========================
# INSTALL DOCKER ON ALL 3 NODES
# =========================
for NODE in "${NODE1}" "${NODE2}" "${NODE3}"; do
  echo "Installing Docker on ${NODE}..."
  gcloud compute ssh "${NODE}" --zone="${ZONE}" --command "
    sudo apt-get update -qq
    sudo apt-get install -y -qq docker.io docker-compose-v2
    sudo systemctl enable docker
    sudo systemctl start docker
  "
done

# =========================
# FORMAT NVMe ON ALL 3 NODES
# =========================
for NODE in "${NODE1}" "${NODE2}" "${NODE3}"; do
  echo "Formatting NVMe on ${NODE}..."
  gcloud compute ssh "${NODE}" --zone="${ZONE}" --command "
    sudo mkfs.ext4 -O ^has_journal /dev/nvme0n1
    sudo mkdir -p /mnt/nvme
    sudo mount /dev/nvme0n1 /mnt/nvme
    sudo chown -R \$USER:\$USER /mnt/nvme
    mkdir -p /mnt/nvme/tikv
  "
done

# =========================
# START PD ON ALL 3 NODES
# =========================
echo "Starting PD on all 3 nodes..."

gcloud compute ssh "${NODE1}" --zone="${ZONE}" --command "
  sudo docker run -d --name pd --net=host --restart=unless-stopped \
    -v /mnt/nvme/tikv/pd:/var/lib/pd \
    pingcap/pd:v7.5.1 \
    --name=pd0 \
    --client-urls=http://0.0.0.0:2379 \
    --peer-urls=http://0.0.0.0:2380 \
    --advertise-client-urls=http://${NODE1_IP}:2379 \
    --advertise-peer-urls=http://${NODE1_IP}:2380 \
    --initial-cluster='pd0=http://${NODE1_IP}:2380,pd1=http://${NODE2_IP}:2380,pd2=http://${NODE3_IP}:2380' \
    --data-dir=/var/lib/pd
"

gcloud compute ssh "${NODE2}" --zone="${ZONE}" --command "
  sudo docker run -d --name pd --net=host --restart=unless-stopped \
    -v /mnt/nvme/tikv/pd:/var/lib/pd \
    pingcap/pd:v7.5.1 \
    --name=pd1 \
    --client-urls=http://0.0.0.0:2379 \
    --peer-urls=http://0.0.0.0:2380 \
    --advertise-client-urls=http://${NODE2_IP}:2379 \
    --advertise-peer-urls=http://${NODE2_IP}:2380 \
    --initial-cluster='pd0=http://${NODE1_IP}:2380,pd1=http://${NODE2_IP}:2380,pd2=http://${NODE3_IP}:2380' \
    --data-dir=/var/lib/pd
"

gcloud compute ssh "${NODE3}" --zone="${ZONE}" --command "
  sudo docker run -d --name pd --net=host --restart=unless-stopped \
    -v /mnt/nvme/tikv/pd:/var/lib/pd \
    pingcap/pd:v7.5.1 \
    --name=pd2 \
    --client-urls=http://0.0.0.0:2379 \
    --peer-urls=http://0.0.0.0:2380 \
    --advertise-client-urls=http://${NODE3_IP}:2379 \
    --advertise-peer-urls=http://${NODE3_IP}:2380 \
    --initial-cluster='pd0=http://${NODE1_IP}:2380,pd1=http://${NODE2_IP}:2380,pd2=http://${NODE3_IP}:2380' \
    --data-dir=/var/lib/pd
"

echo "Waiting for PD cluster to form..."
sleep 15

# =========================
# START TiKV ON ALL 3 NODES
# =========================
echo "Starting TiKV on all 3 nodes..."

PD_ENDPOINTS="${NODE1_IP}:2379,${NODE2_IP}:2379,${NODE3_IP}:2379"

gcloud compute ssh "${NODE1}" --zone="${ZONE}" --command "
  sudo docker run -d --name tikv --net=host --restart=unless-stopped \
    -v /mnt/nvme/tikv/data:/var/lib/tikv \
    pingcap/tikv:v7.5.1 \
    --addr=0.0.0.0:20160 \
    --advertise-addr=${NODE1_IP}:20160 \
    --status-addr=0.0.0.0:20180 \
    --data-dir=/var/lib/tikv \
    --pd=${PD_ENDPOINTS}
"

gcloud compute ssh "${NODE2}" --zone="${ZONE}" --command "
  sudo docker run -d --name tikv --net=host --restart=unless-stopped \
    -v /mnt/nvme/tikv/data:/var/lib/tikv \
    pingcap/tikv:v7.5.1 \
    --addr=0.0.0.0:20160 \
    --advertise-addr=${NODE2_IP}:20160 \
    --status-addr=0.0.0.0:20180 \
    --data-dir=/var/lib/tikv \
    --pd=${PD_ENDPOINTS}
"

gcloud compute ssh "${NODE3}" --zone="${ZONE}" --command "
  sudo docker run -d --name tikv --net=host --restart=unless-stopped \
    -v /mnt/nvme/tikv/data:/var/lib/tikv \
    pingcap/tikv:v7.5.1 \
    --addr=0.0.0.0:20160 \
    --advertise-addr=${NODE3_IP}:20160 \
    --status-addr=0.0.0.0:20180 \
    --data-dir=/var/lib/tikv \
    --pd=${PD_ENDPOINTS}
"

echo "Waiting for TiKV stores to register..."
sleep 20

# =========================
# VERIFY CLUSTER
# =========================
echo "Checking TiKV cluster status..."
gcloud compute ssh "${NODE1}" --zone="${ZONE}" --command "
  curl -s http://127.0.0.1:2379/pd/api/v1/stores | python3 -c '
import json,sys
d=json.load(sys.stdin)
stores=d.get(\"stores\",[])
print(f\"Total stores: {len(stores)}\")
for s in stores:
    store=s[\"store\"]
    print(f\"  Store {store[\"id\"]}: {store[\"address\"]} state={store[\"state_name\"]}\")
'
"

# =========================
# SETUP BENCHMARK ENV ON NODE 1
# =========================
echo "Setting up SPTAG benchmark environment on ${NODE1}..."
gcloud compute ssh "${NODE1}" --zone="${ZONE}" --command "
  sudo apt-get install -y -qq git build-essential cmake apt-utils pkg-config libssl-dev libaio-dev python3-pip curl unzip wget axel

  mkdir -p /mnt/nvme/sift1b
  mkdir -p /mnt/nvme/sptag_bench

  cd ~
  GIT_LFS_SKIP_SMUDGE=1 git clone --recursive https://github.com/BU-EC528-Spring-2026/RAG_StormX.git || true

  cd ~/RAG_StormX/SPTAG
  sudo docker build -t sptag .
"

echo ""
echo "============================================"
echo "DEPLOYMENT COMPLETE!"
echo "============================================"
echo ""
echo "Node IPs:"
echo "  ${NODE1}: ${NODE1_IP} (benchmark + TiKV + PD)"
echo "  ${NODE2}: ${NODE2_IP} (TiKV + PD)"
echo "  ${NODE3}: ${NODE3_IP} (TiKV + PD)"
echo ""
echo "PD endpoints: ${PD_ENDPOINTS}"
echo ""
echo "IMPORTANT: Update benchmarks/benchmark.tikv.nvme.ini"
echo "  Change TiKVPDAddresses to: ${PD_ENDPOINTS}"
```

### 3. Run the Script

```bash
chmod +x deploy-tikv.sh
./deploy-tikv.sh
```

The script takes approximately 15–20 minutes to complete (Docker image pulls and SPTAG build are the slowest steps).

### 4. Verification and Testing

SSH into node 1:

```bash
gcloud compute ssh tikv-node-1 --zone us-east4-b
```
(If you want to exit the node, simply type ```exit```.)
> [!IMPORTANT]
> Now you should be SSHed into tikv-node-1. The following commands should be run from the node itself.

**Run on tikv-node-1 after SSHing in:**

Check that all 3 TiKV stores are up:

```bash
curl -s http://127.0.0.1:2379/pd/api/v1/stores | python3 -m json.tool
```

You should see 3 stores with `"state_name": "Up"`.

Check Docker containers are running:

```bash
sudo docker ps
# Should show 'pd' and 'tikv' containers running
```

**Troubleshooting**

If a TiKV or PD container is not running, check its logs:

```bash
sudo docker logs pd
sudo docker logs tikv
```

If something fails heavily, you can wipe the cluster and start fresh:

```bash
# Run from Cloud Shell (not inside a node)
gcloud compute instances delete tikv-node-1 tikv-node-2 tikv-node-3 --zone us-east4-b --quiet
gcloud compute firewall-rules delete tikv-internal --quiet
```

---

## PART 2: Run TiKV Distributed Benchmark (After SSH into tikv-node-1)

This section runs the SPTAG benchmark against the **distributed 3-node TiKV cluster** deployed above. The benchmark runs on tikv-node-1 (which also hosts one TiKV + PD instance).

##### Prerequisites

- The 3-node TiKV cluster is running (all 3 stores showing `Up` via PD API).
- SPTAG Docker image is built on tikv-node-1.
- NVMe is mounted at `/mnt/nvme` on tikv-node-1.

### 1. Update Benchmark Config (Modify PD address to real IPs of 3 physical nodes created)

The default `benchmark.tikv.nvme.ini` points at `127.0.0.1` (local cluster). Update it to use the distributed PD endpoints.

**Run on tikv-node-1:**

```bash
sed -i 's|TiKVPDAddresses=127.0.0.1:2379,127.0.0.1:2479,127.0.0.1:2579|TiKVPDAddresses=NODE1_IP:2379,NODE2_IP:2379,NODE3_IP:2379|' ~/RAG_StormX/benchmarks/benchmark.tikv.nvme.ini
```

> [!IMPORTANT]
> Replace `NODE1_IP`, `NODE2_IP`, `NODE3_IP` with the actual internal IPs from the deployment output (e.g. `10.150.0.6`, `10.150.0.7`, `10.150.0.8`). Or use the `sed` command printed by `deploy-tikv.sh`.

Verify the change:

```bash
grep TiKVPDAddresses ~/RAG_StormX/benchmarks/benchmark.tikv.nvme.ini
# Should show: TiKVPDAddresses=10.x.x.x:2379,10.x.x.x:2379,10.x.x.x:2379
```

### 2. Download the SIFT1B Dataset

**Run on tikv-node-1 (recommend inside `tmux` for large downloads):**

```bash
tmux
cd /mnt/nvme/sift1b

# Query vectors (1.3 MB) and ground truth (7.7 MB)
wget -O query.public.10K.u8bin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/query.public.10K.u8bin
wget -O GT.public.1B.ibin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/GT.public.1B.ibin

# Base vectors (119 GB) — ~6 min at 300+ MB/s on GCP
axel -n 16 -o base.1B.u8bin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/base.1B.u8bin
```

### 3. Create 100M Subset

```bash
python3 -c "
import os, struct
src = '/mnt/nvme/sift1b/base.1B.u8bin'
dst = '/mnt/nvme/sift1b/base.100M.u8bin'
topk = 100000000
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
"
```

Verify the Dataset

```bash
ls -lh /mnt/nvme/sift1b/
# Expected:
#   base.1B.u8bin          120G
#   base.100M.u8bin         12G
#   query.public.10K.u8bin 1.3M
#   GT.public.1B.ibin      7.7M
```

### 4. Clean Perftest Cache and Run Benchmark

```bash
cd ~/RAG_StormX/SPTAG
rm -f perftest_vector.bin perftest_meta.bin perftest_metaidx.bin \
  perftest_addvector.bin perftest_addmeta.bin perftest_addmetaidx.bin \
  perftest_query.bin perftest_truth.* perftest_addtruth.* \
  perftest_batchtruth.* perftest_quanvectors.bin

cd ~/RAG_StormX
sudo docker run --rm --net=host \
  -e BENCHMARK_CONFIG=/work/benchmarks/benchmark.tikv.nvme.ini \
  -e BENCHMARK_OUTPUT=/mnt/nvme/sptag_bench/output_tikv_distributed.json \
  -v ~/RAG_StormX:/work \
  -v /mnt/nvme:/mnt/nvme \
  sptag bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig --log_level=message'
```

> [!WARNING]
> Each run at the default 1M-vector scale takes approximately **5 minutes**. Ground truth computation adds overhead on the first run.

### 5. View Results

```bash
cat /mnt/nvme/sptag_bench/output_tikv_distributed.json
```

### 6. Cleanup

Stop and delete all resources when done to save credits:
>[!NOTE]
> Alternatively, you could choose to manually shut down VM instances.

```bash
# inside tikv-node 1:
exit

# Run from Cloud Shell (outside of the node now!)
gcloud compute instances delete tikv-node-1 tikv-node-2 tikv-node-3 --zone us-east4-b --quiet
gcloud compute firewall-rules delete tikv-internal --quiet
```

### What to Expect When You See The Test Results（Distributed vs Shared-NVMe）

| Aspect | Shared NVMe (single machine) | Distributed (3 VMs) |
| --- | --- | --- |
| **Network latency** | Zero (localhost) | Real VPC latency (~0.1–0.5 ms per hop) |
| **Raft replication** | Local memory | Cross-network consensus |
| **I/O contention** | 3 TiKV share 1 NVMe | Each TiKV has dedicated NVMe |
| **Fault isolation** | None (single point of failure) | True — any node can fail independently |
| **Expected QPS** | Higher (no network overhead) | ~15–20% lower due to network latency |
| **Expected Recall** | Same | Same (identical algorithm) |

At the default 1M-vector scale, the distributed setup shows slightly lower QPS because network latency dominates over I/O savings. At larger scales (10M+ vectors), per-node dedicated NVMe reduces I/O contention and the gap narrows.

