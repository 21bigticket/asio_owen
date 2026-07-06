# 压测执行手册

## 黄金法则

> **先验证，后压测。** 每次压测前，先 curl 单次确认接口返回 200 + 响应体正常（尤其 JWT Token 是否过期）。
> 不要拿着一个失效的 token/挂掉的上游直接跑 30s 压测，白白浪费 7~20 分钟。

## 测试环境

| 项目 | 值 |
|:-----|:----|
| 虚拟机 | `192.168.139.230`，6 核 / 15GB RAM，Ubuntu 22.04 |
| 帐密 | `root` / `123456` |
| 代码挂载 | `/mnt/mac/Users/mac/code/croot/asio_owen`（macOS NFS 自动同步） |
| 构建命令 | `cd /mnt/mac/Users/mac/code/croot/asio_owen && /usr/bin/cmake --build build --target server -j\$(nproc)` |
| 服务端口 | `8081`（网关），`30001`（zebra-config） |

## 压测工具规范

### 统一使用 wrk

以后压测统一使用 **wrk**（VM 本地已安装 wrk 4.1.0），原因：

- wrk 纯 epoll 多线程，数据更稳定
- wrk 输出连接错误统计（Socket Errors、Non-2xx）
- 本机 macOS 和 VM Ubuntu 均可安装
- plow 作为补充工具保留，仅用于特殊场景

### wrk Lua 脚本规范

所有 POST 请求用 Lua 脚本，存放于 `bench/` 目录：

| 脚本 | 用途 |
|:-----|:-----|
| `bench/wrk_post.lua` | POST JSON（不带 JWT token） |
| `bench/wrk_jwt.lua` | POST JSON（带 JWT token） |
| `bench/wrk_get.lua` | GET 请求 |

创建示例：
```bash
cat > /tmp/wrk_post.lua << "EOF"
wrk.method = "POST"
wrk.body = '{"appid":"member_03150715","config_key":"black_list"}'
wrk.headers["Content-Type"] = "application/json"
EOF
```

### 参数规范

| 参数 | 常规压测值 | 小批量验证值 | 说明 |
|:-----|:---------:|:-----------:|:-----|
| `-t`（线程） | 30 | 4 | 网关 Health 用 30 最优；纯 IO 接口（Redis/MySQL）也用 30 |
| `-c`（连接） | 100 | 50 | 保持 100 连接不变 |
| `-d`（时长） | 30s | 5s | 每轮 30s，每接口 2 轮 |
| `--timeout` | 10s | 10s | 超时 10s，避免长尾请求挂住 |
| 轮间暂停 | 10s | - | 接口间暂停 10s 互不影响 |

### 全量压测脚本

```bash
# 全部 5 个接口（Health → Redis → MySQL → Config直连 → Config网关）
# 每接口 2 轮，轮间暂停 10s
bash bench/bench_full.sh

# 单接口
bash bench/bench_full.sh health
bash bench/bench_full.sh redis
bash bench/bench_full.sh mysql
bash bench/bench_full.sh config
```

### 单次验证脚本

```bash
# curl 确认 200 → wrk 小批量 5s，全部 2xx 才通过
bash bench/verify.sh          # 全部
bash bench/verify.sh config   # 只 config
bash bench/verify.sh health   # 只 health
```

### 手动单条命令

```bash
# Health（GET）
wrk -t30 -c100 -d30s --timeout 10s http://127.0.0.1:8081/api/health

# Redis（GET）
wrk -t30 -c100 -d30s --timeout 10s http://127.0.0.1:8081/api/redis

# MySQL（GET）
wrk -t30 -c100 -d30s --timeout 10s http://127.0.0.1:8081/api/mysql

# Config 直连（POST）
wrk -t30 -c100 -d30s --timeout 10s -s /tmp/wrk_post.lua \
  http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey

# Config 网关（POST，无 token）
wrk -t30 -c100 -d30s --timeout 10s -s /tmp/wrk_post.lua \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey

# Config 网关（POST，带 JWT token）
wrk -t30 -c100 -d30s --timeout 10s -s /tmp/wrk_jwt.lua \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey
```

### 输出解读

关键看两个指标：
1. **Requests/sec** — 吞吐量
2. **Socket Errors**（如果出现则表示连接故障）：
   - `connect` — TCP 连接失败（上游挂了或端口耗尽）
   - `read / write` — 读写超时或断开
   - `timeout` — 请求超时
3. **Non-2xx**（如果出现则表示业务错误）

正常输出只显示 `Thread Stats` 和 `Requests/sec`，有错误时会有 `Socket errors` 和 `Non-2xx` 行。

### 陷阱记录

