# 压测执行手册

## 前置条件

### 服务启动

```bash
# 1. 确保 Redis 运行
redis-cli ping || redis-server --daemonize yes

# 2. 确保上游 zebra-config 运行
curl -s --max-time 3 http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}' | head -1

# 3. 杀旧 server（精确匹配进程名，不会误杀 redis-server）
pgrep -x server | xargs kill -9 2>/dev/null
sleep 2

# 4. 启动新 server
cd /mnt/mac/Users/mac/code/croot/asio_owen/build
rm -f server.log
./server > /dev/null 2>&1 &
sleep 3

# 5. 确认服务正常
curl -s http://127.0.0.1:8081/api/health
# 期望: {"code":0,"msg":"ok","data":"running"}
```

### 清理日志（每轮压测前）

```bash
rm -f /mnt/mac/Users/mac/code/croot/asio_owen/build/server.log
```

---

## 步骤 1：Health（纯网关，无 IO）

```bash
echo "=== Health #1 ==="
/root/go/bin/plow -c 100 -d 30s http://127.0.0.1:8081/api/health 2>&1 | grep -A3 'Elapsed.*30s'
sleep 15

echo "=== Health #2 ==="
/root/go/bin/plow -c 100 -d 30s http://127.0.0.1:8081/api/health 2>&1 | grep -A3 'Elapsed.*30s'

echo "=== 检查 ==="
grep -ciE 'warn|error|fatal' /mnt/mac/Users/mac/code/croot/asio_owen/build/server.log 2>/dev/null || echo '0'
curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:8081/api/health
```

---

## 步骤 2：Redis

```bash
echo "=== Redis #1 ==="
/root/go/bin/plow -c 100 -d 30s http://127.0.0.1:8081/api/redis 2>&1 | grep -A3 'Elapsed.*30s'
sleep 15

echo "=== Redis #2 ==="
/root/go/bin/plow -c 100 -d 30s http://127.0.0.1:8081/api/redis 2>&1 | grep -A3 'Elapsed.*30s'

echo "=== 检查 ==="
grep -ciE 'warn|error|fatal' /mnt/mac/Users/mac/code/croot/asio_owen/build/server.log 2>/dev/null || echo '0'
curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:8081/api/redis
```

---

## 步骤 3：MySQL

```bash
echo "=== MySQL #1 ==="
/root/go/bin/plow -c 100 -d 30s http://127.0.0.1:8081/api/mysql 2>&1 | grep -A3 'Elapsed.*30s'
sleep 15

echo "=== MySQL #2 ==="
/root/go/bin/plow -c 100 -d 30s http://127.0.0.1:8081/api/mysql 2>&1 | grep -A3 'Elapsed.*30s'

echo "=== 检查 ==="
grep -ciE 'warn|error|fatal' /mnt/mac/Users/mac/code/croot/asio_owen/build/server.log 2>/dev/null || echo '0'
curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:8081/api/mysql
```

---

## 步骤 4：Config 直连（上游 baseline）

```bash
BODY='{"appid":"member_03150715","config_key":"black_list"}'

echo "=== Config Direct #1 ==="
/root/go/bin/plow -c 100 -d 30s -m POST \
  -H 'Content-Type: application/json' \
  -b "$BODY" \
  http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey 2>&1 | grep -A3 'Elapsed.*30s'
sleep 15

echo "=== Config Direct #2 ==="
/root/go/bin/plow -c 100 -d 30s -m POST \
  -H 'Content-Type: application/json' \
  -b "$BODY" \
  http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey 2>&1 | grep -A3 'Elapsed.*30s'

echo "=== 检查 ==="
grep -ciE 'warn|error|fatal' /mnt/mac/Users/mac/code/croot/asio_owen/build/server.log 2>/dev/null || echo '0'
```

---

## 步骤 5：Config 网关转发

> ⚠️ Config 网关需要 JWT 鉴权。`jwt_algorithm=RS256`，使用 pixiu-gateway 签发的 RS256 token。

