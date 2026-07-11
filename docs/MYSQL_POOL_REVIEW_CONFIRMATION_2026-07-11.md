# MySQL 连接池代码评审确认报告

日期：2026-07-11

范围：

- `src/db/mysql_pool.hpp`
- `src/db/mysql_connection.cpp`
- `src/db/mysql_connection.hpp`
- `src/db/mysql_pool_stats.hpp`
- `src/db/mysql_pool_stats.cpp`
- `src/app/app_config.hpp`
- `config.d/10-mysql.ini`
- `tests/test_mysql_pool.cpp`
- `tests/test_config_load.cpp`
- `docs/DB_POOL_DESIGN.md`
- `docs/PERF_REPORT.md`
- `docs/GATEWAY_DESIGN.md`

本报告只做问题确认和修复设计，不包含源码改动。

## 结论总览

当前 MySQL 连接池整体架构方向是合理的：

- MySQL 同步阻塞 API 被放入 `asio::thread_pool`，没有阻塞 HTTP `io_context`。
- 空闲连接池、`total_`、`creating_` 使用同一把锁保护，容量控制模型清晰。
- 建连、关闭、ping 等重操作基本在锁外执行。
- `creating_` 预占名额能限制突发并发建连。
- maintain 线程独立于业务 IO 线程，shutdown 使用 `cv_.notify_all()` 唤醒维护线程。

但当前实现仍存在几个会影响生产正确性的关键问题。最需要优先处理的是：

1. 空闲连接复用时没有 `mysql_reset_connection()`，会造成会话状态泄漏。
2. `MYSQL_OPT_READ_TIMEOUT` 在连接建立后动态设置，查询和 ping 超时语义不可靠。
3. `MYSQL_RES*` 和连接释放顺序存在风险，当前代码会先归还连接再处理结果集。
4. SQL 被硬编码限制为 4095 字节。
5. acquire 无等待超时，池满时 worker 可能无限阻塞。
6. SQL 业务错误和连接级错误没有区分，导致不必要的连接销毁。
7. `execute()` 未在入口检查 shutdown 状态。

这些问题里，1 和 3 是直接的功能正确性风险，应当优先修复。

## 严重问题

### P0-1：复用连接未做会话重置

结论：确认成立。

证据：

`src/db/mysql_pool.hpp` 的 `acquire()` 中，空闲连接路径如下：

```cpp
if (!idle_pool_.empty()) {
    auto conn = idle_pool_.front().conn;
    idle_pool_.pop_front();
    return conn;
}
```

也就是说，从 `idle_pool_` 取出的复用连接会直接返回给新请求，没有执行 `mysql_reset_connection()`。

当前只有新建连接路径会执行 reset：

```cpp
// new connection needs session reset (rollback any pending transactions)
if (mysql_reset_connection(conn) != 0) {
    ...
}
```

问题本质：

- 新连接通常没有上一个业务请求留下的 session 状态。
- 复用连接才可能带有上一次请求残留的事务、变量、临时表、字符集、SQL mode 等状态。
- 当前逻辑把 reset 放在新连接路径，而没有放在复用路径，和连接池复用语义相反。

风险：

- 上一个请求如果执行了 `START TRANSACTION` 但没有提交或回滚，下一个请求可能继承未结束事务。
- 上一个请求如果修改了 `autocommit`、`sql_mode`、`time_zone`、字符集、隔离级别等 session 变量，下一个请求可能被污染。
- 临时表、用户变量、prepared statement、锁状态等会跨请求残留。
- 未提交事务可能长期持锁，导致业务慢查询、死锁或连接池耗尽。
- 这种问题通常不稳定复现，容易被误判为 MySQL、ORM 或业务代码问题。

推荐修复：

- `acquire()` 从 `idle_pool_` 取出连接后，在锁外执行 `mysql_reset_connection()`。
- reset 成功才返回连接。
- reset 失败则关闭连接、减少 `total_`、通知等待线程，并进入下一轮 acquire 重试。
- 新连接路径可以不 reset，或者为了统一路径保留一次 reset，但它不是关键保护点。

建议伪代码：

```cpp
MYSQL* acquire() {
    for (...) {
        MYSQL* conn = nullptr;
        {
            std::unique_lock lock(mtx_);
            if (!idle_pool_.empty()) {
                conn = idle_pool_.front().conn;
                idle_pool_.pop_front();
            }
        }

        if (conn) {
            if (mysql_reset_connection(conn) == 0) {
                return conn;
            }
            drop_bad_connection(conn);
            continue;
        }

        // create new connection path...
    }
}
```

注意事项：

- reset 不能在持有 `mtx_` 时执行，否则 MySQL 网络阻塞会扩大锁竞争。
- reset 失败必须 `drop_bad_connection`，不能放回池。
- reset 失败计数应复用现有 `reset_conn_fail_total`。
- 如果后续增加 `ConnectionGuard`，reset 仍应发生在 guard 创建前，确保拿到的连接已处于干净状态。

验证方式：

需要真实 MySQL 集成测试，建议增加：

1. 第一次请求执行 `SET @x = 123`，释放连接。
2. 第二次请求执行 `SELECT @x`。
3. 修复前可能返回 `123`，修复后应返回 `NULL`。

也可以测试事务状态：

