# v3.5 测试计划（鉴权 + 安全模块）

## 测试环境

| 项目 | 配置 |
|------|------|
| 测试机 | 192.168.139.230 (Ubuntu 22.04 x86_64) |
| 客户端 | 同一虚拟机本机（plow / curl） |
| 服务 | asio_owen server，端口 8081 |
| 上游 | zebra-config (Go) :30001 |
| MySQL | 127.0.0.1:3306 (root/Klzz_20200528) |
| Redis | 127.0.0.1:6379 |
| JWT | 测试 token 见 `/tmp/jwt.txt` |

## 任务一：基础功能验证（curl）

启动服务：
```bash
cd /mnt/mac/Users/mac/code/croot/asio_owen/build
./server &
sleep 3
```

### 1.1 本地路由

```bash
# Health
curl -s http://127.0.0.1:8081/api/health
# 期望: {"code":0,"msg":"ok","data":"running"}

# Build（返回版本标记）
curl -s http://127.0.0.1:8081/api/build
# 期望: {"code":0,"build":"gateway-debug-20260703-client-close-trace"}

# Redis
curl -s http://127.0.0.1:8081/api/redis
# 期望: {"code":0,"msg":"ok","data":"..."}

# MySQL
curl -s http://127.0.0.1:8081/api/mysql
# 期望: {"code":0,"msg":"ok","data":[...]}
```

### 1.2 代理路由（zebra-config）

```bash
# 直接请求上游
curl -s --max-time 3 'http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey' \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}'

# 通过网关
curl -s --max-time 3 'http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey' \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}'
# 期望: 200 + 正常 JSON 响应
```

### 1.3 JWT 鉴权测试

```bash
# 准备 JWT token（有效期较长）
JWT="eyJhbGciOiJSUzI1NiIsImtpZCI6InBpeGl1LWp3dC1rZXktMSIsInR5cCI6IkpXVCJ9.eyJhdWQiOiJwaXhpdS1hcGkiLCJleHAiOjE3NzQzNTk5MzUsImlhdCI6MTc3NDEwMDczNSwiaXNzIjoicGl4aXUtZ2F0ZXdheSIsImp0aSI6IjE3NzQxMDA3MzU0Mzc1NTk2Njc5MyIsIm5hbWUiOiJsayIsIm5iZiI6MTc3NDEwMDczNSwic3ViIjoiMSIsInR5cGUiOiJhY2Nlc3MiLCJ1c2VyX2lkIjoxLCJ1c2VyX25hbWUiOiJsayJ9.tPX71KD2fMWTYo6CAPl7gTQDzcq03VOEZZqTJ_p2uEPzRcOdZX0QeaGLtfkA7EqZtRrXSVxhfJxNTDtVjKM-yI9Sf3FXpsUcMSgE5x_3Nem0gajsqGOaHerWcJmDaefJQHJDOjEEWIkhG5dqqHtm9QRcWNqkmmEka7diOEPBnYFrrXHBw4uw3hvjg0V8_k1-fYktpKEUNvL3bFVjdgORUUnWlJsO_rq5WmgZ0eEoadhS85ReUc9XMe9DhmwNztOvmws2Jmu_NcPReJbiL77b64nEO9C8R5PnI0JNtfzw5W0IfEjY_0XbS0XObGrKQDqfG1GiL59aWH09T2N07H0ZUg"
echo "$JWT" > /tmp/jwt.txt

# 带 JWT 请求白名单路径（/api/health 在 auth_whitelist 中 → 跳过 JWT → 200）
curl -s -i http://127.0.0.1:8081/api/health | head -1
# 期望: HTTP/1.1 200 OK

# 带 JWT 请求非白名单代理路由（需要 JWT → 有 token → 200）
curl -s -i 'http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey' \
  -H "Authorization: Bearer $(cat /tmp/jwt.txt)" \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}' | head -1
# 期望: HTTP/1.1 200 OK

# 不带 JWT 请求非白名单路由（无 token → 401）
curl -s -i 'http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey' \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}' | head -1
# 期望: HTTP/1.1 401 Unauthorized

# 带非法 JWT（伪造签名 → 401）
curl -s -i 'http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey' \
  -H 'Authorization: Bearer fake.token.here' \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}' | head -1
# 期望: HTTP/1.1 401 Unauthorized
```

### 1.4 IP 黑名单测试

```bash
# 给 config.ini 加 IP 黑名单
# [ip_blacklist]
# ip = 127.0.0.1

# 重启或 SIGHUP 热加载（如果实现了）
kill -HUP $(pgrep server)

# 本机请求 health（127.0.0.1 被 ban → 403）
curl -s -i http://127.0.0.1:8081/api/health | head -1
# 期望: HTTP/1.1 403 Forbidden

# 恢复：删掉黑名单配置后 kill -HUP
```

