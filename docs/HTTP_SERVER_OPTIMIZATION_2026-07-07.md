# http_server 优化与拆分计划 2026-07-07

> 来源：代码评审报告、`docs/GATEWAY_DESIGN.md`、`src/http/http_server.hpp` 当前实现巡检
> 目标：先修协议正确性缺陷，再按职责拆分 `http_server.hpp`，避免后续网关、鉴权、超时和响应处理继续堆在单个头文件里。

## 一、当前问题概览

`src/http/http_server.hpp` 当前同时承担以下职责：

- 服务启动、accept 循环、连接协程调度。
- 客户端请求头读取、picohttpparser 解析、body framing。
- HTTP header 解析、hop-by-hop header 过滤、日志脱敏。
- 上游连接池 RAII guard、代理请求构建、上游响应解析、失败重试。
- JSON key 转换、错误响应构建、客户端 keep-alive 判断。
- 鉴权调用和限流拒绝响应拼装。

这些逻辑放在一个头文件里会带来三个直接问题：

- 协议修复容易互相影响，例如 body 读取、pipeline 预读、keep-alive 和 proxy retry 共享状态。
- 性能优化难以定位热路径，例如 header 过滤、超时 timer、body buffer 拷贝分散在多个大分支里。
- 单元测试难写，当前多数函数是 `HttpServer` private 成员，无法独立覆盖 JSON 转换、header 解析、response framing 等纯逻辑。

## 二、交付原则

本计划必须先保证线上缺陷能独立修复，不能把 bug fix 绑定到大规模重构：

- P0 bug 先在现有结构上修复并合并。允许为了测试暴露小型 helper，但不要求先完成文件拆分。
- 纯函数抽取、response builder、body reader、client session 拆分属于后续结构优化。
- 函数签名级改造必须一次性覆盖所有调用方，不能假装成可局部合并的小阶段。例如 `read_with_timeout` 返回 `IoResult` 会影响 client header/body、proxy response、chunked reader、upstream write 等所有路径。
- 每个阶段都要保持 keep-alive/pipeline 契约不退化：多读到的字节必须明确归属到当前 body 或下一次请求/响应的 preread buffer。

## 三、必须优先修复的功能缺陷

### 1. JSON 键名转换错误修改数组字符串

**位置：** `json_keys_snake_to_camel`

**问题：** 当前仅靠单一 `expect_key` 标记。遇到逗号就无条件置为 true，无法区分对象内逗号和数组内逗号，导致数组中的字符串值被错误执行下划线转驼峰。

**错误示例：**

```json
{"list": ["hello_world", "foo_bar"]}
```

当前会错误输出：

```json
{"list": ["helloWorld", "fooBar"]}
```

**修复方案：**

- 引入 `scope_stack` 追踪 `{}` / `[]` 嵌套。
- 只有当前作用域是对象，且处于“等待 key”状态时，字符串才执行 snake_case 到 camelCase。
- 数组中的字符串、对象 value 字符串、转义字符串内容必须原样保留。
- 字符串扫描必须显式处理转义：遇到 `\` 时原样复制当前字符和下一个字符，并且不能让被转义的 `"` 结束字符串。
- key 内如果包含转义字符，只允许对未转义的 `_` 执行 camel 转换；`\_`、`\"`、`\\` 等转义序列保持原样。

**建议测试：**

- `{"a_b":1}` -> `{"aB":1}`
- `{"list":["hello_world"]}` 保持数组值不变。
- `{"outer_obj":{"inner_key":"value_text"}}` 只改 key，不改 value。
- `{"list":[{"item_key":"foo_bar"}]}` 改数组内对象 key，不改 value。
- `{"a\\\"b_key":1}` 不因转义引号错位。
- `{"key":"a\\\"b_c"}` value 中的转义引号和下划线保持原样。
- `{"arr":["a\\\"b_c",{"inner_key":"x_y"}]}` 数组值不改，数组内对象 key 仍正常转换。

### 2. 上游响应 Body 超限返回残缺 2xx

**位置：** `read_proxy_response`

**问题：** 上游响应 body 超过 `max_body_size` 时，当前部分路径只做 `resize(max_body_size)` 截断，仍可能返回原 2xx 状态。客户端收到的是合法状态码加残缺 body，无法判断网关发生截断。

