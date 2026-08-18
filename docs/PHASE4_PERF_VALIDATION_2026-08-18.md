# Phase 4 性能、内存与连接池验收记录

**日期**：2026-08-18

**范围**：配置版本历史 Phase 4 上线后的网关吞吐、HttpPool 连接复用与回收、
Release 进程内存趋势及 Redis 配置历史内存占用。

**关联设计**：`docs/CONFIG_HISTORY_DESIGN_2026-08-18.md`

## 1. 验收结论

在 Ubuntu 目标机上使用当前部署版本完成 `./bench.sh` 全量压测，并额外执行一轮
`./bench.sh config` 观察重复网关压力后的资源回收。全部请求无 HTTP、socket 或业务错误，
网关吞吐位于历史短压测区间，HttpPool 在停止流量后完成空闲连接回收，Release RSS 在
首次预热后稳定，Redis 配置历史当前实际占用约 8 KiB。

本次未发现 Phase 4 对原有网关性能、HTTP 连接池或进程内存造成可观察回归。

该结论是当前环境的运行验收结论，不是严格的版本 A/B 性能归因。压测期间系统约有
50%～53% iowait，且直连上游吞吐波动明显，不能用本次单组数据承诺固定性能提升比例。

## 2. 环境与方法

| 项目 | 实测值 |
|---|---|
| 目标机 | `192.168.139.230` |
| 系统 | Ubuntu 22.04，6 vCPU，Intel i7-9750H |
| 编译器 | GCC/G++ 11.4.0 |
| 代码版本 | `523b69c`（与 `origin/master` 一致） |
| 测试 | Ubuntu 构建成功，`ctest` 291/291 passed |
| 服务进程 | PID 59965，systemd 状态 `active` |
| 压测入口 | 仓库根目录 `./bench.sh` |
| 常规参数 | 30 threads、100 connections、每轮 30s、每接口 2 轮 |

执行顺序：

1. 执行一次完整 `./bench.sh`，依次测试 Health、Redis、MySQL、Config 直连和 Config Gateway。
2. 额外执行一次 `./bench.sh config`，再增加两轮直连和两轮 Gateway 压测。
3. 压测前、压测中、停止流量及冷却后采集 `/proc/<pid>/status`、FD 数量和 HttpPool 日志。
4. 使用 Redis `MEMORY USAGE` 读取配置中心各 key 的实际内存占用。
5. 压测后重新检查 systemd、Health、Gateway、journal 和内核错误日志。

## 3. 接口压测结果

### 3.1 完整 `./bench.sh`

| 接口 | 第 1 轮 RPS | 第 2 轮 RPS | 平均 RPS | 平均延迟 | p99 | 错误 |
|---|---:|---:|---:|---:|---:|---:|
| Health | 116,109.53 | 118,152.78 | **117,131.16** | 818.30 / 794.15 us | 2.20 / 1.73 ms | 0 |
| Redis | 25,529.43 | 25,132.57 | **25,331.00** | 3.54 / 3.59 ms | 6.38 / 7.51 ms | 0 |
| MySQL | 11,404.19 | 10,507.60 | **10,955.90** | 7.92 / 8.60 ms | 12.51 / 14.53 ms | 0 |
| Config 直连 | 15,957.73 | 13,670.64 | **14,814.19** | 6.34 / 7.93 ms | 26.15 / 42.26 ms | 0 |
| Config Gateway | 10,941.01 | 9,299.39 | **10,120.20** | 9.13 / 10.25 ms | 40.69 / 35.12 ms | 0 |

### 3.2 额外 Config 重复压测

| 路径 | 第 1 轮 RPS | 第 2 轮 RPS | 平均延迟 | p99 | 错误 |
|---|---:|---:|---:|---:|---:|
| Config 直连 | 8,615.80 | 5,576.11 | 11.44 / 19.85 ms | 43.18 / 99.04 ms | 0 |
| Config Gateway | 9,133.14 | 8,827.94 | 10.34 / 10.73 ms | 33.50 / 36.08 ms | 0 |

四轮 Gateway 为 8,827.94～10,941.01 RPS，中位数约 9,216 RPS，落在历史文档记录的
8.8k～11k 短压测区间。四轮共完成约 1,148,829 个计量期 Gateway 请求，另有 warm-up
流量，未出现 socket error 或非 2xx 响应。

第二组测试中直连上游一度低于 Gateway，结合约 50%～53% iowait，说明主要波动来自
宿主机或上游状态。因此不能使用直连/Gateway 的单轮比值计算本版本固定转发损耗。

