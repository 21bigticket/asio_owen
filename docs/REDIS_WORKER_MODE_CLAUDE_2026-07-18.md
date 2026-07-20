# Redis Worker 模式现状调查与文档勘误记录

> 调查日期：2026-07-18
> 调查人：Claude
> 触发：`./bench.sh redis` 压测后核实 Redis 实际执行模式
> 结论先行：**线上 Redis 当前运行在 worker 模式**，但 `CLAUDE.md` 的 DB 层描述仍停留在旧的 direct 模式，属于文档漂移。本文档仅记录现状与漂移清单，**不修改任何现有文件**。

---

## 0. TL;DR

- `RedisPool` 是 **dual-mode 架构**（`direct` / `worker`），`src/db/redis_pool.hpp:27-34`。
- 实际配置 `config.d/11-redis.ini:10` 显式 `mode = worker`，经 `app_config.hpp:76-90` 读入、`application.cpp:80` 构造，**线上即 worker 模式**。
- worker 模式 = 专用 `asio::thread_pool`（16 线程）+ 共享连接池（min 4 / max 32）+ 锁（`mtx_`/`cv_`）+ 维护线程（PING 探活）。
- 这与 `MysqlPool` 的执行策略**已趋同**（都 post/exec 到专用线程池跑同步客户端），不再是 CLAUDE.md 所述的 "deliberately different"。
- `CLAUDE.md` 的 DB 层段落过时，需更新（见 §7）。`PERF_REPORT.md` 同样含 Redis direct 模式旧描述，也需更新（见 §7.2）。

---

## 1. dual-mode 架构总览

`redis_pool.hpp:27-34`：

```cpp
// RedisPool v2: dual mode (direct / worker)
// - direct: thread-local dedicated connection, lock-free, fastest path.
// - worker: shared connection pool + dedicated thread pool, isolates sync hiredis from io_context.
class RedisPool {
public:
    enum class Mode { Direct, Worker };
```

| 维度 | Direct 模式 | Worker 模式（**当前默认/线上**） |
|---|---|---|
| 执行线程 | io_context 线程直接调用 hiredis | `co_await asio::post(*worker_pool_)` 切到专用线程池 |
| 连接持有 | 每 io 线程一个 `thread_local` 连接（`tls_`） | 共享连接池 `idle_pool_`，acquire/release |
| 同步原语 | 无锁 | `mtx_` + `cv_` |
| 跨线程共享 | 无 | 有（worker 线程共享池） |
| 容量调度 | 固定 = io 线程数 | min_size/max_size/max_creating/acquire_timeout |
| 活性探测 | 命令失败时重连 | maintain 线程 PING + 命令级重试 |
| struct 默认 | `Mode mode = Mode::Direct`（`:42`） | — |

`Mode` 的 struct 默认是 `Direct`（`:42`），但被配置覆盖（见 §3）。

---

## 2. 实际生效配置（`config.d/11-redis.ini` 全值）

```ini
[redis]
host = 127.0.0.1
port = 6379
db = 0
connect_timeout_ms = 1000
cmd_timeout_ms = 500
mode = worker

# worker mode only; ignored when mode = direct
min_size = 4
max_size = 32
max_idle_sec = 120
worker_threads = 16
max_creating = 0
acquire_timeout_ms = 3000
```

字段语义与代码对应：

| 配置键 | 值 | 代码默认 | 作用 |
|---|---|---|---|
| `mode` | **worker** | direct | 模式总开关 |
| `cmd_timeout_ms` | 500 | 1000(app_config) / ≤0→30000(validate) | 单命令超时；`effective_cmd_timeout_ms()`（`:297`）取 500 |
| `min_size` | 4 | 4 | 预热连接数 + maintain 补足下限 |
| `max_size` | 32 | 32 | 连接池上限 |
| `max_idle_sec` | 120 | 120 | idle 连接超期回收阈值 |
| `worker_threads` | 16 | 16 | 专用线程池大小 |
| `max_creating` | 0 | 0 | 0 表示按公式自动算（见下） |
| `acquire_timeout_ms` | 3000 | 3000 | 取连接等待上限，超时返回 nullptr |

