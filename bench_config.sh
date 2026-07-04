#!/bin/bash
# 专门验证 Config 接口（直连 + 网关）的 plow 压测
# 每步检查：curl 先确认正常 → plow 单次 → 确认 2xx 后才继续
# 用法: bash bench_config.sh

set -uo pipefail

PLOW=/root/go/bin/plow
BASE=http://127.0.0.1:8081
UPSTREAM=http://127.0.0.1:30001
SERVER_LOG=/mnt/mac/Users/mac/code/croot/asio_owen/build/server.log
BODY='{"appid":"member_03150715","config_key":"black_list"}'
BUILD_DIR=/mnt/mac/Users/mac/code/croot/asio_owen/build

# 注意！！！zebra-config 的二进制名也是 server，不能用 pgrep -x server 杀
# 只杀从 build 目录启动的 server
kill_our_server() {
    for pid in $(pgrep -x server 2>/dev/null || true); do
        exe=$(readlink -f /proc/$pid/exe 2>/dev/null || echo "")
        if echo "$exe" | grep -q "build/server$" 2>/dev/null; then
            kill -9 $pid 2>/dev/null || true
            echo "  killed our server (PID=$pid)"
        fi
    done
    sleep 1
}

# JWT token (RS256, issued by pixiu-gateway, expires ~2026-07-05)
TOKEN="eyJhbGciOiJSUzI1NiIsImtpZCI6InBpeGl1LWp3dC1rZXktMSIsInR5cCI6IkpXVCJ9.eyJhdWQiOiJwaXhpdS1hcGkiLCJjbGllbnRfdHlwZSI6ImFkbWluIiwiZGV2aWNlX2lkIjoiN2FmZjAzNTctMjQzNS00NjcxLThlNTMtOTM3MzY3MWUwZTEwIiwiZXhwIjoxNzgzNDIwMTU0LCJpYXQiOjE3ODMxNjA5NTQsImlzcyI6InBpeGl1LWdhdGV3YXkiLCJqdGkiOiIxNzgzMTYwOTU0ODQ5Mjc3NDc3MDkiLCJuYW1lIjoiYWRtaW4iLCJuYmYiOjE3ODMxNjA5NTQsInN1YiI6IjEiLCJ0eXBlIjoiYWNjZXNzIiwidXNlcl9pZCI6MSwidXNlcl9uYW1lIjoiYWRtaW4ifQ.OBgz_LmThzMgOvZ6Mr9xdkv4II15Jmd-QwDJwgK_s6zyAHFmIOnFhvus0g_ThwJXdXiKYWN6dpwZAj_DZjTBoDgC_MWLN1ksydmkR9Ta6ySHp-Y1CdWcmKe2qlae3bQg6Ji19o3ZzJYlpUrcAvKh6EEwLGbOCzCSLxl_ZmfxrWCQKtalUagkOEzINDB9jW7d_n09yg2tfRLEm8pzpSaxtH4dpQHdvNvdn92qt6XWwsOFJlffoLfWukAJvz2DsphfjiFZegk3hBemIq5RrXYKlp0E5pkm8BDY_usL84MCAYYM56o3SWkD5EqWthIuqJZ7vAgSZe_C-QHEo8eQOxAkIw"

step=0
pass=0
fail=0

check() {
    step=$((step + 1))
    echo ""
    echo "=== Step $step: $1 ==="
}

ok() {
    echo "  ✅ $1"
    pass=$((pass + 1))
}

not_ok() {
    echo "  ❌ $1"
    fail=$((fail + 1))
    exit 1
}

# 启动我们的 server
start_server() {
    kill_our_server
    rm -f "$SERVER_LOG"
    cd "$BUILD_DIR"
    ./server > /dev/null 2>&1 &
    sleep 4
    if curl -sf --max-time 3 "$BASE/api/health" > /dev/null 2>&1; then
        echo "  server started OK"
    else
        not_ok "server start failed"
    fi
}

