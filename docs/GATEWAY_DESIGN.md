# HTTP 网关设计（当前实现）

> 当前状态更新于 2026-07-13。早期连接复用、probe、压测和故障排查记录保留在
> `docs/CONN_REUSE_PROBE_2026-07-10.md`、`docs/HTTP_POOL_PROBE_REMOVAL_2026-07-11.md`、
> `docs/HTTP_LAYER_REVIEW_2026-07-11.md`、`docs/PERF_REPORT.md` 等文档中。
> 本文是当前实现的总设计入口。

## 目标

网关接收客户端 HTTP/1.x 请求，将 `/{service}/...` 路由到配置的上游 HTTP 服务。
本地路由 `/api/health`、`/api/redis`、`/api/mysql`、`/api/combo` 精确匹配优先，
代理路由作为 fallback。

设计优先级：

1. 协议正确：遵守 HTTP/1.x body framing、hop-by-hop header、HTTP/1.0/1.1 语义。
2. 防请求走私：非法或歧义 framing 直接拒绝。
3. 资源有界：上游连接池有总量和并发上限，空闲连接会回收。
4. 超时完整：客户端读、客户端写、上游 resolve/connect/write/read 都有超时。
5. shutdown 可控：关闭 acceptor 和连接池 socket，in-flight 请求通过 RAII 收尾。

## 模块结构

| 文件 | 职责 |
|------|------|
| `src/http/http_server.hpp` | accept 循环、server 生命周期 |
| `src/http/client_session.hpp` | 客户端连接循环、请求解析、本地路由、代理流程 |
| `src/http/http_pool.hpp` | 上游连接池、分片锁、空闲回收、连接创建 |
| `src/http/proxy_forwarder.hpp` | 构造上游请求、读取上游响应 |
| `src/http/http_body_reader.hpp` | Content-Length/chunked body 读取 |
| `src/http/http_io.hpp` | 带超时的 async read/write |
| `src/http/response_builder.hpp` | 下游响应构造 |
| `src/http/upstream_manager.hpp` | `/{service}` 路由和每个 service 的 HttpPool |

```text
HttpServer
└── ClientSession
    ├── local routes
    ├── SecurityRules
    └── UpstreamManager
        └── HttpPool per service
```

## 请求流程

1. 以 `[server].client_header_read_timeout_ms` 读取客户端 header，默认 10s，最大 64KB。
2. 使用 picohttpparser 解析请求行和 header。
3. 更新 `HeaderParseState`：
   - 非法 `Content-Length` 返回 400。
   - 冲突重复 `Content-Length` 返回 400。
   - `Transfer-Encoding` 存在但不是 chunked 返回 400。
   - chunked 请求会先 de-chunk，再向上游写入新的 `Content-Length`。
4. 先完整消费请求 body，再执行安全规则、本地路由或代理路由。
5. 对代理响应按上游 framing 读取完整 body，再构造下游响应。
6. 下游写响应使用 `write_with_timeout()`，超时由 `downstream_write_timeout_ms` 控制。

## Keep-Alive

客户端侧：

- `ClientSession::run()` 在同一 socket 上循环处理请求。
- HTTP/1.1 默认 keep-alive，`Connection: close` 后写完响应断开。
- HTTP/1.0 默认关闭，只有 `Connection: keep-alive` 才继续复用。
- `client_preread` 保存一次 read 多读到的 pipeline 字节。
- framing 错误、请求超时、body 过大等 400/408/413 场景写完后关闭连接。

上游侧：

- 每个 service 一个 `HttpPool`。
- HTTP/1.1 上游响应默认可复用，除非 `Connection: close`。
- HTTP/1.0 上游响应默认不复用，除非 `Connection: keep-alive`。
- 无 `Content-Length` 且无 `Transfer-Encoding` 的响应按 EOF framing 读取，并标记连接不可复用。
- 当前实现不做主动 idle probe。复用 idle 连接时直接尝试写/读；如果复用的 idle 连接失败，只重试一次并新建连接。
- `conn.read_buffer` 非空时连接不会回到 idle，也不会再次复用，直接关闭。这样避免上游 pipeline/多余字节污染下一次请求。

## Header 处理

请求和响应都会过滤 hop-by-hop header：

- `Connection`
- `Keep-Alive`
- `Proxy-Connection`
- `Proxy-Authenticate`
- `Proxy-Authorization`
- `TE`
- `Trailer`
- `Transfer-Encoding`
- `Upgrade`

