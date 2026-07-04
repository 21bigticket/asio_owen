#!/bin/bash
# =============================================================================
# asio_owen 网关压测脚本
# 使用方法:
#   ./bench.sh                    # 运行全部压测（默认）
#   ./bench.sh health              # 只压 health
#   ./bench.sh redis mysql         # 只压 redis 和 mysql
#   ./bench.sh config              # 只压 config 代理转发
#   ./bench.sh asan                # 跑 ASAN 内存检查压测
#   ./bench.sh all                 # 全部任务（常规 + 内存）
#   ./bench.sh summary             # 打印上次压测结果汇总
#
# 环境变量:
#   CONCURRENCY=100   并发数（默认 100）
#   DURATION=30s      每轮时长（默认 30s）
#   ROUNDS=2          每接口轮数（默认 2）
#   COOLDOWN=10       轮间暂停秒数（默认 10）
#   HOST=127.0.0.1    目标地址（默认本机）
#   PORT=8081         目标端口
#   UPSTREAM=30001    上游服务端口
#   PLOW=/root/go/bin/plow  plow 路径
#   JWT_FILE=/tmp/jwt.txt   JWT token 文件
#   BODY_FILE=/tmp/body.json POST body 文件
#   CONFIG_DIR=/mnt/mac/Users/mac/code/croot/asio_owen
#   BUILD_DIR=build
#   SERVER_LOG=server.log
#
# 结果输出:
#   每次压测结果追加到 bench_results.log
#   可用 ./bench.sh summary 查看汇总
# =============================================================================
set -euo pipefail

# ---- 默认配置 ----
CONCURRENCY=${CONCURRENCY:-100}
DURATION=${DURATION:-30s}
ROUNDS=${ROUNDS:-2}
COOLDOWN=${COOLDOWN:-15}
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-8081}
UPSTREAM=${UPSTREAM:-30001}
PLOW=${PLOW:-/root/go/bin/plow}
JWT_FILE=${JWT_FILE:-/tmp/jwt.txt}
BODY_FILE=${BODY_FILE:-/tmp/body.json}
CONFIG_DIR=${CONFIG_DIR:-/mnt/mac/Users/mac/code/croot/asio_owen}
BUILD_DIR=${BUILD_DIR:-build}
SERVER_LOG=${SERVER_LOG:-server.log}
RESULT_FILE=${RESULT_FILE:-${CONFIG_DIR}/bench_results.log}

BASE_URL="http://${HOST}:${PORT}"
UPSTREAM_URL="http://${HOST}:${UPSTREAM}"
SERVER_PID=""

# ---- 工具函数 ----

info()  { echo "[INFO]  $*"; }
warn()  { echo "[WARN]  $*" >&2; }
err()   { echo "[ERROR] $*" >&2; }

timestamp() {
    date '+%Y-%m-%d %H:%M:%S'
}

run_plow() {
    local label="$1"
    local url="$2"
    shift 2
    local extra_args=("$@")

    info "=== $label ==="
    info "URL: $url, Concurrency: $CONCURRENCY, Duration: $DURATION"
    info "Started at: $(timestamp)"

    local output
    output=$("$PLOW" -c "$CONCURRENCY" -d "$DURATION" "$url" "${extra_args[@]}" 2>&1)

    # 取最后一个 Summary 块（plow 实时输出多个 Summary，最后一个才是最终汇总）
    # 找到最后一个 Summary 的行号，从那里取到文末
    local last_summary_line
    last_summary_line=$(echo "$output" | grep -n '^Summary:' | tail -1 | cut -d: -f1)
    local last_summary
    last_summary=$(echo "$output" | tail -n +"$last_summary_line" | head -10)

    local rps total_count status_2xx status_5xx
    rps=$(echo "$last_summary" | grep "^  RPS " | head -1 | awk '{print $2}' || echo "?")
    total_count=$(echo "$last_summary" | grep "^  Count" | awk '{print $2}' || echo "?")
    status_2xx=$(echo "$last_summary" | grep "^    2xx" | awk '{print $2}' || echo "?")
    status_5xx=$(echo "$last_summary" | grep "^    5xx" | awk '{print $2}' || echo "?")

    local result_line="$(timestamp) | $label | RPS=$rps | Total=$total_count | 2xx=$status_2xx | 5xx=$status_5xx"
    echo "$result_line"
    echo "$result_line" >> "$RESULT_FILE"

    # 检查错误
    local error_count
    error_count=$(echo "$output" | grep -c "Error\|error\|timeout" || true)
    if [ "$error_count" -gt 0 ]; then
        warn "Plow 输出包含错误/超时信息"
        echo "$output" | grep -i "error\|timeout" | head -5
    fi
}

