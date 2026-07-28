#!/usr/bin/env bash
# ============================================================
# ASan/LSan Gateway RSS 回归检查（在 Ubuntu VM 上执行）
#
# 默认：要求 asio-owen-asan.service 已启动，执行 5 轮 Gateway 压测；每轮
# 30s、100 connections、30 threads，随后冷却 75s，并记录 VmRSS/VmHWM。
# 正常完成后停止 ASan 服务，以触发 LeakSanitizer 的退出检查。
#
# 用法：
#   cd /mnt/mac/Users/mac/code/croot/asio_owen
#   bash run_asan_gateway_rss.sh
#
# 可选环境变量：
#   REBUILD=1          先执行 rebuild_asan.sh（会停止普通服务并重建 ASan）
#   ROUND_COUNT=5      压测轮数
#   DURATION=30s       单轮时长（仅支持 wrk 的秒单位）
#   THREADS=30         wrk 线程数
#   CONNECTIONS=100    wrk 连接数
#   COOLDOWN_SEC=75    每轮后的冷却秒数；应大于 HttpPool idle 回收时间
#   STOP_SERVICE=1     正常完成后停止 ASan 服务，触发 LSan；设为 0 则保留服务
# ============================================================
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SERVICE=${SERVICE:-asio-owen-asan.service}
REBUILD=${REBUILD:-0}
ROUND_COUNT=${ROUND_COUNT:-5}
DURATION=${DURATION:-30s}
THREADS=${THREADS:-30}
CONNECTIONS=${CONNECTIONS:-100}
COOLDOWN_SEC=${COOLDOWN_SEC:-75}
STOP_SERVICE=${STOP_SERVICE:-1}
URL=${URL:-http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey}
WRK_SCRIPT=${WRK_SCRIPT:-"$ROOT/bench/wrk_post.lua"}
LOG_DIR=${LOG_DIR:-"$ROOT/logs"}
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
LOG_FILE="$LOG_DIR/asan_gateway_rss_${STAMP}.log"
SANITIZER_PATTERN='ERROR: (AddressSanitizer|LeakSanitizer)|SUMMARY: (AddressSanitizer|LeakSanitizer)|LeakSanitizer: detected memory leaks'

die() {
    echo "ERROR: $*" >&2
    exit 1
}

require_positive_integer() {
    [[ "$2" =~ ^[1-9][0-9]*$ ]] || die "$1 必须是正整数，当前值：$2"
}

service_pid() {
    systemctl show -p MainPID --value "$SERVICE"
}

rss() {
    local pid=$1
    [[ -r "/proc/$pid/status" ]] || die "无法读取 /proc/$pid/status；服务 PID=$pid 可能已退出"
    awk '
        /^VmRSS:|^VmHWM:/ {
            sub(":", "", $1)
            printf "%s=%s%s ", $1, $2, $3
        }
        END { print "" }
    ' "/proc/$pid/status"
}

print_sanitizer_report() {
    echo "=== sanitizer report: journal (${SERVICE}) ==="
    sudo journalctl -u "$SERVICE" -n 500 --no-pager \
        | grep -E -A2 "$SANITIZER_PATTERN" || true
    echo "=== sanitizer report: /tmp/asan_stderr.log ==="
    grep -E -A2 "$SANITIZER_PATTERN" \
        /tmp/asan_stderr.log 2>/dev/null || true
}

print_copy_summary() {
    echo ""
    echo "============================================================"
    echo "=== COPY THIS BLOCK TO CODEX ==="
    echo "log=$LOG_FILE"
    echo "service=$SERVICE rounds=$ROUND_COUNT duration=$DURATION threads=$THREADS connections=$CONNECTIONS cooldown_sec=$COOLDOWN_SEC"
    echo "url=$URL"
    echo "--- RSS and wrk ---"
    grep -E '^=== round [0-9]+/|^=== round [0-9]+ after cooldown|^pid=.* rss=|^Requests/sec:|^Socket errors:|^ *[0-9]+ non-2xx' \
        "$LOG_FILE" || true
    echo "--- sanitizer findings ---"
    if ! grep -E "$SANITIZER_PATTERN" "$LOG_FILE"; then
        echo "no AddressSanitizer/LeakSanitizer ERROR or SUMMARY line found in captured output"
    fi
    echo "=== END COPY BLOCK ==="
    echo "============================================================"
}

