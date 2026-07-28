# 运行验证记录 — 2026-07-28

> 范围：本记录固化 2026-07-28 在 Ubuntu 生产相似环境完成的构建、部署、短压测及
> HttpPool 回收观察。它是一次运行快照，不与不同 revision、配置或 VM 状态下的历史
> 数据作性能提升/回归归因。

## 构建与部署

- 代码版本：`2ca878d`（`fix combo`）。
- 环境：Ubuntu 22.04，GCC 11.4。
- 候选目录构建、测试和 strip 成功；全量 `ctest` 为 **202/202 passed**。
- 切换部署后，Health、MySQL、Redis、Gateway smoke check 均返回 HTTP 200。
- 部署脚本在切换前保留的旧构建目录：
  `build_ubuntu.previous_20260728T033753Z`。

本次验证包含 `/api/combo` 的 Redis 写缓存异步投递改动：缓存写失败时不应阻塞或改变
MySQL 查询结果。该行为已由全量测试覆盖；本报告不单独从短压测数据推导其性能收益。

## 压测方法与边界

- 压测参数：30 threads、100 connections、每轮 30s。
- 每个接口执行两轮。
- 当前记录有 RPS、平均延迟和错误结果；**没有** p50/p95/p99、完整 CPU/VM 干扰数据，
  因而不用于判定小幅性能变化。

## 两轮结果

| 接口/路径 | 第 1 轮 RPS | 第 2 轮 RPS | 平均 RPS | 第 1 / 第 2 轮平均延迟 | 错误 |
|---|---:|---:|---:|---:|---:|
| Health | 140,494 | 122,660 | 131,577 | 未采集 | 0 |
| Redis | 30,966 | 30,361 | 30,664 | 2.93 / 3.02 ms | 0 |
| MySQL | 12,174 | 12,612 | 12,393 | 7.65 / 7.17 ms | 0 |
| Config 直连上游 | 16,694 | 15,971 | 16,333 | 6.17 / 6.65 ms | 0 |
| Config 经 Gateway | 13,728 | 14,521 | 14,125 | 7.04 / 6.54 ms | 0 |

### 可支持的解读

- 各接口两轮均无错误，Health 在本轮样本中的稳态量级约为 **130k RPS**；两轮间
  波动约 14.5%，不能把单轮差异解释为代码微优化收益或回归。
- Config Gateway 相对同次直连平均少约 **13.5% RPS**（14,125 对 16,333），平均延迟
  增加约 **0.38 ms**（6.79 对 6.41 ms）。这描述的是本次环境快照，而非通用固定损耗。
- Gateway 期间观察到 `%wait` 约 51–52%。这值得在后续受控压测中配合 CPU、run queue、
  上游状态和分位延迟继续观察；现有数据不足以判定为缺陷。

## HttpPool 连接复用与空闲回收

Gateway 压测日志中的 `zebra-config` 连接池状态：

| 时间 | total | idle | active | 说明 |
|---|---:|---:|---:|---|
| 13:47:35 | 92 | — | 88 | 压测中，高并发使用上游连接 |
| 13:48:35 | 93 | 93 | 0 | 压测结束后，连接均转为空闲 |
| 13:49:05 | 93 | 93 | 0 | 短暂保留，尚未超过回收阈值 |
| 13:49:35 | 0 | 0 | 0 | 后台维护回收完成 |

同一时段累计计数：

```text
created=94
reused=849213
released_idle=849307
released_closed=0
released_bad=0
```

结论：该轮中上游连接被大量复用，停止流量后约 60 秒内从 93 条空闲连接回落到 0；
没有 active/in_flight 残留，也未见 bad/closed 释放。这验证了 HttpPool 的空闲连接
回收，不存在此前“停流量后上游 idle FD 持续保留”的现象。

## 内存结论的边界

本次没有采集进程 VmRSS、VmHWM 或 heap profile。因此，上述连接回收证明的是 socket/
连接对象没有残留，**不能单独证明进程不存在所有形式的内存泄漏**。

后续内存验证应在每轮压测前、压测结束后及等待超过空闲回收阈值后记录 RSS；连续多轮后
若 RSS 回落并稳定，通常是 allocator 缓存或峰值缓冲。若在回收完成后仍逐轮单调增长，
再用 ASan/LSan 定位。Linux 观察示例：

```bash
PID=$(pgrep -f './server')
pidstat -r -p "$PID" 5
```

仓库提供可重复执行的 VM 脚本：

```bash
cd /mnt/mac/Users/mac/code/croot/asio_owen
REBUILD=1 bash run_asan_gateway_rss.sh
```

它会调用 `rebuild_asan.sh`、运行 5 轮 30s Gateway 压测、每轮冷却 75s、记录
VmRSS/VmHWM 和 wrk 输出，并在正常结束时停止 `asio-owen-asan.service` 触发 LSan。
完整输出保存到 `logs/asan_gateway_rss_<UTC 时间戳>.log`。脚本会停止普通服务；仅可在
测试环境执行。

### ASan Gateway RSS 五轮验证（06:43–06:52 UTC）

该次验证使用 `asio-owen-asan.service`，对 Config Gateway 执行 5 轮
`wrk -t30 -c100 -d30s`，每轮后静置 75 秒。完整原始日志为
`logs/asan_gateway_rss_20260728T064327Z.log`。

| 采样点 | VmRSS | VmHWM | Gateway RPS |
|---|---:|---:|---:|
| 启动后、压测前 | 138,332 KiB | 146,444 KiB | — |
| 第 1 轮冷却后 | 527,464 KiB | 528,372 KiB | 4,767.20 |
| 第 2 轮冷却后 | 529,072 KiB | 529,228 KiB | 5,802.73 |
| 第 3 轮冷却后 | 529,728 KiB | 529,728 KiB | 5,800.44 |
| 第 4 轮冷却后 | 529,596 KiB | 529,968 KiB | 6,780.76 |
| 第 5 轮冷却后 | 530,220 KiB | 530,220 KiB | 6,656.50 |

- 第 1 轮后 RSS 增加约 380 MiB，符合 ASan shadow memory、quarantine 以及首次高并发
  分配带来的显著常驻内存增长；ASan RPS 不能和 Release 性能数据横向比较。
- 后续四轮的冷却后 RSS 落在 527,464–530,220 KiB 区间，首尾差 2,756 KiB（约 0.5%），
  没有出现“每轮压测后持续大幅增长”的泄漏曲线。
- 压测期间请求均完成；原始 wrk 输出未出现 Socket errors 或 Non-2xx 段落。
- 服务停止前后，`journalctl` 和 `/tmp/asan_stderr.log` 均未捕获
  `ERROR: AddressSanitizer`、`ERROR: LeakSanitizer` 或 sanitizer `SUMMARY`。

这构成“本场景未发现 ASan 报错，RSS 在首次 warm-up 后趋稳”的正面证据，**但不是对所有
泄漏路径的数学证明**。下次复测仍应保留该脚本日志；如需进一步确认 LSan 的退出检测，
还应记录 `systemctl cat asio-owen-asan.service` 中的 `ASAN_OPTIONS`，确保没有关闭
`detect_leaks` 且 stderr 仍被 journal 或 `/tmp/asan_stderr.log` 捕获。

## 后续复测要求

需要比较 revision 或优化前后性能时，使用相同二进制构建选项、配置、日志级别、压测命令
和 VM 资源状态，交替多轮运行，并记录 p50/p95/p99、errors、CPU/VM 状态、RSS 与 git
revision。未满足这些条件时，仅将结果作为运行健康快照。