check_server_alive() {
    local url="${BASE_URL}/api/health"
    if curl -sf --max-time 3 "$url" > /dev/null 2>&1; then
        return 0
    else
        return 1
    fi
}

check_upstream_alive() {
    local url="${UPSTREAM_URL}/config.ConfigService/GetByAppAndKey"
    if curl -sf --max-time 3 -H 'Content-Type: application/json' \
        -d '{"appid":"member_03150715","config_key":"black_list"}' "$url" > /dev/null 2>&1; then
        return 0
    else
        return 1
    fi
}

check_stability() {
    info "=== 稳定性检查 ==="
    # 1. server 日志
    local log_errors=0
    if [ -f "$SERVER_LOG" ]; then
        log_errors=$(grep -ciE 'error|fatal|Seg|abort|crash|SIG' "$SERVER_LOG" 2>/dev/null || echo 0)
    fi
    info "Server log errors/fatal/crash: $log_errors"

    # 2. 内核日志
    local dmesg_count=0
    dmesg_count=$(dmesg 2>/dev/null | grep -c 'server\[' || echo 0)
    info "Kernel log server entries: $dmesg_count"

    # 3. 服务存活
    if check_server_alive; then
        info "Server: ALIVE ✅"
    else
        err "Server: DEAD ❌"
        return 1
    fi

    echo "$(timestamp) | stability | log_errors=$log_errors | dmesg=$dmesg_count" >> "$RESULT_FILE"
}

ensure_body_file() {
    if [ ! -f "$BODY_FILE" ]; then
        echo '{"appid":"member_03150715","config_key":"black_list"}' > "$BODY_FILE"
        info "Created $BODY_FILE"
    fi
}

ensure_jwt_file() {
    if [ ! -f "$JWT_FILE" ]; then
        warn "JWT_FILE ($JWT_FILE) not found — JWT 鉴权测试会失败"
    fi
}

# ---- 编译 & 部署 ----

cmd_build() {
    info "Building server..."
    cd "$CONFIG_DIR"
    cmake -B "$BUILD_DIR" -S . -Wno-dev 2>&1 | tail -3
    cmake --build "$BUILD_DIR" --target server -j"$(nproc)" 2>&1 | tail -3
    info "Build complete"
}

cmd_build_asan() {
    info "Building ASAN server..."
    cd "$CONFIG_DIR"
    rm -rf "${BUILD_DIR}_asan"
    CXX=g++ CC=gcc cmake -B "${BUILD_DIR}_asan" -S . -Wno-dev \
        -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
        -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
        -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" 2>&1 | tail -3
    cmake --build "${BUILD_DIR}_asan" --target server -j"$(nproc)" 2>&1 | tail -3
    info "ASAN build complete"
}

cmd_start() {
    local build="$1"
    cd "$CONFIG_DIR/$build"
    rm -f "$SERVER_LOG"
    pkill -9 server 2>/dev/null || true
    sleep 1
    ./server > /tmp/server_stdout.log 2>&1 &
    SERVER_PID=$!
    info "Server started (PID=$SERVER_PID, build=$build)"
    sleep 3
    if check_server_alive; then
        info "Server health check: OK"
    else
        err "Server health check: FAILED"
        return 1
    fi
}

cmd_stop() {
    info "Stopping server..."
    pkill -9 server 2>/dev/null || true
    sleep 1
}

# ---- 压测任务 ----

bench_health() {
    info "=== Health（纯网关，无 IO）==="
    ensure_jwt_file
    for i in $(seq 1 $ROUNDS); do
        run_plow "Health #$i" "${BASE_URL}/api/health"
        [ "$i" -lt "$ROUNDS" ] && sleep "$COOLDOWN"
    done
}

bench_redis() {
    info "=== Redis（thread_local 连接）==="
    for i in $(seq 1 $ROUNDS); do
        run_plow "Redis #$i" "${BASE_URL}/api/redis"
        [ "$i" -lt "$ROUNDS" ] && sleep "$COOLDOWN"
    done
}

bench_mysql() {
    info "=== MySQL（thread_pool worker）==="
    for i in $(seq 1 $ROUNDS); do
        run_plow "MySQL #$i" "${BASE_URL}/api/mysql"
        [ "$i" -lt "$ROUNDS" ] && sleep "$COOLDOWN"
    done
}

