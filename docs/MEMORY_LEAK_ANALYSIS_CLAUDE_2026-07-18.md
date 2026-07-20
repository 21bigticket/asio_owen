# 内存泄漏分析与修复说明

> 生成者:Claude · 日期:2026-07-18
> 目的:供修复 agent 直接据以修改 + 验证。自包含,无需重新调研。
> 关联:[CODE_ANALYSIS_CLAUDE_2026-07-18.md](CODE_ANALYSIS_CLAUDE_2026-07-18.md) §2 P2/P3

---

## 0. 一句话结论

**没有经典堆内存泄漏**(无 `new`/`malloc` 不释放、无 `shared_ptr` 循环引用、无容器无限增长、无连接/文件描述符泄漏)。RAII / `shared_ptr` 覆盖完整。

唯一可称作"泄漏"的是 `src/http/http_pool.hpp` 的 `acquire()` 在 **bad_alloc 异常路径**下的**计数器不回滚**(逻辑泄漏,非字节泄漏)——本文档 §2 给出精确修复方案。**修复 agent 只需改这一处。**

---

## 1. 已排查并确认"无泄漏"的点(修复时请勿动)

这些已逐一确认 RAII 完整,不属于本次修复范围,**不要改动它们**:

| 对象 | 释放机制 | 结论 |
|---|---|---|
| HttpPool 连接(`HttpConn`/socket) | `unique_ptr<HttpConn>`,异常栈展开析构 → `socket.close`;`release`/`release_bad` 路径 `--total` | 连接不泄漏(见 §2 表格,三种异常路径都释放) |
| MysqlPool 连接 | `ConnectionGuard` RAII 归还/drop;shutdown `mysql_close` 全部 idle | 不泄漏 |
| RedisPool 连接 | `ConnectionGuard`;`drop_bad_connection` → `redisFree`;`shutdown_worker_mode` 释放全部 idle | 不泄漏 |
| hiredis reply | `RedisReplyGuard`(`redis_pool.hpp:206`)析构 `freeReplyObject` | 不泄漏 |
| RedisCommandArgv 数据 | 类自身持有(`std::vector<std::string>`),禁拷贝(2026-07-15 UAF 修复) | 不泄漏 |
| coroutine 帧 | `co_spawn` completion handler 持生命期,连接结束即销毁 | 不泄漏 |
| `asio::thread_pool` / maintain 线程 | `shutdown_worker_mode` / mysql shutdown 里 `join` | 不泄漏 |
| rate_limiter buckets | LRU eviction + `max_buckets` 上限 | 不无限增长 |
| spdlog async logger | 进程级 singleton,退出 OS 回收 | 不泄漏 |

---

## 2. 唯一问题:`http_pool.hpp` acquire 计数器泄漏

### 2.1 位置

`src/http/http_pool.hpp` 的 `acquire()` 新建连接分支,当前代码(`:233-282`):

```cpp
// No reusable idle connection -> create new one.
// in_flight slot already reserved above.
if (!try_increment_counter(state->total_count, state->cfg.max_size)) {   // :235
    if (reserved_in_flight) decrement_counter(state->in_flight_count);
    LOG_WARN("HttpPool: max_size reached");
    co_return nullptr;
}
bool reserved_total = true;                                              // :240

auto new_conn = std::make_unique<HttpConn>(state->ioc);                  // :242  ← 在 try 外
try {
    shard_idx = start_shard;
    auto& shard = state->shards[shard_idx];
    std::lock_guard lock(shard.mtx);
    ++shard.total;                                                       // :247
    ++shard.in_flight;                                                   // :248
    new_conn->shard_idx = shard_idx;
    shard.active.insert(new_conn.get());                                // :250  ← 可能抛 bad_alloc
} catch (...) {
    if (reserved_total) decrement_counter(state->total_count);          // :252  ← 只回滚全局
    if (reserved_in_flight) decrement_counter(state->in_flight_count);  // :253
    throw;                                                              // :254  ← 没回滚 shard.total/in_flight
}

try {
    asio::ip::tcp::resolver resolver(state->ioc);
    auto eps = co_await resolve_with_timeout(...);                       // :259
    ...
    co_return std::move(new_conn);                                       // :270
} catch (...) {
    auto& failed_shard = state->shards[new_conn->shard_idx];             // :272
    {
        std::lock_guard lock(failed_shard.mtx);
        failed_shard.active.erase(new_conn.get());                       // :275  ← 这个 catch 是完整的
        --failed_shard.total;                                            // :276
        --failed_shard.in_flight;                                        // :277
    }
    if (reserved_total) decrement_counter(state->total_count);
    if (reserved_in_flight) decrement_counter(state->in_flight_count);
    throw;
}
```

