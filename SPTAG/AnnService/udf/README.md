# SPTAG Aerospike UDF

This directory holds the two server-side artifacts that implement the read-path
push-down for SPANN postings on Aerospike:

| File | Role | How it gets to the server |
| ---- | ---- | ------------------------- |
| `sptag_posting.lua` | Record UDF entry points (`nearest_candidates_pairs`, `nearest_candidates_read`, `compact_posting`, `merge_into`, ...). | Shipped automatically by the SPTAG client on connect via `aerospike_udf_put` (see `AerospikeKeyValueIO.cpp`). |
| `avx.math.c` &rarr; `avx_math.so` | AVX-512 C accelerator the Lua UDF loads via `require "avx_math"`. Covers Float32 (L2 + Cosine) and UInt8 (L2). | **Must be registered with the cluster** via `aql register module 'avx_math.so'` (or `aerospike_udf_put`). The cluster distributes it to every node and puts it where the embedded Lua's `require` finds it. |

If `avx_math.so` is not registered, the Lua falls back to a much slower pure-Lua
scoring path that will not fit inside a typical 50 ms `total_timeout` &mdash;
you will see `udf.c:1109 UDF timed out` in the server log preceded by
`WARNING: avx_math.so not loaded.` from the Lua. The Lua logs an explicit
`avx_math loaded ok via require` on first call when the C module is live, so the
absence of that line is itself a signal.

UInt8+Cosine and Int8/Int16 still hit the slow Lua path; do **not** use them
under the 50 ms budget without first widening `avx.math.c`.

> **Recommendation: `Pairs` is the default UDF mode when PQ is disabled.**
> `Packed` returns the top-N posting entries verbatim and re-scores them on
> the client at full precision; that is only useful when the posting bin
> holds **PQ-compressed** vectors (server-side score is approximate, the
> client must refine). Without PQ, `Packed` pays the same server-side cost
> as `Pairs` plus a full-precision client-side pass and extra bytes over the
> wire — ~2x slower in benchmarks with identical recall. `benchmarks/run_aerospike_udf_ab.sh`
> now skips `Packed` for non-PQ runs unless you pass `--with-packed`.

> **Zero-copy `_ud` entry points** (rolled out alongside this doc): the
> compiled `avx_math.so` exports `batch_candidates_pairs_ud` and
> `batch_candidates_packed_ud` in addition to the original string-taking
> names. The Lua wrapper feature-detects these and, when present, hands the
> Aerospike `Bytes` userdata to C directly — avoiding a per-UDF
> `bytes.get_string(posting, 1, total)` allocation that used to copy the
> entire posting record into a fresh Lua string on every call. If you roll a
> mixed-version cluster, old nodes that only export the string names keep
> working via the wrapper's fallback path (no change to the wire protocol).

## 1. Regenerate the embedded Lua header

The SPTAG client embeds `sptag_posting.lua` in its binary so the UDF is
auto-registered on connect. Whenever you edit the `.lua` file, regenerate the
header:

```bash
python3 SPTAG/AnnService/scripts/embed_sptag_posting_lua.py
```

This rewrites `SPTAG/AnnService/inc/Helper/SptagPostingLuaEmbed.h`. Rebuild the
SPTAG client (`make -C build`) so the new Lua is shipped on the next connect.

## 2. Build `avx_math.so`

Requires `gcc` with AVX-512 (`-mavx512f -mavx512bw -mavx512dq`) and the Lua 5.4
development headers (`apt-get install liblua5.4-dev` on Debian/Ubuntu).

> **Aerospike Server 7.0+ embeds Lua 5.4** (the upgrade from 5.1 happened in
> the 7.0 release). Building against the wrong ABI is the single most common
> reason this module silently refuses to load: the .so registers fine, but
> the first call resolves into a 5.1-vs-5.4 struct-layout mismatch, the
> Lua wrapper falls back to the slow scoring path, and the client sees
> per-record `code=-15` (`AEROSPIKE_NO_RESPONSE`) when the 50 ms UDF
> timeout fires.
>
> If you are still on Aerospike 6.x or older, override with `LUA_VER=5.1`
> when invoking the build script and revert `sptag_posting.lua` to a
> pre-Lua-5.3 version (no `//`, `>>`, `&`, or `string.pack`).

```bash
./SPTAG/AnnService/udf/build_avx_math.sh
# produces SPTAG/AnnService/udf/avx_math.so
```

Sanity-check the entry symbol is exported:

```bash
nm -D SPTAG/AnnService/udf/avx_math.so | grep luaopen_avx_math
# expect: ... T luaopen_avx_math
```

## 3. Deploy `avx_math.so` to every node

> **Do NOT use `aql register module` for the C extension.** Aerospike's
> UDF distribution path was built for Lua source: it accepts the `.so`
> and reports `OK, 1 module added`, but the binary that ends up on each
> node under `lua-userpath` no longer exports `luaopen_avx_math` (the
> SMD framework reserialises ELF as if it were a Lua chunk). Lua's
> `require` then dies with:
>
> ```
> error loading module 'avx_math' from file '/opt/aerospike/usr/udf/lua/avx_math.so':
>   /opt/aerospike/usr/udf/lua/avx_math.so: undefined symbol: luaopen_avx_math
> ```
>
> `aerospike_udf_put(..., AS_UDF_TYPE_LUA, ...)` from the C client has
> the same problem (it goes through SMD too). Skip both and copy the
> .so directly to each node's `lua-userpath`.

### Recommended: `deploy_avx_math.sh`

```bash
./SPTAG/AnnService/udf/deploy_avx_math.sh 10.150.0.33 10.150.0.34
```

The script `scp`s the .so, `install`s it as `0644 root:root` under
`/opt/aerospike/usr/udf/lua/avx_math.so`, and verifies on the remote
that `nm -D` shows `T luaopen_avx_math` so a mangled copy never escapes
unnoticed. Override `DST_DIR=` if your cluster's `lua-userpath` is
different (`grep -E 'user-path|lua-userpath' /etc/aerospike/aerospike.conf`).

### Manual equivalent

```bash
SO=$(pwd)/SPTAG/AnnService/udf/avx_math.so
for NODE in 10.150.0.33 10.150.0.34 ; do
    scp "$SO" "$NODE:/tmp/avx_math.so"
    ssh "$NODE" 'sudo install -m 0644 /tmp/avx_math.so /opt/aerospike/usr/udf/lua/avx_math.so'
    ssh "$NODE" 'nm -D /opt/aerospike/usr/udf/lua/avx_math.so | grep luaopen_avx_math'
done
```

No Aerospike restart is required. The next Lua VM warm-up `require`s
the new file. To force a warm-up immediately, re-register
`sptag_posting.lua` (this is just an SMD update, it always works for
Lua source) and probe via `init()`:

```bash
aql -h <seed> -c "register module '$(pwd)/SPTAG/AnnService/udf/sptag_posting.lua'"
aql -h <seed> -c "execute sptag_posting.init() on <ns>.<set> where pk='diag'"
# expect: | "avx_math:ok" |
```

## 4. Verify the C path is live

After the next benchmark / query, grep the server log:

```bash
sudo tail -f /var/log/aerospike/aerospike.log | grep -iE 'avx_math|sptag_posting'
# expect: ... INFO (udf): ... [sptag_posting] avx_math loaded ok via require
```

If you instead see `WARNING: avx_math.so not loaded. require err=...`, the most
common causes are:

| Symptom in `require err=...` | Cause | Fix |
| --- | --- | --- |
| `module 'avx_math' not found` | The `.so` was never registered with the cluster. | Run the `aql register module 'avx_math.so'` step above. |
| `undefined symbol: lua_pushinteger` (or similar `lua_*`) | Built against a Lua ABI the server doesn't expose. | Rebuild against Lua 5.4 headers (`LUA_VER=5.4 ./build_avx_math.sh`). Do **not** link `-llua5.4` &mdash; the host process resolves Lua symbols. |
| Module loads but every batch returns `code=-15` (no per-record response) | .so was built against the wrong Lua ABI (e.g. 5.1 against a 5.4 server). The wrapper silently falls back to the slow Lua path and exceeds `total_timeout`. | Rebuild against the matching Lua version. Confirm with `nm -D avx_math.so | grep lua_` &mdash; all `lua_*` symbols should be `U` (undefined, resolved by host). |
| `wrong ELF class` / `cannot open shared object file` | Wrong arch, or built without `-fPIC -shared`. | Rebuild with `build_avx_math.sh`; verify with `file avx_math.so`. |
| `Illegal instruction` at first call | CPU lacks AVX-512. | `grep avx512f /proc/cpuinfo` &mdash; if empty, this CPU cannot run the accelerator. |

After re-registering the module, no Aerospike restart is required &mdash; new
UDF invocations pick it up on their next Lua VM warm-up.
