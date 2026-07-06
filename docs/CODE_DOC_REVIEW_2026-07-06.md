# 代码与文档一致性 Review 报告

> Review 日期：2026-07-06
> Review 范围：`docs/` 文档、最近提交 `0489b11 wrk` / `44b2af3 wrk`、核心代码实现、压测与连接复用报告
> 当前分支：`master`
> 当前工作区：review 时为 clean

## 结论摘要

代码与文档在核心架构方向上基本一致：MySQL 仍通过 `asio::thread_pool` 包装同步 libmysqlclient，Redis 仍是 thread-local hiredis，无主动 keepalive PING；HTTP 网关具备上游连接池、响应分帧、hop-by-hop header 过滤、客户端 keep-alive、上游连接复用和热加载上游路由。

但并非完全一致。当前存在几类问题：

1. `HttpPool` 文档声称 `max_size` 是硬上限，但代码没有真正 enforce。
2. chunked 请求转发实现与 `GATEWAY_DESIGN.md` 不一致，且会在真实 chunked 请求下构造非法上游请求。
3. 安全文档声称“鉴权在 body 消费前执行”，代码实际在 body 完整消费后执行。
4. 路径规范化文档声称 query string 会剥离，代码实际没有剥离。
5. 最新 wrk 压测脚本存在复现问题，默认参数也与报告口径不完全一致。
6. `IDLE_TIMEOUT_TUNING.md` 的 60s SYN 数据存在 tcpdump 包数上限截断风险，调参效果结论不够严谨。

性能报告 `docs/PERF_2026-07-05.md` 的主结论基本成立，但适用范围应限定为报告中明确的版本状态：去掉 `Connection: keep-alive` 硬编码、`case_sensitive_paths = true`、Config 接口免鉴权、wrk 在 VM 本地压测。

连接复用报告的主结论成立：`SYN << 请求数`、`TIME_WAIT = 0`、`RST = 0` 足以支持“网关到 Go 后端 HTTP keep-alive 连接复用正常工作”。但 `idle_timeout_sec = 45` 的调参收益没有被严谨证明，需要重跑无包数上限或更高上限的 tcpdump。

## 最近提交口径

最近提交：

```text
0489b11 wrk
44b2af3 wrk
9f296d6 jwt
```

本次 review 以 `0489b11 wrk` 为最新基准。

`0489b11 wrk` 的关键变化：

- 新增 `docs/PERF_2026-07-05.md`
- 新增连接复用 tcpdump/ss 报告
- 更新 `docs/BENCH_STEPS.md`，压测主工具从 plow 切到 wrk
- 将 `config.d/21-http_pool.ini` 的 `idle_timeout_sec` 从 `60` 改为 `45`

`44b2af3 wrk` 的关键变化：

- 新增 `bench/bench.sh`、`bench/bench_full.sh`、`bench/verify.sh` 和 wrk Lua 脚本
- 去掉上游转发里硬编码的 `Connection: keep-alive`
- 转发过滤 header 增加 `proxy-connection`
- `case_sensitive_paths = true`
- Config 接口加入免鉴权白名单
- 上游路由热加载和安全热加载相关逻辑增强

## 验证动作

本地执行过以下验证：

```bash
bash -n bench/bench.sh
bash -n bench/bench_full.sh
bash -n bench/verify.sh
cmake -B build_codex_verify -S .
cmake --build build_codex_verify --target server
ctest --test-dir build_codex_verify --output-on-failure
```

结果：

- 三个 shell 脚本语法检查通过，但 `bench/bench_full.sh` 运行到 `config` 或 `all` 分支时会触发函数外 `local` 的运行时错误。
- `server` 在独立构建目录 `build_codex_verify` 编译通过。
- 编译有 OpenSSL 3 deprecated warning，集中在 `jwt_auth.hpp` 中 `RSA_new` / `RSA_free` / `RSA_set0_key` / `EVP_PKEY_assign_RSA` 等 API。
- `ctest` 未发现测试，因为当前仓库没有 `googletest/`，CMake 未启用 `tests/`。
- 现有 `build/` 目录是在 `/mnt/mac/Users/mac/code/croot/asio_owen/build` 路径生成的，直接在 `/Users/mac/code/croot/asio_owen/build` 下复用会触发 CMake cache path mismatch，因此本次使用独立目录验证。