`max_creating=0` 时由 `compute_max_creating_limit`（`:652-661`）自动推导：
`min(4, max(1,max_size/8), max(1,worker_threads/2))` → 本配置 = `min(4, 4, 8)` = **4**。

---

## 3. 配置读取与构造链路

```
config.d/11-redis.ini
   └─ app_config.hpp:76-97   读 [redis] 段 → RedisPool::Config（mode 映射在 :76-82）
        └─ application.cpp:80  redis_ = make_unique<RedisPool>(ioc_, app_cfg.redis);
             └─ RedisPool 构造（:55-68）→ validate_config → init_worker_mode（:63）
                  └─ 注入路由 application.cpp:94  .redis = redis_.get()
```

**mode 字符串映射**（`app_config.hpp:76-82`）：
- `"worker"` / `"WORKER"` → `Mode::Worker`
- `"direct"` / `"DIRECT"` → `Mode::Direct`
- 其它任意值 → `LOG_WARN("invalid redis.mode ...")` 并 fallback `Direct`

**校验**（`validate_config` `:274-295`）：worker 模式下补齐 `max_size`(32)、`worker_threads`(4)、`max_idle_sec`(120)、`acquire_timeout_ms`(3000) 的缺省值，并钳 `min_size <= max_size`。

---

## 4. Worker 模式运行机制（详细）

### 4.1 启动（`init_worker_mode` `:301-319`）

1. `max_creating_limit_ = compute_max_creating_limit(cfg_)` —— 并发建连上限（本配置 = 4）。
2. `worker_pool_ = make_unique<asio::thread_pool>(cfg_.worker_threads)` —— **16 线程专用池**，与 io_context 的 N 个线程完全隔离。
3. 预热：循环 `min_size`(4) 次 `create_connection()` 成功的推入 `idle_pool_`，`total_` 计数。
4. 起 `maintain_thread_`（`std::thread`）跑 `maintain_loop`。

### 4.2 命令执行路径（executor 切换，**非 post(lambda)**）

`cmd_argv`（`:111-127`）：

```cpp
if (cfg_.mode == Mode::Direct) {
    co_return cmd_argv_sync_impl(std::move(args));   // direct: 不切换
}
// Worker mode: switch executor only — args stays in the coroutine frame.
// Capturing non-POD into a post() lambda triggers GCC 11 coroutine UAF;
co_await asio::post(*worker_pool_, asio::use_awaitable);
if (!running_) co_return make_error(...);
co_return cmd_argv_sync_impl(std::move(args));
```

`get`（`:129-144`）同理，`key_copy` 保留在协程帧里再切换。

> **关键设计点**：用 `co_await asio::post(executor)` 切换执行线程，而不是 `asio::post(pool, [args]{...})` 捕获 lambda。后者会在 GCC 11 上触发协程帧 UAF（捕获非 POD 跨 post 边界）。详见 `docs/REDIS_POOL_LAMBDA_FIX_2026-07-12.md`，也是 memory [[feedback-asio-post-lambda-gcc11]] 记录的同一类坑。

`cmd_argv_sync_impl`（`:153-204`）实际跑同步 hiredis：
- `is_readonly_idempotent(args.front())` 判断是否可重试（白名单见 §4.6）。
- `acquire_conn()` → `ConnectionGuard`（RAII，析构自动 release 或 drop）→ `RedisCommandArgv`（**二进制安全，无格式串重解析**）。
- reply 为空且可重试 → `guard.drop()` 丢弃坏连接 + `continue` 换一条连接重试一次。
- 非幂等命令**永不自动重试**（首次可能已到 Redis，重放会双写）。

### 4.3 连接 acquire / release —— commit `f2b2d22` 的核心优化

`acquire_worker`（`:363-420`）取连接时**不再发 SELECT/PING 探测**（`:376-382` 注释）：

