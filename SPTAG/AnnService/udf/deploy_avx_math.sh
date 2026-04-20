#!/usr/bin/env bash
# ------------------------------------------------------------------
# Copy avx_math.so to every Aerospike node's lua-userpath.
#
# Why this exists:
#   `aql register module 'avx_math.so'` accepts the file and reports
#   "OK, 1 module added", but the cluster's UDF distribution path is
#   designed for Lua source. It mangles ELF binaries (the deployed
#   file no longer exports `luaopen_avx_math`), so Lua's `require`
#   later fails with `undefined symbol: luaopen_avx_math`.
#
#   The supported way to deploy a C extension is to drop the .so
#   under the configured `lua-userpath` on every node, byte-for-byte.
#   No Aerospike restart required -- Lua's require() finds it on the
#   next VM warm-up.
#
# Usage:
#   ./deploy_avx_math.sh node1 node2 node3 ...
#   ./deploy_avx_math.sh 10.150.0.33 10.150.0.34
#
#   SRC=/path/to/avx_math.so ./deploy_avx_math.sh ...      # explicit src
#   DST_DIR=/opt/aerospike/usr/udf/lua ./deploy_avx_math.sh ...  # override path
#   SSH_USER=ec2-user ./deploy_avx_math.sh ...
# ------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="${SRC:-${SCRIPT_DIR}/avx_math.so}"
DST_DIR="${DST_DIR:-/opt/aerospike/usr/udf/lua}"
SSH_USER="${SSH_USER:-}"

if [ ! -f "$SRC" ]; then
    echo "ERROR: source $SRC not found. Run build_avx_math.sh first." >&2
    exit 1
fi

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <node> [<node> ...]" >&2
    echo "       deploys $SRC to <node>:$DST_DIR/avx_math.so" >&2
    exit 1
fi

LOCAL_SHA=$(sha256sum "$SRC" | awk '{print $1}')
LOCAL_SIZE=$(stat -c '%s' "$SRC")
echo "Deploying $SRC"
echo "  size:   $LOCAL_SIZE bytes"
echo "  sha256: $LOCAL_SHA"
echo "  target: $DST_DIR/avx_math.so"
echo

for NODE in "$@" ; do
    TARGET="${SSH_USER:+${SSH_USER}@}${NODE}"
    echo "==> $NODE"
    scp -q "$SRC" "$TARGET:/tmp/avx_math.so"
    ssh "$TARGET" "sudo install -o root -g root -m 0644 /tmp/avx_math.so '$DST_DIR/avx_math.so' \
        && sudo nm -D '$DST_DIR/avx_math.so' 2>/dev/null | grep -q ' T luaopen_avx_math' \
        && echo '  OK: luaopen_avx_math exported, sha256='\$(sha256sum '$DST_DIR/avx_math.so' | awk '{print \$1}') \
        || (echo '  FAIL: luaopen_avx_math missing on remote -- copy did not survive' >&2; exit 2)"
done

echo
echo "All nodes deployed. Now flush warm Lua VMs by re-registering sptag_posting.lua,"
echo "and re-probe with init():"
echo "  aql -h ${1} -c \"register module '${SCRIPT_DIR}/sptag_posting.lua'\""
echo "  aql -h ${1} -c \"execute sptag_posting.init() on <ns>.<set> where pk='diag'\""
