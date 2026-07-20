# 智能指针 + 协程全盘 Review 报告

> 范围:`asio_owen` 全仓库智能指针(`unique_ptr`/`shared_ptr`/自定义 deleter)与 C++20 ASIO 协程(`co_await`/`co_spawn`/`awaitable`)的内存安全、生命周期、并发正确性审计。
>
> 方法:**主审(Claude)亲自精读 DB 层 + 3 个并行 agent 分审 HTTP/Security/App 层 + 主审对全部 P1 级发现做源码交叉验证**。
>
> 日期:**2026-07-19**

---

## 0. 执行摘要

| 严重性 | 数量 | 说明 |
|---|---|---|
| P0 | **0** | 无确定性 crash / UAF / 数据竞争 |
| P1 | **2**(另有 1 条 agent 报告的已驳回) | 均需 OOM 触发,但后果是进程崩 / 池子永久卡死 |
| P2 | **8** | 热重载原子性、跨线程 acceptor、detached terminate、dead code 等 |
| P3 | **10** | RAII 风格不一致、dead code、健壮性边界 |
| 文档漂移 | **4 处** | `CLAUDE.md` 描述与现状不符 |

**核心结论**:智能指针与协程的**主干设计是干净的**。历史教训(`asio::post(lambda)` 捕获非 POD 的 UAF、`RedisCommandArgv` 临时量悬垂)均已正确修复并落实。剩余问题集中在三类边界:

1. **OOM 路径的异常安全**——析构 noexcept 调用可抛操作、计数器在 try 块外分配后不回滚;
2. **热重载原子性**——`SecurityRules::load_from_config` 多步发布非原子;
3. **detached 协程的异常兜底缺失** + **跨线程 `acceptor` 操作的严格性**。

交叉验证价值:HTTP agent 报告的 **P1-3(SecurityRules 热重载数据竞争)经源码核验为误报**——三个子安全模块各有内部 mutex,无竞争。详见 §3.3。

---

## 1. 审计方法与分工

| 层 | 文件 | 审计者 |
|---|---|---|
| DB | `mysql_pool.hpp` / `redis_pool.hpp` / `redis_connection.hpp` / `redis_command.hpp` | 主审(第一手精读) |
| HTTP | `http_pool.hpp` / `http_server.hpp` / `client_session.hpp` / `proxy_forwarder.hpp` / `http_io.hpp` / `http_body_reader.hpp` / `http_context.hpp` / `upstream_manager.hpp` | agent A |
| Security | `security_rules.hpp` / `jwt_auth.hpp` / `ip_blacklist.hpp` / `auth_whitelist.hpp` / `path_blacklist.hpp` | agent B |
| App/Common | `application.hpp`/`.cpp` / `routes.cpp` / `timeout.hpp`(不存在,见 §7)/ `logger.hpp` / `main.cpp` | agent C |

**交叉验证**:主审对 agent 报告的所有 P1 发现回到源码逐条核验(§3),并裁定 P1-3 为误报。

**严重性定义**

- **P0**:确定性 crash / UAF / 数据竞争,正常负载即触发。
- **P1**:严重后果(crash / 池子卡死 / 安全旁路),需特定条件(高并发 / 热重载 / OOM)触发。
- **P2**:健壮性 / 正确性隐患,特定路径下出问题,通常不 crash。
- **P3**:代码质量 / 风格 / 文档,不影响正确性。

---

## 2. 验证干净的核心设计(先给信心)

这些是 C++ 协程服务器最常踩的坑,本仓库经多轮修复后**已确认正确**,改动时勿回归:

### 2.1 `asio::post` 已全部 executor 化,避开 GCC 11 协程 UAF

历史教训:`asio::post(lambda)` 捕获非 POD 对象(`std::string`/`std::function`)在 GCC 11 协程下必触发 UAF。现状三处全部改为 executor 切换:

- `src/db/mysql_pool.hpp:117` —— `co_await asio::post(worker_pool_, asio::use_awaitable);` 随后 `do_query(sql.c_str())`,`sql` 留在协程帧。
- `src/db/redis_pool.hpp:122` —— Worker 模式 `cmd_argv`:`co_await asio::post(*worker_pool_, asio::use_awaitable);` 随后 `cmd_argv_sync_impl(std::move(args))`,`args` 留帧。
- `src/db/redis_pool.hpp:139` —— Worker 模式 `get`:`std::string key_copy(key);` 留帧,再切 executor。

> **文档漂移**:`CLAUDE.md` 仍称 `MysqlPool::execute` 把 SQL 拷进 `char sql_buf[4096]` 并捕获进 post lambda。实际已废弃,见 §7。

### 2.2 `routes.cpp:57` 的 `asio::post(ex, lambda)` 是安全写法

`/api/combo` 的 cache-fill write-back:
```cpp
auto ex = co_await asio::this_coro::executor;
auto redis = services.redis;
asio::post(ex, [redis, data]{ set_cache(redis, data); });
```
这是 **fire-and-forget**(非 `co_await asio::post(lambda, exec, use_awaitable)`),**不在 GCC 11 UAF 模式内**。lambda 按值捕获 `string` 与裸指针,单线程消费;关停流程下 `io_context` 停后才销毁 `redis_`,已 queued 的 lambda 随 `ioc_` 析构被销毁但不被 `operator()` 调用,无悬垂解引用。仓库 `docs/REDIS_POOL_LAMBDA_FIX_2026-07-12.md` / `SUMMARY_2026-07-12.md` 已明确记载此为 GCC 11 ICE 的安全绕过。