## 代码与文档一致的部分

### MySQL 连接池

文档：`docs/DB_POOL_DESIGN.md`

实现：`src/db/mysql_pool.hpp`

一致点：

- MySQL 查询不跑在 `io_context` 上，而是 `asio::thread_pool` worker 执行。
- `execute()` 将 SQL 拷贝到 `char sql_buf[4096]`，避免 `std::string` 跨 worker 线程边界。
- `MysqlPool` 使用 `idle_pool_`、`total_`、`creating_`、`mtx_`、`cv_`。
- `creating_` 限制同一时刻最多一个 worker 建连，避免 thundering herd。
- maintain 跑在独立线程，不挂 `io_context`。
- release 时更新 `last_used_at` 并回到 idle 池。
- shutdown 先 `running_ = false`，通知等待者，再 join worker pool 和 maintain thread。
- 使用 `mysql_reset_connection()`，没有使用 SQL `RESET SESSION`。
- `read_timeout_ms` 只用于 `mysql_ping_with_timeout()`，并在 ping 后恢复为 0，避免影响业务查询。

需要注意：

- 文档中 acquire 伪代码提到新连接后 `mysql_ping()`，当前 acquire 新连接后只做 `mysql_reset_connection()`，没有额外 `mysql_ping()`。这通常可接受，因为新建成功后连接可用；但严格说文档和代码不完全一致。
- maintain 第三阶段检查 `max_check = 4`，文档示例是 `max_check = 8`。这是配置/文档差异，不是明显 bug。

### Redis 连接

文档：`docs/DB_POOL_DESIGN.md`

实现：`src/db/redis_pool.hpp`

一致点：

- Redis 保持 `thread_local redisContext*` 设计，没有共享锁池。
- `redisConnectWithTimeout` 设置建连超时。
- `cmd_timeout_ms > 0` 时通过 `redisSetTimeout` 设置命令超时。
- `cmd_timeout_ms = 0` 表示性能模式，不设置命令超时，和最近压测/排查文档一致。
- 连接断开通过 `ctx->err` 识别，下次 `get_conn()` 自动重建。
- 没有主动 keepalive PING。
- `get()` fast path 跳过通用 `cmd()` 的 `vsnprintf + std::string` 构造。

需要注意：

- 文档早期版本里默认 `cmd_timeout_ms = 1000`，最新配置 `config.d/11-redis.ini` 是 `cmd_timeout_ms = 500`。这是配置取舍，不影响设计一致性。

### 配置加载

文档：`docs/CONFIG_REFACTOR_PLAN.md`

实现：`src/common/config.hpp`

一致点：

- `Config::load("config.ini")` 只用 `config.ini` 定位 `config.d/` 目录。
- 实际按文件名排序加载 `config.d/*.ini`。
- 后加载覆盖先加载。
- 支持重复 key，通过 `raw_entries_` 保留原始顺序，供 `get_list()` / `get_section()` 使用。
- 构建时复制 `config.d/` 到构建目录，并排除 `99-local.ini`。

需要注意：

- `config.d/99-local.ini` 存在于工作区，但 CMake 复制到构建目录时会排除。生产/压测环境如果依赖本地覆盖，需要确认实际运行目录是否有对应覆盖文件。

### HTTP 网关主体

文档：`docs/GATEWAY_DESIGN.md`

实现：

- `src/http/http_server.hpp`
- `src/http/http_pool.hpp`
- `src/http/upstream_manager.hpp`

一致点：

- 本地路由优先，然后代理路由。
- 客户端 keep-alive 循环存在，HTTP/1.0 默认关闭，HTTP/1.1 默认保持。
- `Content-Length` 使用 `optional<size_t>`，能区分 `Content-Length: 0` 和没有 CL。
- 请求 framing 检查包含非法 CL、重复冲突 CL、TE 非 chunked。
- 响应支持 `Content-Length`、chunked、EOF framing。
- `status_text` 端到端保留。
- 上游响应 hop-by-hop header 过滤，`Content-Length` 和 `Transfer-Encoding` 由网关重新计算/控制。
- `ConnGuard` 持有 `shared_ptr<HttpPool>`，能保证热加载上游时飞行请求继续持有旧 pool。
- 复用 idle 上游连接失败时，对 stale idle 做一次 retry。

