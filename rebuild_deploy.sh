#!/bin/bash
# ============================================================
# asio_owen 完整重建 + 测试 + 部署 + 冒烟测试 (Ubuntu 构建)
# 用法: bash rebuild_deploy.sh
# 可选：RUN_TESTS=0 bash rebuild_deploy.sh 跳过 ctest。
# ============================================================
set -euo pipefail

ROOT=${ROOT:-/mnt/mac/Users/mac/code/croot/asio_owen}
BUILD_DIR=${BUILD_DIR:-$ROOT/build_ubuntu}
SERVICE=asio-owen.service
RUN_TESTS=${RUN_TESTS:-1}
KEEP_DEBUG_SYMBOLS=${KEEP_DEBUG_SYMBOLS:-1}
LOG_DIR="$ROOT/logs"
LOG_FILE="$LOG_DIR/rebuild_deploy_$(date -u +%Y%m%dT%H%M%SZ).log"

mkdir -p "$LOG_DIR"
exec > >(tee -a "$LOG_FILE") 2>&1

trap 'echo "=== FAILED at line $LINENO; log: $LOG_FILE ==="' ERR
cd "$ROOT"

echo "=== rebuild_deploy started: $(date -u +%FT%TZ) ==="
echo "=== log: $LOG_FILE ==="

echo "=== [1/7] 停止旧服务 ==="
sudo systemctl stop "$SERVICE" 2>/dev/null || true
sudo systemctl stop asio-owen-asan.service 2>/dev/null || true
sleep 2

echo "=== [2/7] 删除旧构建 ==="
rm -rf "$BUILD_DIR"

echo "=== [3/7] cmake 配置 (Release + tests) ==="
/usr/bin/cmake -B "$BUILD_DIR" -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

echo "=== [4/7] 编译 server 和测试 ==="
/usr/bin/cmake --build "$BUILD_DIR" -j2

if [[ "$RUN_TESTS" == "1" ]]; then
    echo "=== [5/7] ctest ==="
    ctest --test-dir "$BUILD_DIR" --output-on-failure
else
    echo "=== [5/7] ctest skipped (RUN_TESTS=$RUN_TESTS) ==="
fi

echo "=== [6/7] strip + 启动服务 ==="
if [[ "$KEEP_DEBUG_SYMBOLS" == "1" ]]; then
    objcopy --only-keep-debug "$BUILD_DIR/server" "$BUILD_DIR/server.debug"
    strip --strip-unneeded "$BUILD_DIR/server"
    objcopy --add-gnu-debuglink="$BUILD_DIR/server.debug" "$BUILD_DIR/server"
    echo "debug symbols: $BUILD_DIR/server.debug"
else
    strip --strip-unneeded "$BUILD_DIR/server"
fi
ls -lh "$BUILD_DIR/server"

sudo dmesg -c > /dev/null 2>&1 || true
sudo systemctl restart "$SERVICE"
sleep 5

smoke() {
    local name=$1
    shift
    local body
    body=$(mktemp)
    local status
    status=$(curl --connect-timeout 2 --max-time 10 -sS -o "$body" -w "%{http_code}" "$@")
    echo "$name: $status"
    if [[ "$status" != "200" ]]; then
        echo "--- $name response ---"
        cat "$body"
        rm -f "$body"
        return 1
    fi
    rm -f "$body"
}

echo "=== [7/7] 冒烟测试 ==="
smoke "Health" http://127.0.0.1:8081/api/health
smoke "MySQL" http://127.0.0.1:8081/api/mysql
smoke "Redis" http://127.0.0.1:8081/api/redis
smoke "Gateway" http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey \
    -X POST -H "Content-Type: application/json" -d "{}"

echo ""
echo "=== segfault: $(sudo dmesg | grep -c segfault || true) ==="
echo "=== 完成: $(date -u +%FT%TZ) ==="
echo "=== log: $LOG_FILE ==="
