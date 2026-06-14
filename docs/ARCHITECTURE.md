
# 🏗️ RingDB Component Architecture & Data Flow

RingDB is a modern, ultra-high-performance key-value data store written in pure C. It uses a **Shared-Nothing** multi-threaded architecture optimized natively for modern Linux kernels via **`io_uring`** and cross-core **Lockless SPSC Ring Highways**.

---

## 🗺️ High-Level System Blueprint

```text
==================================================================================================
                                    [ NETWORK INGESTION LAYER ]
==================================================================================================
                                    Client Traffic (e.g., ringdb-cli)
                                                  │
                                                  ▼
                                      ╔══════════════════════╗
                                      ║ Linux Kernel Network ║ 
                                      ║ Subsystem (Port 6379)║
                                      ╚══════════╦═══════════╝
                                                 │ (SO_REUSEPORT Balance Loading)
         ┌───────────────────────────────────────┼───────────────────────────────────────┐
         ▼                                       ▼                                       ▼
==================================================================================================
                                      [ CORE PROCESSING LAYER ]
==================================================================================================
  ┌──────────────────────────────┐        ┌──────────────────────────────┐        ┌──────────────────────────────┐
  │         CPU CORE 0           │        │         CPU CORE 1           │        │         CPU CORE 2           │
  │  (Pinned: Thread Affinity)   │        │  (Pinned: Thread Affinity)   │        │  (Pinned: Thread Affinity)   │
  │ ──────────────────────────── │        │ ──────────────────────────── │        │ ──────────────────────────── │
  │ [Native Linux io_uring Ring] │        │ [Native Linux io_uring Ring] │        │ [Native Linux io_uring Ring] │
  │   - Multi-Shot Accept/Recv   │        │   - Multi-Shot Accept/Recv   │        │   - Multi-Shot Accept/Recv   │
  │   - Kernel SQPOLL Mode       │        │   - Kernel SQPOLL Mode       │        │   - Kernel SQPOLL Mode       │
  └──────────────┬───────────────┘        └──────────────┬───────────────┘        └──────────────┬───────────────┘
                 │                                       │                                       │
                 ▼                                       ▼                                       ▼
  ┌──────────────────────────────┐        ┌──────────────────────────────┐        ┌──────────────────────────────┐
  │   [Zero-Copy RESP Parser]    │        │   [Zero-Copy RESP Parser]    │        │   [Zero-Copy RESP Parser]    │
  │  - Vectorized SIMD Scanner   │        │  - Vectorized SIMD Scanner   │        │  - Vectorized SIMD Scanner   │
  │  - Inline Command Hashing    │        │  - Inline Command Hashing    │        │  - Inline Command Hashing    │
  └──────────────┬───────────────┘        └──────────────┬───────────────┘        └──────────────┬───────────────┘
                 │                                       │                                       │
                 ▼                                       ▼                                       ▼
  ┌──────────────────────────────┐        ┌──────────────────────────────┐        ┌──────────────────────────────┐
  │   [xxHash Routing Protocol]  │        │   [xxHash Routing Protocol]  │        │   [xxHash Routing Protocol]  │
  │    (Key % Total Workers)     │        │    (Key % Total Workers)     │        │    (Key % Total Workers)     │
  └──────────────┬───────────────┘        └──────────────┬───────────────┘        └──────────────┬───────────────┘
                 │                                       │                                       │
        ┌────────┴────────┐                     ┌────────┴────────┐                     ┌────────┴────────┐
        │  Is Local Hit?  │                     │  Is Local Hit?  │                     │  Is Local Hit?  │
        └─┬─────────────┬─┘                     └─┬─────────────┬─┘                     └─┬─────────────┬─┘
          │ (Yes)       │ (No)                    │ (Yes)       │ (No)                    │ (Yes)       │ (No)
          ▼             ▼                         ▼             ▼                         ▼             ▼
================════════════════==================================================================
                                 [ CROSS-CORE LOCKLESS HIGHWAY ]
==================================================================================================
          │       ╔═════════════════════════════════════════════════════════════════════╗       │
          │       ║         Bi-Directional Array of Lockless SPSC Ring Buffers          ║       │
          │       ║      - Driven by Hardware C Atomics (<stdatomic.h>)                 ║       │
          │       ║      - Single-Producer Single-Consumer (No Thread Locking / Mutex) ║       │
          │       ╚══════════════╦══════════════════════════════╦═══════════════════════╝       │
          │                      │                              │                               │
          └────────┐             │                              │             ┌─────────────────┘
                   ▼             ▼                              ▼             ▼
==================================================================================================
                                      [ DATA STORAGE LAYER ]
==================================================================================================
  ┌──────────────────────────────┐        ┌──────────────────────────────┐        ┌──────────────────────────────┐
  │     [PRIVATE SHARD 0]        │        │     [PRIVATE SHARD 1]        │        │     [PRIVATE SHARD 2]        │
  │                              │        │                              │        │                              │
  │  - Main RAM Shard Hash Map   │        │  - Main RAM Shard Hash Map   │        │  - Main RAM Shard Hash Map   │
  │  - Contiguous Arena Memory   │        │  - Contiguous Arena Memory   │        │  - Contiguous Arena Memory   │
  │    Allocator (Non-Mallocated)│        │    Allocator (Non-Mallocated)│        │    Allocator (Non-Mallocated)│
  │  - Array-Backed Skip Lists   │        │  - Array-Backed Skip Lists   │        │  - Array-Backed Skip Lists   │
  └──────────────────────────────┘        └──────────────────────────────┘        └──────────────────────────────┘
==================================================================================================
```

