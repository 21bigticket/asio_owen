# 安全模块专项验证

> ✅ **v3.5 安全模块验证完成于 2026-07-04**
> 涉及的 Bug 修复见下文。

## 前置条件

**config.ini 配置要求：**
- `[security]` 段：`jwt_secret`（RS256 时填任意非空值）、`jwt_algorithm = RS256`、`jwt_public_key`（PEM 文件路径）
- `[auth_whitelist]` 段：`path = /api/health` 等路径（使用 `key = value` 格式）
- `[upstream]` 段：`zebra-config = 127.0.0.1:30001` 等上游路由

```bash
# 确保服务运行
curl -sf http://127.0.0.1:8081/api/health || {
    cd /mnt/mac/Users/mac/code/croot/asio_owen/build
    rm -f server.log
    ./server > /dev/null 2>&1 &
    sleep 3
}
```

## 1. JWT 鉴权验证

### 1.1 无 token 请求 — 期望 401 ✅

```bash
BODY='{"appid":"member_03150715","config_key":"black_list"}'
curl -s -o /dev/null -w '%{http_code}' --max-time 3 \
  -X POST -H 'Content-Type: application/json' -b "$BODY" \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey
# 结果: 401
```

### 1.2 签名错误的 token — 期望 401 ✅

```bash
curl -s -o /dev/null -w '%{http_code}' --max-time 3 \
  -X POST -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer BAD_TOKEN' \
  -b "$BODY" \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey
# 结果: 401
```

### 1.3 正确 RS256 token — 期望 200 ✅

**生产 JWT Token（由 pixiu-gateway 签发，RS256 签名）：**
```
eyJhbGciOiJSUzI1NiIsImtpZCI6InBpeGl1LWp3dC1rZXktMSIsInR5cCI6IkpXVCJ9.eyJhdWQiOiJwaXhpdS1hcGkiLCJjbGllbnRfdHlwZSI6ImFkbWluIiwiZGV2aWNlX2lkIjoiN2FmZjAzNTctMjQzNS00NjcxLThlNTMtOTM3MzY3MWUwZTEwIiwiZXhwIjoxNzgzNDIwMTU0LCJpYXQiOjE3ODMxNjA5NTQsImlzcyI6InBpeGl1LWdhdGV3YXkiLCJqdGkiOiIxNzgzMTYwOTU0ODQ5Mjc3NDc3MDkiLCJuYW1lIjoiYWRtaW4iLCJuYmYiOjE3ODMxNjA5NTQsInN1YiI6IjEiLCJ0eXBlIjoiYWNjZXNzIiwidXNlcl9pZCI6MSwidXNlcl9uYW1lIjoiYWRtaW4ifQ.OBgz_LmThzMgOvZ6Mr9xdkv4II15Jmd-QwDJwgK_s6zyAHFmIOnFhvus0g_ThwJXdXiKYWN6dpwZAj_DZjTBoDgC_MWLN1ksydmkR9Ta6ySHp-Y1CdWcmKe2qlae3bQg6Ji19o3ZzJYlpUrcAvKh6EEwLGbOCzCSLxl_ZmfxrWCQKtalUagkOEzINDB9jW7d_n09yg2tfRLEm8pzpSaxtH4dpQHdvNvdn92qt6XWwsOFJlffoLfWukAJvz2DsphfjiFZegk3hBemIq5RrXYKlp0E5pkm8BDY_usL84MCAYYM56o3SWkD5EqWthIuqJZ7vAgSZe_C-QHEo8eQOxAkIw
```