### 2.3 `RedisCommandArgv` 深拷贝持有数据 + 禁 copy/move

`src/db/redis_command.hpp`:
```cpp
struct RedisCommandArgv {
    std::vector<std::string> args;          // 拥有数据
    std::vector<const char*> argv;          // 指向 args[i].c_str()
    std::vector<size_t> argv_len;
    explicit RedisCommandArgv(const std::vector<std::string>& in);
    RedisCommandArgv(const RedisCommandArgv&) = delete;
    RedisCommandArgv& operator=(const RedisCommandArgv&) = delete;
    RedisCommandArgv(RedisCommandArgv&&) = delete;          // 防 argv 指向被 move 走的 args
    RedisCommandArgv& operator=(RedisCommandArgv&&) = delete;
};
```
修复了历史 UAF(临时 `vector` 传入即悬垂,症状为 ~0.5% "ERR unknown command ``")。

### 2.4 `ClientSession` 不继承 `enable_shared_from_this`,但生命周期正确

`src/http/http_server.hpp:75-84` 用 `co_spawn(..., [session](std::exception_ptr ep){...})` 捕获 `shared_ptr<ClientSession>` 延长生命周期,效果与 `enable_shared_from_this` 等价。`socket`、by-value 参数、栈 buffer 均在协程帧内,`co_await` 挂起期间有效。

### 2.5 `HttpPool` hot-reload 代际续命

- `acquire` 协程开头 `auto state = state_;`(局部 `shared_ptr<State>` 副本),之后不再访问 `this`。`UpstreamManager::reload` 整体在 `unique_lock` 下替换 `pools_[name] = make_shared<HttpPool>(...)`,旧 pool 靠在途请求持有的 `shared_ptr` 续命到最后一帧析构。`route()` 在 `shared_lock` 下取 `shared_ptr`,同步性受锁保护。
- `ConnGuard` 禁拷贝禁 move(`proxy_forwarder.hpp:37-40`),避免 double release。

### 2.6 `Application::cleanup()` 析构顺序正确

```cpp
void Application::cleanup() {
    reload_service_->stop(); pool_stats_service_->stop();
    snapshot_service_->stop(); server_->stop();
    signal_exit_.reset(); reload_service_.reset();
    pool_stats_service_.reset(); snapshot_service_.reset();
    server_.reset();
    mysql_->shutdown(); redis_->shutdown();
    redis_.reset(); mysql_.reset();
    drain_timer_.reset(); security_rules_.reset();
}
```
- 持引用方(`reload`/`pool_stats`/`snapshot`)先死于被引用方(`server_`/`security_rules_`)。
- `server_` 先于 `mysql_`/`redis_`。
- `state_` 经 `shared_ptr` 续命至最后一个 `ClientSession` 帧析构。
- `drain_timer_` 在 `ioc_` 之前 reset(成员逆序自动保证),`async_wait` lambda 随 timer 销毁,`[this]` 不被解引用。
- `request_stop()` 的 `stop_requested_.exchange(true)` 幂等。

### 2.7 `with_timeout` / `parallel_group` 取消语义

`read_with_timeout` / `write_with_timeout` 用 `parallel_group` + `wait_for_one` 正确取消未完成操作并**等待取消完成**,buffer 不会被异步操作在 `co_return` 后继续使用。

### 2.8 三个子安全模块 reload 无撕裂

`ip_blacklist` / `auth_whitelist` / `path_blacklist` 均为:**`make_shared` 新对象在锁外构造 → `lock_guard` 下整体 `std::move` 替换 shared_ptr 成员**。读者持同一把内部 mutex 读 shared_ptr 并 dereference,引用计数保证旧对象在读者手里活着。无指针撕裂。**这是 §3.3 驳回 P1-3 的依据。**

### 2.9 DB 层 RAII 全覆盖

`ConnectionGuard`、`MysqlResultPtr`(`unique_ptr<MYSQL_RES, mysql_free_result>`)、`RedisReplyGuard`、`RedisContextPtr`(`unique_ptr<redisContext, redisFree>`)、`jwt_auth.hpp` 中 `BIGNUM`/`EVP_PKEY`/`OSSL_PARAM`/`OSSL_PARAM_BLD`/`EVP_PKEY_CTX` 的自定义 deleter —— 除 §5.3 列出的两处遗漏外,均正确。

---

## 3. P1 详解

### 3.1 P1-1 ｜ `~ConnGuard` 隐式 noexcept 调用可抛操作 → 进程 terminate

**分类**:智能指针 / RAII / 异常安全
**位置**:`src/http/proxy_forwarder.hpp:42-50` + `src/http/http_pool.hpp:289-330`

**问题**:`~ConnGuard` 无显式 `noexcept`,默认析构为 `noexcept(true)`(其成员 `shared_ptr`/`unique_ptr`/`bool` 析构均 noexcept)。但它调用的 `HttpPool::release`(静态)在 `http_pool.hpp:322-327` 内部执行 `shard.idle.push_back(std::move(*conn))`,可抛 `std::bad_alloc`;`std::lock_guard lock(shard.mtx)` 构造亦可抛 `std::system_error`。一旦 `release` 在析构链路里抛出 → `std::terminate`,**整个进程崩**。