1. 第一次请求关闭 autocommit 或开启事务后故意不提交。
2. 第二次请求检查 `@@autocommit` 或尝试查询受锁影响的行。
3. 修复后不应继承上一次请求状态。

### P0-2：先归还连接再处理结果集，存在并发复用风险

结论：确认成立，且比原评审报告中的 RAII 问题更具体、更严重。

证据：

`do_query()` 中结果集路径如下：

```cpp
clear_query_timeout(conn);
release(conn);
std::string json = mysql_result_to_json(mr);
mysql_free_result(mr);
stats_.inc_query_ok();
return {true, "", std::move(json)};
```

问题是连接被 `release(conn)` 放回 `idle_pool_` 后，其他 worker 线程可以立即从池中取走同一个 `MYSQL*` 并执行新的 `mysql_query()`。与此同时，当前线程还在使用这个连接产生的 `MYSQL_RES* mr` 做 `mysql_result_to_json(mr)` 和 `mysql_free_result(mr)`。

风险：

- `MYSQL_RES*` 的生命周期和所属连接的后续操作发生交错。
- 如果 libmysqlclient 的结果集实现仍依赖连接内部状态，可能产生未定义行为。
- 即使当前 MySQL 客户端版本在 `mysql_store_result()` 后把结果完整缓存在客户端，也不应依赖这种隐含行为来允许连接提前复用。
- 如果 `mysql_result_to_json(mr)` 耗时较长，连接会被过早暴露给其他请求，增加并发风险。

推荐修复：

处理顺序应改为：

1. `mysql_store_result(conn)`
2. 如果有结果集，先转换 JSON
3. `mysql_free_result(mr)`
4. 清理或恢复必要状态
5. 归还连接

建议配合 RAII：

```cpp
using MysqlResultPtr = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;

MYSQL_RES* raw = mysql_store_result(conn);
MysqlResultPtr result(raw, mysql_free_result);

std::string json = mysql_result_to_json(result.get());
release(conn);
return {true, "", std::move(json)};
```

更完整的做法是 `ConnectionGuard`：

```cpp
class ConnectionGuard {
public:
    ~ConnectionGuard() {
        if (!conn_) return;
        if (bad_) pool_.drop_bad_connection(conn_);
        else pool_.release(conn_);
    }

    void mark_bad() { bad_ = true; }
    MYSQL* get() const { return conn_; }

private:
    MysqlPool& pool_;
    MYSQL* conn_;
    bool bad_ = false;
};
```

这样即使 JSON 转换抛异常，连接也能被正确释放或销毁。

验证方式：

- 单元层面可将结果转换函数 mock 成抛异常，验证连接计数不泄漏。
- 集成层面可在大结果集查询同时压测小查询，观察是否出现 sporadic MySQL client error。
- ASAN/TSAN 对 libmysqlclient 内部未必完全有效，但仍建议开启压力测试。

### P0-3：异常安全不足，`MYSQL_RES*` 和 `MYSQL*` 资源可能泄漏

结论：确认成立。

证据：

当前代码直接使用裸指针管理：

- `MYSQL* conn = acquire();`
- `MYSQL_RES* mr = mysql_store_result(conn);`
- `mysql_free_result(mr);`
- 多个分支手动 `release(conn)` 或 `drop_bad_connection(conn)`。

典型风险路径：

```cpp
std::string json = mysql_result_to_json(mr);
mysql_free_result(mr);
```

如果 `mysql_result_to_json(mr)` 内部因为 `std::bad_alloc` 或字符串操作抛异常：

- `MYSQL_RES* mr` 不会 `mysql_free_result`。
- 连接在当前代码中已经提前 release，这一点更危险。
- stats 也不会正确记录失败。

构造函数预创建连接时也有类似问题：

```cpp
auto conn = create_connection_with_timeout();
if (conn) {
    std::lock_guard lock(mtx_);
    idle_pool_.push_back({conn, now});
    ++total_;
}
```

如果 `idle_pool_.push_back` 抛出 `std::bad_alloc`，新建出的 `MYSQL*` 没有 RAII 承接，可能泄漏。

推荐修复：

- 为 `MYSQL_RES*` 使用 `std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>`。
- 为新建但尚未入池的 `MYSQL*` 使用 `std::unique_ptr<MYSQL, decltype(&mysql_close)>` 临时持有，成功入池后 `release()`。
- 为查询阶段连接借出使用 `ConnectionGuard`，析构时自动 release/drop。

修复收益：

- 消除异常路径资源泄漏。
- 简化 `do_query()` 多分支手动归还逻辑。
- 避免未来新增分支时漏掉 `release()` 或 `drop_bad_connection()`。

验证方式：

- 编写 throw 注入测试，模拟 JSON 转换抛异常。
- 用 ASAN/Valgrind 验证异常路径无泄漏。
- 对 `min_size` 预创建阶段可通过 fault injection 模拟 `push_back` 抛异常，或者至少用代码审查确认 RAII。

## 重要问题

### P1-1：查询超时设置方式不可靠

结论：高概率成立，需要按 MySQL C API 语义修复。

证据：

查询路径动态设置：

```cpp
void apply_query_timeout(MYSQL* conn) {
    if (cfg_.query_timeout_ms <= 0) return;
    unsigned int rt = (cfg_.query_timeout_ms + 999) / 1000;
    if (rt < 1) rt = 1;
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &rt);
}
```

然后查询后动态清理：

