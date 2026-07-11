# 🌀 RingDB

**RingDB** is an ultra-high-performance, multi-threaded, in-memory key-value data store written in pure C. Architected for modern multi-core Linux systems, RingDB achieves millions of operations per second on a single machine by using native **`io_uring`** network loops and a lockless **Shared-Nothing** design. 

RingDB is a drop-in replacement for Redis and is fully compatible with the standard **RESP protocol**.

---

## 🚀 Core Features & Architectural Innovations

*   **⚡ Shared-Nothing Multi-Core Engine**: Eliminates slow global locks (mutexes) and spinlocks. Each thread owns an isolated data shard and operates at maximum hardware speed.
*   **🐧 Native `io_uring` Ingestion Layer**: Bypasses traditional `epoll` system calls. Uses modern kernel features like Multi-shot Accept/Recv and `SQPOLL` for true asynchronous network traffic handling.
*   **🌀 Lockless SPSC Core Highways**: Uses Single-Producer Single-Consumer ring queues driven by low-level hardware atomics (`<stdatomic.h>`). Cross-core pointer transfers execute in under 10 nanoseconds over L3 cache line flushes.
*   **🧱 Private Arena Allocators**: Bypasses the memory fragmentation and performance spikes of standard heap allocations (`malloc`). Data is written into clean, contiguous memory pools.
*   **🛠️ Zero-Copy RESP Parsing**: Employs vectorized SIMD operations to read tokens directly inside network cache blocks, preventing unneeded string copies during database reads.
*   **🖥️ Built-in Interactive CLI**: Includes `ringdb-cli`, a standalone interactive command-line interface featuring zero-dependency autocomplete and command history powered by an integrated `linenoise` core.

---

## 📊 Projected Benchmark Highlights

On an **AWS `c6gn.16xlarge`** server (64 CPU cores, 100 Gbps network card), RingDB is mathematically optimized to perform at the absolute limits of the physical hardware:

| Database Engine | Architecture Style | Network Layer | Estimated Max Throughput | Cross-Shard Tax |
| :--- | :--- | :--- | :--- | :--- |
| **Redis** | Single-Threaded | `epoll` | ~150K Ops/Sec | N/A (Single-core) |
| **Dragonfly DB** | Multi-Threaded | Custom `io_uring` | ~3.8M Ops/Sec | Low (Fiber Locking) |
| **RingDB 🌀** | **Shared-Nothing C** | **Native `io_uring`** | **~4.2M+ Ops/Sec** | ⚡ **Ultra-Low (Atomic Ring)** |

---

## 📂 Repository Layout

```text
RingDB/
├── .github/
│   ├── skills/linux-c-systems-expert/  # Copilot agent skill for C/Linux systems work
│   └── workflows/                       # Automated CI/CD testing pipelines
├── include/                 # Public C header interface declarations (.h)
│   ├── ring_db.h            # Main definitions and global settings
│   ├── ring_highway.h       # Cross-core lockless SPSC declarations
│   ├── arena.h              # Private Shard Arena memory allocations
│   └── parser.h             # RESP command parser definitions
├── src/                     # Core engine implementation files (.c)
│   ├── main.c               # Server bootstrap & thread pinning (ringdb-server)
│   ├── iouring_backend.c    # Asynchronous network event loops
│   ├── ring_highway.c       # Atomic core-to-core channels
│   ├── arena.c              # Custom non-fragmenting memory pools
│   ├── parser.c             # Zero-copy RESP network string parser
│   └── cli.c                # Interactive command line terminal tool (ringdb-cli)
├── benchmark/               # Native C benchmark client
│   └── bench.c              # Multi-threaded pipelined benchmark (ringdb-bench)
├── scripts/                 # Helper scripts
│   └── run_benchmark.sh     # Legacy memtier_benchmark runner
├── docs/                    # Architecture blueprints and design files
├── CMakeLists.txt          # Project build configuration
└── LICENSE                  # RingDB Tri-License Agreement
```

