# `src/http/` 全量代码审计

> 日期：2026-07-11
> 范围：`src/http/` 目录下 12 个文件、2228 行 C++20
> 审计视角：正确性、安全、协程模型一致性、并发安全、HTTP 协议合规、可维护性
> 关联文档：
> - `docs/HTTP_POOL_PROBE_REMOVAL_2026-07-11.md`（HttpPool probe 移除专题）
> - `docs/CONN_REUSE_PROBE_2026-07-10.md`（probe ec 覆盖 bug 修复）
> - `docs/VERIFY_2026-07-11.md`（同 bug 的全量验证报告）
> - `docs/PERF_REPORT.md`（v3.5 性能基线）
> - `~/.claude/projects/-Users-mac-code-croot-asio-owen/memory/httppool-perf-optimization-2026-07.md`（v3.5 +37% RPS 记忆）

---

## 0. TL;DR

**整体定位：中上**。`src/http/` 的整体架构扎实——分片锁、热重载、RAII、`parallel_group` 超时、日志脱敏都符合现代 Asio 协程的最佳实践。代码质量评价 **中上**，不存在结构性缺陷。

**真正紧急的只有一条**：`response.hpp:15` 的 `json_resp` 直接拼字符串无转义，被 `app/routes.cpp` 用 DB/Redis 错误信息调用 → MySQL 报错含 `"` 时 100% 触发畸形 JSON（C1）。

加上 HEAD 响应 Content-Length 永远 0（H1）、accept 持续失败死循环（H3）、潜在 CRLF 注入（H4）等 5 个 HIGH 级问题，共发现：

| 等级 | 数量 | 典型问题 |
|:----:|:----:|:---------|
| 🔴 CRITICAL | 1 | JSON 注入（C1） |
| 🟠 HIGH | 5 | HEAD CL=0 / accept 死循环 / CRLF 注入 / 64KB 魔法数 / probe 全套 |
| 🟡 MEDIUM | 10 | sync write in catch（C2 修订）/ read_buffer 反转（C3 修订）/ 重复实现 / 别名坑 等 |
| 🟢 LOW | 5 | 函数内 static / 写死的 shard 数 / O(n×m) 等 |

修复 P0 一条预计 1 小时；P1 五条约半天；P2/P3 可按需。

> ⚠️ **修订注记**：本文档初版将 C2（catch 同步 write）和 C3（read_buffer 反转）定为 CRITICAL，**经二次评审降为 MEDIUM**，理由见 §0.5。初版"主要风险集中在两个反模式"的总结不准确——sync I/O 反模式确实存在但触发条件苛刻，影响有限。真正紧急的只剩 C1 一条。

---

## 0.5 修订注记（2026-07-11 二次评审）

本文档初次发布后经二次评审，对部分严重度判断作出修正：

| 编号 | 原定级 | 修正后 | 修正理由 |
|:-----|:------:|:------:|:---------|
| **C2**（catch 同步 write） | CRITICAL | **MEDIUM** | catch 是异常路径、写 ~200B 小响应、最坏几 ms 阻塞，不会"卡死事件循环"。初版将 sync I/O 反模式一律定 CRITICAL 过于激进。详见 §2.C2 内的修订说明。 |
| **C3**（read_buffer 反转） | CRITICAL | **MEDIUM** | 触发条件是"上游发出 framing 之外的多余字节"，正常生产场景不触发。fix 极简（一行条件翻转）但严重度被高估。详见 §2.C3 内的修订说明。 |

**承认初版的不准确之处**：

1. 初版"主要风险集中在两个反模式（字符串无转义 + 同步 I/O 混进协程）"的总结偏激。字符串无转义是真问题（C1），但同步 I/O 那两个（C2 catch、C3 probe syscall）触发条件都很苛刻。
2. 初版把 C2、C3 都定 CRITICAL，导致 P0 列表虚高（实际 P0 只有 C1 一条）。

**真正紧急的只剩 C1 一条**——它在 MySQL 报错时 100% 触发，且当前已被 `app/routes.cpp:17, 30, 37` 多处调用。其余都是 P1/P2 级别的合规性和健壮性改进。

**其它 P1 HIGH 问题（H1/H3/H4）初版已识别**，本次仅做措辞收紧，定级不变。

---

## 1. 文件清单与审计范围

```
src/http/
├── response.hpp            36 行   JSON 响应构造（CRITICAL: 无转义）
├── http_context.hpp        45 行   HttpContext 结构 + handler 类型
├── http_server.hpp         88 行   accept 循环 + session 派发
├── json_transform.hpp      90 行   JSON key snake→camel
├── response_builder.hpp    97 行   下游响应序列化（HIGH: HEAD CL=0）
├── http_io.hpp            113 行   read/write with timeout 工具
├── upstream_manager.hpp   117 行   upstream 路由 + 热重载
├── http_body_reader.hpp   187 行   chunked / content-length body reader
├── http_protocol.hpp      275 行   header 解析工具集
├── proxy_forwarder.hpp    285 行   上游请求构造 + 响应读取
├── client_session.hpp     425 行   客户端会话主循环（CRITICAL: sync write）
└── http_pool.hpp          470 行   HTTP 连接池（CRITICAL: probe / read_buffer）
                         ─────
                         2228 行
```

审计方法：人工通读 + 针对每文件提取以下风险类别：

- **HTTP 协议合规**：framing、hop-by-hop、Content-Length、HEAD/204/304 语义
- **协程模型一致性**：是否避免同步阻塞、是否正确使用 `co_await`
- **并发安全**：shared_mutex 使用、shared_ptr 生命周期、原子操作
- **内存安全**：边界检查、move 后状态、RAII
- **注入防御**：CRLF 注入、JSON 转义、日志注入
- **资源管理**：fd 泄漏、连接泄漏、内存增长
- **错误处理**：异常路径、错误码传播、降级策略

---

## 2. 🔴 CRITICAL 问题（生产直接出 bug）

> 本节为初版定级，二次评审后仅保留 **C1 一条** 为 CRITICAL。C2、C3 经 §0.5 修订降至 MEDIUM，但其代码内容仍在本节展示（带修订标记），以便对照原始评估思路。

### C1. `response.hpp` JSON 注入与畸形输出 ⚠️ **唯一真 CRITICAL**

#### 位置

`src/http/response.hpp:15-22`

#### 代码

```cpp
inline std::string json_resp(int code, const std::string& msg, const std::string& data = "null") {
    std::ostringstream oss;
    oss << R"({"code":)" << code 
        << R"(,"msg":")" << msg << R"(")"
        << R"(,"data":)" << data 
        << "}";
    return oss.str();
}

inline std::string resp_ok_str(const std::string& data) {
    std::ostringstream oss;
    oss << "\"" << data << "\"";          // ← data 也无转义
    return json_resp(OK, "ok", oss.str());
}

inline std::string resp_err(int code, const std::string& msg) {
    return json_resp(code, msg);
}
```

#### 问题

`msg` 和 `data` 直接拼进 JSON 字符串，**没有任何字符转义**。JSON 规范（RFC 8259 §7）要求字符串内的 `"`、`\`、控制字符（U+0000 ~ U+001F）必须转义。当前实现下：

| 输入含 | 输出 |
|:-------|:-----|
| `"` | 提前关闭字符串，后续被当成 JSON 语法 |
| `\` | 转义下一个字符，引发解析器状态错乱 |
| `\n` `\r` `\t` | 在字符串里出现裸控制字符，严格 JSON 解析器拒绝 |
| `</script>` | 如果响应被嵌入 HTML（错误用法），XSS |

#### 触发路径

`app/routes.cpp` 多处把**非字面量**塞进 msg：

```cpp
// routes.cpp:17
ctx.response_body = resp_err(DB_ERROR, res.error);
// res.error 是 MySQL 错误信息

// routes.cpp:30
ctx.response_body = resp_err(DB_ERROR, g.error);
// Redis 错误信息

// routes.cpp:37
ctx.response_body = resp_err(DB_ERROR, e.what());
// 异常 message

// routes.cpp:33, 68, 79
ctx.response_body = resp_ok_str(g.str);
ctx.response_body = resp_ok_str(data);   // data 来自 Redis 缓存值
```

#### 100% 触发的真实场景

MySQL 经典 SQL 语法错误的官方提示文本：

```
You have an error in your SQL syntax; check the manual that corresponds to your
MySQL server version for the right syntax to use near '"foo"' at line 1
```

注意末尾 `'"foo"'` —— MySQL **故意用双引号包裹出错位置附近的 SQL 片段**，这是文档化的行为，不是偶发。

当前 `json_resp` 拼出来的响应是：

```json
{"code":501,"msg":"... near '"foo"' at line 1","data":null}
```

JSON 解析器看到 `"msg":"... near '"` 时，认为字符串已经结束在第二个 `"`，后面的 `foo"'` 被当作语法错误。**严格 JSON 解析器（Java Jackson、Python `json.loads`、Go `encoding/json`、PostgreSQL `jsonb`）全部拒绝**。

