#!/bin/bash
# Kill all zebra-* services except zebra-config

pkill -f 'run.sh' 2>/dev/null
sleep 1

for svc in goods member stock cart passport order pay activity; do
  pkill -f "zebra-${svc}" 2>/dev/null
done

sleep 2

echo "=== live zebras ==="
ps aux | grep zebra- | grep -v grep | grep -v pixiu

echo "=== ports ==="
ss -tlnp | grep -E '3000[0-9]'

echo "=== config test ==="
curl -s --max-time 3 'http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey' \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}' | head -1
