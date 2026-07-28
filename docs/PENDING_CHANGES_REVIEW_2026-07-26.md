# 未提交改动代码审查 — 2026-07-26

> 审查范围：截至 2026-07-26 工作树中相对 `HEAD`（`88b0460 v2-shell`）的全部未提交改动。
> 改动性质：`PERF_ISSUES_TRIAGE_2026-07-21.md` 中三处性能修复的实现 + bench 脚本增强 + 两篇排查文档。
> 审查方式：静态逐行审查（未重新执行压测）；对每个"看似可疑"的点都回查了被调用方源码。
>
> **结论：三处核心修复逻辑全部正确，未发现 P0/P1 bug。回归测试断言核实无误。仅 bench 脚本存在 1 处 shell 语法瑕疵（低危），另有若干非阻塞观察项。**

---

## 1. 改动清单

```
M bench/bench_full.sh             +151  压测脚本增强：进程级系统监控 + 异常检测 + 时间戳对齐
M src/app/pool_stats_service.hpp  +1    每 tick 打 stats 前调一次 upstreams_.evict_stale()
M src/app/reload_service.hpp      +78   config.d fingerprint(name+size+mtime)短路，未变更则跳过 load + 两个 reload
M src/db/mysql_pool.hpp           +2    do_maintain Phase 1 回收加 total_ > min_size 下限保护
M src/http/upstream_manager.hpp   +7    新增 UpstreamManager::evict_stale()，shared_lock 遍历所有 pool
M tests/test_http_pool.cpp        +49   新增回归测试 EvictStaleReclaimsIdleWithoutAcquire
?? docs/PERF_ISSUES_TRIAGE_2026-07-21.md   三处性能问题的定位/修复/验证报告
?? docs/TROUBLESHOOTING_LATENCY_SPIKES.md  尖刺延迟排查手册
```

对应 triage 报告中的三个问题：

| # | 问题 | 修复文件 | 状态 |
|---|------|---------|------|
| 1 | Security rules 每 30s 无条件全量重载，与热路径 `check()` 同抢 `rules_mu_` 写锁 | `reload_service.hpp` | ✅ 逻辑正确 |
| 2 | MySQL 池 maintain 周期性丢弃并重建 min_size 暖连接 | `mysql_pool.hpp` | ✅ 逻辑正确 |
| 3 | Gateway HttpPool idle 连接停流量后不回收（`evict_stale()` 死代码零调用） | `upstream_manager.hpp` + `pool_stats_service.hpp` | ✅ 逻辑正确 |

---

## 2. 核心修复逐行核实

### 2.1 问题 #1 — `reload_service.hpp` fingerprint 短路

**改法**：`start()` 时记录 `config.d/*.ini` 的 fingerprint（文件名 + `file_size` + `last_write_time`，按名排序）；每个 tick 先 `read_config_fingerprint()` 比对，未变化则整块跳过 `Config::load()` + `SecurityRules::reload()` + `UpstreamManager::reload()`。

**核实点 A — 与 `Config::load` 的过滤一致性（通过）**

`Config::load`（`src/common/config.hpp:43`）扫描 config.d 时的过滤条件：
```cpp
// config.hpp:58
if (entry.is_regular_file(ec) && entry.path().extension() == ".ini") {
```

`read_config_fingerprint`（`src/app/reload_service.hpp:80`）的过滤条件：
```cpp
if (!entry.is_regular_file(ec) || entry.path().extension() != ".ini") {
    ec.clear(); continue;
}
```

两者**完全一致**：都只看 regular file + `.ini` 后缀，都按 filename 排序。fingerprint 不会漏掉 `Config::load` 实际会读的文件，也不会误纳入非 `.ini` 文件。✅

**核实点 B — `config_changed` 三段判定（通过）**

```cpp
// reload_service.hpp:46
const bool config_changed =
    !current_fingerprint ||          // 当前读失败 → 视为变更（重试 load）
    !last_config_fingerprint_ ||     // 历史读失败 → 视为变更
    *current_fingerprint != *last_config_fingerprint_;
```
- 两者都有值且相等 → `changed=false`，跳过。这是优化的核心路径。✅
- 任一为 nullopt → `changed=true`，触发重试。语义合理（目录消失/恢复都能响应）。✅

**核实点 B' — fingerprint 只在 `load()` 成功时更新（通过，且无害）**

```cpp
// reload_service.hpp:52
if (new_cfg.load(config_base_)) {
    security_rules_.reload(new_cfg);
    upstreams_.reload(new_cfg, http_pool_config_from(new_cfg));
    next_sec = new_cfg.get_int("security", "config_reload_interval_sec", interval_sec);
    last_config_fingerprint_ = std::move(current_fingerprint);   // ← 仅 load 成功才更新
}
```
曾担心"配置 mtime 变了但内容语法错误 → load 失败 → 不更新 last → 每 tick 重试坏配置"。回查 `Config::load`（`config.hpp:67-71`）发现：单文件 `load_file` 失败只 `LOG_WARN`，整体 `load` 只要目录存在就返回 `true`。因此"load 失败"实际只在 **config.d 目录消失/非目录**时发生——此时 fingerprint 的 `current` 也为 nullopt，与"目录恢复后重试"的语义一致。**不会产生坏配置无限重试**。✅