## 4. HttpPool 连接复用与回收

Gateway 压测期间，`zebra-config` 连接池随并发扩展到约 93 条连接：

| 采样状态 | total | idle | active | in_flight | 说明 |
|---|---:|---:|---:|---:|---|
| 压测中 | 92～93 | 2～7 | 86～90 | 86～90 | 连接被并发请求使用 |
| 停止流量后 | 93 | 93 | 0 | 0 | 全部连接归还为空闲 |
| 冷却约 60 秒后 | 0 | 0 | 0 | 0 | 空闲连接全部回收 |

两组 Gateway 压测累计计数：

```text
created=188
reused=1333053
released_idle=1333241
released_closed=0
released_bad=0
```

- 总获取次数为 `reused + created = 1,333,241`。
- 复用率约为 **99.986%**。
- 平均每条新建连接承载约 **7,091** 次获取。
- `released_idle` 与总获取次数完全相等。
- 冷却后没有 active 或 in_flight 残留，也没有 bad/closed 释放。

结论：连接复用、归还和空闲回收均符合 `docs/GATEWAY_DESIGN.md` 的既有语义，未发现
HTTP 上游连接泄漏或 Phase 4 导致的连接池行为变化。

## 5. Release 进程内存与 FD

| 采样点 | VmRSS | VmHWM | Threads | FD |
|---|---:|---:|---:|---:|
| 全量压测前 | 114,088 KiB | 119,324 KiB | 58 | 22 |
| Gateway 压测中最高采样 | 118,576 KiB | 119,324 KiB | 58 | — |
| 第一组压测冷却后 | 117,276 KiB | 119,324 KiB | 58 | 27 |
| 第二组 Config 压测冷却后 | 117,252 KiB | 119,324 KiB | 58 | 27 |

首次全量压测后 RSS 比初始值增加约 3.1 MiB，属于 Redis、MySQL、HTTP 池和分配器首次
预热的合理范围。关键证据是两次独立压力后的冷却值为 117,276 KiB 和 117,252 KiB，
没有逐轮单调增长；`VmHWM` 全程也没有超过压测前已经达到的 119,324 KiB。

线程数始终为 58。FD 从 22 增至 27，但 HttpPool 冷却后 `total=0`，因此这 5 个常驻 FD
属于预热后的数据库、Redis 或服务最小连接，不是未回收的 HTTP 上游 socket。

`VmSize` 在压测后增大不作为物理内存泄漏证据。它包含线程栈和 allocator arena 的虚拟
地址预留；本项目继续以 VmRSS、VmHWM、FD、线程数及多轮冷却趋势判断常驻资源。

## 6. Redis 配置历史内存实测

对当前 v1 配置中心 key 执行只读 `MEMORY USAGE`：

| Key | 内存 |
|---|---:|
| `asio_owen:config:version` | 72 B |
| `asio_owen:config:files` | 7,276 B |
| `asio_owen:config:history:1` | 7,284 B |
| `asio_owen:config:history:meta` | 840 B |
| `asio_owen:config:history:index` | 91 B |
| `asio_owen:config:audit` | 634 B |
| `asio_owen:config:machines` | 162 B |
| **合计** | **16,359 B（约 15.98 KiB）** |

其中 history snapshot、meta 和 index 合计 8,215 B，约 8.0 KiB。当前只有 v1，不存在
大 key 或明显内存压力。按设计保留 100 个当前规模版本时仍使用 1.3～2.5 MiB 的容量预算；
版本积累后应再次以 `MEMORY USAGE` 校准，而不能把当前单版本实测值简单线性视为最终值。

## 7. 验收后状态

- `systemctl is-active` 返回 `active`。
- `/api/health`、Redis、MySQL 和 Gateway 冒烟测试均返回 HTTP 200。
- 压测期间未观察到 warning/error 日志、segfault 或内核错误。
- 本次仅执行压测和只读观测，没有修改 Redis 数据或运行配置。

## 8. 结论边界与复测要求

本次证据足以支持“当前 Phase 4 版本未出现可观察的网关、HttpPool 和 Release 内存回归”。
由于宿主机 iowait 较高，本报告不支持小幅性能变化的因果判断。

需要比较两个 revision 的精确差异时，应在同一安静环境中交替部署两个二进制，固定配置、
日志级别和上游数据，使用 `PROFILE=1 ROUNDS=5 DURATION=60s` 留存原始结果，并比较中位
RPS、p99、CPU、iowait 和每轮冷却后的 RSS，而不是比较单轮最大 RPS。
