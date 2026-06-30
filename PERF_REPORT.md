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