```cpp
// proxy_forwarder.hpp:42-50
~ConnGuard() {                              // 隐式 noexcept(true)
    if (!conn_) return;
    HttpPool::untrack_active(pool_state_, conn_.get());
    if (good_) {
        HttpPool::release(pool_state_, std::move(conn_));   // 内部 push_back 可抛
    } else {
        HttpPool::release_bad(pool_state_, std::move(conn_));
    }
}

// http_pool.hpp:322-327
{
    std::lock_guard lock(shard.mtx);
    shard.active.erase(conn.get());
    shard.idle.push_back(std::move(*conn));   // ← bad_alloc 在此抛
    --shard.in_flight;                         // 被跳过 → 计数器也泄漏
}
release_in_flight(state);                       // 被跳过 → in_flight_count 泄漏
```

**触发场景**:系统内存紧张(`deque` 扩容分配失败)。每个 `ClientSession` 请求结束/重试 `continue` 都走这条析构路径,属热路径。

**后果**:进程 terminate + `in_flight` 计数器泄漏(但 terminate 已是最坏)。

**修复建议**:
```cpp
~ConnGuard() {
    if (!conn_) return;
    try {
        HttpPool::untrack_active(pool_state_, conn_.get());
        if (good_) HttpPool::release(pool_state_, std::move(conn_));
        else        HttpPool::release_bad(pool_state_, std::move(conn_));
    } catch (const std::exception& e) {
        LOG_ERROR("ConnGuard release failed, dropping connection: ", e.what());
        // 兜底:确保计数器/active 不泄漏 —— 调用一个绝不抛的 close-and-drop
    }
}
```
或更彻底:在 `HttpPool::release` 内部 `catch (const std::bad_alloc&)` 回退到 `release_bad` 路径(close-and-drop),保证整条析构链路 `noexcept`。

**验证方法**:注入式让 `deque::push_back` 在测试模式下抛异常,跑 acquire/release 循环,观察是否 terminate;ASan/TSan 跑高并发压测。

---

### 3.2 P1-2 ｜ `acquire()` 中 `make_unique` 在 try 块外,OOM 时全局计数器永久泄漏

**分类**:智能指针 / 异常安全
**位置**:`src/http/http_pool.hpp:235-255`

**问题**:第 242 行 `make_unique<HttpConn>(state->ioc)` 位于 try 块**外**。此时第 235 行 `try_increment_counter(state->total_count, max_size)` 已成功(`total_count` 已 +1),第 184/181 行 `in_flight_count` 也已 +1。若 `make_unique` 抛 `bad_alloc`,异常直接逃逸,第 251 行的 catch 块(只覆盖 `shard` 操作)不会执行,**两个计数器都不回滚**。

```cpp
// http_pool.hpp:235-255
if (!try_increment_counter(state->total_count, state->cfg.max_size)) { ... }
bool reserved_total = true;                              // total_count 已 +1

auto new_conn = std::make_unique<HttpConn>(state->ioc);  // ← try 块外!bad_alloc 逃逸
try {
    shard_idx = start_shard;
    auto& shard = state->shards[shard_idx];
    std::lock_guard lock(shard.mtx);
    ++shard.total; ++shard.in_flight;
    new_conn->shard_idx = shard_idx;
    shard.active.insert(new_conn.get());
} catch (...) {
    if (reserved_total) decrement_counter(state->total_count);
    if (reserved_in_flight) decrement_counter(state->in_flight_count);
    // ↑ 仅覆盖 shard 操作异常,不覆盖 make_unique
    throw;
}
```

**触发场景**:内存耗尽时 `make_unique` 抛 `bad_alloc`。

**后果**:每次 OOM 失败的 `acquire` 都让 `total_count`/`in_flight_count` 永久 +1。累积到 `max_size` 后**池子永久卡死**(后续 `acquire` 永远拿不到槽位,要么返 `nullptr` 要么卡到 `acquire_timeout`)。渐进退化,非立即 crash。

**修复建议**:把 `make_unique` 移进 try 块;或单独 try 包裹并回滚:
```cpp
std::unique_ptr<HttpConn> new_conn;
try {
    new_conn = std::make_unique<HttpConn>(state->ioc);
} catch (...) {
    if (reserved_total)    decrement_counter(state->total_count);
    if (reserved_in_flight) decrement_counter(state->in_flight_count);
    throw;
}
```

**验证方法**:注入 `make_unique` 失败、`shard.active.insert` 失败两种 case,观察 `total_count`/`shard.total` 是否恢复。

---

### 3.3 ~~P1-3~~ ｜ SecurityRules 热重载数据竞争 —— 【驳回:误报】

**驳回来源**:HTTP agent 报告称 `ip_blacklist_.is_blocked` / `auth_whitelist_.is_whitelisted` / `path_blacklist_.check` 是"无锁读",与 `ReloadService` 回调里 `security_rules_->reload()` 的写并发,构成数据竞争(ASAN 下会报)。

**主审裁定**:**误报**。三个子模块各有自己的内部 mutex,`check` 路径持锁读,`reload` 锁内整体替换 shared_ptr。逐字证据:

```cpp
// src/security/ip_blacklist.hpp:37-55
bool is_blocked(const std::string& ip) const {
    auto normalized = normalize_ip_str(ip);
    std::lock_guard<std::mutex> lock(mu_);          // ← 持锁读
    if (exact_ && exact_->count(normalized)) return true;
    ...
}

// src/security/auth_whitelist.hpp:53-67
bool is_whitelisted(const std::string& path, const std::string& service) const {
    std::lock_guard<std::mutex> lock(mu_);          // ← 持锁读
    ...
}

// src/security/path_blacklist.hpp:49-76
BlockResult check(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mu_);          // ← 持锁读
    ...
}
```

三者 `reload` 均为「锁外 `make_shared` 构造 → 锁内 `std::move` 替换」模式,读者持同一把锁 + 引用计数保旧对象活,**无指针撕裂、无数据竞争**。

**锁顺序分析**(确认无死锁):
- `SecurityRules::reload`(`security_rules.hpp:244`):持 `rules_mu_` → 调 `load_from_config` → 调 `ip_blacklist_.reload` 获 `ip_blacklist_.mu_`。顺序:`rules_mu_` → 子 `mu_`。
- `SecurityRules::check`(`security_rules.hpp:146-152`):在 `rules_mu_` 块内只拷贝 `trusted_proxies_`/`jwt_auth_`/`rate_limiter_`/`case_sensitive_paths_`;**离开块后**(释放 `rules_mu_`)才调用 `ip_blacklist_.is_blocked` 等(只获子 `mu_`)。

两路径不存在「同时持有两把锁」的场景,无死锁。

> **保留观察**:HTTP agent 底层提到的 `state_->security_rules` 裸指针(`client_session.hpp` 通过裸指针调用)本身是真实存在的脆弱点(见 §6 SEC-P3-4 / HTTP-P3),但当前靠「`ioc_.run()` join 完成后才 `cleanup()`」的不变量保护,非 P1。

---

## 4. P2 详解

### 4.1 SEC-P2-1 ｜ `SecurityRules::load_from_config` 非原子,日志与实际不符

**分类**:智能指针 / 热重载
**位置**:`src/security/security_rules.hpp:27-119` + `reload():242-255`

**问题**:注释(行 28-35)声称 "atomic on hot-reload",但实际只有 `build_jwt_auth` 这一步是预先构造。`jwt_auth_` 在行 74 已发布后,行 76-116 仍有多个抛点:`RateLimiter::Config` 构造、`make_shared<RateLimiter>`(行 111)、`load_cors_policy`(行 116)内部的 `string`/`unordered_set` 操作均可抛 `bad_alloc`。异常被 `reload()` catch,但状态已部分更新(新 `jwt_auth_` + 新 ip/auth/path 规则 + **旧 `cors_policy_` / 旧 `rate_limiter_`**)。catch 日志 "keeping previous rules" 与实际不符。

```cpp
jwt_auth_ = std::move(new_jwt);                                    // 行 74:已发布
...
rate_limiter_ = std::make_shared<RateLimiter>(std::move(rate_cfg)); // 行 111:可抛
cors_policy_  = std::make_shared<const CorsPolicy>(load_cors_policy(cfg)); // 行 116:可抛
```

**修复建议**:把 `CorsPolicy` / `RateLimiter::Config` 也先构造到局部变量(同 `new_jwt` 模式),全部成功后在 `rules_mu_` 下一次性发布;或拆为 "build all into locals" + "publish under lock" 两阶段。同步修正日志措辞。

### 4.2 SEC-P2-2 ｜ `case_sensitive_paths` 与子白名单的跨版本归一化窗口

**分类**:热重载 / 安全
**位置**:`security_rules.hpp:146-152` vs 行 173/193/206

**问题**:`check()` 在 `rules_mu_` 下只快照 `case_sensitive_paths_`,但 `auth_whitelist_`/`path_blacklist_`/`ip_blacklist_` 用各自独立 mutex。热重载时存在跨版本窗口:path 用旧 `case_sensitive` 归一化,但查表用新白名单(或反之)。

**触发场景**:reload 把 `case_sensitive_paths` 从 true→false 同时改 `auth_whitelist`,恰逢 `check()` 已离开快照块。

**评估**:Security agent 推演了几种组合,最坏方向都是「突然要求 JWT」而非「突然放行」。建议把 `case_sensitive` 下放到每个子模块的 reload 入参,让归一化与查表在同一把锁内一致。**需验证**:构造 `case_sensitive=true + whitelist /Api/X` 切到 `case_sensitive=false + 无该路径`,看 `/api/x` 是否短暂跳过 JWT。

### 4.3 SEC-P2-3 ｜ `verify_rs256` 的 `EVP_MD_CTX` 未 RAII

**分类**:智能指针 / RAII
**位置**:`src/security/jwt_auth.hpp:239-251`

**问题**:`EVP_MD_CTX` 靠单点 `EVP_MD_CTX_free(ctx)` 手动释放。当前代码安全,但中间任一步加 early-return 即泄漏;OpenSSL 调用本身不抛 C++ 异常,无异常安全网。同文件其他 OpenSSL 对象都已 RAII 化,唯独此处遗漏。

**修复建议**:
```cpp
std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
```

### 4.4 APP-P2-1 ｜ `co_spawn(detached)` 未捕获异常 → terminate 风险

**分类**:协程 / 异常
**位置**:`src/app/application.cpp:50` + `src/http/http_server.hpp:65-66, 96`