```bash
BODY='{"appid":"member_03150715","config_key":"black_list"}'
TOKEN='eyJhbGciOiJSUzI1NiIsImtpZCI6InBpeGl1LWp3dC1rZXktMSIsInR5cCI6IkpXVCJ9.eyJhdWQiOiJwaXhpdS1hcGkiLCJjbGllbnRfdHlwZSI6ImFkbWluIiwiZGV2aWNlX2lkIjoiN2FmZjAzNTctMjQzNS00NjcxLThlNTMtOTM3MzY3MWUwZTEwIiwiZXhwIjoxNzgzNDIwMTU0LCJpYXQiOjE3ODMxNjA5NTQsImlzcyI6InBpeGl1LWdhdGV3YXkiLCJqdGkiOiIxNzgzMTYwOTU0ODQ5Mjc3NDc3MDkiLCJuYW1lIjoiYWRtaW4iLCJuYmYiOjE3ODMxNjA5NTQsInN1YiI6IjEiLCJ0eXBlIjoiYWNjZXNzIiwidXNlcl9pZCI6MSwidXNlcl9uYW1lIjoiYWRtaW4ifQ.OBgz_LmThzMgOvZ6Mr9xdkv4II15Jmd-QwDJwgK_s6zyAHFmIOnFhvus0g_ThwJXdXiKYWN6dpwZAj_DZjTBoDgC_MWLN1ksydmkR9Ta6ySHp-Y1CdWcmKe2qlae3bQg6Ji19o3ZzJYlpUrcAvKh6EEwLGbOCzCSLxl_ZmfxrWCQKtalUagkOEzINDB9jW7d_n09yg2tfRLEm8pzpSaxtH4dpQHdvNvdn92qt6XWwsOFJlffoLfWukAJvz2DsphfjiFZegk3hBemIq5RrXYKlp0E5pkm8BDY_usL84MCAYYM56o3SWkD5EqWthIuqJZ7vAgSZe_C-QHEo8eQOxAkIw'

echo "=== Config Via Gateway #1 ==="
/root/go/bin/plow -c 100 -d 30s -m POST \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $TOKEN" \
  -b "$BODY" \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey 2>&1 | grep -A3 'Elapsed.*30s'
sleep 15

echo "=== Config Via Gateway #2 ==="
/root/go/bin/plow -c 100 -d 30s -m POST \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $TOKEN" \
  -b "$BODY" \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey 2>&1 | grep -A3 'Elapsed.*30s'

echo "=== 检查 ==="
grep -ciE 'warn|error|fatal' /mnt/mac/Users/mac/code/croot/asio_owen/build/server.log 2>/dev/null || echo '0'
```

---

## 步骤 6：ASAN 内存检查压测

> ⚠️ 以下命令需在 **Linux 测试机** 执行（macOS 无 `/proc`）。
> Config 网关需要 JWT 鉴权头。

```bash
# 编译
cd /mnt/mac/Users/mac/code/croot/asio_owen
rm -rf build_asan
CXX=g++ CC=gcc cmake -B build_asan -S . -Wno-dev \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build_asan --target server -j$(nproc)

# 启动（保留 stderr 给 LSAN）
# 注意：不能用 > /dev/null 2>&1，否则 LSAN 报告丢失
cd build_asan
rm -f server.log
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:halt_on_error=0 ./server &
ASAN_PID=$!
sleep 4
curl -s http://127.0.0.1:8081/api/health

# 记录初始 RSS
grep VmRSS /proc/$ASAN_PID/status

# 分阶段压测（每个 3min，间隔 15s 观察 RSS）
echo "=== Phase 1: Health 3min ==="
/root/go/bin/plow -c 100 -d 180s http://127.0.0.1:8081/api/health 2>&1 | tail -3
grep VmRSS /proc/$ASAN_PID/status
sleep 15

echo "=== Phase 2: Redis 3min ==="
/root/go/bin/plow -c 100 -d 180s http://127.0.0.1:8081/api/redis 2>&1 | tail -3
grep VmRSS /proc/$ASAN_PID/status
sleep 15

echo "=== Phase 3: MySQL 3min ==="
/root/go/bin/plow -c 100 -d 180s http://127.0.0.1:8081/api/mysql 2>&1 | tail -3
grep VmRSS /proc/$ASAN_PID/status
sleep 15

echo "=== Phase 4: Config Gateway 3min ==="
TOKEN='eyJhbGciOiJSUzI1NiIsImtpZCI6InBpeGl1LWp3dC1rZXktMSIsInR5cCI6IkpXVCJ9.eyJhdWQiOiJwaXhpdS1hcGkiLCJjbGllbnRfdHlwZSI6ImFkbWluIiwiZGV2aWNlX2lkIjoiN2FmZjAzNTctMjQzNS00NjcxLThlNTMtOTM3MzY3MWUwZTEwIiwiZXhwIjoxNzgzNDIwMTU0LCJpYXQiOjE3ODMxNjA5NTQsImlzcyI6InBpeGl1LWdhdGV3YXkiLCJqdGkiOiIxNzgzMTYwOTU0ODQ5Mjc3NDc3MDkiLCJuYW1lIjoiYWRtaW4iLCJuYmYiOjE3ODMxNjA5NTQsInN1YiI6IjEiLCJ0eXBlIjoiYWNjZXNzIiwidXNlcl9pZCI6MSwidXNlcl9uYW1lIjoiYWRtaW4ifQ.OBgz_LmThzMgOvZ6Mr9xdkv4II15Jmd-QwDJwgK_s6zyAHFmIOnFhvus0g_ThwJXdXiKYWN6dpwZAj_DZjTBoDgC_MWLN1ksydmkR9Ta6ySHp-Y1CdWcmKe2qlae3bQg6Ji19o3ZzJYlpUrcAvKh6EEwLGbOCzCSLxl_ZmfxrWCQKtalUagkOEzINDB9jW7d_n09yg2tfRLEm8pzpSaxtH4dpQHdvNvdn92qt6XWwsOFJlffoLfWukAJvz2DsphfjiFZegk3hBemIq5RrXYKlp0E5pkm8BDY_usL84MCAYYM56o3SWkD5EqWthIuqJZ7vAgSZe_C-QHEo8eQOxAkIw'
BODY='{"appid":"member_03150715","config_key":"black_list"}'
/root/go/bin/plow -c 100 -d 180s -m POST \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $TOKEN" \
  -b "$BODY" \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey 2>&1 | tail -3
grep VmRSS /proc/$ASAN_PID/status

# 正常退出（SIGTERM 触发 LSAN 检测）
kill -TERM $ASAN_PID
wait $ASAN_PID 2>/dev/null
# LSAN 报告打印在 stderr 上（直接在当前终端输出）
echo "=== ASAN done ==="
```

