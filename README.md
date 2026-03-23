# Distributed KV Database for SPTAG

Welcome to the project repository focused on building and integrating a distributed Key-Value (KV) database for SPTAG. 

## Index
1. [The Problem Statement](#1-the-problem-statement)
2. [Current Progress](#2-current-progress)
3. [Setup Guidance](#3-setup-guidance)

---

## 1) The Problem Statement

**About SPTAG**
SPTAG (Space Partition Tree And Graph) is an open-source library released by Microsoft Research and Bing for fast, large-scale vector approximate nearest neighbor (ANN) search. It assumes samples are represented as vectors compared by L2 or cosine distances. By combining space partition trees (like KD-trees or balanced k-means trees) with relative neighborhood graphs (RNG), SPTAG achieves highly efficient and accurate searches. Recent highlights of the library include incremental in-place updates (online vector deletion and insertion) and distributed serving across multiple machines.

**Why a Distributed KV Database Makes Sense**
To support the massive scale of billion-vector searches and distributed serving environments, relying purely on in-memory or single-node storage architectures is a bottleneck. A distributed KV database allows SPTAG to persist, partition, and serve massive vector datasets concurrently across multiple machines. By shifting to a distributed KV model—especially one optimized for modern hardware like NVMe—we can ensure high availability, horizontal scalability, and extremely low-latency lookups, which are critical for real-time approximate nearest neighbor retrieval.

---

## 2) Current Progress

* **Initial Approach:** We initially worked on integrating TiKV as our distributed storage backend. 
* **Pivot:** Our mentor shifted their interest from integrating TiKV to having us either build our own solution from scratch or heavily modify an existing solution.
* **Current Architecture (Aerospike):** We pivoted to using **Aerospike**. We chose Aerospike primarily because it does not rely on Log-Structured Merge (LSM) trees and has the unique ability to write directly to NVMe drives using SPDK, completely bypassing OS buffers for massive I/O performance gains.
* **Status:** We have successfully run SPTAG on Aerospike! This was achieved by directly modifying the SPTAG source code to include an Aerospike client integration. The current implementation is heavily "vibecoded" (a rapid, experimental proof-of-concept), so our immediate next focus is to refactor the code, ensure the architecture makes logical sense, and optimize for stability.

---

## 3) Setup Guidance

### Aerospike 2-Node Cluster on GCP

This guide walks through how to recreate a 2-node Aerospike cluster on Google Cloud using a deployment script.
This setup will:
* Create 2 Google Compute Engine VMs
* Install Aerospike automatically on both
* Configure them as a 2-node cluster
* Use local NVMe SSD storage
* Open the required internal firewall ports for Aerospike traffic

#### Prerequisites
Before starting, ensure:
* You have access to a Google Cloud project with billing enabled.
* Compute Engine API is enabled (`gcloud services enable compute.googleapis.com`).
* You are using **Google Cloud Shell** or have `gcloud` installed locally.

#### Deployment Steps

**1. Find and Set Your Project**
```bash
gcloud projects list
```
Note your Project_ID, you will need it in the next step. 

then run:
```bash 
gcloud config set project YOUR_PROJECT_ID
gcloud config get-value project
```

**2. Create the Deployment Script**
Create a file named `deploy-aerospike.sh` and paste the following bash script into it. Make sure to edit the `PROJECT_ID` and `ZONE` at the top of the file to match your environment.

```bash
#!/bin/bash
set -euo pipefail

# =========================
# USER-EDITABLE SETTINGS
# =========================
# Set your GCP project, zone, VM specs, cluster names, and Aerospike package details here before running.
PROJECT_ID=your-gcp-project-id
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

gcloud config set project "${PROJECT_ID}"

# Create firewall rule
gcloud compute firewall-rules create aerospike-internal \
  --network=default \
  --allow tcp:3000-3004 \
  --source-tags=aerospike \
  --target-tags=aerospike || true

# =========================
# WRITE VM STARTUP SCRIPT
# =========================
cat > startup-aerospike.sh <<'EOF'
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
    echo "Wiping NVMe device $NVME_DEVICE..."
    blkdiscard "$NVME_DEVICE" || true
fi

if [ ! -d "/opt/aerospike/bin" ]; then
    mkdir -p /opt/aerospike
    wget "$AEROSPIKE_URL" -O /tmp/aerospike.tgz
    tar -xzf /tmp/aerospike.tgz -C /tmp
    cd "/tmp/$AEROSPIKE_DIR"
    ./asinstall
fi

mkdir -p /etc/aerospike
mkdir -p /var/log/aerospike
chown -R aerospike:aerospike /var/log/aerospike || true

if[ -b "$NVME_DEVICE" ]; then
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
    service { address any; port 3000 }
    fabric { address any; port 3001 }
    heartbeat {
        mode mesh
        address any
        port 3003
        interval 150
        timeout 10
${SEED_LINES}    }
    admin { address any; port 3004 }
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
EOF

chmod +x startup-aerospike.sh

# =========================
# CREATE NODE 1
# =========================
echo "Creating ${NODE1}..."
gcloud compute instances create "${NODE1}" \
  --project="${PROJECT_ID}" --zone="${ZONE}" \
  --machine-type="${MACHINE_TYPE}" \
  --image-family="${IMAGE_FAMILY}" --image-project="${IMAGE_PROJECT}" \
  --boot-disk-size=30GB --boot-disk-type=pd-balanced \
  --local-ssd=interface=nvme \
  --tags=aerospike \
  --metadata=cluster-name="${CLUSTER_NAME}",namespace-name="${NAMESPACE_NAME}",seed-ips="",nvme-device="${NVME_DEVICE}",aerospike-url="${AEROSPIKE_URL}",aerospike-dir="${AEROSPIKE_DIR}" \
  --metadata-from-file=startup-script=./startup-aerospike.sh

echo "Waiting for ${NODE1} IP..."
sleep 40
NODE1_IP=$(gcloud compute instances describe "${NODE1}" --project="${PROJECT_ID}" --zone="${ZONE}" --format="get(networkInterfaces[0].networkIP)")
echo "${NODE1} internal IP: ${NODE1_IP}"

# =========================
# CREATE NODE 2
# =========================
echo "Creating ${NODE2}..."
gcloud compute instances create "${NODE2}" \
  --project="${PROJECT_ID}" --zone="${ZONE}" \
  --machine-type="${MACHINE_TYPE}" \
  --image-family="${IMAGE_FAMILY}" --image-project="${IMAGE_PROJECT}" \
  --boot-disk-size=30GB --boot-disk-type=pd-balanced \
  --local-ssd=interface=nvme \
  --tags=aerospike \
  --metadata=cluster-name="${CLUSTER_NAME}",namespace-name="${NAMESPACE_NAME}",seed-ips="${NODE1_IP}",nvme-device="${NVME_DEVICE}",aerospike-url="${AEROSPIKE_URL}",aerospike-dir="${AEROSPIKE_DIR}" \
  --metadata-from-file=startup-script=./startup-aerospike.sh

echo "Waiting for ${NODE2} IP..."
sleep 40
NODE2_IP=$(gcloud compute instances describe "${NODE2}" --project="${PROJECT_ID}" --zone="${ZONE}" --format="get(networkInterfaces[0].networkIP)")
echo "${NODE2} internal IP: ${NODE2_IP}"

echo "Waiting for Aerospike to finish installing on ${NODE1}..."
sleep 60

# =========================
# UPDATE NODE 1 SEEDS
# =========================
echo "Updating ${NODE1} config with ${NODE2} as a seed..."

gcloud compute ssh "${NODE1}" \
  --project="${PROJECT_ID}" --zone="${ZONE}" \
  --command "sudo bash -c '
cat > /etc/aerospike/aerospike.conf <<CONF
service { cluster-name ${CLUSTER_NAME}; proto-fd-max 15000 }
logging { file /var/log/aerospike/aerospike.log { context any info } }
network {
    service { address any; port 3000 }
    fabric { address any; port 3001 }
    heartbeat {
        mode mesh
        address any
        port 3003
        interval 150
        timeout 10
        mesh-seed-address-port ${NODE2_IP} 3003
    }
    admin { address any; port 3004 }
}
namespace ${NAMESPACE_NAME} {
    replication-factor 2
    default-ttl 0
    indexes-memory-budget 4G
    storage-engine device { device ${NVME_DEVICE} }
}
CONF
systemctl restart aerospike
'"

echo "Done. Cluster nodes:"
gcloud compute instances describe "${NODE1}" --project="${PROJECT_ID}" --zone="${ZONE}" --format="table(name,networkInterfaces[0].networkIP,status)"
gcloud compute instances describe "${NODE2}" --project="${PROJECT_ID}" --zone="${ZONE}" --format="table(name,networkInterfaces[0].networkIP,status)"
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
Check status:
```bash
systemctl status aerospike --no-pager
asadm -e "info"  # Should output 2 nodes, cluster formed.
```

Test Read/Write (AQL):
```bash
aql
# Inside the AQL shell:
INSERT INTO sptag_data.testset (PK, bin1) VALUES ('key1', 'hello');
SELECT * FROM sptag_data.testset WHERE PK='key1';
```
*(You can verify cross-node communication by running the `SELECT` command from the second Aerospike node.)*

**Troubleshooting**
The most important log for startup issues is `/var/log/startup-aerospike.log`. 
If something fails heavily, you can wipe the cluster and start fresh:
```bash
gcloud compute instances delete aerospike-node-1 aerospike-node-2 --zone us-east4-b
gcloud compute firewall-rules delete aerospike-internal
```

### Setting Up a Single SPTAG Node

To provision a single high-performance VM and run the environment via Docker, execute the following:

In the Google Cloud Shell (press G and S in the google cloud VM-instance window)
```bash
# 1. Provision the GCP Instance
gcloud compute instances create sptag-node \
    --zone=us-east4-b \
    --machine-type=c2-standard-8 \
    --subnet=default \
    --tags=http-server,https-server \
    --create-disk=auto-delete=yes,boot=yes,device-name=slow-disk,name=slow-disk,size=250GB,type=pd-standard,image-family=ubuntu-2404-lts-amd64,image-project=ubuntu-os-cloud \
    --local-ssd=interface=NVME \
    --scopes=default \
    --labels=goog-ops-agent=v2-x86-template
```

Then SSH into the provisioned VM, and run the following:

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
    wget

sudo systemctl enable docker
sudo systemctl start docker

# 2. Clone the Repository (with submodules)
# You will need to authenticate with GitHub!!!!! We will that as an excercise to the reader :) 
# (Just add an ssh key :^) )
git clone --recursive git@github.com:BU-EC528-Spring-2026/RAG_StormX.git
cd RAG_StormX/SPTAG

# 3. Build and Run the Docker Image
sudo docker build -t sptag .
sudo bash launch_docker.sh
```

You should see something along the lines of:

```bash
root@[bunch of numbers and letters]:/app# 
```

From here, you will have to navigate to the GCP VM-instances, and note the internal IP address of one of your aerospike nodes. Use it to run the following:

```bash
cd /app
rm -rf build/build-aero
 cmake -S . -B build/build-aero -DAEROSPIKE=ON \
   -DAEROSPIKE_INCLUDE_DIR=/usr/include \
   -DAEROSPIKE_CLIENT_LIBRARY=/lib/libaerospike.so \
   -DAEROSPIKE_DEFAULT_HOST=[Aerospike internal ip address]
 cmake --build build/build-aero -j8
```

This should show you cmake outputs – it will throw some warnings, but some of them come directly from the SPTAG codebase, and we only added 2 more warnings to it (unused authentication methods that will be refined later down the line)

after your project has been built, run the following commands (pay attention to the variables that you have to provide):

```bash
cd /app
rm -f perftest_vector.bin perftest_meta.bin perftest_metaidx.bin \
      perftest_addvector.bin perftest_addmeta.bin perftest_addmetaidx.bin \
      perftest_query.bin perftest_batchtruth.*
rm -rf proidx/spann_index_aero proidx/spann_index_aero_*
export BENCHMARK_CONFIG=/app/benchmark.aerospike.ini
export BENCHMARK_OUTPUT=/app/results/benchmark_aerospike.json
export SPTAG_AEROSPIKE_HOST=[]
export SPTAG_AEROSPIKE_PORT=3000
export SPTAG_AEROSPIKE_NAMESPACE=sptag_data
export SPTAG_AEROSPIKE_SET=sptag
export SPTAG_AEROSPIKE_BIN=value
./Release/SPTAGTest --run_test=SPFreshTest/BenchmarkFromConfig --log_level=test_suite
```