### 1.5 限流触发测试

```bash
# 配置 ip_rps = 5（每秒 5 个请求），重启
# [rate_limit]
# ip_rps = 5
# ip_burst = 5

# 快速连发 10 个请求，部分应返回 429
for i in $(seq 1 10); do
  curl -s -i http://127.0.0.1:8081/api/health | head -1
done
# 期望: 前 5 个 200，后 5 个 429（带 Retry-After 头）

# 恢复：ip_rps = 0 后 kill -HUP
```

## 任务二：常规性能压测

### 配置

```bash
cd /mnt/mac/Users/mac/code/croot/asio_owen/build

# 编译 Release 版本（无 ASAN，安全模块全开，但 JWT/黑名单/限流不配规则 = 零开销）
cmake -B build -S . -Wno-dev
cmake --build build --target server -j4
```

### 压测顺序（每个接口 30s 长压 × 2 次，间隔 15s）

```bash
# 工具检查
which plow || (apt update && apt install -y plow)
```

#### 2.1 Health（纯网关，无 IO）

```bash
echo "=== 2.1 Health ==="
plow -c 100 -d 30s http://127.0.0.1:8081/api/health
sleep 15
plow -c 100 -d 30s http://127.0.0.1:8081/api/health
sleep 15
```

#### 2.2 Redis（thread_local 连接，微秒级 IO）

```bash
echo "=== 2.2 Redis ==="
plow -c 100 -d 30s http://127.0.0.1:8081/api/redis
sleep 15
plow -c 100 -d 30s http://127.0.0.1:8081/api/redis
sleep 15
```

#### 2.3 MySQL（thread_pool worker，毫秒级 IO）

```bash
echo "=== 2.3 MySQL ==="
plow -c 100 -d 30s http://127.0.0.1:8081/api/mysql
sleep 15
plow -c 100 -d 30s http://127.0.0.1:8081/api/mysql
sleep 15
```

#### 2.4 Config（网关转发 + JWT 全链路）

```bash
echo "=== 2.4 Gateway Proxy + JWT ==="
TOKEN=$(cat /tmp/jwt.txt)
BODY='{"appid":"member_03150715","config_key":"black_list"}'

# 直接请求上游（baseline）
plow -c 100 -d 30s 'http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey' \
  -m POST \
  -H 'Content-Type: application/json' \
  -b "$BODY"
sleep 15

# 通过网关（带 JWT）
plow -c 100 -d 30s 'http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey' \
  -m POST \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -b "$BODY"
sleep 15

# 再跑一次直连取平均
plow -c 100 -d 30s 'http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey' \
  -m POST \
  -H 'Content-Type: application/json' \
  -b "$BODY"
sleep 15

# 再跑一次网关取平均
plow -c 100 -d 30s 'http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey' \
  -m POST \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -b "$BODY"
sleep 15
```

### 结果记录模板

| 接口 | 路径 | #1 RPS | #2 RPS | 平均 RPS | P50 | P99 | 成功率 |
|:----:|:----:|:------:|:------:|:--------:|:---:|:---:|:------:|
| **Health** | `/api/health` | | | | | | |
| **Redis** | `/api/redis` | | | | | | |
| **MySQL** | `/api/mysql` | | | | | | |
| **Config 直连** | `:30001` | | | | | | |
| **Config 网关** | `:8081/zebra-config/...` | | | | | | |

### 稳定性检查

```bash
# 1. server 日志
cat server.log | grep -ciE 'error|fatal|Seg|abort|crash|SIG'
# 期望: 0

# 2. 内核日志
dmesg | grep -c 'server['
# 期望: 0 新增

# 3. 压测失败数（各 plow 输出）
# 期望: 0 timeout, 0 error
```

## 任务三：内存检查压测（ASAN）

### 编译 ASAN 版本

```bash
cd /mnt/mac/Users/mac/code/croot/asio_owen
rm -rf build_asan
CXX=g++ CC=gcc cmake -B build_asan -S . -Wno-dev \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build_asan --target server -j4
```

### 启动 + RSS 监控

```bash
cd build_asan

# 终端 1：启动服务
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:halt_on_error=0 ./server &
SERVER_PID=$!

# 终端 2：RSS 监控
watch -n 2 "grep VmRSS /proc/$SERVER_PID/status"
```

### 分阶段压测（每个 3min，间隔 15s 观察 RSS）

