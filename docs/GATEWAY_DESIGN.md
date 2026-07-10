# HTTP 网关设计

## 目标

在现有 standalone ASIO HTTP 服务器上增加 HTTP 反向代理能力。网关接收客户端 HTTP/1.x 请求，将 `/{service}/...`（例如 `/zebra-config/...`）路由到配置的上游 HTTP 服务，并通过有界连接池复用上游 keep-alive 连接。

本地路由例如 `/api/health`、`/api/redis`、`/api/mysql`、`/api/combo` 仍然拥有精确匹配优先级，优先于代理路由。

设计优先级如下：

1. **协议正确性**：遵守 RFC 7230 的 body framing、hop-by-hop 头处理、HTTP/1.0 与 HTTP/1.1 语义。
2. **防请求走私**：无法确定语义的 framing 直接拒绝，不猜测下游会如何解析。
3. **资源安全**：连接池有硬上限，空闲连接会回收，所有路径都通过 RAII 归还连接。
4. **抗慢连接攻击**：客户端和上游的读写都有超时控制。
5. **干净关闭**：停止 acceptor，等待 in-flight 请求退出，关闭 socket，不泄漏 fd。

## 模块结构

| 文件 | 职责 |
|---|---|
| `src/http/http_server.hpp` | accept 循环、HTTP 解析、body framing、请求转发、客户端 keep-alive |
| `src/http/http_pool.hpp` | 上游连接池，懒创建、空闲回收、硬上限 |
| `src/http/upstream_manager.hpp` | `/{svc}` 路由与每个 service 对应的连接池 |
| `src/http/http_context.hpp` | `HttpContext` 请求/响应数据载体 |

```text
HttpServer
├── UpstreamManager
│   └── HttpPool（每个 service 一个）
├── ConnGuard（RAII）
└── routes_（本地 /api/...）
```

## 请求流程

1. 以 30s 超时和 64KB header 上限读取客户端请求头。
2. 使用 picohttpparser 解析请求行和 header。
3. 通过 `parse_header_line_into` 将 framing header 解析进 `HeaderParseState`：
   - 非法 `Content-Length` 返回 400。
   - 冲突的重复 `Content-Length` 返回 400。
   - `Transfer-Encoding` 最后一个编码不是 `chunked` 返回 400。
   - 同时存在 `Transfer-Encoding` 和 `Content-Length` 时，转发前移除 `Content-Length`。
4. 无论本地路由还是代理路由，都先按 framing 规则消费完整请求 body。这样才能保证 keep-alive 和 pipeline 的后续字节不串位。
5. 分发顺序是精确本地路由、`/{service}/...`、404。

## Keep-Alive 支持确认

当前实现支持客户端和上游两侧 keep-alive。

客户端侧：

- `handle_connection` 在同一个客户端 socket 上循环读取请求，HTTP/1.1 默认保持连接。
- 如果客户端显式发送 `Connection: close`，当前响应写完后断开。
- HTTP/1.0 默认关闭，只有显式 `Connection: keep-alive` 才继续复用客户端连接。
- 读请求头和 body 时维护 `client_preread`，单次 `async_read_some` 多读到的 pipeline 字节会留给下一轮解析。
- framing 错误、请求超时、body 过大等场景会强制关闭客户端连接，避免把残留 body 当成下一次请求头解析。

上游侧：

- 每个 service 有独立 `HttpPool`，空闲连接会回池并被后续请求复用。
- 上游响应为 HTTP/1.1 且没有 `Connection: close` 时，连接可回到 idle 队列。
- 上游响应为 HTTP/1.0 时默认不复用，除非显式 `Connection: keep-alive`。
- 响应没有 `Content-Length` 或 `Transfer-Encoding` 时，按读到 EOF 处理，并标记该上游连接不可复用。
- `conn->read_buffer` 保存上游响应多读出的 pipeline 字节，连接回池时若该 buffer 超过 64KB 会丢弃该连接。
- idle 连接再次取出时会用非阻塞 `message_peek` 探活。健康 keep-alive idle socket 没有数据可读时，正确结果是 `would_block`/`try_again`，这表示连接仍可复用，不是 EOF。

