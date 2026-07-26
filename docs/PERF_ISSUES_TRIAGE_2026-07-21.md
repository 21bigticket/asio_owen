# 性能问题排查报告 — 2026-07-21

> 来源：一次常态运行日志（19:52:34 启动 ~ 20:04:34）+ `./bench.sh` 四组压测（health / redis / mysql / config）。
> 排查范围：`ReloadService`、`MysqlPool` maintain 线程、`HttpPool` idle 回收。
> 结论：**三处独立问题，均已定位到具体代码行**。#1 影响热路径尾延迟、优先级最高。
>
> **状态更新（2026-07-21，另一 agent 实施 + 本 agent 复核）：三处全部已修复并通过复核，新增回归测试。详见文末「修复记录」及各问题末尾的 ✅ 段。**

---

## 压测基线（本次）

| 接口 | RPS | avg lat | max lat | 说明 |
|------|-----|---------|---------|------|
| Health | ~134k | 0.66ms | 8.08ms | 纯内存，接近框架天花板 |
| Redis | ~31k | 2.90ms | 34ms | worker 模式去掉 per-acquire SELECT 后 +24%（较早一轮 25k） |
| MySQL #1 | 12247 | 8.82ms | **270.58ms** | 尾延迟异常抖动，stdev 12.11ms |
| MySQL #2 | 12804 | 7.05ms | 53.12ms | stdev 1.66ms，明显更平稳 |
| Config Direct | ~18k | 5.5ms | 128ms | 上游服务本身 |
| Config Gateway | ~16k | 5.9ms | 78ms | 上游 + 网关转发，较 direct 约 -13% |

> MySQL #1 与 #2 的 max/stdev 差距（270ms vs 53ms）是本次排查的直接触发点，见 **问题 #1**。

---

## 问题 #1 — Security rules 每 30s 无条件全量重载（严重度：高）

### 现象

常态日志中，每个 maintain 周期稳定成对出现，从启动到结束从未间断，且期间配置文件毫无改动：

```
[19:53:04.416] Security rules loaded
[19:53:04.416] Security rules hot-reloaded
[19:53:34.418] Security rules loaded
[19:53:34.418] Security rules hot-reloaded
... 每 30s 一次，全程如此
```

### 根因

`src/app/reload_service.hpp:36` `ReloadService::schedule()` 的定时回调**无条件**执行全量 reload，没有任何 mtime / hash / 内容比对短路：

```cpp
void schedule(int interval_sec) {
    timer_.expires_after(std::chrono::seconds(interval_sec));
    timer_.async_wait([this](std::error_code ec) {
        if (ec || !running_) return;

        Config new_cfg;
        int next_sec = 30;
        if (new_cfg.load(config_base_)) {          // ← 每 tick 全量扫 config.d/ 读磁盘解析
            security_rules_.reload(new_cfg);        // ← 无条件 re-apply
            upstreams_.reload(new_cfg, http_pool_config_from(new_cfg));
            next_sec = new_cfg.get_int("security", "config_reload_interval_sec", 30);
        }
        if (running_ && next_sec > 0) schedule(next_sec);
    });
}
```

`Config::load()`（`src/common/config.hpp:43`）每次都 `directory_iterator` 扫 `config.d/`、逐文件 `getline` 解析、重建 `unordered_map` + `raw_entries_` vector。这部分是纯浪费，但不是最痛的。

**最痛的是锁竞争**。`SecurityRules::reload()`（`src/security/security_rules.hpp:242`）：

```cpp
void reload(const Config& cfg) {
    std::lock_guard<std::mutex> lock(rules_mu_);   // ← 写锁
    try {
        load_from_config(cfg);                     // ← 锁内跑完整个重建：解析 + 建 JWT + reload rate limiter ...
        LOG_INFO("Security rules hot-reloaded");
    } catch (const std::exception& e) {
        LOG_ERROR("Security rules hot-reload rejected, keeping previous rules: ", e.what());
    }
}
```

而**每一个请求**的热路径 `SecurityRules::check()`（`src/security/security_rules.hpp:147`）用**同一把** `rules_mu_` 做状态快照：

