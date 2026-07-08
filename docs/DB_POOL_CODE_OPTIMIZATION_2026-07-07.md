# MySQL / Redis 连接池代码结构优化方案

> 日期：2026-07-07
> 范围：`src/db/mysql_pool.hpp`、`src/db/redis_pool.hpp` 及其调用路径
> 目标：提升正确性、可维护性、可观测性；不破坏当前压测表现和既有架构约束

## 结论

当前 MySQL / Redis 连接池的核心架构是合理的：

- MySQL 使用 `asio::thread_pool` 承接同步 `mysql_query`，没有阻塞 `io_context`。
- Redis 使用 `thread_local redisContext*`，无全局锁，热路径短。
- 两者都已经有基本超时和断线重建能力。

不建议立即做大架构替换。优化应按“先正确性、再结构、最后性能”的顺序推进。

## 不应破坏的约束

- MySQL 同步 libmysqlclient 调用必须继续在 worker pool 内执行，不能迁回 `io_context`。
- Redis 当前性能依赖 thread-local 单连接快路径，不能改成全局带锁连接池。
- 日志仍走异步 spdlog，不要在热路径加高频同步日志。
- 配置语义保持兼容：`mysql.worker_threads` 控制 SQL worker，`redis.cmd_timeout_ms` 控制 hiredis 命令读写超时。

## 当前代码结构问题

### MySQL

`MysqlPool` 当前把以下职责集中在一个 header class 中：

- 生命周期：构造、预创建、shutdown。
- 池管理：`acquire()`、`release()`、`total_`、`creating_`、`idle_pool_`。
- 查询执行：`execute()`、`do_query()`。
- 结果序列化：MySQL row 到 JSON。
- 后台维护：`maintain_loop()`、idle 回收、min_size 补充、ping 检查。
- 底层连接：`create_connection_with_timeout()`、`mysql_ping_with_timeout()`。

问题不是功能错误，而是函数职责过宽，后续加 metrics、测试和错误分类会越来越难。

### Redis

`RedisPool` 代码较短，但也混合了：

- 命令格式化：`cmd(const char* fmt, ...)`。
- 连接生命周期：thread-local 创建、断线重建、shutdown。
- reply 解析：`parse_reply()`。
- 快速路径：`get()`。

主要问题是 `cmd()` 的字符串格式化和 `thread_local` owner 语义不够清晰。

## P1：正确性优先优化

### 1. MySQL `execute()` 不应静默截断 SQL

当前位置：`src/db/mysql_pool.hpp`

当前逻辑：

```cpp
char sql_buf[4096];
size_t len = sql.size();
if (len > sizeof(sql_buf) - 1) len = sizeof(sql_buf) - 1;
std::memcpy(sql_buf, sql.data(), len);
sql_buf[len] = '\0';
```

问题：

- SQL 超过 4095 字节会被静默截断。
- 截断后的 SQL 可能语义变化，最坏情况下执行了调用方完全没预期的查询。

建议：

- 保留固定栈 buffer 的性能设计。
- 超长 SQL 直接返回错误，不执行。

建议语义：

```cpp
if (sql.size() >= sizeof(sql_buf)) {
    co_return Result{false, "sql too long", ""};
}
```

本轮不设计 `execute_owned(std::string sql)` 之类长 SQL API。

原因：

- 项目约束要求不要让 `std::string` 跨 `asio::post` 边界进入 MySQL worker pool。
- 历史事故中，SQL 字符串跨 worker 边界曾引发内存生命周期问题。
- 4096 字节足够当前 `/api/mysql` 和内部健康查询场景。超长 SQL 直接拒绝，比新增高风险 owned 路径更稳妥。

如果未来确实需要长 SQL，必须另起设计文档，先证明所有权模型、worker 边界和历史 double-free 根因不会复现；不能在本轮顺手加入。

### 2. MySQL JSON 序列化应使用 `mysql_fetch_lengths()`

当前位置：`MysqlPool::do_query()`

当前问题：

- field name 直接拼接，未 escape；列别名如果包含 `"`、`\` 或控制字符，会破坏 JSON。
- `row[i] == nullptr` 被输出成字符串 `"NULL"`，不是 JSON `null`。
- 字段值按 C 字符串遍历，遇到 `\0` 会截断。
- value 只转义了 `"` 和 `\`，没有处理控制字符，如 `\n`、`\r`、`\t`、`0x00-0x1f`。

建议：

