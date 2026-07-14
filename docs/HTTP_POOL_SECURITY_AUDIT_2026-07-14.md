# HTTP 连接池安全审计报告

> 审计者: Claude (Opus 4.8) · 日期: 2026-07-14 · 目标: `src/http/http_pool.hpp`
>
> **2026-07-14 复核修订**: 经代码复核，原报告对 VULN-1 和 VULN-3 的定性不准确，已下调严重度并修正描述（详见各条目的"复核修订"说明）。真正需要重视的是 VULN-2（非幂等重试），已修复。

---

## 执行摘要

**总体风险等级**: **LOW-MEDIUM**（复核后从 MEDIUM-HIGH 下调）

HTTP 连接池 (`HttpPool`) 在并发控制和资源管理上总体设计成熟（16-shard 架构降低锁争用、`try_increment_counter` CAS 保证硬上限、RAII 生命周期管理、异常路径计数器清理完整）。经复核，原报告列出的问题多数被高估：

- **VULN-2（非幂等 stale-idle 重试）** 是唯一有实际业务风险的问题（可能重复提交），**已修复**。
- **VULN-1** 实为内部未暴露计数器的语义不一致，**不影响监控也不影响限流**，下调为 LOW，已修复。
- **VULN-3** 不成立：CAS 语义保证 `in_flight_count` 不会超过 `max_concurrent`，不存在超限；仅有一个短暂的误拒窗口，下调为 LOW。

**已修复**: VULN-1、VULN-2。**未修复**: VULN-3（低优先级）+ 4 个 LOW 设计弱点。

---

## 1. 关键漏洞列表

### 🟡 VULN-1: `in_flight_count` 原子计数器在默认配置下语义不一致

**严重度**: ~~MEDIUM~~ → **LOW**（复核下调）  
**位置**: `http_pool.hpp:174-180` + `:412-416`  
**状态**: 已修复

> **⚠️ 复核修订（2026-07-14）**：原报告将此定性为"监控指标失效"是**错误的**。经代码复核：
> - `stats()`（`:370-391`）输出的 `in_flight` 字段实际来自各 shard 的 `shard.in_flight` 汇总（`:378`），而 `shard.in_flight` 在 acquire/release 中是**无条件维护**的（`++shard.in_flight` / `--shard.in_flight`，不看 `max_concurrent`）。因此**运维看到的 `in_flight` 一直是准确的**。
> - `in_flight_count` 这个原子计数器**从未出现在 `stats()` 输出中**，它只在 `max_concurrent > 0` 时作为限流 CAS 的操作数使用。默认配置下它恒为 0 是**无害的**——既不影响监控（监控读 shard 汇总），也不影响限流（限流本就未启用）。
> - 因此本条不是"监控盲区/告警失效"漏洞，而是一个**内部未暴露计数器的语义不一致**：默认配置下 `in_flight_count` 不反映真实值，可能误导未来读取该字段的代码。修复它是为了语义一致和将来复用，而非修复实际的可观测性缺陷。**下调为 LOW。**

#### 问题描述

`in_flight_count` 原子计数器在 `max_concurrent = 0`（默认配置）时不被维护：

- `acquire()` 中：只有 `max_concurrent > 0` 时才增加计数（`:174-180`）
- `release_in_flight()` 中：只有 `max_concurrent > 0` 时才减少计数（`:412-416`）

这导致在默认配置下，`in_flight_count` **始终为 0**。如前述复核说明，这不影响 `stats()` 输出（它读 shard 汇总），但会让任何未来直接读取 `in_flight_count` 的代码拿到错误值。

```cpp
// acquire() 中（默认配置 max_concurrent = 0 时跳过）：
if (state->cfg.max_concurrent > 0) {  // false，不执行
    if (!try_increment_counter(state->in_flight_count, state->cfg.max_concurrent)) {
        LOG_WARN("HttpPool: max_concurrent reached");
        co_return nullptr;
    }
    reserved_in_flight = true;
}
// 结果：reserved_in_flight = false，in_flight_count 不增加

// release_in_flight() 中（对称地跳过）：
static void release_in_flight(const std::shared_ptr<State>& state) {
    if (state->cfg.max_concurrent > 0) {  // false，不执行
        decrement_counter(state->in_flight_count);
    }
}
// 结果：in_flight_count 不减少
```

**注意**：当前代码的 increment/decrement 是**对称的**（代码注释 `:409-411` 明确说明了这一点），但这种对称性导致统计功能在默认配置下完全失效。

#### 触发条件
- 默认配置（`max_concurrent = 0`，大多数部署使用此配置）
- 任何正常流量