只要 `/api/mysql` 触发一次 SQL 错误（参数错配、表名拼写错、注入尝试、上游 schema 变更），客户端拿到的就不是 JSON 而是 HTML 错误页或解析失败异常。

#### 同样会触发的 Redis 场景

`WRONGTYPE Operation against a key holding the wrong value` —— Redis 自己不含 `"`，但 `WATCH` 失败、`MULTI/EXEC` 异常等场景的 message 含引号是常态。`/api/redis` 路径同样脆弱。

#### 生产影响

- **客户端解析失败**：strict JSON parser（如 Java Jackson、Python `json.loads` 默认严格模式）抛异常，整个响应被丢弃。
- **响应分裂**：恶意 Redis 缓存值（被攻击者写入）可以构造 `","evil":"` 这样的内容，让 JSON 字段被注入。
- **日志混淆**：错误响应被日志记录，含特殊字符的 JSON 影响后续日志解析。

#### 修复

加一个简单的 JSON 字符串 escape 函数：

```cpp
inline std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

inline std::string json_resp(int code, const std::string& msg, const std::string& data = "null") {
    std::ostringstream oss;
    oss << R"({"code":)" << code 
        << R"(,"msg":")" << json_escape(msg) << R"(")"
        << R"(,"data":)" << data 
        << "}";
    return oss.str();
}

inline std::string resp_ok_str(const std::string& data) {
    std::ostringstream oss;
    oss << "\"" << json_escape(data) << "\"";
    return json_resp(OK, "ok", oss.str());
}
```

注意 `data` 参数（第三个）调用者负责保证它是合法 JSON 值，所以 `json_resp` 本身不 escape data；但 `resp_ok_str` 把 data 当字符串拼，需要 escape。

#### 验证

新增单测 `tests/test_response.cpp`：

```cpp
TEST(Response, EscapesQuotesInMsg) {
    auto s = resp_err(500, "syntax error near \"WHERE\"");
    EXPECT_NE(s.find("\\\"WHERE\\\""), std::string::npos);
    // 解析回来应当能拿到完整 msg
}

TEST(Response, EscapesControlChars) {
    auto s = resp_err(500, "line1\nline2\ttab");
    EXPECT_NE(s.find("\\n"), std::string::npos);
    EXPECT_NE(s.find("\\t"), std::string::npos);
}
```

**工作量**：1 小时（含单测）。

---

### C2. `client_session.hpp` 协程内同步 `asio::write` 阻塞 io_context

> 📝 **修订（2026-07-11 二次评审）**：严重度 **CRITICAL → MEDIUM**。
>
> 初版认定此条会"卡死 io_context 线程"过于激进。实际情况：
> - catch 块只在协程抛异常时进入，**异常路径本身就是低频**
> - 写的是 ~200 字节的错误响应，正常完成 < 1ms
> - 即使客户端 TCP 窗口满，最坏几 ms 阻塞，且异常路径之后协程就退出了，不会累积
>
> 仍然是反模式（与 v3.5 把 MySQL 改成 `asio::post` 异步的初衷一致），应当修，但不是 P0。归入 P2 修复批次。

#### 位置

`src/http/client_session.hpp:399-411`

#### 代码

```cpp
} catch (const std::system_error& e) {
    LOG_WARN("Connection system_error: ", e.what());
    std::string body = "{\"code\":500,\"msg\":\"connection error\"}";
    auto resp = build_error_response(500, "Internal Server Error", body);
    asio::error_code ec;
    asio::write(socket, asio::buffer(resp), ec);   // ← 同步！
} catch (const std::exception& e) {
    LOG_WARN("Connection error: ", e.what());
    std::string body = "{\"code\":500,\"msg\":\"internal server error\"}";
    auto resp = build_error_response(500, "Internal Server Error", body);
    asio::error_code ec;
    asio::write(socket, asio::buffer(resp), ec);   // ← 同步！
}
```

#### 问题

`asio::write(socket, ...)` 是**同步阻塞**写，内部循环 `write_some` 直到所有字节完成。在协程里调它会**独占 io_context 线程**直到写完，期间该线程上其他协程全部冻结。

讽刺的是，同一文件 `write_simple_error`（行 415-422）就是写法正确的版本：

```cpp
asio::awaitable<void> write_simple_error(...) {
    auto resp = build_error_response(status, reason, body);
    asio::error_code ec;
    co_await asio::async_write(
        socket, asio::buffer(resp), asio::redirect_error(asio::use_awaitable, ec));
}
```

catch 块在 `co_await` 调用点用同样的 `async_write` 即可。

#### 触发场景

异常路径才走这里，触发条件：

- socket 操作抛 `system_error`（连接重置、对端 RST、broken pipe 等）
- 协程内任何代码抛未捕获 `std::exception`（DB handler、route handler 内部异常）

这些都是**生产高频路径**。一旦走到 catch：

1. 同步写发送 ~200 字节的错误响应
2. 如果客户端 TCP 窗口满（slow consumer），写阻塞
3. 该 io_context 线程被卡，其他协程排队等待
4. 多个并发异常会同时卡住多个线程，雪崩

#### 生产影响

- **尾延迟尖峰**：P99/P999 在异常爆发时被 sync write 拖高
- **吞吐塌陷**：大量异常时所有 io_context 线程被 sync write 抢占
- **难以排查**：sync write 不进入 epoll，perf 看不到，需要 strace 才能发现

这正是 PERF_REPORT.md 里"MySQL 同步 API 用 asio::post 包装"想避免的反模式。

#### 修复

把 catch 块里的同步 write 改成异步：

```cpp
} catch (const std::system_error& e) {
    LOG_WARN("Connection system_error: ", e.what());
    std::string body = "{\"code\":500,\"msg\":\"connection error\"}";
    auto resp = build_error_response(500, "Internal Server Error", body);
    asio::error_code ec;
    co_await asio::async_write(socket, asio::buffer(resp),
        asio::redirect_error(asio::use_awaitable, ec));
} catch (const std::exception& e) {
    LOG_WARN("Connection error: ", e.what());
    std::string body = "{\"code\":500,\"msg\":\"internal server error\"}";
    auto resp = build_error_response(500, "Internal Server Error", body);
    asio::error_code ec;
    co_await asio::async_write(socket, asio::buffer(resp),
        asio::redirect_error(asio::use_awaitable, ec));
}
```

或者更简洁——抽一个工具函数复用 `write_simple_error`。

#### 验证

压测时故意触发 500 错误（如让 upstream 全部关掉），观察 P99 是否回退、io_context CPU 占用是否异常。

**工作量**：30 分钟。

---

### C3. `http_pool.hpp:451` `read_buffer` 反转

> 📝 **修订（2026-07-11 二次评审）**：严重度 **CRITICAL → MEDIUM**。
>
> 初版认定此条会"导致响应串包"在原理上正确，但**触发条件比初版描述的苛刻**：
> - `read_buffer` 非空要求上游在响应 framing terminator 之后**多吐字节**
> - HTTP/1.1 单请求-单响应模式下，正常上游不会这么做
> - 真实触发场景：上游 bug（罕见）/ body reader 解析边界 case（待验证）/ 抓包/调试干扰（极罕见）
>
> 因此"正常生产场景不触发"的判断成立。但 fix 是一行条件翻转（`> 64*1024` 改 `!empty()`），代价极低，**值得修但不紧急**。归入 P2。
>
> 注意：此条与 H2（64KB 阈值）是同一问题的两个面，必须一起修。

#### 位置

`src/http/http_pool.hpp:448-467`

#### 代码（简化）

```cpp
static bool is_reusable_idle(HttpConn& conn) {
    if (!conn.socket.is_open() || conn.connection_close) return false;
    assert(conn.read_buffer.size() <= 64 * 1024);
    if (!conn.read_buffer.empty()) return true;     // ← 致命：把"残留"判为"可复用"
    /* ... recv(MSG_PEEK) 探测 ... */
}
```

#### 问题

`read_buffer` 是上一轮 body reader 读过 framing terminator 之后**多读出来的字节**（来源：`proxy_forwarder.hpp:220, 239`）。HTTP/1.1 单请求-单响应模式下，正常服务器不会在响应之后多吐字节，所以 `read_buffer` 非空意味着**协议状态错乱**——要么服务端无视 Content-Length 多发了字节，要么连接被双向复用，要么上一轮响应未被完整消费。