因此，网关支持 keep-alive，但不是无条件复用：只在 framing 明确、连接未关闭、读写未超时、buffer 未异常膨胀时复用。

## 请求头解析状态机

```cpp
struct HeaderParseState {
    std::optional<size_t> content_length;        // 区分 "CL=0" 和 "没有 CL"
    bool duplicate_content_length = false;
    bool invalid_content_length = false;
    bool is_chunked = false;
    bool has_transfer_encoding = false;
    bool connection_close = false;
    bool connection_keep_alive = false;
};
```

`optional<size_t>` 很关键：`Content-Length: 0` 必须表示“有 body framing，但长度为 0”，而不是走“没有长度 framing，需要读到 EOF”的分支。用普通 `size_t` 容易把 CL=0 响应误判成无 framing，从而破坏 keep-alive。

`Connection` 使用 `split_connection_tokens` 解析：按逗号拆分、trim 每个 token、大小写不敏感地精确比较 `"close"` 和 `"keep-alive"`。禁止使用 `find("close")` 这种子串匹配，否则会把 `Connection: closed` 误判为 `close`。

## 分块传输处理

请求侧 chunked body 被完整反分块（de-chunk）后再转发，不保留原始 chunk framing。

实现使用协议感知的 `read_chunked_stream`：

1. `consume_line` 读取到 `\r\n`。
2. `parse_hex_size_line` 解析十六进制 chunk size，允许 `；` 后的 chunk extension。
3. `consume_exact` 精确读取 `size` 字节数据和后续 CRLF，并校验末尾 `\r\n`。
4. size 为 0 时继续读取 trailers，直到空行结束。

实现不能在原始 body 里搜索 `\r\n0\r\n` 之类字符串作为终止条件，因为二进制 chunk 数据里可能包含相同字节。必须使用按行解析的状态机。

`max_body_size` 限制作用在完整输出 buffer 上，包括 chunk 数据和 trailers，并且每次 append 后都检查。

当有 chunked 请求转发给上游时，网关会 de-chunk 后重写 `Content-Length`，不再保留 `Transfer-Encoding: chunked`。响应侧同样：网关会接管响应 framing，必要时 de-chunk，并重新计算 `Content-Length`。

## 逐跳 Header

请求和响应转发都会移除 hop-by-hop header：

- `Connection`
- `Keep-Alive`
- `Proxy-Connection`
- `Proxy-Authenticate`
- `Proxy-Authorization`
- `TE`
- `Trailer`
- `Transfer-Encoding`
- `Upgrade`

`Connection` header 中列出的扩展 token 对应的 header 也会被移除。客户端 `Host` 会被替换为上游 `host:port`。

> 默认不显式发送 `Connection: keep-alive`（HTTP/1.1 默认 keep-alive），避免触发 dubbo-go 的 h2c 升级检测。排查或兼容依赖显式 header 的上游时，可以打开 `[http_pool] send_keep_alive_header = true`。

请求侧如果是 `Transfer-Encoding: chunked`，网关会移除 `Transfer-Encoding` 并写入 `Content-Length`。响应侧：网关接管响应 framing，必要时 de-chunk 并计算 Content-Length。

## 响应分帧

上游响应 body 长度按 RFC 7230 优先级判断：

1. `HEAD` 请求的响应、`1xx`、`204`、`304` 没有 body。
2. `Transfer-Encoding: chunked` 使用 chunk 状态机解析。
3. `Content-Length` 精确读取 N 字节，多读出的字节保存到连接的 `read_buffer`，供下一次响应使用。
4. 没有长度 framing 时读到上游关闭，并且该连接不复用。

上游状态行非法、framing 非法、超时、body 超限、header 超过 64KB、body 不完整时，都会把上游连接标记为 bad，不归还 idle 池。

`Content-Length` 和 `Transfer-Encoding` 不会原样透传给客户端。网关在解析、大小限制和可能的 de-chunk 后重新拥有响应 framing，并按实际 body 重新计算 `Content-Length`。

`status_text` 会通过 `HttpContext::response_status_text` 端到端保留，例如 `201 Created`、`418 I'm a teapot`。不能把非 200 状态都硬编码成 `OK`。