require_positive_integer ROUND_COUNT "$ROUND_COUNT"
require_positive_integer THREADS "$THREADS"
require_positive_integer CONNECTIONS "$CONNECTIONS"
require_positive_integer COOLDOWN_SEC "$COOLDOWN_SEC"
[[ "$DURATION" =~ ^[1-9][0-9]*s$ ]] || die "DURATION 仅支持正整数秒，例如 30s"
[[ "$STOP_SERVICE" == "0" || "$STOP_SERVICE" == "1" ]] || die "STOP_SERVICE 只能是 0 或 1"
command -v wrk >/dev/null 2>&1 || die "未找到 wrk"
[[ -f "$WRK_SCRIPT" ]] || die "未找到 wrk 脚本：$WRK_SCRIPT"

mkdir -p "$LOG_DIR"
exec > >(tee -a "$LOG_FILE") 2>&1

echo "=== ASan Gateway RSS check started: $(date -u '+%FT%TZ') ==="
echo "log=$LOG_FILE"
echo "service=$SERVICE rounds=$ROUND_COUNT duration=$DURATION threads=$THREADS connections=$CONNECTIONS cooldown_sec=$COOLDOWN_SEC"
echo "url=$URL"

if [[ "$REBUILD" == "1" ]]; then
    echo "=== rebuilding and starting ASan service ==="
    bash "$ROOT/rebuild_asan.sh"
fi

[[ "$(systemctl is-active "$SERVICE" 2>/dev/null || true)" == "active" ]] \
    || die "$SERVICE 未启动；先运行 bash rebuild_asan.sh，或设置 REBUILD=1"

PID=$(service_pid)
[[ "$PID" =~ ^[1-9][0-9]*$ ]] || die "无法获取 $SERVICE 的 MainPID：$PID"

echo "=== preflight ==="
echo "pid=$PID rss=$(rss "$PID")"
curl --fail --silent --show-error --max-time 10 \
    -X POST -H 'Content-Type: application/json' \
    --data '{"appid":"member_03150715","config_key":"black_list"}' \
    "$URL" >/dev/null
echo "gateway smoke=200"

for ((round = 1; round <= ROUND_COUNT; round++)); do
    PID=$(service_pid)
    [[ "$PID" =~ ^[1-9][0-9]*$ ]] || die "$SERVICE 在第 $round 轮前已退出"

    echo "=== round $round/$ROUND_COUNT before: $(date -u '+%FT%TZ') ==="
    echo "pid=$PID rss=$(rss "$PID")"
    wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" --timeout 10s \
        -s "$WRK_SCRIPT" "$URL"

    echo "=== round $round cooldown: ${COOLDOWN_SEC}s ==="
    sleep "$COOLDOWN_SEC"
    PID=$(service_pid)
    [[ "$PID" =~ ^[1-9][0-9]*$ ]] || die "$SERVICE 在第 $round 轮冷却期间已退出"
    echo "=== round $round after cooldown: $(date -u '+%FT%TZ') ==="
    echo "pid=$PID rss=$(rss "$PID")"
done

print_sanitizer_report

if [[ "$STOP_SERVICE" == "1" ]]; then
    echo "=== stopping $SERVICE to trigger LeakSanitizer: $(date -u '+%FT%TZ') ==="
    sudo systemctl stop "$SERVICE"
    sleep 2
    print_sanitizer_report
else
    echo "=== STOP_SERVICE=0; ASan service remains active ==="
fi

echo "=== completed: $(date -u '+%FT%TZ') ==="
echo "=== result log: $LOG_FILE ==="
print_copy_summary