当前代码把这些情况判为**可复用**，下次请求 `read_proxy_response` 第 117 行 `buf = std::move(conn.read_buffer)` 会把陈旧字节当作下一个响应的开头解析，造成：

- status line 解析失败 → 502
- 响应串包（陈旧字节恰好构成假响应头）→ **客户端拿到错的 status code 和 body**

#### 修复

详见 `docs/HTTP_POOL_PROBE_REMOVAL_2026-07-11.md` §4-5。**正确做法是移除整个 probe，把 `read_buffer` 非空判定翻转为丢连接**，依赖 `client_session.hpp` 已有的 try-and-retry 路径兜底。

**工作量**：1 小时（净删除 + 翻条件 + 清计数器）。

---

## 3. 🟠 HIGH 问题（特定场景出 bug）

### H1. `response_builder.hpp` HEAD 响应 Content-Length 永远是 0

#### 位置

`src/http/response_builder.hpp:44-97`

#### 代码

```cpp
inline std::string build_downstream_response(
    const HttpContext& ctx, std::string_view method, bool proxy_response) {
    std::string resp = "HTTP/1.1 ";
    resp += std::to_string(ctx.status_code);
    /* ... status text ... */

    bool has_content_type = false;
    if (!ctx.response_headers.empty()) {
        if (proxy_response) {
            std::vector<std::string> filtered = {
                "connection", "keep-alive", "proxy-authenticate",
                "proxy-authorization", "te", "trailer",
                "transfer-encoding", "upgrade"
            };
            add_connection_tokens(ctx.response_headers, filtered);
            filtered.push_back("content-length");          // ← 1. 过滤掉 upstream 的 CL
            for (auto& [k, v] : ctx.response_headers) {
                if (contains_header_name(filtered, k)) continue;
                resp += k + ": " + v + "\r\n";
                /* ... */
            }
        }
        /* ... */
    }
    /* ... */
    bool no_body = http_response_has_no_body(method, ctx.status_code);   // HEAD → true
    /* ... */
    resp += "Content-Length: ";
    resp += no_body ? "0" : std::to_string(ctx.response_body.size());   // ← 2. HEAD 强制 0
    resp += "\r\n\r\n";
    /* ... */
}
```

#### 问题

对于 HEAD 请求走 proxy 路径：

1. 行 67 把 upstream 的 `Content-Length` 头**显式过滤掉**
2. 行 92-93 自己加 `Content-Length: 0`（因为 `no_body=true`）

结果：**所有 HEAD 响应的 Content-Length 永远是 0**。

#### 协议违规

RFC 7230 §3.3.2 + RFC 7231 §4.3.2：

> The HEAD method is identical to GET except that the server MUST NOT send a message body in the response. The server SHOULD send the same header fields in response to a HEAD request as it would have sent if the request had been a GET, except that the payload header fields (Section 3.3) MAY be omitted.

也就是说，HEAD 响应的 `Content-Length` 应当**反映对应 GET 响应的 body 大小**，而不是 0。

#### 生产影响

- **下载工具**：`curl -I`、`wget --spider`、SDK 的 HEAD 探测都依赖 Content-Length 做进度条 / 预分配 / 断点续传决策；CL=0 误导它们认为资源不存在或为空。
- **CDN/缓存层**：部分 CDN 会根据 HEAD 的 CL 决定是否缓存，CL=0 可能被判定为"无效资源"。
- **客户端兼容**：Java HttpClient、Go net/http 在 HEAD CL=0 时会忽略 body，但其他 stricter 的客户端可能拒绝响应。

#### 修复

**方案 A**（推荐）：proxy 路径下不要 filter upstream 的 Content-Length。

```cpp
std::vector<std::string> filtered = {
    "connection", "keep-alive", "proxy-authenticate",
    "proxy-authorization", "te", "trailer",
    "transfer-encoding", "upgrade"
    // 不要再 push "content-length"
};
```

然后行 92-93 改成：

```cpp
bool has_cl = /* 检查 ctx.response_headers 是否已含 Content-Length */;
if (!has_cl) {
    resp += "Content-Length: ";
    resp += no_body ? "0" : std::to_string(ctx.response_body.size());
    resp += "\r\n";
}
```

这样 upstream 透传的 CL 保留，HEAD 时正确反映 GET 大小。

**方案 B**：在 `read_proxy_response` 里把 upstream 的 CL 存到 `HttpContext`，build 时优先使用。

**注意**：方案 A 改完后，HEAD 响应里 upstream 的 CL 可能与 actual body（0 字节）不匹配。这其实是合规的——HEAD 响应本身就不该有 body，CL 描述的是"如果这是 GET 会返回多少字节"。

#### 验证

`curl -I http://gateway/zebra-config/...` 应当返回 upstream 资源的真实大小，而不是 0。

**工作量**：1-2 小时（需要理解整个 proxy 响应链路 + 写测试）。

---

### H2. `http_pool.hpp:178-186, 294-307` 64KB 魔法数阈值

#### 位置

`src/http/http_pool.hpp:178-186`（acquire 路径）和 `src/http/http_pool.hpp:294-307`（release 路径）

#### 代码

```cpp
// acquire
if (idle_conn->read_buffer.size() > 64 * 1024) {
    asio::error_code ec;
    idle_conn->socket.close(ec);
    /* shard accounting decrement */
    idle_conn.reset();
    continue;
}

// release
if (conn->read_buffer.size() > 64 * 1024) {
    asio::error_code ec;
    conn->socket.close(ec);
    /* ... */
    state->released_closed.fetch_add(1, std::memory_order_relaxed);
    return;
}
```

#### 问题

64 KB 这个阈值的来源**不可考**——既不在 RFC 里，也没有任何注释说明。从协议角度，`read_buffer` 在 release 时**应当永远为空**（见 C3 分析），任何残留都意味着协议错乱。1 字节和 64 KB **没有本质区别**——都是同一类异常的指示。

阈值化这个判断相当于："协议错乱只要小于 64KB 就容忍"。这是错的——少量残留和大量残留都是残留，复用它都会导致响应串包，只是串包的严重程度不同。

#### 修复

把 `> 64 * 1024` 改成 `!empty()`：

```cpp
// acquire
if (!idle_conn->read_buffer.empty()) {
    /* close + drop */
}

// release
if (!conn->read_buffer.empty()) {
    /* close + drop */
}
```

同时去掉 `http_pool.hpp:450` 的 `assert(conn.read_buffer.size() <= 64 * 1024);`，改为 `assert(conn.read_buffer.empty());`。

#### 关联

这是 C3 的姊妹问题：C3 是 probe 内部的判定，H2 是 acquire/release 的前置过滤。两者必须一起改。

**工作量**：30 分钟（含改测试）。

---

### H3. `http_server.hpp:77-79` accept 持续失败时死循环

#### 位置

`src/http/http_server.hpp:61-82`

#### 代码

```cpp
asio::awaitable<void> start() {
    LOG_INFO("HTTP server listening on port ", acceptor_.local_endpoint().port());
    while (state_->running) {
        try {
            auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
            if (!state_->running) break;
            auto session = std::make_shared<ClientSession>(state_);
            co_spawn(ioc_, session->run(std::move(socket)),
                [session](std::exception_ptr ep) {
                    if (!ep) return;
                    try { std::rethrow_exception(ep); }
                    catch (const std::exception& e) {
                        LOG_WARN("Client session failed: ", e.what());
                    }
                });
        } catch (const std::exception& e) {
            if (state_->running) LOG_ERROR("Accept error: ", e.what());
            // ← 没有 backoff，直接下一轮
        }
    }
    LOG_INFO("HTTP server stopped");
}
```

#### 问题

如果 `async_accept` 持续抛异常（典型场景：fd 耗尽 `EMFILE` / `ENFILE` / 内核参数异常 / 网络接口 down / acceptor 已关闭），这是一个**无 backoff 的紧密 while 循环**：

1. `async_accept` 在 epoll 里立刻失败返回
2. catch 块只 LOG_ERROR
3. 立刻下一轮 `async_accept`
4. 单核 CPU 100%，日志风暴

更糟的是，如果 `state_->running` 没被外部置 false（例如 SIGINT 没被监听到），循环永远不会退出。

#### 触发场景

- **fd 泄漏**：某个路径忘了 close socket，fd 持续增长直到 ulimit -n 触顶（默认 1024 或 65535）。每个新 accept 都 EMFILE。
- **process fd limit**：cgroup 限制、systemd LimitNOFILE
- **网络异常**：物理网卡 down、iptables 规则异常

