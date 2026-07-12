#!/bin/bash
set -e

cd "$(dirname "$0")"

echo "=== Configuring CMake ==="
cmake_args=(-B build -S . -Wno-dev -DCMAKE_BUILD_TYPE=Release)
if [[ -n "${CC:-}" ]]; then
    cmake_args+=("-DCMAKE_C_COMPILER=${CC}")
fi
if [[ -n "${CXX:-}" ]]; then
    cmake_args+=("-DCMAKE_CXX_COMPILER=${CXX}")
fi
cmake "${cmake_args[@]}" 2>&1 | tail -3

echo "=== Building ==="
cmake --build build --target server -j4 2>&1 | tail -3

echo "=== Killing old server ==="
pkill -f 'build/server' 2>/dev/null || true
sleep 1

echo "=== Starting server ==="
cd build
./server &>/tmp/server.log &
SERVER_PID=$!
sleep 3

if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "=== Server failed to start ==="
    cat /tmp/server.log
    exit 1
fi

echo "=== Verifying ==="
curl -s http://127.0.0.1:8081/api/health && echo ' health OK'
curl -s http://127.0.0.1:8081/api/redis && echo ' redis OK'
curl -s --max-time 5 http://127.0.0.1:8081/api/mysql | head -c 60 && echo ' mysql OK'

echo "=== Done, PID=$SERVER_PID ==="
