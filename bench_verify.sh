#!/bin/bash
# 逐个验证每个接口的 plow 压测是否正常
# 每步检查：全部 2xx？server log 无异常？
# 全部通过后，再跑正式长压

set -euo pipefail

PLOW=/root/go/bin/plow
BASE=http://127.0.0.1:8081
UPSTREAM=http://127.0.0.1:30001
SERVER_LOG=/mnt/mac/Users/mac/code/croot/asio_owen/build/server.log
BODY='{"appid":"member_03150715","config_key":"black_list"}'
PASS=0
FAIL=0

check_log() {
    local returned_0 error_resp
    returned_0=$(grep -c "returned 0" "$SERVER_LOG" 2>/dev/null || echo 0)
    error_resp=$(grep -c "error response" "$SERVER_LOG" 2>/dev/null || echo 0)
    if [ "$returned_0" -gt 0 ] || [ "$error_resp" -gt 0 ]; then
        echo "  ❌ LOG WARNING: returned_0=$returned_0, error_response=$error_resp"
        return 1
    fi
    echo "  ✅ LOG: clean"
    return 0
}

check_plow() {
    local label="$1"
    local output="$2"

    local count_2xx count_total
    count_total=$(echo "$output" | grep "^  Count" | tail -1 | awk '{print $2}')
    count_2xx=$(echo "$output" | grep "^    2xx" | tail -1 | awk '{print $2}')

    if [ "$count_total" = "" ] || [ "$count_total" = "0" ]; then
        echo "  ❌ PLOW: no data"
        return 1
    fi

    if [ "$count_total" != "$count_2xx" ]; then
        local count_4xx count_5xx
        count_4xx=$(echo "$output" | grep "^    4xx" | tail -1 | awk '{print $2}')
        count_5xx=$(echo "$output" | grep "^    5xx" | tail -1 | awk '{print $2}')
        echo "  ❌ PLOW: 2xx=$count_2xx, total=$count_total, 4xx=$count_4xx, 5xx=$count_5xx"
        return 1
    fi

    echo "  ✅ PLOW: $count_total requests, all 2xx"
    return 0
}

run_one() {
    local label="$1"
    local url="$2"
    shift 2
    local extra=("$@")

    echo ""
    echo "=== $label ==="
    local output
    output=$("$PLOW" -c 100 -d 5s "$url" "${extra[@]}" 2>&1)
    echo "$output" | grep -E "^Summary:|^  Count|^    [0-9]|^  RPS" | tail -6

    if check_plow "$label" "$output" && check_log; then
        echo "  ✅ $label PASS"
        PASS=$((PASS + 1))
    else
        echo "  ❌ $label FAIL"
        FAIL=$((FAIL + 1))
    fi
}

# 清理旧日志
rm -f "$SERVER_LOG"
pgrep -x server | xargs kill -9 2>/dev/null || true
sleep 2

cd /mnt/mac/Users/mac/code/croot/asio_owen/build
./server > /dev/null 2>&1 &
sleep 3

# 确认服务存活
curl -sf --max-time 3 "$BASE/api/health" > /dev/null || { echo "Server not running"; exit 1; }

# === 逐个验证 ===
run_one "Health" "$BASE/api/health"
run_one "Redis" "$BASE/api/redis"
run_one "MySQL" "$BASE/api/mysql"
run_one "Config Direct" "$UPSTREAM/config.ConfigService/GetByAppAndKey" \
    -m POST -H "Content-Type: application/json" -b "$BODY"
run_one "Config Via Gateway" "$BASE/zebra-config/config.ConfigService/GetByAppAndKey" \
    -m POST -H "Content-Type: application/json" -b "$BODY"

# === 汇总 ===
echo ""
echo "========== 结果 =========="
echo "通过: $PASS，失败: $FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "✅ 全部通过，可以跑正式压测"
else
    echo "❌ 有失败，请检查后再试"
    exit 1
fi