**修复方案：**

- 一旦响应 body 超限，设置 `resp.error = "upstream_response_body_too_large"`。
- 标记 `conn.connection_close = true`，当前上游连接必须丢弃。
- 调用方将该错误转换成 `502 Bad Gateway` 或现有 proxy error 响应，不透传截断 body。
- 对 `Content-Length`、chunked、EOF-framed 三条 body 读取路径统一处理。

**注意：** 如果 `Content-Length` 本身大于 `max_body_size`，应在读取 body 前直接失败，避免继续读完整大 body。

**自动化测试要求：**

- 新增 `tests/test_proxy_framing.cpp` 或等价测试文件，覆盖上游 `Content-Length` 响应超过 `max_body_size` 时返回 502，且不透传截断 body。
- 覆盖 chunked 响应累计 body 超过 `max_body_size` 时返回 502。
- 覆盖 EOF-framed 响应超过 `max_body_size` 时返回 502。
- 测试必须验证上游连接被标记 bad 或不可复用，避免超限后把残留字节留给下一次请求。
- 如果 `read_proxy_response` 仍是 private，P0 修复允许先增加最小测试 seam，例如把响应读取逻辑移动到可测试 helper；但不要把完整 proxy forwarder 拆分作为修 bug 的前置条件。

### 3. I/O 超时、对端关闭、系统错误无法区分

**位置：** `read_with_timeout`、`write_with_timeout`

**问题：**

- `read_with_timeout` 超时、对端关闭、系统错误都返回 0。
- `write_with_timeout` 统一返回 `false`。
- 日志无法区分慢客户端、上游 idle 关闭、真实系统错误，排障时只能看统一的 read/write failed。

**修复方案：**

```cpp
enum class IoStatus {
    Success,
    Timeout,
    PeerClosed,
    SysError
};

struct IoResult {
    IoStatus status;
    std::size_t bytes = 0;
    asio::error_code ec;
};
```

- `asio::error::operation_aborted` 且 timer 触发：`Timeout`。
- `asio::error::eof` 或 read 返回 0：`PeerClosed`。
- 其他 error_code：`SysError`。
- 调用方日志中带 `status` 和 `ec.message()`。

**交付约束：** 这不是孤立阶段。`read_with_timeout` / `write_with_timeout` 一旦改签名，必须同步改完所有调用方，包括 client header 读取、client body 读取、chunked body reader、upstream response header/body 读取、upstream request write。不能先合并只改 helper 的半迁移状态。

**可选落地方式：**

- 方式 A：一次 PR 完成 `IoResult` 签名迁移和所有调用点日志更新。
- 方式 B：先新增 `read_with_timeout_result` / `write_with_timeout_result`，旧函数保留为兼容 wrapper；等所有调用方迁移后再删除旧 wrapper。该方式仍要防止长期双轨维护。
- 若新增 metrics，计数器也应在同一迁移中接入，避免下一轮再次翻动热路径。

## 四、中等优先级优化

### 1. 头解析减少拷贝和 O(n) erase

**位置：** `parse_header_fields`

**问题：** 当前传值拷贝完整 header 字符串，并在循环内 `erase(0, n)`，多 header 时会产生不必要的移动。

**修复方案：**

- 参数改成 `std::string_view header_lines`。
- 用 `start` / `line_end` 索引扫描 `\r\n`。
- `parse_header_line_into` 也改成接收 `std::string_view`。

### 2. header 过滤减少小写临时字符串

**位置：** proxy request/response header filter

**问题：** 当前多处 `to_lower(k)` 只用于一次比较或 `contains_header_name`，会产生小字符串分配。

**修复方案：**

- 固定 hop-by-hop header 统一使用 `header_iequals` 比较。
- `Connection` 扩展 token 可保留规范化后的 vector，但 `contains_header_name` 应改成大小写不敏感比较：

```cpp
static bool contains_header_name(
    const std::vector<std::string>& names,
    std::string_view name) {
    for (const auto& candidate : names) {
        if (header_iequals(candidate, name)) return true;
    }
    return false;
}
```

### 3. `Expect: 100-continue` 决策：本轮保持忽略

**位置：** `handle_connection` 请求头解析后、读取 body 前

