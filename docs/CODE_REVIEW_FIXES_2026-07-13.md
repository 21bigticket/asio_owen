# Code Review Fixes 2026-07-13

本文记录 2026-07-13 对两份代码 review 报告的核对结论、已修复问题、保留风险和验证结果。

## 范围

- DB：MySQL pool / Redis pool
- HTTP：client session / gateway forwarder / response builder / upstream HTTP pool
- Security + App：path normalize / ACL / JWT / rate limiter / real IP / signal handling / snapshot timer

## 已确认并修复

### 1. RateLimiter 热更新 data race 与 UAF

问题：

- `update_config()` 写 `max_buckets_per_shard_`，`evict_if_needed()` 无同步读取。
- 热更新 `max_buckets < kShards` 时 `max_buckets_per_shard_` 可变成 0。
- `check()` 先取 `bucket` 引用，再 `evict_if_needed()`，容量为 0 时可能淘汰当前 key，形成悬空引用。

修复：

- 构造和热更新统一走 `normalize_config()`。
- `max_buckets < kShards` 统一 clamp 到 `kShards`。
- `max_buckets_per_shard_` 改为 `std::atomic<size_t>`。
- 新增 `RateLimiter.UpdateConfigClampsMaxBucketsBelowShardCount` 回归测试。

### 2. MysqlPool::Config::port 未初始化

问题：

- `MysqlPool::Config::port` 无默认值。
- 默认构造后 `validate_config()` 读取未初始化值，导致测试失败或 UB 风险。

修复：

- `port` 默认值补为 `3306`，与 `MysqlConnection::Config` 保持一致。

### 3. SecurityRules::check() 锁外读取 rate_limiter_

问题：

- `check()` 在 `rules_mu_` 外直接读 `rate_limiter_`。
- 当前实现多数 reload 只调用 `update_config()`，但若后续支持重建/关闭 limiter，会形成 data race/UAF。

修复：

- `rate_limiter_` 从 `unique_ptr` 改为 `shared_ptr`。
- `check()` 在 `rules_mu_` 内快照 `rate_limiter_copy`，出锁后使用。
- `SnapshotService` 改用 `rate_limiter_snapshot()`。

### 4. AuthWhitelist 重复加锁

问题：

- `is_whitelisted()` 调用 `is_path_whitelisted()` 和 `is_service_whitelisted()`，同一请求路径上拿两次锁。

修复：

- `is_whitelisted()` 合并为一次锁内完成 path/service 检查。

### 5. SignalExit 未忽略 SIGPIPE

问题：

- Linux/macOS 上写已关闭 socket 可能触发 SIGPIPE。

修复：

- `SignalExit` 构造时显式 `std::signal(SIGPIPE, SIG_IGN)`。

### 6. HttpPool 新建连接异常安全

核对结论：

- 报告中的“`make_unique` 抛 `bad_alloc` 后 catch 里空指针解引用”不完全准确，因为 `make_unique` 原本在 `try` 外。
- 但真实问题成立：在 shard/global 计数自增后，如果注册 active 或后续流程抛异常，可能无法完整补偿计数。

修复：

- 先构造 `HttpConn`，再进入 shard 计数和 active 注册。
- 注册阶段异常会补偿 global `total_count` 和 `in_flight_count`。

### 7. path_normalize 编码安全旁路

问题：

- `%2F` 保留为字面量，安全层看不到真实路径分隔。
- `%00` 可进入 normalized path，存在 C-string 截断风险。
- `%252e%252e` 双重编码会留下残余 `%xx`，安全层无法可靠解释。

修复：

- `NormalizedPath` 增加 `valid/error`。
- 拒绝 encoded slash、encoded backslash、NUL、非法 percent encoding、解码后仍残留 `%` 的路径。
- `SecurityRules::check()` 对 invalid path 返回 `400 invalid path`。
- 新增 encoded slash / NUL / double encoding 回归测试。

### 8. ACL 配置与请求路径大小写归一不一致

问题：

- `case_sensitive_paths=false` 时请求路径会小写化，但 `path_blacklist` / `auth_whitelist` / `rate_limit_paths` 配置加载时未统一归一。
- 例如配置 `/Admin`，请求 `/ADMIN` 可能绕过。

修复：

- 安全规则加载时统一 normalize path key。
- path blacklist、auth whitelist、rate limit path 使用同一套大小写和 dot-segment 规则。
- 新增 `PathBlacklistNormalizesConfiguredCase` 回归测试。

### 9. JWT 显式配置缺失时静默降级

问题：

