#!/bin/bash
# ============================================================
# asio_owen 压测脚本
# 用法:
#   bash bench.sh              # 跑全部 5 个接口（默认 30s × 2，暂停 10s）
#   bash bench.sh health       # 只跑 health
#   bash bench.sh redis        # 只跑 redis
#   bash bench.sh mysql        # 只跑 mysql
#   bash bench.sh config       # 跑 config 直连 + 网关
#   bash bench.sh all          # 同默认
#
# 环境变量:
#   DURATION=30s  每轮时长
#   ROUNDS=2      每接口轮数
#   COOLDOWN=10   轮间暂停秒数
#   CONCURRENCY=100 并发数
# ============================================================
set -uo pipefail

PLOW=/root/go/bin/plow
BASE=http://127.0.0.1:8081
UPSTREAM=http://127.0.0.1:30001
SERVER_LOG=/mnt/mac/Users/mac/code/croot/asio_owen/build/server.log
BODY='{"appid":"member_03150715","config_key":"black_list"}'
TOKEN='eyJhbGciOiJSUzI1NiIsImtpZCI6InBpeGl1LWp3dC1rZXktMSIsInR5cCI6IkpXVCJ9.eyJhdWQiOiJwaXhpdS1hcGkiLCJjbGllbnRfdHlwZSI6ImFkbWluIiwiZGV2aWNlX2lkIjoiN2FmZjAzNTctMjQzNS00NjcxLThlNTMtOTM3MzY3MWUwZTEwIiwiZXhwIjoxNzgzNDIwMTU0LCJpYXQiOjE3ODMxNjA5NTQsImlzcyI6InBpeGl1LWdhdGV3YXkiLCJqdGkiOiIxNzgzMTYwOTU0ODQ5Mjc3NDc3MDkiLCJuYW1lIjoiYWRtaW4iLCJuYmYiOjE3ODMxNjA5NTQsInN1YiI6IjEiLCJ0eXBlIjoiYWNjZXNzIiwidXNlcl9pZCI6MSwidXNlcl9uYW1lIjoiYWRtaW4ifQ.OBgz_LmThzMgOvZ6Mr9xdkv4II15Jmd-QwDJwgK_s6zyAHFmIOnFhvus0g_ThwJXdXiKYWN6dpwZAj_DZjTBoDgC_MWLN1ksydmkR9Ta6ySHp-Y1CdWcmKe2qlae3bQg6Ji19o3ZzJYlpUrcAvKh6EEwLGbOCzCSLxl_ZmfxrWCQKtalUagkOEzINDB9jW7d_n09yg2tfRLEm8pzpSaxtH4dpQHdvNvdn92qt6XWwsOFJlffoLfWukAJvz2DsphfjiFZegk3hBemIq5RrXYKlp0E5pkm8BDY_usL84MCAYYM56o3SWkD5EqWthIuqJZ7vAgSZe_C-QHEo8eQOxAkIw'

DURATION=${DURATION:-30s}
ROUNDS=${ROUNDS:-2}
COOLDOWN=${COOLDOWN:-10}
CONCURRENCY=${CONCURRENCY:-100}

PASS=0
FAIL=0

ok()   { echo "  ✅ $1"; PASS=$((PASS + 1)); }
fail() { echo "  ❌ $1"; FAIL=$((FAIL + 1)); }

run_plow() {
    local label="$1"
    local url="$2"
    shift 2
    local extra=("$@")

    echo ""
    echo "=== $label ==="
    local output
    output=$("$PLOW" -c "$CONCURRENCY" -d "$DURATION" "$url" "${extra[@]}" 2>&1)

    local last_line
    last_line=$(echo "$output" | grep -n "^Summary:" | tail -1 | cut -d: -f1)
    echo "$output" | tail -n +$last_line | head -6

    local count_total count_2xx count_4xx count_5xx
    count_total=$(echo "$output" | tail -n +$last_line | grep "^  Count" | awk '{print $2}')
    count_2xx=$(echo "$output" | tail -n +$last_line | grep "^    2xx" | awk '{print $2}')
    count_4xx=$(echo "$output" | tail -n +$last_line | grep "^    4xx" | awk '{print $2}')
    count_5xx=$(echo "$output" | tail -n +$last_line | grep "^    5xx" | awk '{print $2}')

    if [ "$count_total" = "$count_2xx" ]; then
        ok "$label: $count_total requests, all 2xx"
    else
        fail "$label: total=$count_total, 2xx=$count_2xx, 4xx=${count_4xx:-0}, 5xx=${count_5xx:-0}"
    fi
}

check_server() {
    if ! curl -sf --max-time 3 "$BASE/api/health" > /dev/null 2>&1; then
        echo "❌ Server not running"
        # 尝试启动
        for pid in $(pgrep -x server 2>/dev/null || true); do
            exe=$(readlink -f /proc/$pid/exe 2>/dev/null || echo "")
            if echo "$exe" | grep -q "build/server$" 2>/dev/null; then
                echo "Server already running (PID=$pid)"
                return 0
            fi
        done
        echo "Starting server..."
        cd /mnt/mac/Users/mac/code/croot/asio_owen/build
        rm -f server.log
        ./server > /dev/null 2>&1 &
        sleep 4
        curl -sf --max-time 3 "$BASE/api/health" > /dev/null || { echo "❌ Server start failed"; exit 1; }
    fi
}

bench_health() {
    check_server
    for i in $(seq 1 $ROUNDS); do
        run_plow "Health #$i" "$BASE/api/health"
        [ "$i" -lt "$ROUNDS" ] && sleep "$COOLDOWN"
    done
}

bench_redis() {
    check_server
    for i in $(seq 1 $ROUNDS); do
        run_plow "Redis #$i" "$BASE/api/redis"
        [ "$i" -lt "$ROUNDS" ] && sleep "$COOLDOWN"
    done
}

bench_mysql() {
    check_server
    for i in $(seq 1 $ROUNDS); do
        run_plow "MySQL #$i" "$BASE/api/mysql"
        [ "$i" -lt "$ROUNDS" ] && sleep "$COOLDOWN"
    done
}

bench_config() {
    check_server
    for i in $(seq 1 $ROUNDS); do
        run_plow "Config Direct #$i" "$UPSTREAM/config.ConfigService/GetByAppAndKey" \
            -m POST -H "Content-Type: application/json" -b "$BODY"
        [ "$i" -lt "$ROUNDS" ] && sleep "$COOLDOWN"
    done
    for i in $(seq 1 $ROUNDS); do
        run_plow "Config Gateway #$i" "$BASE/zebra-config/config.ConfigService/GetByAppAndKey" \
            -m POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN" -b "$BODY"
        [ "$i" -lt "$ROUNDS" ] && sleep "$COOLDOWN"
    done
}

bench_all() {
    bench_health
    sleep "$COOLDOWN"
    bench_redis
    sleep "$COOLDOWN"
    bench_mysql
    sleep "$COOLDOWN"
    bench_config
}

# ---- main ----
case "${1:-all}" in
    health) bench_health ;;
    redis)  bench_redis ;;
    mysql)  bench_mysql ;;
    config) bench_config ;;
    all|*)  bench_all ;;
esac

echo ""
echo "========== 结果 =========="
echo "通过: $PASS, 失败: $FAIL"
[ "$FAIL" -eq 0 ] && echo "✅ 全部正常" || echo "❌ 有失败"
