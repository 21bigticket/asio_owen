#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
server_bin="${1:-$root_dir/build/server}"
compose_file="$root_dir/docker-compose.integration.yml"
project_name="asio-owen-integration-$$"
mysql_port="${ASIO_OWEN_INTEGRATION_MYSQL_PORT:-33079}"
redis_port="${ASIO_OWEN_INTEGRATION_REDIS_PORT:-6389}"
server_port="${ASIO_OWEN_INTEGRATION_SERVER_PORT:-18081}"
runtime_dir="$(mktemp -d)"
server_pid=""

compose() {
    docker compose -p "$project_name" -f "$compose_file" "$@"
}

cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    compose down -v --remove-orphans >/dev/null 2>&1 || true
    rm -rf "$runtime_dir"
}
trap cleanup EXIT

if [[ ! -x "$server_bin" ]]; then
    echo "server binary is not executable: $server_bin" >&2
    exit 1
fi

mkdir -p "$runtime_dir/config.d"
cp "$server_bin" "$runtime_dir/server"

cat >"$runtime_dir/config.d/00-integration.ini" <<EOF
[server]
port = $server_port
io_threads = 2
max_client_connections = 128
log_level = INFO
log_file = $runtime_dir/server.log
downstream_write_timeout_ms = 2000
client_header_read_timeout_ms = 2000
client_body_read_timeout_ms = 2000
combo_deadline_ms = 1500
combo_max_in_flight_queries = 4

[mysql]
host = 127.0.0.1
port = $mysql_port
user = root
pass = integration-root
db = zebra_config
min_size = 1
max_size = 4
max_idle_sec = 30
connect_timeout_ms = 1000
read_timeout_ms = 1000
query_timeout_ms = 3000
acquire_timeout_ms = 1500
keepalive_sec = 1
worker_threads = 2

[redis]
host = 127.0.0.1
port = $redis_port
db = 0
connect_timeout_ms = 1000
cmd_timeout_ms = 1000
mode = worker
min_size = 1
max_size = 4
max_idle_sec = 30
worker_threads = 2
acquire_timeout_ms = 1500

[config_sync]
enabled = false

[security]
jwt_algorithm = HS256
jwt_secret = integration-only-secret-at-least-32-bytes
jwt_issuer = integration
config_reload_interval_sec = 0

[auth_whitelist]
path = /api/health
path = /api/mysql
path = /api/redis
path = /api/combo

[rate_limit]
snapshot_interval_sec = 0

[http_pool]
max_size = 4
max_total_connections = 8
stats_interval_sec = 0
EOF

compose up -d --wait
compose exec -T redis-integration redis-cli SET demo_key integration >/dev/null

"$runtime_dir/server" >"$runtime_dir/stdout.log" 2>&1 &
server_pid=$!

request_until() {
    local path="$1"
    local expected="$2"
    local attempts="${3:-40}"
    local code=""
    for ((i = 0; i < attempts; ++i)); do
        code="$(curl -sS -o "$runtime_dir/response.json" -w '%{http_code}' \
            --connect-timeout 1 --max-time 3 "http://127.0.0.1:$server_port$path" || true)"
        if [[ "$code" == "$expected" ]]; then
            return 0
        fi
        sleep 0.25
    done
    echo "request failed: path=$path expected=$expected actual=$code" >&2
    cat "$runtime_dir/response.json" 2>/dev/null || true
    cat "$runtime_dir/stdout.log" >&2 || true
    return 1
}

request_until /api/health 200
request_until /api/mysql 200
request_until /api/redis 200
request_until /api/combo 200

compose exec -T redis-integration redis-cli DEL demo_key >/dev/null
compose exec -T redis-integration redis-cli LPUSH demo_key wrong-type >/dev/null
request_until /api/redis 500

compose restart redis-integration >/dev/null
for ((i = 0; i < 60; ++i)); do
    if compose exec -T redis-integration redis-cli PING 2>/dev/null |
            grep -q '^PONG$'; then
        break
    fi
    if ((i == 59)); then
        echo "Redis did not become ready after restart" >&2
        exit 1
    fi
    sleep 1
done
compose exec -T redis-integration redis-cli SET demo_key recovered >/dev/null
request_until /api/redis 200 60

compose restart mysql-integration >/dev/null
request_until /api/mysql 200 80

kill -TERM "$server_pid"
for ((i = 0; i < 60; ++i)); do
    if ! kill -0 "$server_pid" 2>/dev/null; then
        wait "$server_pid"
        server_pid=""
        echo "service integration: PASS"
        exit 0
    fi
    sleep 0.25
done

echo "server did not stop within 15 seconds" >&2
exit 1