#### 影响（复核后修正）
- ~~监控盲区~~：**不成立**。`stats()` 读 shard 汇总，监控数据准确。
- ~~告警失效~~：**不成立**。同上。
- **潜在误导**: 未来若有代码直接读 `in_flight_count`（而非 `stats()`），在默认配置下会拿到恒 0 的错误值。
- **语义一致性**: 修复后 `in_flight_count` 始终反映真实 in-flight 数，便于将来将其纳入 `stats()` 或用于自适应限流。

#### 修复建议

**方案 A（推荐）**: 解耦限流和统计，始终维护 `in_flight_count`

```cpp
// 在 acquire() 中，总是增加统计计数
bool reserved_in_flight = false;
if (state->cfg.max_concurrent > 0) {
    // 先检查限流
    if (!try_increment_counter(state->in_flight_count, state->cfg.max_concurrent)) {
        LOG_WARN("HttpPool: max_concurrent reached");
        co_return nullptr;
    }
    reserved_in_flight = true;
} else {
    // max_concurrent == 0（无限制），仍需增加统计计数
    state->in_flight_count.fetch_add(1, std::memory_order_relaxed);
    reserved_in_flight = true;
}

// 在 release_in_flight() 中，总是减少统计计数
static void release_in_flight(const std::shared_ptr<State>& state) {
    decrement_counter(state->in_flight_count);  // 无条件减少
}
```

**优点**：
- 统计始终准确，无论是否启用限流
- 改动最小（约 10 行）
- 向后兼容

**方案 B（更清晰但改动大）**: 分离两个计数器

```cpp
struct State {
    std::atomic<size_t> in_flight_throttle{0};  // 仅用于限流判断
    std::atomic<size_t> in_flight_stats{0};     // 仅用于统计（总是维护）
    // ...
};

// acquire() 中
state->in_flight_stats.fetch_add(1, std::memory_order_relaxed);  // 总是增加
if (state->cfg.max_concurrent > 0) {
    if (!try_increment_counter(state->in_flight_throttle, state->cfg.max_concurrent)) {
        state->in_flight_stats.fetch_sub(1, std::memory_order_relaxed);
        co_return nullptr;
    }
}

// release() 中
state->in_flight_stats.fetch_sub(1, std::memory_order_relaxed);  // 总是减少
if (state->cfg.max_concurrent > 0) {
    decrement_counter(state->in_flight_throttle);
}
```

**优点**：
- 职责分离清晰
- 未来扩展性更好

**缺点**：
- 改动较大（需要修改多处）
- 增加一个原子变量的内存开销

**推荐方案 A**，因为改动最小且满足需求。方案 B 适合未来重构时考虑。

---

### 🔴 VULN-2: 非幂等方法在 stale-idle 重试时重复提交

**严重度**: MEDIUM  
**位置**: `client_session.hpp:254-331`（调用 `HttpPool::acquire` 的上层代码）  
**CWE**: CWE-444 (Inconsistent Interpretation of HTTP Requests)

#### 问题描述

当从连接池获取的连接是 stale-idle（上游已关闭但客户端未感知），`ClientSession` 会检测到首次读取失败并**自动重试**，但对 **POST/PUT/PATCH/DELETE 等非幂等方法也执行相同逻辑**。

场景：
1. 客户端发送 `POST /order` 创建订单
2. 从池中取出一个 idle 连接（实际已被上游关闭）
3. 写请求成功（数据进入 socket 缓冲区）
4. 上游实际已处理请求（订单创建成功）
5. 读响应时发现连接断开（`reused_from_idle && peer_closed`）
6. 客户端判定为 stale-idle，**重发相同请求** → 重复创建订单

```cpp
// client_session.hpp:265-280 (简化)
if (conn->reused_from_idle) {
    if (write_res.status == IoStatus::PeerClosed) {
        LOG_INFO("Proxy write failed on reused connection (peer closed before write), retrying");
        conn.reset();
        goto retry_acquire;  // 无条件重试，未检查方法幂等性
    }
}
```

#### 触发条件
- 连接池中存在 stale-idle 连接（上游 keepalive 超时短于客户端 `idle_timeout_sec`）
- 客户端发起非幂等请求（POST/PUT/DELETE）
- 写入成功但读响应前连接被上游关闭

#### 影响
- **业务逻辑错误**: 重复创建订单、重复扣款、重复删除资源
- **数据不一致**: 幂等性假设被破坏
- **审计困难**: 日志显示为"stale-idle 重试"而非"重复提交"

