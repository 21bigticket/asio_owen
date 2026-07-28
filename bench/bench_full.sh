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
#   DURATION=30s  CONCURRENCY=100  THREADS=30  COOLDOWN=10
#   HOST=127.0.0.1
#   PROFILE=1 PROFILE_DIR=logs/bench    # 保留 pidstat/mpstat/wrk 原始结果
#   PERF=1 PERF_FREQ=49                 # 额外采集 perf (需 root 或免密 sudo)
#   PERF_EVENT=cpu-clock                # 默认软件事件，兼容虚拟机
# ============================================================
set -uo pipefail

# 确保退出时清理监控进程
cleanup() {
    stop_monitor
    rm -f /tmp/bench_monitor_*_$$.log 2>/dev/null || true
}
trap cleanup EXIT INT TERM

HOST=${HOST:-127.0.0.1}
DURATION=${DURATION:-30s}
CONCURRENCY=${CONCURRENCY:-100}
THREADS=${THREADS:-30}
TIMEOUT=10s
COOLDOWN=${COOLDOWN:-10}
ROUNDS=2
PROFILE=${PROFILE:-0}
PROFILE_DIR=${PROFILE_DIR:-logs/bench}
PERF=${PERF:-0}
PERF_FREQ=${PERF_FREQ:-49}
PERF_EVENT=${PERF_EVENT:-cpu-clock}
PROFILE_OUTPUT_DIR=""
PROFILE_SUMMARY=""

PASS=0
FAIL=0
RESULTS=()

ok()   { PASS=$((PASS + 1)); }
fail() { FAIL=$((FAIL + 1)); }

# 系统监控相关
MONITOR_PID=""
MONITOR_LOG=""
MONITOR_PIDS=()
PERF_PID=""
PERF_DATA=""
PERF_ERROR=""

server_pid() {
    local pid
    pid=$(ss -ltnpH 'sport = :8081' 2>/dev/null | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | head -1)
    if [ -n "$pid" ]; then
        echo "$pid"
        return
    fi

    pgrep -x asio_owen 2>/dev/null | head -1 || pgrep -x server 2>/dev/null | head -1
}

safe_label() {
    echo "$1" | tr ' ' '_' | tr -cd '[:alnum:]_.-'
}

duration_seconds() {
    case "$DURATION" in
        *s) echo "${DURATION%s}" ;;
        *) return 1 ;;
    esac
}

init_profile() {
    [ "$PROFILE" = "1" ] || return

    local stamp
    stamp=$(date -u '+%Y%m%dT%H%M%SZ')
    PROFILE_OUTPUT_DIR="${PROFILE_DIR%/}/${stamp}_$$"
    mkdir -p "$PROFILE_OUTPUT_DIR"
    PROFILE_SUMMARY="$PROFILE_OUTPUT_DIR/summary.txt"
    {
        echo "benchmark_started=$(ts)"
        echo "host=$HOST duration=$DURATION threads=$THREADS concurrency=$CONCURRENCY cooldown=$COOLDOWN"
        echo "profile_dir=$PROFILE_OUTPUT_DIR"
        echo "perf_requested=$PERF perf_freq=$PERF_FREQ perf_event=$PERF_EVENT"
    } > "$PROFILE_SUMMARY"
}

start_perf() {
    local label="$1" target_pid="$2"
    PERF_PID=""
    PERF_DATA=""
    PERF_ERROR=""

    [ "$PROFILE" = "1" ] && [ "$PERF" = "1" ] || return
    if ! command -v perf >/dev/null 2>&1; then
        echo "  perf 未安装，跳过 perf 采样" >&2
        return
    fi

    local seconds
    if ! seconds=$(duration_seconds); then
        echo "  DURATION=$DURATION 不是秒单位，跳过 perf 采样" >&2
        return
    fi

    local -a perf_cmd=(perf)
    if [ "${EUID:-$(id -u)}" -ne 0 ]; then
        if sudo -n true 2>/dev/null; then
            perf_cmd=(sudo -n perf)
        else
            echo "  perf 需要 root 或免密 sudo，跳过 perf 采样" >&2
            return
        fi
    fi

    local basename
    basename=$(safe_label "$label")
    PERF_DATA="$PROFILE_OUTPUT_DIR/${basename}.perf.data"
    PERF_ERROR="$PROFILE_OUTPUT_DIR/${basename}.perf.stderr.log"
    "${perf_cmd[@]}" record -e "$PERF_EVENT" -F "$PERF_FREQ" -g -p "$target_pid" -o "$PERF_DATA" -- sleep "$seconds" \
        2>"$PERF_ERROR" &
    PERF_PID=$!
}

stop_perf() {
    [ -n "$PERF_PID" ] || return
    wait "$PERF_PID" 2>/dev/null || true
    PERF_PID=""

    [ -s "$PERF_DATA" ] || return
    local report="${PERF_DATA%.data}.txt"
    if [ "${EUID:-$(id -u)}" -eq 0 ]; then
        perf report --stdio -i "$PERF_DATA" > "$report" 2>&1 || true
    elif sudo -n true 2>/dev/null; then
        sudo -n perf report --stdio -i "$PERF_DATA" > "$report" 2>&1 || true
    fi
}