#### 生产影响

- CPU 100% 持续
- 日志磁盘被打爆（每秒数千条 Accept error）
- 雪崩：主进程忙于 spin，无法响应 SIGTERM，需要 SIGKILL

#### 修复

加指数退避：

```cpp
asio::awaitable<void> start() {
    LOG_INFO("HTTP server listening on port ", acceptor_.local_endpoint().port());
    auto ex = co_await asio::this_coro::executor;
    int backoff_ms = 1;
    constexpr int kMaxBackoffMs = 1000;
    
    while (state_->running) {
        try {
            auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
            backoff_ms = 1;   // 成功了就重置
            if (!state_->running) break;
            auto session = std::make_shared<ClientSession>(state_);
            co_spawn(ioc_, session->run(std::move(socket)),
                [session](std::exception_ptr ep) {
                    if (!ep) return;
                    try { std::rethrow_exception(ep); }
                    catch (const std::exception& e) {
                        LOG_WARN("Client session failed: ", e.what());
                    }
                });
        } catch (const std::system_error& e) {
            if (!state_->running) break;
            LOG_ERROR("Accept error: ", e.what(),
                ", backing off ", backoff_ms, "ms");
            asio::steady_timer timer(ex, std::chrono::milliseconds(backoff_ms));
            co_await timer.async_wait(asio::use_awaitable);
            backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
        }
    }
    LOG_INFO("HTTP server stopped");
}
```

更彻底的方案：连续 N 次失败后给监控发告警 / 触发自恢复（如重新创建 acceptor）。

#### 验证

测试环境 `ulimit -n 50` 后启动 server，连接几个客户端触发 fd 耗尽，观察 CPU 是否合理（应低，因 backoff），日志频率应被限制。

**工作量**：1 小时。

---

### H4. `proxy_forwarder.hpp` / `client_session.hpp` 缺少 CRLF 注入检查 → HTTP 请求走私

#### 位置

- `src/http/client_session.hpp:101-106`（header 解析后无校验）
- `src/http/proxy_forwarder.hpp:56-110` `build_proxy_request` 拼接时无校验
- `src/http/http_protocol.hpp:176-199` `parse_header_*` 系列不验证值

#### 问题

picohttpparser 解析 HTTP 请求时，**对 header value 不验证是否含 `\r\n`**。它只在 header name 里禁止 `:`，对 value 几乎不约束。

当前流程：

1. 客户端发请求，picohttpparser 解析得到 N 个 `(name, value)` 对
2. `client_session.hpp:101-106` 直接把 name/value 拷进 `ctx.headers`
3. `proxy_forwarder.hpp:87-101` 把这些 header 原样拼到 upstream 请求里：
   ```cpp
   forward_req += k + ": " + v + "\r\n";
   ```

如果客户端发的 header value 含 `\r\n`（恶意构造）：

```
POST /zebra-config/api/foo HTTP/1.1\r\n
Host: gateway\r\n
X-Evil: foo\r\n
Content-Length: 0\r\n
\r\n
POST /admin/delete-all HTTP/1.1\r\n
Host: gateway\r\n
Content-Length: 100\r\n
\r\n
{"dangerous":"payload"}
```

picohttpparser 在第一次 `phr_parse_request` 时停在第一个 `\r\n\r\n`，但 header 列表里 `X-Evil` 的 value 可能是 `foo\r\nContent-Length: 0\r\n\r\nPOST /admin/delete-all HTTP/1.1\r\nHost: gateway\r\nContent-Length: 100\r\n\r\n{"dangerous":"payload"}`（具体取决于 picohttpparser 对 value 的解析行为——实测需要验证）。

即便 picohttpparser 严格点拒绝 value 里的 `\r\n`，**header name** 和 **value 里的 `\0`** 也可能被忽视。任何一种遗漏都会导致请求走私。

#### 生产影响

- **upstream 看到的请求数 != gateway 看到的请求数**：第二个被注入的请求绕过 gateway 的鉴权 / 限流
- **请求串接**：A 用户的请求体被附加到 B 用户的请求里
- **典型 CVE 类型**：CVE-2018-8007、CVE-2019-12459 等都是这类

参考：
- https://portswigger.net/web-security/request-smuggling
- https://github.com/envoyproxy/envoy/security/advisories/GHSA-...（envoy 历史漏洞）

#### 修复

在 `client_session.hpp:101-106` 解析 header 后立即校验：

```cpp
auto validate_header_value = [](std::string_view v) -> bool {
    for (unsigned char c : v) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
    }
    return true;
};

for (size_t i = 0; i < num_headers; ++i) {
    std::string_view name(headers[i].name, headers[i].name_len);
    std::string_view value(headers[i].value, headers[i].value_len);
    if (!validate_header_value(name) || !validate_header_value(value)) {
        LOG_WARN("Reject header with CR/LF/NUL: name_len=", name.size(),
            ", value_len=", value.size());
        co_await write_simple_error(socket, 400, "Bad Request",
            "{\"code\":400,\"msg\":\"invalid header\"}");
        co_return;
    }
    update_header_state(name, value, request_header_state);
    ctx.headers.emplace_back(std::string(name), std::string(value));
}
```

更严格的话应当拒绝所有控制字符（< 0x20），但 CR/LF/NUL 是必须的最低门槛。

#### 验证

新增测试：构造含 `\r\n` 的 header value 发给 server，应当收到 400。

**工作量**：1 小时（含 fuzz 测试）。

---

### H5. HttpPool probe 的全部设计缺陷

详见专题文档 `docs/HTTP_POOL_PROBE_REMOVAL_2026-07-11.md`。摘要：

- §2：`recv(MSG_PEEK)` 在协程里同步执行 syscall，劣化调度延迟
- §3：`non_blocking` 直接操作绕过 Asio，破坏内部状态
- §4.1：`read_buffer.empty()` 判定反转（见本文 C3）
- §4.3：MSG_PEEK 看不到对端 FIN，半关闭假阴性

修复：净删除 probe，纯 try-and-retry。

---

## 4. 🟡 MEDIUM 问题（设计 / 维护性）

### M1. `header_iequals` 重复实现

#### 位置

- `src/http/http_protocol.hpp:85-93` `header_iequals(std::string_view, std::string_view)`
- `src/http/http_context.hpp:7-15` `http_header_iequals(std::string_view, std::string_view)`

两份完全相同的代码。`http_context.hpp` 那份只被同文件的 `get_header` 用一次，其它地方都用 `http_protocol.hpp` 那份。

#### 修复

删除 `http_context.hpp` 的版本，`get_header` 改用 `http_protocol.hpp` 的。需要 `#include "http_protocol.hpp"`。

**工作量**：10 分钟。

---

### M2. `http_context.hpp:7-15` 缩进错乱

```cpp
inline bool http_header_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb)) return false;     // ← 多一层缩进
        }                                                               // ← 同上
        return true;
    }                                                                   // ← brace 不平衡观感
```

实际 brace 是配对的，但缩进让人误以为不配对。clang-format 没跑过这文件。

如果 M1 修了，这段代码会被删掉。

**工作量**：5 分钟（clang-format -i）。

---

### M3. `upstream_manager.hpp:47` `pools_.at(svc)` 假设强一致

#### 位置

`src/http/upstream_manager.hpp:39-49`

```cpp
std::optional<RouteResult> route(const std::string& path) {
    std::shared_lock lock(mtx_);
    if (path.empty() || path[0] != '/') return std::nullopt;
    auto end = path.find('/', 1);
    auto svc = (end == std::string::npos) ? path.substr(1) : path.substr(1, end - 1);
    auto it = upstreams_.find(svc);
    if (it == upstreams_.end()) return std::nullopt;
    std::string upstream_path = (end == std::string::npos) ? "/" : path.substr(end);
    auto pool = pools_.at(svc);    // ← 假设 pools_[svc] 一定存在
    return RouteResult{it->second, pool, std::move(upstream_path)};
}
```

#### 问题

`upstreams_` 和 `pools_` 是两个独立 map，由 `add_upstream` 和 `reload` 维护一致性。但**没有编译期保证**：

```cpp
void add_upstream(const std::string& name, std::string host, int port, ...) {
    upstreams_[name] = {std::move(host), port};   // step 1
    pools_[name] = std::make_shared<HttpPool>(ioc_, std::move(pool_cfg));   // step 2
}
```

如果 step 1 之后、step 2 之前有任何异常（如 `make_shared` 抛 `std::bad_alloc`），`upstreams_[name]` 已写入但 `pools_[name]` 没有，下次 `route()` 命中 `pools_.at(svc)` 抛 `std::out_of_range`。

#### 修复