## 主要不一致与风险

### 1. `HttpPool::max_size` 不是硬上限

严重程度：High

文档说法：

- `HttpPool::max_size` 是 idle + active 上游 socket 总数硬上限。
- 超过后 acquire 应返回失败或等待，避免资源耗尽。

代码位置：

- `src/http/http_pool.hpp`

关键逻辑：

```cpp
if (shard.total >= state->cfg.max_size / kShards + 1) {
    // Per-shard limit reached; could try another shard, but for simplicity return nullptr
    // The caller should retry or create anyway since max_size is a soft limit per shard.
}
...
++shard.total;
++shard.in_flight;
```

问题：

- 判断后没有 `co_return nullptr`，没有等待，也没有换 shard。
- 注释本身也承认是 soft limit。
- 因此 `max_size` 当前不是文档声称的硬上限。

影响：

- 正常 wrk 100 并发压测不一定触发。
- 上游慢、上游挂住、客户端高并发、多个服务同时被打时，可能创建超过预期的上游连接。
- 资源安全结论不能按文档里的“硬上限保护”理解。

建议：

- 如果要保持文档设计，修代码：按全局或 shard 聚合 enforce `max_size`，达到上限时返回 503、等待，或尝试其他 shard。
- 如果保留当前行为，文档必须改成“每 shard soft cap，不保证全局硬上限”。

### 2. chunked 请求转发错误

严重程度：High

文档说法：

- 请求侧 chunked body 会按原始 chunked 字节转发给上游。
- 如果保留 `Transfer-Encoding: chunked`，body 必须仍然是 chunked framing。

代码位置：

- `src/http/http_server.hpp`

当前行为：

1. 读客户端 chunked 请求时，`read_chunked_stream()` 会去掉 chunk size、CRLF 和 trailer，只把 chunk data append 到 `ctx.body`。
2. 转发上游时仍保留 `Transfer-Encoding: chunked`。
3. 最终发送 `\r\n` + `ctx.body`，body 已不是 chunked 格式。

影响：

- 如果客户端发送真实 chunked 请求，上游会按 chunked 解析一段非 chunked body，可能卡住、400、超时或协议错乱。
- 当前 wrk POST 使用普通 Content-Length，因此压测报告未覆盖此问题。

建议：

- 方案 A：请求侧保留原始 chunked bytes，转发 `Transfer-Encoding: chunked`。
- 方案 B：网关 de-chunk 后，转发时移除 `Transfer-Encoding`，重新写 `Content-Length: <dechunked-size>`。
- 二选一即可，但代码和文档必须一致。

### 3. 安全链执行顺序与文档不一致

严重程度：Medium

文档说法：

- `GATEWAY_AUTH_DESIGN.md` 强调“尽早拒绝”。
- 鉴权、黑名单、限流在 body 消费之前执行。

代码位置：

- `src/http/http_server.hpp`

当前行为：

- 请求 header parse 后先完整消费 body。
- 然后才执行 `security_rules_->check(...)`。
- 代码注释明确写了：`Auth check (body already consumed, framing intact)`。

影响：

- 安全上更利于 keep-alive / pipeline 正确性，因为拒绝前已消费 body，不会把 body 当下一次请求头。
- 资源上不符合“尽早拒绝”，大 body 的无权限请求仍会先被读入。
- 对 DoS 防护目标有影响。

建议：

- 如果认可当前实现，更新文档：安全检查发生在 body 消费后，理由是保持 keep-alive/pipeline framing 安全；鉴权失败返回 401/403/429 后当前 4xx 会 `Connection: close`。
- 如果要实现文档目标，需在 body 前鉴权失败时强制 `Connection: close` 并不继续复用客户端连接，避免 pipeline 错位。

### 4. 路径规范化没有剥离 query string

严重程度：Medium

文档说法：

- `path_normalize` 输入是 path 部分，不含 query。
- query string 会剥离，日志也避免记录敏感 query。

代码位置：

- `src/security/path_normalize.hpp`
- `src/security/security_rules.hpp`

当前行为：

- `HttpServer` 将 picohttpparser 的 `path` 原样存入 `path_str`。
- HTTP request-target 可能包含 query string，例如 `/api/health?x=1`。
- `security_rules_->check()` 直接把 `raw_path` 传给 `normalize_path()`。
- `normalize_path()` 不处理 `?`。