**核实点 C — `next_sec` 在三个分支下的稳定性（通过）**

| 分支 | next_sec 取值 |
|------|--------------|
| 未变更 | `interval_sec`（保持上一轮传入值） |
| 变更且 load 成功 | `new_cfg.get_int(..., interval_sec)`（读配置，fallback `interval_sec`） |
| 变更但 load 失败 | `interval_sec`（保持） |

修改前 fallback 硬编码 `30`，现改为 `interval_sec` —— 这是合理改进。链式 `schedule(next_sec)` 在首次读到配置值后会稳定在该值（如配置写 60，之后每轮未变更时 `interval_sec` 即为 60）。✅

**核实点 D — `last_config_fingerprint_` 无并发风险（通过）**

仅在 `start()` 和 timer 回调中读写。timer 是单链 `async_wait` → 回调 → `schedule()`，同一时刻只有一个回调在跑，即便 `io_context` 多线程也不会并发访问该成员。无需原子/锁。✅

---

### 2.2 问题 #2 — `mysql_pool.hpp` Phase 1 min_size 保护

**改法**（`src/db/mysql_pool.hpp:318`）：
```cpp
- while (!idle_pool_.empty()) {
+ while (!idle_pool_.empty() && total_ > cfg_.min_size) {
```

**核实 — 边界推演（通过）**

`idle_pool_` 是 deque，`front()` 为最旧（按 `last_used_at` 升序），`age < max_idle_sec` 时 `break`（第 322 行）——遇未过期即停，因后续都更新。

加 `total_ > min_size` 后推演（设 `total_=10, min_size=8`，front 起 8 条过期、第 9 条未过期）：
- iter1: `10>8` ✓ → 回收最旧，`--total_` → 9
- iter2: `9>8` ✓ → 回收，`--total_` → 8
- iter3: `8>8` ✗ → 退出

结果保留 8 条（含 6 条仍过期但受 min_size 保护的暖连接）。**这正是预期语义**：`max_idle_sec` 只裁剪超过 `min_size` 的富余连接，min_size 以内的暖连接仅靠 Phase 3 ping 保活。与标准连接池设计一致。✅

`min_size=0` 时退化为原行为（`total_ > 0` 可回收全部 idle）。✅

Phase 3 刷新 `last_used_at = now` 在有了 min_size 保护后不再造成"部分续命、部分被杀"的偏斜（triage 报告所述）。✅

---

### 2.3 问题 #3 — `upstream_manager.hpp` + `pool_stats_service.hpp` 接线

**改法**：
- 新增 `UpstreamManager::evict_stale()`（`src/http/upstream_manager.hpp:106`）：`shared_lock` 遍历 `pools_`，对每个 pool 调 `HttpPool::evict_stale()`。
- `PoolStatsService::schedule`（`src/app/pool_stats_service.hpp:29`）每 tick 在 `LOG_INFO` 打 stats **前**先 `upstreams_.evict_stale()`。

**核实点 A — 锁层级与死锁（通过）**

```
UpstreamManager::evict_stale()   持 shared_lock(mtx_) 遍历 pools_
  └─ HttpPool::evict_stale()      持 shard.mtx 遍历 shards[]，调 evict_stale_idle
       └─ evict_stale_idle        close idle socket
```

- `shared_lock`（evict/pool_stats/route）与 `unique_lock`（reload）互斥，读读不互斥。✅
- `HttpPool::evict_stale` 只锁自己的 shard mutex，**不回调 UpstreamManager**，锁顺序单向，无死锁。✅
- 在 io_context 线程同步 close idle socket，与 `acquire()` inline 回收同线程，无新增跨线程风险。✅

**核实点 B — `HttpPool::evict_stale()` 的返回值语义（通过，且解释了测试断言）**

回查 `src/http/http_pool.hpp:382`：
```cpp
// 注释（第 381 行）：Returns number of idle connections remaining after eviction.
size_t evict_stale() {
    auto state = state_;
    size_t idle_total = 0;
    for (auto& shard : state->shards) {
        std::lock_guard lock(shard.mtx);
        evict_stale_idle(state, shard, state->cfg);
        idle_total += shard.idle.size();   // ← 累加"回收后剩余"的 idle
    }
    return idle_total;
}
```
**返回值是"回收后剩余的 idle 连接数"**，不是"回收的数量"。这一条是审查中最容易误判的点（直觉上以为返回回收数），但回查源码后确认：测试造 1 条 stale idle → 回收后 idle=0 → 断言 `evict_stale()==0u` **完全正确**。✅

**核实点 C — evict 频率开销（可接受）**