```cpp
{
    std::lock_guard<std::mutex> lock(rules_mu_);   // ← 与 reload() 同一把锁
    proxies_copy = trusted_proxies_;
    jwt_copy = jwt_auth_;
    rate_limiter_copy = rate_limiter_;
    case_sensitive_copy = case_sensitive_paths_;
}
```

### 影响

每 30s，`reload()` 持写锁跑完整个 `load_from_config`（含 JWT 构建、rate limiter reload）期间，**所有请求线程在 `check()` 第 147 行阻塞排队**。在 130k+ RPS 下，哪怕 stall 只有几 ms，也会瞬间堆积上千请求 —— 这与 MySQL bench **#1 的 `max=270ms` 尾延迟 / stdev 12ms**（而 #2 平稳）高度吻合：#1 恰好压中了一次周期性 reload 窗口。

且这一切发生在**配置根本没有变化**的前提下，是纯粹的自造抖动。

### 修复方向（待实施）

1. **mtime/hash 短路**：`ReloadService` 记录上次 `config.d/` 所有 `.ini` 的 mtime（或内容 hash），未变化则整块跳过 `load()` + 两个 `reload()`。这是最小改动，直接消除 99.9% 的无谓重载。
2. （可选，纵深）把 `SecurityRules` 内部改为 `shared_mutex` 或 shared_ptr 快照换指针，让 reload 只在最后一刻换指针，`check()` 走读锁 / 无锁 —— 即便真有配置变更也不 stall 热路径。参考 `UpstreamManager` 已经用的 `shared_mutex` + shared_ptr pool 模式。

> 建议先做 1（收益大、风险低、可立即压测验证），2 视验证结果决定。

### ✅ 已修复（方向 1，2026-07-21）

- **文件**：`src/app/reload_service.hpp`
- **改法**：`start()` 时记录 `config.d/*.ini` 的 fingerprint（文件名 + `file_size` + `last_write_time`，按名排序）；每个 tick 先 `read_config_fingerprint()` 比对，未变化则**整块跳过** `Config::load()` + `SecurityRules::reload()` + `UpstreamManager::reload()`，只在变更时才重载并更新 fingerprint。
- **效果**：配置不变时不再触碰 `rules_mu_` 写锁，热路径 `check()` 的周期性 stall 消除。fingerprint 用 2 次 stat 替代全量 parse，开销可忽略。
- **复核**：逻辑正确（排序后比较 / `next_sec` 未变分支回落 `interval_sec` / 变更后才 `std::move` 更新 last）。
- **遗留（非 bug）**：fingerprint 用 mtime+size，挡不住「同 mtime 同 size 的内容替换」这类极端场景。对常规运维足够；真要防可后续升级为内容 hash。方向 2（`shared_mutex`/shared_ptr 换指针）**未做**，当前无必要。

---

## 问题 #2 — MySQL 连接池周期性丢弃并重建 min_size 连接（严重度：中）

### 现象

稳态（零业务流量）下，maintain 线程几乎每个周期都在“回收 → 立即补回”，`total/idle` 始终回到 8，且回收数量在 3/1 之间诡异交替：

```
[19:53:34.417] maintain: recycled 3 idle connections
[19:53:34.431] maintain: added 3 connections
[19:53:34.432] maintain: pool status total=8, idle=8
[19:54:04.432] maintain: recycled 1 idle connections
[19:54:04.438] maintain: added 1 connections
...
```

启动时建 8 条连接耗时约 30ms（`34.381 → 34.411`），也就是说这些健康的、已认证的 TCP 连接被反复 `mysql_close` 再重连。

### 根因

`src/db/mysql_pool.hpp:309` `do_maintain()` 三个 Phase 配合出了问题：

- **Phase 1（第 318–326 行）** 按 `max_idle_sec`(默认 60s) 回收 idle 连接时**完全不看 `min_size`**，把暖池里的常驻连接也一并 `mysql_close`：

  ```cpp
  while (!idle_pool_.empty()) {
      auto age = ...(now - idle_pool_.front().last_used_at)...;
      if (age < cfg_.max_idle_sec) break;   // ← 只看年龄，不看是否低于 min_size
      to_close.push_back(idle_pool_.front().conn);
      idle_pool_.pop_front();
      --total_;
  }
  ```

- **Phase 2（第 339–373 行）** 随即发现 `idle_pool_.size() < min_size`，又重新建连补回。