**问题**:`co_spawn(ioc_, server_->start(), asio::detached)` 用 `detached`,`start()` 任何未捕获异常都会 `std::terminate`。同文件下方的 `ClientSession` 协程都用带 `exception_ptr` 的完成处理器(`http_server.hpp:76-84`)安全兜底,唯独 `start()` 没有。而 `start()` 内 `http_server.hpp:66` 与 `:96` 两处 `LOG_INFO` 在 `try{}` 块**外**;`spdlog` 内部分配/互斥量异常或 `ostringstream` `bad_alloc` 都会逃逸到 `detached`。

**修复建议**:换成带完成处理器的 `co_spawn`,与 `ClientSession` 一致:
```cpp
co_spawn(ioc_, server_->start(),
    [](std::exception_ptr ep) {
        if (!ep) return;
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { LOG_ERROR("server start failed: ", e.what()); }
    });
```
或把 `start()` 整体包进 `try/catch`。

### 4.5 APP-P2-2 ｜ `request_stop()` 跨线程 `acceptor` 操作(严格 UB)

**分类**:协程 / 并发
**位置**:`src/app/application.cpp:114` + `src/http/http_server.hpp:58-63`

**问题**:`server_->stop()` 在 `request_stop()` 内被信号回调(运行在 io_context 线程 T_A)调用,执行 `acceptor_.cancel(ec)` / `acceptor_.close(ec)`;而 `start()` 协程在另一 io_context 线程 T_B 上可能正进入 `acceptor_.async_accept()` 内部调度。asio 线程安全契约是「同对象并发操作不安全(Shared objects: Unsafe)」。

```cpp
// application.cpp:114
if (server_) server_->stop();
// http_server.hpp:58-63
void stop() {
    if (!state_->running.exchange(false)) return;
    acceptor_.cancel(ec);
    acceptor_.close(ec);
}
```

**评估**:实际工程中 asio 通常容忍此模式(`cancel` 是 `async_accept` 的合法取消源),但严格按契约是 UB。**建议**:用 `asio::post(ioc_, [this]{ server_->stop(); })` 把 `stop` 串行化到 acceptor 归属线程,或显式 `strand`。**需验证**:TSan 在 SIGINT 触发的关停窗口下复测,若不报 race 可降级 P3。

### 4.6 APP-P2-3 ｜ `cleanup()` 中 `timer_.cancel()` 无 ec 重载可抛

**分类**:异常安全
**位置**:`application.cpp:125-143` + `reload_service.hpp:32` / `snapshot_service.hpp:20` / `pool_stats_service.hpp:21`

**问题**:三个 service 的 `stop()` 里 `timer_.cancel();`(无 `error_code` 重载)会抛 `std::system_error`(仅 cancel 自身出错时,概率极低)。一旦第一个 `stop()` 抛出,后续 `stop()` 与 `.reset()` 跳过,经 `run()` catch 重抛后走 `~Application`——成员逆序析构仍能兜底(`steady_timer` 析构不抛),**不 crash**,但留下未排空的 pending callback。

**修复建议**:三处改 `std::error_code ec; timer_.cancel(ec);`;或 `cleanup` 内每个 `stop()` 各自 `try{}` 包裹。

### 4.7 HTTP-P2-1 ｜ `route()` 用 `.at()` 缺防御性检查

**分类**:健壮性
**位置**:`src/http/upstream_manager.hpp:33-43`

**问题**:`route()` 在 `shared_lock` 下用 `pools_.at(svc)` 取 `shared_ptr`,但前面只检查了 `upstreams_.find(svc)`。`.at()` 在极端路径(未来 reload 引入 partial-failure 让两 map 不同步)可抛 `out_of_range`,向上冒泡到 `ClientSession::run` 的 catch 被翻译成 502(误报)。

**修复建议**:改用 `pools_.find(svc)`,未找到返回 `nullopt`。

### 4.8 HTTP-P2-2 ｜ 非静态 `release`/`release_bad` 是 dead code,误用即 UAF

**分类**:智能指针
**位置**:`src/http/http_pool.hpp:285-287, 332-334`

**问题**:非静态 `release(HttpConn)` / `release_bad(HttpConn)` grep 全仓无调用方(`ConnGuard` 用静态版本),它们访问 `state_`(通过 `this`)。hot-reload 下若被误用且 `HttpPool` 已析构即 UAF。

**修复建议**:直接删除这两个非静态重载,强制所有调用走静态版本。

### 4.9 HTTP-P2-3 ｜ `ConnGuard` 冗余持有 `pool_holder_`

**分类**:智能指针 / 热重载
**位置**:`src/http/proxy_forwarder.hpp:29-35`

**问题**:`ConnGuard` 同时持有 `pool_holder_`(`shared_ptr<HttpPool>`)和 `pool_state_`(`shared_ptr<HttpPool::State>`),但所有 `release`/`release_bad`/`untrack_active` 都是 static、只用 State。`pool_holder_` 冗余,且让 `~ConnGuard` 析构顺序里多了一次「可能触发 `HttpPool::~HttpPool → shutdown()` 关闭所有 socket」的副作用——当某 `ConnGuard` 是最后一个持有旧 HttpPool 的引用时,析构同步触发 `shutdown()`,可能在 hot-reload 切换瞬间把同池仍在用的 active 连接 socket 关闭。

