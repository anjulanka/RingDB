#!/bin/bash
set -e

PORT=6379
HOST="127.0.0.1"

CLIENT_THREADS=2 
CONNECTIONS_PER_THREAD=5
PIPELINE_DEPTH=16    
TEST_DURATION=5            

echo "🌀 Launching RingDB High-Performance Benchmark 🌀"

echo "-> Starting ringdb-server in background..."
./build/ringdb-server &
SERVER_PID=$!
sleep 2 

echo "-> Saturating Server with memtier_benchmark..."
echo "Threads: $CLIENT_THREADS | Conns/Thread: $CONNECTIONS_PER_THREAD | Pipeline: $PIPELINE_DEPTH"

memtier_benchmark \
  -s $HOST \
  -p $PORT \
  --protocol=redis \
  --threads=$CLIENT_THREADS \
  --clients=$CONNECTIONS_PER_THREAD \
  --pipeline=$PIPELINE_DEPTH \
  --test-time=$TEST_DURATION \
  --ratio=1:1 \
  -d 32 \
  --key-pattern=S:S

echo "-> Benchmark complete. Stopping server..."
kill $SERVER_PID
echo "✅ Done."