从 triage 报告附录 B 的日志时间戳，`HttpPool stats` 每 30s 一条，即 `evict_stale()` 每 30s 跑一次。单次开销 = Σ(上游数 × 16 shard × 加锁遍历 idle)。shard 内 idle 通常很少，开销微秒级，远低于 acquire 路径里每秒一次的 global sweep。✅

---

## 3. 回归测试核实 — `tests/test_http_pool.cpp`

新增 `HttpPool.EvictStaleReclaimsIdleWithoutAcquire`（第 308 行起）：

**流程**：
1. `accept_one_and_hold` 占住一个 server socket 不响应（让客户端 `connect` 能完成三次握手，但服务端不回数据）。
2. 协程 `acquire` 一条连接 → `release` 回 idle → 手动把 `last_used_at` backdate 为 `time_point{}`（epoch）。
3. `cfg.idle_timeout_sec = 0` → backdate 后 age 极大，`age < 0` 为假 → 判定 stale。
4. `ioc.run()` 结束后（协程跑完），同步调 `pool.evict_stale()`，断言 `total_count` 1→0。

**核实**：
- 覆盖了 #3 的核心场景——**不经 `acquire()` 路径**触发回收。这正是修复要解决的关键（修复前 evict 只在 acquire 内驱动，停流量后永不触发）。✅
- `evict_stale()` 在 `ioc.run()` 返回后同步调用，模拟 `PoolStatsService` tick 上的同步调用，合理。✅
- 断言 `EXPECT_EQ(pool.evict_stale(), 0u)` + `EXPECT_EQ(total_count, 0u)` 双重验证（剩余 idle=0 且 total 归零），正确。✅

**未覆盖（非缺陷，记录）**：多上游池、高水位（93 条）回收场景由运行时压测验证（triage 报告附录 B 已补测 2026-07-22），单元测试只覆盖单池单连接，够用。

---

## 4. 发现的问题

### 🟡 P3 — bench 脚本 `pgrep` 的 `|` / `||` 优先级（`bench/bench_full.sh:50`）

```bash
local target_pid=$(pgrep -x asio_owen 2>/dev/null || pgrep -x server 2>/dev/null | head -1)
```

bash 中管道 `|` 优先级高于 `||`，故实际解析为：
```
pgrep -x asio_owen || (pgrep -x server | head -1)
```

- `asio_owen` 进程**存在**时：`pgrep -x asio_owen` 退出 0 → `||` 短路 → `head -1` 被跳过 → `target_pid` 拿到 `pgrep` 的**全部输出**（可能多行）。
- `asio_owen` 进程**不存在**时：才走 `pgrep -x server | head -1`。

`asio_owen` 是单进程服务，实际运行通常只匹配一行，**无现实影响**；但若同名进程多于一个，`pidstat -p "$target_pid"` 收到多行 PID 会报错。属低危瑕疵。

**建议修法**：
```bash
local target_pid=$(pgrep -x asio_owen 2>/dev/null | head -1)
[ -z "$target_pid" ] && target_pid=$(pgrep -x server 2>/dev/null | head -1)
```

### ⚪ P4 — 非阻塞观察项（均非回归，记录备查）

**O-1. `reload_service`：config.d 持续不可读时每 tick 打 `LOG_ERROR`**

config.d 目录被删除期间，`current_fingerprint` 恒为 nullopt → `config_changed` 恒 true → 每 tick 重试 `Config::load()` → 每次打 `LOG_ERROR "Config directory not found"`（`config.hpp:48`）。

修改前同样每 tick load 并打该 error，**不算回归**。但既然已加 fingerprint 短路，可顺手优化：当 `current` 与 `last` 同为 nullopt 时判为"未变更（仍不可读）"，跳过重试，消除异常运维场景下的日志噪音。属可选改进，非必须。

**O-2. `read_config_fingerprint()` 的 stat 仍在 io_context 线程同步执行**

`TROUBLESHOOTING_LATENCY_SPIKES.md` §2.3 建议"把 `stat()` 挪到 thread pool"。本修复未做该迁移，但相比修改前（`Config::load` 在 io_context 线程**读全部文件内容 + getline parse**），现在只做几次 `file_size`/`last_write_time`（本质 stat）、**不读文件内容**，开销从 O(文件数 × 文件大小) 降到 O(文件数)。对当前只有少量 ini 文件的部署，这通常是合理工程取舍；但本审查未测量文件系统调用耗时，不能把它断言为可忽略。若延迟尖刺仍与 reload tick 对齐，或 config.d 文件数显著增长，应先采样 `stat()` 延迟，再决定是否迁移到 worker pool。

**O-3. bench 脚本依赖 Linux 专属工具（`pidstat`/`nproc`）**

macOS 上两者均不存在：
- `nproc` 缺失 → `ncpu` 为空 → awk 中 `int($8/ncpu)` 除以空字符串 → `cpu_per_core` 计算异常。
- `pidstat` 缺失 → 监控子 shell 静默失败，`MONITOR_LOG` 为空，`analyze_monitor` 无输出。

