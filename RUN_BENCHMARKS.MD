# SPTAG SIFT1B (BigANN) Benchmarks

This document describes how to download the SIFT1B dataset, build the SPTAG Docker image, configure benchmarks for three storage backends (FileIO, RocksDB, Aerospike), and run them on local NVMe SSD.

## Prerequisites

- Ubuntu host with NVMe SSD (`/dev/nvme0n1` or similar)
- Docker installed (`sudo` access required if user is not in `docker` group)
- `axel` or `wget` for downloading the dataset
- For Aerospike benchmarks: a running Aerospike node with a configured namespace

## 1. Prepare NVMe Storage

Format and mount the NVMe device:

```bash
# Identify NVMe device
lsblk | grep nvme

# Format with ext4 (no journal for benchmark performance)
sudo mkfs.ext4 -O ^has_journal /dev/nvme0n1

# Mount
sudo mkdir -p /mnt/nvme
sudo mount /dev/nvme0n1 /mnt/nvme
sudo chown -R "$USER":"$USER" /mnt/nvme

# Create directories for dataset and benchmark artifacts
mkdir -p /mnt/nvme/sift1b
mkdir -p /mnt/nvme/sptag_bench
```

## 2. Download SIFT1B (BigANN) Dataset

Source: [https://big-ann-benchmarks.com/](https://big-ann-benchmarks.com/)

```bash
cd /mnt/nvme/sift1b

# Query vectors (1.3 MB) and ground truth (7.7 MB)
wget -O query.public.10K.u8bin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/query.public.10K.u8bin
wget -O GT.public.1B.ibin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/GT.public.1B.ibin

# Base vectors (119 GB) - use axel for parallel segmented download with resume
axel -n 16 -o base.1B.u8bin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/base.1B.u8bin
```

If `axel` is not installed: `sudo apt-get install -y axel`

### Create 100M subset

The full 1B base is too large for feasible benchmarks. Extract the first 100M vectors:

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

### Verify dataset

```bash
ls -lh /mnt/nvme/sift1b/
# Expected:
#   base.1B.u8bin          120G   (1 billion vectors, dim=128, uint8)
#   base.100M.u8bin         12G   (100 million vectors, dim=128, uint8)
#   query.public.10K.u8bin 1.3M   (10,000 query vectors)
#   GT.public.1B.ibin      7.7M   (ground truth, 100 nearest neighbors per query)
```

File format: `u8bin` = 8-byte header (uint32 num_vectors, uint32 dimension) followed by row-major uint8 vectors.

## 3. Build SPTAG Docker Image

The Dockerfile builds SPTAG with RocksDB and Aerospike backends enabled:

```bash
cd ~/RAG_StormX/SPTAG
sudo docker build -t sptag .
```

This compiles with `-DROCKSDB=ON -DAEROSPIKE=ON`, producing `/app/Release/SPTAGTest` inside the image.

### Verify the image

```bash
sudo docker run --rm sptag bash -lc 'ls -lah /app/Release/SPTAGTest'
sudo docker run --rm sptag bash -lc 'strings /app/Release/SPTAGTest | grep -E "ROCKSDBIO|AEROSPIKEIO|FILEIO" | head'
```

## 4. Benchmark Configuration

Benchmark configs live in `benchmarks/` under the repo root. They are read by the `BenchmarkFromConfig` test case in `SPTAG/Test/src/SPFreshTest.cpp` via the `BENCHMARK_CONFIG` environment variable.

### Key parameters


| Parameter           | Description                                                                                                                               |
| ------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `VectorPath`        | Path to base vectors (u8bin format)                                                                                                       |
| `QueryPath`         | Path to query vectors (u8bin format)                                                                                                      |
| `TruthPath`         | Set to `truth` to enable ground-truth generation and recall computation. Set to `none` to skip recall and measure only throughput/latency |
| `IndexPath`         | Directory where the SPANN index will be built                                                                                             |
| `Storage`           | Backend: `FILEIO`, `ROCKSDBIO`, or `AEROSPIKEIO`                                                                                          |
| `BaseVectorCount`   | Number of vectors for the initial index build                                                                                             |
| `InsertVectorCount` | Number of vectors to insert after build (tests update perf)                                                                               |
| `BatchNum`          | Number of batches to split inserts into                                                                                                   |
| `TopK`              | Number of nearest neighbors to retrieve                                                                                                   |
| `NumThreads`        | Parallel search threads                                                                                                                   |
| `NumQueries`        | Number of queries per search round                                                                                                        |
| `Rebuild`           | `true` = build from scratch, `false` = load existing index                                                                                |
| `Resume`            | `-1` = no resume                                                                                                                          |


### Config files

**FileIO** (`benchmarks/benchmark.fileio.nvme.ini`):

```ini
[Benchmark]
VectorPath=/mnt/nvme/sift1b/base.100M.u8bin
QueryPath=/mnt/nvme/sift1b/query.public.10K.u8bin
TruthPath=truth
IndexPath=/mnt/nvme/sptag_bench/spann_index_fileio
ValueType=UInt8
Dimension=128
BaseVectorCount=100000
InsertVectorCount=900000
DeleteVectorCount=0
BatchNum=9
TopK=10
NumThreads=16
NumQueries=1000
DistMethod=L2
Rebuild=true
Resume=-1
Storage=FILEIO
```

**RocksDB** (`benchmarks/benchmark.rocksdb.nvme.ini`): same as above with `Storage=ROCKSDBIO` and `IndexPath=.../spann_index_rocksdb`.

**Aerospike** (`benchmarks/benchmark.aerospike.nvme.ini`): same as above with `Storage=AEROSPIKEIO` and `IndexPath=.../spann_index_aerospike`. Aerospike connection is configured via environment variables at runtime (not in the INI file).

## 5. Running Benchmarks

All benchmarks are run inside the Docker container. The `--run_test=SPFreshTest/BenchmarkFromConfig` flag runs only the benchmark test case, skipping the 150+ other unit tests.

### FileIO on NVMe

```bash
sudo docker run --rm --net=host \
  -e BENCHMARK_CONFIG=/work/benchmarks/benchmark.fileio.nvme.ini \
  -e BENCHMARK_OUTPUT=/mnt/nvme/sptag_bench/output_fileio.json \
  -v /home/Zhakh/RAG_StormX:/work \
  -v /mnt/nvme:/mnt/nvme \
  sptag bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig'
```

### RocksDB on NVMe

```bash
sudo docker run --rm --net=host \
  -e BENCHMARK_CONFIG=/work/benchmarks/benchmark.rocksdb.nvme.ini \
  -e BENCHMARK_OUTPUT=/mnt/nvme/sptag_bench/output_rocksdb.json \
  -v /home/Zhakh/RAG_StormX:/work \
  -v /mnt/nvme:/mnt/nvme \
  sptag bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig'
```

### Aerospike (remote node)

The Aerospike backend stores postings on a remote Aerospike cluster instead of local disk. Connection parameters can be passed as environment variables or in the benchmark INI's optional `[Aerospike]` section. Environment variables override the INI when both are set.


| Variable                    | Description         | Example       |
| --------------------------- | ------------------- | ------------- |
| `SPTAG_AEROSPIKE_HOST`      | Aerospike node internal IP | `10.150.0.34` |
| `SPTAG_AEROSPIKE_PORT`      | Aerospike port      | `3000`        |
| `SPTAG_AEROSPIKE_NAMESPACE` | Aerospike namespace | `sptag_data`  |
| `SPTAG_AEROSPIKE_SET`       | Aerospike set name  | `sptag`       |
| `SPTAG_AEROSPIKE_BIN`       | Aerospike bin name  | `value`       |


```bash
sudo docker run --rm --net=host \
  -e BENCHMARK_CONFIG=/work/SPTAG/benchmarks/benchmark.aerospike.nvme.ini \
  -e BENCHMARK_OUTPUT=/mnt/nvme/sptag_bench/output_aerospike.json \
  -e SPTAG_AEROSPIKE_HOST=10.150.0.34 \
  -e SPTAG_AEROSPIKE_PORT=3000 \
  -e SPTAG_AEROSPIKE_NAMESPACE=sptag_data \
  -e SPTAG_AEROSPIKE_SET=sptag \
  -e SPTAG_AEROSPIKE_BIN=value \
  -v /home/Zhakh/RAG_StormX:/work \
  -v /mnt/nvme:/mnt/nvme \
  sptag bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig'
```

To sweep practical policy profiles and write one JSON/log pair per setting, run:

```bash
cd ~/RAG_StormX
SPTAG_AEROSPIKE_HOST=10.150.0.36 ./SPTAG/benchmarks/run_aerospike_policy_sweep.sh
```

Outputs are written to `SPTAG/results/aerospike_policy_sweep/<timestamp>/`, including `summary.txt` with the best-throughput and lowest-p99 profiles.

## 6. Results (2026-04-08)

Configuration: 100k base vectors, 9 batches of 100k inserts (total 1M vectors), 1000 queries per round, 16 search threads, L2 distance, TopK=10, recall@10 with brute-force ground truth.

### Summary (after final batch -- 1M total vectors)


| Metric                             | FileIO (NVMe) | RocksDB (NVMe) | Aerospike (10.150.0.33) |
| ---------------------------------- | ------------- | -------------- | ----------------------- |
| Build time (s)                     | 83.2          | 46.8           | 45.2                    |
| Initial QPS (100k vectors)         | 2,429         | 9,612          | 5,823                   |
| Initial recall@10                  | 0.807         | 0.807          | 0.807                   |
| Final QPS (1M vectors)             | 1,196         | 2,927          | 2,872                   |
| Final p50 latency (us)             | 12.52         | 5.62           | 5.26                    |
| Final p99 latency (us)             | 32.18         | 12.14          | 10.12                   |
| Final recall@10                    | 0.841         | 0.841          | 0.841                   |
| Insert throughput (vec/s, batch 9) | 5,661         | 6,758          | 6,822                   |


### Key observations

- Recall is identical across all three backends (~0.807 at 100k, ~0.841 at 1M) -- this is expected since the head index and search algorithm are the same; only the posting storage differs.
- RocksDB on NVMe delivers the best initial QPS (9,612) and maintains good performance as the index grows.
- Aerospike shows the tightest latency distribution (p99 = 10.12 us at 1M vectors) despite going over the network, with comparable throughput to local RocksDB.
- FileIO has higher latency and lower QPS, likely due to the async block-based file I/O overhead vs RocksDB's optimized LSM tree.

### Full JSON results

Output files on NVMe:

- `/mnt/nvme/sptag_bench/output_fileio.json`
- `/mnt/nvme/sptag_bench/output_rocksdb.json`
- `/mnt/nvme/sptag_bench/output_aerospike.json`

## 7. Scaling Up

To benchmark at larger scale, adjust the config parameters:


| Scale        | BaseVectorCount | InsertVectorCount | BatchNum | Expected RAM | Expected Time |
| ------------ | --------------- | ----------------- | -------- | ------------ | ------------- |
| 1M (current) | 100,000         | 900,000           | 9        | ~4 GB        | ~5 min        |
| 10M          | 1,000,000       | 9,000,000         | 9        | ~16 GB       | ~1 hour       |
| 100M         | 10,000,000      | 90,000,000        | 9        | ~64 GB       | ~12+ hours    |


## Run Log

### 2026-04-07

- Installed `axel`, `wget`, build dependencies
- Downloaded SIFT1B dataset to `/mnt/nvme0/sift1b` (on root disk, NVMe was not yet mounted)
- Built SPTAG Docker image with `-DROCKSDB=ON -DAEROSPIKE=ON`
- Created 100M subset (`base.100M.u8bin`)
- Initial FileIO benchmark run (with `TruthPath=none`, recall was null)

### 2026-04-08

- Formatted and mounted NVMe device (`/dev/nvme0n1`, 375 GB) at `/mnt/nvme`
- Re-downloaded dataset directly to NVMe via `axel` (~10 min at ~250 MB/s)
- Created 100M subset on NVMe
- Created benchmark configs for FileIO, RocksDB, Aerospike in `benchmarks/`
- Fixed `TruthPath=truth` to enable recall computation
- Ran all three benchmarks with `--run_test=SPFreshTest/BenchmarkFromConfig`
- Aerospike namespace discovered as `sptag_data` on node `10.150.0.33`
- All benchmarks completed successfully with recall@10 values