`Connection` header 中列出的扩展 token 对应 header 也会过滤。客户端 `Host` 被替换为上游
`host:port`。

上游请求构造还会：

- 过滤 `Accept-Encoding`，尽量让上游返回明文 body，便于日志和 JSON key 转换。
- 过滤 `Expect`，网关不实现 100-continue 透传。
- 扫描所有 header value，发现 CR/LF/NUL 直接返回 400，防止 header 注入和请求走私。
- 如果配置 `send_keep_alive_header = true`，显式向上游发送 `Connection: keep-alive`。

## Body Framing

请求侧：

- `Content-Length` 精确读取指定字节数。
- `Transfer-Encoding: chunked` 使用 chunk 状态机解析并 de-chunk。
- 同时存在 TE 和 CL 时，网关消费 chunked body，转发时移除原始 TE/CL 并写入新的 CL。
- body 大小受 `max_body_size` 限制。

响应侧：

1. `HEAD`、`1xx`、`204`、`304` 视为无 body。
2. chunked 响应 de-chunk 后再发给客户端。
3. Content-Length 响应精确读取 N 字节。
4. 无长度 framing 时读到 EOF，且连接不复用。

上游响应非法状态行、非法 framing、超时、body 超限、header 超限、body 不完整都会标记上游连接为 bad。

## 响应构造

`build_downstream_response()` 会保留上游 `status_text`，例如 `201 Created` 不会被改成
`201 OK`。

代理响应不会原样透传 hop-by-hop header。对于有 body 的响应，网关会过滤上游
`Content-Length`，因为 `json_keys_snake_to_camel()` 可能改变 body 长度，并在最终响应中按
实际 body 重写 CL。

无 body 响应保留上游 `Content-Length` 的语义，尤其是 HEAD 响应。

4xx/5xx 响应会加：

```http
Connection: close
X-Asio-Owen-Status-Source: proxy|local
```

## HttpPool

`HttpPool` 是按 service 独立创建的上游连接池，内部使用 16 个 shard 降低锁竞争。

核心字段：

```text
State
  ├── shards[16]
  │   ├── idle
  │   ├── active
  │   ├── total
  │   └── in_flight
  ├── total_count
  ├── in_flight_count
  ├── acquire_reused / acquire_created
  ├── released_idle / released_closed / released_bad
  └── last_global_evict_ms
```

配置：

- `max_size`：idle + active 上游 socket 全局硬上限。
- `max_concurrent`：active 请求全局上限；`0` 表示不额外限制。
- `max_body_size`：请求/响应 body 上限，由代理层使用。
- `connect_timeout_ms`：resolve + TCP connect 超时。
- `request_timeout_ms`：写上游请求超时。
- `read_timeout_ms`：读上游响应 header/body 超时。
- `idle_timeout_sec`：idle 连接回收阈值。
- `send_keep_alive_header`：是否向上游显式发送 `Connection: keep-alive`。

### acquire

1. 如果 pool 已停止，返回 `nullptr`。
2. 最多每秒做一次跨 shard idle 回收，防止冷 shard 长期持有 fd。
3. 如果配置了 `max_concurrent`，入口处一次性预订 `in_flight_count`。
4. 从 round-robin shard 起，遍历所有 shard 找 idle 连接。
5. idle 连接若 `connection_close` 或 `read_buffer` 非空，直接关闭并继续找下一个。
6. 命中 idle 后标记 `reused_from_idle = true`，登记到 `active`，返回连接。
7. 没有 idle 时，受 `max_size` 限制创建新连接，并登记到 `active`。
8. resolve/connect 超时或失败时回滚计数并抛出异常。

当前实现没有 `is_reusable_idle()`，也没有 `probe_dropped` 指标。历史 probe 问题见
`docs/CONN_REUSE_PROBE_2026-07-10.md`；删除 probe 的设计依据见
`docs/HTTP_POOL_PROBE_REMOVAL_2026-07-11.md`。

### release

连接不会回到 idle 的情况：

- socket 已关闭。
- `connection_close` 已设置。
- `read_buffer` 非空。

这些路径会关闭 socket、减少 `total_count`/`in_flight_count`，并增加
`released_closed`。

正常连接会更新 `last_used_at` 后入 idle 队列，并增加 `released_idle`。

失败连接通过 `release_bad()` 关闭，并增加 `released_bad`。

### stale idle retry

`ClientSession` 对代理请求最多尝试 2 次。只有第一次使用的是复用 idle 连接，且写上游或读上游失败时，才会重试。新建连接失败不做额外重试，避免放大上游故障。

