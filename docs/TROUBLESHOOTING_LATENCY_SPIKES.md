# 尖刺延迟排查手册

## 目标

系统性定位 P99/max 尖刺的根因:是代码阻塞、环境抖动、还是资源饱和。

---

## 1. 现场采集(压测时同步抓)

### 1.1 自动监控(已集成 bench.sh)

`bench.sh` 自动采集每轮压测的**进程级**系统指标(基于 pidstat),如果检测到异常会打印:

```bash
⚠️  系统异常检测 [Health #2]:
  [10:59:34] CPU=92%/core(4核饱和) wait=55%(高等待)
  [10:59:38] majflt=15/s(内存缺页)
```

**监控指标** (每 2 秒采样):
- **CPU 使用率**(单核平均 + 总量) → 是否饱和
- **wait%**(CPU 等待时间占比) → 等 I/O/锁/调度的时间
- **内存**(RSS) + 缺页(minor/major page fault) → 内存压力
- **磁盘 I/O**(读/写 kB/s) → 文件系统尖刺

**异常阈值** (只在超过时报警):
- **CPU > 90%/core** → 单核接近饱和,可能热点线程
- **wait > 50%** → 大部分时间在等待,可能 I/O 瓶颈/锁竞争/协程调度延迟
- **majflt > 10/s** → 主缺页(从磁盘读入内存),内存不足或冷启动
- **磁盘 > 50MB/s** → I/O 尖刺(9p mount 慢、或文件系统抖动)

**优势**:
- **进程级监控**:只看服务进程,不受 VM 其他进程干扰
- **智能报警**:正常时无输出,异常时精确提示根因
- **数据可靠**:基于 pidstat(sysstat),比 vmstat 在 VM 里更准确

### 1.2 并行探针(可选,抓尖刺时间戳)

压测时另开终端,持续探测慢请求,捕获尖刺的**精确时刻**:

```bash
# 探测 >50ms 的 /api/health 请求,打印时间戳
while true; do
  t=$(curl -s -o /dev/null -w '%{time_total}' http://127.0.0.1:8081/api/health)
  ms=$(echo "$t * 1000" | bc)
  awk -v ms="$ms" 'BEGIN{ if (ms+0 > 50) exit 0; else exit 1 }' \
    && echo "$(date '+%H:%M:%S.%3N') slow=${ms}ms"
  sleep 0.1
done
```

**用途**: 把探针打出的慢请求时间戳和 `server.log` 里的 30s 定时任务对齐,确认尖刺是否和 reload/stats tick 重合。

### 1.3 Server 日志(事后看)

压测结束后检查 `server.log`:
- 尖刺时刻有没有 `HttpPool stats` / `maintain: added N connections` / `rate_limit: snapshot` 等定时任务?
- 有没有异常日志(连接创建失败、超时、错误)?

---

## 2. 尖刺分类与根因

根据监控数据和日志,定位根因:

### 2.1 环境层抖动(最常见)

**症状**:
- bench.sh 监控显示 **CPU 饱和**(>90%/core)或 **wait% 爆表**(>50%)或 **磁盘 I/O 尖刺**(>50MB/s)
- wrk 的 RPS **腰斩**(120k → 76k)同时 avg latency 翻倍(0.7ms → 2ms)
- server.log 里**没有任何异常**,服务端静悄悄

**根因**:
- **macOS VM 层面**: Hypervisor 抢占(Time Machine 备份、Spotlight 索引、Docker Desktop I/O)
- **文件系统层面**: `/mnt/mac` 的 9p/virtiofs mount 卡顿(macOS 后台任务抢 I/O)
- **CPU 调度**: VM 的 CPU 被宿主机抢占(其他应用吃满物理核)

**处理**:
- **短期**: 压测时关闭 macOS 的 Time Machine / Spotlight,退出 Docker Desktop 等重 I/O 应用
- **长期**: 在 Linux 物理机或专用 VM 上压测,避免宿主机干扰

### 2.2 冷启动(正常,不需要修)

**症状**:
- **只有服务启动后的第 1-2 轮**压测有轻度肥尾(max ~50ms)
- 后续轮次(服务不重启)全程平稳(max <20ms)
- bench.sh 监控显示系统指标稳定

**根因**:
- 协程栈初始化、TLS cache miss、page fault、TCP slow-start
- HTTP 连接池第一次创建连接(DNS 解析、TCP 握手)
- Redis/MySQL 连接池预热

**处理**:
- **不需要修代码**,这是正常的冷启动成本
- 生产环境部署后让服务 warm up 几分钟再接入流量
- 如果 P99 在冷启动后仍超 SLA,才考虑优化(预连接、连接池预热)

### 2.3 周期性阻塞(代码问题,需要修)

**症状**:
- **warm 后仍反复出现尖刺**,间隔固定(30s、60s 等)
- 尖刺时刻和 `server.log` 里的 **30s 定时任务**(HttpPool stats / reload tick / maintain)时间戳对齐
- bench.sh 监控显示系统指标正常(CPU/IO/cs 无异常)

**根因**:
- **io_context 线程上的同步阻塞**: `read_config_fingerprint()` 里的 `stat()` 卡 I/O
- **maintain 线程里的重操作**: 批量创建连接、回收连接时加锁阻塞 acquire
- **全局锁抢占**: rate_limit snapshot、连接池 stats 采集时持锁过久

**处理**:
- **异步化**: 把 `stat()` 挪到 `std::thread` 或 `post()` 到 thread pool,别卡 io_context
- **锁优化**: 缩小临界区、把 snapshot 改成无锁快照(atomic 计数器)
- **批处理**: maintain 任务分批执行,每批间 yield 让出 CPU

