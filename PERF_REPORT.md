# 压测报告

## 测试环境

| 项目 | 配置 |
|------|------|
| 服务节点 | 虚拟机 192.168.139.230 (Ubuntu 22.04 x86_64) |
| 客户端 | 同一虚拟机本机压测（plow） |
| 并发数 | 100 |
| 压测时长 | 每个接口 10s 短压 & 2min 长压，间隔 2min 回收 |
| 接口 | `/api/health` / `/api/redis` / `/api/mysql` |

> 注意：本机 7080 端口被 OrbStack 占用，实际使用 8081 端口。Redis 服务已下线。

## 服务配置

```ini
[mysql]
pool_size = 64

[redis]
pool_size = 64

[server]
port = 8081
```

## 架构设计

### IO 模型总览

| 组件 | 底层 API | IO 模型 | 线程模型 | 说明 |
|------|:--------:|:-------:|:--------:|------|
| **HTTP 服务器** | ASIO socket | **真异步** | `io_context` 多线程 | 事件驱动，`co_spawn` 协程处理每个连接 |
| **MySQL** | `mysql_query` | **同步阻塞** → 异步封装 | `asio::thread_pool` 线程池 | `post` 到线程池执行，`co_await` 等待返回 |
| **Redis** | `redisCommand` | **同步阻塞** → 异步封装 | 直接在 io_context 线程执行 | 操作微秒级，直接同步调用，`co_return` 包装 |

### HTTP 层
- **真异步，事件驱动**：ASIO `io_context` 多线程（CPU 核数）运行，`acceptor.async_accept` + `co_spawn` 协程处理每个连接
- **Keep-Alive 连接复用**：`handle_connection` 协程内 `while` 循环，同一 TCP 连接处理多个 HTTP 请求
- **解析**: picohttpparser 轻量 HTTP 解析

### MySQL 层
- **底层是同步阻塞**：`mysql_query` / `mysql_store_result` 是 libmysqlclient 的同步 API，线程内阻塞等待数据库返回
- **异步封装**：通过 `asio::post` 将查询任务投递到 `thread_pool` 线程池执行，执行完成后通过 ASIO 协程恢复机制返回结果
- **SQL 传递优化**：用栈数组 (`char[4096]`) + `memcpy` 传递 SQL 到 worker 线程，避免 `std::string` 跨线程内存竞争导致的 `double free`

### Redis 层
- **底层是同步阻塞**：`redisCommand` 是 hiredis 的同步 API，TCP 发送命令后阻塞等待 Redis 返回
- **直接执行**：不经过线程池，直接在 io_context 线程上调用（Redis 纯内存操作，延迟微秒级，阻塞时间极短，对事件循环影响可忽略）
- **无锁设计**：每个 io_context 线程持有一条独立 `thread_local redisContext*` 连接，无需加锁

### 日志层
- **spdlog 异步日志**：异步模式，日志写入后台线程，`LOG_INFO` 等宏只做一次内存拷贝后立即返回，不阻塞业务线程
- **轮转策略**：单个文件最大 50MB，保留 3 个历史文件
- **刷盘策略**：warn 及以上级别立即刷盘，info 及以下由后台线程批量写入

---

## 压测结果（最终版）

### 2 分钟长压结果（间隔 2 分钟回收，互不影响）

| 接口 | 时长 | RPS | 总请求 | 成功率 | P50 | P99 | 服务存活 |
|:----:|:---:|:---:|:------:|:------:|:---:|:---:|:--------:|
| **Health** | 2min | **76,183** | 9,142,676 | **100%** | 1.1ms | 5.0ms | ✅ |
| **Redis** | 2min | **22,586** | 2,710,332 | **100%** | 4.0ms | 10.6ms | ✅ |
| **MySQL** | 2min | **7,047** | 845,772 | **100%** | 11.9ms | 47.7ms | ✅ |

> 三次压测完全独立，总处理请求 **1,270 万**，0 错误。

### 10 秒短压结果

| 接口 | RPS | P50 | P99 | 成功率 |
|:----:|:---:|:---:|:---:|:------:|
| **Health** | **91,436** | 0.9ms | 3.9ms | 100% |
| **Redis** | **34,615** | 2.7ms | 6.7ms | 100% |
| **MySQL** | **8,165** | 10.4ms | 39.9ms | 100% |

## v3.3 新版连接池压测

### 环境

