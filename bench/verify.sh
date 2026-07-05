#!/bin/bash
# 单次验证：curl 确认接口正常，再用 wrk 小批量压 5s
# 用法: bash bench/verify.sh [config|health|all]

set -uo pipefail

HOST=${HOST:-192.168.139.230}
DURATION=5s
CONCURRENCY=50
THREADS=4

PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); echo "  ✅ $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  ❌ $1"; }

verify() {
    local label="$1" url="$2" script="$3"
    echo ""
    echo "=== $label ==="

    # 1. curl 单次确认
    local code
    code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 \
      -H "Content-Type: application/json" \
      -d '{"appid":"member_03150715","config_key":"black_list"}' \
      "$url" 2>/dev/null)
    if [ "$code" != "200" ]; then
        fail "curl $url => HTTP $code (expected 200)"
        return
    fi
    ok "curl 200"

    # 2. wrk 小批量 5s
    local output
    output=$(wrk -t"$THREADS" -c"$CONCURRENCY" -d"$DURATION" --timeout 10s -s "$script" "$url" 2>&1)
    local rps
    rps=$(echo "$output" | grep "Requests/sec:" | awk '{print $2}')
    local avg_lat
    avg_lat=$(echo "$output" | grep "Latency" | head -1 | awk '{print $2}')
    local errors
    errors=$(echo "$output" | grep "Non-2xx" | awk '{print $NF}')
    if [ -z "$errors" ]; then
        ok "wrk 5s: RPS=$rps avg_lat=$avg_lat all 2xx"
    else
        fail "wrk 5s: RPS=$rps errors=$errors"
    fi
}

case "${1:-all}" in
    config)
        verify "Config Direct" \
          "http://${HOST}:30001/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua"
        verify "Config Gateway" \
          "http://${HOST}:8081/zebra-config/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua"
        ;;
    health)
        verify "Health" "http://${HOST}:8081/api/health" "bench/wrk_get.lua"
        ;;
    all)
        verify "Health" "http://${HOST}:8081/api/health" "bench/wrk_get.lua"
        verify "Config Direct" \
          "http://${HOST}:30001/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua"
        verify "Config Gateway" \
          "http://${HOST}:8081/zebra-config/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua"
        ;;
esac

echo ""
echo "========== 结果 =========="
echo "通过: $PASS, 失败: $FAIL"
[ "$FAIL" -eq 0 ] && echo "✅ 全部正常" || echo "❌ 有失败"