- 显式配置 `jwt_algorithm=HS256` 但 `jwt_secret` 为空，或 `jwt_algorithm=RS256` 但没有 public key/JWKS 时，原逻辑会关闭 JWT。

修复：

- 显式配置 algorithm 且缺少必需密钥时，`load_from_config()` 直接抛异常。
- 默认不配置 JWT 仍保持兼容：JWT disabled。
- 新增 HS256/RS256 缺失配置 fail-closed 测试。

### 10. real_ip unknown 共享桶

问题：

- `remote_endpoint()` 异常时返回 `"unknown"`。
- 会导致所有异常连接共享同一个限流 bucket，并绕过 IP blacklist。

修复：

- `get_client_ip()` 异常时返回空字符串。
- `SecurityRules::check()` 对无法 parse 的 client IP 返回 `400 invalid client ip`。

### 11. 请求 header name 注入

问题：

- `proxy_forwarder` 原本只检查 header value 的 CR/LF/NUL，未检查 header name。

修复：

- header name 增加 CR/LF/NUL/冒号检查。
- 新增 `RejectsControlCharInHeaderName` 回归测试。

### 12. 响应头 response splitting

问题：

- upstream/local response headers 直接拼接到下游响应，未过滤 CR/LF/NUL。

修复：

- `response_builder` 增加 `is_safe_header()`。
- 不安全 response header 直接跳过。
- 新增 `DownstreamResponseDropsUnsafeHeaderValue` 回归测试。

### 13. ClientSession catch 块同步写

问题：

- `ClientSession::run()` 的异常 catch 块里使用同步 `asio::write()`。
- 该路径运行在 `io_context` worker 上；如果客户端半关闭或 TCP 缓冲阻塞，会阻塞整个 worker，违反项目“io_context 不做阻塞 IO”的约束。

修复：

- catch 块改为复用 `write_simple_error()`。
- 错误响应通过 `asio::async_write()` + `redirect_error` 写出，不再同步阻塞 `io_context`。

### 14. MySQL ping timeout 参数未生效

问题：

- `mysql_ping_with_timeout(MYSQL*, int read_timeout_ms)` 原实现忽略 `read_timeout_ms`。
- maintain 线程检查 idle 连接时调用 `mysql_ping()`，坏连接可能按连接级默认 read timeout 阻塞，函数名和行为不一致。

修复：

- ping 前读取当前 `MYSQL_OPT_READ_TIMEOUT`。
- 按 `read_timeout_ms` 向上取整到秒，临时设置 `MYSQL_OPT_READ_TIMEOUT` 后执行 `mysql_ping()`。
- ping 后恢复原 read timeout。

边界：

- MySQL C API 的 read timeout 粒度是秒，因此 `500ms` 会生效为 `1s`。
- 如果底层 client library 不接受运行时修改 timeout，会记录 warn log，并继续按当前连接配置执行 ping。

### 15. Client header read timeout 外提配置

问题：

- 原实现把客户端请求头读取 idle timeout 写死为 `30s`。
- 虽然每次 `async_read_some()` 都已有 idle timeout，不是“只有整体超时”，但 30s 对 header 阶段偏宽；攻击者只要 30s 内持续滴入少量字节，仍可长期占用连接。

修复：

- 新增 `[server].client_header_read_timeout_ms`。
- 默认值为 `10000`，比原先硬编码 30s 更紧。
- 该配置只作用于 header 读取阶段；请求 body 读取仍保持原 30s idle timeout，避免扩大行为变更。
- `HttpServerState` / `HttpServer` / `AppConfig` 均透传该配置。
- 新增 `ConfigLoad.ParsesClientHeaderReadTimeout` 测试。

### 16. UpstreamManager 写 API 收敛

问题：

- `UpstreamManager::add_upstream()` 是 public，内部不加锁。
- 当前生产路径只有 init 阶段和 `reload()` 内部使用，`reload()` 已持 `unique_lock`，所以当前没有实际 race。
- 但 public 无锁写 API 容易被后续调用方误用。

修复：

- `Application::register_upstreams()` 改为直接调用 `UpstreamManager::reload()`。
- `add_upstream()` 收为 private 的 `add_upstream_locked()`，只允许在 `reload()` 持锁路径下调用。
- 测试改为通过临时 `[upstream]` 配置和 `reload()` 注入 upstream，不再直接调用写 API。

## 已核对但暂未修复

### Redis reset_worker_connection 协议失步

报告结论：

- `reset_worker_connection()` 在 worker 连接重新借出前执行 `SELECT`。
- 如果连接存在未读残留，`SELECT` 回复可能错位。