## 超时矩阵

| 方向 | 操作 | 当前控制 |
|------|------|----------|
| 客户端 | header read | `client_header_read_timeout_ms`，默认 10s |
| 客户端 | body read | 30s |
| 客户端 | header size | 64KB |
| 客户端 | response write | `downstream_write_timeout_ms`，默认 30s |
| 上游 | resolve | `connect_timeout_ms` |
| 上游 | connect | `connect_timeout_ms` |
| 上游 | request write | `request_timeout_ms` |
| 上游 | response header/body read | `read_timeout_ms` |
| 两侧 | body size | `max_body_size` |

`read_with_timeout()` 和 `write_with_timeout()` 使用 `parallel_group(wait_for_one)`。超时结果会使对应路径按失败处理；resolve/connect 超时时显式 cancel/close 相关对象。

## 当前配置示例

仓库当前 `config.d/21-http_pool.ini`：

```ini
[http_pool]
max_size = 5120
max_concurrent = 0
max_body_size = 10485760
connect_timeout_ms = 1000
read_timeout_ms = 5000
request_timeout_ms = 5000
idle_timeout_sec = 45
send_keep_alive_header = true
stats_interval_sec = 30
```

`[http_pool]` 配置被所有上游连接池共享，目前不支持按 service 覆盖。`[upstream]`
变化需要重启。

## 已知限制

| 项目 | 说明 |
|------|------|
| HTTPS / TLS termination | 当前只支持明文 HTTP。 |
| 负载均衡 | 一个 service 对应一个 host:port，没有多实例轮询或熔断。 |
| 流式 body | 请求和响应 body 都完整缓冲，受 `max_body_size` 限制。 |
| 连接预热 | 第一次请求承担 resolve/connect 延迟。 |
| 热更新 | upstream 列表变更需要重启。 |
| `%2F` upstream 契约 | gateway 转发 raw path，上游必须不解码 `%2F`，否则可能绕过 gateway 路径黑名单。 |

## 当前性能、复用与内存基线

最新总体验证来自 2026-07-13 VM Release 构建巡检，详见
`docs/CODE_REVIEW_FIXES_2026-07-13.md`。历史 `PERF_REPORT.md`、
`PERF_ANALYSIS.md`、`CONN_REUSE_*` 记录的是不同阶段的压测和排查快照，
不能直接当成当前实现。

### 全链路压测

环境：VM `192.168.139.230`，6 核 / 15GB / Ubuntu 22.04 / GCC 11.4。
压测：30 线程、100 并发、30s，两轮取平均。

| 接口 | 平均 RPS | 平均延迟 | errors |
|------|---------:|---------:|-------:|
| `/api/health` | 115k | 0.89ms | 0 |
| `/api/redis` | 17.7k | 5.2ms | 0 |
| `/api/mysql` | 13.5k | 6.8ms | 0 |
| Config Direct | 18.7k | 5.3ms | 0 |
| Config Gateway | 13.4k | 7.1ms | 0 |

Config Direct 是直压同一个上游；Config Gateway 是走网关代理到该上游。

### 网关损耗

| 场景 | Direct RPS | Gateway RPS | 损耗 | 解释 |
|------|-----------:|------------:|-----:|------|
| 上游慢 | 6,275 | 6,285 | ~0% | 上游瓶颈掩盖网关开销 |
| 上游正常 | 15,882 | 12,520 | ~21% | 7 月 12 日最终基线 |
| 7/13 Release | 18,700 | 13,400 | ~28% | 最新 Release 全量压测，errors=0 |
| 上游快 | 17,670 | 12,543 | ~29% | VM 负载较低时上游更快 |

当前应把 **21-29%** 视为网关代理的正常开销区间，而不是 bug。每个网关请求比
直连多出：

1. 下游 HTTP 解析。
2. 安全链路检查。
3. HttpPool acquire/release。
4. 上游请求重建和 header 过滤。
5. 上游 write/read。
6. 上游响应解析和 body 读取。
7. JSON key snake_to_camel 转换。
8. 下游响应重建和写回。

绝对 RPS 会随上游服务状态、VM 负载、日志级别、JWT/鉴权配置、body 大小波动。
判断网关健康时，优先看错误率、复用率、延迟趋势和内存趋势，而不是单次 RPS。

### HTTP 连接复用

最终验证同时看了 TCP 层和 HttpPool 层：

