# 数据库连接池设计文档（当前实现）

> 当前状态更新于 2026-07-13。早期压测、故障排查和阶段性设计请看
> `docs/PERF_REPORT.md`、`docs/DB_POOL_CODE_OPTIMIZATION_2026-07-07.md`、
> `docs/MYSQL_POOL_REVIEW_CONFIRMATION_2026-07-11.md`、
> `docs/REDIS_POOL_REVIEW_CONFIRMATION_2026-07-11.md` 和
> `docs/REDIS_POOL_LAMBDA_FIX_2026-07-12.md`。这些文档保留历史上下文，
> 本文描述当前代码应遵守的总设计。

## 设计目标

- MySQL 同步阻塞 API 必须隔离到独立 `asio::thread_pool`，不能运行在 HTTP `io_context` 线程。
- MySQL 连接池使用硬上限、条件变量等待、空闲回收和 maintain 线程，避免打满数据库。
- Redis 支持两种模式：`direct` 保留 thread-local 快路径，`worker` 用独立线程池隔离同步 hiredis。
- Redis 命令必须有建连超时和命令读写超时兜底；生产默认推荐 `worker` 模式。
- shutdown 必须先停止入口流量，再等待 worker/maintain 退出并释放空闲连接。

## 架构总览

| 组件 | 底层 API | IO 模型 | 当前线程模型 |
|------|:--------:|:-------:|--------------|
| HTTP 服务器 | ASIO socket | 真异步 | 多线程 `io_context` |
| MySQL 连接池 | `mysql_query` / `mysql_store_result` | 同步阻塞 | 共享连接池 + SQL worker pool + maintain 线程 |
| Redis direct | `redisCommand` | 同步阻塞 | 每调用线程一个 thread-local 连接 |
| Redis worker | `redisCommand` | 同步阻塞 | Redis worker pool + 共享连接池 + maintain 线程 |

## MySQL 连接池

### 配置

```ini
[mysql]
host = 127.0.0.1
port = 3306
user = root
pass = xxxx
db = zebra_config
min_size = 8
max_size = 64
max_idle_sec = 60
connect_timeout_ms = 1000
read_timeout_ms = 500
query_timeout_ms = 0
acquire_timeout_ms = 3000
keepalive_sec = 30
worker_threads = 32
max_creating = 0
```

当前代码中 `query_timeout_ms <= 0` 会在 `validate_config()` 中归一化为
`30000`，并在创建连接时设置到 `MYSQL_OPT_READ_TIMEOUT`。因此配置文件里的
`query_timeout_ms = 0` 实际表示使用 30s 安全兜底，不是无限制慢查询。

`read_timeout_ms` 目前只传给 `mysql_ping_with_timeout()`；ping 前会临时设置
`MYSQL_OPT_READ_TIMEOUT`，按 MySQL C API 秒级粒度向上取整，ping 后恢复原
read timeout。

### 结构

```text
MysqlPool
  ├── idle_pool_          std::deque<IdleConn>
  ├── total_              当前连接总数，受 mtx_ 保护
  ├── creating_           正在建连数量，受 mtx_ 保护
  ├── max_creating_limit_ 并发建连上限
  ├── mtx_ + cv_          acquire/release/maintain 同步
  ├── worker_pool_        执行 mysql_query/mysql_store_result
  ├── maintain_thread_    回收、补充、探活
  └── running_            shutdown 标记
```

### 查询流程

`execute(std::string sql)` 的关键点是只切换 executor，不把 SQL 放进
`asio::post(lambda)` 捕获里：

```cpp
co_await asio::post(worker_pool_, asio::use_awaitable);
if (!running_) {
    co_return Result{false, "mysql pool stopped", ""};
}
co_return do_query(sql.c_str());
```

这是为了避开 GCC 11 协程对 `post(lambda, executor, use_awaitable)` 捕获对象的
UAF 风险。历史上的“栈数组传 SQL”是早期方案，不是当前实现。

`do_query()` 在 worker 线程内执行：

1. `acquire()` 获取连接。
2. `mysql_query()` 执行 SQL。
3. `mysql_store_result()` 取结果。
4. `mysql_result_to_json()` 转 JSON。
5. `ConnectionGuard` 析构时归还或丢弃连接。

### acquire/release

`acquire()` 最多迭代重试 2 次：

1. 优先从 `idle_pool_` 取连接。
2. 复用 idle 连接前执行 `mysql_reset_connection()`，清理事务和 session 状态。
3. 无 idle 且未达 `max_size`、`max_creating_limit_` 时，预订 slot 后锁外建连。
4. 池满时用 `cv_.wait_until()` 等待，超过 `acquire_timeout_ms` 返回 `nullptr`。
5. 建连失败或 reset 失败会回滚计数并通知等待者。

