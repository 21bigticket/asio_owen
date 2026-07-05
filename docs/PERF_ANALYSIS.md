# ASIO Owen v3.5 网关 Config 接口压测性能分析

> 分析日期: 2026-07-04
> 测试环境: 虚拟机 192.168.139.230, Ubuntu 22.04, 6 核 15GB RAM, 仅运行 zebra-config
> 上游服务: zebra-config Go 微服务 (127.0.0.1:30001), Triple 协议 HTTP+JSON
> 压测工具: plow (bench.sh), 50 并发, 30s x 3 轮, 轮间 10s
> 对比基准: pixiu (Dubbo-Go-Pixiu) 网关历史数据

---

## 1. 测试结果总览

### 1.1 不同场景 RPS 对比

| 测试场景 | RPS | 相对于 baseline | 说明 |
|:---------|:---:|:--------------:|:------|
| **Health（纯网关，返回静态 body）** | **~70,692** | — | 网关本身吞吐能力 |
| **直连 zebra-config:30001** | **~5,664** | baseline | 上游 Go 服务能力 |
| **asio 网关 → zebra-config (10s)** | **~3,910** | -31% | 短时间压测 |
| **asio 网关 → zebra-config (30s)** | **~3,000** | -47% | 长时间压测，性能衰减 |
| **pixiu 网关历史数据** | **~1,790-4,500** | — | 视优化阶段不同 |

### 1.2 关键现象

1. **Health 可达 70k RPS** → asio 网关自身不是瓶颈
2. **直连 5.6k** → 上游 Go 服务是天花板
3. **网关短时间 3.9k** → 比直连慢约 31%
4. **网关长时间 3.0k** → CPU 从 5.6% → 9.4%，性能随时间衰减

---

## 2. 瓶颈定位

### 2.1 strace 系统调用分析（50 并发，压测中途采样 5s）

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 41.64    0.515543          79      6508       452 futex
 20.38    0.252330          58      4339           sendto
 17.45    0.216066          32      6675           ioctl
 12.54    0.155276          21      7152      3528 recvfrom
  5.26    0.065094          29      2224           getpeername
  2.64    0.032634          44       731           epoll_wait
  0.10    0.001254          33        38           epoll_ctl
------ ----------- ----------- --------- --------- ----------------
100.00    1.238197          44     27667      3980 total
```

### 2.2 热点分布

| syscall | 占比 | 根因 |
|:--------|:---:|:-----|
| **futex** | **41.64%** | `HttpPool::acquire` 全程持 `std::mutex`，50 并发抢锁 |
| **sendto** | 20.38% | 向上游 socket 写 HTTP 请求 |
| **ioctl** | 17.45% | `is_reusable_idle` 中的 `non_blocking()` 切换（2 次 ioctl / 次检查） |
| **recvfrom** | 12.54% | 读上游响应 + `is_reusable_idle` 中的 `receive(peek)` |

### 2.3 瓶颈链路

```
请求到达 → security_rules::check()
  → JWT verify (~3-5μs)
  → path normalize (~1μs)
  → ip_blacklist (~1μs)
  → auth_whitelist (~0.5μs)
  → rate_limiter::check_all (~0.5μs)
→ HttpPool::acquire()                              ← 瓶颈在这里
  → lock(state->mtx)                               ← futex 41%
  → evict_stale_idle()
  → is_reusable_idle()                             ← ioctl 17% + recvfrom 13%
    → non_blocking(true)  [ioctl]
    → receive(peek)       [recvfrom]
    → non_blocking(false) [ioctl]
  → 新建连接 / 复用连接
  → sendto / recvfrom                               ← 正常 IO 33%