**修复建议**:`ConnGuard` 只保留 `pool_state_`(State 的引用计数足以让旧 pool 的 State 活着),不持有 `pool_holder_`。`HttpPool` 本体更早析构(关旧 socket 是预期),但不影响 in-flight 请求(它们已持 `conn_` + `pool_state_`)。

> **注**:HTTP agent 还报告 `http_pool.hpp:421-444` `resolve_with_timeout` 中 `std::to_string(port)` 临时量绑定到 `const std::string& service` 再被 lambda 引用捕获的 GCC 11 风险。**主审降级为 P3**:C++ 标准保证协程临时量活到 full-expression 结束(即 `co_await` 表达式结束),标准上安全,仅 GCC 10/11 早期 patchlevel 理论风险。建议 `std::string port_str = std::to_string(port);` 作为协程帧局部量更稳妥,并用 ASAN 验证。

---

## 5. P3 详解

### 5.1 智能指针 / RAII

| 编号 | 位置 | 问题 | 修复 |
|---|---|---|---|
| SEC-P3-1 | `jwt_auth.hpp:179-190` | `load_rsa_key` 的 `BIO_free(bio)` 手动释放,无 RAII | `unique_ptr<BIO, decltype(&BIO_free)>` |
| SEC-P3-2 | `jwt_auth.hpp:104-117` | `mem_bio` 未 RAII(行 108、116 两处手动 `BIO_free`),同文件其他 OpenSSL 对象都已 RAII,风格不一致 | 同上 |
| HTTP-P3-1 | `http_pool.hpp:74-89` vs `:107-122` | `State::~State()` 与 `HttpPool::shutdown()` 都遍历 shards 关 socket,逻辑重复;`~State` 不检查 running 也不递减 `total_count`,语义不一致 | 关闭逻辑统一到一处 |
| HTTP-P3-2 | `http_pool.hpp:125-137` | 成员版 `track_active`/`untrack_active` dead code(ConnGuard 用静态版) | 删除 |
| DB-P3-1 | `redis_pool.hpp:400` | `acquire_worker` 中 `if (!should_create) continue;` 是死代码(while 退出时 `should_create` 必为 true) | 删除或加注释说明 |
| DB-P3-2 | `redis_pool.hpp:206-238` | `RedisReplyGuard` 定义了 move ctor/assign,但全局无人使用 | 删除 move 定义 |

### 5.2 协程 / 健壮性

| 编号 | 位置 | 问题 | 修复 |
|---|---|---|---|
| APP-P3-1 | `routes.cpp:50-67` | `/api/combo` MySQL 失败(`mysql_ret.ok==false`)时 `data` 留空,返回 `resp_ok_str("")` + `status=200`,客户端无法区分「缓存真空」与「DB 取失败」 | `else { ctx.status_code=500; ctx.response_body=resp_err(DB_ERROR, mysql_ret.error); co_return; }` |
| APP-P3-2 | `logger.hpp:88-99` | `log_warn_impl`/`log_error_impl` 无 `should_log` 短路(与 info/debug 不一致),级别过滤时仍拼字符串 | 对齐 `if (...->should_log(...))` |
| APP-P3-3 | `logger.hpp:42-47` | init 失败兜底 `spdlog::stdout_color_mt("default")` 在 registry 已注册同名 logger 时会二次抛 | 用 `try/catch` 包住兜底创建,或换唯一名 |
| APP-P3-4 | `logger.hpp:50-58` | `Logger::get()` fallback 路径(`logger_` 为空时)写 `logger_` 非原子。正常流程 `init()` 在 worker 线程创建前完成(`std::thread` 构造是同步屏障),理论隐患 | `std::call_once` 或 init 强保证成功 |
| SEC-P3-3 | `security_rules.hpp:282-294` | `build_jwt_auth` 加载公钥文件:文件存在但内容不含 `-----BEGIN` 时,`pub_key` 保留原文件路径(既非 PEM 也未 clear),导致 JWKS 回退被跳过 + RS256 必填校验跳过 + 路径被当 PEM 传给 `JWTAuth` 解析失败抛异常(reload 拒绝,但启动 crash) | 文件存在但解析不到 PEM 时 `pub_key.clear()` + LOG_WARN |
| HTTP-P3-3 | `http_pool.hpp:259` | `std::to_string(port)` 临时量在 `resolve_with_timeout` 引用捕获(标准安全,GCC 11 理论风险,见 §4.9 注) | `std::string port_str = std::to_string(port);` 传帧局部 |

### 5.3 裸指针依赖不变量

| 编号 | 位置 | 问题 |
|---|---|---|
| SEC-P3-4 / HTTP-P3-4 | `client_session.hpp:42`(`state_->security_rules` 裸指针)+ `application.cpp:142`(`security_rules_.reset()`) | 当前安全是因为 `ioc_.run()` 已 join 完所有线程,协程不会再恢复。但属「靠不变量保护」的脆弱设计:若未来 cleanup 阶段做任何 resume(如 drain_timer 改更长且与 cleanup 并行),即 UAF。建议改 `shared_ptr<SecurityRules>` 由 `HttpServerState` 持有,或 cleanup 里先把 `state_->security_rules` 置空再 reset。 |

---

## 6. DB 层详细审计(主审第一手记录)

DB 层经 mysql v3.3 / redis v2 多轮 UAF 修复,**未发现 P0/P1 智能指针或协程 bug**。详细检查点:

### 6.1 `mysql_pool.hpp`

- **`execute(std::string sql)`**(`:111-123`):`sql` 按值传入留帧,`co_await asio::post(worker_pool_, use_awaitable)` 仅切 executor(非 lambda 捕获),随后 `do_query(sql.c_str())` 安全。✓ 符合 GCC 11 教训。
- **`do_query`**:`acquire()` 在 worker_pool_ 线程同步执行,`ConnectionGuard` RAII 管理(`bad_` → `drop_bad_connection`,否则 `release`)。`MysqlResultPtr mr(mysql_store_result(conn), mysql_free_result)` RAII。✓
- **`acquire()` slot 管理**(`:213-275`):`++total_`/`++creating_` 在锁内,创建失败时锁内 `--creating_; --total_; cv_.notify_all()`。`mysql_reset_connection` 失败 → `drop_bad_connection` + `continue`。✓ slot 回滚正确。
- **`shutdown()` 顺序**(`:87-109`):`running_=false` → `cv_.notify_all()` → join maintain → `worker_pool_.join()` → 锁内清 idle。✓
- **成员析构逆序**(`:491-503`):`worker_pool_`(:502)先于 `maintain_thread_`(:503)析构,但析构函数显式 `shutdown()` 已 join 两者,析构时均 `joinable()=false`。✓
- **`running_` 翻转期间的连接归还**:shutdown 在 do_query 执行期间被调用时,`worker_pool_.join()` 等 do_query 完成,`ConnectionGuard` 析构 `release(conn)` 归还,随后 shutdown 关 idle 连接。时序正确。✓

### 6.2 `redis_pool.hpp`

- **`cmd_argv`/`get`**(`:111-144`):`args`/`key_copy` 按值留帧,`co_await asio::post(*worker_pool_, use_awaitable)` 仅切 executor。Direct 模式直接在 io_context 线程同步执行(`thread_local` 连接)。✓
- **`cmd_argv_sync_impl`**(`:153-204`):`ConnectionGuard` + `RedisCommandArgv command(args)`(深拷贝持有数据)。`reply==nullptr` 时 `record_command_failure(ctx,...)` 在 `guard.drop()` 之前调用,ctx 仍有效。✓ retry 仅对 readonly idempotent 命令,非幂等命令不重放。✓
- **`acquire_worker`**(`:363-420`):slot 管理与回滚正确。`if (!should_create) continue;`(`:400`)为死代码(DB-P3-1)。✓
- **Direct 模式 TLS 生命周期**(`:70-89, 429-445`):`tls_` 是 `thread_local`,`shutdown` 只能清当前线程 TLS,其他 io 线程的 TLS `redisContext` 靠 `owner+generation` 双重校验 + `thread_local` 析构释放。`drop_bad_connection` direct 分支:若 `tls.conn==ctx` 则 `reset`(析构 `RedisContextPtr`→`redisFree`),否则 `redisFree(ctx)`。✓
- **`shutdown_worker_mode`**(`:321-336`):`cv_.notify_all()` → join maintain → `worker_pool_->join()` → 锁内清 idle。✓
- **`do_maintain` PING 检查**(`:543-592`):从 idle_pool pop 独占后 PING,`RedisReplyGuard` RAII,dead 则 `redisFree`,alive 则 push 回。`!running_` 时关闭未检查的连接。✓
- **`worker_pool_` 为 `unique_ptr`**(`:669`):仅 Worker 模式 `init_worker_mode()` 里 `make_unique`,Direct 模式为 null。`cmd_argv` Worker 分支 `*worker_pool_` 解引用安全(mode 固定)。✓

### 6.3 `redis_connection.hpp` / `redis_command.hpp`

- `RedisContextPtr = unique_ptr<redisContext, decltype(&redisFree)>`。✓
- `TlsRedisConn { owner, generation, RedisContextPtr conn }`:`thread_local`,`owner+generation` 防 stale pool。✓
- `RedisCommandArgv`:持有 `args`(深拷贝)+ 禁 copy/move(防 `argv` 指向被 move 走的 `args` vector)。✓ 见 §2.3。

---

## 7. 文档漂移(CLAUDE.md,4 处)

1. **`src/common/timeout.hpp` + `with_timeout` 模板**——`find` 仓库不存在该文件(仅 `src/http/http_io.hpp` 有局部 `read_with_timeout`/`write_with_timeout`)。
2. **`/api/combo` 用 `with_timeout` 做 cache→DB 500ms fallback**——实际 `routes.cpp:42-67` 是裸 `co_await`,无超时包装。
3. **`MysqlPool::execute` 把 SQL 拷进 `char sql_buf[4096]` 并捕获进 `asio::post` lambda**——已改为 executor 切换 + `sql.c_str()`(见 §2.1)。
4. **调试 build marker 残留**——`gateway-debug-20260703-client-close-trace` 在 `routes.cpp:84` / `application.cpp:35`,建议生产前清理。

---

## 8. 修复优先级建议

