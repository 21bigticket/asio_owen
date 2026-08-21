# 评分对齐与验收记录(2026-08-21)

**评审方**:Claude(独立验证,全部结论基于本机实测)
**对象**:工作区→已提交的四提交系列(`a9b924c`/`dfad608`/`f260a74`/`de940de`,已推 origin/master)
**结论**:**8.5 / 10**(无条件),基准链 7.5(07-19)→ 7.7(07-26)→ 8.1(08-19)→ 8.2(08-21 交叉)→ 8.4(修正)→ **8.5(终审)**

---

## 1. 本轮事件线

| 时间 | 事件 | 结果 |
|---|---|---|
| 午后 | 外部 AI 交叉评审给 8.2 + 7 项问题 | 逐条实证:**4 实锤新发现、1 方向对细节低估、2 已知重述**;行号 5/5 准确,0 假问题 |
| 傍晚 | 对方修复 11 项 + VM 部署 | 验收 **11/11 属实**,本机复现 338/338 |
| 质疑 | 用户质疑 8.5 → 自压重审 | **降 8.4**:RedisPool guard 漏修(三池第三次不对称)+ 新并发代码无 TSan 证据 |
| 深夜 | 对方二次修复 5 项(含 Redis/FD 总账/结构债) | 验收 5/5,三套全绿,4 分职责提交推送 |
| 终审 | `.inc` 消解纯搬运审计 | **14/14 方法逐字等价**(git 对象比对,字符串感知括号配平) |

## 2. 交叉评审 4 个实锤(P2)及其闭环

| # | 问题 | 修复 | 验证 |
|---|---|---|---|
| 1 | server 端口无范围校验(70000→4464 静默错绑) | `app_config.hpp` throw + 范围检查 | 测试 `RejectsServerPortOutsideTcpRange` |
| 2 | MysqlPool Guard 析构可 terminate(bad_alloc) | `release() noexcept` + catch + **total_ 槽位回滚**(比 HttpPool 原版更完整) | 三套 sanitizer 全绿 |
| 3 | 无进程级资源预算 | 三本账:`io_threads`(0-256)/`max_client_connections`(8192,accept 后 session 前)/`HttpConnectionBudget` 跨池原子共享 | 测试 ×3(共享限流/热更不重建/容量拒绝计数) |
| 4 | `directory_iterator` 构造失败→静默全默认启动 | 构造显式判 `ec` + `increment(ec)` 重写 | 测试 `RejectsUnreadableOrEmptyConfigDirectory` |

同轮追加:RedisPool 归还路径对齐(质疑后发现)、FD 总账(`ProcessFxBudget` 溢出校验+getrlimit+启动/热更双验,`application.cpp:75/247`)、`query_timeout_ms=30000` + 部署文档"45 秒起,不再称 5 秒排空"、CI master 分支+`--target server` 全目标。

## 3. 结构债清零(超出声明部分)

- **`.inc` 清零 + 四个巨型实现头消除**(722/1362/1162/864 行):真编译单元(`admin_history_{query,orphan,common}.cpp` 等),声明集中于 `admin_history_operation.hpp`
- **Kind 状态机清零**:`StartAction` + 声明式 `RepairPlan` 替代运行时 switch
- **http→app 反向 include 清零**:真依赖倒置——`RouteLifecycle` 落 http 层(RAII lease + 双检 drain 竞态),`ShutdownCoordinator` 下沉 `common/`;旧 P1"HandlerLease 死代码"同时转正(`client_session.hpp:155`)
- **`.inc` 纯搬运审计 14/14**:旧片段方法体 vs 新文件(归一化空白+类限定符)逐字等价,零逻辑夹带

## 4. 终审证据(全部本机独立复现)

| 套件 | 结果 |
|---|---|
| 普通构建 | **344/344**(3.7s) |
| ASan | **344/344**(17.7s) |
| TSan | **344/344**(62.9s) |
| 提交树一致性 | `git status` 全清 = 测的树 = 提交的树 = 远程树 |

## 5. 六维评分

| 维度 | 08-19 | 终审 | 依据 |
|---|---:|---:|---|
| 架构 | 8.2 | **8.6** | .inc/巨型头/反向 include 三债清零 + FD 总账;控制面数据面同进程仍开放 |
| 内存安全 | 8.3 | **8.6** | ASan 复跑绿 + 三池 guard 类对齐 |
| 并发 | 8.1 | **8.6** | TSan 复跑绿(新并发原语过竞态检测) |
| 异常安全 | 7.6 | **8.4** | 端口/扫描/Guard 全闭环且带回归测试 |
| 性能 | 8.7 | **8.6** | 准入控制原子开销未压测(唯一保留项) |
| 工程纪律 | 7.6 | **8.2** | 4 分职责提交推送,四轮"代码好闭环慢"pattern 首次断裂 |
| **均分** | 8.1 | **8.50** | |

## 6. 到 9.0 剩余(对 `SCORE_TO_9_ROADMAP_2026-08-20.md` 签字条件的状态更新)

| 条件 | 状态 |
|---|---|
| 分职责提交 + 文档一致 | ✅ 本轮完成 |
| master 五 workflow 绿跑归档 | ⏳ 推送已触发,待首跑结果 |
| Redis 故障矩阵过真实应用路径 | ⏳ 脚本就绪(`tests/service_integration.sh`,wrong-type→500/restart→恢复→200 全走 HTTP 打真实 server),欠 Docker 镜像实跑 |
| 交替 A/B ≥3 轮 + perf 归档 | ❌ 未做(含准入控制开销压测) |
| fuzz corpus 归档 CI 工件 | ❌ 未做 |

## 附录:编译耗时(挂起待办)

拆分后 TU 18→32,增量构建实测 28.5s——真凶是 `asio_owen_app` 单体静态库的链接级联(server+17 测试二进制全量重链),非编译面恶化。四方案(测试合并单二进制/ccache/按域 object library/PCH+Ninja)已记录,另行专项处理。