```bash
TOKEN='eyJhbGciOiJSUzI1NiIsImtpZCI6InBpeGl1LWp3dC1rZXktMSIsInR5cCI6IkpXVCJ9.eyJhdWQiOiJwaXhpdS1hcGkiLCJjbGllbnRfdHlwZSI6ImFkbWluIiwiZGV2aWNlX2lkIjoiN2FmZjAzNTctMjQzNS00NjcxLThlNTMtOTM3MzY3MWUwZTEwIiwiZXhwIjoxNzgzNDIwMTU0LCJpYXQiOjE3ODMxNjA5NTQsImlzcyI6InBpeGl1LWdhdGV3YXkiLCJqdGkiOiIxNzgzMTYwOTU0ODQ5Mjc3NDc3MDkiLCJuYW1lIjoiYWRtaW4iLCJuYmYiOjE3ODMxNjA5NTQsInN1YiI6IjEiLCJ0eXBlIjoiYWNjZXNzIiwidXNlcl9pZCI6MSwidXNlcl9uYW1lIjoiYWRtaW4ifQ.OBgz_LmThzMgOvZ6Mr9xdkv4II15Jmd-QwDJwgK_s6zyAHFmIOnFhvus0g_ThwJXdXiKYWN6dpwZAj_DZjTBoDgC_MWLN1ksydmkR9Ta6ySHp-Y1CdWcmKe2qlae3bQg6Ji19o3ZzJYlpUrcAvKh6EEwLGbOCzCSLxl_ZmfxrWCQKtalUagkOEzINDB9jW7d_n09yg2tfRLEm8pzpSaxtH4dpQHdvNvdn92qt6XWwsOFJlffoLfWukAJvz2DsphfjiFZegk3hBemIq5RrXYKlp0E5pkm8BDY_usL84MCAYYM56o3SWkD5EqWthIuqJZ7vAgSZe_C-QHEo8eQOxAkIw'
curl -s -o /dev/null -w '%{http_code}' --max-time 3 \
  -X POST -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $TOKEN" -b "$BODY" \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey
# 结果: 200
```

### 1.4 白名单路径（/api/health）— 期望 200（跳过 JWT）✅

```bash
curl -s http://127.0.0.1:8081/api/health
# 结果: {"code":0,"msg":"ok","data":"running"}
```

## 2. IP 黑名单验证

### 2.1 当前不在黑名单 — 期望 200 ✅

```bash
curl -s http://127.0.0.1:8081/api/health
# 结果: 200
```

### 2.2 临时加入黑名单 — 期望 403

```bash
MY_IP=$(hostname -I | awk '{print $1}')
CONFIG_FILE=/mnt/mac/Users/mac/code/croot/asio_owen/build/config.ini

# 先确保 [ip_blacklist] 段存在，再追加 IP
if grep -q '^\[ip_blacklist\]' "$CONFIG_FILE"; then
    sed -i "/^\[ip_blacklist\]/a\\ip = $MY_IP" "$CONFIG_FILE"
fi

# 热加载
kill -HUP \$(fuser 8081/tcp 2>/dev/null | awk '{print \$NF}')
sleep 1
HTTP_CODE=\$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8081/api/health)
echo "黑名单验证: $HTTP_CODE (期望 403)"

# 恢复
sed -i '/^ip = '"$MY_IP"'/d' "$CONFIG_FILE"
kill -HUP \$(fuser 8081/tcp 2>/dev/null | awk '{print \$NF}')
```

## 3. IP 限流验证

### 3.1 短时间大量请求 — 期望触发 429

```bash
/root/go/bin/plow -c 200 -d 5s http://127.0.0.1:8081/api/health 2>&1 | grep -E '2xx|4xx|5xx|Latency'
```

### 3.2 查看服务日志确认 429

```bash
grep -i '429\|rate_limit\|too many' /mnt/mac/Users/mac/code/croot/asio_owen/build/server.log | tail -5
```

## 验证通过的 Bug 修复（本次迭代）

| ID | 问题 | 修复 |
|:---|:-----|:-----|
| 🐛 Config 重复 key 覆盖 | INI 解析器 `unordered_map` 对重复 key（如 `path = /api/health` ×4）只保留最后一个 | `config.hpp` 新增 `raw_entries_`（`vector<tuple<section,key,val>>`），`get_list`/`get_section` 改用有序存储 |
| 🐛 JWT 仅支持 HS256 | 代码硬编码 `hs256{}`，生产 token 是 RS256 | `jwt_auth.hpp` 支持 HS256/RS256，按 `algorithm_` 字段 dispatch；`security_rules.hpp` 新增 `jwt_public_key` 配置（支持 PEM 文件路径） |
| 🐛 白名单配置格式不一致 | 设计文档说用 `key = value`，但配置示例用的裸值 | 统一为 `path = /api/health` 格式，更新 config.ini 模板 |
| ⚠️ `pkill -x server` 误杀 zebra-config | Go 二进制名也是 `server` | 改用 `fuser -k 8081/tcp` 或 `readlink /proc/\$pid/exe` 匹配 |
| ⚠️ `LOG_DEBUG` 被日志级别过滤 | 默认 level=info，debug 日志不可见 | 临时查问题时用 `LOG_INFO`，修完移除 |

## 4. 全链路压测

所有测试通过后，运行完整压测：

```bash
cd /mnt/mac/Users/mac/code/croot/asio_owen
DURATION=30s ROUNDS=2 COOLDOWN=10 bash bench.sh all
```
