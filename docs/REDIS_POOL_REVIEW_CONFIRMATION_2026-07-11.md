# Redis 连接池代码评审确认报告

日期：2026-07-11

范围：

- `src/db/redis_pool.hpp`
- `src/db/redis_connection.hpp`
- `src/db/redis_connection.cpp`
- `src/db/redis_command.hpp`
- `src/db/redis_command.cpp`
- `src/db/redis_reply.hpp`
- `src/db/redis_reply.cpp`
- `src/db/redis_pool_stats.hpp`
- `src/db/redis_pool_stats.cpp`
- `src/app/app_config.hpp`
- `config.d/11-redis.ini`
- `tests/test_redis_pool.cpp`
- `docs/DB_POOL_DESIGN.md`
- `docs/PERF_REPORT.md`
- `docs/DB_POOL_CODE_OPTIMIZATION_2026-07-07.md`
- `docs/CODE_DOC_REVIEW_2026-07-06.md`

本报告只做问题确认和修复设计，不包含源码改动。

## 结论总览

当前 RedisPool 是一个明确偏性能的实现：

- 每个 `io_context` 线程通过 `thread_local` 持有专属 `redisContext`。
- Redis 命令直接在当前 `io_context` 线程上用同步 hiredis 执行。
- 没有共享连接池锁，也没有 `asio::thread_pool` 投递开销。
- 通过 owner + generation 避免同一线程内不同 `RedisPool` 实例误复用旧 TLS 连接。
- `cmd_argv()` / `cmd_argv_sync()` 提供参数化接口，能保留参数边界。
- 当前默认配置文件 `config.d/11-redis.ini` 设置 `cmd_timeout_ms = 500`，不是零超时。

该设计和 MySQLPool 不同。MySQLPool 把同步阻塞 API 隔离到 worker pool；RedisPool 则把同步 hiredis 放在 `io_context` 线程上执行。仓库文档明确把这视为性能取舍，不是偶然实现：

- `docs/DB_POOL_DESIGN.md` 写明 Redis 不套用 MySQL 共享池模型。
- `docs/PERF_REPORT.md` 记录 thread_local Redis 是 Redis RPS 提升的重要原因。
- `docs/DB_POOL_CODE_OPTIMIZATION_2026-07-07.md` 明确“不建议立刻改线程池”，但建议未来可增加 `direct | worker` 模式。

因此，评审报告中的问题不能全部按“必须立即改成 MySQL 式线程池”处理。更准确的结论是：

1. 同步 Redis 命令占用 `io_context` 线程：确认成立，这是当前设计的核心取舍。可靠性风险真实存在，但是否立刻改 worker pool 取决于生产环境对尾延迟和故障隔离的要求。
2. shutdown 后命令入口不检查 `running_`：确认成立，属于语义缺陷，建议优先修复。
3. `cmd_timeout_ms = 0` 允许永久阻塞：确认成立，但当前默认配置文件是 500ms；风险在配置被置 0 或历史性能模式下触发。
4. `do_cmd`、`cmd_argv_sync`、`get` 错误处理重复：确认成立。
5. 超时统计依赖错误字符串匹配：确认成立，可靠性一般。
6. 连接数不可通过 RedisPool 自身限制：基本成立，但在 thread_local 模型下连接数天然等于触达 Redis 的线程数；这需要文档说明或引入 worker/shared pool 才能严格限流。
7. `ioc_` 成员未使用：确认成立。
8. 无空闲保活导致首次调用可能失败：现象成立，但仓库文档当前明确反对主动 PING；更推荐使用命令超时、断线重建和可选 idle-before-use 探测，而不是默认后台 PING。

建议优先级：

- 第一批：修 shutdown 语义、禁止或兜底零命令超时、补配置校验、修超时统计。
- 第二批：抽取统一执行模板，减少重复错误处理。
- 第三批：保留 direct 快路径，同时新增可选 worker 模式，用于生产故障隔离优先场景。

## 当前实现摘要

### 连接模型

`RedisPool` 持有一个 class 级 TLS 状态：

```cpp
inline static thread_local TlsRedisConn tls_;
```

`TlsRedisConn` 内容：

```cpp
struct TlsRedisConn {
    const RedisPool* owner = nullptr;
    uint64_t generation = 0;
    RedisContextPtr conn{nullptr, redisFree};
};
```

每个线程上最多持有一个当前 owner/generation 对应的 `redisContext`。当 owner 或 generation 不匹配时，会重置 TLS 连接。

### 建连与重连

`get_conn()` 调用：

```cpp
redisContext* ctx = ensure_redis_tls_connection(
    tls_, this, generation_, connection_config(), created_total_);
```

`ensure_redis_tls_connection()` 中：

- owner/generation 不匹配时 reset。
- `tls.conn == nullptr` 时创建连接。
- `tls.conn->err != 0` 时重建连接。

建连使用：

```cpp
redisConnectWithTimeout(cfg.host.c_str(), cfg.port, tv);
```

命令超时仅在 `cfg.cmd_timeout_ms > 0` 时设置：

```cpp
if (cfg.cmd_timeout_ms > 0) {
    redisSetTimeout(ctx, cmd_tv);
}
```

### 命令执行路径

当前有三条主要路径：

1. `cmd(fmt, ...)`
   - `vsnprintf` 格式化整条命令。
   - 调用 `do_cmd(std::string cmdline)`。
   - `do_cmd()` 内执行 `redisCommand(ctx, cmdline.c_str())`。

2. `cmd_argv(std::vector<std::string>)`
   - coroutine 包装。
   - 直接 `co_return cmd_argv_sync(...)`。

