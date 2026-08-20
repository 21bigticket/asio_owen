# asio_owen 评分到 9.0 的验收路线图

**日期**：2026-08-20  
**评估对象**：当前工作区及最近一次 Ubuntu/GCC 11 部署构建  
**当前评分**：约 **8.4 / 10**  
**目标评分**：**9.0 / 10**  
**评分口径**：架构、内存安全、并发正确性、异常安全、性能、工程纪律六维度综合评估

## 1. 当前结论

项目已经过了“核心功能是否可用”的阶段。最近一次 Ubuntu/GCC 11 构建结果为：

- 构建成功，退出码为 0。
- `ctest` 通过 `333/333`，其中 `ConfigLoad.RejectsUnreadableFileInsteadOfSilentlySkipping` 因权限条件跳过。
- `/api/health`、`/api/mysql`、`/api/redis`、Gateway 冒烟检查均返回 200。
- 关停生命周期相关的新增测试已经纳入 CTest；本地本轮 warning 清理后的定向测试为 `66/66`。

当前扣分主要不是已知 P1 缺陷，而是以下证据和工程闭环还没有完成：

1. 部分 CI 尚未证明在真实仓库分支上触发并全绿。
2. 关停策略已经选择“全部存量 socket 确定性切断”，但部署文档仍保留“5 秒连接排空”的旧语义。
3. Sanitizer、clang-tidy、fuzz、Redis 故障矩阵和受控 A/B 尚未形成可归档的完整证据链。
4. 工作区改动尚未按职责拆成可回滚、可 bisect 的提交。
5. `Application` 旧资源字段和大型实现 header 仍有架构收敛空间。

### 1.1 最新验证记录：2026-08-20 06:34 UTC

`rebuild_deploy.sh` 已完成一次新的 Ubuntu/GCC 11 候选构建和部署：

- Release 构建成功，server 和全部测试目标链接成功。
- CTest `333/333` 通过；`ConfigLoad.RejectsUnreadableFileInsteadOfSilentlySkipping` 仍因权限条件跳过。
- `/api/health`、`/api/mysql`、`/api/redis`、Gateway 均返回 200。
- `segfault: 0`，候选二进制已切换并重启成功。
- 本轮输出中已不再出现此前的 `AppServices` missing-field-initializers、未使用 `checksum` 和未使用 client-session 辅助函数 warning。

这次结果确认了 GCC 11 兼容性、普通构建和部署冒烟链路；它不会替代 ASan、TSan、clang-tidy、fuzz、Redis 应用故障矩阵和受控 A/B 的独立证据。

## 2. 到 9.0 的硬门槛

以下项目全部完成，才建议把评分写成 9.0，而不是仅凭单次全量 CTest 上调：

| 类别 | 验收标准 | 证据 |
|---|---|---|
| CI | GCC 11、ASan、TSan、clang-tidy、fuzz 均在实际分支触发并通过 | CI run URL、完整日志、测试摘要 |
| 测试 | 普通构建全量 CTest 通过；生命周期和关停新增测试稳定通过 | CTest 原始输出 |
| 关停 | 明确 hard-stop 或 graceful-drain 语义；空闲连接、在途请求、超时和幂等均有测试 | 设计文档 + 回归测试 |
| 故障 | Redis wrong-type、nil、CAS、重启场景覆盖真实应用调用路径 | 集成测试日志 |
| 性能 | baseline/candidate 轮级交替，至少 3 轮，记录 p95/p99、RPS、CPU、RSS、FD 和错误率 | A/B 原始日志与汇总 |
| 工程 | 改动按功能拆分提交，`git diff --check`、构建和测试命令可复现 | 提交历史和验证记录 |

## 3. P0：先修工程闭环

### 3.1 修正 clang-tidy 触发分支

`.github/workflows/clang-tidy.yml` 当前 push 触发分支为 `main`，仓库实际使用 `master`。应统一为实际主分支，并保留 pull request 触发。

验收：向 `master` 推送或合并后能看到 clang-tidy workflow，且 `bugprone-*`、`performance-*` 门禁真正失败即阻断构建。

### 3.2 消除 Redis 故障测试重复注册

`CMakeLists.txt` 和 `tests/CMakeLists.txt` 都在 `ASIO_OWEN_BUILD_INTEGRATION_TESTS` 下注册 `tests/redis_fault_matrix.sh`，当前会产生两个 CTest 项并执行同一脚本两次。

验收：集成模式下只保留一个测试项，名称、超时和 `integration` label 统一。

### 3.3 拆分当前工作区提交

建议至少拆成以下提交：

1. 生命周期与 `RouteRuntime`/`ShutdownCoordinator` 修复。
2. HTTP session 注册表和 stop_sessions 行为。
3. 生命周期、关停和回归测试。
4. CI、文档和验证脚本。
5. 测试 warning 与无用代码清理。

每个提交都必须独立通过编译和相关测试，不把无关格式化或生成物混入功能提交。

## 4. P1：把并发正确性变成可证明的契约

### 4.1 统一关停语义

当前实现的实际语义是：

- 进入 draining 后拒绝新连接和新的 keep-alive 请求。
- `stop_sessions()` 取消并关闭所有已注册客户端 socket。
- 在途请求也可能在响应完成前被取消。
- 不承诺 5 秒宽限完成；5 秒只作为 drain deadline/强停上限。

