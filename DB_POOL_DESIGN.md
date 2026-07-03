# 数据库连接池设计文档 (v3.3)

## 设计目标

- **弹性容量**：启动时只创建少量连接，高峰按需扩容，低谷自动回收
- **防止耗尽**：硬上限保护，超出后请求排队等待，避免打爆数据库
- **资源节约**：连接数过多时主动回收空闲连接，避免 MySQL/Redis 端连接数超限
- **线程解耦**：MySQL worker 线程数与连接池大小独立，按 CPU 核数配置
- **Redis 无锁**：Redis 连接轻量，保持 thread_local 零锁设计

## 架构总览

| 组件 | 底层 API | IO 模型 | 线程模型 | 工作线程 |
|------|:--------:|:-------:|:--------:|:--------:|
| **HTTP 服务器** | ASIO socket | 真异步 | `io_context` 多线程 | CPU 核数 |
| **MySQL 连接池** | `mysql_query` | 同步阻塞 | 共享互斥池 + 独立 maintain 线程 | CPU 核数 (worker) + 1 (maintain) |
| **Redis 连接池** | `redisCommand` | 同步阻塞 | **thread_local 专属连接** | 无需 worker 池 |

> Redis 不套用 MySQL 模板。Redis 命令微秒级，thread_local 零锁零跨线程开销，是压测翻倍的关键（PERF_REPORT.md 第 50-51 行）。

## 三层容量控制（MySQL）

```
┌─────────────────────────────────────┐
│          max_size (硬上限)           │  超出后 acquire 等待
│                                     │
│   ┌─────────────────────────────┐   │
│   │  max_idle_sec (空闲超时)     │   │  超过此秒数的空闲连接被回收
│   │                             │   │
│   │   ┌─────────────────────┐   │   │
│   │   │   min_size (常驻)    │   │   │  启动预创建 + 定期补充
│   │   │                     │   │   │
│   │   └─────────────────────┘   │   │
│   └─────────────────────────────┘   │
└─────────────────────────────────────┘
```

| 参数 | 默认值 | 作用 |
|------|:------:|------|
| `min_size` | 8 | 启动时预创建 + 日常保底数量 |
| `max_size` | 64 | 硬上限，超出后 acquire 阻塞等待 |
| `max_idle_sec` | 60 | 空闲超过此秒数的连接被回收，**按时间不按数量** |
| `connect_timeout_ms` | 1000 | 建连超时，包括 TCP 握手 + MySQL 认证 |
| `max_check` | 8 | 每次 maintain 最多检查的空闲连接数 |
| `query_timeout_ms` | 0（不设） | mysql_query 读超时，0 表示不限制（由 MySQL server 端的 wait_timeout 兜底） |

> `read_timeout_ms` 只用于 `mysql_ping` 路径，防止探活卡死。`mysql_query` 执行查询时**不设置读超时**，避免大查询被截断。若需要查询超时控制，可单独设置 `query_timeout_ms`，但当前版本暂不启用。

> v3 到 v3.1 关键变化：(1) acquire 重试改迭代防死循环；(2) maintain 健康检查更新 last_used_at；(3) 补齐 Redis 文档/实现 gap；(4) 增加 shutdown 延迟说明；(5) 增加可观测性建议。

> v3.1 到 v3.2 关键变化：(1) maintain 补充阶段增加 creating_ 防护；(2) maintain 各阶段修改 total_ 后 notify；(3) read_timeout_ms 明确只用于 ping；(4) Redis PING 失败后置空连接；(5) maintain 第二阶段改为一次性计算。

> v3.2 到 v3.3 关键变化：(1) 会话重置从 SQL `RESET SESSION` 改为 C API `mysql_reset_connection()`（修复 MySQL 5.7 兼容性）；(2) Redis 增加 `redisSetTimeout` 命令读写超时保护；(3) 删除 keepalive PING（ioc 线程上无超时的 PING 在半开连接时阻塞整个事件循环）；(4) 已知限制更新对应条目。