客户端响应写当前使用裸 `async_write + redirect_error`，不额外套 `write_with_timeout`。这是为了避免热路径上每次响应写都创建 timer。写失败则直接结束当前客户端连接。

## HTTP/1.0 与 HTTP/1.1 默认行为

上游侧：

```cpp
int upstream_minor_version = status_line.rfind("HTTP/1.0", 0) == 0 ? 0 : 1;
conn.connection_close = header_state.connection_close ||
    (upstream_minor_version == 0 && !header_state.connection_keep_alive);
```

客户端侧：

```cpp
HeaderTokens tokens = split_connection_tokens(ctx.get_header("Connection"));
if (minor_version == 0 && !tokens.keep_alive) break;   // HTTP/1.0 默认关闭
if (tokens.close) break;                                // 显式关闭
```

如果没有正确处理 HTTP/1.0 默认关闭，就会复用上游已经关闭的 socket，表现为第一次 idle timeout 后出现连续 502。

## HttpPool

`HttpPool` 是懒创建、有边界的连接池：

- `max_size`：idle + active 上游 socket 总数上限。
- `max_concurrent`：active 请求数上限；`0` 表示不加这个额外限制。
- `max_body_size`：请求和响应 body 上限。
- `connect_timeout_ms`、`read_timeout_ms`、`request_timeout_ms`。
- `idle_timeout_sec`：懒回收空闲连接的时间阈值。

每次 acquire 返回的是 `unique_ptr<HttpConn>`，并且在 `acquire` 返回前已经在 `State::active` 中登记。idle 复用路径和新建连接路径都在 pool lock 内完成 active 跟踪，避免 shutdown 发生在 `acquire` 返回与 `ConnGuard` 构造之间时泄漏 fd。

`ConnGuard` 不可拷贝、不可移动。这样 `State::active` 中保存的 `HttpConn*` 在 guard 生命周期内始终有效，因为 `unique_ptr` 内对象地址不会变化。析构时先 `untrack_active`，再根据连接状态调用 `release` 或 `release_bad`。

pool state 通过 `shared_ptr<State>` 持有，因此 shutdown 和 in-flight guard 不会访问已析构的 mutex。`HttpPool` 析构只负责设置 `running=false` 并关闭 socket；计数清理由 in-flight `ConnGuard` 退出时完成。

`release` 在以下情况不会把连接放回 idle 队列：

- `socket.is_open()` 为 false，或 `connection_close` 已设置。
- `read_buffer.size() > 64KB`，避免同一个上游连接上的预读字节无限累积。

这些情况下会关闭 socket，并减少 `total` 和 `in_flight`。

### idle probe 语义

`acquire` 从 idle 队列取到连接后，会调用 `is_reusable_idle` 做一次轻量探活，避免复用已经被对端关闭的 stale socket。

探活流程：

1. 暂时把 socket 切到 non-blocking。
2. 用 `receive(..., message_peek)` 读取 1 字节但不消费。
3. 恢复原来的 non-blocking 状态。
4. 按 `receive` 的结果判定是否可复用。

判定规则：

| `message_peek` 结果 | 含义 | 是否复用 |
|---|---|:---:|
| `success && n > 0` | socket 上已有预读字节，例如上游 pipeline 响应残留 | 是 |
| `would_block` / `try_again` | socket 存活，当前没有数据可读，这是正常 idle keep-alive 状态 | 是 |
| `success && n == 0` | EOF，对端已经关闭或 half-close | 否 |
| 其他错误 | 连接状态不可确认或已坏 | 否 |

实现上必须保留 `receive` 的 `error_code`。不能用同一个变量再调用 `non_blocking(was_non_blocking, ec)`，否则恢复成功会覆盖 `would_block`，把健康 idle socket 误判为不可复用。2026-07-10 的 `reused=0` 故障就是这个原因，详见 `docs/CONN_REUSE_PROBE_2026-07-10.md`。

验证连接复用时不要只看 RPS 或 TIME_WAIT。优先看 `HttpPool` 的 `reused/created/probe_dropped/released_idle`，必要时用 tshark 按 `tcp.stream` 统计每条上游 TCP 连接承载的 HTTP request 数。修复后抓包确认：约 113 条 TCP stream 承载 109853 个 HTTP 请求，Top stream 单条承载 1047-1116 个请求，与 `HttpPool total≈112-113` 对齐。