```cpp
void clear_query_timeout(MYSQL* conn) {
    if (cfg_.query_timeout_ms <= 0) return;
    unsigned int rt = 0;
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &rt);
}
```

ping 路径也类似：

```cpp
mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &rt);
int ret = mysql_ping(conn);
mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &restore_rt);
```

问题：

- `mysql_options()` 很多选项要求在 `mysql_real_connect()` 前设置。
- `MYSQL_OPT_READ_TIMEOUT` 通常属于连接/socket 初始化期选项。
- 连接建立后再调用不应假设它会立刻修改底层 socket 的 `SO_RCVTIMEO`。
- 因此 `query_timeout_ms` 当前可能是“配置存在但实际不生效”。

风险：

- 线上以为已经设置查询超时，但慢查询或网络半开仍可能长期阻塞 worker。
- maintain 的 `mysql_ping_with_timeout()` 也可能没有真实超时保护。
- shutdown 时 `worker_pool_.join()` 可能等待更久。

修复选项：

方案 A：只在建连前设置统一 read timeout。

- 在 `create_mysql_connection_with_timeout()` 中，在 `mysql_real_connect()` 前设置 `MYSQL_OPT_READ_TIMEOUT`。
- `query_timeout_ms` 如果大于 0，则使用该值。
- ping 不再动态修改 timeout。
- 优点：实现简单，符合 C API 使用方式。
- 缺点：无法做到查询和 ping 使用不同 read timeout。

方案 B：连接建立后使用 socket fd 级别 timeout。

- 通过 `mysql_get_socket(conn)` 获取底层 fd。
- 用 `setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, ...)` 临时设置读取超时。
- 用 RAII scope 在查询或 ping 后恢复原值。
- 优点：可以动态区分 query timeout 和 ping timeout。
- 缺点：更依赖平台行为；需要处理 macOS/Linux `timeval` 兼容；MySQL client 内部重连或 fd 变化需要谨慎。

推荐：

- 如果只需要“查询有兜底超时”，优先方案 A。
- 如果明确要求 ping 500ms、query 可能 0 或更长，则方案 B 更贴近当前配置语义。
- 无论采用哪个方案，都应删除当前连接后 `mysql_options` 的“设置再恢复”模式，避免误导。

验证方式：

- 构造 `SELECT SLEEP(5)`，设置 `query_timeout_ms = 1000`。
- 修复后应约 1 秒失败。
- 同时验证 `query_timeout_ms = 0` 时不主动打断慢查询。

### P1-2：SQL 长度硬编码 4096 字节

结论：确认成立。

证据：

```cpp
char sql_buf[4096];
size_t len = sql.size();
if (len >= sizeof(sql_buf)) {
    stats_.inc_query_fail();
    co_return Result{false, "sql too long", ""};
}
```

当前测试也固化了该行为：

```cpp
TEST(MysqlPoolTest, RejectsSqlLongerThanStackBufferBeforeAcquire)
```

问题：

- 复杂 SQL、批量 insert、较长 IN 条件、JSON 字段写入都可能超过 4095 字节。
- 连接池层不应该隐式规定业务 SQL 最大长度为 4KB。
- 如果确实要限制 SQL 长度，应是显式配置项，例如 `max_sql_size`，并在错误信息里说明限制来源。

关于历史 double free：

`docs/PERF_REPORT.md` 曾把崩溃归因于"`std::string` 跨线程竞争"，并通过 `char[4096]` 修复。

**2026-07-11 ASan 验证结论：** 历史崩溃的根因是 **GCC 11 协程对 `co_await asio::post(lambda, executor, use_awaitable)` 中临时 lambda 闭包对象用 `memcpy` 复制，不走 copy constructor**。`char[4096]` 是有效的 workaround（绕过 lambda 捕获），但不是唯一解。

推荐修复（已验证通过 ASan）：

```cpp
asio::awaitable<Result> execute(std::string sql) {
    if (!running_) {
        stats_.inc_query_fail();
        co_return Result{false, "mysql pool stopped", ""};
    }
    co_await asio::post(worker_pool_, asio::use_awaitable);  // 仅切换 executor
    if (!running_) {
        stats_.inc_query_fail();
        co_return Result{false, "mysql pool stopped", ""};
    }
    co_return do_query(sql.c_str());  // sql 在协程帧内，无 lambda 捕获
}
```

**注意：以下方案在 GCC 11 下均会触发 use-after-free/double-free（已验证）：**
- `[this, sql = std::move(sql)]()` — move capture
- `[this, sql]()` — copy capture
- `[this, sql_ptr = std::make_unique<string>(...)]()` — unique_ptr capture
- `[this, sql_shared = std::make_shared<string>(...)]()` — shared_ptr capture

## 建议修复优先级

结论：确认成立。

证据：

池满且不能创建时：

```cpp
stats_.inc_acquire_wait();
cv_.wait(lock);
```

没有 `wait_for`，没有等待截止时间。

已有统计项：

- `MysqlPoolStats::inc_acquire_timeout()`
- `MysqlPoolStatsSnapshot::acquire_timeout_total`

但当前没有任何调用路径会增加该计数。

风险：

- 如果连接泄漏、慢查询堆积、数据库长时间阻塞，worker 线程可能永久卡在 acquire。
- worker 卡住后，后续请求也会排队，服务降级能力差。
- shutdown 虽然会 `cv_.notify_all()`，但业务运行期没有快速失败机制。