### 2.2 三种异常路径的泄漏分析

| 抛异常点 | `new_conn`(连接) | 全局 `total_count` | 全局 `in_flight_count` | `shard.total` | `shard.in_flight` |
|---|---|---|---|---|---|
| `:242` `make_unique` 抛 | 没创建,无对象可泄漏 | ❌ `:235` 已 ++,不回滚 | ❌ `:184` 已 ++,不回滚 | — | — |
| `:250` `shard.active.insert` 抛 | ✅ `unique_ptr` 栈展开析构释放 | ✅ catch `:252` 回滚 | ✅ catch `:253` 回滚 | ❌ `:247` 已 ++,**不回滚** | ❌ `:248` 已 ++,**不回滚** |
| `:257-282` resolve/connect 失败 | ✅ `unique_ptr` 栈展开释放 | ✅ catch `:279` 回滚 | ✅ catch `:280` 回滚 | ✅ catch `:276` 回滚 | ✅ catch `:277` 回滚 |

**关键结论**:
- **连接(HttpConn/socket)在所有路径都不泄漏**——`unique_ptr` RAII 保证析构。
- **泄漏的是计数器**:`make_unique` 抛 → 全局两个计数器不回滚;`shard.active.insert` 抛 → shard 两个计数器不回滚。
- 第二个 try(resolve/connect 失败)的 catch 是**完整的**,无需改。

### 2.3 现实影响(为什么仍要修)

每次 bad_alloc 导致计数器永久虚占 +1。长期累积后:
- `state->total_count` 漂移上升,直到 `try_increment_counter(state->total_count, max_size)`(`:235`)永远返回 false → `acquire()` 永远走 "max_size reached" 分支返回 nullptr → **上游连接池永久锁死**,该路由所有请求 503。
- `shard.total`/`shard.in_flight` 漂移 → `stats()` 报告虚高的活跃连接数,监控失真。

触发门槛:需要**反复内存压力**(每次 bad_alloc 才 +1)。现实中 OOM 影响下服务器多半先出别的问题,但(1)逻辑上确实存在、(2)修复极小、(3)有现成测试可扩展——值得修。

### 2.4 修复方案

**核心思路**:让"shard 计数 ++"只在 `shard.active.insert` 成功之后发生,这样 insert 抛异常时 shard 计数还没动,catch 只回滚全局即可。同时把 `make_unique` 移进 try,使它抛时全局计数也能回滚。

**Patch(before → after)**:

```cpp
// ===== AFTER =====
bool reserved_total = true;

std::unique_ptr<HttpConn> new_conn;
try {
    new_conn = std::make_unique<HttpConn>(state->ioc);     // 移进 try:抛则进 catch 回滚全局
    shard_idx = start_shard;
    auto& shard = state->shards[shard_idx];
    std::lock_guard lock(shard.mtx);
    new_conn->shard_idx = shard_idx;
    shard.active.insert(new_conn.get());                   // insert 成功后才 ++ shard 计数
    ++shard.total;                                         // 移到 insert 之后
    ++shard.in_flight;                                     // 移到 insert 之后
} catch (...) {
    // make_unique 抛 → shard 计数未动;insert 抛 → shard 计数未动(insert 在 ++ 之前)
    // 故此处只需回滚全局计数,无需触碰 shard。
    if (reserved_total) decrement_counter(state->total_count);
    if (reserved_in_flight) decrement_counter(state->in_flight_count);
    throw;
}
```

**为什么这样是对的**:
- `make_unique` 抛 → 还没进 shard 任何操作,catch 回滚全局两个计数器,正确。
- `shard.active.insert` 抛 → `++shard.total/in_flight` 在它**之后**,还没执行,shard 计数未动;catch 回滚全局,正确。insert 本身在 `unordered_set::insert` 抛 bad_alloc 时不会留下半插入元素(标准保证强异常保证),所以 shard.active 也干净。
- insert 成功后的 `++shard.total/in_flight` 是 POD 自增(`noexcept`),不会再抛。
- 第二个 try 块(`:257-282`)不变——它已正确处理 active.erase + shard-- + 全局--。