影响：

- `/api/health?x=1` 规范化后会变成 `/api/health?x=1`，不会命中 `/api/health` 白名单。
- 路径黑名单、限流 path 维度也会被 query 干扰。
- 日志脱敏目标未完全满足。

建议：

- 在 `normalize_path()` 开头按 `?` 分割，只规范化 path 部分。
- 或在 `HttpServer` parse request 后拆出 path/query，`HttpContext` 分别保存。

### 5. `SecurityRules` 热加载不是文档里的读侧无锁 atomic shared_ptr

严重程度：Low/Medium

文档说法：

- 使用 `atomic<shared_ptr<const Rules>>`。
- 请求热路径读侧无锁。

代码现状：

- `SecurityRules` 使用 `rules_mu_` 保护部分字段。
- check 时复制 `trusted_proxies_` 和 `jwt_auth_`。
- `IpBlacklist`、`AuthWhitelist`、`PathBlacklist` 内部也有 mutex。
- `RateLimiter` 有 `cfg_mu_` 和分片 mutex。

影响：

- 不一定是性能问题，最近报告中安全场景是 Config 接口免鉴权，并且限流关闭。
- 但文档的“读侧无锁，不影响热路径 RPS”并不准确。

建议：

- 要么实现统一 `Rules` 快照 + atomic shared_ptr。
- 要么文档改成当前事实：细粒度 mutex + shared_ptr 容器替换，热路径有少量锁。

### 6. `bench/bench_full.sh` 运行时会失败

严重程度：Medium

代码位置：

- `bench/bench_full.sh`

问题：

- `config)` 分支和 `all)` 分支在函数外使用 `local BODY=...`。
- `bash -n` 不会报错，但实际运行到该分支会输出 `local: can only be used in a function`。

影响：

- 文档推荐 `bash bench/bench_full.sh` 跑全量压测，但当前脚本不能可靠执行全量。
- 最新性能报告可能是手工命令或不同脚本跑出来的，而不是该脚本可复现。

建议：

- 将 `local BODY=...` 改成 `BODY=...`。
- 顺便把默认 `THREADS` 从 10 改成 30，或文档明确必须 `THREADS=30 bash bench/bench_full.sh`。

### 7. `bench/verify.sh` 对 GET 接口使用 POST body

严重程度：Low

代码位置：

- `bench/verify.sh`

问题：

- `verify()` 函数无论 Health 还是 Config，都发送 `Content-Type` 和 `-d '{"appid"...}'`。
- curl 带 `-d` 默认是 POST。
- `bench/verify.sh health` 实际不是 GET health 验证。

影响：

- 当前 server 本地路由没有限制 method，因此可能仍返回 200。
- 但这不符合文档中的 GET health 语义，容易掩盖 method 处理问题。

建议：

- verify 函数增加 method/body 参数，GET 不带 `-d`。

### 8. wrk 脚本默认参数与报告不一致

严重程度：Low/Medium

报告口径：

- `docs/PERF_2026-07-05.md` 写明 `30 线程 / 100 连接 / 30s / timeout 10s`。

脚本默认：

- `bench/bench_full.sh` 默认 `THREADS=10`。
- `bench/bench.sh` 默认 `THREADS=10`。

影响：

- 不设置环境变量时，脚本结果不能直接复现报告。

建议：

- 默认 `THREADS=30`。
- 或在文档中所有正式压测命令都写 `THREADS=30 CONCURRENCY=100 DURATION=30s`。

## 性能报告结论核对

报告：`docs/PERF_2026-07-05.md`

关键数据：

| 接口 | 平均 RPS | avg_lat | Socket Errors |
|---|---:|---:|---:|
| Health | 95,955 | 0.96ms | 0 |
| Redis | 26,568 | 3.42ms | 0 |
| MySQL | 9,476 | 10.62ms | 0 |
| Config 直连 | 4,396 | 22.79ms | 0 |
| Config 网关 | 4,042 | 24.25ms | 0 |

网关效率：

```text
4,042 / 4,396 = 91.94%
```

判断：

- “最近提交状态下，wrk VM 本地压测性能达标”这个结论 OK。
- “无异常错误”基于报告中的上游和网关日志 0 error/timeout，也 OK。
- “无内存泄漏”基于非 ASAN RSS 稳定、Valgrind definitely/indirectly lost 为 0、ASAN 无报错，结论 OK。