脚本定位明确是 Linux 压测机（troubleshooting 文档亦注明"验证环境：Ubuntu 22.04"），**设计如此非缺陷**。但可在脚本头加工具存在性检查，避免在 macOS 上误跑时静默无监控。建议：
```bash
command -v pidstat >/dev/null || { echo "⚠️ pidstat 未安装(需 sysstat)，监控禁用" >&2; }
```

**O-4. fingerprint 用 mtime+size，挡不住"同 mtime 同 size 的内容替换"**

triage 报告 §问题#1 末尾已自注此项为"非 bug"。对常规运维足够；极端场景（内容变但 mtime/size 恰好不变）才需要升级为内容 hash。当前无需处理。

---

## 5. 文档审查

两篇新增文档质量都很高。

**`PERF_ISSUES_TRIAGE_2026-07-21.md`** — ✅
- 根因定位到具体代码行，含修复前后对比。
- 修复记录表清晰，标注"双 agent 复核"。
- 运行时验证分两层：压测基线 + 8 分钟稳态日志；并补了 2026-07-22 的高水位回收测试（此前唯一缺口）。
- 结论克制严谨：明确区分"消除可复现病态尖刺（max 270→63ms）" vs "环境级偶发长尾（health 164ms）"，不把后者归给本次 fix；明确"吞吐噪声大，不下吞吐结论"。这种不夸大、不乱归因的写法值得保持。

**`TROUBLESHOOTING_LATENCY_SPIKES.md`** — ✅
- 排查流程图清晰，"环境抖动 / 冷启动 / 周期性阻塞 / 资源饱和"四分类实用。
- §7"何时需要修代码"的 4 条判定标准（warm 后仍复现 + 时间戳与定时任务对齐 + 系统监控正常 + 超 SLA）非常务实，能避免误改代码。
- standalone ASIO 示例已与项目对齐，使用 `asio::post` / `std::error_code`。

---

## 6. 结论与建议

### 整体结论

- **三处核心修复（reload fingerprint / mysql min_size / HttpPool evict 接线）逻辑全部正确**，回查被调用方源码后无悬空假设。
- **回归测试断言正确**（`evict_stale()` 返回"剩余 idle 数"而非"回收数"，断言 `0u` 成立）。
- **未发现 P0/P1/P2 级别问题**。
- 唯一实际瑕疵是 bench 脚本的 `pgrep` 优先级（P3，低危，单进程下无现实影响）。

### 建议的提交动作

1. **可直接提交**：`src/` 下五处改动 + `tests/test_http_pool.cpp`（核心修复 + 回归测试，已验证）。
2. **建议提交前修掉**：`bench/bench_full.sh:50` 的 `pgrep` 优先级（一行改动，见 §4 修法）。
3. **可选改进**（不阻塞提交）：O-1（reload 日志噪音）、O-3（bench 工具检查）。
4. **文档**：两篇均可随代码一并提交。

### 提交信息建议（参考 `~/.claude/rules/git-workflow.md`）

可拆为两个 commit，或合并为一个 `perf:`：

```
perf(app,db,http): eliminate periodic hot-path stalls and idle-conn leaks

- reload_service: fingerprint(name+size+mtime) short-circuit; skip
  Config::load + SecurityRules/Upstream reload when config.d unchanged,
  removing the 30s rules_mu_ write-lock stall on the request hot path
- mysql_pool: guard do_maintain Phase 1 with total_ > min_size so the
  warm min_size connections are no longer recycled/rebuilt each cycle
- upstream_manager + pool_stats_service: wire HttpPool::evict_stale()
  to the stats tick so idle upstream conns are reclaimed without
  relying on后续 acquire traffic (fixes post-peak fd accumulation)
- tests: add HttpPool.EvictStaleReclaimsIdleWithoutAcquire
- bench: per-process system monitor (pidstat) + anomaly detection
```

---

---

## 7. 评分对齐（与 `DESIGN_ASSESSMENT_CLAUDE_2026-07-19.md` 基准）

> 本节把本轮未提交改动放进项目既有的打分体系，按相同维度/权重重算，给出"这轮改动让整体水位动了多少"的可追溯结论。方法论严格沿用 07-19 评估报告（6 维度 × 10 分制 × 固定权重），不另立标准。

### 7.1 打分基准溯源 —— 为什么对齐到 07-19 的 7.5，而不是 07-12 的 9.4

仓库 `docs/` 下共四份带评级/打分的报告，演进如下：

| 报告 | 日期 | 总分/结论 | 角色 |
|------|------|----------|------|
| `CODE_REVIEW_2026-07-12.md` | 07-12 | 综合 **8.9 → 9.4/10**（9 维度） | **已被超越**：未检出 security 层两个真 P0 |
| `CODE_ANALYSIS_CLAUDE_2026-07-13.md` | 07-13 | 无总分；列 **2 P0 + 3 P1** | 全模块审计，推翻 07-12 的"安全 9.8" |
| `CODE_ANALYSIS_CLAUDE_2026-07-18.md` | 07-18 | 无总分；确认 P0/P1 全修 + 新发现 10 个 P2 | 增量审计 |
| `DESIGN_ASSESSMENT_CLAUDE_2026-07-19.md` | 07-19 | 综合 **7.5/10**（6 维度加权） | **当前基准** |