3. `cmd_argv_sync(std::vector<std::string>)`
   - 构造 `RedisCommandArgv`。
   - 执行 `redisCommandArgv(ctx, argc, argv, argv_len)`。

4. `get(const char* key)`
   - fast path。
   - 直接执行 `redisCommand(ctx, "GET %s", key)`。

这三条实际执行路径均在当前调用线程同步调用 hiredis。

## 严重问题

### P0/P1-1：同步 hiredis 调用直接占用 `io_context` 线程

结论：确认成立，但它是当前设计取舍，不是实现偏差。

证据：

`cmd_argv()`：

```cpp
asio::awaitable<Reply> cmd_argv(std::vector<std::string> args) {
    co_return cmd_argv_sync(std::move(args));
}
```

`cmd_argv_sync()` 直接执行：

```cpp
redisReply* reply = (redisReply*)redisCommandArgv(
    ctx, command.argc(), command.argv.data(), command.argv_len.data());
```

`get()` 直接执行：

```cpp
redisReply* reply = (redisReply*)redisCommand(ctx, "GET %s", key);
```

`do_cmd()` 直接执行：

```cpp
redisReply* reply = (redisReply*)redisCommand(ctx, cmdline.c_str());
```

这些函数没有 `asio::post` 到独立 worker pool。只要调用发生在 `io_context` 线程上，hiredis 同步调用就会阻塞该 `io_context` 线程。

仓库文档也明确这一点：

- `docs/PERF_REPORT.md` 写明 Redis 直接在 `io_context` 线程执行。
- `docs/DB_POOL_DESIGN.md` 写明 Redis 采用 `thread_local` 专属连接，不套用 MySQL 共享池模型。
- `docs/DB_POOL_CODE_OPTIMIZATION_2026-07-07.md` 写明 Redis 故障时一个命令最多阻塞当前 `io_context` 线程 `cmd_timeout_ms`。

风险：

- Redis 服务端慢查询、主从切换、网络抖动、内核 TCP 重传、Redis hang 住时，同步调用会占住当前 `io_context` 线程。
- 该线程上的所有协程、socket 读写、定时器都无法调度。
- 如果多个 `io_context` 线程都同时命中 Redis 故障，整个 HTTP 服务吞吐可能明显下降。
- 这种风险对 `/api/redis`、`/api/combo` 这类直接访问 Redis 的路径尤其明显。

为什么不是简单的“必须马上改 worker pool”：

当前仓库选择 direct/TLS 模式有明确性能依据：

- Redis 命令通常延迟很低。
- `thread_local` 连接无锁、无跨线程投递。
- 历史压测显示 Redis RPS 对该路径较敏感。
- 文档已经把 `cmd_timeout_ms` 作为故障阻塞上限。

因此更准确的分级是：

- 如果生产目标是极致吞吐，且 Redis 和应用同机房、低延迟、超时严格配置，direct 模式可接受。
- 如果生产目标是故障隔离和稳定尾延迟，应增加 worker 模式。
- 不建议直接删除 direct 模式，否则可能造成明显性能回退。

推荐修复方向：

方案 A：增加可选 worker 模式。

```ini
[redis]
mode = direct | worker
worker_threads = 4
```

- `direct` 保持当前 TLS 快路径。
- `worker` 使用独立 `asio::thread_pool` 执行 hiredis 同步调用。
- worker 模式下可以选择每 worker thread 一个 TLS 连接，仍避免共享连接锁。
- 这样不强迫所有部署为可靠性牺牲性能。

方案 B：保持 direct 模式，但强约束超时。

- 禁止 `cmd_timeout_ms <= 0`。
- 设置兜底默认值，例如 500ms、1000ms 或 30000ms。
- 文档明确：Redis 命令会阻塞当前 `io_context` 线程，`cmd_timeout_ms` 是硬性安全阀。

推荐：

- 短期采用方案 B。
- 中期实现方案 A，让生产部署可根据目标选择。

验证方式：

- Redis 注入延迟：例如通过网络代理或 Redis `DEBUG SLEEP` 类方法模拟阻塞。
- 对比 direct 模式和 worker 模式下 `/api/health` 的 p99 是否受 Redis 故障影响。
- 重点观察 `io_context` 定时器延迟和 HTTP keep-alive 请求处理延迟。

### P0-2：shutdown 语义不完整，命令入口未检查 `running_`

结论：确认成立，建议优先修复。

证据：

`RedisPool` 有 `running_`：

```cpp
std::atomic<bool> running_;
```

构造时设为 true：

```cpp
RedisPool(asio::io_context& ioc, Config cfg)
    : running_(true),
      ...
```

shutdown 时设为 false：

```cpp
void shutdown() {
    if (!running_.exchange(false)) return;
    ...
}
```

但以下命令入口均未检查 `running_`：

- `cmd(const char* fmt, ...)`
- `cmd_argv(std::vector<std::string> args)`
- `cmd_argv_sync(std::vector<std::string> args)`
- `get(const char* key)`
- `do_cmd(std::string cmdline)`
- `get_conn()`

shutdown 后，如果业务代码仍持有 `RedisPool*` 并调用命令：

- `get_conn()` 仍会创建或复用 TLS 连接。
- 命令仍会执行。
- `shutdown()` 对“拒绝新请求”的语义不成立。

TLS 清理问题：

`shutdown()` 只清理当前线程的 TLS 连接：

```cpp
if (redis_tls_owner_matches(tls_, this, generation_)) {
    tls_.conn.reset();
    tls_.owner = nullptr;
    tls_.generation = 0;
}
```

其他 `io_context` 线程中的 `thread_local` 连接无法被当前线程主动 reset。这是 TLS 的固有限制。

当前 generation 机制能解决“下次调用时识别不同池实例”，但不能解决“已 shutdown 的同一 pool 继续调用仍执行命令”。