推荐修复：

- 增加配置：`acquire_timeout_ms`。
- 默认值需要谨慎：
  - 若默认 `0` 表示无限等待，可保持兼容。
  - 若默认非 0，会改变生产行为，但更安全。
- `cv_.wait_for()` 超时后返回 `nullptr`，上层返回 `"no available connection"` 或更明确的 `"acquire timeout"`。

建议配置：

```ini
[mysql]
acquire_timeout_ms = 1000
```

建议伪代码：

```cpp
if (cfg_.acquire_timeout_ms <= 0) {
    cv_.wait(lock);
} else {
    auto ok = cv_.wait_for(lock, timeout, [&] {
        return !running_ || !idle_pool_.empty() ||
               (total_ < cfg_.max_size && creating_ < max_creating_limit_);
    });
    if (!ok) {
        stats_.inc_acquire_timeout();
        return nullptr;
    }
}
```

注意：

- `wait_for` 应使用谓词，避免虚假唤醒导致错误超时。
- 最好使用总 deadline，而不是每次循环重新等待完整 timeout，否则可能无限延长。

验证方式：

- 构造 `max_size=1`，借出连接不归还，另一个 acquire 等待。
- 设置 `acquire_timeout_ms=50`。
- 验证超时后返回错误且 `acquire_timeout_total == 1`。

### P1-4：查询失败不区分错误类型，直接销毁连接

结论：确认成立。

证据：

`mysql_query()` 失败：

```cpp
if (mysql_query(conn, sql)) {
    std::string err = mysql_error(conn);
    LOG_WARN("MySQL query failed: ", err);
    drop_bad_connection(conn);
    stats_.inc_query_fail();
    return {false, std::move(err), ""};
}
```

`mysql_store_result()` 失败：

```cpp
if (mysql_field_count(conn) != 0) {
    std::string err = mysql_error(conn);
    LOG_WARN("MySQL store result failed: ", err);
    drop_bad_connection(conn);
    stats_.inc_query_fail();
    return {false, std::move(err), ""};
}
```

没有检查 `mysql_errno(conn)`。

风险：

- SQL 语法错误、表不存在、主键冲突、数据截断等业务错误也会销毁连接。
- 异常流量或错误请求可能导致连接频繁重建。
- 在高并发下，频繁建连可能放大数据库压力。
- 连接池 metrics 中 `connect_ok_total` 会异常升高，但不一定容易归因。

推荐修复：

增加错误分类函数：

```cpp
bool is_mysql_connection_error(unsigned int err) {
    switch (err) {
        case CR_SERVER_GONE_ERROR:
        case CR_SERVER_LOST:
        case CR_CONN_HOST_ERROR:
        case CR_CONNECTION_ERROR:
        case CR_SERVER_LOST_EXTENDED:
            return true;
        default:
            return false;
    }
}
```

需要包含对应 MySQL client error code 头文件，通常是：

```cpp
#include <mysql/errmsg.h>
```

处理策略：

- 连接级错误：`drop_bad_connection(conn)`。
- 业务级错误：尝试 `mysql_reset_connection(conn)` 后 `release(conn)`。
- 如果业务级错误后的 reset 失败，再 `drop_bad_connection(conn)`。

注意：

- 对 `mysql_store_result()` 失败，若错误来自连接中断，应 drop。
- 对非连接错误，reset 后 release 更稳妥。
- 错误响应应保留原始错误信息给调用方。

验证方式：

- 执行非法 SQL：`SELECT * FROM table_that_does_not_exist`。
- 修复前连接会被销毁，`total_` 减少后 maintain 或 acquire 重建。
- 修复后连接应 reset 并回池，`connect_ok_total` 不应随业务错误增长。

### P1-5：shutdown 后 `execute()` 仍可能投递任务

结论：确认成立。

证据：

`execute()` 入口没有检查 `running_`：

```cpp
asio::awaitable<Result> execute(const std::string& sql) {
    ...
    Result res = co_await asio::post(... worker_pool_ ...);
    co_return res;
}
```

shutdown 顺序：

```cpp
running_.exchange(false)
cv_.notify_all()
maintain_thread_.join()
worker_pool_.join()
close idle connections
```

风险：

- 如果业务层在 shutdown 后仍调用 `execute()`，可能向已经 join 的 `asio::thread_pool` 投递任务。
- ASIO 对 join 后 post 的行为不应作为业务正常路径依赖。
- 当前 `acquire()` 内部会检查 `running_`，但任务已经被投递到 worker_pool 才会失败。

推荐修复：

`execute()` 入口先检查：

```cpp
if (!running_) {
    stats_.inc_query_fail();
    co_return Result{false, "mysql pool stopped", ""};
}
```

同时，post 后 worker 内部最好也再次检查，以处理 check 和 post 之间 shutdown 的竞态：

```cpp
[this, sql = ...] {
    if (!running_) return Result{false, "mysql pool stopped", ""};
    return do_query(sql.c_str());
}
```

验证方式：

- 创建 pool 后立即 `shutdown()`。
- 再调用 `execute("SELECT 1")`。
- 应返回稳定错误，不应崩溃或卡住。

## 中等问题

### P2-1：`ioc_` 成员未使用

结论：确认成立。

证据：

构造函数保存：