改用 `find`：

```cpp
auto pool_it = pools_.find(svc);
if (pool_it == pools_.end()) {
    LOG_ERROR("upstream '", svc, "' exists in upstreams_ but not pools_ (invariant violated)");
    return std::nullopt;
}
auto pool = pool_it->second;
```

更彻底：把 `upstreams_` 和 `pools_` 合并成 `std::unordered_map<std::string, UpstreamEntry>`，一个 entry 含 host/port/pool。

**工作量**：20 分钟。

---

### M4. `upstream_manager.hpp:67` 配置错误静默吞掉

#### 位置

`src/http/upstream_manager.hpp:66-67`

```cpp
int port = 0;
try { port = std::stoi(port_str); } catch (...) { continue; }
```

`[upstream]` 段里 `service = host:badport` 会被静默跳过。生产配错时排查困难。

#### 修复

```cpp
try {
    port = std::stoi(port_str);
} catch (const std::exception& e) {
    LOG_WARN("Skipping upstream '", name, "': invalid port '", port_str, "' (", e.what(), ")");
    continue;
}
```

**工作量**：5 分钟。

---

### M5. `client_session.hpp:119` reference alias 容易踩坑

#### 位置

`src/http/client_session.hpp:91, 119, 155, 180`

```cpp
std::string body_buffer = client_preread.substr(pret);   // 行 91
/* ... */
std::string& preread = body_buffer;                       // 行 119，别名！
/* ... */
auto body_result = co_await read_chunked_body(socket, preread, kMaxBodySize, client_timeout);
/* ... body reader 内部修改 preread ... */
client_preread = std::move(preread);                      // 行 155，等于 move body_buffer
```

#### 问题

`preread` 是 `body_buffer` 的**引用别名**，body reader 内部把多读出来的字节填进 `preread`（实际是 `body_buffer`），然后 `client_preread = std::move(preread)` 把 body_buffer move 出来。

逻辑正确，但可读性差：

- 看代码的人需要知道 `preread` 是别名
- 重构时极易把 `preread` 当成独立变量
- move 之后 body_buffer 处于 valid-but-unspecified 状态，下次循环开始处 line 91 重新赋值所以 OK，但容易踩坑

#### 修复

直接用 `body_buffer`，去掉别名：

```cpp
auto body_result = co_await read_chunked_body(socket, body_buffer, kMaxBodySize, client_timeout);
/* ... */
client_preread = std::move(body_buffer);
```

或者用更清晰的两段式：

```cpp
std::string leftover;
auto body_result = co_await read_chunked_body(socket, body_buffer, leftover, kMaxBodySize, ...);
// body_buffer 消费完，leftover 留作下次
client_preread = std::move(leftover);
```

后者更清晰但需要改 body reader 接口。

**工作量**：30 分钟（含测试）。

---

### M6. `http_body_reader.hpp:165-170` chunked 溢出检查依赖顺序

#### 位置

`src/http/http_body_reader.hpp:165-170`

```cpp
if (*chunk_size > max_body_size ||
    result.body.size() > max_body_size - *chunk_size) {
    result.status = BodyReadStatus::TooLarge;
    result.body.clear();
    co_return result;
}
```

#### 问题

C++ 的 `||` 是短路求值：第一个条件为真时不计算第二个。但**逻辑上**第二个条件 `max_body_size - *chunk_size` 依赖 `*chunk_size <= max_body_size`，否则 size_t 减法下溢。

当前代码顺序正确（先 check 再减），但**重构脆弱**：

- 任何人调换条件顺序 → 立刻溢出
- 任何人拆成两个独立 if 而不重新组织 → 立刻溢出

#### 修复

改成不依赖顺序的写法：

```cpp
if (*chunk_size > max_body_size) {
    result.status = BodyReadStatus::TooLarge;
    result.body.clear();
    co_return result;
}
if (result.body.size() > max_body_size - *chunk_size) {  // 现在 *chunk_size <= max_body_size 保证不下溢
    result.status = BodyReadStatus::TooLarge;
    result.body.clear();
    co_return result;
}
```

或者更直接：

```cpp
if (*chunk_size > max_body_size - result.body.size()) {  // body.size() <= max 否则之前就 TooLarge 了
    /* TooLarge */
}
```

后者依赖 "body.size() <= max_body_size 是 loop 不变量"——需要在循环开头加一次 check。

**工作量**：15 分钟。

---

### M7. `client_session.hpp:388-390` 硬编码状态码判断

#### 位置

`src/http/client_session.hpp:388-390`

```cpp
if (ctx.status_code == 400 || ctx.status_code == 408 || ctx.status_code == 413) {
    co_return;
}
```

#### 问题

"哪些状态码要主动关连接"被硬编码在 session 里。语义上：

- 400 / 408 / 413：客户端请求有问题，连接不可复用
- 503：服务端没资源（pool 满），连接**可以**复用——所以不在列表里
- 其他 4xx / 5xx：根据 `response_builder.hpp:86-89` 已经发了 `Connection: close`，但 session 这里又判一遍

逻辑分散在两个地方（response_builder 加 Connection: close、session 决定 co_return），容易出现不一致。

#### 修复

统一信号源。两个选择：

**方案 A**：让 `build_downstream_response` 返回一个 `bool should_close`：

```cpp
struct DownstreamResponse {
    std::string data;
    bool should_close;
};
DownstreamResponse build_downstream_response(...);
```

session 根据这个 bool 决定是否 `co_return`。

**方案 B**：session 自己解析响应里的 `Connection: close`：

```cpp
auto client_conn = ctx.get_header("Connection");   // 但这是请求头，不是响应头
// 需要单独保存响应头里的 Connection
```

方案 A 更清晰。

**工作量**：30 分钟。

---

### M8. `http_pool.hpp` Shard 用裸指针管理 active set

#### 位置

`src/http/http_pool.hpp:46-54, 124-143`

```cpp
struct Shard {
    std::mutex mtx;
    std::deque<HttpConn> idle;
    std::unordered_set<HttpConn*> active;   // ← 裸指针
    /* ... */
};

void track_active(HttpConn* conn) {
    if (!conn) return;
    auto& shard = state_->shards[conn->shard_idx];
    std::lock_guard lock(shard.mtx);
    shard.active.insert(conn);
}
```

#### 问题

HttpConn 的实际所有权在调用方（`acquire` 返回的 `unique_ptr`、`ConnGuard` 持有的 unique_ptr）。pool 的 `active` set 只是"观察"。

当前调用约定下安全（`release` / `release_bad` 必须在 `unique_ptr` 析构前调用），但**没有 RAII 强制**。如果有人写：

```cpp
auto conn = co_await pool->acquire(...);
// 忘了 release / release_bad
return;  // ← unique_ptr 析构，HttpConn 消失，active 里指针变野
```

下次 pool 遍历 `active` 时就是 use-after-free。

`ConnGuard` (`proxy_forwarder.hpp:28-54`) 是为解决这个设计的，但只在 proxy 路径用。本地 handler 路径根本不拿 conn，所以这个风险目前是潜在的。

#### 修复

要么：

1. 把 `unique_ptr<HttpConn>` 本身存进 pool，调用方拿 raw observer pointer
2. 把 active set 改成 `std::unordered_set<std::weak_ptr<HttpConn>>`，pool 持有弱引用
3. 强制 ConnGuard 在所有路径使用，禁止直接持有 `unique_ptr<HttpConn>`

**工作量**：1-2 小时（方案 3 改动最小）。

---

## 5. 🟢 LOW 问题（风格 / 微优化）

### L1. `client_session.hpp:209-220` 函数内 static atomic

```cpp
{
    static std::atomic<int64_t> last_log_ms{0};
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(...).count();
    int64_t last = last_log_ms.load(...);
    if (now - last > 30000 && last_log_ms.compare_exchange_strong(last, now)) {
        LOG_WARN("Security check rejected: ...");
    }
}
```

函数级 static 跨 session 共享，重启才能清零，且对单元测试不友好（测试间状态泄漏）。挪到 `HttpServerState`。

**工作量**：10 分钟。

---

### L2. `http_protocol.hpp:120-136` `parse_header_list_token` O(n×m)

每个 token 都和 `expected` 比较。复杂度可接受（HTTP header token 数小），但单次遍历就能完成。微优化。

**工作量**：5 分钟（非必须）。

---

### L3. `http_pool.hpp:46` `kShards = 16` 写死

16 个分片对 32 核 / 64 核机器可能不够。理论上应该 `min(hardware_concurrency() * 2, 64)` 之类。当前不是瓶颈。

**工作量**：10 分钟。

---