**问题：** 客户端发送 `Expect: 100-continue` 时，网关当前不会先返回 `100 Continue`，而是按普通请求继续读取 body。`docs/GATEWAY_AUTH_DESIGN.md` 已记录“忽略 Expect，按正常流程消费 body 后鉴权”。

**决策：** 本轮保持当前“忽略 Expect”的行为，不实现 `100 Continue`。

**理由：**

- 当前 `handle_connection` 的安全链在 body 消费之后执行。这样做是为了保证 keep-alive/pipeline 安全：无论本地路由、代理路由还是鉴权拒绝，都先按 framing 规则消费完整 body，避免残留 body 被解析成下一次请求头。
- 如果在读取 body 前发送 `100 Continue`，后续再因 IP 黑名单、鉴权或限流拒绝，请求 body 已经被客户端发送，无法节省传输成本。
- 如果为了避免浪费 body 传输而在 `100 Continue` 前执行安全链，则需要重排当前“body 先消费再鉴权”的模型，并重新证明 framing 错误、请求走私、pipeline 字节保留等路径仍然安全。本轮优化不扩大到这个架构变化。
- 因此，对 `Expect: 100-continue` 的处理策略保持与 `GATEWAY_AUTH_DESIGN.md` 一致：忽略该头，按普通请求读取 body，再进入安全链。

**DoS 权衡：**

- 忽略 `Expect` 且鉴权在 body 消费后执行，意味着恶意客户端可以发送 `Expect: 100-continue` 加大 body，在最终被鉴权拒绝前占用下行带宽和连接读时间。
- 当前缓解依赖已有 body size 上限、header/body 读取超时、连接级 keep-alive 控制和限流规则。
- 如果未来出现“未鉴权大 body 拖垮网关”的真实压测或线上证据，应单独设计“header-only pre-auth”阶段：只允许 IP 黑白名单、路径黑名单、全局限流等不依赖 body 的规则在读取 body 前执行。该设计必须重新验证请求走私和 pipeline 安全。

**实现要求：**

- 不新增提前写 `HTTP/1.1 100 Continue` 的分支。
- 转发上游时过滤 `Expect`，避免把客户端的 continue 语义透传给上游。
- 文档和测试只验证“不会因 Expect 破坏 body 读取和 keep-alive”，不要求优化等待 `100 Continue` 的客户端行为。

### 4. `SecurityRules` 生命周期决策：本轮不替换对象

**位置：** `HttpServer::set_security_rules`、`security_rules_`

**当前状态：** `Application::cleanup()` 先 reset `server_`，再 reset `security_rules_`，现有关闭顺序下裸指针暂时不悬空。

**风险：** 如果后续热更新改成替换整个 `SecurityRules` 对象，并发请求可能持有旧裸指针访问。同时，当前 `SnapshotService` 和 `ReloadService` 都持有 `SecurityRules&`，即使 `HttpServer` 改成 atomic `shared_ptr`，这两个服务仍会引用旧对象，语义会变得不一致。

**决策：** 本轮保持当前热更新语义：不替换整个 `SecurityRules` 对象，只更新其内部 state。

**理由：**

- 当前 `ReloadService` / `SnapshotService` 的设计基于稳定的 `SecurityRules&`。
- 只更新内部字段可以保持三个调用方看到同一个规则对象，避免 `HttpServer`、snapshot、reload 分别指向不同规则实例。
- 是否将规则热更新改成“整体对象替换 + atomic shared_ptr load”属于独立架构设计，必须连同 `SnapshotService` / `ReloadService` 一起改，不能只改 `HttpServer`。

**本轮要求：**

- `HttpServer` 可以继续保存 `SecurityRules*`，但文档中必须明确生命周期由 `Application` 保证：`server_` 生命周期短于 `security_rules_`。
- 如果只为了表达非拥有关系，也可改成 `SecurityRules&` 或封装成 `std::reference_wrapper<SecurityRules>`，但这不是本轮必须项。
- 不引入 `std::shared_ptr<SecurityRules>` 原子替换方案，除非同时修改 `Application`、`ReloadService`、`SnapshotService` 的持有模型。

**未来必须整体改造的触发条件：**