- **Phase 3（第 377–401 行）** 健康检查只 ping 最旧的 `max_check=4` 条，ping 通后 `push_back` 并**刷新 `last_used_at = now`**：

  ```cpp
  idle_pool_.push_back({to_check[checked].conn, std::chrono::steady_clock::now()});
  ```

  于是这 4 条被“续命”永不过期；剩下未被 ping 到的那几条越过 60s，下一轮被 Phase 1 回收 —— 这正是 `recycled 3` / `recycled 1` 交替计数的来源（每轮被续命的 4 条不同，剩余过期数在变）。

### 影响

- 稳态暖池被反复销毁重建，浪费 CPU / 网络 / MySQL 侧握手开销。
- 回收后到补回之间的窗口，若刚好有请求进来会命中新建的冷连接（首查略慢）。
- 与 `max_idle_sec` 的语义预期相悖：正常池设计中，`max_idle` 只应裁剪**超过 `min_size` 的富余连接**。

### 修复方向（待实施）

- **Phase 1 回收前判断 `min_size` 下限**：只回收 `total_ - min_size` 范围内的超额 idle 连接，min_size 以内的连接**永不因年龄回收**，仅靠 Phase 3 的 ping 保活。
- 一并核对 Phase 3 刷新时间戳的合理性：保活成功刷新 `last_used_at` 本身没错，但要与 Phase 1 的 min_size 保护配合，否则仍会出现“部分续命、部分被杀”的偏斜。

### ✅ 已修复（2026-07-21）

- **文件**：`src/db/mysql_pool.hpp`（`do_maintain` Phase 1，约第 318 行）
- **改法**：回收循环的 while 条件加下限保护 —— `while (!idle_pool_.empty() && total_ > cfg_.min_size)`。
- **效果**：稳态 `total == min_size` 时 Phase 1 直接不进循环，暖池连接不再被周期性 `mysql_close` + 重建；`max_idle_sec` 回归其应有语义（只裁剪超过 min_size 的富余连接）。Phase 3 的时间戳刷新在有了 min_size 保护后不再造成偏斜。
- **复核**：逻辑正确，churn 消除。

---

## 问题 #3 — Gateway HttpPool idle 连接停止流量后不回收（严重度：中）

### 现象

config 压测把 `zebra-config` 池撑到高水位，压测结束后数字冻结、再不下降：

```
[20:01:04] zebra-config={total=93, idle=3,  active=90, in_flight=90, reused=412020, ...}   ← 压测中
[20:02:04] zebra-config={total=94, idle=94, active=0,  in_flight=0,  reused=960556, ...}   ← 压测刚结束
[20:02:34] zebra-config={total=94, idle=94, ...}   ← 之后每 30s 完全不变
[20:04:34] zebra-config={total=94, idle=94, ...}   ← 直到日志结束仍是 94
```

94 条上游连接全部 idle，却始终不释放，长期占用 fd。

### 根因

`src/http/http_pool.hpp` 的 idle 回收**完全是 lazy 的，只由 `acquire()` 驱动**，没有任何后台定时器：

- `acquire()` 入口（第 152–168 行）有一个 1s 节流的“全局 sweep”，遍历所有 shard 调 `evict_stale_idle`；
- 每次 pop idle 前（第 197 行）也会对当前 shard `evict_stale_idle`。

两者**都只在 `acquire()` 被调用时才执行**。压测一停，`zebra-config` 不再有任何 `acquire()`，回收逻辑永远不触发，94 条连接就永久停在池里。

此外，公有方法 `evict_stale()`（第 382 行）**全项目零调用**，是死代码 —— 本应作为后台清理入口但从未接线。`PoolStatsService`（`src/app/pool_stats_service.hpp`）的定时器也只打印 `pool_stats()` 日志，不驱动任何回收。

```cpp
// http_pool.hpp:382 — 定义了但没人调
size_t evict_stale() {
    ...
    evict_stale_idle(state, shard, state->cfg);
    ...
}
```

### 影响

- 流量停止 / 流量转移后，闲置上游连接长期占用 fd，直到该上游下次来流量才被清。
- 单上游 94 条只是压测规模；生产多上游 + 流量波动下，可能累积大量僵尸 idle 连接。
- 与 `idle_timeout_sec=60` 的配置语义预期不符（用户以为 60s 后会回收，实际不会）。