### L4. `proxy_forwarder.hpp:168-169` HTTP 版本判定只看 1.0 / 1.1

```cpp
int upstream_minor_version = status_line.rfind("HTTP/1.0", 0) == 0 ? 0 : 1;
```

HTTP/0.9 走 else 分支被当 1.1，影响 keep-alive 默认值。理论瑕疵，实际不会遇到（upstream 都是 1.1+）。

**工作量**：5 分钟（加注释）。

---

### L5. `http_body_reader.hpp:54-57` max_line_size 检测位置

```cpp
while (true) {
    auto pos = buffer.find("\r\n");
    if (pos != std::string::npos) { /* return */ }
    if (buffer.size() > max_line_size) { /* InvalidChunk */ }
    /* 读更多 */
}
```

`buffer.size() > max_line_size` 在 `find` 之后才检查，意味着已经存了 max+1 字节才报错。内存峰值略高。低危。

**工作量**：5 分钟（改成 `>=` 或调整顺序）。

---

## 6. ✅ 设计良好之处

为了平衡，下面这些是**正确且做得好**的：

### S1. `http_io.hpp` 用 `parallel_group` + `wait_for_one` 实现 timeout

`src/http/http_io.hpp:54-113`

```cpp
auto [order, read_ec, n, timer_ec] = co_await asio::experimental::make_parallel_group(
    [&](auto token) { return sock.async_read_some(asio::buffer(buf, size), token); },
    [&](auto token) { return timer.async_wait(token); }
).async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);
```

这是 Asio 协程下处理 timeout 的标准做法：

- `wait_for_one` 保证第一个完成的操作完成后取消另一个
- Asio 的 cancellation propagation 正确处理 socket 操作
- `order[0]` 判断哪个先完成

避免了手写 `async_compose` + timer + cancel 的复杂度。

### S2. `http_pool.hpp` 分片锁（kShards=16）

`src/http/http_pool.hpp:46-54`

v3.5 的核心优化。把单一 mutex 拆成 16 个，配合 `pick_shard()` round-robin（thread_local 计数器），显著降低锁竞争。memory `httppool-perf-optimization-2026-07.md` 记录：futex CPU 从 41% 降到 5%，RPS +37%。

### S3. `upstream_manager.hpp` 用 `shared_ptr<HttpPool>` 做热重载

`src/http/upstream_manager.hpp:53-96`

reload 时旧 pool 被 in-flight 请求的 `shared_ptr` 续命，不会 use-after-free。in-flight 请求完成后旧 pool 析构 shutdown。设计正确。

### S4. `http_protocol.hpp:246-257` 日志脱敏

```cpp
inline std::string sanitize_header_value(std::string_view key, std::string_view value) {
    if (header_iequals(key, "authorization") || header_iequals(key, "cookie") ||
        header_iequals(key, "set-cookie")) {
        return "<redacted len=" + std::to_string(value.size()) + ">";
    }
    /* ... truncation ... */
}
```

authorization / cookie / set-cookie 在日志里被脱敏，避免凭证泄漏到日志文件。生产级实践。

### S5. 多层 body size cap

| 层 | 限制 | 位置 |
|:---|:-----|:-----|
| HTTP header size | 64 KB | `http_protocol.hpp:13` `kMaxHeaderSize` |
| Body size | 10 MB | `http_protocol.hpp:14` `kMaxBodySize` |
| picohttpparser headers | 128 | `client_session.hpp:75` |
| HttpPool max_body_size | 10 MB（默认） | `http_pool.hpp:23` |

多层防御，单点突破不会无限增长。

### S6. `proxy_forwarder.hpp:42-54` `ConnGuard` RAII

```cpp
struct ConnGuard {
    /* ... */
    ~ConnGuard() {
        if (!conn_) return;
        HttpPool::untrack_active(pool_state_, conn_.get());
        if (good_) HttpPool::release(pool_state_, std::move(conn_));
        else HttpPool::release_bad(pool_state_, std::move(conn_));
    }
    void set_bad() { good_ = false; }
};
```

异常路径自动走 `release_bad`，保证连接状态一致。这是解决 M8 隐患的正确模式（虽然只在 proxy 路径用）。

### S7. `http_pool.hpp:367-386` CAS 计数

```cpp
static bool try_increment_counter(std::atomic<size_t>& counter, size_t limit) {
    size_t cur = counter.load(std::memory_order_relaxed);
    while (cur < limit) {
        if (counter.compare_exchange_weak(cur, cur + 1, ...)) return true;
    }
    return false;
}
```

全局计数（total_count / in_flight_count）用 lock-free CAS，避免在分片锁之上再加全局锁。内存序正确（acq_rel on success, relaxed on failure）。

### S8. `http_protocol.hpp:138-161` 严格 framing 检查

```cpp
if (header_iequals(k, "content-length")) {
    auto parsed = parse_decimal_size(v);
    if (!parsed) state.invalid_content_length = true;
    else if (state.content_length && *state.content_length != *parsed) {
        state.duplicate_content_length = true;   // ← 多个不同 CL → 拒绝
    }
    /* ... */
}
```

符合 RFC 7230 §3.3.3：
- 重复且不同的 Content-Length → 拒绝
- Transfer-Encoding 非 chunked → 拒绝
- 解析失败的 Content-Length → 拒绝

### S9. `http_body_reader.hpp:95-127` Content-Length body reader 边界处理

```cpp
if (content_length > max_body_size) {
    // 不读，直接 TooLarge
    result.status = BodyReadStatus::TooLarge;
    co_return result;
}
if (preread.size() >= content_length) {
    // 预读已经够，直接切
    result.body = preread.substr(0, content_length);
    preread.erase(0, content_length);
    co_return result;
}
```

边界条件考虑周到：oversized 提前拒绝、preread 已够直接切片、不足才发起 socket 读。

### S10. `response_builder.hpp:31-42` `build_error_response` 简单正确

错误响应永远是 `Connection: close` + 固定 JSON 格式，避免错误响应里的状态被污染。

---

## 7. 优先级矩阵

| 优先级 | 编号 | 改动 | 预计工作量 |
|:------:|:----:|:-----|:----------:|
| **P0** | **C1** | `response.hpp` JSON escape | **1h** |
| **P1** | H4 | header CRLF/NUL 注入检查（建议先 fuzz picohttpparser 验证可利用性） | 1h |
| **P1** | H1 | HEAD Content-Length 透传 | 1-2h |
| **P1** | H3 | accept backoff | 1h |
| **P2** | C2 ↓ | `client_session.hpp` catch 块改 async write（**修订降级**） | 30min |
| **P2** | C3 + H2 ↓ | read_buffer 反转 + probe 移除（**修订降级**，详见 `HTTP_POOL_PROBE_REMOVAL_2026-07-11.md`） | 1h |
| **P2** | M1 | 删除重复 `header_iequals` | 10min |
| **P2** | M2 | `http_context.hpp` clang-format | 5min |
| **P2** | M3 | `pools_.at` 改 find | 20min |
| **P2** | M4 | upstream reload 加日志 | 5min |
| **P2** | M5 | 删除 `preread` 别名 | 30min |
| **P2** | M6 | chunked 溢出检查改顺序无关 | 15min |
| **P2** | M7 | 统一 connection-close 决策 | 30min |
| **P2** | M8 | HttpConn 生命周期 RAII | 1-2h |
| **P3** | L1-L5 | 各种小优化 | 按需 |

**总工作量**：P0 约 1h、P1 约 3-4h、P2 约 5-6h、P3 按需。

---

## 8. 推荐实施顺序

按"风险下降曲线"和"改动独立性"排序（**已按二次评审调整**）：

### 阶段 1（本周内，独立 PR）

1. **C1**：`response.hpp` JSON escape —— **唯一 P0**，最先做
2. **M1 + M2 + M4**：超小改动批量（删重复 `header_iequals`、clang-format、reload 加日志）

### 阶段 2（下周）

3. **H3**：accept backoff，独立改动
4. **H4**：header CRLF 检查，独立改动但需要先 fuzz picohttpparser 确认可利用性
5. **H1**：HEAD Content-Length，涉及 proxy 响应链路，需要小心改

### 阶段 3（重构窗口）

6. **C2 + C3 + H2 + M5 + M7 + M8**：HttpPool probe 移除 + read_buffer 反转 + catch async + 别名清理 + connection-close 决策统一 + HttpConn 生命周期 —— 这些有耦合，建议一起想清楚再动
7. **M3**：`pools_.at` 改 find
8. **M6**：chunked 溢出检查顺手改

### 阶段 4（按需）

9. **L1-L5**：低优先级，闲暇时做

---

## 9. 验证策略

