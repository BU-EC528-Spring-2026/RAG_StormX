# Distributed KV Database for SPTAG

Welcome to the project repository focused on building and integrating a distributed Key-Value (KV) database for SPTAG. (Here is our first [video demo](https://drive.google.com/file/d/1i-0wsOg0iPfVE9vz2vDpDfisrusYUJAf/view?usp=sharing).) (This is our second [video demo](https://drive.google.com/file/d/1Qjj-PX5dkWIvpudbBmp3PT15rgMjaVBZ/view?usp=sharing) explaining how to get through 4.[Benchmarks](#4-benchmarks) section.)
Here is our [demo3 slides](https://docs.google.com/presentation/d/1tDpxalJ8GZXDQENHnX0TcBkUOgIK2lVbXCJuONbyl74/edit?usp=sharing).

## Index

1. [Problem Statement](#1-problem-statement)
2. [Current Progress](#2-current-progress)
3. [Setup Guidance](#3-setup-guidance)
   - [Build 2 Aerospike Nodes](#aerospike-2-node-cluster-on-gcp)
   - [Build 1 SPTAG Node](#setting-up-a-single-sptag-node)
   - [Run the SPTAG Benchmark on top of Aerospike nodes](#run-the-sptag-benchmark)
4. [Benchmarks](#4-benchmarks)

---

## 1) Problem Statement

### What is SPTAG?

[SPTAG](https://github.com/microsoft/SPTAG) (Space Partition Tree And Graph) is Microsoft's library for fast approximate nearest-neighbor (ANN) search in high-dimensional vector spaces (hundreds to thousands of dimensions).

Traditional spatial data structures like quad-trees and oct-trees split on every dimension, causing the search space to explode exponentially. SPTAG avoids this **dimensionality curse** by using a **clustered balanced tree** whose branching factor stays small regardless of the number of dimensions. Once the tree narrows the search to an approximate region, SPTAG switches to a **Relative Neighborhood Graph** -- a graph where nearby vectors are connected -- and performs a greedy walk to find the closest match. This two-phase approach (tree descent + graph traversal) keeps both index size and query latency practical at billion-vector scale.

### Why do we need a distributed KV store?

SPTAG returns the *index* of the nearest vector, but something still needs to store and retrieve the actual content (embeddings, posting lists, metadata) that corresponds to each index. A naive in-memory dictionary fails for three reasons:

1. **Scale** -- datasets can be hundreds of gigabytes; they do not fit in a single machine's RAM.
2. **Persistence** -- an in-memory structure is lost on restart.
3. **Concurrency** -- many concurrent readers cause lock contention on a single-process data structure.

A distributed key-value database solves all three: data lives on SSD across multiple machines, survives restarts, and serves many readers in parallel.

### Why Aerospike over TiKV?

Our workload is heavily **read-dominated** (many queries per write), so read latency is the critical metric.

| Aspect | TiKV | Aerospike |
| --- | --- | --- |
| **Storage engine** | LSM tree -- multiple on-disk levels with compaction | Primary index lives in DRAM; each 64-byte entry stores the record's physical SSD location, so every lookup is a fast index hit in memory followed by a single SSD read |
| **Read path** | If the key is not in the block cache, TiKV must search through multiple SST file levels, causing tail-latency spikes (up to ~10 ms) | Uses direct I/O (`O_DIRECT`) on raw devices to bypass the OS page cache, keeping read latency consistently < 1 ms |
| **Read/write amplification** | Multiple intermediate levels cause both read and write amplification | The in-memory index points straight to the record on disk -- no intermediate levels, so amplification stays near 1x |
| **Consistency trade-off** | Strong consistency via Raft | Relaxed consistency by default, with a large performance gain |

In short, Aerospike trades a small amount of consistency for dramatically lower and more predictable read latency, which is the right trade-off for ANN serving.

---

## 2) Current Progress

- **Initial Approach:** We initially worked on integrating TiKV as our distributed storage backend. 
- **Pivot:** Our mentor shifted their interest from integrating TiKV to having us either build our own solution from scratch or heavily modify an existing solution.
- **Current Architecture (Aerospike):** We pivoted to using **Aerospike**. We chose Aerospike primarily because it does not rely on Log-Structured Merge (LSM) trees and uses direct I/O on raw NVMe devices, bypassing the OS page cache for massive I/O performance gains.
- **Status:** We have successfully run SPTAG on Aerospike! This was achieved by directly modifying the SPTAG source code to include an Aerospike client integration. The current implementation is heavily "vibecoded" (a rapid, experimental proof-of-concept), so our immediate next focus is to refactor the code, ensure the architecture makes logical sense, and optimize for stability.
- **Infrastructure:** We can spin up a **2-node Aerospike cluster** on GCP with a single deployment script, along with a dedicated SPTAG node with local NVMe SSD.
- **Benchmarking:** We have been benchmarking SPTAG against **three storage backends** (FileIO, RocksDB, and Aerospike) using the [SIFT1B (BigANN)](https://big-ann-benchmarks.com/) dataset on NVMe SSDs. Initial results show identical recall across all backends, with RocksDB and Aerospike significantly outperforming FileIO in throughput and latency. See the [Benchmarks](#4-benchmarks) section for full details.

---

## 3) Setup Guidance

### Aerospike 2-Node Cluster on GCP

This guide walks through how to recreate a 2-node Aerospike cluster on Google Cloud using a deployment script.  
This setup will:

- Create 2 Google Compute Engine VMs
- Install Aerospike automatically on both
- Configure them as a 2-node cluster
- Use local NVMe SSD storage
- Open the required internal firewall ports for Aerospike traffic

#### Prerequisites

Before starting, ensure:

- You have access to a Google Cloud project with billing enabled.
- Compute Engine API is enabled You can run the following command in the Google Cloud Shell: (`gcloud services enable compute.googleapis.com`).
- You are using **Google Cloud Shell** or have `gcloud` installed locally.

#### Deployment Steps

**1. Find and Set Your Project**

You can find your GCP project ID in the Google Cloud Console as shown in the screenshot below:

![GCP Project ID Screenshot](docs/GCP%20project-id.png)

Alternatively, you can list all your projects using (though this might output all of the BU projects for you):

**Run in Google Cloud Shell:**

```bash
gcloud projects list
```

> [!NOTE]
> Note your Project_ID, you will need it for the next step.

then run:

```bash
gcloud config set project YOUR_PROJECT_ID
gcloud config get-value project
```

**2. Create the Deployment Script**  
Create a file named `deploy-aerospike.sh` and paste the following bash script into it. 

> [!IMPORTANT]
> Make sure to edit the `PROJECT_ID` and `ZONE` at the top of the file to match your environment.
> The below scripts are to be ran on Google Cloud Shell (see the screenshot below)

![GCP Shell](docs/Google-shell.png)

```bash
#!/bin/bash
set -euo pipefail

# =========================
# USER-EDITABLE SETTINGS
# =========================
# Set your GCP project, zone, VM specs, cluster names, and Aerospike package details here before running.
PROJECT_ID="your-gcp-project-id"
ZONE="us-east4-b"
MACHINE_TYPE="n2-standard-4"
IMAGE_FAMILY="ubuntu-2404-lts-amd64"
IMAGE_PROJECT="ubuntu-os-cloud"
CLUSTER_NAME="sptag-cluster"
NAMESPACE_NAME="sptag_data"
NVME_DEVICE="/dev/disk/by-id/google-local-nvme-ssd-0"
NODE1="aerospike-node-1"
NODE2="aerospike-node-2"
AEROSPIKE_URL="https://download.aerospike.com/artifacts/aerospike-server-community/8.1.1.1/aerospike-server-community_8.1.1.1_tools-12.1.1_ubuntu24.04_x86_64.tgz"
AEROSPIKE_DIR="aerospike-server-community_8.1.1.1_tools-12.1.1_ubuntu24.04_x86_64"

# Stop if the placeholder project ID was not changed.
if [[ "$PROJECT_ID" == "your-gcp-project-id" ]]; then
  echo "Please edit PROJECT_ID at the top of the script before running."
  exit 1
fi

# Set the active gcloud project so all following compute commands run against the correct project.
gcloud config set project "${PROJECT_ID}"

# Create a firewall rule that allows Aerospike nodes with the aerospike tag to talk to each other on the needed ports.
gcloud compute firewall-rules create aerospike-internal \
  --network=default \
  --allow tcp:3000-3004 \
  --source-tags=aerospike \
  --target-tags=aerospike || true

# =========================
# WRITE VM STARTUP SCRIPT
# =========================
# This creates the startup script that each VM will run automatically on first boot to install and configure Aerospike.
cat > startup-aerospike.sh <<'EOF'
#!/bin/bash
set -euxo pipefail

# Send all startup-script output into a log file so installation and config issues can be debugged later.
exec > /var/log/startup-aerospike.log 2>&1
export DEBIAN_FRONTEND=noninteractive

# Read cluster settings from the VM metadata so the same startup script can configure both nodes dynamically.
CLUSTER_NAME="$(curl -fs -H 'Metadata-Flavor: Google' \
  http://metadata.google.internal/computeMetadata/v1/instance/attributes/cluster-name)"
NAMESPACE_NAME="$(curl -fs -H 'Metadata-Flavor: Google' \
  http://metadata.google.internal/computeMetadata/v1/instance/attributes/namespace-name)"
SEED_IPS_RAW="$(curl -fs -H 'Metadata-Flavor: Google' \
  http://metadata.google.internal/computeMetadata/v1/instance/attributes/seed-ips || true)"
NVME_DEVICE="$(curl -fs -H 'Metadata-Flavor: Google' \
  http://metadata.google.internal/computeMetadata/v1/instance/attributes/nvme-device)"
AEROSPIKE_URL="$(curl -fs -H 'Metadata-Flavor: Google' \
  http://metadata.google.internal/computeMetadata/v1/instance/attributes/aerospike-url)"
AEROSPIKE_DIR="$(curl -fs -H 'Metadata-Flavor: Google' \
  http://metadata.google.internal/computeMetadata/v1/instance/attributes/aerospike-dir)"

# Install the tools needed to download, unpack, and prepare the Aerospike package.
apt-get update
apt-get install -y wget curl python3 tar util-linux

# Wipe the local NVMe SSD if it exists so Aerospike can use a clean device for storage.
if [ -b "$NVME_DEVICE" ]; then
    echo "Wiping NVMe device $NVME_DEVICE..."
    blkdiscard "$NVME_DEVICE" || true
fi

# Download and install Aerospike only if it is not already installed on the VM.
if [ ! -d "/opt/aerospike/bin" ]; then
    mkdir -p /opt/aerospike
    wget "$AEROSPIKE_URL" -O /tmp/aerospike.tgz
    tar -xzf /tmp/aerospike.tgz -C /tmp
    cd "/tmp/$AEROSPIKE_DIR"
    ./asinstall
fi

# Make sure the Aerospike config and log directories exist after installation.
mkdir -p /etc/aerospike
mkdir -p /var/log/aerospike
chown -R aerospike:aerospike /var/log/aerospike || true

# Give the Aerospike user access to the NVMe device so the database can use it as its storage engine.
if [ -b "$NVME_DEVICE" ]; then
    chown aerospike:aerospike "$(readlink -f "$NVME_DEVICE")" || true
    usermod -aG disk aerospike || true
fi

# Build heartbeat seed lines from the metadata-provided IP list so this node can discover the other cluster member.
SEED_LINES=""
if [[ -n "${SEED_IPS_RAW}" ]]; then
  IFS=',' read -ra SEEDS <<< "${SEED_IPS_RAW}"
  for ip in "${SEEDS[@]}"; do
    [[ -n "${ip}" ]] && SEED_LINES="${SEED_LINES}        mesh-seed-address-port ${ip} 3003"$'\n'
  done
fi

# Write the Aerospike server configuration using the metadata values and generated seed list.
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

# Reload systemd, enable Aerospike at boot, and start the database service now.
systemctl daemon-reload
systemctl enable aerospike
systemctl start aerospike
EOF

# Make the generated startup script executable so GCP can run it during VM boot.
chmod +x startup-aerospike.sh

# =========================
# CREATE NODE 1
# =========================
# Create the first Aerospike VM. It starts with no seed IPs because it is the initial cluster member.
echo "Creating ${NODE1}..."
gcloud compute instances create "${NODE1}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --machine-type="${MACHINE_TYPE}" \
  --image-family="${IMAGE_FAMILY}" \
  --image-project="${IMAGE_PROJECT}" \
  --boot-disk-size=30GB \
  --boot-disk-type=pd-balanced \
  --local-ssd=interface=nvme \
  --tags=aerospike \
  --metadata=cluster-name="${CLUSTER_NAME}",namespace-name="${NAMESPACE_NAME}",seed-ips="",nvme-device="${NVME_DEVICE}",aerospike-url="${AEROSPIKE_URL}",aerospike-dir="${AEROSPIKE_DIR}" \
  --metadata-from-file=startup-script=./startup-aerospike.sh

# Wait briefly so node 1 has time to boot and get assigned its internal IP address.
echo "Waiting for ${NODE1} IP..."
sleep 40

# Read node 1’s internal IP so it can be passed as the seed IP when creating node 2.
NODE1_IP=$(gcloud compute instances describe "${NODE1}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --format="get(networkInterfaces[0].networkIP)")

# Print node 1’s internal IP for visibility and later troubleshooting.
echo "${NODE1} internal IP: ${NODE1_IP}"

# Show the tail of the startup log from node 1 so installation problems can be caught early.
echo "Checking ${NODE1} startup log..."
gcloud compute ssh "${NODE1}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --command "sudo tail -50 /var/log/startup-aerospike.log"

# =========================
# CREATE NODE 2
# =========================
# Create the second Aerospike VM and give it node 1’s internal IP as its heartbeat seed.
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
  --tags=aerospike \
  --metadata=cluster-name="${CLUSTER_NAME}",namespace-name="${NAMESPACE_NAME}",seed-ips="${NODE1_IP}",nvme-device="${NVME_DEVICE}",aerospike-url="${AEROSPIKE_URL}",aerospike-dir="${AEROSPIKE_DIR}" \
  --metadata-from-file=startup-script=./startup-aerospike.sh

# Wait briefly so node 2 can boot and receive its internal IP.
echo "Waiting for ${NODE2} IP..."
sleep 40

# Read node 2’s internal IP so node 1 can later be updated to know about it too.
NODE2_IP=$(gcloud compute instances describe "${NODE2}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --format="get(networkInterfaces[0].networkIP)")

# Print node 2’s internal IP for confirmation and debugging.
echo "${NODE2} internal IP: ${NODE2_IP}"

# Give node 1 additional time to finish its install before rewriting and restarting its Aerospike config.
echo "Waiting for Aerospike to finish installing on ${NODE1}..."
sleep 60

# =========================
# UPDATE NODE 1 SEEDS
# =========================
# Rewrite node 1’s Aerospike config so it also includes node 2 as a mesh heartbeat seed, then restart Aerospike.
echo "Updating ${NODE1} config with ${NODE2} as a seed..."

gcloud compute ssh "${NODE1}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --command "sudo bash -c '
mkdir -p /etc/aerospike
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
        mesh-seed-address-port ${NODE2_IP} 3003
    }

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
systemctl restart aerospike
'"

# Print a blank line and a final status message once both nodes are created and the seed update is complete.
echo
echo "Done."

# Show the final VM names, internal IPs, and status values so you can confirm both nodes exist and are running.
echo "Cluster nodes:"
gcloud compute instances describe "${NODE1}" --project="${PROJECT_ID}" --zone="${ZONE}" --format="table(name,networkInterfaces[0].networkIP,status)"
gcloud compute instances describe "${NODE2}" --project="${PROJECT_ID}" --zone="${ZONE}" --format="table(name,networkInterfaces[0].networkIP,status)"

# Print a ready-to-run verification command to check Aerospike cluster info from node 1.
echo
echo "Verify with:"
echo "gcloud compute ssh ${NODE1} --zone ${ZONE} --project ${PROJECT_ID} --command 'asadm -e \"info\"'"

```

**3. Run the Script**

```bash
chmod +x deploy-aerospike.sh
./deploy-aerospike.sh
```

**4. Verification and Testing**  
SSH into a node:

```bash
gcloud compute ssh aerospike-node-1 --zone us-east4-b
```

> [!IMPORTANT]
> Now you should be SSHed into a node! So the following commands should be ran from the nodes themselves.

**Run on an Aerospike node after SSHing in:**

Check status:

```bash
systemctl status aerospike --no-pager
asadm -e "info"  # Should output 2 nodes, cluster formed!
```

You should see something like this:
![as-adm output](docs/aerospike-asadm-output.png)

Test Read/Write (AQL):

```bash
aql
# Inside the AQL shell:
INSERT INTO sptag_data.testset (PK, bin1) VALUES ('key1', 'hello');
SELECT * FROM sptag_data.testset WHERE PK='key1';
```

*(You can verify cross-node communication by running the* `SELECT` *command from the second Aerospike node.)*
![aql output](docs/aql-output.png)

**Troubleshooting**  
The most important log for startup issues is `/var/log/startup-aerospike.log`.   
If something fails heavily, you can wipe the cluster and start fresh:

```bash
gcloud compute instances delete aerospike-node-1 aerospike-node-2 --zone us-east4-b
gcloud compute firewall-rules delete aerospike-internal
```

---

### Setting Up a Single SPTAG Node

To provision a single high-performance VM and run the environment via Docker, execute the following:

#### Provision the GCP Instance

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

#### Prepare the SPTAG VM and Docker Environment

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
git clone --recursive https://github.com/BU-EC528-Spring-2026/RAG_StormX.git
cd RAG_StormX/SPTAG

# 3. Build and Run the Docker Image
sudo docker build -t sptag .
sudo bash launch_docker.sh
```

You should see something along the lines of:

```bash
root@[bunch of numbers and letters]:/app# 
```

---

#### Find the Aerospike Internal IP

From here, you will have to navigate to the GCP VM-instances,  

> [!IMPORTANT]
> Note the internal IP address of one of your aerospike nodes from the VM instances window on the Google Cloud Platform. You have to input the internal IP address of any of your 2 aerospike nodes.

Finding Aerospike Internal IP

The highlighted area in the image shows where to look for the "Internal IP" column in your list of VM instances. Copy that internal IP address and use it in the build step below by replacing `[Aerospike internal ip address]` with the value you found (for example, `10.150.0.28`):

![aerospike internal IP](docs/GCP%20VM%20instances%20-%3E%20finding%20internal%20IP%20aerospike.png)

---

#### Rebuild SPTAG with Aerospike Enabled

> [!IMPORTANT]
> NOTE the `DAEROSPIKE_DEFAULT_HOST`, you should populate that with your own internal IP address.

**Run inside the SPTAG Docker shell:**

```bash
cd /app
rm -rf build/build-aero
 cmake -S . -B build/build-aero -DAEROSPIKE=ON \
   -DAEROSPIKE_INCLUDE_DIR=/usr/include \
   -DAEROSPIKE_CLIENT_LIBRARY=/lib/libaerospike.so \
   -DAEROSPIKE_DEFAULT_HOST=[IMPOOOOORTAAANT!!!!!! put your Aerospike internal ip address]

 cmake --build build/build-aero -j8
```

> [!TIP]
> Example of how it looks for us:

```bash
cd /app
rm -rf build/build-aero
cmake -S . -B build/build-aero -DAEROSPIKE=ON \
  -DAEROSPIKE_INCLUDE_DIR=/usr/include \
  -DAEROSPIKE_CLIENT_LIBRARY=/lib/libaerospike.so \
  -DAEROSPIKE_DEFAULT_HOST=10.150.0.28

cmake --build build/build-aero -j8
```

This should show you cmake outputs – it will throw some warnings, but some of them come directly from the SPTAG codebase, and we only added 2 more warnings to it (unused authentication methods that will be refined later down the line)

---

#### Run the SPTAG Benchmark

after your project has been built, run the following commands (pay attention to the variables that you have to provide):  

> [!IMPORTANT]
> NOTE the `SPTAG_AEROSPIKE_HOST`, you should populate that with your own internal IP address.

**Run inside the SPTAG Docker shell:**

```bash
cd /app
rm -f perftest_vector.bin perftest_meta.bin perftest_metaidx.bin \
      perftest_addvector.bin perftest_addmeta.bin perftest_addmetaidx.bin \
      perftest_query.bin perftest_batchtruth.*
rm -rf proidx/spann_index_aero proidx/spann_index_aero_*

export SPTAG_AEROSPIKE_HOST=your_ip_address
export BENCHMARK_CONFIG=/app/benchmark.aerospike.ini
export BENCHMARK_OUTPUT=/app/results/benchmark_aerospike.json
export SPTAG_AEROSPIKE_PORT=3000
export SPTAG_AEROSPIKE_NAMESPACE=sptag_data
export SPTAG_AEROSPIKE_SET=sptag
export SPTAG_AEROSPIKE_BIN=value

mkdir -p /app/results /app/proidx/spann_index_aero

./Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig --log_level=test_suite
```

> [!TIP]
> Example of how to fill in the above:

```bash
root@ce80966fc3d0:/app# rm -f perftest_vector.bin perftest_meta.bin perftest_metaidx.bin \
       perftest_addvector.bin perftest_addmeta.bin perftest_addmetaidx.bin \
       perftest_query.bin perftest_batchtruth.*
root@ce80966fc3d0:/app# rm -rf proidx/spann_index_aero proidx/spann_index_aero_*
root@ce80966fc3d0:/app# export BENCHMARK_CONFIG=/app/benchmark.aerospike.ini
root@ce80966fc3d0:/app# export BENCHMARK_OUTPUT=/app/results/benchmark_aerospike.json
root@ce80966fc3d0:/app# export SPTAG_AEROSPIKE_HOST=10.150.0.28
root@ce80966fc3d0:/app# export SPTAG_AEROSPIKE_PORT=3000
root@ce80966fc3d0:/app# export SPTAG_AEROSPIKE_NAMESPACE=sptag_data
root@ce80966fc3d0:/app# export SPTAG_AEROSPIKE_SET=sptag
root@ce80966fc3d0:/app# export SPTAG_AEROSPIKE_BIN=value
root@ce80966fc3d0:/app# ./Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig --log_level=test_suite
```

This is what you should see:
![SPTAG running on top of Aerospike nodes](docs/SPTAG-benchmark-running.png)


---

#### Benchmark Results

Results:

to see your benchmark results, you can:
```bash
cat /app/results/benchmark_aerospike.json
```
Example: 
![our Benchmark results](docs/benchmark-results.png)

---

## 4) Benchmarks

This section walks through running the full SPTAG benchmark suite on the SIFT1B (BigANN) dataset across three storage backends: **FileIO**, **RocksDB**, and **Aerospike**. It assumes you have already completed all the steps in [Setup Guidance](#3-setup-guidance) above (Aerospike cluster is running, SPTAG node is provisioned, tools are installed, and the repo is cloned).

> [!WARNING]
> The benchmark workflow involves downloading a **119 GB** dataset and running compute-intensive index builds. Budget at least **30 minutes** for setup and **5 minutes per benchmark run** at the default 1M-vector scale. Larger scales can take hours (see [Scaling Up](#scaling-up) below). Consider running long downloads and benchmarks inside a `tmux` or `screen` session so they survive SSH disconnects.

### Prepare NVMe Storage

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

### Download the SIFT1B (BigANN) Dataset

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

#### Create 100M Subset

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

#### Verify the Dataset

```bash
ls -lh /mnt/nvme/sift1b/
# Expected:
#   base.1B.u8bin          120G   (1 billion vectors, dim=128, uint8)
#   base.100M.u8bin         12G   (100 million vectors, dim=128, uint8)
#   query.public.10K.u8bin 1.3M   (10,000 query vectors)
#   GT.public.1B.ibin      7.7M   (ground truth, 100 nearest neighbors per query)
```

File format: `u8bin` = 8-byte header (uint32 num_vectors, uint32 dimension) followed by row-major uint8 vectors.

### Build the SPTAG Docker Image

If you have not already built the Docker image during the setup steps, build it now. The Dockerfile compiles SPTAG with both RocksDB and Aerospike backends enabled (`-DROCKSDB=ON -DAEROSPIKE=ON`).

**Run on the SPTAG VM (not inside Docker):**

```bash
cd ~/RAG_StormX/SPTAG
sudo docker build -t sptag .
```

> [!NOTE]
> If you already built the image during [Setup Guidance](#3-setup-guidance), you can skip this step.

### Run the Benchmarks

All benchmarks are run inside the Docker container. The `--run_test=SPFreshTest/BenchmarkFromConfig` flag runs only the benchmark test case, skipping all other unit tests. Pre-built benchmark configuration files are provided in the `benchmarks/` directory of the repository.

> [!WARNING]
> Each benchmark run at the default 1M-vector scale (100k base + 9 batches of 100k inserts) takes approximately **5 minutes**. Ground truth computation adds some overhead on the first run.

#### FileIO on NVMe

```bash
sudo docker run --rm --net=host \
  -e BENCHMARK_CONFIG=/work/benchmarks/benchmark.fileio.nvme.ini \
  -e BENCHMARK_OUTPUT=/mnt/nvme/sptag_bench/output_fileio.json \
  -v ~/RAG_StormX:/work \
  -v /mnt/nvme:/mnt/nvme \
  sptag bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig'
```

#### RocksDB on NVMe

```bash
sudo docker run --rm --net=host \
  -e BENCHMARK_CONFIG=/work/benchmarks/benchmark.rocksdb.nvme.ini \
  -e BENCHMARK_OUTPUT=/mnt/nvme/sptag_bench/output_rocksdb.json \
  -v ~/RAG_StormX:/work \
  -v /mnt/nvme:/mnt/nvme \
  sptag bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig'
```

#### Aerospike (Remote Node)

The Aerospike backend stores postings on your remote Aerospike cluster instead of local disk. You need to pass the connection parameters as environment variables:

| Variable | Description | Example |
| --- | --- | --- |
| `SPTAG_AEROSPIKE_HOST` | Internal IP of an Aerospike node | `10.150.0.33` |
| `SPTAG_AEROSPIKE_PORT` | Aerospike service port | `3000` |
| `SPTAG_AEROSPIKE_NAMESPACE` | Namespace configured on the cluster | `sptag_data` |
| `SPTAG_AEROSPIKE_SET` | Aerospike set name | `sptag` |
| `SPTAG_AEROSPIKE_BIN` | Aerospike bin name | `posting` |

> [!IMPORTANT]
> Replace the `SPTAG_AEROSPIKE_HOST` value below with the internal IP of one of your Aerospike nodes (find it in the GCP VM instances list, as described in [Find the Aerospike Internal IP](#find-the-aerospike-internal-ip)).

```bash
sudo docker run --rm --net=host \
  -e BENCHMARK_CONFIG=/work/benchmarks/benchmark.aerospike.nvme.ini \
  -e BENCHMARK_OUTPUT=/mnt/nvme/sptag_bench/output_aerospike.json \
  -e SPTAG_AEROSPIKE_HOST=10.150.0.33 \
  -e SPTAG_AEROSPIKE_PORT=3000 \
  -e SPTAG_AEROSPIKE_NAMESPACE=sptag_data \
  -e SPTAG_AEROSPIKE_SET=sptag \
  -e SPTAG_AEROSPIKE_BIN=posting \
  -v ~/RAG_StormX:/work \
  -v /mnt/nvme:/mnt/nvme \
  sptag bash -lc 'cd /work && /app/Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig'
```

### Where to Find Results

Each benchmark writes a JSON output file to the path specified by `BENCHMARK_OUTPUT`. After running all three benchmarks, you will find:

| Backend | Output File |
| --- | --- |
| FileIO | `/mnt/nvme/sptag_bench/output_fileio.json` |
| RocksDB | `/mnt/nvme/sptag_bench/output_rocksdb.json` |
| Aerospike | `/mnt/nvme/sptag_bench/output_aerospike.json` |

To inspect results:

```bash
cat /mnt/nvme/sptag_bench/output_fileio.json
cat /mnt/nvme/sptag_bench/output_rocksdb.json
cat /mnt/nvme/sptag_bench/output_aerospike.json
```

The benchmark config files used for each run are in the [`benchmarks/`](benchmarks/) directory. You can adjust parameters there (e.g. vector counts, thread counts, distance method) and re-run.

### Scaling Up

To benchmark at larger scale, edit the config files in `benchmarks/` and adjust the parameters below. Keep in mind that larger runs require significantly more RAM and time:

| Scale | BaseVectorCount | InsertVectorCount | BatchNum | Expected RAM | Expected Time |
| --- | --- | --- | --- | --- | --- |
| 1M (default) | 100,000 | 900,000 | 9 | ~4 GB | ~5 min |
| 10M | 1,000,000 | 9,000,000 | 9 | ~16 GB | ~1 hour |
| 100M | 10,000,000 | 90,000,000 | 9 | ~64 GB | ~12+ hours |

> [!WARNING]
> At the 100M scale, expect runs to take **12+ hours** and require at least **64 GB of RAM**. Make sure your SPTAG VM has enough resources and that the benchmark is running inside a `tmux` or `screen` session.