- 热更新语义从“更新内部字段”改成“替换整个 `SecurityRules` 对象”。
- `ReloadService` 或 `SnapshotService` 需要跨 reload 看到新的规则对象实例，而不是同一个对象内的新 state。
- 规则对象析构可能与 in-flight 请求并发发生。

一旦满足任一条件，必须三方一起改：`HttpServer`、`ReloadService`、`SnapshotService` 都改为可安全获取当前规则对象的模型，例如统一从 `shared_ptr` holder atomic load，不能只改 `HttpServer`。

## 五、建议拆分后的文件结构

结构拆分阶段建议优先抽纯工具，减少行为风险；当实现体积变大或测试需要链接内部 helper 时，再引入 `asio_owen_core` 静态库承载 `.cpp`。

```text
src/http/
  http_server.hpp          // HttpServer public API、accept loop、成员变量
  client_session.hpp       // handle_connection 主流程
  http_protocol.hpp        // HeaderParseState、trim、CL/TE/Connection 解析、hop-by-hop 判断
  http_io.hpp              // read/write timeout、IoResult
  http_body_reader.hpp     // fixed/chunked body 读取、BodyReadResult
  proxy_forwarder.hpp      // ConnGuard、上游请求构建、retry、read_proxy_response
  response_builder.hpp     // build_error_response、build_downstream_response、状态短语
  json_transform.hpp       // json_keys_snake_to_camel
```

### 文件职责边界

**`http_server.hpp`**

- 保留 `HttpServer::route`、`HttpServer::upstreams`、`set_security_rules`、`start`、`stop`。
- 不再包含 header 解析、body 读取、proxy response parsing 等大段 private helper。
- 成员变量仍为 `ioc_`、`acceptor_`、`routes_`、`running_`、`upman_`、`security_rules_`。

**`client_session.hpp`**

- 承载单个客户端 socket 的请求循环。
- 负责调用协议解析、body reader、鉴权、本地路由、proxy forwarder、response builder。
- 保留 pipeline 所需的 `client_preread`。

**`http_protocol.hpp`**

- `HeaderParseState`、`HeaderTokens`、`HeaderListTokens`。
- `trim_view`、`parse_decimal_size`、`parse_hex_size_line`。
- `update_header_state`、`parse_header_fields`、`split_connection_tokens`。
- `is_hop_by_hop_header`、`contains_header_name`。

**`http_io.hpp`**

- `IoStatus`、`IoResult`。
- `read_with_timeout`、`write_with_timeout`。
- 后续如果复用连接级 timer，也应只在这里调整。

**`http_body_reader.hpp`**

- `consume_line`、`consume_exact`、`read_chunked_stream`。
- 可新增 `read_content_length_body`，把 `handle_connection` 中读取固定长度 body 的循环移出。
- 阶段 3 抽取时可以先保留当前 `read_with_timeout` 的粗粒度失败语义；阶段 4 `IoResult` 迁移后，再把失败细分为 timeout、peer closed、sys error。

### body reader 与 preread 契约

`http_body_reader.hpp` 不能隐藏 pipeline 语义。所有 body reader 都必须显式接收并维护调用方传入的 preread buffer：

```cpp
enum class BodyReadStatus {
    Success,
    ReadFailed,   // 阶段 3 兼容旧 read_with_timeout 语义
    Timeout,
    PeerClosed,
    TooLarge,
    InvalidChunk,
    SysError
};

struct BodyReadResult {
    BodyReadStatus status;
    std::string body;
    asio::error_code ec;
};
```

建议接口形态：

```cpp
asio::awaitable<BodyReadResult> read_content_length_body(
    asio::ip::tcp::socket& socket,
    std::string& preread,
    size_t content_length,
    size_t max_body_size,
    std::chrono::milliseconds timeout);

asio::awaitable<BodyReadResult> read_chunked_body(
    asio::ip::tcp::socket& socket,
    std::string& preread,
    size_t max_body_size,
    std::chrono::milliseconds timeout);
```

契约：