`release()` 只更新时间并放回 idle 队列：

```text
idle_pool_.push_back({conn, now})
cv_.notify_one()
```

连接是否过期、是否需要补充，由 maintain 线程处理。

### maintain

maintain 线程通过 `cv_.wait_for(keepalive_sec)` 等待，shutdown 时可被
`cv_.notify_all()` 立即唤醒。

每轮维护分三段：

1. 回收超过 `max_idle_sec` 的空闲连接，锁外 `mysql_close()`。
2. 补充连接到 `min_size`，先预订 slot，再锁外建连，最后批量入池。
3. 检查最旧的少量 idle 连接，当前 `max_check = 4`。

## Redis 连接池

### 配置

```ini
[redis]
host = 127.0.0.1
port = 6379
db = 0
connect_timeout_ms = 1000
cmd_timeout_ms = 500
mode = worker

min_size = 4
max_size = 32
max_idle_sec = 120
worker_threads = 16
max_creating = 0
acquire_timeout_ms = 3000
```

`cmd_timeout_ms <= 0` 会被归一化为 30000ms；建连超时小于 100ms 时按
100ms 处理。当前配置启用 `worker` 模式。

### direct 模式

direct 模式为每个调用线程维护一个 `thread_local` Redis 连接：

```text
TlsRedisConn
  ├── owner
  ├── generation
  └── unique_ptr<redisContext, redisFree>
```

`ensure_redis_tls_connection()` 会在 owner/generation 不匹配、连接不存在或
`ctx->err != 0` 时重建连接。direct 模式无共享池锁，性能最好，但同步
`redisCommand` 会占用当前调用线程。

### worker 模式

worker 模式在 `RedisPool` 内创建：

- `asio::thread_pool(worker_threads)`
- `idle_pool_`
- `total_`
- `creating_`
- `maintain_thread_`

`cmd_argv()`、`get()` 和格式化后的 `cmd()` 在 worker 模式下只切换 executor：

```cpp
co_await asio::post(*worker_pool_, asio::use_awaitable);
co_return cmd_argv_sync_impl(std::move(args));
```

不要改回 `asio::post(lambda, executor, use_awaitable)` 捕获 `std::string` 或
`std::vector<std::string>`。GCC 11 协程下该写法存在闭包浅拷贝导致的
heap-use-after-free 风险，详见 `docs/REDIS_POOL_LAMBDA_FIX_2026-07-12.md`。

worker 模式 acquire 逻辑：

1. 优先取 idle 连接。
2. 取出后执行 `SELECT db`，确保 DB 状态正确。
3. 无 idle 时受 `max_size` 和 `max_creating_limit_` 限制建连。
4. 池满时等待 `acquire_timeout_ms`。
5. maintain 线程回收超时 idle、补充到 `min_size`、少量 PING 探活。

### Redis 连接创建

所有模式都通过 `create_redis_connection()`：

1. `redisConnectWithTimeout(host, port, connect_timeout_ms)`。
2. 如 `cmd_timeout_ms > 0`，调用 `redisSetTimeout()` 设置命令读写超时。
3. 如 `db != 0`，执行 `SELECT db`。
4. 成功后递增 `created_total`。

命令失败且无 reply 时，错误路径会记录统计并丢弃坏连接；direct 模式重置 TLS
连接，worker 模式释放底层 `redisContext` 并递减 `total_`。

## shutdown 顺序

应用层应先停止 HTTP accept 和新协程调度，再销毁连接池。

MySQL:

1. `running_ = false`
2. `cv_.notify_all()`
3. join maintain 线程
4. join SQL worker pool
5. 关闭 idle 连接

Redis worker:

1. `running_ = false`
2. `cv_.notify_all()`
3. join maintain 线程
4. join Redis worker pool
5. 关闭 idle 连接

Redis direct 只能清理当前线程的 TLS 连接；其他线程的 TLS 连接会在线程退出时释放，
或在新 pool generation 首次访问时被重置。

## 当前已知限制

| 限制 | 说明 |
|------|------|
| MySQL `query_timeout_ms=0` | 当前实现会变为 30s 安全超时，不是无限制。 |
| MySQL ping 超时 | `read_timeout_ms` 会在 ping 前临时设置 `MYSQL_OPT_READ_TIMEOUT`，ping 后恢复。 |
| Redis direct | 故障 Redis 会阻塞调用线程直到命令超时。生产稳定性优先时用 worker。 |
| Redis worker reset | 复用连接时用 `SELECT db` 重置 DB，不处理 pubsub/MULTI/MONITOR 等状态；当前业务未使用这些命令。 |
| shutdown | 必须先停止入口流量，避免 pool 停止后仍有业务协程访问。 |