```cpp
// No per-acquire SELECT/PING probe: db is fixed (switching
// requires a restart) and set once at connect time, so the
// connection's db state never drifts. Liveness is covered by
// the maintain-loop PING and by command-level retry on
// failure (see cmd_argv_sync_impl). This removes one Redis
// round-trip per command — the main worker-mode throughput
// cost vs direct mode.
```

即 `f2b2d22 perf(redis): drop per-acquire SELECT in worker mode, add idempotent retry` 的落地：
- **之前**：每次 acquire 发一个 SELECT（或 PING）探活确认连接可用 → 每命令多一次 Redis 往返，这是 worker 模式相对 direct 的主要吞吐开销。
- **之后**：db 在 `create_connection` 时设一次（`RedisConnectionConfig.db`，`:642-650`），运行期不变；活性交给 maintain 线程的 PING + 命令失败时的幂等重试覆盖。省掉每命令一次往返。

acquire 完整流程：
1. 锁内查 `idle_pool_`，非空则 pop front 返回。
2. 空且 `total_ < max_size && creating_ < max_creating_limit_` → 置 `should_create`，锁外 `create_connection()`（建连慢，不放锁内）。
3. 否则 `cv_.wait_until(deadline)`，超时（`acquire_timeout_ms`）返回 nullptr + `inc_acquire_timeout`。
4. 建连失败 → `--total_`、`notify_all`、最多重试 2 轮。

`release_worker`（`:422-427`）：推回 `idle_pool_` 尾部 + `cv_.notify_one()`。
`drop_bad_connection`（`:429-445`）：worker 分支 `redisFree` + `--total_` + `notify_all`。

### 4.4 维护循环（`maintain_loop` / `do_maintain` `:475-592`）

`maintain_thread_` 每 30s（`cv_.wait_for(30s)`）执行 `do_maintain`，三件事：
1. **回收超期 idle**：`age >= max_idle_sec`(120s) 的连接 `redisFree` + `--total_`，记 `add_idle_recycled`。
2. **补足 min_size**：若 `idle_pool_.size() < min_size`，在 `max_size` 余量内补建。
3. **PING 探活**：最多取 `max_check=4` 个 idle 连接发 `redisCommand(ctx,"PING")`，非 PONG 视为死连接清理（`inc_ping_fail`）。

> 这是 acquire 阶段取消探测后，连接活性的**后台兜底**——前台命令路径不再为探活付费，死连接由后台批量发现 + 命令级幂等重试吸收。

### 4.5 关闭（`shutdown_worker_mode` `:321-336`）

顺序：`cv_.notify_all` → `join(maintain_thread_)` → `join(worker_pool_)` → 锁内释放全部 idle。
即先停维护线程，再 join worker 池（**等所有在飞命令跑完**），最后释放连接。`Application::cleanup()`（`application.cpp:138-139`）调用 `redis_->shutdown()` + `reset()`。

### 4.6 重试语义（`is_readonly_idempotent` `:459-473`）

只读、无副作用命令白名单，命令级失败可换连接重试一次：

```
GET, MGET, STRLEN, EXISTS, TTL, PTTL, TYPE,
HGET, HMGET, HGETALL, HKEYS, HVALS, HLEN, HEXISTS,
LLEN, LRANGE, LINDEX, SCARD, SISMEMBER, SMEMBERS,
ZCARD, ZSCORE, ZRANGE, ZRANK
```

显式排除所有写命令（SET/EXPIRE/INCR/GETDEL/GETEX/…）：首次失败可能命令已到 Redis，重放会双写。大小写不敏感匹配（动词 toUpper 后比较）。

> 与 gateway 层 [[http-pool-vuln-fixes-2026-07-14]] 中 VULN-2 "非幂等误重试" 的修复同源思路：**只对确认幂等的操作重试，其余绝不自动重放**。

---

## 5. Direct 模式现状（残留，非默认）

代码完整保留，`mode = direct` 时走：
- `cmd_argv`/`get` 不切 executor，直接在 io_context 线程调 `*_sync_impl`。
- `acquire_direct`（`:353-361`）→ `ensure_redis_tls_connection`：每 io 线程一个 `thread_local TlsRedisConn tls_`（`:679`），lock-free。
- 命令失败时 `ensure_redis_tls_connection` 内重连。

