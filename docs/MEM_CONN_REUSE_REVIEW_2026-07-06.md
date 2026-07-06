# 内存检查与连接复用报告评估（2026-07-06）

## 结论

内存检查报告整体做到位，证据链足以支撑“暂无发现业务代码内存泄漏”的结论。

连接复用验证方向正确，方法论完整，实际证据可以支撑“连接复用正常工作”；但现有实际报告的数据口径仍需整理，暂不建议对外强调“精确复用率为 99.x%”，除非补齐 `HttpPool` 计数器或更精确的抓包隔离数据。

## 内存检查评估

### 已做到位的部分

1. 工具覆盖较完整

   报告同时使用了 ASAN/LSAN、Valgrind memcheck、非 ASAN RSS 趋势监控，覆盖了编译期插桩、二进制插桩和生产形态运行时趋势三类证据。

2. 路径覆盖较全面

   已覆盖启动退出、`/api/health`、`/api/redis`、`/api/mysql`、`/api/combo`、网关代理转发，以及 v3.5 新增的鉴权和限流链路。对当前项目的主要热路径来说，覆盖面是够用的。

3. 泄漏分类判断清楚

   Valgrind 结果中明确区分了：

   - `definitely lost = 0`
   - `indirectly lost = 0`
   - 少量 `possibly lost` 倾向于第三方库或运行时缓存
   - `still reachable` 属于进程退出时由 OS 回收的缓存

   这比只看 `ERROR SUMMARY` 更合理。

4. 能解释常驻内存来源

   报告没有简单把 RSS 视为泄漏，而是解释了线程栈虚拟内存、spdlog async queue、MySQL/Redis 连接、ASIO/socket buffer 等常驻内存来源。尤其指出 `spdlog::init_thread_pool(262144, 1)` 会预分配较大队列，这是有价值的。

5. 有压测趋势数据

   非 ASAN 版本多轮压测 RSS 稳定，ASAN/LSAN 无泄漏报告，Valgrind 无确定泄漏。三类证据方向一致。

### 仍需整理的问题

1. ASAN RSS 描述前后不一致

   报告前面提到 ASAN 版本 RSS 会增长到数百 MB，后面 v3.5 又记录 ASAN RSS 约 3-4 MB。两者可能来自不同构建、不同优化级别、不同采样方式或不同进程匹配方式，但当前文档没有统一解释。

   建议：增加一段“不同 ASAN 记录的口径说明”，明确每组数据对应的构建类型、启动方式、采样 PID 和压测阶段。

2. “已完成”和“计划”混在一起

   文档后半部分包含模块级 Valgrind、snapshot 损坏/加载专项、30 分钟长压等测试计划。有些是后续建议，不应和已执行结果混在同一结论里。

   建议：拆成两个小节：

   - 已完成验证
   - 后续建议验证

3. 代理转发路径没有新增 ASAN/Valgrind 专项报告

   文档记录 proxy 转发 3 分钟 RSS 稳定，但也说明该轮没有新增 ASAN/Valgrind 报告。这个不影响当前结论，但对“网关新增代码完全覆盖”来说还差一点。

   建议：后续补一次 ASAN 下的 proxy 转发长压，或者 Valgrind 下短周期 proxy 转发。

### 内存检查最终判断

当前证据足以支撑：

> ASAN/LSAN 无泄漏报告，非 ASAN RSS 长压稳定，Valgrind 无 definitely/indirectly lost；暂无发现业务代码内存泄漏。

不建议表述为：

> 已证明绝对无内存泄漏。

更严谨的说法是：

> 在已覆盖的启动退出、本地接口、数据库/Redis、网关转发、鉴权限流路径中，未发现业务代码内存泄漏证据。

## 连接复用评估

### 已做到位的部分

1. 验证方法设计完整

   `docs/CONN_REUSE_VERIFY.md` 和 `docs/CONN_REUSE_TCPDUMP.md` 给出的验证方法是合理的，包含：

   - `ss` 观察 ESTABLISHED 数量
   - TIME_WAIT 堆积检查
   - tcpdump/tshark 统计 HTTP 请求数与 SYN 数
   - 每条 TCP 流承载请求数
   - FIN/RST 关闭事件
   - 单条连接请求时间线
   - `HttpPool` 内置计数器建议

   这套方法论是到位的。

2. 实际报告已能证明复用在工作

   现有实际数据包含：

   - 请求数远大于新建连接数
   - TIME_WAIT 为 0
   - RST 为 0
   - ESTAB 压测前后没有持续增长
   - 60 秒压测下 SYN/秒稳定

   这些证据可以支撑“连接复用正常工作”，不是每次请求都新建连接。

