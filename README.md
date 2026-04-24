# RAG_StormX: Distributed KV Storage for SPTAG-Based RAG Workloads

RAG_StormX is a final project exploring how distributed key-value (KV) storage can support large-scale approximate nearest-neighbor (ANN) search for retrieval-augmented generation (RAG) workloads. The project builds around [SPTAG](https://github.com/microsoft/SPTAG), Microsoft's ANN search library, and investigates how different storage backends affect scalability, latency, throughput, and deployment complexity.

This main branch is intended to serve as the **project overview and final documentation hub**. Backend-specific implementation details, setup commands, and experiments are kept in separate branches or supporting documentation so that the main branch stays clean and easy to understand.

## Project Links

- [Video Demo 1](https://drive.google.com/file/d/1i-0wsOg0iPfVE9vz2vDpDfisrusYUJAf/view?usp=sharing)
- [Video Demo 2](https://drive.google.com/file/d/1Qjj-PX5dkWIvpudbBmp3PT15rgMjaVBZ/view?usp=sharing)
- [Demo 3 Slides](https://docs.google.com/presentation/d/1tDpxalJ8GZXDQENHnX0TcBkUOgIK2lVbXCJuONbyl74/edit?usp=sharing)

---

## Index

1. [Problem Statement](#1-problem-statement)
2. [Background: SPTAG and KV Storage](#2-background-sptag-and-kv-storage)
3. [Project Approach](#3-project-approach)
4. [Progress and Accomplishments](#4-progress-and-accomplishments)
5. [Benchmarking Overview](#5-benchmarking-overview)
6. [Key Findings](#6-key-findings)
7. [Repository Organization](#7-repository-organization)
8. [Final Status and Future Work](#8-final-status-and-future-work)

---

## 1) Problem Statement

Modern RAG systems depend on fast vector search. Given a query embedding, the system must quickly find nearby vectors and retrieve the corresponding data, such as document chunks, metadata, posting lists, or other payloads. ANN libraries like SPTAG can efficiently search through high-dimensional vector indexes, but the search result is usually only an identifier. A separate storage layer must still retrieve the data associated with that identifier.

A simple local or in-memory KV store is not enough at larger scales because:

1. **Scale:** large vector datasets and payloads can reach hundreds of gigabytes or more.
2. **Persistence:** in-memory storage is lost after restarts and is not suitable for durable serving.
3. **Concurrency:** many simultaneous search requests can create bottlenecks if all reads go through one local process or disk.
4. **Fault tolerance:** production-style systems need storage that can survive failures and continue serving data.

Our project investigates how distributed KV systems can support SPTAG-style ANN workloads and what tradeoffs appear when moving from local storage to distributed storage.

---

## 2) Background: SPTAG and KV Storage

### What is SPTAG?

[SPTAG](https://github.com/microsoft/SPTAG) stands for **Space Partition Tree And Graph**. It is an ANN search library designed for fast similarity search over high-dimensional vectors.

Traditional spatial data structures, such as quad-trees or oct-trees, split across dimensions and become inefficient as dimensionality grows. SPTAG avoids this dimensionality problem by combining two ideas:

- a **space-partitioning tree** that quickly narrows the search region, and
- a **relative neighborhood graph** that supports greedy traversal among nearby vectors.

This two-stage design makes SPTAG practical for large vector-search workloads.

### Why does SPTAG need KV storage?

SPTAG can identify nearby vectors, but real RAG systems also need to fetch the content tied to those vectors. That content may include embeddings, posting lists, document chunks, or metadata. A KV store is a natural fit because each vector ID can map to the payload needed after ANN search.

In this project, we explored how different KV backends behave when used with SPTAG-like workloads. 

---

## 3) Project Approach

Our project followed a benchmark-driven approach. Instead of only discussing distributed storage theoretically, we built and tested storage paths around SPTAG and compared how different backends behaved under similar benchmark settings.

The main storage backends explored were Aerospike and TiKV, however we additionally tested both RocksDB and FileIO:

| Backend | Role in Project | Notes |
| --- | --- | --- |
| FileIO | Local baseline | Simple file-based storage path used as a baseline. |
| RocksDB | Embedded KV baseline | Local key-value store used to compare against distributed options. |
| Aerospike | Distributed KV experiment | Explored as a low-latency distributed KV backend with NVMe-backed storage. |
| TiKV | Distributed KV baseline | Explored as a distributed transactional KV option and comparison point. |

The project began with interest in TiKV integration. Over time, the work shifted toward an optimization of Aeropsike alongside TiKV. (I need to add something else here im just not sure. )

---

## 4) Progress and Accomplishments

By the end of the project, we accomplished the following:

- Built and ran the SPTAG benchmark environment.
- Reproduced the SPFresh benchmark workflow inside a Docker-based setup.
- Prepared benchmark configurations for different storage backends.
- Deployed Aerospike nodes on Google Cloud Platform.
- Created scripts and documentation for standing up backend infrastructure.
- Modified SPTAG source paths to experiment with external KV storage.
- Ran benchmarks using the SIFT1B / BigANN dataset workflow.
- Compared local and distributed storage behavior using QPS, latency, and recall.
- Identified that recall can remain consistent across backends when search/index settings are held constant, while throughput and latency are heavily affected by storage engine behavior and deployment topology.
- Received mentor feedback that batching, `MultiGet`-style APIs, and server-side aggregation are important for improving distributed ANN serving performance.

---

## 5) Benchmarking Overview

Our benchmark work focused on comparing storage backends under SPTAG's benchmark workflow. The goal was not only to find the fastest backend, but also to understand why the results looked the way they did.

The benchmark setup used:

- SPTAG's SPFresh benchmark path.
- SIFT1B / BigANN-style vector data.
- Local NVMe storage for dataset and index artifacts.
- Backend-specific KV paths for retrieving stored payloads/postings.
- Metrics such as QPS, latency, and recall.

### High-Level Result Summary

| Backend | General Outcome | Interpretation |
| --- | --- | --- |
| FileIO | Worked as a local baseline | Useful for comparison, but not distributed or fault tolerant. |
| RocksDB | Strong local embedded performance | Performed well because it avoids network overhead and stays local. |
| Aerospike | Demonstrated distributed KV integration | Showed that SPTAG can run with a distributed KV backend, but performance depends on batching and request path design. |
| TiKV | Useful distributed comparison point | Helped frame tradeoffs around stronger consistency, LSM-based storage, and distributed read overhead. |

> Final numeric results should be placed here once we agree on the exact values to present. 
>
> | Backend | QPS | Mean Latency | Recall@K | Notes |
> | --- | ---: | ---: | ---: | --- |
> | FileIO | TBD | TBD | TBD | Local baseline |
> | RocksDB | TBD | TBD | TBD | Embedded KV baseline |
> | Aerospike | TBD | TBD | TBD | Distributed KV experiment |
> | TiKV | TBD | TBD | TBD | Distributed KV comparison |

---

## 6) Key Findings

### 1. Distributed storage is not automatically faster

A distributed KV store can improve scalability, persistence, and fault tolerance, but it also introduces network overhead. For ANN workloads, the storage path must be carefully designed so that distributed reads do not become the bottleneck.

### 2. Batching is critical

ANN search often needs to retrieve many related keys. Sending many individual requests can create unnecessary latency. A `MultiGet` or batched retrieval API is important because it reduces round trips and better matches the access pattern of graph-based vector search.

### 3. Recall depends mostly on index/search settings

When the same SPTAG index and search parameters are used, recall should remain mostly consistent across storage backends. The backend mainly affects how quickly the required data can be retrieved.

### 4. Storage engine design matters

Local embedded systems like RocksDB can perform well because they avoid network overhead. Distributed systems like Aerospike and TiKV offer different tradeoffs around latency, consistency, scaling, replication, and operational complexity.

### 5. Backend integration needs clean layering

One of the biggest lessons was that storage integration should be separated cleanly from the search logic. The code path should make it easy to swap backends, compare results, and avoid duplicating distance computations across the client and storage layer.

---

## 7) Repository Organization

The repository is organized around a clean main branch and backend-specific implementation branches.

```text
RAG_StormX/
├── README.md                         # Main project overview and final documentation hub
├── SPTAG/                            # SPTAG source tree and benchmark configs
├── docs/                             # General project documentation and final presentation assets
│   ├── project-overview.md           # High-level project explanation
│   ├── benchmark-summary.md          # Final benchmark table and interpretation
│   ├── architecture.md               # System architecture and design notes
│   └── presentation-assets/          # Diagrams, screenshots, and figures for final slides
├── results/                          # Final selected benchmark outputs or summaries
└── branches:
    ├── aerospike                     # Aerospike-specific implementation and setup
    └── tikv                          # TiKV-specific implementation and setup
```



---

## 8) Final Status and Future Work

### Current Status

The project successfully demonstrated that SPTAG can be benchmarked against multiple KV storage backends and that distributed KV integration is possible. The team also built cloud infrastructure, benchmark scripts, and documentation to compare backend behavior under ANN-style workloads.

The main project outcome is not just one backend implementation, but a better understanding of what a storage layer for RAG-style ANN serving needs:

- fast read latency,
- efficient batched retrieval,
- scalable storage capacity,
- persistence,
- fault tolerance,
- and a clean interface between ANN search and KV retrieval.

( I would love some further insight into what to add here on the tikv side as well. as well as the sectuion below. )

### Future Work

Important next steps include:

1. Refactor backend integration code so storage logic is cleanly separated from SPTAG search logic.
2. Implement a stronger `MultiGet` API that accepts query context and supports server-side filtering or aggregation.
3. Reduce duplicated distance computation in the search path.
4. Run benchmarks at higher thread counts to measure each backend near maximum throughput.
5. Compare latency at maximum throughput, not only raw QPS.
6. Improve fault-tolerance testing by measuring behavior under node failures.
7. Finalize branch-specific documentation for Aerospike and TiKV.

---

## Acknowledgments

This project was developed for the EC528 final project sequence at Boston University. We thank our mentor, Qi Chen, for guidance on SPTAG, distributed KV design, benchmarking methodology, and performance optimization directions.