风险：

- shutdown 后仍可能发起 Redis 命令。
- 如果 `RedisPool` 对象尚未析构但已 shutdown，语义混乱。
- 如果 shutdown 流程中 RedisPool 先关闭而其他协程还在运行，可能出现请求继续访问已停止池的行为。

推荐修复：

所有入口第一步检查 `running_`：

```cpp
if (!running_.load(std::memory_order_acquire)) {
    stats_.inc_cmd_fail();
    co_return Reply{false, "Redis pool stopped", "", 0};
}
```

同步入口：

```cpp
if (!running_.load(std::memory_order_acquire)) {
    stats_.inc_cmd_fail();
    return Reply{false, "Redis pool stopped", "", 0};
}
```

建议 `get_conn()` 也检查：

```cpp
if (!running_) return nullptr;
```

同时更新注释：

- shutdown 只能清理当前线程 TLS 连接。
- 其他线程 TLS 连接会在线程退出时由 `unique_ptr` 自动释放。
- 如果 shutdown 后这些线程再次调用 RedisPool，会因为 `running_ == false` 直接拒绝，不会继续使用旧连接。
- 如果新 RedisPool 实例创建，generation 不同，旧 TLS 会在新实例首次调用时被 reset。

验证方式：

1. 创建 `RedisPool`。
2. 调用 `shutdown()`。
3. 再调用 `get()`、`cmd_argv()`、`cmd_argv_sync()`。
4. 应返回 `"Redis pool stopped"`，且 `created_total` 不增加。

### P0/P1-3：零命令超时模式存在永久阻塞风险

结论：确认成立，但当前默认配置文件不是 0。

证据：

`RedisPool::Config` 默认：

```cpp
int cmd_timeout_ms = 0;    // 0 表示不设命令超时（性能模式），>0 表示毫秒数
```

`create_redis_connection()`：

```cpp
if (cfg.cmd_timeout_ms > 0) {
    ...
    redisSetTimeout(ctx, cmd_tv);
}
```

因此当 `cmd_timeout_ms == 0` 时：

- 只设置了建连超时。
- 没有设置命令读写超时。
- `redisCommand()` / `redisCommandArgv()` 后续读写可能无限阻塞。

当前配置情况：

- `src/app/app_config.hpp` 默认读取值为 `1000`。
- `config.d/11-redis.ini` 当前设置为 `500`。
- `docs/PERF_REPORT.md` 曾记录压测/内网可设 `cmd_timeout_ms=0` 性能模式。

所以风险不是“当前默认一定永久阻塞”，而是“代码允许 0，且历史文档鼓励某些场景用 0”。

风险：

- Redis 节点 hang 住、网络黑洞、TCP 半开时，当前 `io_context` 线程可能永久卡在 hiredis 同步调用。
- `ctx->err` 只有调用返回后才有机会被设置；如果 syscall 永久不返回，自动重连不会触发。
- shutdown 也无法中断正在同步阻塞的 hiredis 调用。

推荐修复：

短期建议禁止 0：

```cpp
if (cfg.cmd_timeout_ms <= 0) {
    cfg.cmd_timeout_ms = 1000; // 或配置层直接报错
}
```

更兼容的方式：

- 保留 `cmd_timeout_ms = 0` 语义，但只允许通过显式 `redis.allow_no_cmd_timeout = true` 开启。
- 默认配置和生产配置禁止 0。
- 启动日志中如果发现 0，打印 `LOG_WARN`。

兜底方案：

```ini
[redis]
cmd_timeout_ms = 1000
max_cmd_timeout_ms = 30000
```

如果用户配置 0，自动改为 30000ms 兜底，避免永久挂死。

验证方式：

- 使用网络代理制造 Redis 读阻塞。
- 设置 `cmd_timeout_ms = 0`，确认修复前命令可永久卡住。
- 修复后应在兜底时间内返回失败，并增加 timeout 统计。

## 重要问题

### P1-1：执行和错误处理逻辑重复

结论：确认成立。

重复路径：

`cmd_argv_sync()`：

- 检查 args。
- `get_conn()`。
- 构造 command。
- `redisCommandArgv()`。
- reply 空时补 `ctx->err`。
- 记录失败。
- parse reply。
- free reply。
- 记录结果。

`get()`：

- `get_conn()`。
- `redisCommand(ctx, "GET %s", key)`。
- reply 空时补 `ctx->err`。
- 记录失败。
- parse reply。
- free reply。
- 记录结果。

`do_cmd()`：

- `get_conn()`。
- `redisCommand(ctx, cmdline.c_str())`。
- reply 空时补 `ctx->err`。
- 记录失败。
- parse reply。
- free reply。
- 记录结果。

风险：

- 新增 running 检查时容易漏路径。
- 修改 nullptr 错误处理时容易漏路径。
- 改超时统计或错误分类时容易漏路径。
- 后续如果增加重试/退避，也会重复。

推荐修复：

抽取统一执行函数，例如：

```cpp
template <typename Fn>
Reply execute_redis_command(Fn&& fn) {
    if (!running_) return stopped_reply();

    redisContext* ctx = get_conn();
    if (!ctx) return no_connection_reply();

    redisReply* raw = fn(ctx);
    if (!raw) return handle_null_reply(ctx);

    RedisReplyPtr reply(raw, freeReplyObject);
    Reply r;
    parse_redis_reply(reply.get(), r);
    record_command_result(r, ctx);
    return r;
}
```

三个入口只保留命令构造差异：

```cpp
return execute_redis_command([&](redisContext* ctx) {
    return static_cast<redisReply*>(redisCommand(ctx, "GET %s", key));
});
```

