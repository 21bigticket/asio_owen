# RedisPool — Lambda 捕获 UAF 隐患修复

> 日期：2026-07-12
> 范围：`src/db/redis_pool.hpp`（3 处 `asio::post(lambda, executor, use_awaitable)`）
> 关联：`docs/SUMMARY_2026-07-12.md` 第五节「关键教训」、`docs/MYSQL_POOL_REVIEW_CONFIRMATION_2026-07-11.md`、`docs/REDIS_POOL_REVIEW_CONFIRMATION_2026-07-11.md`

## 背景：两份不同性质的评审

### A. 昨天的扎实评审（已大部分修复）

`docs/REDIS_POOL_REVIEW_CONFIRMATION_2026-07-11.md` 共 1490 行，逐函数确认了 8 类问题。当前代码已经处理了其中大部分：

- P0-2（shutdown 后命令入口不检查 `running_`）→ 已在 `cmd_argv` / `get` / `do_cmd` / `cmd_argv_sync_impl` 顶部统一加 `if (!running_)` 检查
- P0/P1-3（`cmd_timeout_ms <= 0` 永久阻塞）→ `validate_config` 已加 `<= 0 → 30000ms` 兜底
- P1-4（`ioc_` 成员未使用）→ 已移除
- P1-2（超时统计依赖字符串匹配）→ `record_command_failure` 已加 `ctx->err == REDIS_ERR_IO && (errno == ETIMEDOUT|EAGAIN|EWOULDBLOCK)` 双重判定
- P1-1（执行/错误处理重复）→ 已抽取 `RedisReplyGuard` / `ConnectionGuard` / `fill_error_if_empty` / `record_command_failure`
- 第三批（worker 模式可选）→ 已落地为 `Mode::Worker` + `asio::thread_pool`

### B. 今天的外部「P0~P3 风险清单」（伪问题）

| 原评级 | 原描述 | 实际核对 | 真实评级 |
|:------:|:-------|:---------|:--------:|
| P0 | Worker 模式 `release_conn` 未调用 `redisResetConnection` | `acquire_worker` 弹出后已调用 `reset_worker_connection`（执行 `SELECT %d` 重选 DB）。同步 `redisCommand` 路径下 reply 一定被消费完，无 unconsumed buffer。本项目不用 pubsub/MULTI/monitor，SELECT 已覆盖实际状态。 | P4（防御性，非 bug） |
| P1 | Direct 模式线程退出前 TLS 连接未释放，FD 泄漏 | `TlsRedisConn::conn` 是 `unique_ptr<redisContext, redisFree>`，thread_local 析构自动 `redisFree`。配合 `generation_` 机制，pool 销毁后下次访问会 owner/generation 不匹配触发 reset。无泄漏路径。 | 不成立 |
| P2 | 命令超时未在每次使用时重新 `redisSetTimeout` | `redisSetTimeout` 内部是 `setsockopt(SO_RCVTIMEO/SO_SNDTIMEO)`，socket 选项持久，不会因命令执行而清除。hiredis 不会自动 `redisReconnect`。每次重设纯属浪费 syscall。 | 不成立 |
| P3 | 错误信息依赖 errno，应优先 `ctx->errstr` | 用户拿到的错误本就来自 `fill_error_if_empty(ctx)` 返回的 `ctx->errstr`。errno 只在 `record_command_failure` 内做 timeout stats 分类用（hiredis errstr 不一定含 "timeout" 字样，errno 更可靠）。 | 不成立 |

**结论：A 评审是有效输入，B 评审是低质量噪声，不要按它修代码。**

## 真正的隐患：3 处 `asio::post(lambda)` 捕获非 POD

memory `feedback-asio-post-lambda-gcc11.md` 早就标了「redis_pool.hpp 仍有 3 处潜在炸点」。当前代码（`src/db/redis_pool.hpp`）里这 3 处都还在：