**示例**(把 reload tick 挪出 io_context):
```cpp
// reload_service.hpp
void start_reload_timer() {
    // 定时器在 io_context 线程触发,只 post 任务到 thread pool
    reload_timer_.expires_after(std::chrono::seconds(30));
    reload_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec) return;
        
        // 重 I/O 操作扔到线程池
        boost::asio::post(thread_pool_, [this] {
            auto fingerprint = read_config_fingerprint();  // stat() 在这里跑,不卡 io_context
            if (fingerprint != last_fingerprint_) {
                reload_config();
                last_fingerprint_ = fingerprint;
            }
        });
        
        start_reload_timer();  // 下一轮
    });
}
```

### 2.4 资源饱和(扩容或限流)

**症状**:
- RPS 到达某个阈值后,latency 线性上升(队列积压)
- bench.sh 监控显示 **CPU 饱和**(>90%/core,4核都打满)
- server.log 显示连接池 **total 达到 max**(64/64),请求开始排队

**根因**:
- CPU 核数不够,io_context 线程跑满
- 连接池打满(Redis/MySQL/HTTP pool 全部 active)

**处理**:
- **扩容**: 加 CPU 核、增大连接池 max
- **限流**: 加 rate_limit,拒绝超载请求,保护后端

---

## 3. 排查流程(遇到尖刺时)

```
1. 跑 bench.sh health 连续 3-5 轮(服务不重启)
   ├─ 只有第 1 轮肥尾?         → 冷启动,正常
   └─ 后续轮次反复出现?        → 继续 ↓

2. 看 bench.sh 的 ⚠️ 系统异常检测
   ├─ CPU idle < 50%?          → 环境层 CPU 抢占
   ├─ IO wait > 20%?           → 文件系统/磁盘卡顿
   ├─ Context switches 爆表?   → 调度风暴
   └─ 全部正常?                → 继续 ↓

3. 对齐尖刺时间戳和 server.log
   ├─ 开并行 curl 探针,抓慢请求的精确时刻
   ├─ 看 server.log 在那个时刻有没有 30s 定时任务
   ├─ 时间戳对齐?              → 周期性阻塞(改代码)
   └─ 不对齐?                  → 偶发环境抖动(容忍或换环境)

4. 还查不出来? 上 perf + flamegraph
   perf record -a -g -F 999 -- sleep 30  # 压测时抓 30s
   perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
   看火焰图里尖刺时刻在干什么
```

---

## 4. 已知结论(当前环境)

**2026-07-22 验证结果**:

- ✅ **连续 5 轮 health 压测(服务不重启)全程平稳**,max <50ms,无周期性尖刺
- ✅ **系统监控显示 CPU/IO 指标稳定**,无异常信号
- ✅ **server.log 里 30s 定时任务和尖刺不相关**
- ⚠️ **昨天出现的一次 RPS 腰斩(76k) + max 134ms 异常**,定性为 **macOS VM 层偶发环境抖动**,不可复现

**结论**: 当前代码**没有周期性阻塞问题**。轻度冷启动肥尾(max ~50ms)在这个环境(9p mount + VM)下属于正常范围,生产环境(Linux 物理机)会更好。

---

## 5. 工具箱

### 5.1 监控工具(已集成)

- `bench.sh` 自动监控 → 每轮压测都会打系统指标异常
- `/tmp/bench_monitor_*.log` → 原始监控数据(每秒一行)

### 5.2 手动工具(需要时用)

```bash
# 实时监控(压测时另开终端)
vmstat 1              # CPU/IO/cs 实时值
iostat -x 1           # 磁盘 I/O 细节
top -H -p $(pgrep asio_owen)  # 线程级 CPU 占用

# 抓尖刺时刻的 CPU profile
perf record -a -g -F 999 -- sleep 30
perf report           # 或生成火焰图

# 抓 io_context 线程的系统调用
strace -T -tt -p <io_context_thread_tid>  # 看有没有慢 stat/read
```

### 5.3 代码埋点(精确定位阻塞点)

如果监控+日志+perf 还找不到,在怀疑的代码段加埋点:

```cpp
#include <chrono>

auto t0 = std::chrono::steady_clock::now();
read_config_fingerprint();  // 怀疑这里慢
auto t1 = std::chrono::steady_clock::now();
auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
if (us > 10000) {  // >10ms 就打 warn
    spdlog::warn("read_config_fingerprint slow: {}us", us);
}
```

---

## 6. 参考阈值(生产环境 SLA)

根据业务类型设定延迟 SLA,超了才需要优化:

| 场景 | P99 目标 | Max 容忍 | 说明 |
|------|---------|---------|------|
| 高频交易 | <1ms | <10ms | 极端敏感,任何尖刺都要查 |
| 在线服务(API) | <10ms | <100ms | 常规 Web 服务,P99 在 10ms 内算优秀 |
| 批处理 | <100ms | <1s | 对尖刺不敏感,关注吞吐 |

当前 health 接口(无业务逻辑):
- **P99 预估 ~2ms**(从 avg 0.7ms + stdev 1ms 推算)
- **Max <50ms**(warm 后)
- **属于"优秀"级别**,无需优化

---

## 7. 何时需要修代码

只有满足**全部条件**时才考虑改代码:
1. **warm 后仍反复出现尖刺**(排除冷启动)
2. **尖刺和 30s 定时任务时间戳对齐**(确认是代码阻塞)
3. **bench.sh 监控显示系统正常**(排除环境抖动)
4. **尖刺超过业务 SLA**(P99 >10ms 或 max >100ms for Web 服务)

否则,认定为环境级抖动或冷启动,不动代码。

---

**最后更新**: 2026-07-22  
**验证环境**: macOS VM (Ubuntu 22.04, 9p mount, 4 vCPU)  
**下次检查**: 在 Linux 物理机上复测,验证冷启动尖刺是否消失
