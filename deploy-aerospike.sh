#!/bin/bash
set -euo pipefail

# =========================
# USER-EDITABLE SETTINGS
# =========================
# Set your GCP project, zone, VM specs, cluster names, and Aerospike package details here before running.
# Auto-detect from Google Cloud Shell env, or fall back to gcloud config
PROJECT_ID="${PROJECT_ID:-${GOOGLE_CLOUD_PROJECT:-$(gcloud config get-value project 2>/dev/null)}}"
if [[ -z "$PROJECT_ID" || "$PROJECT_ID" == "(unset)" ]]; then
    echo "ERROR: Could not detect GCP project. Run: gcloud config set project YOUR_PROJECT_ID"
    exit 1
fi
echo "Using GCP project: $PROJECT_ID"
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


# Set the active gcloud project so all following compute commands run against the correct project.
gcloud config set project "${PROJECT_ID}"

# Create a firewall rule that allows Aerospike nodes with the aerospike tag to talk to each other on the needed ports.
if gcloud compute firewall-rules describe aerospike-internal --project="${PROJECT_ID}" &>/dev/null; then
    echo "Firewall rule aerospike-internal already exists, skipping."
else
    gcloud compute firewall-rules create aerospike-internal \
      --network=default \
      --allow tcp:3000-3004 \
      --source-tags=aerospike \
      --target-tags=aerospike
fi

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
if gcloud compute instances describe "${NODE1}" --project="${PROJECT_ID}" --zone="${ZONE}" &>/dev/null; then
    echo "${NODE1} already exists, skipping creation."
else
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
fi

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
if gcloud compute instances describe "${NODE2}" --project="${PROJECT_ID}" --zone="${ZONE}" &>/dev/null; then
    echo "${NODE2} already exists, skipping creation."
else
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
fi

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
if gcloud compute instances describe "${NODE3}" --project="${PROJECT_ID}" --zone="${ZONE}" &>/dev/null; then
    echo "${NODE3} already exists, skipping creation."
else
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

rm -f node3-seeds.txt

echo "Waiting for ${NODE3} IP..."
sleep 40
fi

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