```

---

## 3. 根因详解

### 3.1 锁竞争（futex 41%）

`HttpPool::acquire` 在 `std::lock_guard lock(state->mtx)` 下完成以下全部操作：

1. `evict_stale_idle()` — 遍历 idle 队列淘汰过期连接
2. 循环尝试取空闲连接
3. 对每个连接调用 `is_reusable_idle()` — 含 3 次系统调用
4. 更新 `state->total`, `state->in_flight`, `state->active` 计数器
5. 如果无空闲连接，判断是否达到 `max_size`

**问题：** 锁持有时间包含系统调用（`ioctl` + `recvfrom`）。50 并发下，锁竞争成为第一热点。

**代码位置：** `http_pool.hpp:124-153`

```cpp
{
    std::lock_guard lock(state->mtx);   // ← 全程持锁
    evict_stale_idle(state);
    while (!state->idle.empty()) {
        auto conn = std::make_unique<HttpConn>(...);
        state->idle.pop_front();
        if (!is_reusable_idle(*conn)) {  // ← 锁内做系统调用！
            ...
            continue;
        }
        ...
        co_return std::move(conn);
    }
    ...
}
```

### 3.2 连接健康检查开销（ioctl 17% + recvfrom 13%）

`is_reusable_idle()` 每次复用空闲连接时都会做一次 socket 健康探测：

```cpp
static bool is_reusable_idle(HttpConn& conn) {
    // ...
    conn.socket.non_blocking(true, ec);     // syscall #1: ioctl
    char byte = 0;
    size_t n = conn.socket.receive(         // syscall #2: recvfrom(MSG_PEEK)
        asio::buffer(&byte, 1), asio::socket_base::message_peek, ec);
    conn.socket.non_blocking(false, ec);    // syscall #3: ioctl
    // ...
}
```

每个空闲连接复用都产生 **3 次系统调用**。50 并发 × 3000 RPS = 每秒最多 9000 次 `is_reusable_idle` 调用，每次 3 个 syscall = 27000 syscalls/s。

### 3.3 CPU 随时间增长的原因

压测过程中 CPU 从 5.6% → 9.4%（60s 内几乎翻倍），RPS 从 3.9k → 3.0k。

**根因：锁竞争随 idle 队列增长而加剧。**

阶段一（压测初启，idle 队列为空）：
- `acquire` 直接走"新建分支"（`http_pool.hpp:143-152`），锁内仅做计数器自增
- 锁持有时间在纳秒级，50 并发几乎无排队

阶段二（压测稳态，idle 队列积累）：
- `release` 把连接 `push_back` 到 idle 队列（`http_pool.hpp:207`）
- `acquire` 进入 while 循环（`http_pool.hpp:127-142`），**每弹出一个候选连接就在锁内调用 `is_reusable_idle`，触发 3 次 syscall**
- 单次 `acquire` 的锁持有时间从纳秒级跳到微秒级（被 syscall 主导）
- 50 并发同时抢锁 → futex 排队 → 平均等待时间累积 → CPU 在 futex wait/spin 上消耗

**实测验证（推翻"上游 Go 关闭连接"的假设）：**

```
压测前: 58 ESTAB
压测后: 58 ESTAB（全部保持）
连接到 30001: 50 ESTAB（全部保持）
```

上游 zebra-config 全程未关闭任何连接，连接重建不是 CPU 增长的原因。此前"Go `http.Transport` 90s idle 超时"的说法混淆了客户端/服务端语义——zebra-config 是服务端，`http.Transport.idle_conn_timeout` 是客户端配置，不适用。

**本质：锁持有时间 ≈ idle 队列弹出次数 × 3 syscall；负载稳定后该值上扬 → futex 排队恶化 → CPU 升高 + RPS 下降。**

---

## 4. 优化建议（按收益排序）

### 🔴 高收益（预期提升 20-40%）

#### 4.1 锁外做连接健康检查

将 `is_reusable_idle` 移出锁范围。锁内只做队列操作（O(1)），锁外做系统调用。

```cpp
// 锁内 —— 仅取连接
std::unique_ptr<HttpConn> conn;
{
    std::lock_guard lock(state->mtx);
    if (state->idle.empty()) { ... }
    conn = std::make_unique<HttpConn>(std::move(state->idle.front()));
    state->idle.pop_front();
}

// 锁外 —— 健康检查（系统调用密集）
if (conn && !is_reusable_idle(*conn)) {
    conn->socket.close();
    // 重新获取锁更新计数器
}
```

**风险：** 锁外检查时连接可能已被关闭，需要处理竞争条件。

#### 4.2 缓存连接健康状态

在 `HttpConn` 上增加 `last_health_check_ms` 和 `health_ok` 字段，每次检查后标记，60s 内不再重复检查。

```cpp
struct HttpConn {
    // ... existing fields ...
    int64_t last_health_check_ms = 0;
    bool health_check_passed = true;
};
```

`is_reusable_idle` 检查时间戳，60s 内直接返回缓存值。

### 🟡 中等收益（预期提升 10-20%）

#### 4.3 用 `SO_ERROR` 替代 `non_blocking + peek`

当前 3 次 syscall（non_blocking true / receive peek / non_blocking false）可以用 1 次 `getsockopt(SO_ERROR)` 替代。

```cpp
#include <sys/socket.h>
int err = 0;
socklen_t len = sizeof(err);
getsockopt(conn.socket.native_handle(), SOL_SOCKET, SO_ERROR, &err, &len);
if (err != 0) return false;
```

但需要验证 `SO_ERROR` 在 Linux 上的行为——连接关闭后 `SO_ERROR` 返回 `ECONNRESET` 还是 `0`（待确认）。

#### 4.4 增大上游 idle_timeout

上游 Go 服务默认 90s 关闭空闲连接。网关侧 `idle_timeout_sec` 可以增大到 300s，减少连接重建频率。

#### 4.5 锁粒度细化

`state->total`、`state->in_flight` 等计数器改用 `std::atomic<int>`，减少锁内操作。

### 🟢 低收益（预期提升 < 5%）

#### 4.6 减少 `getpeername` 调用

`is_reusable_idle` 中的 `non_blocking()` 返回当前状态后，缓存在 `HttpConn` 中。

---

## 5. 附录

### 5.1 测试命令

```bash
# 直连（baseline）
plow -c 50 -d 30s --body '{"appid":"member_03150715","config_key":"black_list"}' \
  -T 'application/json' -m POST --timeout 30s \
  http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey

# asio 网关
TOKEN='<jwt_token>'
plow -c 50 -d 30s --body '{"appid":"member_03150715","config_key":"black_list"}' \
  -T 'application/json' -m POST -H "Authorization: Bearer $TOKEN" --timeout 30s \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey

# Health（纯网关）
plow -c 50 -d 10s http://127.0.0.1:8081/api/health

# strace 采样
PID=$(pgrep -x server)  # 需要排除 Go 的 server 进程
strace -p $PID -c -S time 2>&1
```

### 5.2 关键代码文件

| 文件 | 职责 |
|:-----|:------|
| `src/http/http_pool.hpp` | HTTP 上游连接池，含 `acquire` / `is_reusable_idle` / `evict_stale_idle` |
| `src/http/http_server.hpp` | 网关核心，`handle_connection` / `read_proxy_response` / `read_chunked_stream` |
| `src/security/jwt_auth.hpp` | JWT 验签 |
| `src/security/security_rules.hpp` | 安全规则编排 |

### 5.3 连接池配置（config.ini）

```ini
[http_pool]
max_size = 5120          # 最大连接数
max_concurrent = 0       # 最大并发（0=不限）
max_body_size = 10485760 # 10MB
connect_timeout_ms = 1000
read_timeout_ms = 5000
request_timeout_ms = 5000
idle_timeout_sec = 60    # 空闲连接超时
```