**等价替代方案**(若不想调整 ++ 顺序,可用标志位):
```cpp
bool tracked_shard = false;
try {
    new_conn = std::make_unique<HttpConn>(state->ioc);
    ...
    ++shard.total; ++shard.in_flight;
    tracked_shard = true;
    shard.active.insert(new_conn.get());
} catch (...) {
    if (tracked_shard) { --shard.total; --shard.in_flight; }  // 需持 shard.mtx
    if (reserved_total) decrement_counter(state->total_count);
    if (reserved_in_flight) decrement_counter(state->in_flight_count);
    throw;
}
```
两种都正确,**推荐第一种**(调整 ++ 顺序)更简单,无需额外标志和重复加锁。

### 2.5 测试建议

扩展 `tests/test_http_pool.cpp`。现有用例 `FailedConnectCleansGlobalAndShardCounters`(`:79`)只覆盖 resolve/connect 失败(第二个 try 的 catch)。需要补**构造期/insert 期异常**的回滚:

1. **make_unique 抛 bad_alloc 的回滚**:很难直接触发 `make_unique<HttpConn>` 抛(它内部开 socket)。可改用一个能注入故障的测试 double,或用 `max_size` 配合计数器观察:把 `cfg.max_size` 设很小,在构造路径注入异常(可能需要把 `HttpConn` 构造的失败点暴露为 seam)。
2. **更实际的回归测试**:构造一个场景让 `shard.active.insert` 抛(例如 mock `unordered_set` 或用限容容器),断言异常后 `state->total_count == 0`、`state->in_flight_count == 0`、所有 `shard.total == 0`、`shard.in_flight == 0`。
3. **若注入故障成本高**:至少加一个文档化断言——在正常 acquire/release 循环 N 次后,`total_count` 与实际 `shard.total` 之和相等(无漂移)。这能兜住未来任何计数不一致的回归。

验证清单(修完后逐项确认):
- [ ] `make_unique` 抛 → `total_count`/`in_flight_count` 回到初始值
- [ ] `shard.active.insert` 抛 → `total_count`/`in_flight_count`/`shard.total`/`shard.in_flight` 全部回到初始值
- [ ] resolve/connect 失败 → 原有行为不变(第二个 catch 未改)
- [ ] 正常 acquire/release → 计数器无漂移
- [ ] `cmake --build build && ctest --test-dir build --output-on-failure` 全绿
- [ ] 用 ASan/UBSan 构建(`rebuild_asan.sh`)跑一遍,确认无新告警

---

## 3. 不要误修的(长得像泄漏但不是)

修复 agent 容易顺手"修"这几个——**它们不是内存泄漏,属于别的范畴,请勿在本任务里动**:

| 位置 | 实际类别 | 说明 |
|---|---|---|
| `http_pool.hpp:285` by-value `release(HttpConn)` 重载 | **UAF**(释放后用),不是泄漏 | `make_unique<HttpConn>(std::move(conn))` 造新地址,`shard.active.erase` no-op,原地址悬垂。而且 grep 确认**当前是 dead code**(只用 static `unique_ptr` 重载)。属于 CODE_ANALYSIS §2 P2 #另一条,单独处理。 |
| `rate_limiter.hpp:217` `snapshot_busy_` 异常泄漏 | **标志泄漏**(功能禁用),非内存 | 异常路径标志卡 true → snapshot 静默禁用;`~RateLimiter` 里抛是 `std::terminate` 方向。属稳定性问题,单独修(RAII 包裹 `store(false)`)。 |
| `rate_limiter.hpp:371` `write_snapshot` 无 failbit 检查 | **数据损坏**,非泄漏 | 截断文件覆盖好快照。单独修。 |

---

## 4. 给修复 agent 的执行清单

1. **只改 `src/http/http_pool.hpp` 的 `acquire()`**(`:240-255` 那个 try/catch 块),按 §2.4 的 patch。
2. 扩展 `tests/test_http_pool.cpp` 覆盖构造期/insert 期异常的计数回滚(§2.5)。
3. 跑 `cmake --build build && ctest --test-dir build --output-on-failure`;有 `rebuild_asan.sh` 的话再跑一遍 ASan 构建。
4. **不要动** §1 列出的任何 RAII 路径,不要动 §3 列出的三个非泄漏项。
5. 提交信息建议:`fix(http_pool): roll back shard/global counters on bad_alloc in acquire`。
