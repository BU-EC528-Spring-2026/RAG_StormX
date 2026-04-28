# Distributed KV Database for SPTAG

Welcome to the project repository focused on building and integrating Aerospike into SPTAG.

## Index

1. [Setup Guidance](#1-setup-guidance)
  - [Build 3 Aerospike Nodes](#aerospike-3-node-cluster-on-gcp)
  - [Build 1 SPTAG Node](#setting-up-a-single-sptag-node)
2. [Benchmarks](#2-benchmarks-aerospike-only)
  - [Prepare and download SIFT1B](#download-the-sift1b-bigann-dataset)
  - [Run Aerospike policy sweep](#run-aerospike-policy-sweep)
3. [Aerospike UDFs: design and current status](#3-aerospike-udfs-design-and-current-status)
4. [Registering UDFs on remote Aerospike nodes](#4-registering-udfs-on-remote-aerospike-nodes) *(prerequisite for Packed / Pairs modes)*
  - [Run UDF benchmarks](#7-run-udf-benchmarks)

---

### Why Aerospike?

The vector-search workload is heavily **read-dominated** (many queries per write), so predictable read latency matters more than single-node strong consistency. Aerospike keeps a **primary index in DRAM** (each 64-byte entry points at the record’s on-disk location), so a lookup is a memory hit plus typically **one** storage read, without LSM-style level walks. The storage engine can use **direct I/O** on raw devices (for example local NVMe), which avoids double-buffering in the page cache and helps keep tail latency stable for large postings. This combination fits ANN serving where we need high QPS, bounded latency, and horizontal scale.

---

## 1) Setup Guidance

### Aerospike 3-Node Cluster on GCP

This guide walks through how to recreate a 3-node Aerospike cluster on Google Cloud using a deployment script.
This setup will:

- Create 3 Google Compute Engine VMs
- Install Aerospike automatically on all three
- Configure them as a 3-node cluster
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
PROJECT_ID="YOUR-PROJECT-NAME"
ZONE="us-east4-b"
MACHINE_TYPE="n2-standard-4"
IMAGE_FAMILY="ubuntu-2404-lts-amd64"
IMAGE_PROJECT="ubuntu-os-cloud"
CLUSTER_NAME="sptag-cluster"
NAMESPACE_NAME="sptag_data"
NVME_DEVICE="/dev/disk/by-id/google-local-nvme-ssd-0"
NODE1="aerospike-node-1"
NODE2="aerospike-node-2"
NODE3="aerospike-node-3"
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

# Read node 2’s internal IP so node 1 and node 3 can later be updated.
NODE2_IP=$(gcloud compute instances describe "${NODE2}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --format="get(networkInterfaces[0].networkIP)")

# Print node 2’s internal IP for confirmation and debugging.
echo "${NODE2} internal IP: ${NODE2_IP}"

# =========================
# CREATE NODE 3 
# =========================
echo "${NODE1_IP},${NODE2_IP}" > node3-seeds.txt

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
  --tags=aerospike \
  --metadata="cluster-name=${CLUSTER_NAME},namespace-name=${NAMESPACE_NAME},nvme-device=${NVME_DEVICE},aerospike-url=${AEROSPIKE_URL},aerospike-dir=${AEROSPIKE_DIR}" \
  --metadata-from-file="startup-script=./startup-aerospike.sh,seed-ips=./node3-seeds.txt"

rm node3-seeds.txt

echo "Waiting for ${NODE3} IP..."
sleep 40

# Read node 3’s internal IP.
NODE3_IP=$(gcloud compute instances describe "${NODE3}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --format="get(networkInterfaces[0].networkIP)")

# Print node 3’s internal IP for confirmation and debugging.
echo "${NODE3} internal IP: ${NODE3_IP}"

# Give existing nodes additional time to finish their installs before rewriting and restarting configs.
echo "Waiting for Aerospike to finish installing on ${NODE1} and ${NODE2}..."
sleep 60

# =========================
# UPDATE NODE 1 & 2 SEEDS
# =========================
# Rewrite node 1 and 2 Aerospike configs so they include the full mesh, then restart Aerospike.

echo "Updating ${NODE1} config for full 3-node mesh..."
gcloud compute ssh "${NODE1}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --command "sudo bash -c '
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
        mesh-seed-address-port ${NODE3_IP} 3003
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

echo "Updating ${NODE2} config for full 3-node mesh..."
gcloud compute ssh "${NODE2}" \
  --project="${PROJECT_ID}" \
  --zone="${ZONE}" \
  --command "sudo bash -c '
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
        mesh-seed-address-port ${NODE1_IP} 3003
        mesh-seed-address-port ${NODE3_IP} 3003
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

# Print a blank line and a final status message once all nodes are created and the seed updates are complete.
echo
echo "Done."

# Show the final VM names, internal IPs, and status values so you can confirm all nodes exist and are running.
echo "Cluster nodes:"
gcloud compute instances list --project="${PROJECT_ID}" --filter="tags.items=aerospike" --format="table(name,networkInterfaces[0].networkIP,status)"

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
asadm -e "info"  # Should output 3 nodes, cluster formed!
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
gcloud compute instances delete aerospike-node-1 aerospike-node-2 aerospike-node-3 --zone us-east4-b
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
git clone --recursive -b aerospike-udf https://github.com/BU-EC528-Spring-2026/RAG_StormX.git
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
> Note the internal IP address of one of your Aerospike nodes from the VM instances window on the Google Cloud Platform. Use one of the cluster node internal IPs for the SPTAG and benchmark commands.

The highlighted area in the image shows where to look for the "Internal IP" column in your list of VM instances. Copy that internal IP address and use it in the build step below by replacing `[Aerospike internal ip address]` with the value you found (for example, `10.150.0.28`):

![aerospike internal IP](docs/GCP%20VM%20instances%20-%3E%20finding%20internal%20IP%20aerospike.png)

---

#### Rebuild SPTAG with Aerospike Enabled

> [!IMPORTANT]
> NOTE the `DAEROSPIKE_DEFAULT_HOST`, you should populate that with your own internal IP address.

> [!IMPORTANT]
> **Minimum Aerospike version:** the server-side UDF search path (`AerospikeUDFMode = Packed | Pairs`) requires **Aerospike server >= 6.0** and **Aerospike C client >= 6.0**, because it uses `aerospike_batch_apply` to dispatch the posting-filter UDF across all keys in a single network round-trip. The CMake configure step verifies that `aerospike/aerospike_batch.h` exports `aerospike_batch_apply`; older clients fail fast at configure time. If you are running an older deployment and cannot upgrade, keep `AerospikeUDFMode=0` (Off) and the legacy `MultiGet` path will be used.

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

## 2) Benchmarks (Aerospike only)

This section covers preparing the SIFT1B (BigANN) data on NVMe and running the **Aerospike**-backed benchmark. It assumes you completed [Setup Guidance](#1-setup-guidance): the Aerospike cluster is running, the SPTAG image is built, the repo is cloned, and dataset paths match `benchmark.aerospike.nvme.ini` or the copy under `benchmarks/` in the image.

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

The full 1B base is too large for feasible benchmarks. Extract the first 100M vectors (~12 GB):

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

If you have not already built the Docker image during the setup steps, build it now. The Dockerfile enables the Aerospike client (`-DAEROSPIKE=ON`).

**Run on the SPTAG VM (not inside Docker):**

```bash
cd ~/RAG_StormX/SPTAG
sudo docker build -t sptag .
```

> [!NOTE]
> If you already built the image during [Setup Guidance](#1-setup-guidance), you can skip this step.

### Run Aerospike policy sweep

After the dataset is prepared and the SPTAG Docker image is built, run the policy sweep from inside the SPTAG Docker container. The script runs practical Aerospike client-policy profiles and writes one JSON/log pair per setting, plus a `summary.txt` report.

```bash
cd /app
export SPTAG_AEROSPIKE_HOST=10.150.0.36
export SPTAG_AEROSPIKE_PORT=3000
export SPTAG_AEROSPIKE_NAMESPACE=sptag_data
export SPTAG_AEROSPIKE_SET=sptag
export SPTAG_AEROSPIKE_BIN=value
export SPFRESH_BINARY=/app/Release/SPTAGTest
export BENCHMARK_CONFIG=/app/benchmarks/benchmark.aerospike.nvme.ini
export RESULTS_DIR=/app/results
mkdir -p "$RESULTS_DIR"
ulimit -n 65535

./benchmarks/run_aerospike_policy_sweep.sh
```

Set `SPTAG_AEROSPIKE_HOST` to a reachable Aerospike node's internal IP. Results are written under `$RESULTS_DIR/<timestamp>/`, including `benchmark_aerospike_policy_<profile>.json`, matching `.log` files, and `summary.txt`.

### Scaling Up

To benchmark at larger scale, edit the config files in `benchmarks/` and adjust the parameters below. Keep in mind that larger runs require significantly more RAM and time:


| Scale        | BaseVectorCount | InsertVectorCount | BatchNum | Expected RAM | Expected Time |
| ------------ | --------------- | ----------------- | -------- | ------------ | ------------- |
| 1M (default) | 100,000         | 900,000           | 9        | ~4 GB        | ~5 min        |
| 10M          | 1,000,000       | 9,000,000         | 9        | ~16 GB       | ~1 hour       |
| 100M         | 10,000,000      | 90,000,000        | 9        | ~64 GB       | ~12+ hours    |


> [!WARNING]
> At the 100M scale, expect runs to take **12+ hours** and require at least **64 GB of RAM**. Make sure your SPTAG VM has enough resources and that the benchmark is running inside a `tmux` or `screen` session.

---

## 3) Aerospike UDFs: design and current status

SPTAG can push parts of the SPANN posting read path into **Aerospike server-side UDFs** (Lua in [`SPTAG/AnnService/udf/sptag_posting.lua`](SPTAG/AnnService/udf/sptag_posting.lua), optionally accelerated by a small AVX C module `avx_math.so`). The full build, registration, and troubleshooting story is in [`SPTAG/AnnService/udf/README.md`](SPTAG/AnnService/udf/README.md). To **compile, test, and register** those files on your cluster, follow [Registering UDFs on remote Aerospike nodes](#4-registering-udfs-on-remote-aerospike-nodes). In practice there are two different “UDF” topics:

1. **Search UDF mode (`AerospikeUDFMode`)** — `run_aerospike_udf_ab.sh` compares **Off** vs **Pairs** (and optional **Packed**) for batch scoring on the server. This path depends on a modern Aerospike client/server, `avx_math.so` compiled and registered from an Aerospike node, and matching Lua 5.4 ABI; otherwise you see slow Lua fallbacks, timeouts, or `require` errors (see the UDF README).
2. **Experimental posting pipeline flags** — separate from the A/B script, the client can enable **filtered read** and **merge** UDFs for posting access and `MergePostings` updates. The checked-in benchmark artifacts show how these behaved on the same 1M-vector SPFresh run (SIFT 100M subset, UInt8 L2, 16 threads, 1000 queries):


| Artifact                                                                                             | What it tests                                    | What happened                                                                                                                                                                                                                                 |
| ---------------------------------------------------------------------------------------------------- | ------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [`results/output_aerospike_no_udf.json`](results/output_aerospike_no_udf.json)                       | No experimental UDF flags; normal Aerospike I/O. | **Baseline:** ~~2.3 ms mean query latency and ~0.81 recall@10 before inserts; recall rises toward **~~0.84** by batch 9.                                                                                                                      |
| [`results/output_aerospike_udf_filtered_only.json`](results/output_aerospike_udf_filtered_only.json) | **Filtered read** UDF only.                      | **Regression:** recall stays in line with the baseline (~0.84 by batch 9) but **mean search latency** rises to tens of ms (roughly an order of magnitude vs MultiGet), because the path issues **per-key UDF** work instead of a batched get. |
| [`results/output_aerospike_udf_merge_only.json`](results/output_aerospike_udf_merge_only.json)       | **Merge** UDF for posting updates only.          | **Correctness failure:** first batch search recall collapses to **~0.04** and drifts to **~0.0075** by batch 9 even though Latency looks fast — the merge path **corrupts or mis-assembles** posting blobs during inserts.                    |


A short structured summary of the same comparison is in [`results/benchmark_comparison_summary.json`](results/benchmark_comparison_summary.json). **Bottom line:** the experimental merge-style UDFs are not production-ready until the posting merge semantics are fixed; the filtered-read path is mainly a **latency regression** for this workload. Use the search-path UDF A/B script documented in [Run UDF benchmarks](#7-run-udf-benchmarks).

---

## 4) Registering UDFs on remote Aerospike nodes

Search-path UDFs use two files under [`SPTAG/AnnService/udf/`](SPTAG/AnnService/udf/): the **Lua** module [`sptag_posting.lua`](SPTAG/AnnService/udf/sptag_posting.lua) (UDF entry points) and a **C** extension built from [`avx.math.c`](SPTAG/AnnService/udf/avx.math.c) into a shared library `avx_math.so` that Lua loads with `require "avx_math"`. The `.so` is CPU-architecture-sensitive, so compile and register it from an Aerospike storage node. The Lua file is architecture-agnostic, so it can be registered from the SPTAG VM as long as `aql` can read the file.

> [!NOTE]
> Full detail and troubleshooting lives in [`SPTAG/AnnService/udf/README.md`](SPTAG/AnnService/udf/README.md). This section is the minimal path if you only have the sources and remote shell access to your nodes.

### 1) Install `aql` / Aerospike tools

Run this on the SPTAG VM, or on whichever Aerospike node will run `aql register module`:

```bash
cd /tmp
wget https://download.aerospike.com/artifacts/aerospike-tools/8.4.0/aerospike-tools_8.4.0_ubuntu20.04_x86_64.tgz
tar -xvzf aerospike-tools_8.4.0_ubuntu20.04_x86_64.tgz
cd aerospike-tools_8.4.0_ubuntu20.04_x86_64
sudo dpkg -i *.deb
sudo apt-get -f install -y
```

### 2) Set the Aerospike connection variables

Use the internal IP address of an Aerospike node in your cluster:

```bash
export SPTAG_AEROSPIKE_HOST=<AEROSPIKE_INTERNAL_IP>
export SPTAG_AEROSPIKE_PORT=3000
export SPTAG_AEROSPIKE_NAMESPACE=sptag_data
export SPTAG_AEROSPIKE_SET=sptag
export SPTAG_AEROSPIKE_BIN=value
```

> [!WARNING]
> `SPTAG_AEROSPIKE_HOST` must be an Aerospike node's **internal IP** that is reachable from the machine running `aql` and from the SPTAG VM/container. Do not use the SPTAG VM IP, an old cluster's IP, or a public/external IP unless you intentionally configured networking for that. Use this same IP for `aql` and the UDF benchmark.

### 3) Build dependencies on an Aerospike node

- **Toolchain:** `gcc` with **AVX-512** (`-mavx512f -mavx512bw -mavx512dq`). **Each Aerospike node** that will load `avx_math.so` must be **x86-64 with AVX-512**; check e.g. `grep avx512f /proc/cpuinfo` on a node.
- **Lua headers** for the **same** embedded Lua as the server (for almost all current deployments, **Lua 5.4**): e.g. on Ubuntu, `sudo apt-get install liblua5.4-dev` and a `lua5.4` package for the local test.
- **Aerospike 6.x** with Lua 5.1 is a special case: compile against Lua 5.1 headers and use a compatible [`sptag_posting.lua`](SPTAG/AnnService/udf/sptag_posting.lua) (see the UDF README). The default in this repo targets **Aerospike 7+ / Lua 5.4**.

Install the packages on the Aerospike node where you compile the C extension:

```bash
sudo apt-get update
sudo apt-get install -y git build-essential liblua5.4-dev lua5.4 binutils
```

### 4) Compile and register `avx_math.so` on an Aerospike node

Run these commands on the Aerospike storage node. This keeps the compiled `.so` matched to the CPU architecture that will execute it. The source file is [`SPTAG/AnnService/udf/avx.math.c`](SPTAG/AnnService/udf/avx.math.c); create the same file on the Aerospike node before compiling:

```bash
mkdir -p ~/sptag_udf
cd ~/sptag_udf
vim avx.math.c   # paste the C source here

grep -m1 avx512f /proc/cpuinfo
gcc -O3 -fPIC -shared -fno-plt -funroll-loops \
  -mavx512f -mavx512bw -mavx512dq \
  -march=native \
  -I/usr/include/lua5.4 \
  avx.math.c -o avx_math.so \
  -ldl
```

The `grep -m1 avx512f /proc/cpuinfo` command must print a CPU flag. If it prints nothing, that node cannot run this AVX-512 accelerator.

**Sanity checks** (the build should succeed with no link errors; confirm the module’s public entry and that Lua C API symbols stay **unresolved** in the `.so`—Aerospike’s process provides them at load time):

```bash
file avx_math.so
nm -D avx_math.so | grep ' T luaopen_avx_math'   # must show T (defined)
nm -D avx_math.so | grep ' U lua_' | head         # should show U, not T — do not link -llua
```

If `gcc` cannot find `lua.h` for 5.4, install `liblua5.4-dev` or adjust the `-I/usr/include/lua5.4` include path to wherever the Aerospike node's matching Lua headers live.

Run the standalone harness before registration:

```bash
lua5.4 test_avx_math_local.lua
# or: AVX_MATH_SO="$PWD/avx_math.so" lua5.4 test_avx_math_local.lua
```

You should see only `[PASS] ...` lines. If the interpreter segfaults or throws, **do not** register that `.so` on the cluster.

Register the compiled native module with `aql` from that same Aerospike node:

```bash
aql -h "$SPTAG_AEROSPIKE_HOST" -c "register module '$(pwd)/avx_math.so'"
```

### 5) Register [`sptag_posting.lua`](SPTAG/AnnService/udf/sptag_posting.lua)

The Lua UDF is architecture-agnostic, so you can register it from the SPTAG VM or from an Aerospike node. Run the command from a machine where [`SPTAG/AnnService/udf/sptag_posting.lua`](SPTAG/AnnService/udf/sptag_posting.lua) exists:

```bash
cd ~/RAG_StormX/SPTAG/AnnService/udf
aql -h "$SPTAG_AEROSPIKE_HOST" -c "register module '$(pwd)/sptag_posting.lua'"
```

If you are already inside `aql`, the interactive equivalent is:

```bash
aql -h "$SPTAG_AEROSPIKE_HOST"
aql> register module 'sptag_posting.lua'
```

The interactive form only works when `aql` was started from a directory containing the file being registered. If in doubt, use the absolute-path `-c` commands above.

### 6) Confirm the cluster sees Lua + C together

With your namespace, set, and a dummy key (any PK), run the UDF’s `init()` to confirm `avx_math` loaded:

```bash
aql -h "$SPTAG_AEROSPIKE_HOST" -c "execute sptag_posting.init() on sptag_data.sptag where pk='diag'"
# expect: | "avx_math:ok" |  (or "avx_math:missing" if the C module did not load)
```

On a node, watch the log while you run a benchmark or the command above:

```bash
sudo tail -f /var/log/aerospike/aerospike.log | grep -iE 'avx_math|sptag_posting'
# Good:  ... [sptag_posting] avx_math loaded ok via require
# Bad:   WARNING: avx_math.so not loaded. require err=...
```

If the probe returns `"avx_math:missing"` or the server log reports a load error, rebuild `avx_math.so` on an AVX-512 Aerospike node with the correct Lua headers and re-register it from that Aerospike node with `aql`.

For the full error matrix (`require err=...`, `code=-15` timeouts, wrong Lua ABI, missing AVX-512), use [`SPTAG/AnnService/udf/README.md`](SPTAG/AnnService/udf/README.md) §4.

### 7) Run UDF benchmarks

After `aql` is installed and both `avx_math.so` and [`sptag_posting.lua`](SPTAG/AnnService/udf/sptag_posting.lua) are registered, run the UDF A/B benchmark script from inside the SPTAG Docker container.

```bash
export SPTAG_AEROSPIKE_HOST=10.150.0.36
export SPTAG_AEROSPIKE_PORT=3000
export SPTAG_AEROSPIKE_NAMESPACE=sptag_data
export SPTAG_AEROSPIKE_SET=sptag
export SPTAG_AEROSPIKE_BIN=value
export SPFRESH_BINARY=/app/Release/SPTAGTest
export BENCHMARK_CONFIG=/app/benchmarks/benchmark.aerospike.nvme.ini
export RESULTS_DIR=/app/results
mkdir -p "$RESULTS_DIR"
ulimit -n 65535
cd /app
./benchmarks/run_aerospike_udf_ab.sh
# Options:
# ./benchmarks/run_aerospike_udf_ab.sh --pq
# ./benchmarks/run_aerospike_udf_ab.sh --pq --allow-packed-pq
```

The script writes one JSON plus one log per mode under `RESULTS_DIR`.