### 关闭流程

1. `running.exchange(false)`，后续 `acquire` 返回 `nullptr`。
2. 加锁关闭所有 idle socket，并 cancel+close 所有 active socket。active socket 被 cancel 后，正在等待的 `async_read_some` 会失败，对应协程沿异常/错误路径退出，`ConnGuard` 执行 `release_bad`。
3. 每个 `ConnGuard` 持有 shared `State`，直到自己的协程完全 unwind。

## 超时与 Body 限制

| 方向 | 操作 | 超时或限制 |
|---|---|---|
| 客户端 | header 读取、body 读取 | 30s |
| 客户端 | header 大小 | 64KB 硬上限 |
| 客户端 | 写响应 | `downstream_write_timeout_ms`，默认 30s |
| 上游 | resolve | `connect_timeout_ms` |
| 上游 | connect | `connect_timeout_ms` |
| 上游 | 写请求 | `request_timeout_ms` |
| 上游 | 读响应 header + body | `read_timeout_ms` |
| 两侧 | body 大小 | `max_body_size`，默认 10MB |

客户端和上游读操作、上游写操作都通过 `steady_timer` 控制超时。超时时 timer callback 调用 `socket.cancel()`，让 pending async operation 以 `operation_aborted` 完成，协程随后按失败处理。每次操作使用独立 timer，不共享 timer 状态。

body 大小必须在读取循环内检查，而不是只在最后检查。恶意上游即使声明较小 `Content-Length` 后继续发送更多字节，也不能耗尽内存。

## Pipeline 支持

keep-alive 连接上的一次 `async_read_some` 可能读到多个 pipeline 请求/响应的字节。实现中有两个预读 buffer：

- `client_preread`：属于单个 `handle_connection`，跨客户端请求循环保存当前请求 body 之后的字节。
- `conn->read_buffer`：属于上游池化连接，跨上游请求保存当前响应 body 之后的字节。

交给下一轮解析时应 move，避免反复 copy 导致 O(N²) buffer 增长。

## 配置

网关配置位于 `config.d/` 目录，按文件名排序加载，后加载覆盖先加载：

```ini
[http_pool]
max_size = 256
max_concurrent = 0
max_body_size = 10485760
connect_timeout_ms = 1000
read_timeout_ms = 30000
request_timeout_ms = 60000
idle_timeout_sec = 60
send_keep_alive_header = false

[upstream]
config = 127.0.0.1:30001
member = 127.0.0.1:30003
goods = 127.0.0.1:30006
order = 127.0.0.1:30009
```

`[http_pool]` 配置被所有上游连接池共享。目前没有实现按 service 覆盖 pool 配置。上游列表在启动时加载；热更新和更严格的 service 名校验（例如 `[a-z][a-z0-9-]*`）是后续设计项。

## 已知限制

| 项目 | 说明 |
|---|---|
| HTTPS / TLS termination | 当前只支持明文 HTTP；下游 TLS 需要额外 OpenSSL 集成 |
| 负载均衡 | 一个 service 对应一个 host:port；没有多实例轮询或熔断 |
| 重试 | 只对复用 idle 上游连接时的写/读失败做一次重试，避免 TCP idle 关闭的 TOCTOU；新建连接失败不重试 |
| 流式 body | body 会完整缓冲后再转发，受 `max_body_size` 限制 |
| 连接预热 | 第一次请求需要承担 connect 延迟 |
| 可观测性 | 目前只有 `LOG_INFO`/`LOG_WARN`；没有 QPS、p99、连接池 gauge |
| 热更新 | `[upstream]` 变化需要重启 |
| `%2F` upstream 契约 | gateway 转发时使用 raw path（含 `%2F`）。upstream **必须不解码 `%2F`**，否则攻击者可通过 `%2F` 绕过 gateway 的路径黑名单 |

## 构建与测试

```bash
cmake -B build -S .
cmake --build build --target server
ctest --test-dir build --output-on-failure
```

