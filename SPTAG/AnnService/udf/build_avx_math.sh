#!/usr/bin/env bash
# ------------------------------------------------------------------
# Build the avx_math.so Lua C accelerator that the SPTAG posting-list
# UDF (sptag_posting.lua) loads via `require "avx_math"`.
#
# The resulting .so must be deployed to every Aerospike server node
# under the `lua-userpath` configured in aerospike.conf (default
# /opt/aerospike/usr/udf/lua/). See README.md in this directory for
# the full deploy procedure.
#
# Usage:
#   ./build_avx_math.sh                    # builds ./avx_math.so against Lua 5.4
#   LUA_VER=5.4 ./build_avx_math.sh        # explicit Lua ABI
#   OUT=/tmp/avx_math.so ./build_avx_math.sh
#
# Requires gcc with AVX-512 support and Lua 5.4 development headers.
# Aerospike Server 7.0+ embeds Lua 5.4. Building against 5.1 headers
# produces an .so that fails to load on the cluster (silent fallback to
# the slow Lua scoring path -> 50 ms UDF timeouts -> client code=-15).
# ------------------------------------------------------------------
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="${SRC_DIR}/avx.math.c"
OUT="${OUT:-${SRC_DIR}/avx_math.so}"
CC="${CC:-gcc}"

# Aerospike Server 7.0+ uses Lua 5.4. Allow override for older clusters.
LUA_VER="${LUA_VER:-5.4}"

LUA_INC=""
SEARCH_DIRS=(
    "/usr/include/lua${LUA_VER}"
    "/usr/local/include/lua${LUA_VER}"
    "/usr/include/lua-${LUA_VER}"
    "/usr/local/include/lua-${LUA_VER}"
    "/usr/include"
    "/usr/local/include"
)
for cand in "${SEARCH_DIRS[@]}"; do
    if [ -f "${cand}/lua.h" ]; then
        # Verify the version macro inside lua.h matches what we want, so we
        # don't accidentally pick up a /usr/include/lua.h shipped by another
        # version when both are installed.
        if grep -q "LUA_VERSION_NUM[[:space:]]*$(echo "$LUA_VER" | awk -F. '{printf "%d0%d", $1, $2}')" "${cand}/lua.h"; then
            LUA_INC="$cand"
            break
        fi
    fi
done
if [ -z "$LUA_INC" ]; then
    echo "ERROR: could not locate Lua ${LUA_VER} headers." >&2
    echo "       On Debian/Ubuntu: sudo apt-get install liblua${LUA_VER}-dev" >&2
    echo "       (Aerospike Server 7.0+ embeds Lua 5.4 -- pin LUA_VER=5.4.)" >&2
    exit 1
fi

echo "Building $OUT"
echo "  src:     $SRC"
echo "  cc:      $CC"
echo "  lua-ver: $LUA_VER"
echo "  lua-inc: $LUA_INC"

# IMPORTANT: do NOT link `-llua${LUA_VER}`. The Aerospike host process
# already exports the Lua C API symbols; linking the .so against libluaX.Y
# pulls in a second Lua runtime and you'll get duplicate-state crashes at
# the first `lua_State *L` callback.
#
# MARCH defaults to `x86-64-v4` (implies AVX-512F/BW/DQ/CD, the same ISA
# level we already require) so the .so is portable across any v4-capable
# cluster node. Override with `MARCH=native` when building on the target
# Aerospike node to pick up vendor-specific tuning.
#
# USE_FMA=1 enables _mm512_fmadd_ps in the f32 L2/Dot inner loops. Left off
# by default because the existing SPTAG scoring path deliberately avoids FMA
# for bit-exact parity with the client-side ComputeDistance (Packed mode
# re-scores client-side; a mismatch would perturb the final ranking).
MARCH="${MARCH:-x86-64-v4}"
USE_FMA="${USE_FMA:-0}"
EXTRA_DEFS=""
if [ "$USE_FMA" = "1" ]; then
    EXTRA_DEFS="-DUSE_FMA"
fi

"$CC" -O3 -fPIC -shared -fno-plt -funroll-loops \
    -mavx512f -mavx512bw -mavx512dq \
    -march="$MARCH" \
    $EXTRA_DEFS \
    -I"$LUA_INC" \
    "$SRC" -o "$OUT" \
    -ldl

echo "Done. Quick sanity checks:"
echo "  file $OUT"
echo "  nm -D $OUT | grep luaopen_avx_math   # expect: T luaopen_avx_math"
echo "  nm -D $OUT | grep -E 'lua_[a-z]+' | head    # expect: U (undefined, resolved by host)"
echo
echo "Deploy to each Aerospike node (or register once via aql, the cluster"
echo "will replicate to the rest):"
echo "  aql -h <seed> -c \"register module '$OUT'\""