- 每行调用 `mysql_fetch_lengths(mr)`。
- field name 使用 `MYSQL_FIELD::name_length` 驱动 escape，不假定 null-terminated。
- `row[i] == nullptr` 输出 `null`。
- field name 和 value 都使用长度驱动 JSON escape。

建议在 P1 阶段直接拆到独立文件，避免 P1 先 inline 拆函数、P2 再移动同一块代码造成重复改动：

```cpp
// src/db/mysql_result_json.hpp
inline void append_json_string(std::string& out, const char* data, unsigned long len);
inline std::string mysql_result_to_json(MYSQL_RES* mr);
```

收益：

- 修复 NULL / 二进制 / 控制字符边界。
- `do_query()` 从“查询 + 序列化”变成只负责编排。

### 3. MySQL `mysql_store_result()` 失败语义要区分

当前逻辑：

```cpp
MYSQL_RES* mr = mysql_store_result(conn);
release(conn);

if (!mr) return {true, "", "[]"};
```

问题：

- 对 `SELECT`，`mysql_store_result()` 返回 `nullptr` 可能是错误。
- 对 `INSERT/UPDATE` 或不返回结果集的语句，`nullptr` 是正常。

建议：

```cpp
MYSQL_RES* mr = mysql_store_result(conn);
// release must happen after field_count/error classification.
if (!mr) {
    if (mysql_field_count(conn) != 0) {
        // query should have returned rows but failed
        std::string err = mysql_error(conn);
        drop_bad_connection(conn);
        return {false, err, ""};
    }
    release(conn);
    return {true, "", "[]"};
}
release(conn);
```

决策：

- `mysql_store_result()` 失败时，如果 `mysql_field_count(conn) != 0`，说明查询预期有结果集却获取失败。
- 失败原因可能是网络断开、服务端错误、结果读取中断等。为了避免复用状态不明确的连接，出错一律 `drop_bad_connection(conn)`。
- 不在本阶段区分“语法错误后连接是否仍可复用”这类细粒度场景，优先保证连接池安全。
- 实现时必须先移除当前 `mysql_store_result(conn); release(conn); if (!mr)` 的顺序，避免连接已经回到 idle pool 后再次 drop/release。

### 4. Redis `cmd()` 应避免格式化整条命令字符串

当前位置：`src/db/redis_pool.hpp`

当前逻辑：

- `cmd(fmt, ...)` 使用 `vsnprintf` 生成一整条命令。
- 再调用 `redisCommand(ctx, cmdline.c_str())`。

问题：

- 带空格、换行、二进制内容的 value 不安全。
- 格式字符串使用不当会造成命令语义变化。
- 未来如果传用户输入，风险更高。

建议：

- 保留 `get(const char*)` 快速路径。
- 新增安全 API：

```cpp
asio::awaitable<Reply> cmd_argv(std::vector<std::string> args);
```

内部使用 `redisCommandArgv()`。

说明：

- `cmd_argv` 参数使用 owning `std::vector<std::string>`，避免 coroutine awaitable 被保存时 `initializer_list<string_view>` 悬空。
- Redis 命令仍在 `io_context` 线程同步执行，不跨 worker pool。

示例：

```cpp
co_await redis->cmd_argv({"SET", "cache:user:1", data});
co_await redis->cmd_argv({"EXPIRE", "cache:user:1", "300"});
```

这是当前 `/api/combo` 的真实 bug，不只是防御性改进：`data` 来自 MySQL 查询结果，如果包含空格、换行或引号，`SET cache:user:1 %s` 会被拆成错误命令。

旧 `cmd(fmt, ...)` 可保留给固定内部命令，但应进入迁移期：

```cpp
[[deprecated("Use cmd_argv() for all commands carrying dynamic arguments")]]
asio::awaitable<Reply> cmd(const char* fmt, ...);
```

迁移原则：

- 新代码一律使用 `cmd_argv()`。
- 旧 `cmd(fmt, ...)` 只允许固定命令或纯内部常量。
- `/api/combo` 的 `SET cache:user:1 %s` 必须在 P1 阶段优先迁移，因为 `data` 是动态值。

### 5. Redis thread-local 连接需要 owner / generation

当前字段：

```cpp
inline static thread_local RedisPtr tls_conn_{nullptr, redisFree};
```

问题：

- `tls_conn_` 是 class 级 thread-local，所有 `RedisPool` 实例共享。
- 当前应用是单实例，正常。
- 但测试、多实例、reload 或重建 RedisPool 时，可能复用旧配置的连接。

