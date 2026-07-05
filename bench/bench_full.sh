#!/bin/bash
# ============================================================
# asio_owen wrk 压测 - 多接口 30s × 2 轮
# 用法:
#   bash bench/bench_full.sh              # 跑全部
#   bash bench/bench_full.sh health       # 只跑 health
#   bash bench/bench_full.sh redis        # 只跑 redis
#   bash bench/bench_full.sh mysql        # 只跑 mysql
#   bash bench/bench_full.sh config       # 只跑 config
#
# 环境变量:
#   DURATION=30s  CONCURRENCY=100  THREADS=10  COOLDOWN=10
#   HOST=192.168.139.230
# ============================================================
set -uo pipefail

HOST=${HOST:-192.168.139.230}
DURATION=${DURATION:-30s}
CONCURRENCY=${CONCURRENCY:-100}
THREADS=${THREADS:-10}
TIMEOUT=10s
COOLDOWN=${COOLDOWN:-10}
ROUNDS=2

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

curl_check() {
    local label="$1" url="$2" method="${3:-GET}" body="${4:-}"
    local code
    if [ "$method" = "POST" ]; then
        code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 -X POST \
          -H "Content-Type: application/json" \
          -d "$body" "$url" 2>/dev/null)
    else
        code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 "$url" 2>/dev/null)
    fi
    echo "  $label => HTTP $code"
    [ "$code" = "200" ]
}

echo "=========================================="
echo "  wrk 压测 - ${ROUNDS}轮 × ${DURATION}"
echo "  ${THREADS}t/${CONCURRENCY}c  暂停${COOLDOWN}s"
echo "=========================================="

run_rounds() {
    local name="$1" url="$2" script="$3" method="${4:-GET}" body="${5:-}"
    echo ""
    echo "--- $name ---"
    # curl 确认
    curl_check "$name" "$url" "$method" "$body" || { fail; echo "  ❌ curl失败，跳过"; return; }
    for i in $(seq 1 $ROUNDS); do
        run_wrk "$name #$i" "$url" "$script"
        [ "$i" -lt "$ROUNDS" ] && sleep "$COOLDOWN"
    done
}

case "${1:-all}" in
    health)
        run_rounds "Health" "http://${HOST}:8081/api/health" "bench/wrk_get.lua"
        ;;
    redis)
        run_rounds "Redis" "http://${HOST}:8081/api/redis" "bench/wrk_get.lua"
        ;;
    mysql)
        run_rounds "MySQL" "http://${HOST}:8081/api/mysql" "bench/wrk_get.lua"
        ;;
    config)
        local BODY='{"appid":"member_03150715","config_key":"black_list"}'
        run_rounds "Config Direct" \
          "http://${HOST}:30001/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua" POST "$BODY"
        run_rounds "Config Gateway" \
          "http://${HOST}:8081/zebra-config/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua" POST "$BODY"
        ;;
    all)
        # Health → 暂停 → Redis → 暂停 → MySQL → 暂停 → Config
        local BODY='{"appid":"member_03150715","config_key":"black_list"}'

        run_rounds "Health" "http://${HOST}:8081/api/health" "bench/wrk_get.lua"
        echo ""; echo "--- 暂停 ${COOLDOWN}s ---"; sleep "$COOLDOWN"

        run_rounds "Redis" "http://${HOST}:8081/api/redis" "bench/wrk_get.lua"
        echo ""; echo "--- 暂停 ${COOLDOWN}s ---"; sleep "$COOLDOWN"

        run_rounds "MySQL" "http://${HOST}:8081/api/mysql" "bench/wrk_get.lua"
        echo ""; echo "--- 暂停 ${COOLDOWN}s ---"; sleep "$COOLDOWN"

        run_rounds "Config Direct" \
          "http://${HOST}:30001/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua" POST "$BODY"
        echo ""; echo "--- 暂停 ${COOLDOWN}s ---"; sleep "$COOLDOWN"

        run_rounds "Config Gateway" \
          "http://${HOST}:8081/zebra-config/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua" POST "$BODY"
        ;;
esac

echo ""
echo "========== 汇总 =========="
echo "通过: $PASS, 失败: $FAIL"
echo ""
printf "%-20s %10s %12s %10s\n" "接口" "RPS" "avg_lat" "errors"
printf "%-20s %10s %12s %10s\n" "--------------------" "----------" "------------" "----------"
for r in "${RESULTS[@]}"; do
    IFS="|" read -r label rps lat err <<< "$r"
    if [ "$err" = "0" ]; then
        printf "%-20s %10s %12s %10s\n" "$label" "$rps" "$lat" "0"
    else
        printf "%-20s %10s %12s %10s\n" "$label" "$rps" "$lat" "$err"
    fi
done
echo ""
[ "$FAIL" -eq 0 ] && echo "✅ 全部正常" || echo "❌ 有失败"