`third_party/asio` 和 `third_party/spdlog` 已随仓库提交，clone 后不需要联网下载这两个头文件库。MySQL、hiredis、OpenSSL 仍使用系统开发包，CMake 会在 `pkg-config` 可用时优先使用它；不可用时通过 `find_path`/`find_library` 查找常见系统路径和 Homebrew 路径。

## 风险历史

以下问题来自设计和实现 review，保留为回归参考。

| 分类 | 问题 | 修复 |
|---|---|---|
| 请求走私 | 同时存在 CL + TE；下游按 CL，网关按 TE | 请求侧存在 TE 时移除 CL；上游响应侧遇到歧义时关闭连接 |
| 请求走私 | 多个 `Content-Length` 值不一致 | 检测 `duplicate_content_length` 并拒绝 |
| 分帧 | `Content-Length: 0` 触发“读到 EOF” | 用 `optional<size_t>` 区分“没有 CL”和“CL=0” |
| 分帧 | 用 `find("0\r\n\r\n")` 判断 chunked 结束，遇到 trailer-less 或二进制 chunk 会失败 | 改成按行解析的状态机 |
| Header 解析 | `substr(colon + 2)` 假设冒号后一定有空格 | key/value 两边都 `trim_copy` |
| Header 解析 | 非法 CL 导致 `stoul` 异常未捕获 | `parse_decimal_size` 返回 `optional` |
| 连接池 | shutdown 发生在 `acquire` 返回与 `ConnGuard` 构造之间导致 socket 泄漏 | 在 `acquire` 内持锁登记 active |
| 连接池 | `ConnGuard` 可移动，`active` 集合里留下悬空指针 | guard 不可移动，`unique_ptr` 保证 `HttpConn*` 稳定 |
| 资源 | 上游响应 body 没有大小限制 | 读取循环内和循环后都检查 `max_body_size` |
| 慢连接 | 客户端读取没有超时 | `read_with_timeout` 使用 30s 预算 |
| 协议 | HTTP/1.0 默认被当成 keep-alive | 检测 `HTTP/1.0` 前缀，默认关闭 |
| Header 解析 | `Connection: closed` 被子串匹配成 `close` | `split_connection_tokens` 精确比较 |
| 状态行 | `201 Created` 被渲染成 `201 OK` | 端到端保留 `status_text` |
| 转发 | 上游返回压缩 body，日志中只能看到二进制预览 | 转发时过滤 `Accept-Encoding`，让上游默认返回明文 body |
| 连接池 | 同一池化连接上的预读字节无限累积 | release 时 `read_buffer > 64KB` 则丢弃连接 |
| 连接池 | idle 复用连接探活通过后、真正写/读前对端关闭（TOCTOU） | 复用 idle 连接失败时做一次重试；新建连接失败不重试 |
| 连接池 | keep-alive 连接 `Connection` 头被 hop-by-hop 过滤，上游误用 `Connection: close` | 通过 `[http_pool] send_keep_alive_header` 可开关地显式添加 `Connection: keep-alive` |
| 连接池 | idle probe 的 `would_block` 被恢复 non-blocking 的成功结果覆盖，健康 idle socket 被误判不可复用 | `receive` 和恢复 socket 状态使用不同 `error_code`，保留 `would_block` 判定 |
| 连接池 | 缺少连接池状态统计，异常时不易定位 | acquire/retry/error 路径附带 `pool_stats={...}` |
| 客户端 | 请求解析失败直接断连 | 返回 `HTTP/1.1 400 Bad Request` + `Connection: close` |
| 客户端 | `phr_header headers[32]` 在 header 多时解析失败 | 扩大到 `headers[128]` |
| 客户端 | header 超过 64KB 静默断连 | 返回 `431 Request Header Fields Too Large` |
| 客户端 | `std::system_error` catch 只打 DEBUG 后直接关闭连接 | 改为 WARN 并尝试同步写 `500` |
| 客户端 | 非 2xx 响应来源不明 | 所有 4xx/5xx 响应带 `X-Asio-Owen-Status-Source` 头 |
| 压测 | plow `--body=file` 缺少 `@` 前缀导致 body 内容为文件名本身 | 修正为 `--body=@/path/to/file` |