注意：

- `freeReplyObject` 返回 `void`，RAII deleter 要匹配。
- 对 `redisCommandArgv`，`RedisCommandArgv command(args)` 必须活到调用结束。
- 对 varargs `cmd()`，仍建议逐步迁移到 `cmd_argv()`，减少整条命令字符串接口。

验证方式：

- 保持现有 RedisReply、RedisCommandArgv 测试通过。
- 增加 stopped/no connection/null reply 的单元或 mock 测试。

### P1-2：超时统计依赖字符串匹配

结论：确认成立。

证据：

```cpp
void record_command_failure(const std::string& err) {
    stats_.inc_cmd_fail();
    if (err.find("timeout") != std::string::npos ||
        err.find("Timeout") != std::string::npos ||
        err.find("timed out") != std::string::npos) {
        stats_.inc_timeout();
    }
}
```

问题：

- 依赖 hiredis / libc / OS 的错误文案。
- 不同平台、不同语言环境、不同 hiredis 版本可能不同。
- 可能误判业务错误字符串中包含 timeout 的情况。
- 对 `REDIS_ERR_IO`、`REDIS_ERR_EOF`、`REDIS_ERR_OTHER` 没有分类。

推荐修复：

把 `redisContext` 错误码传给记录函数：

```cpp
void record_command_failure(redisContext* ctx, const std::string& err) {
    stats_.inc_cmd_fail();
    if (ctx && ctx->err == REDIS_ERR_IO && errno == ETIMEDOUT) {
        stats_.inc_timeout();
    }
}
```

更稳妥：

- 在调用失败时立即读取 `ctx->err`、`ctx->errstr`、`errno`。
- 不要在后续逻辑中依赖全局 `errno`，因为它可能被其他函数调用覆盖。

建议定义错误快照：

```cpp
struct RedisErrorSnapshot {
    int redis_err = 0;
    int sys_errno = 0;
    std::string message;
};
```

然后：

```cpp
bool is_timeout(const RedisErrorSnapshot& e) {
    return e.sys_errno == ETIMEDOUT ||
           e.sys_errno == EAGAIN ||
           e.sys_errno == EWOULDBLOCK;
}
```

注意：

- `redisSetTimeout` 底层可能导致 `EAGAIN` / `EWOULDBLOCK`，不一定总是 `ETIMEDOUT`。
- macOS/Linux errno 表现可能略有差异。
- 业务 `REDIS_REPLY_ERROR` 不应该被当作连接错误或超时。

验证方式：

- 构造真实 Redis 命令超时，确认 timeout_total 增加。
- 构造 Redis 返回 `-ERR timeout something` 业务错误，确认不误计为网络 timeout。

### P1-3：连接数不可由 RedisPool 显式限制

结论：基本成立，但需要按当前 thread_local 模型理解。

当前连接数上界：

- 每个调用过 Redis 的线程最多一个 TLS 连接。
- 实际连接数约等于触达 Redis 的 `io_context` 线程数。
- 如果将来其他线程也调用 RedisPool，同样会创建该线程自己的 TLS 连接。

代码中没有：

- `max_connections`
- 全局连接计数硬上限
- 等待队列
- acquire/release

已有统计：

```cpp
created_total_
```

但它是累计创建次数，不是当前活动连接数。

风险：

- 如果应用 io_context 线程数过大，Redis 连接数也会增加。
- 如果多类线程都调用 RedisPool，会超出预期。
- Redis 服务端 `maxclients` 较低时可能连接失败。

为什么不容易在当前模型直接限流：

- TLS 连接不是集中池化资源。
- 当前线程调用时没有 acquire/release 生命周期。
- 跨线程统计当前活动 TLS 连接需要额外注册/注销机制。
- 真正严格限流会把模型推向共享池或 worker pool。

推荐修复：

短期：

- 文档明确连接数约等于触达 Redis 的线程数。
- 启动日志打印 Redis direct 模式连接模型和超时。
- 增加配置说明，不要把 `io_context` 线程数设到远超 Redis `maxclients` 的量级。

中期：

- worker 模式下使用固定 `worker_threads`，连接数约等于 worker 数。
- 或实现共享连接池 `max_connections`。

不建议：

- 在现有 direct/TLS 模式上硬加全局锁计数并阻塞等待，这会破坏模型简洁性，且难以处理 TLS 连接释放时机。

验证方式：

- 用多线程同时调用 RedisPool，观察 Redis `CLIENT LIST` 或 `INFO clients`。
- 验证连接数是否接近调用线程数。

### P1-4：`ioc_` 成员未使用

结论：确认成立。

证据：

构造函数：

```cpp
RedisPool(asio::io_context& ioc, Config cfg)
    : running_(true),
      ioc_(ioc),
      ...
```

成员：

```cpp
asio::io_context& ioc_;
```

后续没有使用 `ioc_`。

影响：

- 增加无效依赖。
- 让读者误以为 RedisPool 内部使用 `io_context` 做调度。
- 和实际 direct/TLS 同步模型不一致。

修复建议：

- 移除 `ioc_` 成员。
- 构造函数改为 `RedisPool(Config cfg)`。
- 或保留构造参数但不保存，以降低调用方改动。

如果未来增加 worker 模式：

- direct 模式仍不需要 `io_context`。
- worker 模式也可内部持有 `asio::thread_pool`，不需要外部 `io_context&`。

### P1-5：无空闲保活，首次调用可能失败

结论：现象成立，但默认后台 PING 不一定是正确修复。

当前实现：

- 没有 keepalive timer。
- 没有 idle-before-use ping。
- 命令失败后依赖 `ctx->err != 0`，下一次 `get_conn()` 重建。

仓库文档明确反对主动 keepalive PING：