建议（已实现）：

给 TLS 包一层状态，并同时记录 `generation`，避免 `RedisPool*` 地址复用时误判 owner 匹配：

```cpp
struct TlsRedisConn {
    const RedisPool* owner = nullptr;
    uint64_t generation = 0;
    RedisPtr conn{nullptr, redisFree};
};
inline static thread_local TlsRedisConn tls_;
```

`get_conn()` 中检查：

```cpp
if (tls_.owner != this || tls_.generation != generation_) {
    tls_.conn.reset();
    tls_.owner = this;
    tls_.generation = generation_;
}
```

这样不会破坏 thread-local 快路径，也能消除多实例和地址复用隐患。实现落点：

- `src/db/redis_connection.hpp` / `.cpp`：`TlsRedisConn`、`redis_tls_owner_matches()`、`reset_redis_tls_owner()`。
- `src/db/redis_pool.hpp`：`generation_`、`next_generation_`、owner/generation 匹配后才复用 TLS 连接。

## P2：结构拆分优化

### MySQL 建议拆分方向

保持对外 API 不变：

```cpp
class MysqlPool {
public:
    asio::awaitable<Result> execute(const std::string& sql);
};
```

内部按职责拆：

| 文件 | 职责 |
|:-----|:-----|
| `mysql_pool.hpp` | public API、Config、Result、池状态 |
| `mysql_connection.hpp` | `MYSQL*` RAII、connect、close、reset、ping |
| `mysql_result_json.hpp` | `MYSQL_RES*` 到 JSON，包含严格 escape；该文件应在 P1 阶段直接落地 |
| `mysql_pool_stats.hpp` | counters、snapshot、日志格式化 |

如果暂时不想拆文件，至少先拆函数：

- `copy_sql_to_stack_buffer_or_error()`
- `execute_on_worker()`
- `query_mysql()`
- `mysql_result_to_json()`
- `drop_bad_connection()`
- `return_idle_connection()`

### Redis 建议拆分方向

| 文件 | 职责 |
|:-----|:-----|
| `redis_pool.hpp` | public API、Config、Reply |
| `redis_connection.hpp` | thread-local owner、connect、reconnect、timeout |
| `redis_reply.hpp` | `redisReply` 解析 |
| `redis_command.hpp` | `cmd_argv` 参数构造 |

短期可以只做函数拆分：

- `format_command_for_legacy_cmd()`
- `execute_command_string()`
- `execute_command_argv()`
- `parse_reply()`
- `ensure_tls_connection()`

## P3：性能优化

### 1. MySQL 扩容速度

当前 `creating_ > 0` 时其他 worker 等待，最多一个线程建连。

优点：

- 防止数据库故障时 thundering herd。

缺点：

- 冷启动或连接全断时恢复较慢。

建议（已实现）：

增加配置：

```ini
[mysql]
max_creating = 2
```

默认保守值：

```cpp
// MysqlPool::Config::max_creating == 0 表示自动推导
max_creating = std::min({
    static_cast<size_t>(4),
    std::max<size_t>(1, cfg_.max_size / 8),
    std::max<size_t>(1, cfg_.worker_threads / 2)
});
```

只有当 `total_ < max_size` 且 `creating_ < max_creating` 时才并发建连。

实现语义：

- `MysqlPool::Config::max_creating` 默认为 `0`，表示按上述公式自动推导。
- 配置显式设置 `max_creating > 0` 时，以配置值为准，并限制不超过 `max_size`。

原因：

- `max_size=8` 时默认仍为 1，保持当前保守行为。
- `max_size=64` 且 `worker_threads>=8` 时默认最多 4。
- 如果 `worker_threads=4`，默认最多 2 个 worker 同时建连，避免所有 worker 都卡在建连上导致正常查询排队。

### 2. MySQL maintain ping 策略

当前 maintain 每轮最多检查 4 条 idle 连接。

问题：

- 这部分和 acquire 路径的 `mysql_reset_connection()` 有一定重叠。
- 当前默认 `keepalive_sec=30`，每轮最多 ping 4 条，频率并不高。

建议：

- 保留 idle 回收和 min_size refill。
- 本轮不建议改 ping 频率，避免引入额外变量。
- 如果未来压测证明 maintain ping 有开销，再考虑每 4 轮 ping 一次，或只在最近查询失败率升高时触发 ping。

### 3. Redis 故障时阻塞 io_context 的权衡