```cpp
MysqlPool(asio::io_context& ioc, Config cfg)
    : running_(true),
      ioc_(ioc),
      ...
```

成员：

```cpp
asio::io_context& ioc_;
```

后续没有使用。

影响：

- 增加构造依赖。
- 让读者误以为 MySQL pool 需要绑定 `io_context`。
- 不影响运行正确性，但增加理解成本。

修复建议：

- 移除 `ioc_` 成员。
- 构造函数改为 `MysqlPool(Config cfg)`。
- 同步修改调用方 `Application::initialize()` 和测试。

兼容性注意：

- 这是 public 构造接口变化。
- 如果项目内部只有当前调用方，改动成本低。
- 如果担心影响外部使用，可保留构造参数但不存成员，或增加新构造重载。

### P2-2：maintain 健康检查固定 4 个，巡检周期长

结论：确认成立。

证据：

```cpp
const size_t max_check = 4;
size_t cnt = std::min(idle_pool_.size(), max_check);
```

默认：

- `max_size = 64`
- `keepalive_sec = 30`
- 每次最多检查 4 个

因此如果 idle 有 64 个连接，全量巡检需要：

- `64 / 4 = 16` 个周期
- `16 * 30s = 480s`
- 即 8 分钟

风险：

- 死连接可能在 idle 池中滞留较久。
- 下一次业务请求取到死连接后才发现问题。
- acquire 目前复用 idle 时只 reset，不 ping；如果 reset 也能发现断链，问题会暴露在业务路径上。

修复建议：

动态计算检查数量：

```cpp
size_t max_check = std::clamp(idle_pool_.size() / 10, size_t{2}, size_t{16});
```

或配置化：

```ini
[mysql]
health_check_max_per_round = 8
```

更深入选择：

- 如果 acquire 复用路径每次都 `mysql_reset_connection()`，它本身就是一次网络交互，可发现断链。
- maintain ping 的价值会降低。
- 可以保留少量 maintain ping，用于提前清理长期 idle 死连接。

验证方式：

- 构造大量 idle 连接后停掉 MySQL。
- 观察 dead connection 被清理所需时间。
- 对比修复前后 `ping_fail_total` 和业务请求失败率。

### P2-3：配置合法性校验不足

结论：基本成立。

证据：

`app_config_from()` 对部分配置做了 clamp：

```cpp
.min_size = static_cast<size_t>(std::max(0, cfg.get_int("mysql", "min_size", 8))),
.max_size = static_cast<size_t>(std::max(1, cfg.get_int("mysql", "max_size", 64))),
.worker_threads = static_cast<size_t>(std::max(1, cfg.get_int("mysql", "worker_threads", 32))),
```

但没有校验：

- `min_size <= max_size`
- `max_idle_sec > 0`
- `connect_timeout_ms > 0`
- `read_timeout_ms >= 0` 或 `> 0`
- `query_timeout_ms >= 0`
- `keepalive_sec > 0`
- `max_creating <= max_size` 虽然后续 `compute_max_creating_limit` 会 clamp

风险：

- `min_size > max_size` 时，构造函数会尝试预创建超过 `max_size` 的连接，因为预创建循环不检查 `max_size`。
- `keepalive_sec <= 0` 可能导致 maintain 线程高频循环。
- 负 timeout 被转换为 unsigned 或进入计算后语义异常。

推荐修复：

在 `MysqlPool::Config` 进入 pool 前统一 normalize：

```cpp
void validate_mysql_config(MysqlPool::Config& cfg) {
    cfg.max_size = std::max<size_t>(1, cfg.max_size);
    cfg.min_size = std::min(cfg.min_size, cfg.max_size);
    cfg.worker_threads = std::max<size_t>(1, cfg.worker_threads);
    cfg.max_idle_sec = std::max(1, cfg.max_idle_sec);
    cfg.connect_timeout_ms = std::max(1, cfg.connect_timeout_ms);
    cfg.read_timeout_ms = std::max(0, cfg.read_timeout_ms);
    cfg.query_timeout_ms = std::max(0, cfg.query_timeout_ms);
    cfg.keepalive_sec = std::max(1, cfg.keepalive_sec);
}
```

更严格的方式是非法配置直接报错并退出，而不是静默修正。

推荐策略：

- 对明显危险配置，如 `max_size == 0`、`worker_threads == 0`，直接修正或报错都可以。
- 对 `min_size > max_size`，建议至少打 `LOG_WARN`，因为这通常是配置错误。
- 对 timeout 负数，建议直接 clamp 到默认值并记录 warning。

验证方式：

- 增加 config 单元测试：
  - `min_size = 100, max_size = 10`
  - `keepalive_sec = 0`
  - `query_timeout_ms = -1`
- 验证最终配置符合预期。

### P2-4：魔法数字未提取

结论：确认成立。

当前魔法数字：

- `4096`：SQL 栈缓冲区大小。
- `2`：acquire 重试次数。
- `4`：maintain 每轮健康检查数量。
- `1`：timeout 秒级换算后最小值。

影响：

- 修改行为时不容易发现所有相关位置。
- 文档中部分地方写 `max_check=8`，代码实际为 `4`，容易出现文档和实现偏差。

修复建议：

```cpp
static constexpr int kAcquireRetries = 2;
static constexpr size_t kDefaultHealthCheckMax = 4;
```

如果移除 SQL 栈缓冲，则 `4096` 不再需要。

