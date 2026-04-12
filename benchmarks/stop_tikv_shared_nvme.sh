#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FILE="${SCRIPT_DIR}/docker-compose.tikv.shared-nvme.yml"
NVME_ROOT="${NVME_ROOT:-/mnt/nvme/tikv_shared}"

echo "Stopping TiKV cluster"
sudo env NVME_ROOT="${NVME_ROOT}" docker compose -f "${COMPOSE_FILE}" down