### 9.1 单元测试

每个修复都应有对应单测：

| 修复 | 测试名 | 验证点 |
|:-----|:-------|:-------|
| C1 | `Response.EscapesSpecialChars` | `"`, `\`, `\n` 被转义 |
| C2 | `ClientSession.NoSyncWriteInCatch` | 用 mock socket 验证 catch 路径用 async |
| C3 | `HttpPool.DropsIdleWithResidualBuffer` | release 后 acquire 不复用残留连接 |
| H1 | `ResponseBuilder.PreservesUpstreamContentLengthForHead` | HEAD 响应保留 upstream CL |
| H3 | `HttpServer.BackoffsOnAcceptFailure` | 模拟 EMFILE，验证退避 |
| H4 | `ClientSession.RejectsCrlfInHeaderValue` | 含 `\r\n` 的 header → 400 |

### 9.2 集成测试

- 现有 `tests/test_proxy_framing.cpp` 扩展：覆盖 HEAD、204、304、chunked trailer
- 现有 `tests/test_http_pool.cpp` 扩展：覆盖 read_buffer 残留场景

### 9.3 压测回归

每个 P0/P1 修复后跑：

```bash
# 同机基线
wrk -t15 -c50 -d60s --latency -s /tmp/post.lua \
  http://127.0.0.1:8081/zebra-config/...

# strace 看 syscall 分布
strace -f -c -e trace=futex,ioctl,recvfrom,sendto,write,read \
  -p $(pgrep -f "build/server") 2>&1 | tail -30
```

预期：所有修复后 RPS 不下降（应当略升），P99 不变差。

### 9.4 ASan/UBSan

```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
      -B build_asan -S .
cmake --build build_asan
ctest --test-dir build_asan --output-on-failure
```

每个 PR 合并前必须 ASan/UBSan 干净。

---

## 10. 附录

### 10.1 文件 × 行号索引

| 文件 | 关键行号 | 说明 |
|:-----|:--------:|:-----|
| `response.hpp` | 15-22 | C1: json_resp 无转义 |
| `http_context.hpp` | 7-15 | M1+M2: 重复实现 + 缩进 |
| `http_server.hpp` | 61-82 | H3: accept 死循环 |
| `http_server.hpp` | 36-88 | S(无): 整体设计 OK |
| `json_transform.hpp` | 9-90 | S(无): 实现合理 |
| `response_builder.hpp` | 44-97 | H1: HEAD CL=0 |
| `response_builder.hpp` | 31-42 | S10: error response 设计 |
| `http_io.hpp` | 54-113 | S1: parallel_group 用法 |
| `upstream_manager.hpp` | 39-49 | M3: pools_.at |
| `upstream_manager.hpp` | 53-96 | S3: 热重载设计 |
| `upstream_manager.hpp` | 66-67 | M4: 静默吞错 |
| `http_body_reader.hpp` | 54-57 | L5: max_line_size 检测位置 |
| `http_body_reader.hpp` | 95-127 | S9: CL body reader 边界 |
| `http_body_reader.hpp` | 165-170 | M6: 溢出检查顺序 |
| `http_protocol.hpp` | 85-93 | M1: 重复 header_iequals |
| `http_protocol.hpp` | 138-161 | S8: framing 严格 |
| `http_protocol.hpp` | 246-257 | S4: 日志脱敏 |
| `proxy_forwarder.hpp` | 28-54 | S6: ConnGuard RAII |
| `proxy_forwarder.hpp` | 56-110 | H4: build_proxy_request 无 CRLF 检查 |
| `proxy_forwarder.hpp` | 168-169 | L4: HTTP 版本判定 |
| `proxy_forwarder.hpp` | 220, 239 | C3 来源：read_buffer 写入 |
| `client_session.hpp` | 75 | (无): headers[128] 栈分配 |
| `client_session.hpp` | 101-106 | H4: header 解析无 CRLF 校验 |
| `client_session.hpp` | 119 | M5: preread 别名 |
| `client_session.hpp` | 209-220 | L1: 函数内 static atomic |
| `client_session.hpp` | 243-348 | S(无): retry 路径设计正确 |
| `client_session.hpp` | 388-390 | M7: 硬编码状态码 |
| `client_session.hpp` | 399-411 | C2: 协程内同步 write |
| `http_pool.hpp` | 46-54 | S2: 分片锁；M8: 裸指针 |
| `http_pool.hpp` | 178-186, 294-307 | H2: 64KB 阈值 |
| `http_pool.hpp` | 367-386 | S7: CAS 计数 |
| `http_pool.hpp` | 448-467 | C3+H5: probe 全套问题 |

### 10.2 风险类别覆盖矩阵

| 文件 | 协议合规 | 协程一致性 | 并发安全 | 内存安全 | 注入防御 | 错误处理 |
|:-----|:-------:|:---------:|:-------:|:-------:|:-------:|:-------:|
| response.hpp | — | — | — | — | 🔴C1 | — |
| http_context.hpp | — | — | — | — | — | — |
| http_server.hpp | — | — | — | — | — | 🟠H3 |
| json_transform.hpp | — | — | — | — | — | — |
| response_builder.hpp | 🟠H1 | — | — | — | — | — |
| http_io.hpp | — | ✅ | — | — | — | — |
| upstream_manager.hpp | — | — | 🟡M3 | — | — | 🟡M4 |
| http_body_reader.hpp | ✅ | ✅ | — | 🟡M6 | — | ✅ |
| http_protocol.hpp | ✅ | — | — | ✅ | 🟠H4* | — |
| proxy_forwarder.hpp | 🟡L4 | ✅ | — | — | 🟠H4 | ✅ |
| client_session.hpp | — | 🔴C2 | — | — | 🟠H4 | 🟡M7 |
| http_pool.hpp | 🔴C3 | 🟠H5 | ✅ | 🟡M8 | — | ✅ |

\* H4 涉及多文件，根源在 parse 阶段（http_protocol.hpp）+ build 阶段（proxy_forwarder.hpp）都没校验。

### 10.3 协议合规参考

- **RFC 7230**：HTTP/1.1 message syntax and routing（framing、Content-Length、Transfer-Encoding、hop-by-hop）
- **RFC 7231**：HTTP/1.1 semantics and content（方法语义、状态码、HEAD/GET 等价性）
- **RFC 8259**：JSON（字符串转义规则 §7）
- **CVE-2018-8007** 等：HTTP request smuggling 历史漏洞

### 10.4 工具建议

- **clang-format**：所有 .hpp 跑一遍（M1+M2 修复时顺手）
- **clang-tidy**：开启 `bugprone-*, cert-*, security-*` 检查，能发现 C1 / M5 之类
- **cppcheck**：补充静态分析
- **ASan/UBSan**：CI 必跑
- **fuzzing**：对 `phr_parse_request` + `parse_header_*` 链路做 libfuzzer，发现 H4 类问题

---

## 11. 结论

`src/http/` **整体架构是扎实的**——质量评价 **中上**。主要支撑点（分片锁、热重载、parallel_group 超时、RAII、日志脱敏、CAS 计数）都符合现代 Asio 协程最佳实践。这反映了 v3.x 系列（特别是 v3.5）的迭代打磨。

**二次评审后的修正结论**（详见 §0.5）：

1. **真正紧急的只剩 C1 一条**（`response.hpp` JSON 无转义）。MySQL 报错含 `"` 时 100% 触发，且 `app/routes.cpp` 多处已在使用。**这一条任何"中上"代码库都不该有**，是当前最值得 P0 修复的问题。
2. **C2、C3 在二次评审后降为 MEDIUM**：catch 同步 write 是反模式但触发路径窄；read_buffer 反转只在异常上游行为下触发。两者仍应修，但不是 P0。
3. **H1、H3、H4 三条 HIGH 是合规性问题**：HEAD Content-Length 违反 RFC、accept 无 backoff 在 fd 耗尽时雪崩、潜在 CRLF 注入——都属于"上线越晚越危险"的合规性问题，应排进 P1。

**修复路径清晰**：本周内做 C1（P0，1h），下周做 P1（H1/H3/H4，半天），重构窗口统一处理 P2 的生命周期/路由/状态码决策。

**对前一轮外部 review 的回应**：

外部 review 给出的"整体中上"判断本文档完全同意。但其对 4 个问题的定级偏低（read_buffer 反转叫"低、正常场景不触发"），并漏掉了 C1/H1/H3/H4 四条真问题。本结论介于两份 review 之间：架构没问题、C1 必须立即修、其余按 P1/P2 推进。

---

## 12. 第三次补充审计 — 架构设计 / 并发逻辑 / 资源安全（2026-07-11 补充）