### P2-5：可观测性可以继续增强

结论：建议项成立，但不是当前最优先。

已有指标：

- `query_ok_total`
- `query_fail_total`
- `connect_ok_total`
- `connect_fail_total`
- `reset_conn_fail_total`
- `acquire_wait_total`
- `acquire_timeout_total`
- `acquire_retry_exhausted_total`
- `idle_recycled_total`
- `ping_fail_total`
- `total`
- `idle`
- `creating`
- `max_creating`

缺少指标：

- acquire 等待耗时。
- acquire timeout 次数虽有字段，但未使用。
- SQL 错误分类统计：连接错误、业务错误、store result 错误。
- 连接使用时长。
- reset 成功次数。
- drop connection 原因分类。

建议：

- 本轮修复至少让 `acquire_timeout_total` 生效。
- 增加 `dropped_conn_total` 和 reason 可能更有用。
- 慢查询日志应谨慎，避免高 QPS 下日志放大，可采样或只在超过阈值时记录。

## 文档与实现不一致

### 文档说 acquire 会 reset，代码只 reset 新连接

`docs/DB_POOL_DESIGN.md` 中写的是：

- acquire 会执行 `mysql_reset_connection()`。
- 目的为“清理上一个用户残留的 session 状态”。

但代码实际是：

- idle 复用路径不 reset。
- 新连接路径 reset。

这是明确的不一致。应优先修代码，再同步文档。

### 文档说 max_check 为 8，代码为 4

`docs/DB_POOL_DESIGN.md` 配置表中写：

```text
max_check | 8 | 每次 maintain 最多检查的空闲连接数
```

代码实际：

```cpp
const size_t max_check = 4;
```

应统一：

- 要么代码改到 8。
- 要么文档改成 4。
- 更好是配置化或动态计算。

### `PERF_REPORT.md` 的 SQL string double free 归因需要复核

`docs/PERF_REPORT.md` 中写：

```text
std::string 跨线程竞争导致 double free
```

**2026-07-11 ASan 验证结论：** 该归因需要补充。

经 ASan 三次精准定位（PID 6073 unique_ptr、PID 9281 shared_ptr、PID 11395 shared_ptr 第二次），确认根因是 **GCC 11 协程对 `co_await asio::post(lambda, executor, use_awaitable)` 中临时 lambda 闭包对象用 `memcpy` 复制，不走 copy constructor**。导致：

- `unique_ptr<string>` 方案：worker 线程 (T2) 和 io_context 线程 (T36) 各析构一次同一块 string 内存 → **heap-use-after-free**
- `shared_ptr<string>` 方案：两个 `shared_ptr` 实例共享控制块但 `use_count` 均为 1（因 memcpy 未调用 copy ctor 递增引用计数），第一个析构 free 控制块，第二个析构尝试 atomic write 已释放内存 → **heap-use-after-free**
- `string` 值捕获方案：同样 memcpy 导致两个 string 内部指针相同 → **double free**

**最终修复**（`mysql_pool.hpp:117-122`）：

```cpp
// 修复前（各种方案均失败）：
auto sql_ptr = std::make_unique<std::string>(std::move(sql));
Result res = co_await asio::post(
    [this, sql_ptr = std::move(sql_ptr)]() -> Result {
        return do_query(sql_ptr->c_str());
    },
    worker_pool_, asio::use_awaitable);
co_return res;

// 修复后（绕开 lambda 捕获，仅切换 executor）：
co_await asio::post(worker_pool_, asio::use_awaitable);  // 切换到 worker 线程
if (!running_) {
    stats_.inc_query_fail();
    co_return Result{false, "mysql pool stopped", ""};
}
co_return do_query(sql.c_str());  // sql 在协程帧内，单一所有权
```

**关键区别：** `asio::post(executor, use_awaitable)` 仅切换协程执行器，`sql` 始终在协程帧内，无 lambda 跨线程传递，彻底规避 GCC 11 的协程闭包 memcpy bug。

**验证结果：** ASan build + MySQL 压测，`heap-use-after-free` 消失，MySQL 返回 HTTP 200。

## 建议修复优先级

### 第一批：正确性与资源安全

目标：先消除会话污染、连接过早复用、异常泄漏。

建议包含：

1. 复用连接 reset。
2. 调整结果集处理顺序，先转换/free，再 release。
3. `MYSQL_RES*` RAII。
4. `ConnectionGuard` 管理 release/drop。
5. `execute()` shutdown 快速失败。

理由：

- 这些问题影响数据正确性和进程稳定性。
- 改动集中在 `MysqlPool::do_query()` 和 `acquire()`。
- 不需要立刻改变配置文件行为。

### 第二批：超时和错误分类

目标：让失败行为可控，减少不必要连接重建。

建议包含：

1. `acquire_timeout_ms`。
2. `query_timeout_ms` 的真实实现或移除误导性动态设置。
3. ping timeout 的真实实现。
4. `mysql_errno()` 分类，业务错误 reset 后 release，连接错误 drop。
5. 对 timeout 和 drop reason 增加统计。

理由：

- 这批涉及配置语义，需要更谨慎。
- 超时改动可能影响线上慢查询，需要明确默认值和兼容策略。

### 第三批：清理与文档同步

目标：降低维护成本。

建议包含：

