#!/bin/bash
# ============================================================
# asio_owen wrk 压测脚本
# 用法:
#   bash bench/bench.sh              # 跑全部
#   bash bench/bench.sh config       # 只跑 config 直连 + 网关
#   bash bench/bench.sh health       # 只跑 health (GET)
#
# 环境变量:
#   DURATION=30s  CONCURRENCY=100  THREADS=30
#   HOST=127.0.0.1  (VM 地址)
# ============================================================
set -uo pipefail

HOST=${HOST:-127.0.0.1}
DURATION=${DURATION:-30s}
CONCURRENCY=${CONCURRENCY:-100}
THREADS=${THREADS:-30}
TIMEOUT=10s

PASS=0
FAIL=0
RESULTS=()

ok()   { PASS=$((PASS + 1)); }
fail() { FAIL=$((FAIL + 1)); }

run_wrk() {
    local label="$1" url="$2" script="$3"
    echo ""
    echo "=== $label ==="
    local output
    output=$(wrk -t"$THREADS" -c"$CONCURRENCY" -d"$DURATION" --timeout "$TIMEOUT" -s "$script" "$url" 2>&1)
    echo "$output"
    local rps
    rps=$(echo "$output" | grep "Requests/sec:" | awk '{print $2}')
    local avg_lat
    avg_lat=$(echo "$output" | grep "Latency" | head -1 | awk '{print $2}')
    local errors
    errors=$(echo "$output" | grep "Non-2xx" | awk '{print $NF}')
    if [ -z "$errors" ]; then
        RESULTS+=("$label|$rps|$avg_lat|0")
        ok
    else
        RESULTS+=("$label|$rps|$avg_lat|$errors")
        fail
    fi
}

echo "=========================================="
echo "  wrk 压测 - ${DURATION} × ${THREADS}t/${CONCURRENCY}c"
echo "=========================================="

case "${1:-all}" in
    all|config)
        echo ""
        echo "--- 先 curl 确认 ---"
        curl -s -o /dev/null -w "直连: %{http_code}\n" --max-time 5 \
          -H "Content-Type: application/json" \
          -d '{"appid":"member_03150715","config_key":"black_list"}' \
          "http://${HOST}:30001/config.ConfigService/GetByAppAndKey"
        curl -s -o /dev/null -w "网关(无token): %{http_code}\n" --max-time 5 \
          -H "Content-Type: application/json" \
          -d '{"appid":"member_03150715","config_key":"black_list"}' \
          "http://${HOST}:8081/zebra-config/config.ConfigService/GetByAppAndKey"

        run_wrk "Config Direct" \
          "http://${HOST}:30001/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua"
        sleep 5
        run_wrk "Config Gateway" \
          "http://${HOST}:8081/zebra-config/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua"
        ;;
    health)
        run_wrk "Health" "http://${HOST}:8081/api/health" "bench/wrk_get.lua"
        ;;
esac

echo ""
echo "========== 结果 =========="
echo "通过: $PASS, 失败: $FAIL"
for r in "${RESULTS[@]}"; do
    IFS="|" read -r label rps lat err <<< "$r"
    if [ "$err" = "0" ]; then
        echo "  ✅ $label  RPS=$rps  avg_lat=$lat"
    else
        echo "  ❌ $label  RPS=$rps  avg_lat=$lat  errors=$err"
    fi
done
[ "$FAIL" -eq 0 ] && echo "✅ 全部正常" || echo "❌ 有失败"