| 优先级 | 项 | 改动量 | 收益 |
|---|---|---|---|
| 🔴 立即 | P1-1 + P1-2(`http_pool` try 边界 + `~ConnGuard` try/catch) | 小 | 消除 OOM 路径进程崩 / 池子卡死 |
| 🟠 尽快 | APP-P2-1(detached → 带完成处理器) + SEC-P2-1(reload 原子化) | 中 | 稳定性 + 热重载正确性 |
| 🟡 复测定级 | APP-P2-2(acceptor 跨线程 cancel) | 中(TSan 复测) | 视 TSan 结果决定是否上 strand |
| 🟢 有空 | P2 其余 + 全部 P3 + 文档漂移 | 小~中 | 健壮性 / 可维护性 |
| 🟢 尽快 | 文档漂移(§7) | 小 | 避免误导后续维护 |

---

## 9. 附录:各层「已检查且无问题」清单

### 9.1 Security 层(agent B)
- `shared_ptr` 快照锁内拷贝:`cors_policy()`/`rate_limiter_snapshot()`/`check()` 行 146-152 均在 `rules_mu_` 内拷贝,锁外使用,旧对象 refcount 保活。✓
- `JWTAuth` 自定义 deleters:`build_rsa_pubkey_from_jwks` 中 `BignumDeleter`/`ParamBldDeleter`/`ParamDeleter`/`PkeyCtxDeleter`/`EVP_PKEY_Deleter` 全 RAII;`raw_pkey` 在行 97 显式 `EVP_PKEY_free` 处理 `EVP_PKEY_fromdata` 失败,行 102 接管到 `unique_ptr`。无泄漏、无 double-free。✓
- `JWTAuth::verify` 对 `pkey_` 并发访问:`pkey_` 构造后只读,`EVP_VerifyInit` 用独立 `EVP_MD_CTX`,OpenSSL 1.1+ 允许共享。✓
- `ReloadService`/`SnapshotService` 持 `SecurityRules&`:cleanup 顺序为 stop → reset 各 service → reset server → reset security_rules_,均发生在 `ioc_.run()` join 后。✓
- `check()` 内 `auto& path = norm.path`/`auto& normalized_ip = normalized_ip_result.str`:指向同栈帧局部,调用链全程同步无 `co_await`。✓
- `RateLimiter` 与 reload 的 cfg 读写:`update_config` 与 `check_*` 都在 `cfg_mu_` 下;`rate_limiter_snapshot()` 返回的 `shared_ptr` 让旧 RateLimiter 保活。✓
- `SnapshotService::persist_snapshot` 重入:`snapshot_busy_.exchange(true)` 保护 destructor 与 timer 并发。✓

### 9.2 App 层(agent C)
- `routes.cpp:57` `asio::post(ex, [redis,data]{...})`:fire-and-forget,不在 GCC 11 UAF 模式(见 §2.2)。✓
- `application.cpp:111-123 request_stop()`:`stop_requested_.exchange(true)` 幂等;`drain_timer_` 在 `cleanup()` 内 reset,async_wait lambda 随 timer 销毁,`[this]` 不被解引用。✓
- `application.cpp:69-74 catch 路径`:`initialize()` 半完成时 `ioc_.stop()`+`join_all()`(threads 可能为空)+ `cleanup()`(全部 `if(...)` 守卫)安全;半构造 `MysqlPool` 析构经 `~MysqlPool`→`shutdown()` 正确 join。✓
- `Logger::init`:`init_thread_pool(262144, 1)` 在所有 worker 线程创建前调用,`std::thread` 构造同步屏障保证可见性;`async_logger` 自身由 spdlog 内部线程池保证线程安全。✓

### 9.3 HTTP 层(agent A)
- `ClientSession` 不继承 `enable_shared_from_this`:用 `co_spawn` 捕获 shared_ptr 延长生命周期,等价正确。✓
- `acquire` 协程 `auto state = state_` 后不访问 this:hot-reload 替换 HttpPool 不影响在途请求。✓
- `UpstreamManager::reload`(`unique_lock`)/`route`(`shared_lock`)两 map 同步性受锁保护。✓
- `read_with_timeout`/`write_with_timeout` 的 `parallel_group`+`wait_for_one`:正确取消未完成操作并等待取消完成。✓
- `read_proxy_response` 中 `tmp` 栈 buffer 与 `buf` 局部 string:均在协程帧,安全。✓
- `HttpConn` move-only,`deque::push_back(std::move(*conn))` 后 `unique_ptr` 析构原对象:move 后指针已 erase 出 active,安全。✓
- `acquire` 的 max_concurrent/max_size/in_flight 计数器在成功/失败路径平衡(除 P1-2 的 bad_alloc 路径)。✓
- `evict_stale_idle` 持锁访问 `last_used_at`:与 release 持锁设置互斥,无竞争。✓
- `HttpServer::start` 经 request_stop → `server_->stop()` → acceptor close 触发 `async_accept` 出错退出:生命周期正确,cleanup 在 `ioc.run()` 返回后才 reset server_。✓

---

## 10. 审计元信息

- **审计对象**:`asio_owen` @ commit `4704813`(master)
- **审计日期**:2026-07-19
- **审计者**:Claude(主审)+ 3 个并行 general-purpose agent(HTTP/Security/App 层)
- **交叉验证**:主审对全部 P1 发现逐条回源码核验,驳回 P1-3(误报)
- **覆盖文件**:19 个(`src/db/` 4 + `src/http/` 8 + `src/security/` 5 + `src/app/` 3 + `src/common/` 1)

---

*报告生成于 2026-07-19 by Claude。如需对任意条目深入(复现脚本、修复 PR、TSan 复测),请指明条目编号。*