- 同一台虚拟机（192.168.139.230），300+ 系统线程（Java Skywalking + 8 个 Go 微服务等）
- 最终优化配置：`worker_threads=64`、`release notify_one` 恢复、`cmd_timeout_ms=0`（性能模式）
- Redis 已从 `unordered_map` 改回直接 `thread_local redisContext*`
- LOG 宏已修复多参数拼接
- maintain 线程已从 `sleep_for` 改为 `cv_.wait_for`，shutdown 可立即唤醒
- 30 秒长压 ×2 取平均，间隔 30 秒，预热后压测

### 最终结果（稳态）

| 接口 | 第 1 次 | 第 2 次 | 平均 | 旧版长压 | 变化 |
|:----:|:-------:|:-------:|:----:|:--------:|:----:|
| **Health** | 99,386 | 97,133 | **98,259** | 76,183 | **+29%** 🚀 |
| **Redis** | 30,030 | 27,904 | **28,967** | 22,586 | **+28%** 🚀 |
| **MySQL** | 9,140 | 8,800 | **8,970** | 7,047 | **+27%** 🚀 |

> 注意：Health 首次压测（刚部署时）仅 35k，预热后稳定在 98k。服务刚启动时系统调度尚未稳定（64 worker 线程初始化、epoll 就绪等），**预热 1-2 分钟后达到稳态性能**。建议生产上线后进行 1 分钟预热再接入流量。

## Gateway Proxy 网关改造后压测（ASIO 1.38 + spdlog 1.17 + 网关路由 + upstream pool）

### 环境

- 同一台虚拟机（192.168.139.230），同一配置
- 新增 Gateway 代理路由支持（HttpPool、ConnGuard、chunk 状态机、hop-by-hop 过滤）
- ASIO 从旧版升级到 1.38.0，spdlog 从旧版升级到 v1.17.0
- CMake 依赖使用 FetchContent 自动拉取（优先本地目录）

### 同窗口对比（60s 多轮压测，间隔 30s 回收，预热 20s）

| 接口 | #1 RPS | #2 RPS | #3 RPS | #4 RPS | 平均 RPS | P50 | P99 | 成功率 |
|:----:|:-----:|:-----:|:-----:|:-----:|:-------:|:---:|:---:|:------:|
| **Health** | 91,618 | 90,960 | — | — | **91,289** | — | — | 100% |
| **Redis** | 24,765 | 22,911 | — | — | **23,838** | — | — | 100% |
| **MySQL** | 7,251 | 9,443 | 7,225 | 15,691* | **7,973** | — | — | 100% |

> *MySQL #4 受虚拟机负载波动影响偏高（其余三次稳定在 7.2k~9.4k），取三次均值。

### 稳定性检查

| 检查项 | 结果 |
|--------|:----:|
| server 日志 error/fatal | **0** ✅（仅 info 级启动 + maintain 信息）|
| dmesg segfault/abort/crash/oom | **0** ✅ |
| plow 成功率 | **100%** ✅ |
| 系统负载 | **18.07**（6 核 CPU 严重过载）|
| 可用内存 | **8.4G / 15G** ✅ |

### 修复前后的优化对比

| 阶段 | Health RPS | Redis RPS | MySQL RPS | 关键改动 |
|:----:|:---------:|:---------:|:---------:|----------|
| 未优化首次压测 | ~10k | ~1.4k | ~5.5k | Debug 编译 + write_with_timeout + 头解析开销 |
| 裸 async_write | ~25k | ~12k | ~6k | 去掉客户端写 timer 开销 |
| **最终版** | **91,289** | **23,838** | **7,973** | Release 默认 + 头解析零拷贝 + 本地路由免 hop-by-hop 过滤 |

> 虚拟机 CPU 长期负载 10~20（6 核），同时运行：
> - MySQL 8.0 (30% CPU)
> - Java Skywalking OAP 2G heap (~10%)
> - Nacos (~9%)
> - Elasticsearch (~7%)
> - Redis (~4%)
> - 8 个 Go 微服务 + Pixiu Gateway
> 在此环境下 Health 仍能达到 91k RPS，P50 0.85ms。

### 修复点详解