需要同步更新 `docs/DEPLOYMENT_ROLLOUT_GUIDE_2026-08-18.md`，避免它继续声称服务会进行 5 秒连接排空。若未来改回真正 graceful drain，则必须修改实现和测试，而不是只改文字。

### 4.2 增加关停锚点测试

至少补充：

- 空闲 keep-alive socket 在 stop 后被关闭。
- 在途慢请求在 hard-stop 语义下被取消，并且不会产生重复 completion。
- drain deadline 到达时仍有 active session，forced-stop 计数和日志正确。
- `request_stop()`、`begin_draining()`、`mark_stopped()` 重复调用保持幂等。
- stop 发生在 worker completion 已产生但尚未回到 io executor 的窗口。

### 4.3 暴露可观测状态

`/api/metrics` 或等价内部指标应至少能区分：

- active sessions；
- active handlers；
- drain started/completed；
- drain timeout；
- forced stop；
- 被取消的 session/operation 类型。

这样线上才能区分“正常快速关停”和“因为慢请求被强切”。

## 5. P1：完成真实验证矩阵

### 5.1 Sanitizer 和 GCC 11

保留当前 sanitizer 分组，并确认两个 job 的 target 列表和 `-R` 正则都覆盖：

- `test_route_runtime`；
- `test_shutdown_coordinator`；
- `test_ready_routes`；
- `test_client_session` 生命周期测试；
- Admin/config/history/reload 相关测试。

验收：ASan 和 TSan workflow 均为绿色；不能只看 configure 成功或空匹配成功。

### 5.2 clang-tidy

clang-tidy 必须在实际应用库目标上执行，而不是只配置不编译。当前门禁重点是 `bugprone-*` 和 `performance-*`，出现新诊断应阻断构建。

### 5.3 Fuzz

六个 fuzz target 至少完成一次固定时长运行并保存摘要：

`fuzz_admin_json`、`fuzz_path_normalize`、`fuzz_http_framing`、`fuzz_ini`、`fuzz_history`、`fuzz_jwt`。

记录运行时长、执行次数、crash/oom/timeout 结果和 corpus 位置。没有原始运行证据时，只能记为“目标已定义”，不能记为“fuzz 已验证”。

### 5.4 Redis 故障矩阵

现有脚本覆盖 wrong-type、nil、CAS 和 Redis restart，但 9.0 需要确认这些场景真正经过应用的 RedisPool/config-center 调用链，而不是仅由 `redis-cli` 验证 Redis 服务器行为。

验收：集成测试启动最小应用或测试 harness，注入故障后验证 HTTP 状态、ready 状态、版本指针和恢复行为。

## 6. P1：建立性能证据

使用 `controlled_ab.sh` 做 baseline/candidate 轮级交替，不允许先完整跑完 A 再跑完整 B。至少执行：

- 预热 1 轮；
- 正式 3 轮以上；
- 每轮相同并发、时长和冷却时间；
- 固定 `BASELINE_PID`、`CANDIDATE_PID` 或等价进程参数；
- 记录 RPS、p50/p95/p99、CPU、RSS、FD、5xx/连接错误。

验收建议：candidate 相对 baseline 无统计显著回退；若有变化，必须解释是实现变化还是环境噪声。性能分不以“脚本存在”计满分，以归档的原始结果计分。

## 7. P2：架构收敛

这些项目不是到 9.0 的阻塞项，但完成后可继续提升架构分：

- 删除 `Application` 中已经由 `RouteRuntime` 接管的旧 owning/non-owning 资源字段。
- 让 `AppServices` 通过明确的构造函数或工厂创建，减少裸指针和部分初始化风险。
- 继续把大型 `*_impl.hpp` 拆成真正的 `.cpp` 编译单元，而不只是换文件名。
- 降低 `http` 层对 `app` 层 Admin 类型的反向 include 传播。
- 最终消除 `.inc + Kind` 状态机，改为按端点职责划分的自由函数或小组件。

## 8. 目标评分分解

达到以下分项目标后，简单平均约为 8.85；再加上 CI、文档和提交纪律全部闭环，才可四舍五入到 9.0：

| 维度 | 当前估计 | 目标 |
|---|---:|---:|
| 架构 | 8.4 | 8.7 |
| 内存安全 | 8.5 | 8.8 |
| 并发正确性 | 8.4 | 8.8 |
| 异常安全 | 8.0 | 8.7 |
| 性能 | 8.6 | 8.8 |
| 工程纪律 | 8.1 | 9.2 |

## 9. 最终签字条件

满足以下条件后，才在新的评审记录中写“9.0”：

1. `master` 上 GCC 11、ASan、TSan、clang-tidy、fuzz CI 均有绿色 run。
2. 普通构建全量 CTest 通过，唯一跳过项有环境原因和明确记录。
3. 关停语义、部署指南、GATEWAY 设计文档和测试保持一致。
4. Redis fault matrix 经过真实应用路径验证。
5. A/B 结果没有未解释的性能回退。
6. 工作区按职责拆分提交，任意功能提交可独立回滚和复测。

在这些条件完成前，评分应保持在 8.3～8.4 区间，避免把“代码已经写好”误记成“系统已经被证实”。