> 来源：在移除 idle probe 后，针对连接池架构、并发模型、资源管理进行的深层次审计。
> 共发现 12 个问题，按严重程度分为三等。

### 🔴 致命 / 严重（必须修复，否则存在线上故障风险）

#### S1. 连接未按目标端点（host:port）隔离，存在跨业务串请求风险

**代码位置：** `src/http/http_pool.hpp:32-44`, `src/http/http_pool.hpp:146`

**问题：** `acquire(host, port)` 接收目标地址参数，但 `HttpConn` 无 `host`/`port` 成员字段。从 idle 队列弹出连接时，无任何目标地址校验逻辑。所有分片的 idle 队列全局共享，不按上游分组。

**当前缓解：** `UpstreamManager` 为每个 service 创建独立的 `HttpPool` 实例（`unordered_map<string, shared_ptr<HttpPool>>`），一个 pool 只服务一个上游地址。当前架构下不会串。

**风险：** 如果后续有人拿到同一个 `HttpPool` 实例对两个不同 host 调 `acquire`，连接会发错服务器。属于防御性缺失。

**建议：** 在 `HttpConn` 中增加 `std::string host; int port;` 字段，弹出 idle 时校验匹配。

#### S2. 无 RAII 守卫，漏调用 release 会导致计数泄漏与悬空指针

**代码位置：** `src/http/http_pool.hpp:146`, `src/http/http_pool.hpp:257-338`

**问题：** `acquire` 返回 `unique_ptr<HttpConn>`，完全依赖调用方手动调用 `release()` / `release_bad()`。如果调用方异常退出、遗漏归还：
- `shard.active` 集合中残留裸指针，变成悬空指针
- `shard.total` / `shard.in_flight` 永不扣减
- 全局 `total_count` / `in_flight_count` 永不扣减

**当前缓解：** `client_session.hpp` 使用 `ConnGuard` RAII 包裹（`src/http/proxy_forwarder.hpp:28-54`），但 RAII 在 proxy 层而非池层。

**风险：** max_size / max_concurrent 限流彻底失效（计数只增不减），shutdown 遍历 active 集合时存在悬空指针风险。

**建议：** 将 `ConnGuard` 下沉到连接池层，不对外暴露原始 `HttpConn` 指针。

#### S3. max_concurrent 限流逻辑错误，有空闲连接也可能返回失败

**代码位置：** `src/http/http_pool.hpp:160-165`

**问题：** 限流检查嵌套在 idle 弹出的 `while` 循环内部。每弹一个连接前就尝试 +1 `in_flight_count`，一旦失败直接 `co_return nullptr`，存在两个缺陷：
- 第一个分片弹出失败后直接终止，剩余 15 个分片的 idle 连接完全不遍历
- 限流失败后连"新建连接"路径也不走

**风险：** 高并发限流临界状态下，明明存在可用 idle 连接，却因为单次尝试失败直接返回空。**这是当前最严重的并发 bug。**

**建议：** 在 `acquire` 入口处统一尝试获取 `in_flight` 配额，成功后再进入 idle 弹出 / 新建流程。

### 🟠 中等风险（建议修复，影响稳定性与性能）

#### S4. 超时异步操作未显式取消，存在生命周期竞态

**代码位置：** `src/http/http_pool.hpp:388-431`

**问题：** `resolve_with_timeout` 和 `connect_with_timeout` 使用 `parallel_group::wait_for_one` 实现超时，但超时触发后没有显式取消另一侧：
- `connect_with_timeout` 超时返回后 `async_connect` 仍在后台运行，socket 被关闭后 handler 触发时有生命周期风险
- `resolve_with_timeout` 中 `resolver` 是局部变量，函数返回后析构自动取消，但依赖析构行为而非显式 cancel

**风险：** 极端时序下可能触发 handler 访问已销毁的协程栈。

**建议：** 超时分支中显式调用 `resolver.cancel()` 和 `socket.cancel()`。

#### S5. 空闲连接仅惰性回收，峰值后资源长期占用

**代码位置：** `src/http/http_pool.hpp:159`, `src/http/http_pool.hpp:433-446`

**问题：** `evict_stale_idle` 只在 `acquire` 弹 idle 时才执行。如果流量突增后突降，长时间没有新请求，过期的 idle 连接会一直占用文件描述符和内存。

**风险：** 大流量峰值后长期持有大量无用连接，极端情况下触发 fd 上限。

**建议：** 启动后台协程定期回收，或增加 `force_cleanup()` 接口。

#### S6. DNS 解析无缓存，每次新建连接重复解析

**代码位置：** `src/http/http_pool.hpp:230`

**问题：** 每次新建连接都调用 `resolver.async_resolve` 全量解析，无任何缓存。对于域名固定的上游，重复解析既增加延迟，又放大 DNS 服务器压力。同时只有一个 `connect_timeout_ms`，解析和连接各占一份，总超时可达 2 倍配置值。

**建议：** 增加 DNS 结果缓存（TTL 60s），拆分 `resolve_timeout_ms` 和 `connect_timeout_ms`。

#### S7. 全局计数与分片计数存在瞬时不一致

**代码位置：** `src/http/http_pool.hpp:208-210`, `src/http/http_pool.hpp:247-252`

**问题：** `shard.total` / `shard.in_flight`（锁保护）与全局 `total_count` / `in_flight_count`（原子变量）分开更新，中间存在时间窗口。`stats()` 采样时可能落在窗口内，分片求和与全局计数不一致。

**风险：** 仅影响可观测性，不影响功能正确性；但排查问题时可能造成数据迷惑。

**建议：** `stats()` 以分片锁内求和为准，移除全局原子计数。

### 🟡 代码质量与设计冗余

#### S8. connection_close 字段无赋值路径，属于死代码

**位置：** `src/http/http_pool.hpp:35`

`HttpConn::connection_close` 在连接池代码中**没有任何一处将其设为 true**。该字段依赖上层 `read_proxy_response` 设置，但连接池未做任何约束和说明，属于半完成接口。

#### S9. Config 中部分字段在连接池层未生效

**位置：** `src/http/http_pool.hpp:20-30`

- `max_body_size`：连接池层完全未引用，实际由 `read_proxy_response` 控制
- `send_keep_alive_header`：连接池层不构造请求，该字段仅在 `build_proxy_request` 中使用

属于职责边界不清晰，配置冗余。

#### S10. stats() 全分片串行加锁，观测影响业务

**位置：** `src/http/http_pool.hpp:342-364`

`stats()` 遍历 16 个分片依次加锁统计，调用期间阻塞所有分片的 `acquire`/`release`。如果 `pool_stats_service` 定时调用频繁，会成为隐性性能瓶颈。

#### S11. 分片选择策略负载不均

**位置：** `src/http/http_pool.hpp:90-93`

`pick_shard()` 使用线程局部轮询，当线程数不是 16 的倍数、或不同线程调用频率差异大时，部分分片锁竞争高、部分空闲。

#### S12. shutdown 与析构函数逻辑重复

**位置：** `src/http/http_pool.hpp:73-88`, `src/http/http_pool.hpp:103-122`

`State::~State` 和 `HttpPool::shutdown` 都做了关闭 idle/active 连接的逻辑，代码冗余。`shutdown` 扣减了计数，析构函数不扣减（析构时计数已无意义），行为不一致。

---

### 12 个补充问题的优先级矩阵

| 优先级 | 编号 | 问题 | 严重度 |
|:------:|:----:|:-----|:------:|
| **P0** | S3 | max_concurrent 限流逻辑 bug | 并发安全 — 高并发下误杀可用请求 |
| **P0** | S4 | 超时操作未 cancel | 生命周期安全 — 极端时序下 UAF |
| **P1** | S1 | 连接未按 host:port 隔离 | 防御性缺失 — 当前架构不触发但漏洞存在 |
| **P1** | S2 | 池层无 RAII 守卫 | 资源安全 — 依赖调用方自律 |
| **P1** | S5 | 空闲连接惰性回收 | 资源泄漏 — 峰值后 fd 占用 |
| **P2** | S6 | DNS 解析无缓存 | 性能 — 重复解析增加延迟 |
| **P2** | S7 | 全局/分片计数不一致 | 可观测性 — stats 瞬时不一致 |
| **P3** | S8 | connection_close 无赋值路径 | 死代码 |
| **P3** | S9 | 部分 Config 字段池层未生效 | 配置冗余 |
| **P3** | S10 | stats() 串行加锁 | 观测性能 |
| **P3** | S11 | pick_shard 负载不均 | 性能偏斜 |
| **P3** | S12 | shutdown / ~State 逻辑重复 | 代码冗余 |
