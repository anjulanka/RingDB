================================================================================
                    RINGDB PROJECT KNOWLEDGE MANIFESTO
================================================================================
[SYSTEM OVERVIEW]
Project Name:         RingDB
Target Machine:       Windows 11 Pro (Ryzen 7 3800XT 8-Core/16-Thread, 32GB RAM)
Dev Environment:      WSL2 Ubuntu Sandbox via VS Code (Targeting Linux Kernel 5.19+)
Programming Language: Pure C (Strictly avoiding C++ runtime / abstractions)
Wire Protocol:        100% RESP (Redis Serialization Protocol) Compliant
Market Positioning:   An open-source, ultra-high-performance drop-in replacement
                      for Redis and Dragonfly DB.

--------------------------------------------------------------------------------
[CORE ARCHITECTURAL PILLARS]
1. Shared-Nothing Engine: No global mutexes or spinlocks. Database is split 
   into isolated shards matching CPU core count. Threads share nothing.
2. Hardware Thread Affinity: Workers permanently pinned to native hardware 
   CPU cores using 'pthread_setaffinity_np' to maximize cache locality.
3. Native io_uring Ingestion: Uses Linux io_uring with SQPOLL and Multi-Shot
   Accept/Recv to eliminate User-to-Kernel context switching barriers.
4. Lockless SPSC Highways: Inter-core packet routing runs via Single-Producer 
   Single-Consumer queues powered by hardware-level atomics (<stdatomic.h>).
   Pointer transfers take < 10 nanoseconds over L3 cache flushes.
5. Private Arena Storage: Avoids malloc fragmentation. Core shards pre-allocate
   contiguous blocks of RAM, appending writes sequentially for clean cache lines.
6. Zero-Copy RESP Parser: Reads incoming text tokens directly inside network cache
   buffers using SIMD vector acceleration. No heap copies for read workloads.

--------------------------------------------------------------------------------
[LEGAL, GOVERNANCE & MONETIZATION]
- Licensing System: Redis-style "Tri-License" Framework (AGPLv3 / SSPLv1 / RSALv2).
- Protection Strategy: Permanently blocks cloud giants (like AWS) from hosting
  RingDB as a commercial managed service without open-sourcing their stack.
- Contributor Guardrails: Branch protection rules requiring automated PR approval,
  strict code quality audits, QPS benchmark proofs, and an automated CLA bot.

--------------------------------------------------------------------------------
[REPOSITORY ARCHITECTURE BLUEPRINT]
RingDB/
├── .github/workflows/       # Automated CI/CD pipelines
├── include/                 # C Public Interface Declarations (.h)
│   ├── ring_db.h            # Globals & Configurations
│   ├── ring_highway.h       # SPSC Atomic Highway Channels
│   └── arena.h              # Private Shard Arena Allocations
├── src/                     # Core Source Implementation (.c)
│   ├── main.c               # Thread Management & Bootstrap Entry
│   ├── iouring_backend.c    # Async Network Ingestion
│   ├── ring_highway.c       # Inter-core Communication Channels
│   ├── arena.c              # Linear Allocation Pools
│   ├── parser.c             # Zero-copy SIMD RESP Tokenizer
│   └── cli.c                # Interactive Client Tool (ringdb-cli)
├── docs/                    # Architecture charts (README / ARCHITECTURE.md)
└── CMakeLists.txt          # Root Project Compilation Instructions

--------------------------------------------------------------------------------
[PROJECT ROADMAP & REMAINING TASKS]
We have successfully initialized the repository, finalized the high-level data
flows, written the markdown documentation (README, ARCHITECTURE, CONTRIBUTING),
configured the Tri-License framework, and updated your Windows 11 WSL2 environment.

NEXT STEPS IN LINE:
Step 1: Write the master CMakeLists.txt file to link liburing and configure -O3 flags.
Step 2: Code the foundational main.c entry point for core thread pool initialization.
Step 3: Build the core data structures (SPSC Rings, Arena Allocator, Hash Maps).
================================================================================