## 审阅追记

本文档经多轮内部审阅（v1 → v2 → v3 → v3.1 → v3.2 → v3.3），修复发现的问题见下表。历史版本归档于 git log。

## MySQL 连接池（不再使用模板抽象）

`ConnectionPool<T>` 模板只剩 MySQL 一个用户，抽象反而增加间接性。直接写 `MysqlPool` 更清晰。

### 配置

```ini
[mysql]
host = 192.168.139.230
port = 3306
user = root
pass = xxxx
db = zebra_config
min_size = 8
max_size = 64
max_idle_sec = 60           ; 空闲连接超过此秒数被回收
connect_timeout_ms = 1000   ; 建连超时（TCP握手 + 认证）
read_timeout_ms = 500       ; 仅用于 mysql_ping 探活读超时，不作用于查询
keepalive_sec = 30          ; maintain 周期
```

### 结构

```
MysqlPool
  ├── idle_pool_           — std::deque<IdleConn>, 每元素带 last_used_at 时间戳, 队首最老
  ├── total_               — 当前总连接数（普通 int, 由 mtx_ 保护）
  ├── creating_            — 正在创建的连接数（普通 int, 由 mtx_ 保护, 用于 thundering herd 防护）
  ├── mtx_ + cv_           — 互斥锁 + 条件变量（maintain 线程与 worker 线程共用同一把锁）
  ├── running_             — std::atomic<bool>, 控制 shutdown
  ├── worker_pool_         — asio::thread_pool(CPU核数), 执行 SQL 查询
  └── maintain_thread_     — std::thread, 独立线程跑 maintain 循环（不挂 io_context）
```

> 注意：`total_`、`creating_`、`idle_pool_` 均由 `mtx_` 保护，均为普通 `int` / `std::deque`，无需 atomic。`running_` 是唯一的 atomic 字段，用于 shutdown 时无锁快速检测。

### acquire 流程（完整版）

```
acquire():
  // 迭代重试，最多 2 次（1 次初始 + 1 次重试），防止递归死循环
  for retry = 0; retry < 2; ++retry:
    lock:
      while true:
        if !idle_pool_.empty():                    // 优先复用
          conn = idle_pool_.pop_front(); return conn
        if creating_ > 0:                          // 有人在建连
          cv.wait(lock);                            // 等建完复用 slot
          continue                                  // 唤醒后重新检查所有条件
        if total_ < max_size:                      // 预订 slot
          total_++; creating_++; break
        cv.wait(lock)                               // 池满，等归还
        // 唤醒后从 while 开头重新判断，优先取归还的连接
    unlock

    // 锁外建连（TCP + 认证）
    // 实现时必须设置：MYSQL_OPT_CONNECT_TIMEOUT=connect_timeout_ms
    // （仅建连阶段设 connect_timeout，不设 read_timeout。read_timeout 只用于下面的 ping）
    conn = create_connection_with_timeout()
    lock:
      creating_--
      if !conn:
        total_--; cv.notify_all()                   // 失败回滚
        continue                                    // 继续重试循环

    // 轻量健康检查
    // 实现时必须设置：MYSQL_OPT_READ_TIMEOUT=read_timeout_ms（仅 ping 路径设读超时）
    if mysql_ping(conn) != 0:
      mysql_close(conn)
      lock:
        total_--; cv.notify_all()
      continue                                      // 重试（最多到 retry=2）

    // 会话重置：清理上一个用户残留的 session 状态
    // 使用 C API mysql_reset_connection()，不走 SQL 解析、不写 binlog。
    // MySQL 5.7.6+ 支持。不要用 SQL 语句 "RESET SESSION"（仅 MySQL 8.0+ 可用）。
    // 注意：mysql_reset_connection() 内部已回滚未提交事务，无需前面再调 ROLLBACK。
    mysql_reset_connection(conn);
    return conn

  // 重试耗尽，返回 nullptr
  return nullptr
```

