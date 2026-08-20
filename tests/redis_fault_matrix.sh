#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
compose_file="$root_dir/docker-compose.redis-fault.yml"
port="${ASIO_OWEN_REDIS_FAULT_PORT:-6389}"
cleanup() { docker compose -f "$compose_file" down -v --remove-orphans >/dev/null 2>&1 || true; }
trap cleanup EXIT

docker compose -f "$compose_file" up -d --wait
redis=(redis-cli -h 127.0.0.1 -p "$port")
test "$("${redis[@]}" ping)" = PONG
"${redis[@]}" flushall >/dev/null
"${redis[@]}" set fault:string value >/dev/null
if "${redis[@]}" lpush fault:string value >/dev/null 2>&1; then exit 1; fi
test "$("${redis[@]}" eval 'return redis.call("get", KEYS[1])' 1 missing)" = ""
"${redis[@]}" set fault:cas 1 >/dev/null
test "$("${redis[@]}" eval 'local v=redis.call("get",KEYS[1]); if v==ARGV[1] then redis.call("set",KEYS[1],ARGV[2]); return 1 end; return 0' 1 fault:cas 1 2)" = 1
test "$("${redis[@]}" get fault:cas)" = 2
docker compose -f "$compose_file" restart redis-fault >/dev/null
until test "$("${redis[@]}" ping 2>/dev/null || true)" = PONG; do sleep 0.2; done
test "$("${redis[@]}" set fault:after-restart ok)" = OK
echo "redis fault matrix: PASS"