当前 Redis 命令在 `io_context` 线程同步执行。

优点：

- 快路径极短，无 post、无线程切换、无锁。

风险：

- Redis 故障时，一个命令最多阻塞当前 io_context 线程 `cmd_timeout_ms`。

建议：

- 当前压测 Redis RPS 较高，不建议立刻改线程池。
- 如果生产更关注故障隔离，可增加可选模式：

```ini
[redis]
mode = direct | worker
worker_threads = 4
```

默认仍用 `direct`。

## P4：可观测性优化

### MySQL stats

建议增加 counters：

- `query_ok_total`
- `query_fail_total`
- `connect_ok_total`
- `connect_fail_total`
- `reset_conn_fail_total`（`mysql_reset_connection()` 失败）
- `acquire_wait_total`
- `acquire_timeout_total`（如果未来加 acquire timeout）
- `acquire_retry_exhausted_total`
- `idle_recycled_total`
- `ping_fail_total`

提供：

```cpp
std::string stats() const;
MysqlPoolStats snapshot() const;
```

### Redis stats

当前 `total_conns_` 只增不减，更像 created total，不是当前连接数。

建议改名或新增：

- `created_total`
- `reconnect_total`
- `cmd_ok_total`
- `cmd_fail_total`
- `nil_total`
- `timeout_total`

`total_conns_` 如果保留，应改名为 `created_total_`，避免误读。

## P5：测试补齐

### MySQL 单测

当前 `test_mysql_pool.cpp` 是 placeholder。建议优先补纯函数测试：

- SQL 超长拒绝。
- JSON escape：`"`、`\`、`\n`、`\r`、`\t`、控制字符。
- `NULL` 输出 JSON `null`。
- `mysql_store_result == nullptr && mysql_field_count != 0` 返回错误。

其中 JSON escape 可以完全脱离真实 MySQL 测。

### Redis 单测

建议补：

- `parse_reply()` 对 string/status/integer/nil/error/array 的解析。
- `cmd_argv` 参数含空格时不破坏命令边界。
- TLS owner 变更时重建连接。

如果不想引入 mock hiredis，可先把 reply 解析拆成纯函数后测试。

## 建议实施顺序

### 阶段 1：低风险正确性修复

1. MySQL SQL 超长拒绝，不再静默截断。
2. MySQL JSON escape 直接拆到 `mysql_result_json.hpp`，使用 `mysql_fetch_lengths()` 长度驱动。
3. MySQL `mysql_store_result()` 失败语义区分；先修 release 顺序，预期结果集读取失败时一律 drop bad connection。
4. Redis TLS owner 校验。
5. 新增 Redis `cmd_argv()`，并立刻迁移 `/api/combo` 的 `SET` / `EXPIRE`。

### 阶段 2：Redis 安全命令 API

1. 旧 `cmd(fmt, ...)` 加 `[[deprecated]]`，限制为内部固定命令。
2. 搜索并迁移其他动态参数调用点。
3. 后续版本逐步删除旧 API。

### 阶段 3：结构拆分

1. 先拆纯函数，不改 public API。
2. 再按文件拆 `mysql_result_json.hpp`、`redis_reply.hpp`。
3. 最后考虑 `.cpp`，降低 header-only 编译成本。

### 阶段 4：性能和 metrics

1. MySQL `max_creating` 可配置。
2. maintain ping 暂不改；仅在压测证明有开销时再降频。
3. 增加 `stats()` 和 counters。
4. 跑 `THREADS=30 bash bench/bench_full.sh` 对比。

## 不建议现在做的事

- 不建议把 Redis 改成全局锁池。
- 不建议把 MySQL 查询放回 `io_context`。
- 不建议默认给 MySQL 查询设置强 read timeout，容易误杀慢查询；如需查询超时，应单独设计 `query_timeout_ms`。
- 不建议在热路径打高频 INFO 日志，metrics 更适合。

## 验收标准

- `cmake --build build_codex_stage0` 通过。
- `ctest --test-dir build_codex_stage0 --output-on-failure` 通过。
- ASan+UBSan 单测通过。
- `/api/mysql`、`/api/redis`、`/api/combo` 功能正常。
- `THREADS=30 bash bench/bench_full.sh` 中 Redis/MySQL RPS 允许相对当前基线回退不超过 10%。
- 新增正确性测试覆盖 SQL 超长、JSON escape、Redis argv 参数边界、TLS owner 重建。