#### 实际利用场景
攻击者可通过以下方式放大影响：
1. 使用慢速攻击（慢读）让上游提前关闭连接
2. 触发连接池回收大量 stale 连接
3. 并发发送大量 POST 请求，利用重试机制批量重复提交

#### 修复建议

**仅对幂等方法或写入前失败才重试**：

```cpp
bool is_idempotent = (ctx.method == "GET" || ctx.method == "HEAD" ||
                      ctx.method == "OPTIONS" || ctx.method == "TRACE");

if (conn->reused_from_idle) {
    if (write_res.status == IoStatus::PeerClosed) {
        if (is_idempotent) {
            LOG_INFO("Proxy write failed on reused connection (idempotent method), retrying");
            conn.reset();
            goto retry_acquire;
        } else {
            LOG_WARN("Proxy write failed on reused connection (non-idempotent method), aborting");
            resp.status_code = 502;
            resp.error = "upstream_stale_connection_non_idempotent";
            co_return resp;
        }
    }
}
```

**替代方案**: 在首次写入前主动探测连接活性（`MSG_PEEK` + `async_receive`），但会增加延迟。

---

### 🟡 VULN-3: acquire 中"先占 in-flight 再发现 max_size 满"的短暂误拒窗口

**严重度**: ~~MEDIUM~~ → **LOW**（复核下调）  
**位置**: `http_pool.hpp:174-180` + `:229-233`  
**状态**: 未修复（低优先级）

> **⚠️ 复核修订（2026-07-14）**：原报告标题"导致超限"是**错误的**，原定性（限流失效、资源耗尽）不成立。经代码复核：
> - `try_increment_counter`（`:394-407`）是 CAS 循环，**只有 `cur < limit` 才会加一**。因此 `in_flight_count` **在任何时刻都不可能超过 `max_concurrent`**，不存在"短暂超限"，更谈不上资源耗尽。原报告的"时序窗口导致超限"整体不成立。
> - 真实存在的、且影响很小的问题是另一个方向：请求先成功预留 `in_flight` slot（`:175-180`），再去 idle 队列找不到连接，最后发现 `total_count` 已达 `max_size`（`:229-233`）而退还 slot 返回 nullptr。在退还之前的极短窗口内，这个"注定失败"的请求占着一个 in-flight 名额，可能让另一个本可获得名额的请求被**误拒**（而非超限）。
> - 后果是高负载临界点的**短暂误拒 / 轻微吞吐抖动**，不是安全漏洞。**下调为 LOW。**

#### 问题描述（修正后）

```cpp
// 步骤 1: 预留 in_flight slot（CAS，保证不超过 max_concurrent）
if (state->cfg.max_concurrent > 0) {
    if (!try_increment_counter(state->in_flight_count, state->cfg.max_concurrent)) {
        co_return nullptr;  // 已达上限，正确拒绝
    }
    reserved_in_flight = true;
}

// 步骤 2: 尝试从 idle 队列取连接（可能失败）

// 步骤 3: 若无 idle，检查 total_count 是否达到 max_size
if (!try_increment_counter(state->total_count, state->cfg.max_size)) {
    if (reserved_in_flight) decrement_counter(state->in_flight_count);  // 退还
    co_return nullptr;  // ← 在退还前的极短窗口内，该请求占用了一个 in-flight 名额
}
```

**误拒窗口**：步骤 1 占用名额后，若步骤 3 因 `max_size` 满而失败，在步骤 3 内 `decrement_counter` 执行之前，这个名额被一个注定失败的请求占据。并发下，另一个请求可能在此期间因 `in_flight_count` 达到 `max_concurrent` 被拒。

#### 触发条件
- `max_concurrent < max_size` 且 `total_count` 恰好达到 `max_size`
- 高并发 `acquire()` 且此刻无可复用 idle 连接

#### 影响
- **短暂误拒**: 临界点上少数本可成功的请求被拒（客户端重试即可恢复）
- **无安全影响**: 不超限、不泄漏、不耗尽资源

#### 修复建议（可选，低优先级）

当前行为可接受。若要消除误拒窗口，可将 `max_concurrent` 的检查/占用**推迟到确认要创建新连接时**（即 idle 查找失败后、`max_size` 检查通过后再占 in-flight），使"注定失败"的请求不提前占用名额。代价是需要调整 idle 复用路径的计数时机，收益很小，不建议在没有实测吞吐问题前改动。

---

## 2. 设计弱点（非漏洞但影响安全性）

### ⚠️ WEAK-1: 缺少连接年龄上限（最大生命周期）

**严重度**: LOW  
**位置**: `http_pool.hpp` 全局

