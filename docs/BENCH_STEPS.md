# 压测执行手册

## 黄金法则

> **先验证，后压测。** 每次上 plow 前，先 curl 单次确认接口返回 200 + 响应体正常（尤其 JWT Token 是否过期）。
> 不要拿着一个失效的 token/挂掉的上游直接跑 30s 压测，白白浪费 7~20 分钟。

## 测试环境

| 项目 | 值 |
|:-----|:----|
| 虚拟机 | `192.168.139.230`，6 核 / 15GB RAM，Ubuntu 22.04 |
| 帐密 | `root` / `123456` |
| 代码挂载 | `/mnt/mac/Users/mac/code/croot/asio_owen`（macOS NFS 自动同步） |
| 压测工具 | `/root/go/bin/plow` |
| 构建命令 | `cd /mnt/mac/Users/mac/code/croot/asio_owen && cmake --build build --target server -j\$(nproc)` |
| 服务端口 | `8081`（网关），`30001`（zebra-config） |

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
# 先 curl 验证
curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:8081/api/health || exit 1

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
# 先 curl 验证
curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:8081/api/redis || exit 1

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
# 先 curl 验证
curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:8081/api/mysql || exit 1

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

# 先 curl 验证直连
curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H 'Content-Type: application/json' \
  -d "$BODY" \
  http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey || exit 1

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

# 先 curl 验证网关（Token 是否过期，服务是否正常）
curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $TOKEN" \
  -d "$BODY" \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey || exit 1

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
# 每阶段先 curl 确认接口正常
echo "=== Phase 1: Health 3min ==="
curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:8081/api/health || break
/root/go/bin/plow -c 100 -d 180s http://127.0.0.1:8081/api/health 2>&1 | tail -3
grep VmRSS /proc/$ASAN_PID/status
sleep 15

echo "=== Phase 2: Redis 3min ==="
curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:8081/api/redis || break
/root/go/bin/plow -c 100 -d 180s http://127.0.0.1:8081/api/redis 2>&1 | tail -3
grep VmRSS /proc/$ASAN_PID/status
sleep 15

echo "=== Phase 3: MySQL 3min ==="
curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:8081/api/mysql || break
/root/go/bin/plow -c 100 -d 180s http://127.0.0.1:8081/api/mysql 2>&1 | tail -3
grep VmRSS /proc/$ASAN_PID/status
sleep 15

echo "=== Phase 4: Config Gateway 3min ==="
TOKEN='eyJhbGciOiJSUzI1NiIsImtpZCI6InBpeGl1LWp3dC1rZXktMSIsInR5cCI6IkpXVCJ9.eyJhdWQiOiJwaXhpdS1hcGkiLCJjbGllbnRfdHlwZSI6ImFkbWluIiwiZGV2aWNlX2lkIjoiN2FmZjAzNTctMjQzNS00NjcxLThlNTMtOTM3MzY3MWUwZTEwIiwiZXhwIjoxNzgzNDIwMTU0LCJpYXQiOjE3ODMxNjA5NTQsImlzcyI6InBpeGl1LWdhdGV3YXkiLCJqdGkiOiIxNzgzMTYwOTU0ODQ5Mjc3NDc3MDkiLCJuYW1lIjoiYWRtaW4iLCJuYmYiOjE3ODMxNjA5NTQsInN1YiI6IjEiLCJ0eXBlIjoiYWNjZXNzIiwidXNlcl9pZCI6MSwidXNlcl9uYW1lIjoiYWRtaW4ifQ.OBgz_LmThzMgOvZ6Mr9xdkv4II15Jmd-QwDJwgK_s6zyAHFmIOnFhvus0g_ThwJXdXiKYWN6dpwZAj_DZjTBoDgC_MWLN1ksydmkR9Ta6ySHp-Y1CdWcmKe2qlae3bQg6Ji19o3ZzJYlpUrcAvKh6EEwLGbOCzCSLxl_ZmfxrWCQKtalUagkOEzINDB9jW7d_n09yg2tfRLEm8pzpSaxtH4dpQHdvNvdn92qt6XWwsOFJlffoLfWukAJvz2DsphfjiFZegk3hBemIq5RrXYKlp0E5pkm8BDY_usL84MCAYYM56o3SWkD5EqWthIuqJZ7vAgSZe_C-QHEo8eQOxAkIw'
BODY='{"appid":"member_03150715","config_key":"black_list"}'
curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $TOKEN" \
  -d "$BODY" \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey || break
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
| ❌ 拿着失效 token/挂掉上游直接压测 | 浪费 7~20 分钟。先用 curl 单次确认 200，再上 plow |
| ❌ ASAN 版本 LSAN 报告被 `2>&1` 吞掉 | 不要用 `./server > /dev/null 2>&1`，LSAN 报告在 stderr；或用 `ASAN_OPTIONS=log_path=/tmp/asan_log` |

## v3.6 压测结果（wrk，VM 本地，2026-07-05）

### 工具与参数

- **工具：** wrk 4.1.0（VM 本地安装）
- **配置：** 30 线程 / 100 连接 / 30s 时长 / 10s 超时
- **程序状态：** 去掉 `Connection: keep-alive` 硬编码，`case_sensitive_paths = true`，Config 接口免鉴权

### 结果

| 接口 | #1 RPS | #2 RPS | 平均 RPS | avg_lat | Socket Errors |
|:----|:------:|:------:|:--------:|:-------:|:-------------:|
| Health | 94,812 | 97,097 | **95,955** | 0.96ms | 0 |
| Redis | 25,930 | 27,206 | **26,568** | 3.42ms | 0 |
| MySQL | 10,533 | 8,419 | **9,476** | 10.62ms | 0 |
| Config 直连 | 4,483 | 4,309 | **4,396** | 22.79ms | 0 |
| Config 网关 | 3,405 | 4,679 | **4,042** | 24.25ms | 0 |

**网关转发效率：** 4,042 / 4,396 = **92%**

**错误统计：** 压测期间上游（zebra-config）和网关日志均无 error / timeout / cancel。

### 与 v3.5（plow）对比

> 两轮工具不同（plow vs wrk）、测试位置不同（VM 本地 vs 本机跨网络），绝对值不可直接对比。仅供参考。

| 接口 | v3.5 plow | v3.6 wrk |
|:----|:---------:|:--------:|
| Health | 74,980 | **95,955** |
| Redis | 22,598 | **26,568** |
| MySQL | 7,698 | **9,476** |
| Config 直连 | 6,183 | **4,396** |
| Config 网关 | 5,634 | **4,042** |

### 压测脚本

```bash
# wrk lua 脚本（bench/wrk_post.lua）
wrk.method = "POST"
wrk.body = '{"appid":"member_03150715","config_key":"black_list"}'
wrk.headers["Content-Type"] = "application/json"

# 全部压测
bash bench/bench_full.sh

# 单接口
bash bench/verify.sh config     # curl 确认 + wrk 5s 小批量
THREADS=30 bash bench/bench.sh config  # 正式压测
```

完整脚本见 `bench/` 目录。

### ASAN RSS 记录（待执行）

| 阶段 | 压测前 VmRSS | 压测后 VmRSS | 变化 |
|:----|:------------:|:------------:|:----:|
| Health 3min | - | - | - |
| Redis 3min | - | - | - |
| MySQL 3min | - | - | - |
| Config 3min | - | - | - |
