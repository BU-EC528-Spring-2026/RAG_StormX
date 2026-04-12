#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FILE="${SCRIPT_DIR}/docker-compose.tikv.shared-nvme.yml"
NVME_ROOT="${NVME_ROOT:-/mnt/nvme/tikv_shared}"

echo "Preparing TiKV data dirs on shared NVMe: ${NVME_ROOT}"
sudo mkdir -p \
  "${NVME_ROOT}/pd0" "${NVME_ROOT}/pd1" "${NVME_ROOT}/pd2" \
  "${NVME_ROOT}/tikv0" "${NVME_ROOT}/tikv1" "${NVME_ROOT}/tikv2"
sudo chown -R "$USER":"$USER" "${NVME_ROOT}"

if ! docker compose version >/dev/null 2>&1; then
  echo "docker compose is required but not found"
  exit 1
fi

echo "Stopping existing TiKV stack (if any)"
sudo env NVME_ROOT="${NVME_ROOT}" docker compose -f "${COMPOSE_FILE}" down --remove-orphans >/dev/null 2>&1 || true

echo "Starting 3PD + 3TiKV cluster"
sudo env NVME_ROOT="${NVME_ROOT}" docker compose -f "${COMPOSE_FILE}" up -d

echo "Waiting for PD and 3 TiKV stores to become Up"
for _ in $(seq 1 60); do
  if curl -sf http://127.0.0.1:2379/pd/api/v1/stores >/dev/null; then
    UP_COUNT="$(curl -sf http://127.0.0.1:2379/pd/api/v1/stores | python3 -c 'import json,sys; d=json.load(sys.stdin); print(sum(1 for s in d.get("stores", []) if s.get("store", {}).get("state_name") == "Up"))' 2>/dev/null || echo 0)"
    if [[ "${UP_COUNT}" -ge 3 ]]; then
      break
    fi
  fi
  sleep 1
done

curl -s http://127.0.0.1:2379/pd/api/v1/stores | head -n 160

echo "TiKV cluster is up on shared NVMe root: ${NVME_ROOT}"
echo "PD list: 127.0.0.1:2379,127.0.0.1:2479,127.0.0.1:2579"