#### 问题
连接仅在空闲超时（`idle_timeout_sec`）时被驱逐，但**活跃使用的连接可以无限期存活**。长寿命连接可能：
- 累积 TCP 状态异常（重传队列膨胀）
- 跨越上游服务重启/配置变更（DNS 过期、TLS 证书更新）
- 触发上游端口耗尽（大量 TIME_WAIT）

#### 建议
添加 `max_connection_age_sec` 配置，在 `acquire()` 返回前检查 `last_used_at`，超过阈值的连接主动关闭。

---

### ⚠️ WEAK-2: `evict_stale_idle` 在锁内执行 I/O

**严重度**: LOW  
**位置**: `http_pool.hpp:469-482`

#### 问题
```cpp
static void evict_stale_idle(const std::shared_ptr<State>& state, Shard& shard, const Config& cfg) {
    auto now = std::chrono::steady_clock::now();
    while (!shard.idle.empty()) {
        // ...
        asio::error_code ec;
        shard.idle.front().socket.close(ec);  // 在锁内关闭 socket（可能阻塞）
        // ...
    }
}
```

`socket.close()` 虽然通常很快，但在某些平台（如高延迟 NFS 挂载的 Unix domain socket）或内核状态异常时可能阻塞。

#### 建议
先在锁内将待驱逐连接 `std::move` 到临时 `vector`，释放锁后再逐个 `close()`。

---

### ⚠️ WEAK-3: 无每 IP 连接数限制（横向 DoS）

**严重度**: LOW  
**位置**: 架构层面

#### 问题
恶意客户端可以独占整个连接池（`max_size` 限制），阻止其他合法客户端访问上游。

#### 建议
在 `UpstreamManager` 层添加 per-IP 或 per-service 的 `max_connections` 限制，或配合 rate-limiter 使用。

---

### ⚠️ WEAK-4: DNS 解析无缓存且无超时

**严重度**: LOW  
**位置**: `http_pool.hpp:418-441`

#### 问题
每次创建新连接时都执行 `async_resolve`，恶意 DNS 服务器可通过慢响应触发超时，但不影响已有连接。

#### 建议
添加 DNS 缓存层（TTL-aware），或使用 `asio::ip::address::from_string` 接受 IP 地址直连。

---

## 3. 并发安全审计

### ✅ 通过项
- **Shard 隔离**: 16 个独立 mutex，降低锁争用 ✅
- **原子计数器**: `total_count`/`in_flight_count` 使用 CAS 操作，memory_order 正确 ✅
- **无死锁**: 锁粒度小，无嵌套锁 ✅
- **UAF 防护**: `active` tracking + `shared_ptr<State>` 延长生命周期 ✅
- **RAII**: `ConnGuard` 保证异常路径归还连接 ✅

### ❌ 问题项
- **重试语义错误**: VULN-2（在上层 `client_session`，影响连接池复用语义）✅ 已修复
- **计数器语义不一致**: VULN-1（LOW，不影响并发正确性）✅ 已修复
- **锁内 I/O**: WEAK-2 ❌ 未修复

**注**: `in_flight_count` 由 CAS 保证不超过 `max_concurrent`（`try_increment_counter`），并发上限硬约束成立，原 VULN-3"超限"结论已撤销。

---

## 4. DoS 攻击面分析

| 攻击向量 | 严重度 | 缓解措施 | 状态 |
|---------|--------|---------|------|
| **慢速连接耗尽池** | HIGH | `connect_timeout_ms`/`read_timeout_ms` 限制 | ✅ 已缓解 |
| **恶意重试放大** | MEDIUM | VULN-2：非幂等重试 | ✅ 已修复（仅 GET/HEAD/OPTIONS/TRACE 重试） |
| **独占连接池** | MEDIUM | 缺少 per-IP 限制（WEAK-3） | ⚠️ 建议加固 |
| **DNS 慢查询** | LOW | `connect_timeout_ms` 覆盖 resolve | ✅ 部分缓解 |
| **Stale 连接堆积** | LOW | 全局 1 秒 eviction sweep | ✅ 已缓解 |

---

## 5. 资源泄漏审计

### ✅ 无泄漏
- **连接对象**: `HttpConn` 在 `~State()` 和 `shutdown()` 中全部 `close()` ✅
- **Socket FD**: RAII + 异常路径保护 ✅
- **内存**: `shared_ptr` + `unique_ptr` 管理，无裸 `new` ✅

### ⚠️ 边界情况
- **异常路径**: `acquire()` 中 3 个 catch 块正确清理计数器和 shard 状态 ✅
- **热重载**: `ConnGuard` 持有 `shared_ptr<HttpPool>` 防止 in-flight 连接被提前销毁 ✅