**以 07-19 为对齐基准的理由**：
1. **方法论最严**：6 维度 + 显式权重 + 档位锚点（参照 Folly/Proxygen=9.0~9.5、一线大厂线上服务=8.0~8.5），不像 07-12 的 9 维度自带"自我抬分"倾向（如"连接复用 9.5""内存安全 9.5"这类强项重复计权）。
2. **时间点最新且已含 P0/P1 修复**：07-19 评估时（commit `4704813`），07-13 的 2 P0 + 3 P1 已全部修复，基准是"清完高危后的水位"，本轮改动是它的纯增量。
3. **07-12 的 9.4 被 07-13 证伪**：07-12 给"安全防护 9.8"，但 07-13 发现 JWT fail-open（全路由无认证）和 redis `cmd()` format-string 注入两个 P0——这两个是 07-12 完全漏掉的。以一个漏了 P0 的打分为基准没有意义。

> 注：07-19 报告本身基于同日的 `SMART_PTR_COROUTINE_REVIEW_CLAUDE_2026-07-19.md`（条目编号 P1-1/P1-2/APP-P2-x/SEC-P2-x/HTTP-P3-x 等均出自该 review）。本节引用这些编号时即指该来源。

### 7.2 三处修复 → 维度映射（逐项论证）

| 修复 | 主影响维度 | 次影响维度 | 性质判定 |
|------|-----------|-----------|---------|
| #1 reload fingerprint 短路 | 并发正确性、性能工程 | — | **频率缓解**，非机制修复 |
| #2 mysql min_size 保护 | 性能工程 | 内存安全(边缘) | 资源生命周期优化 |
| #3 HttpPool evict_stale 接线 | 工程纪律、性能工程 | 并发正确性(锁层级验证) | dead code 转正 + 回归测试 |

**#1 的性质判定依据**：07-13 §问题#1 修复方向列了两条——方向1（mtime/hash 短路）、方向2（`SecurityRules` 改 `shared_mutex` 或 shared_ptr 换指针，让 reload 不 stall 热路径）。本轮只做了方向1。也就是说 **`rules_mu_` 仍是普通 `mutex`，`check()` 仍是"持锁拷贝整份快照"**（`security_rules.hpp:147` 附近）；只是 reload 触发频率从"每 30s 必然"降到"仅配置变更时"。配置真变更时，那次 reload 仍会 stall 所有请求线程。所以是"把 stall 从周期性变成偶发性"，不是"消除 stall 机制"。→ 并发维度只给 +0.1，不给 +0.5。

**#3 的性质判定依据**：`HttpPool::evict_stale()`（`http_pool.hpp:382`）在本轮前是死代码（全项目零调用，07-21 triage §问题#3 指出）。本轮 `UpstreamManager::evict_stale()`（`upstream_manager.hpp:106`）+ `PoolStatsService` tick（`pool_stats_service.hpp:29`）将其转正。这**直击 07-19 §3.6 工程纪律维度的系统性扣分项"dead code 残留 / 代码卫生"**——但需注意：07-19 §3.6 明确列的 dead code 是 `release/release_bad`（HTTP-P2-2）、成员版 `track_active/untrack_active`（HTTP-P3-2）、`RedisReplyGuard` move 语义（DB-P3-2）、`acquire_worker`（DB-P3-1），**这些本轮没清**；本轮清的是另一处（evict_stale），属于同一类问题的一次清理，不是清完 07-19 的清单。→ 工程纪律给 +0.2 而非更高。

### 7.3 更新后 6 维度评分表（含加权计算）

| 维度 | 权重 | 07-19 基线 | 当前 HEAD | 变动 | 加权(当前) |
|------|------|-----------|------------|------|-----------|
| 架构设计 | 20% | 8.0 | **8.0** | — | 1.600 |
| 内存安全 / RAII | 20% | 7.5 | **8.0** | +0.5 | 1.600 |
| 并发正确性 | 15% | 7.5 | **7.6** | +0.1 | 1.140 |
| 异常安全 / 健壮性 | 15% | 6.5 | **6.7** | +0.2 | 1.005 |
| 性能工程 | 15% | 8.5 | **8.6** | +0.1 | 1.290 |
| 工程纪律 | 15% | 7.0 | **7.2** | +0.2 | 1.080 |
| **合计** | 100% | **7.540 ≈ 7.5** | **7.715 ≈ 7.7** | **+0.175（展示分 +0.2）** | — |

加权计算过程（透明可复算）：
```
8.0×0.20 + 8.0×0.20 + 7.6×0.15 + 6.7×0.15 + 8.6×0.15 + 7.2×0.15
= 1.600 + 1.600 + 1.140 + 1.005 + 1.290 + 1.080
= 7.715
```