当前判断：

- 常规 `redisCommand` 同步路径会读取完整 reply。
- 风险更可能来自 pipeline/协议误用、连接状态被命令改变、或未来开放更底层 Redis API。
- 这项需要单独定策略：例如禁止危险命令、归还前丢弃特定连接、或 worker 模式借出时重连而非 SELECT。

### MySQL execute 与文档中的栈 buffer 不一致

报告结论：

- 文档记录过 `char buf[4096]` 防 UAF，但当前 `execute(std::string sql)` 未使用栈 buffer。

当前判断：

- 这不是立即 UAF：协程参数 `std::string sql` 会跨 suspend 保存。
- 但确实与历史性能/设计文档描述不一致，需要后续更新文档或重新评估栈 buffer 优化是否仍必要。

### 其他保留项

- Redis direct 模式重连可能在 `io_context` 线程阻塞。
- RateLimiter snapshot 反序列化未限制 count 上限。
- Global token bucket reject refund 逻辑仍需压测/并发模型复核。
- `OPTIONS` 当前仍跳过完整安全链。这可能是 CORS 语义选择，也可能需要按生产策略改为至少走 IP blacklist / rate limit。

### Slowloris 复核修正

报告结论：

- 原报告称“只有 30s 整体超时，无字节间超时”。

当前判断：

- 当前 `ClientSession` 每次 `async_read_some()` 都调用 `read_with_timeout()`，因此已经存在每次读的 idle timeout。
- 真实剩余风险是 timeout 过宽：攻击者只要在 30s 内持续滴入少量字节，仍可长期占用连接。
- 已通过 `[server].client_header_read_timeout_ms` 外提配置解决默认过宽问题；是否再增加“总 header deadline”需要单独评估。

## 验证

构建：

```bash
cmake --build build-mac
```

全量测试：

```bash
ctest --test-dir build-mac --output-on-failure
```

结果：

- 155/155 passed
- 0 failed

## VM 部署与运行巡检

时间：2026-07-13。

环境：

- VM：`192.168.139.230`
- 代码目录：`/mnt/mac/Users/mac/code/croot/asio_owen`
- 运行目录：`/mnt/mac/Users/mac/code/croot/asio_owen/build`
- 进程：`26303 ./server`
- 构建：Release stripped，`build/server` 大小约 1.2 MB

部署命令：

```bash
bash rebuild_deploy.sh
```

部署后 smoke：

| 接口 | 状态 |
|------|------|
| `/api/health` | 200 |
| `/api/mysql` | 200 |
| `/api/redis` | 200 |
| Gateway zebra-config | 200 |
| segfault | 0 |

### 7/13 Release 全量压测

用户在 VM Release 构建上执行 30s x 2 轮全量压测，结果如下：

| 接口 | RPS #1 | RPS #2 | RPS avg | avg_lat | errors |
|------|-------:|-------:|--------:|---------|--------|
| Health | 114,170 | 115,181 | 115k | 0.89ms | 0 |
| Redis | 16,696 | 18,661 | 17.7k | 5.2ms | 0 |
| MySQL | 12,627 | 14,393 | 13.5k | 6.8ms | 0 |
| Config Direct | 19,274 | 18,101 | 18.7k | 5.3ms | 0 |
| Config Gateway | 13,996 | 12,801 | 13.4k | 7.1ms | 0 |

网关损耗：

```text
18.7k -> 13.4k = 28%
```

与上一轮 Debug 口径对比：

| 接口 | Debug | Release | 变化 |
|------|------:|--------:|------|
| Health | 156k | 115k | -26% |
| Redis | 42.6k | 17.7k | -58% |
| MySQL | 14.0k | 13.5k | -4% |
| Config Direct | 15.9k | 18.7k | +18% |
| Config Gateway | 12.5k | 13.4k | +7% |

解读：

- 本轮所有接口 `errors=0`，功能和稳定性优先结论成立。
- Config Direct/Gateway 均较上一轮上升，Gateway 损耗约 28%，仍落在当前文档记录的 21-29% 正常区间内。
- 压测环境与上一轮一致，因此 Health/Redis 应按同环境回退记录：Health 下降 26%，Redis 下降 58%。这两项需要单独定位，不能用网关链路提升来抵消。
- 当前已确认 Config Direct/Gateway 提升、MySQL 基本持平；待定位项集中在本地 health 快路径和 Redis 业务路径。

### 内存与进程状态

通过工作目录筛选 ASIO 进程，避免误匹配 VM 上另一个 Go `server` 进程：