> **v3.5 实际执行结果：** RSS 全程 3.6→2.7 KB，无增长趋势。Config Gateway 需要 JWT Token，已在脚本中硬编码。见 MEM_CHECK.md 详细记录。

---

## 结果记录模板

| 接口 | #1 RPS | #2 RPS | 平均 RPS | 成功率 |
|:----|:------:|:------:|:--------:|:------:|
| Health | | | | % |
| Redis | | | | % |
| MySQL | | | | % |
| Config 直连 | | | | % |
| Config 网关 | | | | % |

### ASAN RSS 记录

| 阶段 | 压测前 VmRSS | 压测后 VmRSS | 变化 |
|:----|:------------:|:------------:|:----:|
| Health 3min | | | |
| Redis 3min | | | |
| MySQL 3min | | | |
| Config 3min | | | |

---

## 陷阱记录

| 陷阱 | 说明 |
|:----|:------|
| ❌ `pkill -9 server` | 会误杀 `redis-server` + `zebra-config`（Go 二进制也叫 server），改用 `pgrep -x server \| xargs kill -9` |
| ❌ `-b @/tmp/file.json` | plow 不支持 `-b @file` 语法，发的是文件名本身，上游返回 404 |
| ❌ `-d '{"key":"val"}'` | `-d` 是 duration 不是 body，body 用 `-b` |
| ❌ 旧 server 未完全退出就启动 | 新版 bind 失败，`sleep 2` 等端口释放 |
| ❌ Config 网关缺少 JWT 头 | 网关启用 JWT 鉴权后，缺少 `Authorization: Bearer` 会返回 401/403 |
| ❌ ASAN 版本 LSAN 报告被 `2>&1` 吞掉 | 不要用 `./server > /dev/null 2>&1`，LSAN 报告在 stderr；或用 `ASAN_OPTIONS=log_path=/tmp/asan_log` |

## v3.5 已验证的结果记录

### 常规压测（30s × 2，100 并发）

| 接口 | #1 RPS | #2 RPS | 平均 RPS | 成功率 |
|:----|:------:|:------:|:--------:|:------:|
| Health | 75,131 | 74,829 | **74,980** | 100% |
| Redis | 22,685 | 22,511 | **22,598** | 100% |
| MySQL | 7,708 | 7,688 | **7,698** | 100% |
| Config 直连 | 6,199 | 6,167 | **6,183** | 100% |
| Config 网关 | 5,492 | 5,776 | **5,634** | 100% |

### ASAN RSS 记录（每阶段 3min）

| 阶段 | 压测前 VmRSS | 压测后 VmRSS | 变化 |
|:----|:------------:|:------------:|:----:|
| Health 3min | 3,668 KB | 2,772 KB | -24% |
| Redis 3min | 3,704 KB | 2,612 KB | -30% |
| MySQL 3min | 3,744 KB | 2,728 KB | -27% |
| Config 3min | 3,668 KB | 2,736 KB | -25% |

> 结论：全部 2xx，无泄漏趋势，RSS 不增反降。
