# 配置中心修复提交审查 + 内存泄漏排查 + 评分对齐

> 评估对象:`asio_owen` @ commit `93ead3d`(master,2026-08-19)
> 评估范围:`93ead3d`("fix: close config-center review findings")逐项核对、新增面内存泄漏专项排查、与既有评分体系(07-19 基准 / 07-26 校准)的对齐重评
> 评估依据:静态代码审查(routes.cpp / config_admin.hpp / config_history*.hpp / config_sync_service.hpp / http_protocol.hpp / client_session.hpp / proxy_forwarder.hpp / application.cpp / redis_pool.hpp / security_rules.hpp)、`docs/PHASE4_PERF_VALIDATION_2026-08-18.md` 运行验收记录、历史评审文档
> 评估者:Claude Code

---

## 1. 总评

| | |
|---|---|
| **审查结论** | `93ead3d` 提交信息声明的 7 项修复**全部落地且实现一致**,未发现阻塞问题 |
| **内存泄漏** | **未发现无界泄漏**;新增自保活状态机完成路径完备(详见 §3) |
| **综合评分** | **8.0 / 10**(07-26 的 7.7 → 8.0,进入"优秀生产级"区间下沿) |
| **最弱项** | 异常安全 / 健壮性(7.2)——新子系统缺全链路异常/关停审计 |

---

## 2. 提交审查:声明 vs 落地核对

28 文件,+1247/-239。逐项核对结果:

| 声明 | 核对结果 | 关键证据 |
|---|---|---|
| 移除默认管理员 + 交互式哈希脚本 | ✅ | `hash_admin_password.py` 改 getpass 交互、强制 ≥100k 迭代、双次确认;`app_config.hpp:90` 忽略非 `pbkdf2_sha256$` 前缀的 `[admin]` 条目,默认 admin/admin 路径堵死 |
| token auth-version 吊销 | ✅ | `config_admin.hpp:888` `admin_auth_version()` = SHA256(username + "\n" + password_hash) 作 `av` 声明签入 JWT;`verify_admin_token` 比对当前配置的 `av`,不匹配即拒——改密即时吊销全部旧 token |
| PBKDF2/RSA/dry-run/file-sync 移出 io_context | ✅ | `Application` 新增 `admin_auth_workers_`(2 线程)与 `config_file_workers_`(1 线程);login 改协程 `co_await asio::post` 切换并加 16 并发上限(溢出 429 + Retry-After);`AdminRequestOperation::start()` 与 `AdminHistoryOperation::start()` 整体在 auth 池执行;`cleanup()` 先 join 再 reset,顺序显式 |
| 空白填充头名拒绝(TE 走私) | ✅ | `http_protocol.hpp:118` `trim_view(k).size() != k.size()` 置 `invalid_header_name`;`client_session.hpp:154` 连接层拒绝并记录日志字段;`proxy_forwarder.hpp` 转发前统一 trim 头名再匹配,消除 `"Transfer-Encoding "` 类绕过 |
| 健康检查串行化 + Lua 原子校验 | ✅ | `ConfigHistoryService` pending 队列 + 单飞(in_flight)+ 完成后补发;`inconsistent_` 初始为 `true`(fail-closed,未跑过检查不报健康);save/rollback 脚本 KEYS 8→9/9→10,新增当前版本三元组(zset member + snapshot hash + snapshot key)存在性校验,与 `EXISTS KEYS[9]/[10]` 用法核对一致 |
| 心跳 TTL / 状态文件抑制 / CSP+no-store | ✅ | Lua 健康检查内 HGETALL + 按 `machine_ttl_sec`(默认 3600,下限 60)HDEL 过期机器;状态文件内容不变则跳过写(读回比对);`set_json` 统一补 `Cache-Control: no-store` |
| settings 页修复 | ✅ | 回滚后刷新、唯一文件名、分页 guard(HTML 变更未逐行深审,以配套测试为准) |