### 7.4 分维度详评（对标 07-19 §3 格式：撑分 / 扣分 / 动作）

#### 7.4.1 架构设计 —— 8.0 → **8.0**（不变）

**为什么不变**：本轮三处修复都是"既有架构内的实现优化"，未触碰任何架构决策：
- 未改池策略（MysqlPool 仍 thread_pool offload、Redis 仍 worker/direct 双模式、HttpPool 仍 16-shard）。
- 未改并发模型（未引入 strand、未把 `rules_mu_` 升级为 `shared_mutex`、未改 shared_ptr 代际续命设计）。
- 未改生命周期模型（`Application` 仍持有所有子系统、`cleanup()` 顺序未动）。

fingerprint 短路（`reload_service.hpp:69` `read_config_fingerprint`）是"配置变更检测策略"的实现改进，不构成架构变更。→ 维持 8.0。

**07-19 列的架构扣分项（关停 fragile / 无统一 strand / `State::~State` 与 `shutdown` 职责重叠）本轮均未触及**，故无加分空间。

#### 7.4.2 内存安全 / RAII —— 7.5 → **8.0**（+0.5）

**已完成的关键修复（`d948077`，在本轮性能提交之前）**：
- P1-1 已修：`ConnGuard::~ConnGuard()` 明确 `noexcept`；`HttpPool::release/release_bad` 同样 `noexcept`，`idle.push_back` 的 OOM 会关闭连接并回滚计数，而非从析构函数传播异常导致 `terminate`。
- P1-2 已修：`HttpPool::acquire()` 把 `make_unique<HttpConn>` 放入回滚保护的 `try` 内；分配或 `active.insert` 抛异常时，`total_count` 和 `in_flight_count` 都会撤销预留。

**仍扣分**：`jwt_auth` 的 `EVP_MD_CTX`/`BIO` 尚未 RAII 化，`security_rules` 仍有依赖生命周期不变量的裸指针。两项 P1 消除后，RAII 主干可从 7.5 提至 8.0；本轮 #2/#3 没有引入新的资源所有权风险。

#### 7.4.3 并发正确性 —— 7.5 → **7.6**（+0.1）

**撑分（本轮新增）**：
- **reload 写锁持有频率大幅下降**：修复前 `ReloadService::schedule` 每 30s 无条件进入 `security_rules_.reload()` 持 `rules_mu_` 写锁跑完整个 `load_from_config`（含 JWT 构建、rate limiter reload）；修复后（`reload_service.hpp:46` `config_changed` 判定）配置不变时整块跳过，`rules_mu_` 在稳态下不再被写锁持有。这与请求热路径 `SecurityRules::check()` 持同一把锁做快照（`security_rules.hpp:147`）形成直接缓解——周期性 stall 消除。
- **锁层级仍清晰**：新增的 `UpstreamManager::evict_stale` 取 `shared_lock`，与 `reload` 的 `unique_lock` 互斥、与 `pool_stats`/`route` 的 `shared_lock` 并存，单向锁序，无死锁（本 review §2.3 已验证）。

**为什么只 +0.1 而非更高**：
- **锁机制本身未改**。`rules_mu_` 仍是 `std::mutex`（非 `shared_mutex`），`check()` 仍是"持锁拷整份 `trusted_proxies_`/`jwt_auth_`/`rate_limiter_` 快照"。配置真变更时那一次 reload 仍 stall 全部请求线程。07-13 方向2（读写锁或 shared_ptr 换指针）未做。→ 是"stall 频率从周期性降为偶发性"，不是"消除 stall 可能性"。
- **07-19 列的并发扣分项（APP-P2-2 acceptor 跨线程 cancel UB、SEC-P2-1 reload 非原子半更新、SEC-P2-2 case_sensitive 归一化窗口、HTTP-P2-3 ConnGuard 冗余 pool_holder）本轮均未触及**。

综合：+0.1，到 7.6。

#### 7.4.4 异常安全 / 健壮性 —— 6.5 → **6.7**（+0.2，仍是最弱项）

**改善来源**：上节两项 P1 修复同时消除了异常安全问题：析构路径不再因连接池扩容 OOM 而 `terminate`，`HttpConn` 分配失败也不会永久泄漏池计数。#1/#2/#3 本身未扩大异常边界。

**仍存的异常安全扣分项**：
- 07-18 P2 #4：`upstream_manager.hpp:114` `add_upstream_locked` `make_shared` 抛异常留两 map 不一致 → `route().at()` 抛 `out_of_range` → 500，且不可自愈。

**后续已完成（不重新估算本节历史分数）**：`RateLimiter::persist_snapshot()` 已用 RAII 恢复 busy 标志并检查 write/flush/close；accept loop 的 detached 协程已有异常完成处理器。剩余分数上限主要受 upstream reload 异常原子性和更广泛的关停路径审计限制。

#### 7.4.5 性能工程 —— 8.5 → **8.6**（+0.1，最强项继续加固）