start_monitor() {
    local label="$1"
    MONITOR_LOG="/tmp/bench_monitor_${label// /_}_$$.log"
    MONITOR_PIDS=()

    # 找到服务进程 PID: 优先找 asio_owen,否则找监听 8081 端口的 server 进程
    local target_pid
    target_pid=$(server_pid)
    if [ -z "$target_pid" ]; then
        echo "⚠️  未找到服务进程(asio_owen/server),跳过监控" >&2
        return
    fi
    if [ "$PROFILE" = "1" ]; then
        echo "[$label] server_pid=$target_pid" >> "$PROFILE_SUMMARY"
    fi

    # 获取系统核心数
    local ncpu=$(nproc)

    # 后台循环采集进程级指标: CPU/内存/上下文切换/磁盘IO
    (
        local tick=0
        while true; do
            # 每 2 秒采集一次(降低输出密度)
            if [ $((tick % 2)) -eq 0 ]; then
                # pidstat -u: CPU, -r: 内存, -w: 上下文切换, -d: 磁盘IO
                # -h: 单行输出, -p: 指定进程
                pidstat -u -r -w -d -h -p "$target_pid" 1 1 | awk -v ts="$(date +%H:%M:%S)" -v ncpu="$ncpu" '
                    /^#/ { next }  # 跳过注释行
                    NF >= 10 {
                        # 格式: Time UID PID %usr %system %guest %wait %CPU CPU minflt/s majflt/s VSZ RSS %MEM kB_rd/s kB_wr/s kB_ccwr/s iodelay Command
                        printf "%s cpu_total=%s%% cpu_per_core=%s%% wait=%s%% ncpu=%s mem=%sMB minflt=%s majflt=%s rd=%skB/s wr=%skB/s\n",
                            ts,
                            $8,                # %CPU (多核累加)
                            int($8/ncpu + 0.5), # 单核平均 CPU%
                            $7,                # %wait
                            ncpu,
                            int($13/1024),     # RSS 转 MB
                            $10,               # minflt/s
                            $11,               # majflt/s
                            $16,               # kB_rd/s
                            $17                # kB_wr/s
                    }
                '
            fi
            tick=$((tick + 1))
            sleep 1
        done > "$MONITOR_LOG" 2>&1
    ) &
    MONITOR_PID=$!
    MONITOR_PIDS+=("$MONITOR_PID")

    [ "$PROFILE" = "1" ] || return
    local basename
    basename=$(safe_label "$label")
    if command -v pidstat >/dev/null 2>&1; then
        pidstat -t -u -r -w -d -h -p "$target_pid" 1 > "$PROFILE_OUTPUT_DIR/${basename}.pidstat.log" 2>&1 &
        MONITOR_PIDS+=("$!")
    else
        echo "pidstat unavailable" > "$PROFILE_OUTPUT_DIR/${basename}.pidstat.log"
    fi
    if command -v mpstat >/dev/null 2>&1; then
        mpstat -P ALL 1 > "$PROFILE_OUTPUT_DIR/${basename}.mpstat.log" 2>&1 &
        MONITOR_PIDS+=("$!")
    else
        echo "mpstat unavailable" > "$PROFILE_OUTPUT_DIR/${basename}.mpstat.log"
    fi
    start_perf "$label" "$target_pid"
}

stop_monitor() {
    local pid
    for pid in "${MONITOR_PIDS[@]:-}"; do
        [ -n "$pid" ] || continue
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    MONITOR_PIDS=()
    MONITOR_PID=""
    stop_perf
}