### 2.1 非阻塞观察(两项)

1. **登录限流维度收窄**:`AdminLoginThrottle` 从 (ip, username) 二元组改为纯 IP(`routes.cpp:115-135`)。防跨用户名枚举爆破更有效,但单 IP 触发锁定(5 次失败锁 15 分钟,`kMaxEntries=4096` 上限)会连带封锁该 IP 全部用户名。属合理取舍,记录在案。
2. **`verify_admin_token` 双重解码**:为取 `av` 声明对 token 做了第二次 `jwt::decode`(库内部 verify 已解过一次)。轻微冗余,非正确性问题。

### 2.2 顺带修复(提交信息未列)

- `HGETALL` reply.ok 检查:Redis 出错现在正确返回 500(此前可能误判 409)(`routes.cpp:476-481`)。
- 日志注入防护:异常 reason 经 `sanitize_body_preview` 后再入日志(`routes.cpp:1541-1544`)。
- `insecure_no_auth` 模式首次命中打 `LOG_ERROR` 安全警告(一次性,原子去重)。

---

## 3. 内存泄漏专项排查

### 3.1 排查对象与方法

07-26 后新增 4 个 `enable_shared_from_this` 自保活状态机(`AdminRequestOperation`、`AdminHistoryOperation`、`ConfigHistoryService`、`ConfigSyncService`)是最高可疑面——此类对象若回调链断裂,shared_ptr 自引用永不释放,按请求累积。逐条审查完成路径:

### 3.2 结论:无无界泄漏

**恰好一次完成**:`complete()` 以 `completed_.exchange(true, acq_rel)` 原子去重(`routes.cpp:850`),`fail()` 最终收敛到 `complete()`(`routes.cpp:847`),双触发被吞。

**回调链必达**:
- 每条状态转移回调均以 `complete()` / `fail()` / 链下一状态收尾(逐分支核对 read_config_version → read_config_files → verify_hash → save/conflict 全链,及 machines/history 各链);
- `run_command` 的 co_spawn 完成器:`ep` 转错误 reply、回调经 auth 池 `asio::post` 派发、post 抛异常就地降级同步 `invoke()`——无断链路径(`routes.cpp:781-807`)。

**无引用环**:持有链为 operation → completion → handler → 协程帧;协程帧不反向持有 operation,`ctx_` 为裸引用不持所有权。completion 触发后协程恢复、写响应、帧析构,最后一个 `self` 随回调 lambda 结束释放。

**Redis 挂死不悬挂回调链**:池化连接创建即带 `cmd_timeout_ms`(默认 30s 安全超时,`redis_pool.hpp:283,647`)。挂死的 Redis 在 ≤30s 内变为错误 reply → 500 + complete。最坏滞留 = 超时 × 并发数,**有界,不累积**。

**增长型容器均有界**:
- `AdminLoginThrottle.entries_`:`kMaxEntries=4096` 超限触发过期清扫(`routes.cpp:164`);
- `pending_health_checks_`:每次 launch 时 swap 清空;
- Redis 侧:历史快照 GC 批处理(`gc_batch_size=20`)+ 机器心跳 TTL——后者正是本提交修掉的唯一真"只增不减"点(修复前 stale 机器条目永久留在 hash)。

### 3.3 实证旁证

- 2026-07-12:Valgrind definitely lost = 0,LSAN 无报告,四轮压测 RSS 恒定 75MB(`docs/BENCH_STEPS.md` §287-289);
- 2026-08-18 Phase4(含配置历史子系统运行中):Release RSS 预热后稳定,`ctest` 291/291(`docs/PHASE4_PERF_VALIDATION_2026-08-18.md`)。

### 3.4 边界与建议