**要点**：

- `creating_` 计数器防止 thundering herd：同一时刻至多 1 个线程建连，其他线程 wait 等复用
- `create_connection_with_timeout()` 内部设置 `MYSQL_OPT_CONNECT_TIMEOUT=1000ms`，建连不卡死
- `mysql_ping()` 设置 `MYSQL_OPT_READ_TIMEOUT=500ms`，ping 超时 < 1s
- 会话重置避免残留事务/临时表污染下一个用户，使用 `mysql_reset_connection()` C API（MySQL 5.7.6+）
- 迭代重试而非递归 `return acquire()`，避免死循环吃满 CPU
- 被 `cv.wait` 唤醒后从 while 开头重新判断所有条件（`idle_pool_`、`creating_`、`total_`），确保正确性

### release 流程

```
release(conn):
  lock:
    conn.last_used_at = now()
    idle_pool_.push_back(conn)
    cv.notify_one()
```

**单一 idle 池**，没有两阶段队列。归还就是归还可复用，acquire 立刻能取走。释放时机、关闭与否全部交给 maintain 判断。

### maintain 流程（独立线程）

```
void maintain_loop() {
  while (running_) {
    std::this_thread::sleep_for(keepalive_sec);
    if (!running_) break;
    do_maintain();
  }
}

do_maintain():
  now = now()
  // 第一阶段：回收超时空闲连接（锁外 close）
  lock:
    to_close.clear()
    while !idle_pool_.empty():
      if now - idle_pool_.front().last_used_at < max_idle_sec:
        break                   // 队首最老，没超时则后面都年轻
      to_close.push_back(idle_pool_.front().conn)
      idle_pool_.pop_front()
      total_--
    cv_.notify_all()           // total_ 减少，通知 acquire 中等待的 worker
  unlock
  for conn in to_close:
    mysql_close(conn)           // 锁外执行，不阻塞 acquire/release

  // 第二阶段：补充到 min_size（锁外建连）
  // 一次性计算需要补充的数量，避免在循环中反复拿锁
  // 注意：maintain 预订 slot 时不设置 creating_，因为 maintain 是独占执行
  // 若 worker 在此期间 acquire 看到 total_ >= max_size 会短暂等待，maintain 完成后 notify 即可恢复
  lock:
    size_t need = (idle_pool_.size() < min_size_)  
               ? std::min(min_size_ - idle_pool_.size(), 
                          max_size_ - total_) 
               : 0;
    total_ += need;
  unlock
  conns_to_push.clear()
  for (size_t i = 0; i < need; ++i):
    conn = create_connection_with_timeout()
    lock:
      if !conn:
        total_--; continue
      conns_to_push.push_back(conn)
  lock:
    for conn in conns_to_push:
      conn.last_used_at = now()
      idle_pool_.push_back(conn)
    cv_.notify_all()           // 补充了连接，通知 acquire 中等待的 worker

  // 第三阶段：空闲连接健康检查（只检查最旧 max_check 个）
  // 全量检查会清空 idle 池，导致期间 acquire 饥饿
  lock:
    size_t check_cnt = std::min(idle_pool_.size(), max_check_);  // e.g. max_check=8
    healthy.clear()
    for (size_t i = 0; i < check_cnt; ++i):
      healthy.push_back(idle_pool_.front())
      idle_pool_.pop_front()
  unlock
  for auto& [conn, last_used] : healthy:
    if mysql_ping(conn) != 0:
      mysql_close(conn)
      lock: 
        total_--
        cv_.notify_all()       // total_ 减少，通知 acquire 中等待的 worker
    else:
      // 注意：last_used_at 更新为 now()，避免下次 maintain 重复检查该连接
      lock: idle_pool_.push_back({conn, now()})
```

**要点**：