即 CLAUDE.md 当前描述的那套行为 **= direct 模式**，它仍存在、仍可配，但**不再是线上配置**。保留它的理由：lock-free 最快路径，适合 hiredis 调用极快、单机 io 线程数固定的场景；以及 GCC 11.4 协程 ICE 的 fire-and-forget 调用点兼容（`cmd_argv_sync` 同步兼容路径，`:146-150`）。

---

## 6. 与 MysqlPool 的对比（策略已趋同）

| 维度 | MysqlPool | RedisPool (worker) |
|---|---|---|
| 异步策略 | `asio::post` 到专用 `thread_pool` | `co_await asio::post(*worker_pool_)` 切 executor |
| 专用线程池 | `worker_threads` 默认 **32**（`app_config.hpp:72`） | `worker_threads` 默认 **16** |
| 连接池 | min/max/acquire_timeout/max_creating | 同语义 |
| 同步原语 | mutex + condition_variable | `mtx_` + `cv_` |
| 跨线程传 SQL | `char sql_buf[4096]` 栈数组（见 CLAUDE.md 警告） | args 留协程帧，不跨 post 边界捕获 |
| 探活 | — | maintain 线程 PING |
| 重试 | — | 只读幂等命令重试一次 |

**结论**：两个池现在都是 "post/exec 到专用线程池跑同步客户端" 的同一套策略。CLAUDE.md "two pools use **deliberately different async strategies** — this is the core architectural decision" 的论断已不成立。

> 注意：CLAUDE.md 里 MysqlPool 那段 `char sql_buf[4096]` 的 "do not regress" 警告**仍然有效**（MySQL 侧未变，且 Redis 侧的 executor 切换正是为规避同类 UAF）。更新文档时这段应保留。

---

## 7. ⚠️ 文档漂移清单（只记录，不改）

### 7.1 `CLAUDE.md` — DB layer 段（**过时，需更新**）

**当前原文**（`CLAUDE.md` "DB layer (`src/db/`)" 小节）：

> The two pools use **deliberately different async strategies** — this is the core architectural decision and the source of past crashes:
> - **`MysqlPool`** wraps the *synchronous* libmysqlclient API (`mysql_query`/`mysql_store_result`) by `asio::post`-ing each query onto a dedicated `asio::thread_pool` (sized to `pool_size`). ...
> - **`RedisPool`** calls synchronous `redisCommand` **directly on the io_context thread** (no thread_pool). This is safe because (a) hiredis calls are microseconds-fast and (b) each io_context thread holds its own `thread_local redisContext*` (`tls_conn_`), so there are no locks and no cross-thread sharing.

**过时点逐条**：

| 原文论断 | 现状（worker 模式） | 代码依据 |
|---|---|---|
| "deliberately different async strategies ... core architectural decision" | 两者策略趋同，都是专用线程池 | `redis_pool.hpp:122` vs MysqlPool 的 post |
| "`RedisPool` calls `redisCommand` directly on the io_context thread (no thread_pool)" | 经 `asio::post(*worker_pool_)` 切到 16 线程专用池 | `redis_pool.hpp:119-122` |
| "each io_context thread holds its own `thread_local redisContext*` (`tls_conn_`)" | `tls_` 仅 direct 模式使用；worker 用共享 `idle_pool_` | `redis_pool.hpp:340-344, 679` |
| "there are no locks and no cross-thread sharing" | worker 有 `mtx_`/`cv_`，worker 线程共享连接池 | `redis_pool.hpp:363-420, 675-676` |
| `tls_conn_` 命名 | 实际成员名是 `tls_`（类型 `TlsRedisConn`） | `redis_pool.hpp:679` |

