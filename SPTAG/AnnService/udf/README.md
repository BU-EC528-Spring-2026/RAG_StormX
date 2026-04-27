# SPTAG Aerospike UDF

This directory holds the two server-side artifacts that implement the read-path
push-down for SPANN postings on Aerospike:

| File | Role | How it gets to the server |
| ---- | ---- | ------------------------- |
| `sptag_posting.lua` | Record UDF entry points (`nearest_candidates_pairs`, `nearest_candidates_read`, `compact_posting`, `merge_into`, ...). | **Register out-of-band before running UDF-mode benchmarks**, typically from the SPTAG VM with `aql -h "$SPTAG_AEROSPIKE_HOST" -c "register module '<path>/sptag_posting.lua'"`. This Lua source is architecture-agnostic. Earlier builds of the SPTAG client auto-uploaded this Lua on connect via `aerospike_udf_put`; that was **disabled** because every client-process startup would race to re-upload the same module and invalidate per-VM Lua bytecode caches mid-query. |
| `avx.math.c` &rarr; `avx_math.so` | AVX-512 C accelerator the Lua UDF loads via `require "avx_math"`. Covers Float32 (L2 + Cosine) and UInt8 (L2). | **Compile and register from an Aerospike storage node** so the native module matches the CPU architecture that will execute it, e.g. `MARCH=native LUA_VER=5.4 ./build_avx_math.sh` followed by `aql -h "$SPTAG_AEROSPIKE_HOST" -c "register module '$(pwd)/avx_math.so'"`. |

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

The generated `SptagPostingLuaEmbed.h` is kept for ad-hoc operator scripts that
want to ship the Lua source programmatically. The benchmark path documented here
does **not** auto-register UDFs on connect; use `aql register module` before
running UDF-mode benchmarks. Whenever you edit the `.lua` file and need the
embedded header to match, regenerate it:

```bash
python3 SPTAG/AnnService/scripts/embed_sptag_posting_lua.py
```

This rewrites `SPTAG/AnnService/inc/Helper/SptagPostingLuaEmbed.h`.

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

## 3. Register UDF modules with `aql`

Register the native module from an Aerospike storage node after compiling it
there. This matters because `avx_math.so` is CPU-architecture-sensitive:

```bash
cd ~/RAG_StormX/SPTAG/AnnService/udf
aql -h "$SPTAG_AEROSPIKE_HOST" -c "register module '$(pwd)/avx_math.so'"
```

Register the Lua module from either the SPTAG VM or an Aerospike node. It is
architecture-agnostic; the only requirement is that `aql` can read the file:

```bash
cd ~/RAG_StormX/SPTAG/AnnService/udf
aql -h "$SPTAG_AEROSPIKE_HOST" -c "register module '$(pwd)/sptag_posting.lua'"
```

Probe both together with:

```bash
aql -h "$SPTAG_AEROSPIKE_HOST" -c "execute sptag_posting.init() on <ns>.<set> where pk='diag'"
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
| `module 'avx_math' not found` | The `.so` was not registered or the filename is wrong. | Build `avx_math.so` on an Aerospike node and register it with `aql`; see §3. |
| `undefined symbol: lua_pushinteger` (or similar `lua_*`) | Built against a Lua ABI the server doesn't expose. | Rebuild against Lua 5.4 headers (`LUA_VER=5.4 ./build_avx_math.sh`). Do **not** link `-llua5.4` &mdash; the host process resolves Lua symbols. |
| Module loads but every batch returns `code=-15` (no per-record response) | .so was built against the wrong Lua ABI (e.g. 5.1 against a 5.4 server). The wrapper silently falls back to the slow Lua path and exceeds `total_timeout`. | Rebuild against the matching Lua version. Confirm with `nm -D avx_math.so | grep lua_` &mdash; all `lua_*` symbols should be `U` (undefined, resolved by host). |
| `wrong ELF class` / `cannot open shared object file` | Wrong arch, or built without `-fPIC -shared`. | Rebuild with `build_avx_math.sh`; verify with `file avx_math.so`. |
| `Illegal instruction` at first call | CPU lacks AVX-512. | `grep avx512f /proc/cpuinfo` &mdash; if empty, this CPU cannot run the accelerator. |

After re-registering the module, no Aerospike restart is required &mdash; new
UDF invocations pick it up on their next Lua VM warm-up.