---

## ⚡ Core Architectural Pillars

### 1. Hardware Pinning & Thread Affinity
RingDB builds a worker pool matching your exact system CPU core count. Using `pthread_setaffinity_np`, each worker thread locks permanently to a single hardware core. This eliminates operating system context-switching tax and maximizes CPU L1/L2 cache locality.

### 2. Native `io_uring` Ingestion Engine
Traditional engines rely on slow system loops (`epoll`) to fetch network data. RingDB builds an independent `io_uring` ring buffer per core worker. Utilizing **Kernel Submission Queue Polling (SQPOLL)** and **Multi-shot Requests**, client data drops directly into shared-memory buffers. This shaves off traditional User-to-Kernel space context boundaries.

### 3. Zero-Copy RESP Parser
Incoming Redis Serialization Protocol (RESP) tokens are processed directly inside the active `io_uring` buffer ring.
* **Vectorized Scanning**: SIMD instructions check 32-byte chunks simultaneously to isolate structural markers (`\r\n`) rapidly.
* **Inline Hashing**: Text commands are matched via fast integer hashes instead of heavy string comparisons.

### 4. Lockless SPSC Ring Highways
Cross-shard communication uses zero mutexes or heavy locks. RingDB links all execution cores using a web of **Single-Producer Single-Consumer (SPSC)** ring channels. Driven by low-level atomic memory ordering pointers (`<stdatomic.h>`), reference pointers jump across cores in under 10 nanoseconds over native L3 hardware cache synchronization loops.

### 5. Private Shard Arena Storage
To prevent memory fragmentation and allocation spikes caused by traditional heap heap allocations (`malloc`), each core manages an isolated, contiguous block of main motherboard RAM. Data structures (Hash Maps, Lists, Skip Lists) append elements sequentially inside this private space. This guarantees high CPU cache predictability.

---

## 🔄 End-to-End Execution Lifecycles

### Path A: The Ephemeral Read Data Flow (`GET user:9999`)
1. **Ingestion**: Raw bytes land on Port 6379. The Linux Kernel maps the packet to Core 0's native `io_uring` submission path.
2. **Parsing**: Core 0's SIMD scanner tokenizes the parameters. The data string remains inside the buffer while an inline hash evaluates the command.
3. **Routing**: The router calculates `user:9999 % Total_Cores`. The index resolves to **Shard 2**.
4. **Highway Leap**: Core 0 drops a tiny 16-byte tracking packet containing a buffer reference onto the `Core0_to_Core2` SPSC atomic ring channel.
5. **Execution**: Core 2 extracts the token during its queue tick, queries its private **Shard 2 Hash Map**, and drops the output reference onto the return lane (`Core2_to_Core0`).
6. **Network Output**: Core 0 captures the response payload and passes it straight to its native `io_uring` send pipeline. The temporary network buffer slot is instantly recycled. **Total memory copies: 0.**

### Path B: The Persistent Write Data Flow (`SET user:1122 "active"`)
1. **Ingestion & Parsing**: Follows the identical path to Core 0, tokenizing the raw payload.
