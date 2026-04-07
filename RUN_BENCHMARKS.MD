# SPTAG SIFT (BigANN) Benchmarks Run Log

This file is a **command-by-command run log** for downloading BigANN SIFT data onto local NVMe, building SPTAG with the required backends, and running the benchmark configs (FileIO, RocksDB, Aerospike) as described in the attached plan.

## Host + paths (assumptions from plan)
- Dataset NVMe mount: `/mnt/nvme0`
- Index/artifacts NVMe mount: `/mnt/nvme1`
- Dataset dir: `/mnt/nvme0/sift1b`
- Repo root: `/home/Zhakh/RAG_StormX`

## 2026-04-07

### Install downloader + create dataset dir

```bash
sudo apt-get update
sudo apt-get install -y axel wget ca-certificates
sudo mkdir -p /mnt/nvme0/sift1b
sudo chown -R "$USER":"$USER" /mnt/nvme0/sift1b
ls -ld /mnt/nvme0/sift1b
```

Result:
- `axel` already installed.
- `/mnt/nvme0/sift1b` created and writable by user.

#### If `axel` is missing (quick install recipes)

Host (Ubuntu/Debian):

```bash
sudo apt-get update
sudo apt-get install -y axel
axel --version
```

Inside the SPTAG Docker container (Ubuntu 20.04 base):

```bash
apt-get update
apt-get install -y axel
axel --version
```

If `apt-get install axel` fails, you can still download with `wget` (slower, but universally available):

```bash
sudo apt-get update
sudo apt-get install -y wget ca-certificates
wget --version
```

### Download dataset files (BigANN)

Target URLs:
- `base.1B.u8bin`: `https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/base.1B.u8bin`
- `query.public.10K.u8bin`: `https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/query.public.10K.u8bin`
- `GT.public.1B.ibin`: `https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/GT.public.1B.ibin`

Commands executed:

```bash
cd /mnt/nvme0/sift1b

# query + GT are small; base is huge and uses axel resume via .st file.
wget -O query.public.10K.u8bin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/query.public.10K.u8bin
wget -O GT.public.1B.ibin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/GT.public.1B.ibin

# base (119+ GB)
axel -n 16 -o base.1B.u8bin https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann/base.1B.u8bin
```

Notes:
- `axel` uses a state file `base.1B.u8bin.st` for resume; if a partial exists, it will resume instead of restarting.

Verification:

```bash
ls -lh /mnt/nvme0/sift1b
wc -c /mnt/nvme0/sift1b/query.public.10K.u8bin /mnt/nvme0/sift1b/GT.public.1B.ibin
```

Observed:
- `base.1B.u8bin` present (~120G)
- `query.public.10K.u8bin` size = 1,280,008 bytes
- `GT.public.1B.ibin` size = 8,000,008 bytes
- If you see `No state file, cannot resume!` that’s not a failure — it just means there’s no `.st` resume file yet (typically on the first attempt, or if you deleted the `.st`).
- For very large files, prefer `axel` over `wget` since `axel` supports segmented downloads (`-n 16`) and resumes from the `.st` state file.

### Install build dependencies (RocksDB headers/libs, build tools, etc.)

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  libboost-all-dev libtbb-dev libnuma-dev \
  libsnappy-dev libjemalloc-dev libgflags-dev \
  zlib1g-dev libssl-dev libyaml-dev \
  librocksdb-dev
```

Status:
- Completed successfully. Verified installed packages:
  - `libboost-all-dev`
  - `librocksdb-dev`
  - `libtbb-dev`
  - `libyaml-dev`

### Aerospike client + Aerospike-enabled build approach

The repo already provides a Docker build that installs the **Aerospike C client**. See:
- `SPTAG/Dockerfile` (installs Aerospike client .deb from Aerospike artifacts).
- `SPTAG/launch_docker.sh` (runs `docker run -it --rm -v $(pwd):/app -v /tmp:/tmp sptag /bin/bash`).

Because we also need the **RocksDB** backend (`Storage=ROCKSDBIO`), we updated `SPTAG/Dockerfile` to install `librocksdb-dev` and build SPTAG with `-DROCKSDB=ON` as well.

#### Build docker image

Note: current user is not in the `docker` group, so docker commands require `sudo`.

```bash
cd /home/Zhakh/RAG_StormX/SPTAG
sudo docker build -t sptag .
```