```text
主动 PING 不仅不必要，而且在 Redis 服务 hang 住时会把 io_context 线程一起卡死。
```

风险：

- Redis 服务端断开 idle 连接后，下一次业务命令可能失败一次。
- 失败后下一次才重建。
- 对缓存读取路径，可能导致一次 cache miss 或接口降级。

权衡：

- 后台 PING 在 direct 模式下仍会运行在某个线程上，如果 Redis hang，也可能阻塞该线程。
- 如果为每个 TLS 连接做后台 PING，需要跨线程访问 TLS，这不现实。
- 如果在使用前 PING，会给冷连接路径增加一次额外 RTT。

推荐修复：

轻量方案：

- 在 `TlsRedisConn` 中记录 `last_used_at`。
- 如果连接 idle 超过阈值，例如 `idle_validate_sec`，下一次使用前执行一次 `PING`。
- `PING` 必须受 `cmd_timeout_ms` 约束。
- PING 失败则立即重建并执行原命令，或直接返回错误由下一次调用重建。

更可靠方案：

- worker 模式下可在 worker 线程内维护连接和 idle ping。
- direct 模式默认不启用后台 PING。

推荐默认：

- 不加后台保活。
- 可选 idle-before-use validate。
- 继续依赖命令路径断线检测和超时。

验证方式：

- Redis 配置较短 idle timeout，等待连接被断开。
- 下一次 GET 修复前失败一次，修复后可先 ping/reconnect 再执行，或至少错误分类明确。

## 中等问题和优化建议

### P2-1：命令注入接口风险

结论：评审中的区分基本正确，但需要细化。

当前安全接口：

```cpp
cmd_argv(std::vector<std::string> args)
cmd_argv_sync(std::vector<std::string> args)
```

这两个接口使用 `redisCommandArgv`，参数边界由 argv/len 明确提供，能安全承载空格、二进制和用户输入。

`get(const char* key)`：

```cpp
redisCommand(ctx, "GET %s", key)
```

hiredis 的格式化命令会把 `%s` 参数作为 bulk string 处理，不是简单拼接，所以用于 GET key 是合理的。

风险接口：

```cpp
cmd(const char* fmt, ...)
```

它先用 `vsnprintf` 格式化成整条命令字符串，再调用：

```cpp
redisCommand(ctx, cmdline.c_str())
```

如果上层把用户输入拼进 fmt 或拼成完整命令，可能产生命令边界问题。该接口已经标记：

```cpp
[[deprecated("Use cmd_argv() for commands carrying dynamic arguments")]]
```

建议：

- 新代码禁止使用 `cmd(fmt, ...)` 承载动态参数。
- 内部路由优先迁移到 `cmd_argv()` 或专用 fast path。
- 长期可以把 `cmd()` 限制为 private 或删除。

### P2-2：重连失败缺少退避

结论：成立。

当前行为：

`ensure_redis_tls_connection()` 中，如果没有连接或连接错误，每次调用都会尝试：

```cpp
tls.conn.reset(create_redis_connection(cfg, created_total));
```

如果 Redis 持续不可用：

- 每个请求都可能触发一次 connect。
- connect 有 timeout，默认配置 1000ms。
- 在 direct 模式下，这个 connect 也发生在 `io_context` 线程上。

风险：

- Redis 故障期间持续放大 connect 压力。
- 每个请求都阻塞当前 `io_context` 线程最多 connect timeout。
- 日志可能大量输出 connect failed。

推荐修复：

在 TLS 状态中增加：

```cpp
std::chrono::steady_clock::time_point next_reconnect_at;
std::string last_connect_error;
```

连接失败后设置冷却：

```cpp
next_reconnect_at = now + reconnect_backoff_ms;
```

冷却期内直接返回 `nullptr`，错误为：

```text
Redis reconnect cooling down: <last error>
```

配置：

```ini
[redis]
reconnect_backoff_ms = 200
reconnect_backoff_max_ms = 2000
```

注意：

- backoff 状态是 thread_local，每个线程独立。
- 这符合当前 TLS 模型。
- 不需要全局锁。

验证方式：

- Redis 停止后连续请求 `/api/redis`。
- 修复前 `created_total/connect_fail log` 高频增长。
- 修复后冷却期内快速失败，connect 尝试频率下降。

### P2-3：配置合法性校验不足

结论：成立。

当前配置解析：

```cpp
app.redis = RedisPool::Config{
    .host = cfg.get("redis", "host", "127.0.0.1"),
    .port = cfg.get_int("redis", "port", 6379),
    .connect_timeout_ms = cfg.get_int("redis", "connect_timeout_ms", 1000),
    .cmd_timeout_ms = cfg.get_int("redis", "cmd_timeout_ms", 1000)
};
```

构造函数也没有校验：

```cpp
RedisPool(asio::io_context& ioc, Config cfg)
```

`create_redis_connection()` 有局部 clamp：

```cpp
if (connect_ms < 100) connect_ms = 100;
if (cmd_ms < 100) cmd_ms = 100;
```

但问题仍包括：

- `host` 为空。
- `port <= 0` 或 `port > 65535`。
- `cmd_timeout_ms == 0` 被允许。
- `connect_timeout_ms <= 0` 被静默改成 100。
- 负 `cmd_timeout_ms` 会走“不设置命令超时”路径，因为 `cfg.cmd_timeout_ms > 0` 不成立。

尤其是负 `cmd_timeout_ms`：

- 这会等价于零超时。
- 风险比用户显式配置 0 更隐蔽。

推荐修复：

配置 normalize：

