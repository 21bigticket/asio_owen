#!/bin/bash
# ============================================================
# asio_owen ASan 构建 + 部署 + 冒烟测试 (Ubuntu 构建)
# 用法: bash rebuild_asan.sh
# ============================================================
set -euo pipefail

ROOT=/mnt/mac/Users/mac/code/croot/asio_owen
BUILD_DIR="$ROOT/build_asan_ubuntu"
SERVICE=asio-owen-asan.service
cd "$ROOT"

echo "=== [1/6] 停止旧服务 ==="
sudo systemctl stop asio-owen.service 2>/dev/null || true
#sudo systemctl stop asio-owen-asan.service 2>/dev/null || true
sleep 2

echo "=== [2/6] 删除旧构建 ==="
rm -rf "$BUILD_DIR"

echo "=== [3/6] cmake 配置 (ASan) ==="
/usr/bin/cmake -B "$BUILD_DIR" -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
  -DBUILD_TESTING=OFF \
  2>&1 | tail -3

echo "=== [4/6] 编译 ==="
/usr/bin/cmake --build "$BUILD_DIR" --target server -j1 2>&1 | tail -5

echo "=== [5/6] 启动服务 ==="
dmesg -c > /dev/null 2>&1 || true

# 使用 systemctl 启动 ASan 服务
sudo systemctl restart "$SERVICE"
sleep 8

echo "=== [6/6] 冒烟测试 ==="
echo -n "Health: "; curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8081/api/health; echo ""
echo -n "MySQL:  "; curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8081/api/mysql;  echo ""
echo -n "Redis:  "; curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8081/api/redis;  echo ""
echo -n "Gateway:"; curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey -X POST -H "Content-Type: application/json" -d "{}"; echo ""

echo ""
echo "=== ASan 错误 ==="
cat /tmp/asan_stderr.log 2>/dev/null | grep -A2 "ERROR:" | head -20
echo ""
echo "=== segfault: $(dmesg 2>/dev/null | grep -c segfault) ==="
echo "=== 完成 ==="