1. 移除未使用 `ioc_`。
2. 魔法数字提取或配置化。
3. 健康检查数量动态化。
4. 配置校验完善。
5. 同步更新 `DB_POOL_DESIGN.md`、`PERF_REPORT.md`。

理由：

- 对功能正确性影响较小。
- 可以在前两批稳定后处理。

## 推荐修复设计草案

### `do_query()` 推荐结构

目标：

- acquire 后由 guard 持有连接。
- 所有分支自动 release/drop。
- 结果集先处理完再归还连接。

示意：

```cpp
Result do_query(const char* sql) {
    MYSQL* raw = acquire();
    if (!raw) {
        stats_.inc_query_fail();
        return {false, "no available connection", ""};
    }

    ConnectionGuard conn(*this, raw);

    ScopedSocketTimeout timeout(raw, cfg_.query_timeout_ms);

    if (mysql_query(raw, sql)) {
        auto err_no = mysql_errno(raw);
        std::string err = mysql_error(raw);
        if (is_mysql_connection_error(err_no)) {
            conn.mark_bad();
        } else {
            conn.reset_before_release();
        }
        stats_.inc_query_fail();
        return {false, std::move(err), ""};
    }

    MysqlResultPtr mr(mysql_store_result(raw), mysql_free_result);
    if (!mr) {
        if (mysql_field_count(raw) != 0) {
            auto err_no = mysql_errno(raw);
            std::string err = mysql_error(raw);
            if (is_mysql_connection_error(err_no)) {
                conn.mark_bad();
            } else {
                conn.reset_before_release();
            }
            stats_.inc_query_fail();
            return {false, std::move(err), ""};
        }

        stats_.inc_query_ok();
        return {true, "", "[]"};
    }

    std::string json = mysql_result_to_json(mr.get());
    stats_.inc_query_ok();
    return {true, "", std::move(json)};
}
```

注意：

- 上面只是结构草案，不是可直接粘贴的最终代码。
- `ConnectionGuard` 是否在业务错误后 reset，应结合 acquire reset 策略决定，避免重复 reset 太多。
- 如果 acquire 已经保证每次复用前 reset，则业务错误后也可以直接 release，但为了处理未完成结果或事务，错误后 reset 更保守。

### acquire reset 推荐结构

目标：

- idle 复用连接 reset。
- reset 锁外执行。
- reset 失败销毁连接并继续 retry。

示意：

```cpp
MYSQL* acquire() {
    for (int retry = 0; retry < kAcquireRetries; ++retry) {
        MYSQL* idle = nullptr;

        {
            std::unique_lock lock(mtx_);
            while (true) {
                if (!running_) return nullptr;

                if (!idle_pool_.empty()) {
                    idle = idle_pool_.front().conn;
                    idle_pool_.pop_front();
                    break;
                }

                if (total_ < cfg_.max_size && creating_ < max_creating_limit_) {
                    ++total_;
                    ++creating_;
                    break;
                }

                if (!wait_for_capacity(lock)) {
                    stats_.inc_acquire_timeout();
                    return nullptr;
                }
            }
        }

        if (idle) {
            if (reset_connection(idle)) return idle;
            drop_bad_connection(idle);
            continue;
        }

        MYSQL* created = create_connection_with_timeout();
        ...
        return created;
    }

    stats_.inc_acquire_retry_exhausted();
    return nullptr;
}
```

### 超时推荐结构

如果选择 fd 级动态超时：

```cpp
class ScopedRecvTimeout {
public:
    ScopedRecvTimeout(MYSQL* conn, int timeout_ms);
    ~ScopedRecvTimeout();

private:
    int fd_ = -1;
    bool active_ = false;
    timeval old_{};
};
```

使用：

```cpp
ScopedRecvTimeout timeout(conn, cfg_.query_timeout_ms);
mysql_query(conn, sql);
```

注意：

- `timeout_ms <= 0` 时不设置。
- 析构时恢复旧值。
- `setsockopt` 失败应记录 warning，但不应崩溃。
- macOS/Linux 都使用 `timeval`，但 `getsockopt` 的 `socklen_t` 处理要正确。

如果选择连接级统一 read timeout：

- 在 `create_mysql_connection_with_timeout()` 的 `mysql_real_connect()` 前设置。
- 删除 `apply_query_timeout()` 和 `clear_query_timeout()`。
- 文档明确 query timeout 不支持每次动态切换。

## 测试计划

### 必须补的单元测试

1. `execute()` shutdown 后快速失败。
2. `acquire_timeout_ms` 超时计数递增。
3. 配置解析支持 `acquire_timeout_ms`。
4. 长 SQL 不再被 4096 入口拒绝，或如果保留限制，则配置化并测试新限制。
5. `max_creating` 默认计算不受新增字段影响。

难点：

- 当前 `MysqlPool` 成员都是 private，且依赖真实 MySQL。
- 若要做纯单元测试，需要抽象 MySQL C API 或提供 test hook。

### 必须补的集成测试

需要真实 MySQL。

1. session 变量不泄漏：
   - 请求 1：`SET @asio_owen_pool_test = 123`
   - 请求 2：`SELECT @asio_owen_pool_test`
   - 期望：`NULL`

2. autocommit 不泄漏：
   - 请求 1：`SET autocommit = 0`
   - 请求 2：`SELECT @@autocommit`
   - 期望：默认值，通常为 `1`