- `preread` 是输入/输出参数。调用前保存已经从 socket 多读出的字节；调用成功后只保留“当前 body 之后”的字节，供下一次请求或上游响应继续解析。
- `read_content_length_body` 成功时返回的 `body.size()` 必须等于 `content_length`；如果原始 `preread` 超过 body 长度，超出的部分留在 `preread`。
- `read_chunked_body` 成功时返回 dechunk 后的 body；chunk framing 和 trailers 被消费丢弃；final chunk 之后多读到的字节留在 `preread`。
- 任何失败状态下，调用方必须关闭当前客户端连接或丢弃当前上游连接。失败后的 `preread` 不再具备继续解析的契约。
- client 侧 `client_preread` 和 upstream 侧 `conn.read_buffer` 使用同一契约，但所有权不同：前者属于 `ClientSession`，后者属于池化上游连接。
- 阶段 3 可以只返回 `ReadFailed`；阶段 4 完成 `IoResult` 迁移后，必须将其拆分成 `Timeout`、`PeerClosed`、`SysError`。

这个契约必须先于 `ClientSession` 拆分落地，否则 body reader 和 session 会在“下一次请求字节归谁保存”上产生隐式耦合。

**`proxy_forwarder.hpp`**

- `ProxyResponse`、`ConnGuard`。
- 构建上游请求、过滤 request headers、写上游、读取上游响应、一次 stale idle retry。
- `read_proxy_response` 在这里集中维护 RFC 7230 响应 framing。

**`response_builder.hpp`**

- 统一状态短语映射。
- `build_error_response(int code, std::string_view msg)`。
- `build_downstream_response(ctx, method, proxy_response)`。
- 统一过滤响应 hop-by-hop header 和设置 `Content-Length`。

**`json_transform.hpp`**

- 只放 JSON key 转换逻辑。
- 单独加 GoogleTest，避免通过端到端 proxy 测试覆盖这类纯字符串状态机。

### 模块依赖方向

拆分时必须保持依赖单向流动，避免 `client_session.hpp`、`proxy_forwarder.hpp`、`http_protocol.hpp` 互相 include：

```text
http_protocol.hpp
json_transform.hpp
http_io.hpp
        ^
        |
http_body_reader.hpp
        ^
        |
proxy_forwarder.hpp
        ^
        |
client_session.hpp
        ^
        |
http_server.hpp
```

约束：

- `http_protocol.hpp` 必须是纯状态机和纯函数，只依赖标准库和 `response.hpp` 中已有的 header 比较工具；不能依赖 `ClientSession`、`ProxyForwarder`、`HttpServer`、`HttpPool`。
- `proxy_forwarder.hpp` 可以依赖 `http_protocol.hpp`、`http_io.hpp`、`http_body_reader.hpp`、`http_pool.hpp`、`upstream_manager.hpp`，但不能依赖 `client_session.hpp`。
- `client_session.hpp` 负责 orchestration，可以依赖 proxy、body reader、response builder，但不能被底层 protocol 或 proxy 模块反向依赖。
- 共享数据结构如果被两个方向都需要，优先下沉到更底层的纯头文件。例如 `ProxyResponse` 只由 proxy 返回给 session，可放在 `proxy_forwarder.hpp`；`HeaderParseState` 被 client 和 proxy 共同使用，必须放在 `http_protocol.hpp`。

### ClientSession 生命周期与关停契约

阶段 5 不能只把 `handle_connection` 移到 `ClientSession` 类里。`ClientSession` 一旦持有 `routes_`、`UpstreamManager`、`SecurityRules` 的引用，就必须重新证明 shutdown 顺序：

- `HttpServer::stop()` 停止 accept，不代表已 `co_spawn` 的 session 立即结束。
- `Application::cleanup()` 当前先 reset `server_`，再 reset `security_rules_`。如果 session 对象脱离 `HttpServer` 生命周期继续运行，直接持有 `HttpServer` 内部成员引用会产生悬垂风险。
- `UpstreamManager` 析构会影响上游连接池；in-flight proxy 请求必须通过 `shared_ptr<HttpPool>` 保持池对象和 state 存活。

拆分前必须选择一种生命周期模型：

**方案 A：保持 session 逻辑不逃逸 `HttpServer` 所有权**

- `handle_connection` 可以移到私有 helper 或内部类，但 session 不保存比 `HttpServer` 更长的引用。
- `HttpServer` 析构/stop 前仍需等待或取消 in-flight session，不能仅靠 reset 对象。