**撑分（本轮新增，有运行时记录佐证）**：
- **消除 `rules_mu_` 周期性写锁 stall**（#1）：MySQL 压测的可复现病态尖刺 `max=270.58ms / stdev=12.11ms` → 修复后两轮收敛到 `max=63.78ms / 73.40ms`、`stdev≈1.5/1.9ms`（triage 报告附录 A）。吞吐持平，符合"消 stall 不增吞吐"预期。
- **消除暖连接 churn**（#2）：稳态 `total=8, idle=8` 长期静止，无修复前每 30s 的 `recycled→added` 空转（triage 报告附录 B）。
- **HttpPool idle 不再依赖流量回收**（#3）：停流量后 `zebra-config` 从 `total=93, idle=93` 在 ≥`idle_timeout_sec` 内回落至 `total=0`（2026-07-22 补测）；fd 不再累积。

**评分校准**：07-19 评估未覆盖这三处问题，它的 8.5 不应被事后反推为 8.2-8.3；那会把未知缺陷的假设混入增量评分。本轮代码和运行日志表明三个确定的资源/周期性工作问题已得到缓解，因此给 +0.1。由于本审查未独立复跑压测，且现有数据只有两轮短压测、没有 p95/p99 分位数或受控环境的重复样本，不把一次 `max`/stdev 收敛当作足以支撑 +0.2 的定量性能结论。

**07-19 §3.5 列的性能扣分项（仍存）**：
- `trusted_proxies_` 每请求全量 `std::vector<std::string>` 拷贝（`security_rules.hpp:147` 的快照拷贝，与 #1 同源，但 #1 只减了写侧频率，没改读侧的 per-request copy）。

**后续已完成（不将软 deadline 误记为查询取消）**：logger 所有级别入口均在格式化前检查 `should_log()`；`/api/combo` 的 MySQL fallback 有 500ms soft deadline 和最多 8 个 in-flight permit。deadline 返回 504，但不取消同步 MySQL；worker completion 才释放 permit。

→ 性能工程 8.5 → 8.6。要再往上必须修上面三个扣分项，并用受控、多轮基准确认收益。

#### 7.4.6 工程纪律 —— 7.0 → **7.2**（+0.2）

**撑分（本轮新增）**：
- **dead code 转正**：`HttpPool::evict_stale()` 从全项目零调用 → 由 `PoolStatsService` tick 驱动，直击 07-19 §3.6 扣分项"dead code 残留 / 缺乏定期清理"这一**系统性问题**的一次清理。
- **回归测试**：新增 `HttpPool.EvictStaleReclaimsIdleWithoutAcquire`（`tests/test_http_pool.cpp:308`），白盒注入 stale idle 验证无 acquire 路径的回收。
- **文档质量**：`PERF_ISSUES_TRIAGE_2026-07-21.md` 根因定位到行 + 双 agent 压测验证 + 2026-07-22 高水位回收补测；`TROUBLESHOOTING_LATENCY_SPIKES.md` 给出"何时需要修代码"的 4 条判定标准。文档意识强（07-19 已点名这是项目优点之一）。

**为什么只 +0.2**：
- HTTP-P2-2 `release/release_bad` 与 HTTP-P3-2 成员版 `track_active/untrack_active` 已删除；`RedisPool::acquire_worker` 经回查仍是 worker 模式入口，不是 dead code。`RedisReplyGuard` move 语义仍可在后续 API 收口时简化。
- 07-19 列的文档漂移中，`gateway-debug-20260703` 调试 marker 已删除；`/api/combo` soft deadline 的语义已在 `CLAUDE.md` 和 `DB_POOL_DESIGN.md` 对齐。其余历史文档仍应随代码演进复核。
- 本轮新增文档已与 standalone ASIO 接口对齐；既有四处文档漂移仍未处理。

**后续已完成（不回填本节历史分数）**：`.github/workflows/sanitizers.yml` 在 push/PR 上运行 ASan 与 TSan，构建并执行不依赖 socket 或外部数据库的测试集。它为并发状态机和内存边界提供持续验证，但尚不能替代完整集成环境的 sanitizer 覆盖。

### 7.5 关键解读 —— 为什么当前 HEAD 为 7.7，而性能提交本身只贡献有限

当前 HEAD 相比 07-19 基线的总提升为 +0.175：其中 `d948077` 已完成的两项 P1 资源/异常安全修复贡献了主要增量，本轮性能提交的直接量化贡献是并发 +0.1、性能 +0.1、工程纪律 +0.2。后者质量很高（逻辑核实、运行记录、回归测试齐全），但在 07-19 的权重模型里位移有限。三个结构性原因，仍印证 07-19 报告 §7 的核心论断（"瓶颈不是能力，是流程"）：

1. **异常安全仍是最弱项（6.7，权重 15%）**。两项 P1 已修，但 `RateLimiter`、detached 协程和关停路径的异常处理仍未系统化；从 6.7 拉到 8.0 仍是最高杠杆。
2. **架构设计（20%）本轮未触及**。本轮是性能与代码卫生优化，不是生命周期或并发模型的架构调整。
3. **性能虽是强项且本轮重点，但权重只有 15%、基数已高（8.5）**，本轮 +0.1 的直接加权贡献仅为 0.015。