- maintain **跑在独立线程**，不挂 io_context。即使 maintain 阻塞，HTTP 服务器不受影响
- `mysql_ping` 有 `MYSQL_OPT_READ_TIMEOUT=500ms` 超时控制，单连接探活 < 500ms
- 回收和建连均在锁外执行，不阻塞 acquire/release 热点路径
- 健康检查后更新 `last_used_at` 为 `now()`，避免下次 maintain 重复检查同一连接
- **第三阶段争议**：被 ping 的连接 push_back 到队尾后，acquire 的 pop_front 取到的是未被检查的旧连接，健康检查的有效性打折。实现时评估：acquire 路径每次已做 mysql_ping + 会话重置，maintain 第三阶段是否为冗余？如果确认为冗余可直接删除。

### 查询流程

```
execute(sql)
  ├── SQL 拷贝到栈数组 char[4096]（避免 string 跨线程）
  └── co_await asio::post(..., worker_pool_)
        └── do_query()
              ├── pool_.acquire()    → 迭代重试 + slot 预订 + 锁外建连 + ping + 会话重置
              ├── mysql_query()      → 执行查询
              ├── mysql_store_result → 获取结果
              ├── pool_.release()    → 标记 last_used_at，入 idle 池
              └── 构造 JSON 返回
```

## Redis 连接池

### 设计决策

**不套用 MySQL 共享池模型**。Redis 采用 thread_local 专属连接。压测证明 thread_local 是 Redis RPS 翻倍的关键原因。

**不需要主动 keepalive PING**。所有命令（包括 PING 类型的定时保活）都在 get_conn() 路径上通过 `ctx->err` 检测断线，连接坏了下次 cmd 自然感知重建。主动 PING 不仅不必要，而且在 Redis 服务 hang 住时会把 io_context 线程一起卡死。

### 配置

```ini
[redis]
host = 192.168.139.230
port = 6379
connect_timeout_ms = 1000  ; 建连超时（TCP 连接 + 握手）
cmd_timeout_ms = 1000      ; redisCommand 命令读写超时（io_context 线程上必须设）
```

> 不设 `max_retry`。断线后自动重连策略：当前 cmd 失败 -> 标记 ctx 断开 -> 下次 `get_conn()` 重建。单次 cmd 内不重试，避免阻塞 io_context 线程。

> `cmd_timeout_ms` 是 io_context 线程上所有 Redis 命令（包括定时 PING 类逻辑，如果将来需要）的读写超时保护。不设此值意味着 `redisCommand` 在网络分区或 Redis 挂起时无限阻塞 io_context 线程。设置 1s 超时后，故障场景最多阻塞当前 io_context 线程 1s，其他 io_context 线程（如果有独立连接）不受影响。

### 结构

```
RedisPool
  ├── thread_local redisContext*   — 每 io_context 线程专属连接
  └── get_conn()                   — 懒创建 + 断线自动重建
```

### 命令流程

```
cmd(fmt, ...)
  ├── vsnprintf 格式化命令字符串
  └── 在当前 io_context 线程直接执行
        └── do_cmd(cmdline)
              ├── get_conn()            → 懒创建或断线重建 + 设命令超时（redisSetTimeout 一次，后续所有 cmd 继承）
              ├── redisCommand(ctx,cmd)  → 直接执行（无锁，SO_RCVTIMEO 由 get_conn 已设好）
              ├── 失败则标记断开（ctx->err 被 hiredis 置非零），下次 get_conn 自动重建
              └── parse_reply()          → 解析结果
```

**`get_conn()` 断线重建 + 命令超时**：

```
get_conn():
  if tls_conn_ != nullptr:
    if ctx->err != 0:                     // 上次命令失败，检测断开
      redisFree(tls_conn_)
      tls_conn_ = nullptr
  if tls_conn_ == nullptr:
    struct timeval tv = {1, 0};           // 1s 建连超时
    tls_conn_ = redisConnectWithTimeout(host, port, tv)
    if !tls_conn_ || tls_conn_->err:
      // 建连失败，记录错误
      if tls_conn_: redisFree(tls_conn_); tls_conn_ = nullptr
  if tls_conn_ != nullptr:
    struct timeval cmd_tv = {1, 0};       // 1s 命令读写超时
    redisSetTimeout(tls_conn_, cmd_tv);    // 每次建连/重连后设一次
  return tls_conn_
```