| 陷阱 | 说明 |
|:----|:------|
| ❌ `pkill -9 server` | 会误杀 `redis-server` + `zebra-config`（Go 二进制也叫 server），改用按路径匹配杀 |
| ❌ 旧 server 未完全退出就启动 | 新版 bind 失败，`sleep 2` 等端口释放 |
| ❌ Config 网关缺少 JWT 头 | 网关启用 JWT 鉴权后，缺少 `Authorization: Bearer` 会返回 401/403 |
| ❌ 拿着失效 token/挂掉上游直接压测 | 浪费 7~20 分钟。先用 curl 单次确认 200，再上 wrk |
| ❌ ASAN 版本 LSAN 报告被 `2>&1` 吞掉 | 不要用 `./server > /dev/null 2>&1`，LSAN 报告在 stderr |
| ❌ wrk 从本机跨网络压 VM | 网络延迟会拉低 RPS，必须在 VM 本地压 |
| ❌ 本机 `rm -rf build` 后 VM 编译 | 代码挂载 NFS 自动同步，直接在 VM 上编译即可，本机 keep 源码不动 |

## 前置条件

### 服务启动

```bash
# 1. 确保 Redis 运行
redis-cli ping || redis-server --daemonize yes

# 2. 确保上游 zebra-config 运行
curl -s --max-time 3 http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}' | head -1

# 3. 杀旧 server（按路径匹配，不会误杀 Go server）
for pid in $(pgrep -x server); do
    exe=$(readlink -f /proc/$pid/exe 2>/dev/null || echo "")
    if echo "$exe" | grep -q "build/server$"; then
        kill -9 $pid 2>/dev/null || true
    fi
done
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

## 内存检测

### 方法一：RSS 监控（常规压测前 + 后对比）

```bash
PID=$(pgrep -x server | head -1)
grep VmRSS /proc/$PID/status
# 跑完压测后再看一次
grep VmRSS /proc/$PID/status
# 不变则无泄漏
```

### 方法二：Valgrind memcheck

```bash
# 启动 Valgrind
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --track-origins=yes --log-file=/tmp/valgrind.log \
  ./server &

# 做一轮短压测（各 10s 即可）
wrk -t10 -c50 -d10s http://127.0.0.1:8081/api/health
wrk -t10 -c50 -d10s http://127.0.0.1:8081/api/redis
wrk -t10 -c50 -d10s http://127.0.0.1:8081/api/mysql
wrk -t10 -c50 -d10s -s /tmp/wrk_post.lua \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey

# SIGINT 退出触发报告
kill -INT $PID

# 检查结果
grep -E "definitely|indirectly|possibly|still reachable|ERROR SUMMARY" /tmp/valgrind.log
```

期望结果：`definitely lost: 0 bytes in 0 blocks`

### 方法三：ASAN（AddressSanitizer）长压

```bash
# 编译 ASAN 版本
rm -rf build_asan
CXX=g++ CC=gcc cmake -B build_asan -S . -Wno-dev \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build_asan --target server -j$(nproc)

# 启动（注意：stderr 不能重定向，否则 LSAN 报告丢失）
cd build_asan
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:halt_on_error=0 ./server &
PID=$!
sleep 4

# 记录初始 RSS，压测每阶段 3min，观察 RSS
grep VmRSS /proc/$PID/status
wrk -t30 -c100 -d180s http://127.0.0.1:8081/api/health
grep VmRSS /proc/$PID/status
# ... 同法压 Redis / MySQL / Config Gateway

# SIGTERM 触发 LSAN 检测
kill -TERM $PID
```

> ASAN 版本 RSS 会比普通版本高很多（142MB→1GB+），这是 ASAN 自身的 quarantine/shadow/fake-stack overhead，不是泄漏。详见 `docs/PERF_2026-07-05.md`。

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

> 两轮工具不同（plow vs wrk），绝对值不可直接对比。仅供参考。

| 接口 | v3.5 plow | v3.6 wrk |
|:----|:---------:|:--------:|
| Health | 74,980 | **95,955** |
| Redis | 22,598 | **26,568** |
| MySQL | 7,698 | **9,476** |
| Config 直连 | 6,183 | **4,396** |
| Config 网关 | 5,634 | **4,042** |

### 内存检测结论

| 检测方法 | 结果 | 结论 |
|:---------|:-----|:------|
| RSS 监控（普通版本） | 四轮压测 RSS 恒定 75MB 不变 | 无泄漏 |
| Valgrind memcheck | definitely lost = 0 | 无泄漏 |
| ASAN 12min 长压 | 零报错，RSS 增长来自 ASAN 自身 | 无泄漏 |