```cpp
if (cfg.host.empty()) throw std::invalid_argument("redis.host is empty");
if (cfg.port <= 0 || cfg.port > 65535) throw std::invalid_argument("invalid redis.port");
if (cfg.connect_timeout_ms <= 0) cfg.connect_timeout_ms = 1000;
if (cfg.cmd_timeout_ms <= 0) cfg.cmd_timeout_ms = 1000;
```

如果要保留性能模式：

```cpp
if (cfg.cmd_timeout_ms == 0 && !cfg.allow_no_cmd_timeout) {
    cfg.cmd_timeout_ms = 1000;
}
if (cfg.cmd_timeout_ms < 0) {
    cfg.cmd_timeout_ms = 1000;
}
```

建议：

- 负数绝不应表示性能模式。
- 0 是否允许需要显式策略。

验证方式：

- 配置测试覆盖 host 空、port 越界、timeout 负数、timeout 0。

### P2-4：可观测性不足

结论：成立。

已有 stats：

- `created_total`
- `reconnect_total`
- `cmd_ok_total`
- `cmd_fail_total`
- `nil_total`
- `timeout_total`

不足：

- 没有当前活跃 TLS 连接数。
- 没有 connect fail total。
- 没有 connect timeout total。
- 没有按错误码分类。
- 没有命令耗时。
- 没有直接模式下 io_context 阻塞耗时统计。
- 没有 reconnect backoff 命中次数。

建议新增：

- `connect_fail_total`
- `connect_timeout_total`
- `io_error_total`
- `reply_error_total`
- `stopped_total`
- `reconnect_suppressed_total`
- `cmd_duration_us_total`
- `cmd_duration_us_max`

高阶指标：

- p95/p99 需要 histogram，当前 stats 结构不适合直接精确统计。
- 可以先记录 max 和慢命令采样日志。

### P2-5：`cmd_argv()` coroutine 语义是同步执行

结论：需要文档明确。

`cmd_argv()` 是 awaitable，但内部只是：

```cpp
co_return cmd_argv_sync(std::move(args));
```

它不会让出到 worker 线程，也不会真正异步等待 Redis。调用方看到 `co_await redis.cmd_argv(...)`，可能误以为不会阻塞 `io_context`。

建议：

- 文档明确 `cmd_argv()` 是 coroutine 形式的同步 hiredis 包装。
- 如果新增 worker 模式，`cmd_argv()` 在 worker 模式下才会真正 `asio::post`。
- 可考虑命名或注释强调 direct mode blocking。

## 设计亮点确认

### TLS 无锁架构

结论：成立。

优点：

- 热路径无全局 mutex。
- 每个线程复用自己的 Redis TCP 连接。
- 避免了共享连接池 acquire/release 成本。
- 对低延迟小命令非常适合。

代价：

- 阻塞发生在 `io_context` 线程。
- 连接数跟线程数绑定。
- shutdown 无法跨线程主动释放 TLS。

### owner + generation 机制

结论：成立。

证据：

```cpp
generation_(next_generation_.fetch_add(1, std::memory_order_relaxed))
```

匹配函数：

```cpp
return tls.owner == owner && tls.generation == generation;
```

价值：

- 同一线程中如果 RedisPool 销毁后又创建新实例，不会因为地址复用错误地复用旧连接。
- 测试 `RedisTlsOwner.RequiresMatchingOwnerAndGeneration` 覆盖了基本语义。

限制：

- generation 不能替代 `running_` 检查。
- 对同一 pool shutdown 后继续调用，owner/generation 仍匹配，当前代码仍会执行命令。

### nullptr reply 边界处理

结论：成立。

当前代码处理了 hiredis 返回 nullptr 但 `ctx->err == 0` 的情况：

```cpp
if (ctx->err == 0) {
    ctx->err = REDIS_ERR_IO;
    snprintf(ctx->errstr, sizeof(ctx->errstr), "redisCommand returned nullptr (OOM or disconnect)");
}
```

优点：

- 避免失败路径没有错误信息。
- 确保下次 `get_conn()` 能通过 `ctx->err != 0` 重建。

改进：

- 该逻辑重复三次，建议抽取。
- 修改 hiredis context 的 `err` 字段属于务实处理，但应集中封装，避免未来误用。

### 参数化接口

结论：成立。

`RedisCommandArgv` 保留参数边界和长度：

```cpp
argv.push_back(arg.data());
argv_len.push_back(arg.size());
```

测试覆盖了：

```cpp
std::string("a\0b", 3)
```

说明接口能承载二进制内容和空格。

## 文档与实现不一致

### `cmd_timeout_ms` 默认语义存在多处表述

现状：

- `RedisPool::Config` 默认 `cmd_timeout_ms = 0`。
- `app_config_from()` 默认读取值为 `1000`。
- `config.d/11-redis.ini` 设置为 `500`。
- `docs/DB_POOL_DESIGN.md` Redis 配置示例为 `1000`，且强调必须设。
- `docs/PERF_REPORT.md` 曾建议压测/内网设 `0` 性能模式。
- `docs/CODE_DOC_REVIEW_2026-07-06.md` 认为 `cmd_timeout_ms = 0` 是性能模式，和压测文档一致。

建议统一：

生产默认：

```ini
cmd_timeout_ms = 500 或 1000
```

文档明确：

- `0` 只允许在压测或完全可信内网故障模型下使用。
- 生产不建议 0。
- 如果代码层面改为禁止 0，必须同步更新 PERF_REPORT 的历史建议。

### DB_POOL_DESIGN 中旧的 “gap” 表已过时

`docs/DB_POOL_DESIGN.md` 的 gap 表写：

- 当前使用 `redisConnect`。
- 当前未设置 `redisSetTimeout`。
- keepalive timer 存在。

但当前代码已经：