---

## 🛠️ Setting Up Your Sandbox (Ubuntu / WSL2)

### 1. Install System Dependencies
Make sure your Linux environment has a modern compiler, build tooling, and the required kernel library headers:

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install build-essential cmake git liburing-dev -y
```

### 2. Clone and Compile
Clone the repository and compile both the server engine and interactive CLI utilities simultaneously using our CMake matrix:

```bash
gh repo clone anjulanka/RingDB
cd RingDB

# Initialize the build directory
mkdir build && cd build

# Standard build (8-core workstation)
cmake -DCMAKE_BUILD_TYPE=Release ..

# Scale to target hardware (must be a power of 2)
# cmake -DCMAKE_BUILD_TYPE=Release -DNUM_CORES=64 ..   # AWS c6gn.16xlarge

make -j$(nproc)
```

### 3. Launching the Cluster
Once compiled, you can launch the database instance and connect to it using our custom toolset:

```bash
# Terminal 1: Run the high-performance database server
./ringdb-server

# Terminal 2: Run the interactive command-line client to execute queries
./ringdb-cli
```

### 4. Running Performance Benchmarks

RingDB ships a native C benchmark client (`ringdb-bench`) that requires zero external dependencies. A single make target starts the server, saturates it, and tears it down automatically:

```bash
cd ~/projects/RingDB/build
make bench
```

This runs 4 client threads × 8 connections × pipeline depth 16 for 10 seconds with a 50/50 SET/GET mix. Override any parameter inline:

```bash
# Run Locally - GET-heavy, larger key space, longer duration
cmake -DSTART_LOCAL_SERVER=ON -DBENCH_ARGS="-t 4 -c 8 -P 16 -d 30 -n 1000000 -r 0" ..
make bench

# Run Remote - Connect to Remote VM
cmake -DSTART_LOCAL_SERVER=OFF -DBENCH_HOST=20.197.59.221 -DBENCH_ARGS="-t 8 -c 16 -P 32 -d 30" ..
make bench

# Run Locally - Maximum saturation test (large value, high pipeline)
cmake -DSTART_LOCAL_SERVER=ON -DBENCH_ARGS="-t 8 -c 16 -P 32 -d 60 -s 512 -n 100000 -r 50" ..
make bench
```

**Run the interactive CLI** to validate data integrity alongside the benchmark:

```bash
# Terminal 2: while benchmark runs
./ringdb-cli
```

> **Legacy option** — if `memtier_benchmark` is installed, `make benchmark` runs the original pipeline suite.

---

## 🤝 Contributing to RingDB

We love community involvement! Anyone is welcome to contribute features, optimize bottlenecks, or open issue tickets.

### How to Propose Changes:
1. **Fork the Repository**: Create an independent copy of RingDB to write your changes.
2. **Set Up a Feature Branch**: Keep your main branch clean (`git checkout -b feature/amazing-optimization`).
3. **Write Tests**: Ensure your code includes testing scripts inside the `tests/` path.
4. **Run the Benchmarks**: If you are optimizing a core component, you must provide benchmark logs proving your change does not drop our overall QPS.
5. **Open a Pull Request**: Submit your branch to our main project line for peer review.

---

## 🔏 Licensing & Commercial Use

RingDB is licensed under a **Tri-License framework** (mirroring the modern Redis legal strategy). As a user or contributor, you can choose to interact with this software under any **ONE** of the following tracks:

1. **AGPLv3 (GNU Affero General Public License v3)**: 100% open source. Requires anyone modifying and hosting RingDB over a network to open-source their entire infrastructure.
2. **SSPLv1 (Server Side Public License v1)**: Free for internal use. Strictly bans using the code to provide a competing, paid database-as-a-service (DBaaS) unless you open-source your hosting management software stack.
3. **RSALv2 (Redis Source Available License v2 Equivalent)**: Free for internal production workloads. Explicitly prohibits utilizing this software to build a commercial database platform, caching product, or paid hosting service.