- `redisConnectWithTimeout` 设 1s 建连超时
- `redisSetTimeout` 设 1s 命令读写超时（包括 redisCommand、redisGetReply 的 recv），防止 io_context 线程无限阻塞
- 建连超时 ≠ 命令读写超时，两者必须分别设置。`redisConnectWithTimeout` 只保 TCP 握手，不保后续 redisCommand
- 断线检测通过 `ctx->err` 判断，不额外 ping
- 不设 `max_retry`：单次 cmd 失败即返回错误给上层，下次自动重建
- **不需要主动 keepalive PING**：命令路径已有 `ctx->err` 断线检测 + 自动重建，PING 不提供额外价值，反而在 Redis 服务 hang 时阻塞 io_context

### 文档 vs 当前实现 gap

> 以下为文档设计与当前代码（`redis_pool.hpp`）的差距，实现时需对齐：

| 项目 | 文档设计 | 当前代码 | 状态 |
|------|---------|---------|:----:|
| `get_conn()` 断线重建 | 检测 `ctx->err` 后自动重建 | `redisCommand` 失败后不置空 `tls_conn_`，下次不重建 | 需实现 |
| `redisConnectWithTimeout` | 1s 建连超时 | 当前使用 `redisConnect`（默认阻塞） | 需修改 |
| `redisSetTimeout` | 1s 命令读写超时（每次 get_conn 设） | 当前未设置，`redisCommand` 无限阻塞 | 需新增 |
| keepalive timer | 已移除（不需要主动 PING，cmd 路径自带断线检测） | `keepalive_timer_` 存在但空转 | 需删除 timer |

### 变更说明（相对旧版）

| 项目 | 旧版 | 新版 |
|------|------|------|
| 模型 | thread_local 无锁 | thread_local 无锁（不变） |
| 断线重连 | 无 | `get_conn()` 检测 `ctx->err` 后重建 |
| 命令超时 | 无（无限阻塞） | 建连 `redisConnectWithTimeout` 1s + 命令 `redisSetTimeout` 1s |
| keepalive | ASIO timer 空转 | **已移除**（cmd 路径自带断线检测，不需要主动 PING） |

## Worker 线程与连接池解耦

```
旧版：MySQL 连接池大小 64 → worker 线程 64
新版：MySQL 连接池大小 64 → worker 线程 = CPU 核数（8~12）
```

- worker 线程负责 SQL 执行任务调度
- maintain 跑在独立线程，不与 worker 和 io_context 共享
- 三者各司其职，互不阻塞

## 完整生命周期

```
启动
  ├── 预创建 min_size(8) 个 MySQL 连接（锁外建连，1s 超时）
  ├── 创建 asio::thread_pool(CPU核数) — 执行 SQL
  ├── 创建独立 maintain 线程 — 定时维护
  └── Redis 无启动动作（thread_local 懒创建）

运行 — MySQL
  ├── acquire()
  │     ├── creating_ 防护 → 同一时刻至多 1 个建连
  │     ├── slot 预订 + 锁外建连（1s 超时，500ms 读超时）
  │     ├── ping 探活（500ms 超时）
  │     ├── 会话重置（mysql_reset_connection）
  │     └── 失败回滚（total--），迭代重试最多 2 次
  ├── release()
  │     └── 标记 last_used_at → 归还到 idle 池
  └── maintain（独立线程，每 keepalive_sec）
        ├── 回收空闲超时连接（锁外 close）
        ├── 补充到 min_size（锁外建连）
        └── 空闲连接健康检查（锁外 ping，仅最旧 8 个，更新 last_used_at）

运行 — Redis
  ├── 每个线程首次 cmd() 时懒创建（1s 建连超时 + 1s 命令超时）
  ├── 连接断开时 cmd 失败 → ctx->err 非零 → 下次 get_conn() 自动重建
  └── 不需要主动 keepalive PING（cmd 路径自带断线检测）

关闭
  ├── running_ = false
  ├── cv_.notify_all()           — 唤醒 acquire 中等待的 worker
  ├── MySQL: worker_pool_.join() — 等待执行中的 SQL 完成（最长等待 1 个建连超时 + 1 个 ping 超时 ≈ 1.5s）
  ├── MySQL: maintain 线程 join  — maintain 下次 sleep 醒来后检测 running_ 退出
  └── MySQL: lock + 关闭所有空闲连接
  └── Redis: thread_local 连接在线程退出时自动释放（RedisPool 不需要 keepalive timer）
```