```bash
TOKEN=$(cat /tmp/jwt.txt)
BODY='{"appid":"member_03150715","config_key":"black_list"}'

# 阶段 1：Health 3min（纯网关热路径）
echo "=== Phase 1: Health 3min ==="
grep VmRSS /proc/$SERVER_PID/status
plow -c 100 -d 180s http://127.0.0.1:8081/api/health
sleep 15
grep VmRSS /proc/$SERVER_PID/status

# 阶段 2：Redis 3min
echo "=== Phase 2: Redis 3min ==="
grep VmRSS /proc/$SERVER_PID/status
plow -c 100 -d 180s http://127.0.0.1:8081/api/redis
sleep 15
grep VmRSS /proc/$SERVER_PID/status

# 阶段 3：MySQL 3min
echo "=== Phase 3: MySQL 3min ==="
grep VmRSS /proc/$SERVER_PID/status
plow -c 100 -d 180s http://127.0.0.1:8081/api/mysql
sleep 15
grep VmRSS /proc/$SERVER_PID/status

# 阶段 4：Gateway Proxy + JWT 3min（全链路，带动限流记录积累）
echo "=== Phase 4: Gateway Proxy + JWT 3min ==="
grep VmRSS /proc/$SERVER_PID/status
plow -c 100 -d 180s 'http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey' \
  -m POST \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -b "$BODY"
sleep 15
grep VmRSS /proc/$SERVER_PID/status
```

### 检查 ASAN 报告 + 退出

```bash
# 正常退出（触发 LSAN 检测）
kill -TERM $SERVER_PID
wait $SERVER_PID
# 检查 stderr：不应有 "LeakSanitizer: detected memory leaks"
```

### RSS 记录模板

| 阶段 | 压测前 VmRSS | 压测后 VmRSS | 变化 | 说明 |
|:----|:------------:|:------------:|:----:|------|
| Health 3min | | | | 纯热路径，应持平 |
| Redis 3min | | | | 应持平 |
| MySQL 3min | | | | 连接池扩容后应稳定 |
| Gateway Proxy 3min | | | | 限流 LRU 填充后应稳定 |
| 退出后 | — | 0 | — | 所有 RAII 析构释放 |

## 任务四：限流 + 黑名单专项压测

### 配置限流

编辑 `config.ini`：

```ini
[rate_limit]
ip_rps = 1000
ip_burst = 2000

[ip_blacklist]
ip = 192.168.139.1
```

```bash
kill -TERM $(pgrep server)
./server &
```

### 触发限流

```bash
# 大量请求打 health，预期部分 429
plow -c 200 -d 60s http://127.0.0.1:8081/api/health
# 检查：能否看到 Retry-After 头
# 检查：日志 30s 内只打一次 WARN
```

### 限流 + JWT 全链路

```bash
TOKEN=$(cat /tmp/jwt.txt)
BODY='{"appid":"member_03150715","config_key":"black_list"}'

# 高并发代理转发，同时触 IP 限流
plow -c 200 -d 60s 'http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey' \
  -m POST \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -b "$BODY"
```

### 检查

```bash
# 日志采样检查：同一 reason 30s 内只打一次
grep -c "Security check rejected" server.log
# 期望：少量，不随请求数等比增长

# 检查是否有 Retry-After 头
grep -c "429" server.log
```

## 任务五：Snapshot 落盘/加载测试

```bash
# 1. 启动服务，产生限流记录
./server &
SERVER_PID=$!
plow -c 100 -d 60s http://127.0.0.1:8081/api/health
sleep 35  # 等 snapshot 落盘

# 2. 检查文件
ls -la /var/lib/asio_owen/rate_limit.bin 2>/dev/null || echo "no file (snapshot_path may differ)"
ls -la /mnt/mac/Users/mac/code/croot/asio_owen/build/rate_limit.bin 2>/dev/null || echo "no local snapshot"

# 3. 正常重启
kill -TERM $SERVER_PID
wait $SERVER_PID
sleep 2
./server &
sleep 3
grep VmRSS /proc/$(pgrep server)/status

# 4. 损坏文件测试
echo "garbage" > /tmp/rate_limit.bin  # 替换实际路径
# 重启后检查日志：rate_limit: snapshot corrupted, starting empty
```

## 快速检查命令汇总

```bash
# 服务存活
curl -s --max-time 3 http://127.0.0.1:8081/api/health | head -1

# 日志
grep -ciE 'error|fatal|Seg|abort|crash|SIG' server.log

# 内核崩溃
dmesg | grep -c 'server['

# ASAN 报告（stderr）
# 手动检查终端输出

# 内存
grep VmRSS /proc/$(pgrep server)/status
```