1. **新子系统无专项 sanitizer 长跑记录**:CI sanitizers 跑非 socket 测试集,配置中心全链路未做过 07-12 式 12min LSAN 长压。建议目标机补一轮 `ASAN_OPTIONS=detect_leaks=1` 的 `./bench.sh`。
2. **Direct 模式 Redis TLS 连接** shutdown 时随 io 线程生命周期释放——文档化已知局限(有界),当前配置 worker 模式不触发。
3. 进程退出时刻可能有极少量在途 operation 随 io_context 停止滞留——进程消亡即释放,无实际影响。

---

## 4. 评分对齐(与 07-19 基准 / 07-26 校准)

### 4.1 沿革与基准选择

| 报告 | 总分 | 角色 |
|---|---|---|
| `CODE_REVIEW_2026-07-12.md` | 8.9 → 9.4 | 已被超越(漏 2 个真 P0) |
| `DESIGN_ASSESSMENT_CLAUDE_2026-07-19.md` | 7.5 | 方法论基准(6 维度加权 + 档位锚点) |
| `PENDING_CHANGES_REVIEW_2026-07-26.md` §7 | 7.7 | 上一校准点 |
| 本文 | **8.0** | 当前 |

### 4.2 07-26 之后的实质变化

1. **方向 2 落地**:`SecurityRules` 改 `shared_ptr<const SecuritySnapshot>` 换指针发布 + generation 计数 + TLS 缓存(`security_rules.hpp:500-516`),每请求持锁拷整份快照的热路径问题**机制性消除**——07-26 §7.8.4 明示此项值 +0.2~0.3;
2. **配置中心整套新子系统上线**(Redis 单一事实源 + 版本化历史 + Lua 原子校验 + 心跳 TTL + 管理页),`93ead3d` 关闭一批安全评审发现;
3. **07-19 §6.2 清单项大体完成**(acceptor strand、detached 兜底、jwt_auth RAII、combo soft deadline——07-26 §7.6 已标注);
4. **测试/CI**:28 个测试文件、321 个 TEST 宏(07-12 时代约 145);sanitizers CI 在位;
5. **Phase4 性能验收**:无回归,且文档自认"非严格 A/B、iowait 50%+"——方法论诚实。

### 4.3 更新后评分表

| 维度 | 权重 | 07-26 | 当前 | 变动 | 依据摘要 |
|---|---|---|---|---|---|
| 架构设计 | 20% | 8.0 | **8.2** | +0.2 | 快照换指针为机制级升级;新子系统沿用既有 offload 模式、三份设计文档齐全;扣分:回调状态机复杂度上升、新面未经 07-13 级同等审计 |
| 内存安全 / RAII | 20% | 8.0 | **8.2** | +0.2 | jwt_auth RAII 已闭;新代码 shared_ptr 自保活模式规范(§3 排查);未见新的裸所有权 |
| 并发正确性 | 15% | 7.6 | **8.0** | +0.4 | 方向 2 + 健康检查串行化 + 单飞 + CAS 限流;扣分:io_context/auth 池/redis 池三方跳转编排未经专项并发审计 |
| 异常安全 / 健壮性 | 15% | 6.7 | **7.2** | +0.5 | fail-closed 姿态(`inconsistent_` 初始 true、未配置→503、Redis 错→500)、恰好一次完成、关停顺序显式;仍是**最弱项**:新子系统无全链路异常审计 |
| 性能工程 | 15% | 8.6 | **8.7** | +0.1 | TLS 缓存快照是热路径真收益;PBKDF2/RSA 出 io_context 带 16 permit 上限;Phase4 验收但非 A/B |
| 工程纪律 | 15% | 7.2 | **7.6** | +0.4 | 测试随提交同步增长、sanitizers CI、文档同步更新;扣分:连续三次"页面配置"零信息量提交信息、配置中心评审无入库审计文档(本文件补此缺口) |
| **合计** | 100% | **7.7** | **8.0** | +0.3 | 计算见下 |

加权计算(可复算):
```
8.2×0.20 + 8.2×0.20 + 8.0×0.15 + 7.2×0.15 + 8.7×0.15 + 7.6×0.15
= 1.640 + 1.640 + 1.200 + 1.080 + 1.305 + 1.140
= 8.005 ≈ 8.0
```