> shutdown 延迟说明：`worker_pool_.join()` 需等待所有已提交的 SQL 执行完毕。最坏情况一个 worker 正在 `create_connection_with_timeout()`（1s）或 `mysql_ping()`（500ms），join 延迟约 1.5s。这是可接受的，因为 shutdown 是低频操作且异步信号驱动。

## 所有漏洞修复对照

| 漏洞 | 严重程度 | v3.3 修复方案 |
|:----:|:--------:|--------------|
| **v3: acquire 递归重试死循环** | P0 | 改为迭代重试 `for(retry=0;retry<2;++retry)` |
| **v3: creating_ 流程不清晰** | P0 | 增强 while 循环内注释，标注唤醒后重新检查所有条件 |
| **v3: Redis keepalive 空转** | P0 | 已移除（v3.3），依赖 cmd 路径 ctx->err 断线检测 |
| **v3: Redis get_conn 断线重建缺位** | P0 | 同上，需实现 `ctx->err` 检测后重建 |
| **v2: ping 阻塞 + maintain 挂 io_context** | P0 | maintain 独立线程；建连 1s 超时；ping 500ms 超时 |
| **v2: thundering herd** | P0 | `creating_` 计数器 |
| **v2: pending_queue 矛盾** | P0 | 删掉 pending_queue，回归单 idle 池 |
| **v1: 连接抖动** | P0 | 按 `max_idle_sec` 时间回收 |
| **v1: Redis 误套 MySQL 模板** | P0 | Redis 保持 thread_local |
| **v1: 持锁建连接** | P0 | slot 预订 + 锁外建连 |
| **v3: maintain 健康检查 last_used_at 未更新** | P1 | 入池时更新为 `now()` |
| **v3: shutdown 延迟风险** | P1 | 文档注明最长约 1.5s |
| **v3: total_/creating_ 类型不明确** | P1 | 统一为普通 int + mtx_ 保护 |
| **v2: ConnectionPool<T> 模板多余** | P1 | 删除模板，直接写 MysqlPool |
| **v2: 没有会话重置** | P1 | acquire 时 `mysql_reset_connection()` C API（5.7.6+） |
| **v2: Redis max_retry 语义不清** | P1 | 删掉 `max_retry` |
| **v2: cv.wait 后流程缺失** | P1 | wait 后重新完整循环 |
| **v1: 失败路径空白** | P1 | 建连失败回滚；acquire 迭代重试最多 2 次 |
| **v1: maintain 时序不匹配** | P1 | 独立线程 + 按空闲时长回收 |
| **v3.1: maintain 补充不感知 creating_** | P1 | maintain 第二阶段一次性计算 need = min(min_size - idle, max_size - total)，避免与 acquire 竞争 slot |
| **v3.1: maintain 修改 total_ 后不 notify** | P1 | 回收/补充/探活各阶段结束后 `cv_.notify_all()` |
| **v3.1: read_timeout_ms 作用域不明** | P1 | 明确仅用于 mysql_ping，查询不设读超时 |
| **v3.1: Redis PING 失败后连接僵尸化** | P1 | PING 失败后 `redisFree` + `tls_conn_ = nullptr` |
| **v3.1: maintain 补充用 while true 反复拿锁** | P2 | 改为一次性计算 need 数量，分批建连 |
| **v3.2: RESET SESSION 用错 API** | P0 | `mysql_query(conn, "RESET SESSION")` → `mysql_reset_connection(conn)` C API（5.7.6+） |
| **v3.2: Redis 命令读写无超时** | P0 | `redisConnectWithTimeout` 设 1s 建连超时后，追加 `redisSetTimeout` 设 1s 命令读写超时 |
| **v3.2: keepalive PING 阻塞 io_context** | P0 | 删除主动 PING。命令路径已有 `ctx->err` 断线检测 + `redisSetTimeout` 超时保护，不需要额外保活 |

