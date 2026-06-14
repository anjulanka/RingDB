# 📝 RingDB Project Knowledge Manifesto

## 🌐 System Overview
*   **Project Name:** RingDB
*   **Target Machine:** Windows 11 Pro (Ryzen 7 3800XT 8-Core/16-Thread, 32GB RAM)
*   **Dev Environment:** WSL2 Ubuntu Sandbox via VS Code (Targeting Linux Kernel 5.19+)
*   **Programming Language:** Pure C (Strictly avoiding C++ runtime / abstractions)
*   **Wire Protocol:** 100% RESP (Redis Serialization Protocol) Compliant
*   **Market Positioning:** An open-source, ultra-high-performance drop-in replacement for Redis and Dragonfly DB. Fully includes native server and client CLI utilities.

---

## 🏗️ Core Architectural Pillars

1.  **Shared-Nothing Engine:** No global mutexes or spinlocks. The database is split into isolated shards matching the CPU core count. Threads share nothing.
2.  **Hardware Thread Affinity:** Workers are permanently pinned to native hardware CPU cores using `pthread_setaffinity_np` to maximize cache locality.
3.  **Native `io_uring` Ingestion:** Uses Linux `io_uring` with `SQPOLL` and Multi-Shot Accept/Recv to eliminate User-to-Kernel context-switching barriers.
4.  **Lockless SPSC Highways:** Inter-core packet routing runs via Single-Producer Single-Consumer queues powered by hardware-level atomics (`<stdatomic.h>`). Pointer transfers take less than 10 nanoseconds over L3 cache flushes.
5.  **Private Arena Storage:** Avoids `malloc` fragmentation. Core shards pre-allocate contiguous blocks of RAM, appending writes sequentially for clean cache lines.
6.  **Zero-Copy RESP Parser:** Reads incoming text tokens directly inside network cache buffers using SIMD vector acceleration. No heap copies occur for read workloads.
7.  **Dual Ecosystem Binaries:** The architecture splits into `ringdb-server` (the core multithreaded network engine) and `ringdb-cli` (an interactive console prompt leveraging the lightweight `linenoise` utility for text history and autocomplete).

---

## 🔏 Legal, Governance & Monetization

*   **Licensing System:** Redis-style "Tri-License" Framework (AGPLv3 / SSPLv1 / RSALv2).
*   **Protection Strategy:** Permanently blocks cloud giants (like AWS) from hosting RingDB as a commercial managed service without open-sourcing their stack.
*   **Contributor Guardrails:** Branch protection rules requiring automated PR approval, strict code quality audits, QPS benchmark proofs, and an automated CLA bot.

---

## 📂 Repository Architecture Blueprint

```text
RingDB/
├── .github/workflows/       # Automated CI/CD pipelines
├── deps/                    # Lightweight third-party primitives (linenoise)
├── docs/                    # Architecture charts (README / ARCHITECTURE.md)
├── include/                 # C Public Interface Declarations (.h)
│   ├── ring_db.h            # Globals & Configurations
│   ├── ring_highway.h       # SPSC Atomic Highway Channels
│   ├── arena.h              # Private Shard Arena Allocations
│   └── parser.h             # RESP Command Parser Declarations
├── src/                     # Core Source Implementation (.c)
│   ├── main.c               # Thread Management & Server Bootstrap (ringdb-server)
│   ├── iouring_backend.c    # Async Network Ingestion
│   ├── ring_highway.c       # Inter-core Communication Channels
│   ├── arena.c              # Linear Allocation Pools
│   ├── parser.c             # Zero-copy SIMD RESP Tokenizer
│   └── cli.c                # Interactive Client Tool Terminal (ringdb-cli)
└── CMakeLists.txt          # Root Project Compilation Instructions (Dual Targets)
```

---

## 🏁 Project Roadmap & Progress Tracker

We have successfully initialized the repository folder layout, finalized data flows, and written all markdown documentation. We also configured the Redis-style Tri-License and added the integrated CLI tool ecosystem.

### 🎯 Remaining Tasks in Order:
*   [ ] **Step 1:** Write the master `CMakeLists.txt` file to link `liburing`, setup both execution targets, and configure compiler optimizations flags (`-O3` and `-march=native`).
*   [ ] **Step 2:** Code the foundational `src/main.c` entry point for core thread pool initialization.
*   [ ] **Step 3:** Build the core data structures (SPSC Rings, Arena Allocator, Hash Maps).