**建议改法**（待用户批准后另行执行）：把该段改写成 dual-mode 架构说明，明确：
- Redis 当前默认 worker（专用 16 线程池 + 共享连接池 + 锁），与 MysqlPool 策略趋同；
- direct 是可选的 lock-free 最快路径（`mode=direct`），保留用于特定场景与 GCC 11.4 兼容；
- 两者都靠 "executor 切换 / 栈数组" 规避 GCC 11 协程 UAF，"do not regress" 警告对两边都适用（链接 `docs/REDIS_POOL_LAMBDA_FIX_2026-07-12.md`）。

### 7.2 `PERF_REPORT.md` — ⚠️ 同样漂移,需更新（本条更正）

**更正**:此前误判为"无需改"。复核 `docs/PERF_REPORT.md` 实含 Redis direct 模式旧描述,与 §7.1 的 CLAUDE.md 同源漂移:

| 位置 | 原文论断 | 现状（worker 模式） |
|---|---|---|
| `PERF_REPORT.md:36` | Redis "直接在 io_context 线程执行" | 经 `asio::post(*worker_pool_)` 切到 16 线程专用池 |
| `PERF_REPORT.md:49-52` | "直接执行：不经过线程池" / "每个 io_context 线程持有一条独立 `thread_local redisContext*`" / "无锁设计" | worker 用共享 `idle_pool_` + `mtx_`/`cv_`,`tls_` 仅 direct 模式使用 |
| `PERF_REPORT.md:329` | `tls_map_ → 裸指针`（Redis 优化项，仍描述 thread_local 直连） | direct 模式残留描述 |

更新方向与 §7.1 一致：注明 Redis 默认 worker 模式，direct 为可选路径。

### 7.3 其它 docs（已存在且仍准确，本文档不重复其内容）

- `docs/REDIS_POOL_LAMBDA_FIX_2026-07-12.md` —— `redis_pool.hpp:121` 注释引用，记录 GCC 11 post(lambda) UAF 修复，仍准确。
- `docs/REDIS_POOL_REVIEW_CONFIRMATION_2026-07-11.md` —— 较早的 Redis 池复核，注意其时点可能仍是 direct/旧 worker，引用前应核对。
- `docs/SUMMARY_2026-07-12.md` —— 阶段总结，`redis_pool.hpp:121` 同样引用。

---

## 8. 性能基线（2026-07-18 `./bench.sh redis`）

| 轮次 | RPS | avg 延迟 | max 延迟 | 错误 |
|---|---|---|---|---|
| Redis #1 | 30929.59 | 2.95ms | 32.63ms | 0 |
| Redis #2 | 30655.95 | 2.97ms | 38.39ms | 0 |

两轮 RPS 波动 < 1%、延迟几乎一致，worker 模式 + 连接池 + acquire 不探测的组合表现稳定。`f2b2d22` 省掉每命令一次 SELECT 往返的效果在此基线中已体现（否则 2.95ms 延迟里会多一个 RTT）。

> 参考量级：同次压测 Health（纯本地）142k RPS、MySQL 15k RPS、Config Gateway 14.6k RPS。Redis 30.8k RPS 介于纯本地与含上游往返之间，与其 "本地同步调用 + 单次 Redis RTT" 的预期吻合。

---

## 9. 相关 commit

- `f2b2d22 perf(redis): drop per-acquire SELECT in worker mode, add idempotent retry` —— §4.3、§4.6 的来源。
- `4704813 test(redis): cover RedisCommandArgv owning data from a temporary vector` —— `RedisCommandArgv` UAF 防护测试（`redis_pool.hpp:179` 使用点），见 memory [[feedback-redis-command-argv-uaf-2026-07-15]]。

---

## 10. 待办（需用户批准）

- [ ] 按 §7.1 更新 `CLAUDE.md` 的 DB layer 段为 dual-mode 现状（本文档已备齐改写依据）。
- [ ] 可选：在 `CLAUDE.md` "Known runtime endpoints" 或架构段补一句 Redis 默认 worker 模式，避免后续读者重复走本次调查路径。
- [ ] 本文不触发任何代码改动；如需切换为 direct 模式做 A/B 压测，改 `config.d/11-redis.ini:10` 即可（重启生效）。
