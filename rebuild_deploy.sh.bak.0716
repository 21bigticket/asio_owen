#!/bin/bash
# ============================================================
# asio_owen 完整重建 + 部署 + 冒烟测试
# 用法: bash rebuild_deploy.sh
# ============================================================
set -euo pipefail

ROOT=/mnt/mac/Users/mac/code/croot/asio_owen
cd "$ROOT"

echo "=== [1/6] 杀旧进程 ==="
PORT=8081
PID=$(lsof -ti:$PORT 2>/dev/null || true)
if [ -n "$PID" ]; then
    echo "Killing PID $PID on port $PORT"
    kill -9 $PID 2>/dev/null || true
fi
sleep 2

echo "=== [2/6] 删除旧构建 ==="
rm -rf build

echo "=== [3/6] cmake 配置 (Release) ==="
/usr/bin/cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF 2>&1 | tail -3

echo "=== [4/6] 编译 ==="
/usr/bin/cmake --build build --target server -j2 2>&1 | tail -5

echo "=== [4.5/6] strip ==="
strip build/server
ls -lh build/server

echo "=== [5/6] 启动服务 ==="
dmesg -c > /dev/null 2>&1 || true
cd build
rm -f server.log
if lsof -ti:$PORT > /dev/null 2>&1; then
    echo "ERROR: Port $PORT still in use, abort"
    exit 1
fi
./server > /dev/null 2>&1 &
sleep 5

echo "=== [6/6] 冒烟测试 ==="
echo -n "Health: "; curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8081/api/health; echo ""
echo -n "MySQL:  "; curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8081/api/mysql;  echo ""
echo -n "Redis:  "; curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8081/api/redis;  echo ""
echo -n "Gateway:"; curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey -X POST -H "Content-Type: application/json" -d "{}"; echo ""

echo ""
echo "=== segfault: $(dmesg | grep -c segfault) ==="
echo "=== 完成 ==="