```text
26303|/mnt/mac/Users/mac/code/croot/asio_owen/build|./server
```

巡检结果：

| 指标 | 值 |
|------|---:|
| RSS | 120,276 KB |
| VmRSS | 120,276 KB |
| VmHWM | 121,528 KB |
| VmSize | 3,673,712 KB |
| Private_Dirty | 113,568 KB |
| Threads | 58 |
| fd count | 123 |
| VM available memory | 8,034 MB |
| Swap used | 0 |

结论：

- RSS 约 117-120 MB，和 7/12 的 116 MB 基线同量级。
- 两次间隔 5s 采样 RSS 均为 120,276 KB，未见即时增长。
- 线程数 58 高于 7/12 的 41，和当前 worker/thread-pool 配置、后台 maintain、日志线程、gdb attach 时额外线程观察口径有关；需要长期趋势判断，不单看一次绝对值。

### 日志巡检

`server.log` 尾部表现：

- MySQL maintain 稳定：`total=10, idle=10`
- Redis maintain 稳定：`total=8, idle=8`，周期性 recycle/add idle connections
- Security rules 每 30s hot reload 正常
- 未发现 segfault、fatal、abort、terminate、ASAN/UBSAN 或 core 相关日志
- warn 仅来自本次人工无 JWT 访问 `/api/stats`、`/api/pool_stats`、`/api/http_pool_stats`，返回 `401 invalid jwt`，不是服务异常

### HTTP 复用巡检

最新稳定 HttpPool 日志：

```text
zebra-config={total=95, idle=95, active=0, in_flight=0,
reused=1596104, created=188, released_idle=1596292,
released_closed=0, released_bad=0}
```

计算：

| 指标 | 值 |
|------|---:|
| HttpPool total | 95 |
| idle | 95 |
| active / in_flight | 0 / 0 |
| reused | 1,596,104 |
| created | 188 |
| released_bad | 0 |
| released_closed | 0 |
| 复用率 | 99.988% |
| 平均每 created 连接承载请求 | 8,490 |

`ss` 也能看到 ASIO 进程到上游 `127.0.0.1:30001` 存在大量稳定 ESTABLISHED 连接，和 `HttpPool total=95` 一致。当前复用状态正常：`reused` 远大于 `created`，坏连接和 close 释放均为 0。

### 崩溃堆栈核查

核查项：

```text
/proc/sys/kernel/core_pattern = core
coredumpctl list = empty
/var/lib/systemd/coredump = empty
repo core files = empty
journalctl -k / dmesg segfault grep = empty
server.log crash grep = empty
```

结论：

- 当前 VM 未发现可用历史 core 文件、systemd coredump、kernel segfault 记录或服务日志中的崩溃记录。
- 因此没有“崩溃时堆栈”可还原。

对当前运行进程执行了 `gdb -batch -ex "thread apply all bt 8" -p 26303`。由于 Release 二进制已 stripped，只能看到系统调用层和地址；运行态线程栈显示：

- io_context 线程在 `epoll_wait`
- worker/thread-pool 多数线程在 `pthread_cond_wait`
- 没看到同步 socket write、异常处理卡住、崩溃或明显死锁栈

后续如果要保留可符号化崩溃栈，应在 VM 上启用 core dump，并保留未 strip 二进制或单独 debug symbols。

## 变更文件

代码：

- `src/app/snapshot_service.hpp`
- `src/app/app_config.hpp`
- `src/app/application.cpp`
- `src/http/http_server.hpp`
- `src/common/signal_exit.hpp`
- `src/db/mysql_pool.hpp`
- `src/db/mysql_connection.cpp`
- `src/http/http_pool.hpp`
- `src/http/client_session.hpp`
- `src/http/upstream_manager.hpp`
- `src/http/proxy_forwarder.hpp`
- `src/http/response_builder.hpp`
- `src/security/auth_whitelist.hpp`
- `src/security/path_normalize.hpp`
- `src/security/rate_limiter.hpp`
- `src/security/real_ip.hpp`
- `src/security/security_rules.hpp`

测试：

- `tests/test_jwt_auth.cpp`
- `tests/test_client_session.cpp`
- `tests/test_config_load.cpp`
- `tests/test_path_normalize.cpp`
- `tests/test_proxy_framing.cpp`
- `tests/test_proxy_forwarder.cpp`
- `tests/test_rate_limiter.cpp`
- `tests/test_response.cpp`
- `tests/test_security_chain.cpp`
- `tests/test_upstream_manager.cpp`

文档：

- `docs/CODE_REVIEW_FIXES_2026-07-13.md`