## 已知限制

| 限制 | 说明 | 影响 |
|------|------|:----:|
| **密码变更** | MySQL 连接长期存活，DBA 侧改密码后现有连接不受影响，直到断开重连。这是连接池通用行为 | 低（密码变更需配合重启） |
| **RESET SESSION 兼容性** | `mysql_reset_connection()` C API 需要 MySQL 5.7.6+（MySQL Connector/C 需链接对应版本）。不要用 SQL 语句 `RESET SESSION`（仅 MySQL 8.0+ 支持） | 低（当前环境 5.7+） |
| **Redis 命令超时兜底** | `redisSetTimeout` 设 1s 后，故障 Redis 最多卡住 io_context 线程 1s（建连 1s + 命令 1s，依次发生则 2s）。此超时基于 `SO_RCVTIMEO`，不区分连接池和业务命令 | 低（设了超时就一定不会无限阻塞） |
| **shutdown 延迟** | 最坏约 1.5s（建连超时 + ping 超时） | 低（低频操作） |
| **acquire 失败直接返回** | 2 次迭代重试耗尽后 `acquire()` 返回 `nullptr`，上层构造错误响应，不会无限重试。若 MySQL 短暂不可用，用户请求会收到错误（而非挂起等待） | 低（设计如此，快速失败优于长时间阻塞） |

## 可观测性建议

以下关键指标建议在实现时加入（`LOG_DEBUG` 级别或计数器），否则线上定位问题只能靠猜：

| 指标 | 位置 | 说明 |
|------|------|------|
| `total_` / `idle_` / `creating_` 快照 | maintain 周期打印 | 了解连接池水位 |
| acquire 等待耗时 | `cv.wait` 前后打点 | 判断连接是否够用 |
| 建连次数/失败次数 | `create_connection_with_timeout` 前后 | 发现网络或权限问题 |
| 连接被回收数量 | maintain 回收时计数 | 了解回收是否激进 |
| ping 失败次数 | `mysql_ping` 失败时 | 发现死连接 |
| acquire 重试次数 | retry > 0 时 | 发现连接质量下降 |

## 配置汇总

```ini
[mysql]
host = 192.168.139.230
port = 3306
user = root
pass = Klzz_20200528
db = zebra_config
min_size = 8                ; 最小连接数（启动预创建 + 日常保底）
max_size = 64               ; 最大连接数（硬上限，超出后等待）
max_idle_sec = 60           ; 空闲回收超时（秒），按时间不按数量
connect_timeout_ms = 1000   ; 建连超时（TCP握手 + MySQL认证）
read_timeout_ms = 500       ; 仅用于 mysql_ping 探活读超时（查询不设读超时）
keepalive_sec = 30          ; maintain 维护周期（秒）

[redis]
host = 192.168.139.230
port = 6379
connect_timeout_ms = 1000   ; 建连超时（TCP 连接 + 握手）
cmd_timeout_ms = 1000       ; 命令读写超时（redisCommand 超时），防止 io_context 线程无限阻塞
```