- 使用 `redisConnectWithTimeout`。
- `cmd_timeout_ms > 0` 时使用 `redisSetTimeout`。
- 没有 keepalive timer。

该表是历史残留，应更新或标注为历史问题。

## 推荐修复优先级

### 第一批：语义和安全阀

目标：不改架构，先修 shutdown 和永久阻塞风险。

建议包含：

1. 所有命令入口和 `get_conn()` 检查 `running_`。
2. `cmd_timeout_ms <= 0` 配置校验或兜底。
3. 负 timeout 不允许等价为无超时。
4. `record_command_failure` 使用错误码/errno，不依赖字符串。
5. 文档说明 shutdown 对 TLS 的语义。

理由：

- 改动小。
- 不破坏 direct/TLS 性能模型。
- 能处理评审中最明确的语义缺陷。

### 第二批：去重和错误分类

目标：降低维护成本，防止未来修复漏分支。

建议包含：

1. 抽取统一执行模板。
2. RAII 包装 `redisReply*`。
3. 统一 nullptr reply 处理。
4. 增加 connect fail、reply error、timeout 分类统计。
5. 为 `cmd_argv_sync()`、`get()`、`do_cmd()` 共享同一错误处理路径。

理由：

- 当前重复逻辑多，新增 running 检查和错误分类时容易漏。
- 抽取后再做 worker 模式更容易。

### 第三批：可选 worker 模式

目标：在不牺牲现有 direct 性能路径的前提下，提供生产故障隔离方案。

建议配置：

```ini
[redis]
mode = direct
worker_threads = 4
```

行为：

- direct：保持当前同步 TLS 快路径。
- worker：`cmd_argv()` / `get()` 通过 `asio::post` 投递到 Redis worker pool。
- worker 线程内部也可以使用 `thread_local redisContext`，连接数约等于 worker_threads。

注意：

- `cmd_argv_sync()` 在 worker 模式下仍是同步函数，应该只允许 worker 内部或明确调用方风险。
- public API 可能需要区分 async 和 sync。
- `/api/combo` 中后台写缓存如果直接调 sync，需要确认不会跑在 io_context 线程。

### 第四批：连接治理和可观测性

建议包含：

1. reconnect backoff。
2. idle-before-use validate 可选配置。
3. 文档说明连接数模型。
4. stats 增强。
5. 慢命令采样日志。

## 推荐修复设计草案

### running 检查

同步 helper：

```cpp
bool is_running() const {
    return running_.load(std::memory_order_acquire);
}
```

同步错误：

```cpp
Reply stopped_reply() {
    stats_.inc_cmd_fail();
    return Reply{false, "Redis pool stopped", "", 0};
}
```

入口：

```cpp
asio::awaitable<Reply> get(const char* key) {
    if (!is_running()) {
        co_return stopped_reply();
    }
    ...
}
```

`get_conn()`：

```cpp
redisContext* get_conn() {
    if (!is_running()) return nullptr;
    ...
}
```

### 配置校验

建议在构造函数开始 normalize：

```cpp
static Config normalize_config(Config cfg) {
    if (cfg.host.empty()) {
        throw std::invalid_argument("redis.host is empty");
    }
    if (cfg.port <= 0 || cfg.port > 65535) {
        throw std::invalid_argument("redis.port out of range");
    }
    if (cfg.connect_timeout_ms <= 0) {
        cfg.connect_timeout_ms = 1000;
    }
    if (cfg.cmd_timeout_ms <= 0) {
        cfg.cmd_timeout_ms = 1000;
    }
    return cfg;
}
```

如果保留性能模式：

```cpp
bool allow_no_cmd_timeout = false;
```

并只在显式允许时接受 0。

### 统一执行模板

示意：

```cpp
using RedisReplyPtr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

template <typename Fn>
Reply execute_sync(Fn&& fn) {
    if (!is_running()) return stopped_reply();

    redisContext* ctx = get_conn();
    if (!ctx) {
        stats_.inc_cmd_fail();
        return Reply{false, "no Redis connection", "", 0};
    }

    redisReply* raw = fn(ctx);
    if (!raw) {
        return handle_null_reply(ctx);
    }

    RedisReplyPtr reply(raw, freeReplyObject);
    Reply r;
    parse_redis_reply(reply.get(), r);
    record_command_result(ctx, r);
    return r;
}
```

注意：

- `freeReplyObject` 的函数指针类型可能需要 cast 或自定义 deleter。
- `record_command_result` 对 Redis `ERROR` reply 应计为 command failure，但不应标记连接断开。

### 错误快照

```cpp
struct RedisErrorSnapshot {
    int redis_err = 0;
    int sys_errno = 0;
    std::string message;
};
```

失败时立即捕获：

```cpp
RedisErrorSnapshot snapshot_error(redisContext* ctx) {
    return {
        .redis_err = ctx ? ctx->err : 0,
        .sys_errno = errno,
        .message = ctx ? ctx->errstr : "no Redis context"
    };
}
```

然后分类：

```cpp
bool is_timeout_error(const RedisErrorSnapshot& e) {
    return e.sys_errno == ETIMEDOUT ||
           e.sys_errno == EAGAIN ||
           e.sys_errno == EWOULDBLOCK;
}
```

## 测试计划

### 单元测试

建议新增：

1. `RedisPool` shutdown 后 `cmd_argv_sync()` 返回 stopped。
2. `RedisPool` shutdown 后 `get()` 返回 stopped。
3. `RedisPool` shutdown 后不创建新连接。
4. 配置 `cmd_timeout_ms < 0` 会 normalize 或报错。
5. 配置 `cmd_timeout_ms = 0` 的行为符合新策略。
6. timeout 分类不依赖错误字符串。
7. `RedisCommandArgv` 现有二进制参数测试继续保留。