## 当前性能与内存基线

最新全链路基线来自 2026-07-13 VM Release 构建巡检，详见
`docs/CODE_REVIEW_FIXES_2026-07-13.md`。环境为 VM `192.168.139.230`
（6 核 / 15GB / Ubuntu 22.04 / GCC 11.4），30 线程、100 并发、30s
压测两轮取平均。

| 接口 | 平均 RPS | 平均延迟 | errors | 说明 |
|------|---------:|---------:|-------:|------|
| `/api/health` | 115k | 0.89ms | 0 | 纯 HTTP 本地路由 |
| `/api/redis` | 17.7k | 5.2ms | 0 | 当前 Redis worker 配置下的业务接口 |
| `/api/mysql` | 13.5k | 6.8ms | 0 | MySQL worker pool + 同步 libmysqlclient |
| Config Direct | 18.7k | 5.3ms | 0 | 直压上游 gRPC/HTTP 服务，不经过网关 |
| Config Gateway | 13.4k | 7.1ms | 0 | 经网关代理到同一上游 |

运行时资源基线：

| 指标 | 值 | 备注 |
|------|---:|------|
| VmRSS | 120 MB | 7 月 13 日 Release stripped 巡检，VmHWM 约 122 MB |
| 线程数 | 58 | 包含 io_context、DB/Redis worker、maintain 和日志线程 |
| fd count | 123 | 含 listener、日志、MySQL/Redis/HTTP upstream 连接 |
| ASan 错误 | 0 | 最近 ASan 构建未发现 heap-use-after-free |
| server.log error | 0 | 7 月 13 日巡检未见 crash/fatal/error；人工无 JWT stats 请求产生 401 warn |
| coredump | 0 | `coredumpctl`、`/var/lib/systemd/coredump`、repo core 文件均为空 |

历史 `docs/MEM_CHECK.md` 记录过 v3.3/v3.4 阶段的 ASAN、Valgrind 和 RSS
验证，其中部分配置如 `worker_threads=64`、`cmd_timeout_ms=0` 是当时压测快照。
当前总设计以本节和源码配置为准；历史内存文档用于回溯泄漏排查方法和趋势。

### 内存判断准则

- 生产泄漏判断优先看非 ASAN Release/stripped 进程的 RSS 长时间趋势。
- ASAN 进程 RSS 会受 shadow memory、quarantine 和 runtime metadata 影响，不能直接和生产 RSS 对比。
- Valgrind 的 `definitely lost = 0`、`indirectly lost = 0` 比 `possibly lost`
  更适合判断业务代码是否确定泄漏；线程栈、系统库缓存常落在 `possibly lost`。
- 连接池扩容会带来一次性 RSS 上升，后续稳定不增长才是关键。

### DB 侧性能解读

- MySQL RPS 主要受上游数据库、`worker_threads`、`max_size`、SQL 复杂度和
  `mysql_store_result()` 结果大小影响。
- Redis direct 模式历史上吞吐更高，但故障隔离弱；当前配置使用 worker 模式，
  稳定性优先。
- 7 月 13 日 Release 压测环境与上一轮一致，Redis RPS 从 42.6k 降到 17.7k，
  应按同环境性能回退记录。当前稳定性指标正常（errors=0、连接池稳定、RSS 稳定），
  但 Redis 业务路径吞吐需要单独定位。
- 不要把早期 `PERF_REPORT.md` 中的 `pool_size=64`、`cmd_timeout_ms=0`、
  “Redis 直接在 io_context 执行”等描述套到当前配置。

## 历史文档索引

- `docs/PERF_REPORT.md`：阶段性压测记录，包含已过时的早期架构描述。
- `docs/DB_POOL_CODE_OPTIMIZATION_2026-07-07.md`：早期 DB pool 优化建议。
- `docs/MYSQL_POOL_REVIEW_CONFIRMATION_2026-07-11.md`：MySQL 审计和 GCC 11 协程问题分析。
- `docs/REDIS_POOL_REVIEW_CONFIRMATION_2026-07-11.md`：Redis 审计和 direct/worker 取舍。
- `docs/REDIS_POOL_LAMBDA_FIX_2026-07-12.md`：Redis worker lambda 捕获 UAF 修复，是当前实现的重要依据。
- `docs/MEM_CHECK.md`：历史内存泄漏检查记录，包含 ASAN/Valgrind/RSS 方法和阶段性数据。