analyze_monitor() {
    local label="$1"
    [ ! -f "$MONITOR_LOG" ] && return

    # 分析异常:
    # - cpu_per_core > 90% → 单核接近饱和
    # - wait > 50% → 大部分时间在等待(I/O/锁/调度)
    # - majflt > 10 → 主缺页(从磁盘读入内存),内存不足或冷启动
    # - rd/wr > 50MB/s → 磁盘 I/O 尖刺
    local anomalies=$(awk '
        /cpu_total=/ {
            # 提取字段: cpu_total=311% cpu_per_core=78% wait=43% ncpu=4 ...
            split($0, a, " ")
            for (i in a) {
                if (a[i] ~ /^cpu_per_core=/) { gsub(/cpu_per_core=|%/, "", a[i]); cpu_per = a[i] }
                if (a[i] ~ /^wait=/) { gsub(/wait=|%/, "", a[i]); wait = a[i] }
                if (a[i] ~ /^ncpu=/) { gsub(/ncpu=/, "", a[i]); ncpu = a[i] }
                if (a[i] ~ /^majflt=/) { gsub(/majflt=/, "", a[i]); majflt = a[i] }
                if (a[i] ~ /^rd=/) { gsub(/rd=|kB\/s/, "", a[i]); rd = a[i] }
                if (a[i] ~ /^wr=/) { gsub(/wr=|kB\/s/, "", a[i]); wr = a[i] }
            }
            ts = $1

            # 检测异常
            anomaly = 0
            msg = ""
            if (cpu_per+0 > 90) {
                msg = msg " CPU=" cpu_per "%/core(" ncpu "核饱和)"
                anomaly = 1
            }
            if (wait+0 > 50) {
                msg = msg " wait=" wait "%(高等待)"
                anomaly = 1
            }
            if (majflt+0 > 10) {
                msg = msg " majflt=" majflt "/s(内存缺页)"
                anomaly = 1
            }
            if (rd+0 > 51200 || wr+0 > 51200) {  # >50MB/s
                msg = msg " disk=" int(rd/1024) "MB/s读+" int(wr/1024) "MB/s写"
                anomaly = 1
            }

            if (anomaly) {
                printf "  [%s]%s\n", ts, msg
            }
        }
    ' "$MONITOR_LOG")

    if [ -n "$anomalies" ]; then
        echo "⚠️  系统异常检测 [$label]:"
        echo "$anomalies"
    fi

    if [ "$PROFILE" = "1" ]; then
        local basename
        basename=$(safe_label "$label")
        cp "$MONITOR_LOG" "$PROFILE_OUTPUT_DIR/${basename}.monitor.log" 2>/dev/null || true
    else
        rm -f "$MONITOR_LOG"
    fi
}

# 时间戳，格式与 server.log 一致（YYYY-MM-DD HH:MM:SS.mmm），便于压测日志对齐。
# GNU date（Linux 压测机）支持 %3N 毫秒；BSD date（macOS）不支持，退化为秒级。
if date '+%3N' 2>/dev/null | grep -qE '^[0-9]{3}$'; then
    ts() { date '+%Y-%m-%d %H:%M:%S.%3N'; }
else
    ts() { date '+%Y-%m-%d %H:%M:%S'; }
fi

run_wrk() {
    local label="$1" url="$2" script="$3"
    echo ""
    echo "=== $label ==="
    echo "  [开始 $(ts)]"

    # 启动系统监控
    start_monitor "$label"

    local output
    output=$(wrk -t"$THREADS" -c"$CONCURRENCY" -d"$DURATION" --timeout "$TIMEOUT" -s "$script" "$url" 2>&1)

    # 停止监控并分析
    stop_monitor
    analyze_monitor "$label"

    echo "  [结束 $(ts)]"
    echo "$output"
    local rps
    rps=$(echo "$output" | grep "Requests/sec:" | awk '{print $2}')
    local avg_lat
    avg_lat=$(echo "$output" | grep "Latency" | head -1 | awk '{print $2}')
    local errors
    errors=$(echo "$output" | grep "Non-2xx" | awk '{print $NF}')
    if [ "$PROFILE" = "1" ]; then
        local basename
        basename=$(safe_label "$label")
        printf '%s\n' "$output" > "$PROFILE_OUTPUT_DIR/${basename}.wrk.log"
        {
            echo ""
            echo "[$label]"
            echo "rps=${rps:-missing} avg_latency=${avg_lat:-missing} errors=${errors:-0}"
        } >> "$PROFILE_SUMMARY"
    fi
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
if [ "$PROFILE" = "1" ]; then
    init_profile
    echo "  诊断采样: $PROFILE_OUTPUT_DIR"
fi
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
        BODY='{"appid":"member_03150715","config_key":"black_list"}'
        run_rounds "Config Direct" \
          "http://${HOST}:30001/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua" POST "$BODY"
        run_rounds "Config Gateway" \
          "http://${HOST}:8081/zebra-config/config.ConfigService/GetByAppAndKey" \
          "bench/wrk_post.lua" POST "$BODY"
        ;;
    all|*)
        # Health → 暂停 → Redis → 暂停 → MySQL → 暂停 → Config
        BODY='{"appid":"member_03150715","config_key":"black_list"}'

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
for r in "${RESULTS[@]:-}"; do
    [ -n "$r" ] || continue
    IFS="|" read -r label rps lat err <<< "$r"
    if [ "$err" = "0" ]; then
        printf "%-20s %10s %12s %10s\n" "$label" "$rps" "$lat" "0"
    else
        printf "%-20s %10s %12s %10s\n" "$label" "$rps" "$lat" "$err"
    fi
done
echo ""
[ "$FAIL" -eq 0 ] && echo "✅ 全部正常" || echo "❌ 有失败"
echo ""
echo "=========================================="
echo "  压测全部结束: $(ts)"
echo "  #3 观察窗口从此刻起算：勿停服务，静置 60-90s"
echo "  观察 server.log 中 HttpPool stats 的 zebra-config total/idle 是否回落至个位数"
if [ "$PROFILE" = "1" ]; then
    echo "  诊断日志: $PROFILE_OUTPUT_DIR"
fi
echo "=========================================="
