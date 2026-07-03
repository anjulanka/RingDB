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

# Explicitly link standard thread support and native io_uring system drivers
target_link_libraries(ringdb-server PRIVATE pthread ${URING_LIB})

# ------------------------------------------------------------------------------
# 🖥️ TARGET 2: THE INTERACTIVE CLI UTILITY (ringdb-cli)
# ------------------------------------------------------------------------------
set(CLI_SOURCES
    src/cli.c
)

add_executable(ringdb-cli ${CLI_SOURCES})

# Link threads for client interface stability
target_link_libraries(ringdb-cli PRIVATE pthread)

# ------------------------------------------------------------------------------
# 🏎️ TARGET 3: BENCHMARK RUNNER
# ------------------------------------------------------------------------------
add_custom_target(benchmark
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_benchmark.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS ringdb-server
    COMMENT "Running high-velocity QPS benchmarks..."
)