## 压测结果

### 环境

- 虚拟机 192.168.139.230，Ubuntu 22.04，6 核 CPU
- 上游：zebra-config（Go 服务，127.0.0.1:30001）
- 压测工具：plow，100 并发，30s，POST JSON body

### 性能对比

| 路径 | #1 RPS | #2 RPS | 平均 RPS | 成功率 |
|:---|:---:|:---:|:---:|:---:|
| **直连** | 4,963 | 4,077 | **4,520** | 100% |
| **通过网关** | 4,486 | 4,027 | **4,257** | 100% |

网关相比直连损耗约 **6%**（在误差范围内）。

### 优化历程

| 阶段 | RPS | connection-error | 4xx | 关键改动 |
|:---|:---:|:---:|:---:|------|
| 初始版 | ~4k | ~64% | — | 无 |
| 日志降级 + timer 旁路 + header vector | ~13k | ~50% | — | `LOG_DEBUG`，`timeout<=0` 无 timer |
| idle 探活 + retry | ~14k | ~50% | — | `is_reusable_idle` + `message_peek` + 一次重试 |
| `Connection: keep-alive` | ~14k | ~4% | — | 转发可配置显式加 `Connection: keep-alive` |
| header 解析修复 | ~13k | 0 | ~50% | `pret<0` 返回 400，`headers[128]` |
| 4xx 来源追踪 | ~6k | 0 | ~50%（实为 plow 用法错误） | `X-Asio-Owen-Status-Source` 日志 |
| **plow body 修复** | **4,257** | **0** | **0** | `--body=@file` 加 `@` 前缀 |

> `Connection: keep-alive` 行是历史实验阶段记录。当前默认仍是 `send_keep_alive_header = false`；2026-07-10 的 `reused=0` 问题由 idle probe 错误码覆盖导致，不靠显式 keep-alive 头修复。

> plow 的 `--body` 参数如果值是文件路径必须加 `@` 前缀（`--body=@file.json`），否则 body 会是文件名字符串本身导致上游返回 4xx。

### 稳定性

| 检查项 | 结果 |
|--------|:----:|
| server 日志 `[error]` | **0** |
| dmesg 新 crash | **0** |
| plow 成功率 | **100%** |
| 压测后服务健康 | **正常** |
| 压测后内存（非 ASAN，含 proxy 转发） | **~56 MB**（Release stripped，3min 压测 RSS 全程稳定持平） |

## 压测工具用法

### plow POST 请求

```bash
# 直接指定 body 字符串（-b 参数接收字符串，-d 是 duration 不是 body）
plow -c 100 -d 30s -m POST \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer <token>' \
  -b '{"key":"value"}' \
  http://127.0.0.1:8081/api/example
```

> **注意：** `-b` 是 body 参数，`-d` 是 duration（时长），不要搞混。
> ⚠️ 实测 `-b @/tmp/file.json` 语法在当前 plow 版本（v1.x）无效，会发送文件名字符串本身而非文件内容，导致上游返回 404。必须用 `-b '{"key":"val"}'` 直接传 JSON 字符串。
> 如果 JSON 较长或需要换行，可以先赋值给 shell 变量：`BODY='{"key":"val"}'` 然后 `-b "$BODY"`。

### plow GET 请求

```bash
plow -c 100 -d 30s http://127.0.0.1:8081/api/health
```

### 常用参数

| 参数 | 说明 | 示例 |
|------|------|------|
| `-c, --concurrency=N` | 并发连接数 | `-c 100` |
| `-d, --duration=DUR` | 压测时长 | `-d 30s`、`-d 5m` |
| `-m, --method=METHOD` | HTTP 方法 | `-m POST` |
| `-H 'Key: Value'` | 请求头，可重复 | `-H 'Content-Type: application/json'` |
| `-b, --body=BODY` | 请求 body；以 `@` 开头则为文件路径 | `-b '{"k":"v"}'`、`-b @file.json` |
| `-T, --content=TYPE` | 快捷设置 Content-Type 头 | `-T 'application/json'` |
| `--json` | 输出 JSON 格式结果 | `--json` |