### 修复方向（待实施）

- 给 `HttpPool` 或 `UpstreamManager` 接一个**后台定时器**（复用现有 `PoolStatsService` 的定时器最省事），周期性对所有上游池调用现成的 `evict_stale()`，让回收不再依赖流量。
- 或：把 `evict_stale()` 从死代码转正，由 `PoolStatsService` 每 tick 顺带调用一次（打 stats 的同时做清理），一石二鸟。

### ✅ 已修复（采纳「一石二鸟」方案，2026-07-21）

- **文件**：`src/http/upstream_manager.hpp`、`src/app/pool_stats_service.hpp`
- **改法**：新增 `UpstreamManager::evict_stale()`，`shared_lock` 遍历所有 upstream pool 调用现成的 `HttpPool::evict_stale()`；`PoolStatsService::schedule` 每 tick 在打印 stats 前先 `upstreams_.evict_stale()` 驱动一次回收。`HttpPool::evict_stale()` 由此从死代码转正。
- **效果**：idle 回收不再依赖后续 `acquire()` 流量，停流量后闲置上游连接按 `idle_timeout_sec` 正常释放，不再累积僵尸 fd。
- **锁复核**：`evict_stale()` 取 `shared_lock`（只读遍历 `pools_` map，与 `reload()` 的 `unique_lock` 互斥），shard 内 idle 删除由 `HttpPool` 每-shard mutex 保护；在 io_context 线程 close idle socket 与 `acquire()` inline 回收同线程，无新增风险。
- **回归测试**：`tests/test_http_pool.cpp` 新增 `HttpPool.EvictStaleReclaimsIdleWithoutAcquire` —— `idle_timeout_sec=0` 制造 stale，不经 acquire 直接 `evict_stale()`，断言 `total_count` 1→0。本地跑通（2ms PASS）。
- **运行时验证（✅ 完全，2026-07-22 补测）**：
  - 早先日志已证明**无流量残留回收**：`zebra-config` 从 `total=1 → total=0`（20:24:58 → 20:25:28）。
  - 2026-07-22 补测覆盖了**压测后高水位回收**（此前唯一缺口）：`./bench.sh config` 于 09:48:35 结束，日志显示
    ```
    09:48:42  zebra-config={total=93, idle=93, active=0, ...}   ← 刚结束，93 条全 idle
    09:49:12  zebra-config={total=93, idle=93, ...}             ← ~30s 后仍持有
    09:49:42  zebra-config={total=0,  idle=0, reused=794919}    ← 全部回收归零
    ```
    高水位 93 条在停流量 ~60–90s 内（≥ `idle_timeout_sec`）被后台 `evict_stale()` tick 全部释放。**#3 至此完全验证通过。**

---

## 优先级与实施建议

| # | 问题 | 严重度 | 改动量 | 状态 |
|---|------|--------|--------|------|
| 1 | Security rules 无条件重载 + 热路径同锁 | **高** | 小（加 mtime 短路） | ✅ 已修复 + 运行时验证 |
| 2 | MySQL 池暖连接 churn | 中 | 中（Phase 1 加 min_size 保护） | ✅ 已修复 + 运行时验证 |
| 3 | HttpPool idle 不回收 | 中 | 小（接后台定时器） | ✅ 已修复 + 回归测试 + 运行时验证（含压测后高水位回收，2026-07-22 补测） |

三者互相独立，已分别修复，并已跑过一轮 `./bench.sh`（health/redis/mysql/config）+ 8 分钟运行日志验证。结果见下节。

### 运行时验证结果（2026-07-21 压测 + 日志，双 agent 复核）

- **#1 reload — ✅ 完全验证**：日志中 `Security rules loaded` **仅在启动时出现一次**（20:24:28.648），此后 8 分钟无任何重载。对比修复前每 30s 一对 `loaded/hot-reloaded`，无变更不再重载的目标达成。
- **#1 ↔ MySQL 尾延迟 — ✅ 病态尖刺已消除（但非全部长尾）**：修复前 MySQL #1 `max=270.58ms / stdev=12.11ms` 的可复现病态尖刺，本轮**未复现**，两轮收敛到 `max=63.78ms / 73.40ms`、`stdev≈1.5/1.9ms`。吞吐持平（~12.3k RPS，符合“消 stall 不增吞吐”预期）。
  - **免责**：同轮 Health #2 出现过 `max=164.76ms`，而 health **不触发 reload、不涉及 gateway 池**——说明环境本身仍有偶发调度/系统级长尾。因此结论限定为：**#1 消除了 MySQL 那个可复现、stdev 高的病态尖刺**；偶发的单次百毫秒长尾属环境级，**不归因于 #1，也未声称消除**。
