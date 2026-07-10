#!/usr/bin/env bash
# run_native_bench.sh — one-command RingDB native benchmark
# Usage: ./run_native_bench.sh [ringdb-bench flags]
#   Defaults: -t 4 -c 8 -P 16 -d 10 -n 100000 -r 50
#
# Run from the build directory:
#   cd build && ../scripts/run_native_bench.sh
#   cd build && ../scripts/run_native_bench.sh -t 8 -c 16 -P 32 -d 30 -r 0

set -e

BENCH_ARGS="${*:--t 4 -c 8 -P 16 -d 10 -n 100000 -r 50}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$(pwd)"

echo "============================================================="
echo " RingDB Native Benchmark"
echo " Args: $BENCH_ARGS"
echo "============================================================="

# Kill any existing server
pkill ringdb-server 2>/dev/null || true
sleep 0.3

echo "[bench] Starting ringdb-server..."
"${BUILD_DIR}/ringdb-server" &
SERVER_PID=$!
sleep 1

# Validate server is up
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "[bench] ERROR: ringdb-server failed to start"
    exit 1
fi
echo "[bench] Server PID $SERVER_PID is live."

# Run benchmark
# shellcheck disable=SC2086
"${BUILD_DIR}/ringdb-bench" $BENCH_ARGS
BENCH_EXIT=$?

echo "[bench] Stopping server..."
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true

exit $BENCH_EXIT