限制：

- 报告明确说 Config 接口免鉴权，因此不代表 JWT 全链路性能。
- 报告中的 wrk 参数是 30 线程，仓库脚本默认是 10 线程，复现时必须显式指定或修脚本。
- 报告未覆盖 chunked 请求、带 query 的白名单/黑名单、安全拒绝大 body 等边界。

## 连接复用报告结论核对

报告：

- `docs/CONN_REUSE_VERIFY_2026-07-05.md`
- `docs/CONN_REUSE_TCPDUMP_2026-07-05.md`
- `docs/IDLE_TIMEOUT_TUNING.md`

关键证据：

- `TIME_WAIT = 0`
- `RST = 0`
- 新建连接数远小于请求数
- 估算复用率约 99%+
- 去掉硬编码 `Connection: keep-alive` 后，上游 HTTP/1.1 默认 keep-alive 仍然工作

判断：

- “连接复用正常工作”结论 OK。
- “去掉 `Connection: keep-alive` 硬编码后仍能复用”结论 OK。
- `docs/CONN_REUSE_TCPDUMP_2026-07-05.md` 对首次错误 BPF filter 的反思是合理的。

需要修正：

- `docs/IDLE_TIMEOUT_TUNING.md` 中 60s 压测用 `tcpdump -c 2000`，而最终纯 SYN 恰好是 2000。这个结果可能被 cap 截断。
- 因此“15s 与 60s 的 SYN/秒完全一致”不严谨。
- “`idle_timeout_sec = 45` 与 60 无显著差异”可以保留为暂定观察，但不应作为严格结论。

建议重跑：

```bash
timeout 70s tcpdump -i lo -nn -w /tmp/reuse_tuned_60s_full.pcap \
  "tcp dst port 30001 and (tcp[13] & 0x12 == 0x02)"
```

或将 `-c` 提高到明显不会截断的值，例如 20000+，并记录实际包数是否达到 cap。

## 优先修复清单

### P0/P1：应尽快修

1. 修 `HttpPool::max_size`，保证文档中的硬上限真实存在。
2. 修 chunked 请求转发：保留原始 chunked 或 de-chunk 后重写 Content-Length。
3. 修 `normalize_path()` 剥离 query string。
4. 修 `bench/bench_full.sh` 函数外 `local`。

### P2：建议修

1. 修 `bench/verify.sh`，GET 接口不要用 POST body。
2. 将 wrk 脚本默认 `THREADS` 改为 30，或者文档明确正式压测必须设置 `THREADS=30`。
3. 更新 `GATEWAY_AUTH_DESIGN.md` 中的安全链执行顺序，说明当前实现 body 后鉴权的原因，或改代码实现 body 前拒绝并强制关连接。
4. 更新 `GATEWAY_AUTH_DESIGN.md` 中热加载读侧无锁的表述。
5. 重跑 idle timeout tuning 抓包，避免 `-c 2000` 截断。

### P3：可后续处理

1. OpenSSL 3 deprecated warning 后续可用 EVP PKEY 新 API 替换 RSA deprecated API。
2. 为 `path_normalize`、chunked 转发、`HttpPool max_size` 增加单元测试。
3. 如果需要测试，考虑将 GoogleTest vendored 或改成可选 FetchContent，避免 `ctest` 无测试。

## 总体判断

当前代码可以支持最近报告里的 wrk 性能结论和连接复用结论；这两类报告的主结论可以认可。

但文档中关于资源硬上限、chunked 请求转发、安全链执行顺序、路径规范化、热加载无锁读等表述不完全匹配当前代码。若以这些文档作为上线验收标准，需要先修代码或修文档。

推荐下一步先修 4 个会影响真实行为的问题：

1. `HttpPool max_size` 硬上限。
2. chunked 请求转发。
3. query string 剥离。
4. wrk 脚本可复现性。

修完后重新跑：

```bash
cmake -B build_codex_verify -S .
cmake --build build_codex_verify --target server
THREADS=30 CONCURRENCY=100 DURATION=30s bash bench/bench_full.sh
```

并在 VM 上重跑连接复用 tcpdump，确认 `SYN << 请求数`、`TIME_WAIT = 0`、`RST = 0` 仍成立。