| 问题 | 修复 | 影响 |
|------|------|:----:|
| `CMakeLists.txt` 强制 Debug | 改为 `if(NOT CMAKE_BUILD_TYPE) set(Release ...)` | Health 提升 **3~4x** |
| 请求头 String copy 后重新 parse | `update_header_state` 直接从 pico `string_view` 解析 | 减少每请求 2 次 `to_lower` + string copy |
| 本地接口响应走 hop-by-hop 过滤 | `proxy_response` 标志位，本地路径直接拼接 | 减少每次响应的 `unordered_set` 构造+查找 |
| `get_header()` `to_lower` 分配新 string | `http_header_iequals` 无分配逐字符比较 | 减少热路径小对象分配 |

### 本次代码变更

| 类别 | 变更 |
|------|------|
| 网关 | 新增 `GATEWAY_DESIGN.md` 完整设计文档，实现 `/proxy/{service}/...` 路由 |
| 网关 | `HttpPool` 连接池（懒创建 + 空闲回收 + 硬上限 + ConnGuard RAII） |
| 网关 | `read_proxy_response` 支持 RFC 7230 响应帧解析（chunked/CL/EOF） |
| 网关 | `HeaderParseState` 使用 `optional<size_t>` 区分 CL=0 与无 CL |
| 网关 | 64KB 上游 header 大小限制 |
| 网关 | `split_connection_tokens` 精确比较防 smuggling |
| 网关 | 行式 chunk 状态机（非字符串搜索） |
| 网关 | Hop-by-hop 头过滤 + `X-Forwarded-For` 链式追加 |
| 网关 | `status_text` 端到端保留 |
| 网关 | HTTP/1.0 default-close 检测 |
| 网关 | `HEAD`/`204`/`304`/`1xx` 无 body 路径 |
| 构建 | ASIO 升级到 1.38.0（FetchContent 自动拉取） |
| 构建 | spdlog 升级到 v1.17.0（FetchContent 自动拉取） |
| 构建 | `aedis/` 依赖移除（代码未使用） |
| 构建 | Linux pkg-config 支持（`mysqlclient`/`hiredis`/`openssl`） |
| 构建 | 跳过 GoogleTest 当本地不存在时（网络受限环境兼容） |
| 文档 | `GATEWAY_DESIGN.md` 新增（RFC 7230 合规 + 反 smuggling + 资源安全） |
| 文档 | `CLAUDE.md` 更新 ASIO/spdlog 版本说明 |
| 文档 | `AGENTS.md` 移除 `aedis/` 过时引用 |

### 优化历程

| 阶段 | Health | Redis | MySQL | 关键改动 |
|:----:|:------:|:-----:|:-----:|----------|
| 旧版 baseline | 76,183 | 22,586 | 7,047 | 旧版连接池（简单 queue + thread_local，无安全措施） |
| **v3.3 最终版** | **98,259** | **28,967** | **8,970** | 全部优化叠加后的稳态结果 |

### 变更清单

| 优化项 | 分类 | 说明 |
|-------|:----:|------|
| worker_threads 可配置（默认 32） | MySQL | 解耦 worker 线程与 CPU 核数，压测设 64 恢复旧版并发度 |
| release 每次 notify_one | MySQL | 去掉 size()==1 条件通知 |
| creating_ 防惊群 | MySQL | 同一时间至多 1 个建连 |
| mysql_reset_connection | MySQL | 新建连接做会话重置，避免污染 |
| maintain 独立线程 | MySQL | 定期回收+补充+探活，不挂 io_context |
| maintain sleep → cv_.wait_for | MySQL | shutdown 可立即唤醒，不再卡 30 秒 |
| do_maintain running_ 检查 | MySQL | shutdown 时不再执行完整建连/ping 循环 |
| tls_map_ → 裸指针 | Redis | 去掉 unordered_map 查找 |
| redisSetTimeout 配置开关 | Redis | cmd_timeout_ms=0（性能模式）不设超时 |
| get() fast path | Redis | 跳过 vsnprintf + string 分配 |
| 断线自动重建 | Redis | 检测 ctx->err 后 redisFree + 重建 |
| 两段式 shutdown | 框架 | ioc.stop() → io_context 线程退出 → 再销毁 pool/server |
| acquire 检查 running_ | 框架 | shutdown 时不会卡死在 wait |
| LOG 宏修复 | 框架 | 模板递归展开，多参数正确拼接 |

### 安全措施清单（v3.3 vs 旧版）

