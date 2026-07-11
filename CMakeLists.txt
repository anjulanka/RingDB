# ==============================================================================
#  Copyright 2026 The RingDB Authors
#  Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
# ==============================================================================

cmake_minimum_required(VERSION 3.16)
project(RingDB C)

# Force modern C11 standard for strict low-level atomics (<stdatomic.h>)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ------------------------------------------------------------------------------
# 🔥 COMPILER OPTIMIZATION FLAGS (Maximum Performance Tuning)
# ------------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -O3                  # Aggressive math and speed optimizations
        -march=native        # Tells compiler to exploit your local Ryzen 7 SIMD vectors
        -Wall                # Turn on all standard code warning guardrails
        -Wextra              # Turn on extra validation warning checks
        -pthread             # Enable native multi-threading support
    )
endif()

# Tell CMake where to look for your internal header (.h) definitions
include_directories(include)

# ------------------------------------------------------------------------------
# 🐧 DEPENDENCY RESOLUTION (Linux Kernel Networking Headers)
# ------------------------------------------------------------------------------
# Locate and link the native asynchronous Linux io_uring library
find_library(URING_LIB uring REQUIRED)
if(NOT URING_LIB)
    message(FATAL_ERROR "liburing library not found! Please run: sudo apt install liburing-dev")
endif()

# ------------------------------------------------------------------------------
# 🏗️ TARGET 1: THE CORE DATABASE ENGINE (ringdb-server)
# ------------------------------------------------------------------------------
set(SERVER_SOURCES
    src/main.c
    src/iouring_backend.c
    src/ring_highway.c
    src/arena.c
    src/parser.c
    src/ring_db_storage.c
)

add_executable(ringdb-server ${SERVER_SOURCES})

# Allow build-time core count without editing source:
#   cmake -DNUM_CORES=64 ..   (must be a power of 2; defaults to 8)
if(NOT DEFINED NUM_CORES)
    set(NUM_CORES 8)
endif()
target_compile_definitions(ringdb-server PRIVATE NUM_CORES=${NUM_CORES})

# Explicitly link standard thread support and native io_uring system drivers
target_link_libraries(ringdb-server PRIVATE pthread ${URING_LIB})

# ------------------------------------------------------------------------------
# 🖥️ TARGET 2: THE INTERACTIVE CLI UTILITY (ringdb-cli)
# ------------------------------------------------------------------------------
set(CLI_SOURCES
    src/cli.c
)

add_executable(ringdb-cli ${CLI_SOURCES})

# Allow build-time target IP specification (defaults to loopback if omitted)
if( NOT DEFINED SERVER_IP)
    set(SERVER_IP "127.0.0.1")
endif()
target_compile_definitions( ringdb-cli PRIVATE SERVER_IP="${SERVER_IP}")

# Link threads for client interface stability
target_link_libraries(ringdb-cli PRIVATE pthread)

# ------------------------------------------------------------------------------
# 🏎️ TARGET 3: NATIVE BENCHMARK BINARY (ringdb-bench)
# ------------------------------------------------------------------------------
set(BENCH_SOURCES
    benchmark/bench.c
)

add_executable(ringdb-bench ${BENCH_SOURCES})
target_link_libraries(ringdb-bench PRIVATE pthread)

# ------------------------------------------------------------------------------
# 🏎️ TARGET 4: ONE-COMMAND NATIVE BENCHMARK (make bench)
# Spins up ringdb-server, saturates it with ringdb-bench, then tears it down.
# Defaults: 4 threads × 8 conns = 32 connections, pipeline=16, 10 s, 50/50 mix.
# Override any flag: make bench BENCH_ARGS="-t 8 -c 16 -P 32 -d 30 -r 0"
# ------------------------------------------------------------------------------

# Toggle to skip local server launch (default: ON)
option(START_LOCAL_SERVER "Start local server" ON)
set(BENCH_HOST "127.0.0.1" CACHE STRING "Host for benchmark")

# Compile the target host definition directly into the benchmark binary
target_compile_definitions(ringdb-bench PRIVATE DEF_HOST="${BENCH_HOST}")

if(NOT DEFINED BENCH_ARGS)
    set(BENCH_ARGS "-t 4 -c 8 -P 16 -d 10 -n 100000 -r 50")
endif()

# Conditional execution: Local server script vs Direct remote targeting
if(START_LOCAL_SERVER)
    add_custom_target(bench
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_native_bench.sh ${BENCH_ARGS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS ringdb-server ringdb-bench
        COMMENT "RingDB native benchmark: ${BENCH_ARGS}"
        USES_TERMINAL
    )
else()
    add_custom_target(bench
        COMMAND ${CMAKE_BINARY_DIR}/ringdb-bench -h ${BENCH_HOST} ${BENCH_ARGS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS ringdb-bench
        COMMENT "RingDB native benchmark: ${BENCH_ARGS}"
    )
endif()

# ------------------------------------------------------------------------------
# 🏎️ TARGET 5: LEGACY BENCHMARK RUNNER (make benchmark — requires memtier)
# ------------------------------------------------------------------------------
add_custom_target(benchmark
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_benchmark.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS ringdb-server
    COMMENT "Running memtier_benchmark suite (requires: sudo apt install memtier-benchmark)"
)
