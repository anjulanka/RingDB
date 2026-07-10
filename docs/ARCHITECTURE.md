
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
Traditional engines rely on slow system loops (`epoll`) to fetch network data. RingDB builds an independent `io_uring` ring buffer per core worker. Utilizing **Kernel Submission Queue Polling (SQPOLL)** and **Multi-shot Requests**, client data drops directly into shared-memory buffers. Each accepted client socket has `TCP_NODELAY` set immediately so small responses are transmitted without Nagle-algorithm delay.

### 3. Full-Pipeline Command Processing (3-Phase)
The `OP_READ` handler processes **all commands** that arrive in a single recv buffer before issuing any send — a true pipeline loop with three distinct phases:

**Phase 1 — Parse + Dispatch (non-blocking):**
- For each command in the recv buffer, compute the target shard hash.
- **Local shard hit**: Execute inline (zero wait) — `db_get`/`db_set` directly into this core's arena. Result recorded in a stack-allocated slot.
- **Remote shard hit**: Push request onto the `CoreX → CoreY` SPSC lane immediately and move to the next command. All N remote requests are in-flight simultaneously by the time Phase 1 finishes.

**Phase 2 — Parallel Collect (~100 ns total, not N × 100 ns):**
- Poll `highway_process_requests()` until every dispatched highway request has a `OP_HIGHWAY_RESPONSE`. Remote cores process all in-flight requests in parallel in their own event loops. Total wait ≈ **max(individual RTTs)** ≈ one L3 cache hop (~100 ns) regardless of pipeline depth.

**Phase 3 — Ordered Assemble + Single Send:**
- Walk the slot array in original command order and write RESP replies into `resp_acc`. Submit one `io_uring_prep_send` for the entire combined response. **One network syscall per pipeline burst.**

MGET uses the existing async scatter-gather path (Step B of the event loop) unchanged.

### 4. Zero-Copy RESP Parser
Incoming Redis Serialization Protocol (RESP) tokens are processed directly inside the active `io_uring` buffer ring.
* **`memchr`-based scanning**: Uses `memchr` to locate `\r\n` markers, which compilers auto-vectorize with SSE2/NEON.
* **Inline Hashing**: Text commands are matched via fast 32-bit integer hashes instead of string comparisons.
* **Bytes-consumed return value**: The parser returns the number of bytes consumed, allowing the pipeline loop to advance through the buffer for the next command without re-scanning.

### 5. Lockless SPSC Ring Highways
Cross-shard communication uses zero mutexes or heavy locks. RingDB links all execution cores using a web of **Single-Producer Single-Consumer (SPSC)** ring channels driven by `<stdatomic.h>` acquire/release semantics.

**Cache layout** (critical for performance):
* `head` (producer-owned) and `tail` (consumer-owned) sit on **separate 64-byte cache lines** — eliminating the false-sharing bounce that previously cost ~100 ns per push/pop.
* Ring size is 256 entries — matching the 256 possible in-flight request IDs (`uint8_t`) so the ring can never overflow under any valid workload.

### 6. Private Shard Arena Storage
Each core manages an isolated 1 GB contiguous block of RAM. On startup, `mmap` first attempts **2 MB huge pages** (`MAP_HUGETLB`) to reduce TLB pressure by 512× versus 4 KB pages, falling back silently to standard pages if the OS has not pre-allocated huge pages.

---

## ⚙️ System Constants Reference

| Constant | Value | File | Rationale |
|---|---|---|---|
| `NUM_CORES` | 8 (default) | `ring_db.h` | Set via `cmake -DNUM_CORES=N`; must be power of 2 |
| `RING_SIZE` | 256 | `ring_highway.h` | Matches `uint8_t` request ID range; 256×16B×64 rings = 256 KB (L2-resident) |
| `HASH_MAP_BUCKETS` | 262144 | `ring_db.h` | 256K buckets/shard → avg chain 0.015 at 1M keys; 2 MB/shard bucket array |
| `ARENA_SHARD_SIZE` | 1 GB | `arena.h` | Per-shard `mmap` reservation; huge-page backed when available |
| `READ_BUF_SIZE` | 65536 | `iouring_backend.c` | 64 KB handles pipeline=16 × 512B values without truncation |
| `QUEUE_DEPTH` | 8192 | `iouring_backend.c` | io_uring SQ/CQ depth per core |
| `MAX_RESP_ARGS` | 64 | `parser.h` | Supports MGET with up to 63 keys per command |
| `sq_thread_idle` | 10 ms | `iouring_backend.c` | SQPOLL parks after 10 ms idle; stays awake perpetually under load |

---

## 🔄 End-to-End Execution Lifecycles

### Path A: The Ephemeral Read Data Flow (`GET user:9999`)
1. **Ingestion**: Raw bytes land on Port 6379. `SO_REUSEPORT` maps the connection to one core's `io_uring` ring. `TCP_NODELAY` is set on the accepted socket.
2. **Phase 1 — Parse + Dispatch**: The `OP_READ` pipeline loop iterates over every command in the recv buffer. For `GET user:9999`, `memchr`-based RESP parsing extracts the key; djb2 hash computes `target_shard = hash & (NUM_CORES-1)`.
   - **Local hit** (target == current core): direct `db_get` lookup; result stored in stack slot immediately.
   - **Remote hit** (target != current core): `highway_send_request(core_id, target_shard, &req)` pushes the packet onto the `CoreX → CoreY` SPSC lane and **returns immediately** — no blocking. The slot is marked `is_highway=true`. The loop continues parsing the next command.
3. **Phase 2 — Parallel Collect**: After all commands are dispatched, `highway_process_requests()` is polled until every `is_highway` slot has `REQ_STATE_RESPONSE_READY`. Remote cores process all in-flight requests simultaneously; total wait ≈ **one L3 cache round-trip (~100 ns)** regardless of how many remote commands were dispatched.
4. **Phase 3 — Ordered Assemble**: The slot array is walked in original command order. Bulk-string GET replies and `$-1\r\n` nil replies are written into `resp_acc`.
5. **Batch send**: A single `io_uring_prep_send` ships `resp_acc` in one shot. **One syscall for N commands.**

### Path B: The Persistent Write Data Flow (`SET user:1122 "active"`)
1. **Ingestion & Parsing**: Identical Phase 1 entry. Key hash determines owning shard.
2. **Phase 1 — Local write**: `db_set` executes inline; `success=true` stored in slot. Next command parsed immediately.
3. **Phase 1 — Remote write**: `highway_send_request` dispatches the SET to the owner core non-blocking. Loop advances.
4. **Phase 2 — Parallel Collect**: All remote SET responses arrive concurrently. Remote cores write key+value into their own isolated arena and push `HS_SUCCESS`.
5. **Phase 3 — Assemble + Batch send**: `+OK\r\n` written per SET slot in order. One combined send.