- **#2 MySQL churn — ✅ 完全验证**：稳态长时间保持 `total=8, idle=8`，无修复前每 30s 的 `recycled→added` 空转。压 MySQL 后扩到 14、回收到 10、最终回落 8——这是对**超过 min_size 富余连接**的正常回收，与 min_size 保护语义一致。
- **#3 HttpPool 回收 — ✅ 完全验证（2026-07-22 补测）**：无流量下 `zebra-config total=1→0`（20:24-20:25）证明**后台 tick 回收生效**；压测后高水位回收缺口已于 2026-07-22 补上——`./bench.sh config`（09:48:35 结束）后 `zebra-config` 从 `total=93, idle=93` 静置 ~60-90s 回落至 `total=0`（09:49:42）。

### 待补测 / 待观察

- **#3 压测后高水位回收 — ✅ 已于 2026-07-22 补测完成**：`./bench.sh config`（09:48:35 结束）后不停服务静置 ~60-90s，`server.log` 中 `zebra-config` 从 `total=93, idle=93`（09:48:42）回落至 `total=0, idle=0`（09:49:42），后台 `evict_stale()` tick 按 `idle_timeout_sec` 正常驱动。**三处 fix 至此全部运行时验证完成，无遗留待测项。**
- **吞吐数字勿过度解读**：本轮环境噪声偏大（同一 health 压测在 98k~118k 间跳），故**不对吞吐 RPS 下“提升/回归”结论**。要判定吞吐影响需在安静环境多轮取中位数。
- **环境级长尾单独观察**：Health 的 164ms max 提示存在 MySQL 之外的全局抖动源（VM 调度 / 系统），与本次三处 fix 无关，需单独排查，勿把所有尾延迟都归给 MySQL。
- **#3 fd 累积（长期）**：多上游 + 长时间运行下的 fd 增长曲线，可用 `lsof -p <pid> | wc -l` 周期采样佐证。

---

## 修复记录

| # | 文件 | 改法一句话 |
|---|------|-----------|
| 1 | `src/app/reload_service.hpp` | fingerprint(name+size+mtime) 短路，配置未变则跳过 load + 两个 reload |
| 2 | `src/db/mysql_pool.hpp` | Phase 1 回收 while 条件加 `total_ > cfg_.min_size` 下限保护 |
| 3 | `src/http/upstream_manager.hpp`、`src/app/pool_stats_service.hpp` | 新增 `UpstreamManager::evict_stale()`，`PoolStatsService` 每 tick 打 stats 前驱动一次回收 |
| 测试 | `tests/test_http_pool.cpp` | 新增 `HttpPool.EvictStaleReclaimsIdleWithoutAcquire`，覆盖 #3 |

**验证**：`cmake --build build` 通过；新回归测试单跑 PASS（2ms）；`test_config_load` 7/7 PASS。`ctest` 全量在当前沙箱有 37/182 因 `bind: Operation not permitted`（listener socket 权限限制）失败，与本次改动无关。

**运行时验证（压测 + 8 分钟日志）**：#1 reload 无变更不再触发（loaded 仅启动一次）；#1 MySQL 病态尾延迟 270ms→63ms 收敛；#2 稳态 8/8 无 churn；#3 无流量后台回收生效。详见「运行时验证结果」节。

**补测完成（2026-07-22）**：#3 压测后高水位回收已验证——`./bench.sh config` 跑完静置 ~60-90s，`zebra-config` 从 `total=93, idle=93` 回落至 `total=0`。**三处 fix 全部运行时验证完成，无遗留待测项。**

---

## 附录 A — 修复前后压测数据全表（2026-07-21）

同一 `./bench.sh`，各接口 2 轮 × 30s、30 线程 / 100 连接。「修复前」为本报告开头的基线，「修复后」为 patch + rebuild 后同脚本重跑。