| 层级 | 指标 | 值 | 方法 |
|------|------|---:|------|
| TCP | TCP stream 总数 | 46 | `tshark -e tcp.stream` |
| TCP | HTTP 请求总数 | 149,952 | 全量 pcap 统计 |
| TCP | 平均每 stream 请求 | 3,260 | 请求数 / stream 数 |
| TCP | 复用率 | >99.9% | stream 数远小于请求数 |
| HttpPool | `reused` | 1,596,104 | 7/13 `server.log` |
| HttpPool | `created` | 188 | 7/13 `server.log` |
| HttpPool | 复用率 | 99.988% | reused / (reused + created) |
| HttpPool | 平均每连接请求 | 8,490 | reused / created |
| HttpPool | `released_bad` | 0 | 7/13 `server.log` |
| HttpPool | `released_closed` | 0 | 7/13 `server.log` |

7 月 13 日巡检时最新稳定状态：

```text
zebra-config={total=95, idle=95, active=0, in_flight=0,
reused=1596104, created=188, released_idle=1596292,
released_closed=0, released_bad=0}
```

同时 `ss` 显示 ASIO 进程到上游 `127.0.0.1:30001` 存在稳定
ESTABLISHED 连接，和 `HttpPool total=95` 一致。

当前 HttpPool stats 不再有 `probe_dropped`。判断复用是否正常：

- 正常：`reused` 持续增长，`created` 稳定在池容量或重连量级，`released_bad`
  不持续暴涨。
- 异常：`reused` 不增长，`created` 接近请求数，或 `released_bad` 与请求量同步增长。
- TCP 层验证必须用全量 pcap 按 `tcp.stream` 统计，不能用小样本 SYN/SYN-ACK 比例估算。

`docs/CONN_REUSE_PROBE_2026-07-10.md` 中的 `probe_dropped` 是历史 probe
实现的指标；当前 probe 已删除，相关历史数据只用于理解事故演进。

### 内存和稳定性

7 月 13 日 Release 巡检：

| 指标 | 值 |
|------|---:|
| VmRSS | 120 MB |
| VmHWM | 122 MB |
| 线程数 | 58 |
| fd count | 123 |
| segfault | 0 |
| coredump | 0 |
| server.log error | 0 |
| ASan 错误 | 0 |

崩溃堆栈核查：

- `/proc/sys/kernel/core_pattern = core`
- `coredumpctl list` 为空
- `/var/lib/systemd/coredump` 为空
- repo 下未发现 `core` / `core.*` / `*.core`
- `journalctl -k` / `dmesg` 未发现 segfault、trap、core dumped
- `server.log` 未发现 crash、fatal、abort、terminate、ASAN/UBSAN

因此当前 VM 没有可还原的历史崩溃堆栈。对运行中进程用 `gdb`
抓取线程栈时，Release 二进制已 stripped，只能看到系统调用层；现状为
io_context 线程在 `epoll_wait`，worker/thread-pool 多数线程在
`pthread_cond_wait`，未见同步 socket write、异常处理卡住、崩溃或明显死锁栈。

历史 `docs/MEM_CHECK.md` 记录过更早阶段的 ASAN、Valgrind 和 RSS 检查：

- 非 ASAN Release/stripped 压测 RSS 稳定，是判断生产泄漏的主要依据。
- ASAN RSS 增长通常来自 shadow memory/quarantine，不能直接当生产泄漏证据。
- Valgrind `definitely lost = 0`、`indirectly lost = 0` 才是业务确定泄漏判断重点。
- 网关、HttpPool、UpstreamManager 加入后，历史记录显示 proxy 转发压测中 RSS 稳定。

后续每次改 HttpPool、body reader、proxy_forwarder、ClientSession 或 shutdown 流程，
都应至少更新本节或新增带日期的验证文档，并说明它是否覆盖当前基线。

## 历史文档索引

- `docs/CONN_REUSE_PROBE_2026-07-10.md`：`is_reusable_idle` 错误码覆盖导致复用失效的排查记录。
- `docs/HTTP_POOL_PROBE_REMOVAL_2026-07-11.md`：删除主动 idle probe、改用 try-and-retry 的设计依据。
- `docs/HTTP_LAYER_REVIEW_2026-07-11.md`：HTTP 层代码审计记录。
- `docs/PERF_REPORT.md`：阶段性压测报告，其中部分架构描述是历史状态。
- `docs/MEM_CHECK.md`：历史 ASAN/Valgrind/RSS 内存检查记录。