| # | 函数 | 行 | 危险捕获 |
|:-:|:-----|:--:|:--------|
| 1 | `cmd_argv` | 134-140 | `[this, args = std::move(args)]` — `args` 是 `std::vector<std::string>` |
| 2 | `get` | 151-157 | `[this, key_copy = std::move(key_copy)]` — `key_copy` 是 `std::string` |
| 3 | `do_cmd` | 482-488 | `[this, cmdline = std::move(cmdline)]` — `cmdline` 是 `std::string` |

### 风险机理（详见 `docs/SUMMARY_2026-07-12.md` 第五节）

GCC 11.4 协程实现下，`co_await asio::post(lambda, executor, use_awaitable)` 对临时 lambda 闭包对象用 `memcpy` 复制，绕过 copy/move constructor。导致：

- `std::string`、`std::vector<std::string>>` 等捕获对象的内部指针被「浅拷贝」共享
- worker 线程和 io_context 线程各析构一次 → heap-use-after-free

MySQL 池曾因此连续 4 个修复方案失败（详见 `docs/SUMMARY_2026-07-12.md` 第二阶段表），最终方案是**只切 executor、不带 lambda**。

### 对照 MySQL 已修好的写法（`src/db/mysql_pool.hpp:111-123`）

```cpp
asio::awaitable<Result> execute(std::string sql) {
    if (!running_) {
        stats_.inc_query_fail();
        co_return Result{false, "mysql pool stopped", ""};
    }

    co_await asio::post(worker_pool_, asio::use_awaitable);   // ← 只切 executor
    if (!running_) {
        stats_.inc_query_fail();
        co_return Result{false, "mysql pool stopped", ""};
    }
    co_return do_query(sql.c_str());                          // ← sql 留在协程帧内
}
```

关键差异：

| | Redis 当前（危险） | MySQL 已修（安全） |
|:-|:-------------------|:--------------------|
| 写法 | `asio::post(lambda, executor, use_awaitable)` | `asio::post(executor, use_awaitable)` |
| 非 POD 数据存放位置 | lambda 捕获（GCC 11 浅拷贝风险） | 协程帧（asio 框架管理，跨线程切换安全） |
| 线程切换后 | lambda 被 asio 内部 move/fwd，触发 GCC bug | 协程 resume，参数随 frame 自动跟随 |

## 修复方案

3 处统一改成 Direct/Worker 对称结构，参照 MySQL `execute` 模板：

```cpp
// 修复后（cmd_argv 为例）
asio::awaitable<Reply> cmd_argv(std::vector<std::string> args) {
    if (!running_) {
        co_return make_error("redis pool is shutdown");
    }
    if (cfg_.mode == Mode::Direct) {
        co_return cmd_argv_sync_impl(std::move(args));
    }

    co_await asio::post(*worker_pool_, asio::use_awaitable);
    if (!running_) {
        co_return make_error("redis pool is shutdown");
    }
    co_return cmd_argv_sync_impl(std::move(args));
}
```

`get` 和 `do_cmd` 同理。

### 为什么这是零功能差异

- `co_await asio::post(executor, use_awaitable)` 的语义就是「切到目标 executor 后让协程 resume」
- 原写法「post 一个 lambda，lambda 返回结果，asio 把结果传回 io_context」是同一件事的不同封装
- 唯一区别是非 POD 参数放哪：协程帧（safe）vs lambda 闭包（GCC 11 unsafe）

### 编译器假设

- 生产仍用 GCC 11.4（见 `docs/SUMMARY_2026-07-12.md` 验证环境），隐患真实存在
- 即使未来升级到 GCC 12+（asio 协程实现修过），这个写法本身也更干净，向后兼容老编译器

## 验证

- `cmake --build build` 编译通过
- `ctest --test-dir build --output-on-failure` 单元测试通过
- `tests/test_redis_pool.cpp` 已有覆盖
- 生产灰度后用 ASan 复测，确认无 heap-use-after-free（参见 MySQL 同类验证流程）
