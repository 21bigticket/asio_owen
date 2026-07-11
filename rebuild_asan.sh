#!/bin/bash
# ============================================================
# asio_owen ASan 构建 + 部署 + 冒烟测试
# 用法: bash rebuild_asan.sh
# ============================================================
set -euo pipefail

ROOT=/mnt/mac/Users/mac/code/croot/asio_owen
cd "$ROOT"

echo "=== [1/6] 杀旧进程 ==="
pkill -9 -f "build_asan.*server" 2>/dev/null || true
pkill -9 -f "build.*server" 2>/dev/null || true
sleep 2

echo "=== [2/6] 删除旧构建 ==="
rm -rf build_asan

echo "=== [3/6] cmake 配置 (ASan) ==="
/usr/bin/cmake -B build_asan -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
  -DBUILD_TESTING=OFF \
  2>&1 | tail -3

echo "=== [4/6] 编译 ==="
/usr/bin/cmake --build build_asan --target server -j1 2>&1 | tail -5

echo "=== [5/6] 启动服务 ==="
dmesg -c > /dev/null 2>&1 || true
cd build_asan
cp -r ../config.d . 2>/dev/null || true
cp -r ../jwt_keys . 2>/dev/null || true
rm -f server.log
ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:log_path=/tmp/asan_crash:symbolize=1:external_symbolizer_path=$(which llvm-symbolizer 2>/dev/null || which addr2line)" \
  nohup ./server > /tmp/asan_stdout.log 2>/tmp/asan_stderr.log &
sleep 8

echo "=== [6/6] 冒烟测试 ==="
echo -n "Health: "
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8081/api/health || echo "FAIL"
echo ""

echo -n "MySQL:  "
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8081/api/mysql || echo "FAIL"
echo ""

echo ""
echo "=== ASan 错误 ==="
cat /tmp/asan_stderr.log 2>/dev/null | grep -A2 "ERROR:" | head -20
echo ""
echo "=== segfault: $(dmesg 2>/dev/null | grep -c segfault) ==="
echo "=== 完成 ==="
