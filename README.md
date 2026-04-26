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