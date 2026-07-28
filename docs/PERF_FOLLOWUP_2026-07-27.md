# 性能复测待办 — 2026-07-27

## 当前结论

- `OperationDeadline` 相比此前 mutex 方案表现明显更好：健康检查约从 `~80k RPS` 回升到 `~115k RPS`。
- 当前结果仍低于此前记录的 `~151k RPS` 基线，但该差异尚不能归因于新 deadline 实现：尚未确认两者是否使用相同二进制、配置、安全链、日志级别、编译选项和 VM 资源状态。
- `perf` 中 `OperationDeadline` timer 回调约占 2.72%，这是采样占比，不等同于删除该逻辑即可获得同等百分比吞吐提升。
- 已静态核对 `OperationDeadline` 的生命周期：session 的 I/O 与 timer 均在同一 strand；已取消/过期 timer 回调会先检查 `ec` 或 operation id，未确认存在 P0 级别的 socket 生命周期问题。

## 明天优先做：可比性复测

使用相同 VM、相同配置、相同压测命令和固定日志级别，交替执行：

1. `baseline`
2. `current`
3. `baseline`
4. `current`

每轮记录：RPS、平均延迟、p50/p95/p99、错误数、CPU 使用率、运行时配置摘要和 git revision。压测前做相同时间的预热，并记录是否有其他 VM 负载。

## 决策规则

- 若交替多轮后差距不稳定，优先归因于环境/基线不可比，不继续微优化。
- 若在完全一致条件下 `current` 持续比 baseline 低约 20% 以上，再分析新的 deadline 路径。
- 仅在差距稳定时，再评估减少 `OperationDeadline` 的 `shared_ptr<State>`、timer arm/disarm 或字符串路径的成本；不要仅根据单个 perf 热点百分比决定重构。

## 验证限制

当前受沙箱限制，本地 socket 测试无法 bind（`Operation not permitted`）。`server` 构建已通过；需在 VM 上运行带 socket 的 deadline 回归测试和 ASAN 构建验证。