3. 能识别错误抓包口径

   tcpdump 报告主动指出第一次抓包存在 BPF filter 问题：`tcp[13] & 2 == 2` 会同时匹配 SYN 和 SYN-ACK，并且端口 30001 上混有其他服务流量。这说明报告没有盲目接受错误数据，分析态度是严谨的。

4. 发现并记录 idle timeout 调优点

   后续 `IDLE_TIMEOUT_TUNING.md` 记录了 `idle_timeout_sec` 从 60 调到 45 的实验，并观察到 SYN/秒稳定、TIME_WAIT 为 0，说明连接池进入稳定周转状态。

### 仍需整理的问题

1. 实际报告数据口径不统一

   `CONN_REUSE_TCPDUMP_2026-07-05.md` 中同时出现了：

   - SYN-ACK = 237
   - 纯 SYN = 500
   - 请求数约 56,445
   - 复用率 99.1%
   - 结论中又写约 99.4%

   这些数字可能来自不同实验轮次，但当前文档放在一起容易造成混淆。

   建议：按实验轮次拆开，例如：

   - 实验 A：无效抓包，数据作废
   - 实验 B：SYN-ACK 抓包，结果如何
   - 实验 C：纯 SYN 抓包，结果如何
   - 实验 D：60 秒调优验证，结果如何

2. `ss + TIME_WAIT` 只能证明“基本正常”，不是精确复用率

   `CONN_REUSE_VERIFY_2026-07-05.md` 只用 ESTAB 和 TIME_WAIT 判断，结论写“基本正常”是合适的。但它不能单独证明复用率为 99.x%。

3. 抓包未完全隔离网关流量

   使用 `src port 30001` 仍会包含所有访问 30001 的客户端，包括其他 zebra 服务的内部 Triple 流量。报告中已承认这一点。

   建议：后续尽量按网关进程的本地源端口集合过滤，或在压测窗口内停掉其他访问 30001 的流量。

4. `tcpdump -c 1000` 样本可能偏向压测开头

   只抓 1000 个包容易集中在连接池初始填充阶段，不能代表完整 30 秒或 60 秒压测。

   建议：按时长抓包，而不是按包数截断，例如抓完整 30 秒/60 秒 pcap 后再统计。

5. 缺少正常路径的 `HttpPool` 计数器佐证

   代码里已有 `acquire_reused`、`acquire_created`、`released_idle`、`released_closed`、`released_bad` 等计数器，但目前主要在错误路径日志中打印。连接复用报告如果能补充压测前后的这些计数器，将比 tcpdump 更直接。

### 建议补充的最小验证

建议补一轮最小闭环验证：

1. 在正常路径临时打印或暴露 `HttpPool::stats()`。
2. 压测前记录 `created/reused/released_idle/released_bad`。
3. 跑 60 秒网关 POST 压测。
4. 压测后再次记录 counters。
5. 计算：

```text
reuse_rate = delta_reused / (delta_reused + delta_created)
bad_rate   = delta_released_bad / (delta_reused + delta_created)
```

通过条件：

- `reuse_rate > 95%`
- `bad_rate` 很低，理想情况下低于 1%
- `released_closed` 不快速增长
- `TIME_WAIT` 不堆积
- wrk/plow socket errors 为 0

这样可以把连接复用结论从“抓包推断”提升到“应用层计数器 + TCP 抓包双证据”。

### 连接复用最终判断

当前证据足以支撑：

> 网关到 Go 后端的 HTTP/1.1 连接复用正常工作，不存在每请求新建连接的问题。

当前证据不建议强表述为：

> 精确复用率已严谨证明为 99.1% 或 99.4%。

更严谨的说法是：

> 基于 tcpdump、TIME_WAIT、ESTAB 和 60 秒压测观察，连接复用率显著高于 95%；具体精确复用率建议以后用 `HttpPool` 计数器或更干净的 pcap 重新固化。

## 总体建议

1. 内存检查报告保留当前结论，但整理 ASAN RSS 口径，把已执行结果和后续计划分开。
2. 连接复用报告建议合并为一份最终版，把作废抓包、有效抓包、调优验证分实验列出。
3. 给 `HttpPool` 增加一个只读 stats 输出方式，便于后续不用改代码就能验证复用率。
4. 后续对外汇报时，内存部分可以说“做到位”；连接复用部分建议说“验证方向和证据基本到位，但精确复用率需要再固化”。
