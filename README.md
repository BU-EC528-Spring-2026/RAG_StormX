# RAG_StormX: A Distributed KV Backend for SPTAG Disk-ANN Serving

**TL;DR — We added a distributed key-value store (Aerospike) as a pluggable
posting-store backend for [Microsoft SPTAG](https://github.com/microsoft/SPTAG)'s
SPFresh disk-ANN engine, then benchmarked it head-to-head against local FileIO
and embedded RocksDB on 1M BigANN/SIFT vectors. Result: the networked KV store
matched local RocksDB on throughput and beat it on tail latency, while both
outperformed the file-I/O baseline — at identical recall.**

| Metric (1M vectors, final batch) | FileIO (NVMe) | RocksDB (NVMe) | **Aerospike (distributed)** |
| --- | ---: | ---: | ---: |
| **p99 search latency (ms)** | 32.18 | 12.14 | **10.12** |
| p50 search latency (ms) | 12.52 | 5.62 | **5.26** |
| Search QPS | 1,196 | 2,927 | 2,872 |
| Insert throughput (vec/s, batch 9) | 5,661 | 6,758 | 6,822 |
| Build time (s) | 83.2 | 46.8 | 45.2 |
| recall@10 | 0.841 | 0.841 | 0.841 |

*Setup: SIFT/BigANN subset, 100k base vectors + 900k inserts (9 batches),
1,000 queries per round, 16 search threads, TopK=10, L2, recall@10 against
brute-force ground truth. Measured 2026-04-08 on GCP with NVMe-backed storage;
full run log and reproduction steps in [RUN_BENCHMARKS.MD](./RUN_BENCHMARKS.MD).
Recall is identical across backends by design — the SPTAG head index and search
parameters are held constant so the posting store is the only variable.*

A separate TiKV comparison run (same config, see
[Section 5](#5-results)) measured 297 QPS at 53.5 ms mean latency — the cost of
transactional consistency and gRPC coordination on this read path.

## Project Links

- [Video Demo 1](https://drive.google.com/file/d/1i-0wsOg0iPfVE9vz2vDpDfisrusYUJAf/view?usp=sharing)
- [Video Demo 2](https://drive.google.com/file/d/1Qjj-PX5dkWIvpudbBmp3PT15rgMjaVBZ/view?usp=sharing)
- [Final Video Demo — complete guide](https://drive.google.com/file/d/1SUSC7lx1FgxmBtHYBH_o1mtLxmDPPEph/view?usp=drivesdk)
- [Demo 3 Slides](https://docs.google.com/presentation/d/1tDpxalJ8GZXDQENHnX0TcBkUOgIK2lVbXCJuONbyl74/edit?usp=sharing)
- [Final Demo Slides](https://docs.google.com/presentation/d/1WhShUvGfNcFhBS8wZ76EZeJvH3MpQtOolj9kGcOfCoU/edit?usp=sharing)

---

## Index

1. [Problem Statement](#1-problem-statement)
2. [Background: SPTAG and KV Storage](#2-background-sptag-and-kv-storage)
3. [Project Approach](#3-project-approach)
4. [What We Built](#4-what-we-built)
5. [Results](#5-results)
6. [Key Findings](#6-key-findings)
7. [Repository Organization](#7-repository-organization)
8. [Future Work](#8-future-work)

---

## 1) Problem Statement

Modern RAG systems depend on fast vector search. Given a query embedding, the
system must quickly find nearby vectors and retrieve the corresponding data —
document chunks, metadata, posting lists, or other payloads. ANN libraries like
SPTAG search high-dimensional vector indexes efficiently, but the search result
is only an identifier: a separate storage layer must retrieve the data
associated with it.

A simple local or in-memory KV store is not enough at larger scales:

1. **Scale:** large vector datasets and payloads can reach hundreds of gigabytes.
2. **Persistence:** in-memory storage is lost after restarts.
3. **Concurrency:** many simultaneous searches bottleneck on one local process or disk.
4. **Fault tolerance:** production systems must survive node failures.

This project asks: **can a distributed KV store back SPTAG-style ANN retrieval
without an unacceptable latency penalty — and what does the read path have to
look like for that to work?**

## 2) Background: SPTAG and KV Storage

[SPTAG](https://github.com/microsoft/SPTAG) (**Space Partition Tree And
Graph**) is an ANN library that combines a space-partitioning tree (to narrow
the search region quickly) with a relative neighborhood graph (for greedy
traversal among nearby vectors), avoiding the dimensionality blow-up of
quad/oct-trees.

Its SPFresh disk-ANN path keeps **posting lists in an external store** keyed by
vector ID — exactly the seam where a pluggable KV backend slots in: after the
in-memory head index picks candidate postings, the engine fetches them from the
posting store. That store is what we swap.

## 3) Project Approach

Benchmark-driven. We added swappable storage backends to SPTAG's SPFresh path,
holding index and search parameters fixed so the **storage engine is the only
variable**:

| Backend | Role | Storage path |
| --- | --- | --- |
| **FileIO** | Local baseline | Block-based file I/O on NVMe |
| **RocksDB** | Embedded-KV baseline | Local LSM store on NVMe |
| **Aerospike** | Distributed-KV experiment | NVMe-backed Aerospike over the network |
| **TiKV** | Distributed-KV comparison | Transactional LSM store via PD/RawKV |

The project began with TiKV integration (Go sidecar over a Unix domain socket,
later a PD/RawKV controller); measured coordination overhead then shifted the
focus to optimizing an Aerospike read path, keeping TiKV as the
stronger-consistency comparison point.

## 4) What We Built

- `AerospikeKeyValueIO` — a C++ storage backend implementing SPTAG's KV
  interface against the Aerospike client (configured at runtime via
  `SPTAG_AEROSPIKE_HOST/PORT/NAMESPACE/SET/BIN`), selected with
  `Storage=AEROSPIKEIO` in the benchmark config.
- A TiKV backend: `ExtraTiKVController` (C++) plus a Go sidecar (`tikv_uds/`)
  speaking RawKV through PD.
- Reproducible GCP infrastructure: scripts that provision a 3-node Aerospike
  cluster on local NVMe SSDs (`storage-engine device`, replication-factor 2)
  and an end-to-end benchmark workflow (`bench-aerospike.sh`, `bench-e2e.sh`,
  `deploy-aerospike.sh` on the `aerospike-udf` branch).
- Benchmark harness configs for all four backends (`benchmarks/*.ini`) driving
  SPTAG's SPFresh benchmark on the SIFT1B/BigANN workflow.
- Server-side compute experiments: Aerospike Lua UDF variants that move
  distance-related work onto the storage nodes, plus client policy sweeps
  (result JSONs committed under `SPTAG/results/` on the `aerospike-udf` branch).

## 5) Results

Headline three-way comparison: see the table at the top (2026-04-08 run,
1M vectors). Highlights:

- **Aerospike had the lowest tail latency of all backends** — p99 10.12 ms vs
  12.14 ms for local RocksDB and 32.18 ms for the FileIO baseline — despite the
  network round trip.
- **Throughput was on par with local RocksDB** (2,872 vs 2,927 QPS) and ~2.4×
  the FileIO baseline.
- **Recall was identical (0.841)** across backends — the swap changes retrieval
  performance only, not search quality.
- **TiKV** (same config, separate run): 297 QPS at 53.5 ms mean — transactional
  guarantees and gRPC/PD coordination dominate this read-heavy path.
- **Naive server-side compute made things worse.** Later UDF experiments
  (2026-04-25, JSONs on the `aerospike-udf` branch) sent per-posting work to the
  server without batching: throughput dropped from 2,229 QPS (UDF off) to 1,415
  (pairs encoding) and 739 (packed encoding). Round trips, not compute
  placement, are the bottleneck — batched/`MultiGet`-style APIs are the right
  lever (consistent with our mentor's guidance).

Caveats we know about: 1,000 queries per measurement round; single-run numbers
(no variance reported); the raw JSONs for the 04-08 run lived on the benchmark
VM's NVMe — the committed artifacts are the run log in `RUN_BENCHMARKS.MD` and
the April-25 result JSONs on the `aerospike-udf` branch.

## 6) Key Findings

1. **Distributed is not automatically slower for ANN retrieval.** With an
   NVMe-backed, low-hop read path, a networked KV matched local embedded
   storage on throughput and won on tail latency.
2. **Batching is the lever that matters.** ANN search fetches many related
   keys; per-key round trips dominate. A `MultiGet`-style batched API matches
   the graph-traversal access pattern — our UDF results show what happens
   without it.
3. **Recall is a property of the index and search settings, not the store** —
   storage can be optimized independently.
4. **Storage-engine design matters:** embedded RocksDB wins on cold start
   (9,612 initial QPS); Aerospike's tight read path keeps the latency
   distribution narrow as the index grows; TiKV pays for transactions.
5. **Clean layering pays off.** Keeping storage behind SPTAG's KV interface is
   what made a four-backend comparison possible without touching search logic.

## 7) Repository Organization

```text
RAG_StormX/
├── README.md                  # this overview
├── RUN_BENCHMARKS.MD          # full reproduction guide + results + run log
├── SPTAG/                     # SPTAG source tree (Aerospike/TiKV backends added)
│   └── AnnService/…/AerospikeKeyValueIO.*   # Aerospike storage backend (C++)
├── benchmarks/                # per-backend benchmark .ini configs + TiKV compose
├── aerospace_client/          # Aerospike smoke-test / persistence-check scripts
├── tikv_uds/                  # TiKV Go sidecar (RawKV via PD)
├── insert_test_index/         # sample index loader config
└── docs/                      # benchmark and deployment screenshots
```

Branches:

- **`aerospike-udf`** — Aerospike UDF experiments, policy sweeps, committed
  result JSONs (`SPTAG/results/`), and the GCP end-to-end automation
  (`deploy-aerospike.sh`, `bench-aerospike.sh`, `bench-e2e.sh`).
- **`FileIO-RocksDB_TiKV`** — local-backend and TiKV comparison work.

## 8) Future Work

1. A real `MultiGet`/batched retrieval API with query context and server-side
   filtering or aggregation.
2. Remove duplicated distance computation between client and storage layer.
3. Scale to 10M/100M vectors; report latency at max throughput, not just raw QPS.
4. Multi-run benchmarks with variance, and committed raw JSONs for every run.
5. Fault-tolerance measurement under node failure.

---

## Acknowledgments

Developed for the EC528 (Cloud Computing) final project at Boston University.
We thank our mentor **Qi Chen** (Microsoft Research, SPTAG/SPANN) for guidance
on SPTAG, distributed KV design, benchmarking methodology, and the
batching/`MultiGet` optimization direction.