### MySQL（核心验证点）

| 轮次 | 指标 | 修复前 | 修复后 | 说明 |
|------|------|--------|--------|------|
| #1 | RPS | 12247 | 13303 | 持平（环境噪声内） |
| #1 | avg lat | 8.82ms | 6.78ms | — |
| #1 | **max lat** | **270.58ms** | **63.78ms** | **病态尖刺消除** |
| #1 | **stdev** | **12.11ms** | **1.50ms** | **抖动大幅收敛** |
| #2 | RPS | 12804 | 11376 | 持平（环境噪声内） |
| #2 | avg lat | 7.05ms | 7.93ms | — |
| #2 | max lat | 53.12ms | 73.40ms | 同量级 |
| #2 | stdev | 1.66ms | 1.94ms | 同量级 |

> 结论：修复前 #1 那轮 `max=270ms / stdev=12` 的**可复现病态尖刺**（#2 正常，说明 #1 压中了 reload 窗口）在修复后**未复现**，两轮统一收敛到 60-73ms / stdev≈1.5-1.9ms。吞吐持平，符合“消除锁 stall 不改变吞吐”的预期。

### 其余接口（吞吐波动，仅供参考，勿据此下结论）

| 接口 | 修复前 RPS | 修复后 RPS | 备注 |
|------|-----------|-----------|------|
| Health | ~134k | 98k~118k（同压测跳动） | 环境噪声大；health 不涉及本次任何 fix |
| Redis | ~31k | ~28.6k | 噪声区间 |
| Config Direct | ~18k | ~19.5k | 上游自身负载在变 |
| Config Gateway | ~16k | ~13.6k | 噪声区间 |

> Health #2 曾出现 `max=164.76ms`。Health 路径**不触发 reload、不经 gateway 池**，此长尾证明环境本身存在偶发调度抖动——**与三处 fix 无关**，不计入 #1 账。因整轮吞吐噪声偏大（health ±20%），本报告**不对吞吐 RPS 下“提升/回归”结论**。

## 附录 B — 关键运行时日志证据（server.log，20:24:28 启动起 ~8 分钟）

**#1 reload 无变更不再触发** — 全日志 `Security rules loaded` 仅一次：
```
[20:24:28.648] Security rules loaded          ← 仅启动这一次
（此后 8 分钟无任何 loaded / hot-reloaded）
```
对比修复前：每 30s 一对 `Security rules loaded` + `Security rules hot-reloaded`。

**#2 MySQL 稳态无 churn** — total=8 池长期静止，无 recycled→added 空转：
```
[20:24:58.648] maintain: pool status total=8, idle=8
[20:25:28.648] maintain: pool status total=8, idle=8
...（稳态持续，无 recycled/added 成对出现）
```
（日志中另一条 total 在 4↔8 波动并伴随 added/recycled 的是 **Redis worker 池**，redis bench 压到 created_total=7 后的正常富余扩缩容，非 MySQL、非 bug。）

**#3 HttpPool 无流量后台回收生效** — zebra-config 残留连接被 tick 清除：
```
[20:24:58.649] HttpPool stats: ... zebra-config={total=1, idle=1, ...}
[20:25:28.649] HttpPool stats: ... zebra-config={total=0, idle=0, ...}   ← 无流量下自动归零
```
修复前：无后续 acquire 时该值永久冻结不降。

**#3 压测后高水位回收（2026-07-22 补测）** — `./bench.sh config` 于 09:48:35 结束，其后 `zebra-config` 高水位在停流量后完整回落：
```
[09:48:42.897] HttpPool stats: ... zebra-config={total=93, idle=93, active=0, ...}   ← 刚结束，93 条全 idle
[09:49:12.898] HttpPool stats: ... zebra-config={total=93, idle=93, ...}             ← ~30s 后仍持有
[09:49:42.900] HttpPool stats: ... zebra-config={total=0,  idle=0, reused=794919}    ← 后台 tick 全部回收归零
```
高水位 93 条在 ≥ `idle_timeout_sec` 后被 `evict_stale()` 释放，**#3 完全验证通过**。

---

*排查者：Claude Code / 2026-07-21（定位）。修复：另一 agent 实施 + 本 agent 复核。运行时验证：双 agent 复核压测日志。同日。*