# ============================================================
# Step 1: curl 直连
# ============================================================
check "curl direct upstream"
DIRECT_CODE=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d "$BODY" \
  "$UPSTREAM/config.ConfigService/GetByAppAndKey")
if [ "$DIRECT_CODE" = "200" ]; then
    ok "curl direct: 200"
else
    not_ok "curl direct: $DIRECT_CODE (expected 200)"
fi

# ============================================================
# Step 2: curl 网关
# ============================================================
check "curl via gateway"
start_server
GATEWAY_CODE=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 \
  -H "Content-Type: application/json" \
  -d "$BODY" \
  "$BASE/zebra-config/config.ConfigService/GetByAppAndKey")
if [ "$GATEWAY_CODE" = "200" ]; then
    ok "curl gateway: 200"
else
    not_ok "curl gateway: $GATEWAY_CODE (expected 200)"
fi

# ============================================================
# Step 3: plow 直连 5s
# ============================================================
check "plow direct 5s"
OUTPUT_DIRECT=$("$PLOW" -c 100 -d 5s -m POST \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -b "$BODY" \
  "$UPSTREAM/config.ConfigService/GetByAppAndKey" 2>&1)

LAST_DIRECT=$(echo "$OUTPUT_DIRECT" | grep -n "^Summary:" | tail -1 | cut -d: -f1)
echo "$OUTPUT_DIRECT" | tail -n +$LAST_DIRECT | head -5

COUNT_TOTAL=$(echo "$OUTPUT_DIRECT" | tail -n +$LAST_DIRECT | grep "^  Count" | awk '{print $2}')
COUNT_2XX=$(echo "$OUTPUT_DIRECT" | tail -n +$LAST_DIRECT | grep "^    2xx" | awk '{print $2}')
RPS=$(echo "$OUTPUT_DIRECT" | tail -n +$LAST_DIRECT | grep "^  RPS" | head -1 | awk '{print $2}')

if [ "$COUNT_TOTAL" = "$COUNT_2XX" ]; then
    ok "plow direct: $COUNT_TOTAL requests, RPS=$RPS, all 2xx"
else
    not_ok "plow direct: total=$COUNT_TOTAL, 2xx=$COUNT_2XX"
fi

# ============================================================
# Step 4: plow 网关 5s
# ============================================================
check "plow gateway 5s"
start_server

OUTPUT_GATEWAY=$("$PLOW" -c 100 -d 5s -m POST \
  -H "Content-Type: application/json" \
  -b "$BODY" \
  "$BASE/zebra-config/config.ConfigService/GetByAppAndKey" 2>&1)

LAST_GATEWAY=$(echo "$OUTPUT_GATEWAY" | grep -n "^Summary:" | tail -1 | cut -d: -f1)
echo "$OUTPUT_GATEWAY" | tail -n +$LAST_GATEWAY | head -5

COUNT_TOTAL=$(echo "$OUTPUT_GATEWAY" | tail -n +$LAST_GATEWAY | grep "^  Count" | awk '{print $2}')
COUNT_2XX=$(echo "$OUTPUT_GATEWAY" | tail -n +$LAST_GATEWAY | grep "^    2xx" | awk '{print $2}')
RPS=$(echo "$OUTPUT_GATEWAY" | tail -n +$LAST_GATEWAY | grep "^  RPS" | head -1 | awk '{print $2}')

if [ "$COUNT_TOTAL" = "$COUNT_2XX" ]; then
    ok "plow gateway: $COUNT_TOTAL requests, RPS=$RPS, all 2xx"
else
    not_ok "plow gateway: total=$COUNT_TOTAL, 2xx=$COUNT_2XX"
fi

# ============================================================
echo ""
echo "========== Config 验证结果 =========="
echo "通过: $pass, 失败: $fail"
if [ "$fail" -eq 0 ]; then
    echo "✅ 全部正常"
else
    echo "❌ 有失败"
    exit 1
fi