bench_config() {
    info "=== Config 代理转发 ==="
    ensure_body_file
    ensure_jwt_file
    local body_arg="-b $(cat "$BODY_FILE")"
    local jwt_arg=""
    if [ -f "$JWT_FILE" ]; then
        jwt_arg="-H Authorization: Bearer $(cat "$JWT_FILE")"
    fi

    # 直连上游（baseline）
    for i in $(seq 1 $ROUNDS); do
        run_plow "Config Direct #$i" \
            "${UPSTREAM_URL}/config.ConfigService/GetByAppAndKey" \
            -m POST -H 'Content-Type: application/json' -b "$(cat "$BODY_FILE")"
        [ "$i" -lt "$ROUNDS" ] && sleep "$COOLDOWN"
    done

    # 通过网关
    for i in $(seq 1 $ROUNDS); do
        run_plow "Config Via Gateway #$i" \
            "${BASE_URL}/zebra-config/config.ConfigService/GetByAppAndKey" \
            -m POST -H 'Content-Type: application/json' -H "Authorization: Bearer $(cat "$JWT_FILE")" -b "$(cat "$BODY_FILE")"
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

# ---- ASAN 内存检查压测 ----

bench_asan() {
    info "========== ASAN 内存检查压测 =========="

    cmd_stop
    cmd_build_asan
    cmd_start "${BUILD_DIR}_asan"
    sleep "$COOLDOWN"

    # 记录 RSS
    local rss_file="/tmp/asan_rss_$(date +%s).log"
    echo "ASAN RSS monitoring -> $rss_file"

    local pid
    pid=$(pgrep -f "${BUILD_DIR}_asan/server" | head -1)
    info "ASAN server PID: $pid"

    # 阶段 1: Health 3min
    info "=== Phase 1: Health 3min ==="
    grep VmRSS "/proc/$pid/status" 2>/dev/null | tee -a "$rss_file"
    /root/go/bin/plow -c 100 -d 180s http://127.0.0.1:8081/api/health 2>&1 | tail -5
    sleep "$COOLDOWN"
    grep VmRSS "/proc/$pid/status" 2>/dev/null | tee -a "$rss_file"

    # 阶段 2: Redis 3min
    info "=== Phase 2: Redis 3min ==="
    grep VmRSS "/proc/$pid/status" 2>/dev/null | tee -a "$rss_file"
    /root/go/bin/plow -c 100 -d 180s http://127.0.0.1:8081/api/redis 2>&1 | tail -5
    sleep "$COOLDOWN"
    grep VmRSS "/proc/$pid/status" 2>/dev/null | tee -a "$rss_file"

    # 阶段 3: MySQL 3min
    info "=== Phase 3: MySQL 3min ==="
    grep VmRSS "/proc/$pid/status" 2>/dev/null | tee -a "$rss_file"
    /root/go/bin/plow -c 100 -d 180s http://127.0.0.1:8081/api/mysql 2>&1 | tail -5
    sleep "$COOLDOWN"
    grep VmRSS "/proc/$pid/status" 2>/dev/null | tee -a "$rss_file"

    # 阶段 4: Config Gateway + JWT 3min
    info "=== Phase 4: Config Gateway + JWT 3min ==="
    grep VmRSS "/proc/$pid/status" 2>/dev/null | tee -a "$rss_file"
    local jwt_token=""
    [ -f "$JWT_FILE" ] && jwt_token=$(cat "$JWT_FILE")
    /root/go/bin/plow -c 100 -d 180s \
        "${BASE_URL}/zebra-config/config.ConfigService/GetByAppAndKey" \
        -m POST -H 'Content-Type: application/json' \
        -H "Authorization: Bearer $jwt_token" \
        -b "$(cat "$BODY_FILE")" 2>&1 | tail -5
    sleep 15
    grep VmRSS "/proc/$pid/status" 2>/dev/null | tee -a "$rss_file"

    # 正常退出 + ASAN 报告
    info "=== Stopping ASAN server (SIGTERM for LSAN) ==="
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    info "ASAN server exited. Check stderr for LeakSanitizer report."

    # 打印 RSS 记录
    info "=== RSS 记录 ==="
    cat "$rss_file"

    echo "$(timestamp) | ASAN done | RSS log: $rss_file" >> "$RESULT_FILE"
}

# ---- curl 基础功能验证 ----

cmd_curl_test() {
    info "=== 基础功能验证 ==="
    ensure_body_file
    ensure_jwt_file

    echo "--- 1.1 本地路由 ---"
    echo "Health:   $(curl -s -o /dev/null -w '%{http_code}' --max-time 3 ${BASE_URL}/api/health)"
    echo "Build:    $(curl -s -o /dev/null -w '%{http_code}' --max-time 3 ${BASE_URL}/api/build)"
    echo "Redis:    $(curl -s -o /dev/null -w '%{http_code}' --max-time 3 ${BASE_URL}/api/redis)"
    echo "MySQL:    $(curl -s -o /dev/null -w '%{http_code}' --max-time 3 ${BASE_URL}/api/mysql)"

    echo "--- 1.2 代理路由 ---"
    echo "Direct:   $(curl -s -o /dev/null -w '%{http_code}' --max-time 3 -H 'Content-Type: application/json' -d @${BODY_FILE} ${UPSTREAM_URL}/config.ConfigService/GetByAppAndKey)"
    echo "Gateway:  $(curl -s -o /dev/null -w '%{http_code}' --max-time 3 -H 'Content-Type: application/json' -d @${BODY_FILE} ${BASE_URL}/zebra-config/config.ConfigService/GetByAppAndKey)"

    echo "--- 1.3 JWT ---"
    echo "No JWT:   $(curl -s -o /dev/null -w '%{http_code}' --max-time 3 -H 'Content-Type: application/json' -d @${BODY_FILE} ${BASE_URL}/zebra-config/config.ConfigService/GetByAppAndKey)"
    if [ -f "$JWT_FILE" ]; then
        echo "With JWT: $(curl -s -o /dev/null -w '%{http_code}' --max-time 3 -H 'Content-Type: application/json' -H \"Authorization: Bearer $(cat $JWT_FILE)\" -d @${BODY_FILE} ${BASE_URL}/zebra-config/config.ConfigService/GetByAppAndKey)"
    fi
}

# ---- summary ----

cmd_summary() {
    if [ ! -f "$RESULT_FILE" ]; then
        echo "No results yet (file: $RESULT_FILE)"
        return
    fi
    echo "========== 压测结果汇总 =========="
    echo "来源: $RESULT_FILE"
    echo ""
    while IFS= read -r line; do
        echo "$line"
    done < "$RESULT_FILE"
    echo ""
    echo "========== 性能数据 =========="
    echo "| 接口 | #1 RPS | #2 RPS | 平均 RPS |"
    echo "|:----|:------:|:------:|:--------:|"
    for label in "Health" "Redis" "MySQL" "Config Direct" "Config Via Gateway"; do
        local rps1 rps2 avg
        rps1=$(grep "$label #1" "$RESULT_FILE" 2>/dev/null | grep -oP 'RPS=\K[0-9.]+' || echo "-")
        rps2=$(grep "$label #2" "$RESULT_FILE" 2>/dev/null | grep -oP 'RPS=\K[0-9.]+' || echo "-")
        if [ "$rps1" != "-" ] && [ "$rps2" != "-" ]; then
            avg=$(echo "scale=1; ($rps1 + $rps2) / 2" | bc 2>/dev/null || echo "?")
        else
            avg="-"
        fi
        echo "| $label | $rps1 | $rps2 | $avg |"
    done
}

# ---- 主入口 ----

main() {
    local task="${1:-all}"

    # 检查 plow
    if [ ! -x "$PLOW" ]; then
        err "plow not found at $PLOW"
        exit 1
    fi

    # 写入结果文件头
    if [ ! -f "$RESULT_FILE" ]; then
        echo "# bench results - $(date)" > "$RESULT_FILE"
    fi

    # build 单独处理
    if [ "$task" = "build" ]; then
        cmd_build
        return
    fi
    if [ "$task" = "build_asan" ]; then
        cmd_build_asan
        return
    fi

    # 启动服务（编译过的就直接启动）
    if [ "$task" != "summary" ] && [ "$task" != "asan" ]; then
        cmd_stop
        cmd_start "$BUILD_DIR"
    fi

    case "$task" in
        health)
            bench_health
            ;;
        redis)
            bench_redis
            ;;
        mysql)
            bench_mysql
            ;;
        config)
            bench_config
            ;;
        asan)
            bench_asan
            ;;
        curl|test)
            cmd_curl_test
            ;;
        all|full)
            bench_all
            check_stability
            cmd_stop
            bench_asan
            ;;
        summary)
            cmd_summary
            ;;
        *)
            echo "Usage: $0 [build|build_asan|health|redis|mysql|config|asan|curl|all|summary]"
            echo ""
            echo "  build      编译 Release"
            echo "  build_asan 编译 ASAN"
            echo "  health     只压 Health"
            echo "  redis      只压 Redis"
            echo "  mysql      只压 MySQL"
            echo "  config     压 Config 代理转发"
            echo "  asan       跑 ASAN 内存检查"
            echo "  curl       基础功能验证"
            echo "  all        全部"
            echo "  summary    查看结果汇总"
            ;;
    esac
}

main "$@"