### 4.4 解读

- 07-26 路线图预测"修完 §6.2 ≈ 8.2";当前 8.0 略低于该值是**合理的**:§6.2 确大体完成,但配置中心新增数千行未经 07-13/07-18 式全量审计的面,按"未知不加分"原则压住上限。
- 进入 07-19 档位锚点的"优秀生产级"下沿(8.0-8.5 = 一线大厂核心线上服务)。
- **最大杠杆仍是异常安全(7.2)**:对配置中心做一轮全链路异常/关停审计 + 跨层回归测试,8.3-8.5 可期;若再补新子系统 sanitizer 长压与并发专项审计,可稳固 8.5。

### 4.5 评分快照(对标 07-19 §8 / 07-26 §7.7)

| 维度 | 07-19 | 07-26 | 当前(08-19) | 下一档动作 |
|---|---|---|---|---|
| 架构设计 | 8.0 | 8.0 | **8.2** | 新子系统架构专项评审 |
| 内存安全 / RAII | 7.5 | 8.0 | **8.2** | 新子系统 ASan/LSan 长压 |
| 并发正确性 | 7.5 | 7.6 | **8.0** | 三方跳转编排并发审计 |
| 异常安全 / 健壮性 | 6.5 | 6.7 | **7.2** | 全链路异常/关停审计 + 跨层回归测试 |
| 性能工程 | 8.5 | 8.6 | **8.7** | 受控 A/B 基准(低 iowait 环境) |
| 工程纪律 | 7.0 | 7.2 | **7.6** | 提交信息规范、评审文档随批入库 |
| **综合** | **7.5** | **7.7** | **8.0** | — |

---

## 5. 后续建议(按 ROI 排序)

1. **异常安全审计**(最高杠杆,维度 7.2):配置中心状态机 + ConfigSync 文件 I/O + 双新线程池关停路径的全链路异常边界审计,补跨层回归测试。
2. **新子系统 sanitizer 长压**:目标机 `ASAN_OPTIONS=detect_leaks=1` 跑 `./bench.sh` 全量 + `config` 附加轮,补齐 Phase4 未覆盖的泄漏实证。
3. **并发专项审计**:io_context ↔ admin_auth_workers ↔ redis worker 池的跳转/派发逻辑做 07-13 式逐函数过审(本文只做了完成路径通读)。
4. **工程纪律**:提交信息避免再次出现"页面配置"式零信息量;评审结论随批入库(本文即补 93ead3d 的审计缺口)。
5. 工作区遗留:未提交的 `config.ini` 删除与未跟踪 `.zed/` 需明确处置。

---

## 6. 边界声明(诚实边界)

1. **本机未能构建复跑**:评估机无 mysql-client/hiredis(仅 openssl),旧 build 缓存中 MySQL 亦为 NOTFOUND——非本提交回归。测试结论采信 Phase4 文档(目标机 291/291)。
2. **±0.1-0.2 为判断性估值**:沿用基准方法论粒度,不假装更精确。
3. **审查深度不等价历史专项审计**:对新代码做了一轮完成路径通读与关键机制核对,非 07-13/07-18 式逐函数全量审计;范围外历史扣分项按"未见修复即默认仍存"处理。
4. **settings.html 前端变更未逐行深审**,以配套测试与功能描述为准。

---

*审查者:Claude Code / 2026-08-19(静态审查,回查 routes/config_admin/config_history*/config_sync/http_protocol/client_session/proxy_forwarder/application/redis_pool/security_rules 源码核实;未重新执行压测与测试,运行时结论采信 Phase4 验收记录)。评分对齐以 `docs/DESIGN_ASSESSMENT_CLAUDE_2026-07-19.md` 为方法论基准、`docs/PENDING_CHANGES_REVIEW_2026-07-26.md` §7 为上一校准点。*