3. 业务 SQL 错误不销毁连接：
   - 记录 `connect_ok_total`
   - 执行非法 SQL
   - 再执行正常 SQL
   - 期望：连接不因业务错误频繁重建

4. 连接级错误会销毁连接：
   - 建立连接后重启 MySQL 或 kill connection
   - 下一次查询应 drop bad connection
   - 后续查询应能重建并恢复

5. 查询超时真实生效：
   - `query_timeout_ms = 1000`
   - 执行 `SELECT SLEEP(5)`
   - 期望约 1 秒失败

### 压测验证

修复后至少跑：

```bash
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure
./build.sh
```

如果本地 MySQL 可用，再跑：

```bash
wrk/plow /api/mysql
wrk/plow /api/combo
```

观察：

- `query_ok_total`
- `query_fail_total`
- `connect_ok_total`
- `reset_conn_fail_total`
- `acquire_wait_total`
- `acquire_timeout_total`
- `ping_fail_total`
- RSS 是否增长
- server log 是否出现 error/fatal/crash/free

## 风险评估

### 修复 reset 的性能影响

每次复用连接执行 `mysql_reset_connection()` 会增加一次 MySQL client/server 往返或协议操作。

可能影响：

- `/api/mysql` RPS 下降。
- p50/p99 延迟上升。

但这是连接池隔离 session 状态的必要成本。对生产正确性来说，不能为了省这次 reset 允许 session 泄漏。

可选优化：

- release 时做轻量清理，acquire 时按需 reset。
- 区分只读简单查询路径和事务路径。
- 但这些都需要更强的使用约束，目前不建议先做。

### 改动 SQL string 传递的风险

移除 `char[4096]` 能解决功能限制，但会触碰历史 double free workaround。

建议策略：

- 第一批可以暂不移除 4KB，先修 reset 和 RAII。
- 第二批单独改 SQL 传递，并用 ASAN + 高并发压测验证。
- 文档中同步说明历史 double free 归因已复核。

### 超时改动的兼容风险

如果默认开启 `query_timeout_ms`，慢查询会被中断，可能改变业务行为。

建议：

- 默认仍保持 `query_timeout_ms = 0`。
- 只修正“配置大于 0 时必须真实生效”。
- `acquire_timeout_ms` 默认是否为 0 需要产品化决策：兼容优先则 0，稳定性优先则给 1000 或 3000。

## 最小修复集建议

如果只允许一次小修，建议最小集为：

1. idle 复用连接 reset。
2. `MYSQL_RES*` RAII。
3. 先 `mysql_result_to_json()` 和 `mysql_free_result()`，再 `release(conn)`。
4. `execute()` 入口检查 `running_`。

这四项可以显著降低数据污染和资源泄漏风险，且不改变配置文件。

如果允许一次中等修复，建议再加入：

5. 业务错误和连接错误分类。
6. `acquire_timeout_ms`。
7. 查询/ping 超时真实实现。

如果允许完整修复，最后加入：

8. 移除 4KB SQL 限制。
9. 移除 `ioc_`。
10. 动态健康检查数量。
11. 配置合法性校验。
12. 文档同步。

## 待确认问题

以下问题需要进一步验证，而不是只靠静态代码审查下最终结论：

1. `MYSQL_OPT_READ_TIMEOUT` 在当前链接的 MySQL client 版本中，连接后调用是否完全无效。
   - 静态判断是高概率无效或不可靠。
   - 最终应通过 `SELECT SLEEP()` 和网络故障测试确认。

2. 历史 `std::string` double free 根因。
   - 标准 C++ 值捕获不应导致 double free。
   - 需要查历史提交或崩溃栈，确认是否当时代码存在引用捕获、悬垂指针、提前 release、结果集交错等问题。

3. `mysql_reset_connection()` 的成本。
   - 正确性上必须做。
   - 性能影响需要压测量化。

4. 业务错误后是否必须立即 reset 再 release。
   - acquire 复用前 reset 已能兜底。
   - 但错误后 reset 可以更早释放事务/锁状态。
   - 推荐错误后 reset，失败则 drop。

## 最终建议

**已修复：**

- ✅ 复用连接未 reset（`mysql_reset_connection` 在 `acquire()` 中已加）
- ✅ 结果集和连接释放顺序（`ConnectionGuard` RAII 保证作用域）
- ✅ acquire 无超时（`cv_.wait_until` + `acquire_timeout_ms`）
- ✅ 错误类型区分（`is_connection_error()` 区分连接错误和 SQL 业务错误）
- ✅ shutdown 检查（`execute()` 双重 `running_` 检查）
- ✅ **GCC 11 协程 lambda memcpy bug**（`co_await asio::post(executor, use_awaitable)` 仅切换 executor，绕开 lambda 捕获）

当前评审报告中的多数问题成立，尤其是：

- 复用连接未 reset。
- 结果集和连接释放顺序不安全。
- 资源缺少 RAII。
- 动态 read timeout 不可靠。
- acquire 无超时。
- 错误类型不区分。

建议不要一次性做大而全的重构。更稳妥的节奏是：

1. 先修 P0 正确性问题，保持配置语义不变。
2. 再修 timeout、错误分类、acquire timeout。
3. 最后处理 SQL 4KB、`ioc_`、健康检查、配置校验和文档同步。

每批修复都应配套 `ctest`、本地 smoke test，以及至少一次真实 MySQL 集成验证。