**方案 B：引入共享 server state**

```cpp
struct HttpServerState {
    std::unordered_map<std::string, Handler> routes;
    UpstreamManager upstreams;
    SecurityRules* security_rules;
    std::atomic<bool> running;
};
```

- `HttpServer` 和每个 `ClientSession` 持有 `std::shared_ptr<HttpServerState>`。
- `HttpServer::stop()` 设置 `running=false` 并关闭 acceptor；state 直到所有 session 退出后再析构。
- `security_rules` 仍按本轮决策保持非拥有指针，但 `Application` 必须保证其生命周期长于所有 session，或把它也纳入同一个共享 state/lifetime 管理。

本轮如果不能完成上述生命周期证明，`ClientSession` 拆分阶段不应实施；可以先停留在函数级拆分，避免引入新的悬垂引用风险。

## 六、推荐实施顺序

### 阶段 0：P0 bug 直接修复

目标：先修线上正确性问题，不等待结构重构。

- 在现有代码上修复 `json_keys_snake_to_camel` 数组字符串误转换问题。
- 在现有代码上修复上游 response body 超限返回截断 2xx 的问题。
- 新增自动化测试：
  - `tests/test_json_transform.cpp` 覆盖对象 key、数组 value、嵌套对象、转义引号、转义反斜杠。
  - `tests/test_proxy_framing.cpp` 覆盖 `Content-Length`、chunked、EOF-framed 三种上游超限路径。
- 如果为了测试需要移动少量 helper 到新 header，可以做最小抽取；不得把完整 `proxy_forwarder.hpp` 或 `client_session.hpp` 拆分作为 P0 修复前置条件。

### 阶段 1：纯协议工具抽取

目标：只抽纯函数，不改 socket / coroutine / keep-alive 控制流。

- 新增 `http_protocol.hpp`，移动 header/token/trim/parse 相关函数。
- 将 `parse_header_fields` 改为 `string_view` 扫描。
- 将 header filter 的 `contains_header_name` 改为大小写不敏感比较，减少 `to_lower` 使用。
- 新增或补充 `tests/test_http_protocol.cpp`。

### 阶段 2：响应构建统一化

目标：减少 `handle_connection` 尾部响应拼接分支，但不改变客户端写响应策略。

- 新增 `response_builder.hpp`。
- 抽出状态短语函数，例如 `reason_phrase(int status)`。
- 抽出 `build_error_response`。
- 抽出 downstream response header 过滤与 `Content-Length` 设置。
- 保持客户端写响应仍使用裸 `async_write + redirect_error`，不要把 hot path 重新套 timer。

### 阶段 3：body reader 抽取，但保持现有 I/O helper 签名

目标：先把 body framing 从 `handle_connection` / proxy 读取中抽出来，同时稳定 preread 契约。

- 新增 `http_body_reader.hpp`。
- 移动 chunked 和 fixed length body 读取逻辑。
- reader 接口必须遵守“body reader 与 preread 契约”章节。
- 本阶段可以继续使用当前 `read_with_timeout` 的 `size_t/0` 语义，避免同时改所有调用方。

### 阶段 4：`IoResult` 协调迁移

目标：结构化区分 timeout、peer closed、sys error，并同步改完所有调用方。

- 新增 `http_io.hpp`，引入 `IoResult`。
- `read_with_timeout` / `write_with_timeout` 改签名或新增 result 版本。
- 同一阶段必须更新 client header/body、body reader、proxy response、upstream write 的所有调用点。
- 日志中输出 `Timeout` / `PeerClosed` / `SysError`。
- 若项目已有 metrics 框架，同步加入 `io_timeout_total`、`io_peer_closed_total`、`io_sys_error_total` 这类计数器；如果暂时没有 metrics 框架，至少预留集中打点位置，避免下次再翻动每条热路径。

### 阶段 5：ProxyForwarder 拆分

目标：隔离上游代理复杂度。注意 P0 body 超限 bug 此时应已修复，本阶段只做结构拆分。

- 新增 `proxy_forwarder.hpp`。
- 移动 `ProxyResponse`、`ConnGuard`、上游请求构建、上游响应读取。
- 保留 stale idle retry 语义：只对复用 idle 连接的首次读/写失败重试一次。
- 不改变 `HttpPool` 的 active/idle 跟踪和 release 规则。