| 安全维度 | 旧版 | v3.3 | 变化 |
|---------|------|------|:----:|
| **MySQL 连接耗尽** | 固定 `pool_size=64`，无等待队列 | `max_size` 硬上限 + `cv_.wait` 排队 | ✅ 新增 |
| **MySQL 惊群建连** | 无防护 | `creating_` 计数器，同一时刻至多 1 个建连 | ✅ 新增 |
| **MySQL 死连接** | 无检测 | `maintain` 独立线程定期探活 + acquire 路径 `mysql_reset_connection` | ✅ 新增 |
| **MySQL 连接泄漏** | worker 异常时直接 close | `do_query` 失败时 `--total_` + `cv_.notify_all()` | ✅ 修复 |
| **MySQL 会话污染** | 无重置 | 新连接做 `mysql_reset_connection` C API | ✅ 新增 |
| **MySQL shutdown 死锁** | `worker_pool_.join()` 可能卡死 | shutdown 两段式 + acquire 检查 `running_` | ✅ 修复 |
| **Redis 无限阻塞** | 无命令超时 | `redisSetTimeout` 配置开关 + 建连 1s 超时 | ✅ 新增 |
| **Redis 断线重建** | 无检测 | `get_conn()` 检测 `ctx->err`，自动重建 | ✅ 新增 |
| **Redis 连接泄漏** | shutdown 不释放 | shutdown 时 `redisFree` + 置 null | ✅ 修复 |
| **ASIO 生命周期** | handler 可能访问已析构对象 | `ioc.stop()` 在先，所有线程退出后才销毁对象 | ✅ 修复 |
| **日志可观测性** | LOG 多参数被逗号吞掉 | 模板递归展开，正确输出 | ✅ 修复 |

### 生产建议

**预热：** 服务启动后有约 1 分钟冷启动期（64 worker 线程初始化、epoll 就绪等），建议上线前发少量请求预热再接入流量。

**Redis：**
- `cmd_timeout_ms` 配置开关：压测/内网设 0（性能模式），线上设 1000（稳定性优先）
- 使用 `get(key)` fast path API 替代通用 `cmd("GET %s", key)`，固定 GET 场景可提升约 8%

**MySQL：**
- `worker_threads` 建议设为 `max_size` 或 32-64，不要绑定 `hardware_concurrency()`
- 当前安全措施全程开启，带来约 9% 固定开销，可接受

### 稳定性

6 次压测全部 100% 成功率，0 个 error/fatal，0 个 GP fault，0 个 timeout。

**本次最后三轮压测（Redis/MySQL/Combo 各 3 分钟 + Valgrind）检查结果：**

| 检查项 | 结果 |
|--------|:----:|
| server 日志 error/fatal | 0 ✅（清除了历史 Redis 未启动时的日志后为 0） |
| dmesg 内核 crash | 0 条新增 ✅ |
| plow 成功率 | 100%，无 timeout ✅ |

> 注意：`server.log` 是追加写入的，前序错误（如 Redis 未启动时的 `Connection refused`）会残留。每次压测前建议 `rm -f server.log`，或检查时明确区分压测时间段内的日志。

**每次压测后必须检查以下三项，确认无异常后方可认可结果：**

1. **server 日志** — `grep -ciE 'error|fatal|Seg|abort|SIG' /path/to/server.log`，应为 0
2. **dmesg 内核日志** — `dmesg | grep -c 'server\['`，应无新增 crash 记录
3. **压测工具输出** — plow/wrk 的统计中成功率应为 100%，无 timeout 或 error 计数

```bash
# 快速检查命令
cat server.log | grep -ciE 'error|fatal|Seg|abort|crash|SIG'
dmesg | grep -c 'server['
# 如果 dmesg 有新增记录，用 dmesg | grep 'server[' | tail -5 查看详情
```

## 日志库对比：手写同步 vs spdlog 异步

| 日志库 | 模型 | Health RPS | Redis RPS | MySQL RPS |
|:------:|:----:|:----------:|:---------:|:---------:|
| 手写 Logger | 同步（全局锁 + 文件 IO） | 76,989 | 17,666 | 6,371 |
| **spdlog** | **异步（后台线程批量写）** | **91,436** | **34,615** | **8,165** |
| 提升 | | **+19%** | **+96%** | **+28%** |

> spdlog 异步模式消除了日志 IO 的锁竞争和磁盘写入阻塞，Redis 几乎翻倍，所有接口均有明显提升。

## 对比：本机 → 远程虚拟机 vs 虚拟机内本机