难点：

- 当前 RedisPool 直接依赖 hiredis C API，不容易 mock。
- 可以先把配置 normalize、错误分类、reply 解析做成纯函数测试。

### 集成测试

需要真实 Redis。

1. 正常 GET/SET：
   - `SET key value`
   - `GET key`
   - 验证 ok 和 stats。

2. shutdown 后拒绝命令：
   - 创建 pool。
   - 执行一次命令。
   - shutdown。
   - 再执行命令。
   - 验证不再执行 Redis 操作。

3. 命令超时：
   - 通过 Redis/网络代理制造阻塞。
   - 设置 `cmd_timeout_ms = 100`。
   - 验证命令在合理时间失败，`timeout_total` 增加。

4. 重连：
   - 建立连接。
   - 停止 Redis 或 kill client。
   - 下一次命令失败。
   - 再恢复 Redis。
   - 后续命令能重建成功，`reconnect_total` 增加。

5. worker/direct 对比：
   - 如果实现 worker 模式，故障 Redis 下 `/api/health` p99 应明显比 direct 更稳定。

### 压测验证

修复 direct 模式后：

```bash
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure
./build.sh
```

Redis 压测：

```bash
wrk/plow /api/redis
wrk/plow /api/combo
```

重点观察：

- Redis RPS 是否明显回退。
- `/api/health` 是否受 Redis 压测影响。
- `cmd_fail_total`
- `timeout_total`
- `reconnect_total`
- server log 是否出现大量 connect failed。

故障注入：

- Redis 停止。
- Redis hang。
- 网络黑洞。
- Redis 恢复。

观察 direct 模式下 `cmd_timeout_ms` 是否真正限制阻塞上限。

## 风险评估

### 禁止 `cmd_timeout_ms = 0` 的性能风险

设置 `redisSetTimeout` 会给 socket 设置超时。正常情况下这不应显著影响命令性能，但历史文档中把 `cmd_timeout_ms = 0` 称为性能模式，说明曾经对性能敏感。

建议：

- 不要直接在同一 PR 中同时改架构和改超时默认。
- 先用当前 `cmd_timeout_ms = 500` 跑基线。
- 如果禁止 0，压测确认 Redis RPS 回退在可接受范围。

### worker 模式的性能风险

worker 模式会增加：

- `asio::post` 开销。
- 跨线程恢复协程开销。
- worker 线程上下文切换。
- 如果使用共享连接池，还会增加锁竞争。

因此建议 worker 模式可选，不替换 direct 默认。

### idle PING 的风险

后台 PING 可能：

- 在 Redis hang 时阻塞执行 PING 的线程。
- 增加额外流量。
- 和 TLS 模型不匹配。

更推荐：

- 使用前按 idle 时间可选校验。
- worker 模式再考虑后台维护。

## 最小修复集建议

如果只做一次小修：

1. 所有入口检查 `running_`。
2. `get_conn()` 检查 `running_`。
3. `cmd_timeout_ms <= 0` 做兜底或拒绝。
4. 负 timeout 不允许变成无超时。
5. 补充 shutdown/TLS 注释。

如果允许中等修复：

6. 抽取统一执行模板。
7. RAII 管理 `redisReply*`。
8. 错误分类改为错误码/errno。
9. 增加 connect fail 和 timeout stats。
10. 增加 reconnect backoff。

如果允许完整修复：

11. 增加 `mode = direct | worker`。
12. worker 模式下使用 Redis 专用 thread pool。
13. 文档同步连接数模型和故障隔离策略。
14. 增加故障注入集成测试。

## 待确认问题

以下问题需要通过压测或故障注入确认：

1. `cmd_timeout_ms = 500` 对 Redis RPS 的实际影响。
   - 当前配置已经是 500，但需要和 0 做同环境对比。

2. Redis 故障时 direct 模式对 `/api/health` 的影响范围。
   - 理论上阻塞当前 `io_context` 线程。
   - 实际影响取决于线程数、请求分布、Redis 路由占比。

3. hiredis 超时失败时 errno 的具体表现。
   - 可能是 `EAGAIN`、`EWOULDBLOCK`、`ETIMEDOUT`。
   - 需要在 macOS/Linux 都确认。

4. worker 模式的性能回退。
   - 需要和 direct 模式同机、同构建、同 Redis 配置压测。

5. 空闲连接被服务端关闭后的首次失败概率。
   - 如果 Redis 服务端 idle timeout 较长，实际影响可能很低。
   - 如果中间设备会清 idle TCP，影响更明显。

## 最终建议

评审报告中的核心风险基本成立，但需要按当前仓库的性能设计重新定级：

- “同步 Redis 阻塞 io_context”是事实，也是当前有意选择的 direct/TLS 模式。建议不要直接删除该模式，而是通过强制命令超时和可选 worker 模式解决可靠性诉求。
- “shutdown 后仍可执行命令”是明确缺陷，应优先修。
- “零超时永久阻塞”是明确风险。当前配置文件不是 0，但代码允许 0，且历史文档存在性能模式建议，应统一策略。
- “错误处理重复、字符串识别 timeout、ioc_ 冗余、配置校验不足”都成立，适合第二批清理。
- “无 keepalive 首次失败”成立，但默认后台 PING 不是最佳修复；更适合做可选 idle-before-use 校验或 worker 模式维护。

推荐路线：

1. 保留 direct/TLS 架构。
2. 先修 shutdown、超时兜底、配置校验、错误分类。
3. 再抽取统一执行模板。
4. 最后新增 worker 模式作为生产故障隔离选项。

每批修复都应配套 `ctest`、Redis 集成测试和一次 Redis 故障注入验证。