### 阶段 6：ClientSession 拆分

目标：让 `HttpServer` 回到 acceptor/router 容器职责。该阶段必须先完成生命周期证明。

- 新增 `client_session.hpp`。
- `HttpServer::start` 中 `co_spawn(ioc_, ClientSession(...).run(), asio::detached)`，或保留 private helper 形式。
- 落地前必须按“ClientSession 生命周期与关停契约”选择方案 A 或 B。
- 保留 `client_preread` 跨请求循环支持 pipeline。

## 七、CMake 与编译策略

当前 `asio_owen_deps` 是 `INTERFACE`，多数逻辑以 header-only 方式被 server 和 tests 共同 include。拆分策略有两种：

**短期方案：**

- 小型纯函数新文件可以先以 `.hpp` 为主，例如 `json_transform.hpp`、轻量 `http_protocol.hpp`。
- tests 直接 include 对应 header。
- 不改顶层 CMake 目标结构。
- 如果 `http_protocol.hpp` 或 `response_builder.hpp` 体积明显变大，或 tests 编译时间明显上升，不应继续扩大 header-only 范围。

**中期方案：**

- 建议在出现以下任一情况时直接新增 `asio_owen_core` 静态库，而不是等到所有拆分完成：
  - 新增非模板、非 inline 的较大 helper。
  - `proxy_forwarder`、`response_builder`、`http_body_reader` 开始承载较多实现代码。
  - tests 因 header-only 实现重复编译导致反馈变慢。
  - 需要把 private helper 变成可链接的测试目标，而不是继续暴露实现细节到头文件。

```cmake
add_library(asio_owen_core
    src/app/application.cpp
    src/app/routes.cpp
    picohttpparser.c
)
target_link_libraries(asio_owen_core PUBLIC asio_owen_deps)

add_executable(server src/main.cpp)
target_link_libraries(server PRIVATE asio_owen_core)
```

- 后续如果 `proxy_forwarder.cpp`、`response_builder.cpp` 等从 header-only 转成 `.cpp`，统一加入 `asio_owen_core`。
- tests 链接 `asio_owen_core`，避免重复编译和链接口径不一致。

## 八、验证清单

每个阶段完成后至少执行：

```bash
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure
```

P0 修复必须有自动化测试：

- JSON 转换：对象 key、数组 value、嵌套对象、转义引号、转义反斜杠。
- Proxy framing：上游 `Content-Length`、chunked、EOF-framed body 超过 `max_body_size` 时，客户端收到 502，不收到截断 2xx。

涉及 proxy、framing、keep-alive 的阶段，还需要补充手工验证：

- `/api/health` 正常返回。
- `Content-Length` 请求 body 可正常转发。
- chunked 请求 body 被 dechunk 后转发，并重写 `Content-Length`。
- 上游 `Content-Length`、chunked、EOF-framed 响应均可正常处理。
- keep-alive 连续请求不会串包，framing 错误后强制关闭连接。
- stale idle 上游连接失败时只重试一次。

### 性能回归护栏

阶段 2、阶段 3、阶段 4 都会触碰热路径，必须跑压测或至少短压 sanity：

- health 本地路由压测，确认 response builder 抽取没有引入明显回退。
- proxy POST 压测，确认 header filter 和 `IoResult` 改造没有破坏连接复用。
- 与同环境最近一次基线相比，Health RPS 回退超过 5% 或 proxy POST RPS 回退超过 8% 时，需要解释原因；超过 10% 不应直接合并，除非该阶段明确用性能换协议正确性。
- 压测必须记录构建类型、并发、时长、接口、成功率、RPS、P99、server error/warn 摘要。
- 对 `IoResult` / body reader 改造，除了 RPS，还要关注 4xx/5xx 来源和上游连接复用率，避免错误分类改变 retry 行为。

## 九、非目标

本轮不建议同时做以下改动：

- HTTPS/TLS termination。
- 多上游负载均衡、熔断、主动健康检查。
- 流式 body 转发。
- MySQL / Redis pool 行为调整。
- 日志系统替换或同步日志。

这些属于更大架构变化，应在 `http_server.hpp` 结构拆清楚后单独设计。