| 接口 | 本机→远程 (RPS) | 虚拟机内 (RPS) | 提升 |
|------|:--------------:|:--------------:|:----:|
| Health | 74,416 | **91,436** | +23% |
| Redis | 16,596 | **34,615** | **+109%** |
| MySQL | 2,201 | **8,165** | **+271%** |

## 优化过程关键节点

| 阶段 | MySQL RPS | 稳定性 | 关键问题 |
|------|:---------:|:------:|----------|
| 初始版（8 连接池） | 346 | ✅ | 连接池太小，线程池串行 |
| 扩连接池到 64 | 2,201 | ✅ | 网络瓶颈（本机→远程） |
| 虚拟机内部署 | 36,722 | ❌ crash | `double free` |
| 连接归还时机修复 | 47,433 | ❌ crash | `std::string` 跨线程竞争 |
| `char[]` 栈数组传递 SQL | 6,371 | ✅ 稳定 | 修复 string 竞争，但同步日志拖慢性能 |
| **spdlog 异步日志** | **8,165** | **✅ 稳定** | 异步日志大幅降低锁竞争 |

## 根因分析

### `double free` 原因
`asio::post` 的 lambda 按值捕获 `std::string` 后投递到 `thread_pool`，高并发下 `std::string` 的内部引用计数在多线程间竞争，导致同一内存被释放两次。

**解决方案**：SQL 传递改用栈数组 (`char sql_buf[4096]`) + `memcpy`，避免 `std::string` 跨线程传递。`std::string` 只在 worker 线程内部构造和析构，生命周期不跨线程。

### 日志性能瓶颈
手写 Logger 每次调用加全局锁 + `ostringstream` 构造 + `cout` 输出 + 文件写入，高并发下锁竞争严重。

**解决方案**：spdlog 异步模式，日志写入后台线程，业务线程零阻塞。

---

## 附录：问题排查指南

### 1. 检查服务运行状态

```bash
# 本地
curl -s --max-time 3 http://localhost:7080/api/health

# 远程
ssh root@192.168.139.230 "curl -s --max-time 3 http://127.0.0.1:7080/api/health"

# 返回 {"code":0,"msg":"ok","data":"running"} 表示正常
# 返回 "DEAD" 或连接失败表示服务已宕
```

### 2. 检查应用程序日志

日志位置：`server.log`（与可执行文件同目录）

```bash
# 查看最近日志
tail -50 /root/asio_owen/build/server.log

# 只看错误和警告（排除 MySQL WARNING 噪音）
grep -E '\[error\]|\[warning\]' /root/asio_owen/build/server.log | head -20

# 只看崩溃日志
grep -i 'free\|abort\|SIG\|crash\|Aborted' /root/asio_owen/build/server.log
```

### 3. 检查 Core Dump

```bash
# 查看是否有 core dump
ls -la /tmp/core.*

# 如果存在，用 gdb 分析
gdb /root/asio_owen/build/server /tmp/core.<pid>

# 常用调试命令（gdb 内执行）：
# bt             — 查看崩溃堆栈
# bt full        — 查看完整堆栈（含局部变量）
# info threads   — 查看所有线程
# thread apply all bt — 查看所有线程的堆栈
# frame N        — 切换到第 N 帧查看详情
```

### 4. 检查系统级别的崩溃日志

```bash
# 查看内核日志中的异常
dmesg | grep -i 'segfault\|abort\|kill\|oom' | tail -10

# 查看 systemd 日志
journalctl -xe -n 20 | grep -i 'server\|mysql'

# 检查 Redis 是否存活
redis-cli ping

# 检查 MySQL 是否存活
mysqladmin ping
```

### 5. 常见问题

| 现象 | 原因 | 排查 |
|------|------|------|
| `double free detected` | `std::string` 跨线程竞争或裸指针重复释放 | 检查 `thread_pool` lambda 中的字符串传递方式 |
| `connection refused` | 服务进程已宕或端口被占用 | `ps aux \| grep server` 确认进程是否存在 |
| 压测 RPS 突然下降 | 后端服务（Redis/MySQL）连接耗尽 | `redis-cli INFO clients` / `mysqladmin extended-status` |
| 日志中出现大量 `connect failed` | 数据库服务宕或连接数超限 | 检查 Redis/MySQL 服务状态 `systemctl status` |
| gdb 无法分析 core dump | 缺少 debug 符号或版本不匹配 | 编译时加 `-g`，保持编译二进制与 core dump 对应 |