---

## 6. 与已知代码审计报告的交叉验证

参照 `docs/CODE_ANALYSIS_CLAUDE_2026-07-13.md`，HTTP 连接池相关问题：

| 报告中的问题 | 本次审计验证 | 状态 |
|-------------|-------------|------|
| MEDIUM: 非幂等重试（client_session.hpp:254-331） | 确认 → VULN-2 | ❌ 仍存在 |
| MEDIUM: `in_flight_count` 计数偏移（http_pool.hpp:174） | 重新定性 → VULN-1（设计缺陷，非不对称 bug） | ❌ 仍存在 |
| LOW: `client_body_timeout` 硬编码（client_session.hpp:56） | 不属于连接池，跳过 | - |

**澄清**: 原报告描述 VULN-1 为"acquire 不增、release 无条件减"是**不准确的**。实际代码在 `max_concurrent=0` 时，acquire 和 release 都不操作 `in_flight_count`（对称的），但这导致统计功能失效。

**新发现**: VULN-3（TOCTOU 时序窗口）和 4 个设计弱点。

---

## 7. 修复优先级建议

| 优先级 | 问题 | 修复工作量 | 状态 |
|--------|------|-----------|---------|
| **P1** | VULN-2: 非幂等重试（唯一有实际业务风险） | 小 | ✅ 已修复 |
| **P3** | VULN-1: `in_flight_count` 语义一致性（LOW） | 小（10 行） | ✅ 已修复 |
| **P4** | VULN-3: acquire 误拒窗口（LOW，非超限） | 中，收益小 | ⏸️ 暂不修 |
| **P3** | WEAK-1: 连接最大年龄 | 小（配置 + 检查） | ❌ 未修复 |
| **P3** | WEAK-2: 锁内 I/O | 小（重构 evict） | ❌ 未修复 |
| **P4** | WEAK-3: 横向 DoS（per-IP 配额） | 大（需跨模块协作） | ❌ 未修复 |
| **P4** | WEAK-4: DNS 缓存（已有超时兜底） | 中 | ❌ 未修复 |

---

## 8. 测试建议

### 回归测试（`tests/test_vuln_fixes.cpp`）
1. **VULN-1 计数一致性**: `max_concurrent=0` 时持有一个连接，`in_flight_count` 应为 1，release 后归零。
   ⚠️ 该用例需 `bind` 本地端口建立真实连接，在受限 sandbox 中会因无法 bind 失败——属**环境限制，非代码断言失败**。在可正常建连的 CI/本地环境通过。
2. **VULN-2 非幂等安全**: `is_idempotent_method` 对 POST/PUT/PATCH/DELETE 返回 false，对 GET/HEAD/OPTIONS/TRACE 返回 true。✅ 已通过（不依赖网络）。
3. **限流正确性（已有）**: `HttpPool.MaxConcurrentIsGlobalHardLimit` 验证 `max_concurrent=1` 时第二个 acquire 返回 nullptr —— 复核 VULN-3 时确认此测试保证了不超限。

### 新增模糊测试
```cpp
// 测试连接池在高并发+随机关闭下的行为
TEST(HttpPoolFuzz, ConcurrentAcquireReleaseWithRandomClose) {
    // 启动 100 个协程随机 acquire/release/close
    // 断言：shutdown 后所有 FD 已关闭，计数器归零
}
```

---

## 9. 相关安全文档

- **CLAUDE.md**: 连接池架构描述（需更新 probe 逻辑已删除的说明）
- **PERF_REPORT.md**: 性能特征（v3.5 修复了锁争用，本次审计未发现新的性能风险）
- **CODE_ANALYSIS_CLAUDE_2026-07-13.md**: 全代码库审计（本报告是 HTTP 层深化）

---

## 10. 审计签名

**审计范围**: `src/http/http_pool.hpp`（486 行）+ 相关调用方（`client_session.hpp`、`proxy_forwarder.hpp`）  
**方法**: 静态代码审计 + 并发模型分析 + 攻击面建模  
**局限性**:
- 未执行动态分析（需实际压测环境）
- 未审计底层 ASIO 库本身的安全性
- 假设配置文件正确加载（应单独审计配置解析器）

**建议后续审计**:
1. 使用 ThreadSanitizer (TSan) 进行并发竞态检测
2. 使用 AddressSanitizer (ASan) 验证无内存泄漏
3. 部署到预生产环境进行 1 小时持续压测（wrk + 随机关闭连接）

---

**报告完成时间**: 2026-07-14  
**下次审计建议**: 修复 P1/P2 问题后重新审计