**本轮性能改动的真实价值不只在"涨分"，还在**：
- **巩固最强项**（性能工程，消除三个被压测证实的隐患）；
- **顺带还一笔 dead code 债**（evict_stale 转正）；
- **建立可复现的 perf 排查方法论**（两篇文档 + bench 脚本的进程级监控）。

这些都不直接转化为总分，但显著降低了"未来回归"和"排查成本"——属于 07-19 §4"强项总览"里"会从错误中学习""性能过硬"这两条的延续。

### 7.6 提分路径（沿用 07-19 §6，标注本轮后的位置）

| 阶段 | 目标分 | 关键动作 | 对应维度提升 |
|------|--------|---------|-------------|
| 本轮后现状 | **7.7** | 未四舍五入为 7.715 | — |
| → §6.1 | **~7.8** | 已完成 `persist_snapshot()` RAII/完整写入检查；继续清理文档漂移与剩余 API 卫生项 | 异常 6.7→7.0、纪律 7.2→7.5 |
| → §6.2 | **~8.2** | 已完成 acceptor strand、detached 异常兜底、SecurityRules 两阶段 reload、jwt_auth RAII，及 `/api/combo` soft deadline + in-flight permit；剩余工作是补跨层回归测试和审计其他异常边界 | 内存 8.0→8.5、异常 7.0→8.0、并发 7.6→8.0、纪律 7.5→8.0 |
| → §6.3 | **~9.0** | TSan/ASan 进 CI；异常安全全链路审计；显式生命周期管理替代隐式不变量；统一 strand 策略；fuzz 测试；错误处理统一框架；关停流程重写 | 全维度系统性提升 |

**最大杠杆仍是 §6.1/§6.2 的异常安全项**（snapshot RAII、APP-P2-1/3）。异常安全从 6.7 提到 7.0 或 8.0，对综合分的直接贡献分别为 +0.045 和 +0.195；再叠加内存安全、并发和工程纪律的配套改进，路线图才可到约 7.8 / 8.2。它仍明显高于本轮三处性能修复的直接加权贡献。

### 7.7 评分快照（对标 07-19 §8，扩展为本轮增量列）

| 维度 | 07-19 基线 | 本轮(07-26) | 修完 §6.1 | 修完 §6.2 | 冲刺 §6.3 |
|------|-----------|------------|----------|----------|----------|
| 架构设计 | 8.0 | **8.0** | 8.0 | 8.0 | 9.0 |
| 内存安全 / RAII | 7.5 | **8.0** | 8.0 | 8.5 | 9.0 |
| 并发正确性 | 7.5 | **7.6** | 7.6 | 8.0 | 9.0 |
| 异常安全 / 健壮性 | 6.5 | **6.7** | 7.0 | 8.0 | 9.0 |
| 性能工程 | 8.5 | **8.6** | 8.6 | 8.6 | 9.0 |
| 工程纪律 | 7.0 | **7.2** | 7.5 | 8.0 | 9.0 |
| **综合** | **7.5** | **7.7** | **~7.8** | **~8.2** | **~9.0** |

### 7.8 本评分对齐的边界声明（诚实边界）

1. **本轮审查未重新执行压测**。性能工程维度的 +0.1 依据来自 triage 报告中的运行记录（MySQL max 270→63ms、稳态 8/8 无 churn、zebra-config 93→0 回收）；本审查只做了静态核实，未独立复跑。现有记录可证明功能性改善，但不足以单独量化整体尾延迟收益。
2. **维度得分的 ±0.1/0.2 是判断性估值，非精确测量**。07-19 本身也是"判断性打分 + 档位锚点"方法论，本轮沿用同一粒度，不假装比基准更精确。
3. **除已按 `d948077` 复核并上调的 P1-1/P1-2 外，范围外的扣分项（07-18 P2 #1-#10、07-19 APP-P2-x/SEC-P2-x 等）默认仍存**，除非有后续 commit 明确修复。本节对已引用的条目做了当前源码核实；未重新全仓审计的条目按"未改动"处理。
4. **若后续把方向2（`SecurityRules` 上 `shared_mutex` 或 shared_ptr 换指针）也做了**，并发正确性可从 7.6 再上 0.2-0.3（机制级修复，非频率缓解），那是独立于本轮的下一步。

---

*审查者：Claude Code / 2026-07-26（静态逐行审查，回查 http_pool/config/upstream_manager 源码核实调用契约）。未重新执行压测；运行时验证以 triage 报告中的历史运行记录为准。评分对齐（§7）以 `docs/DESIGN_ASSESSMENT_CLAUDE_2026-07-19.md` 为基准，方法论沿用其 6 维度加权模型，并纳入当前 HEAD 中 `d948077` 和 `88b0460` 的相关修复。*
