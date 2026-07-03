# HTTP Gateway Design

## Goal

Add HTTP reverse-proxy support to the existing standalone ASIO HTTP server.
The gateway accepts client HTTP/1.x requests, routes `/proxy/{service}/...`
to configured upstream HTTP services, and reuses upstream keep-alive
connections through a bounded pool.

Local routes such as `/api/health`, `/api/redis`, `/api/mysql`, and
`/api/combo` keep exact-match priority over proxy routing.

Design priorities, in order:

1. **Protocol compliance** — RFC 7230 body framing, hop-by-hop stripping,
   HTTP/1.0 vs 1.1 semantics.
2. **Anti-smuggling** — reject framing the gateway cannot reason about
   rather than guessing what downstream might do.
3. **Resource safety** — bounded pool, idle eviction, RAII connection
   return on every path.
4. **Slowloris resistance** — every client/upstream read and write has
   a timeout.
5. **Clean shutdown** — acceptor stops, in-flight drains, sockets close,
   no fd leak.

## Module Layout

| File | Responsibility |
|---|---|
| `src/http/http_server.hpp` | Accept loop, HTTP parsing, body framing, forwarding, client keep-alive |
| `src/http/http_pool.hpp` | Lazy-create pool with idle eviction and hard cap |
| `src/http/upstream_manager.hpp` | `/proxy/{svc}` routing, per-service pool |
| `src/http/http_context.hpp` | `HttpContext`: request/response data carrier |

```
HttpServer
├── UpstreamManager
│   └── HttpPool (per service)
├── ConnGuard (RAII)
└── routes_ (local /api/...)
```

## Request Flow

1. Read client headers with a 30s timeout and a 64KB header cap.
2. Parse the request with picohttpparser.
3. Parse framing headers via `parse_header_line_into` into `HeaderParseState`:
   - invalid `Content-Length` -> 400
   - conflicting duplicate `Content-Length` -> 400
   - `Transfer-Encoding` without final `chunked` -> 400
   - `Transfer-Encoding` plus `Content-Length` strips `Content-Length` before forwarding
4. Always consume the request body according to framing before dispatching,
   including local routes. This preserves keep-alive and pipelined bytes.
5. Dispatch exact local route first, then `/proxy/{service}/...`, then 404.

## Header Parsing State Machine

```cpp
struct HeaderParseState {
    std::optional<size_t> content_length;        // distinguishes "CL=0" from "no CL"
    bool duplicate_content_length = false;
    bool invalid_content_length = false;
    bool is_chunked = false;
    bool has_transfer_encoding = false;
    bool connection_close = false;
    bool connection_keep_alive = false;
};
```

`optional<size_t>` is critical: `Content-Length: 0` must be treated as "body
present, zero bytes" and trigger the read path with `remaining = 0`, not the
"no length framing" branch that reads to EOF. Using a plain `size_t` here is
a known bug pattern that breaks keep-alive for every CL=0 response.

`Connection` tokens are parsed with `split_connection_tokens`, which splits
on commas, trims each token, and compares case-insensitively against
`"close"` and `"keep-alive"`. Substring matching (`find("close")`) is
forbidden — it would misclassify `Connection: closed` as `close`.

## Chunked Handling

Chunked bodies are kept as raw chunked bytes when forwarding to the upstream
(the upstream expects the same chunk framing the client sent). Parsing is
protocol-aware via `read_chunked_stream`:

1. `consume_line` reads up to `\r\n`.
2. `parse_hex_size_line` parses the hex size, allowing chunk extensions
   after `;`.
3. `consume_exact` reads exactly `size` bytes plus the required CRLF,
   verifying the trailing `\r\n`.
4. For size 0, read trailers until the empty line terminator.

The implementation must not search for string markers such as `\r\n0\r\n`
inside the raw body, because binary chunk data can contain those bytes.
A line-oriented state machine is the only safe terminator strategy.

Body size is bounded by `max_body_size` across the full `out` buffer
(headers + chunk data + trailers), checked at each append.

## Hop-By-Hop Headers

Both request and response forwarding remove hop-by-hop headers:

- `Connection`
- `Keep-Alive`
- `Proxy-Authenticate`
- `Proxy-Authorization`
- `TE`
- `Trailer`
- `Transfer-Encoding`
- `Upgrade`

Headers named by `Connection` tokens are removed as well. Client `Host` is
replaced with the upstream host and port. `X-Forwarded-For`,
`X-Forwarded-Proto: http`, and `Via: 1.1 asio_owen` are added when
forwarding. Existing `X-Forwarded-For` is preserved by chaining, not
overwritten.

Chunked requests keep `Transfer-Encoding: chunked` and `Content-Length` is
stripped when any transfer encoding is present. The raw chunked body bytes
are forwarded as-is to the upstream verbatim, since the upstream expects the
same chunk framing the client sent. This is a request-side concern only — the
response-side framing is always re-owned by the gateway after de-chunking.

## Response Framing

Upstream response parsing follows RFC 7230 body length precedence:

1. Responses to `HEAD`, and status `1xx`, `204`, `304`, have no body.
2. `Transfer-Encoding: chunked` is parsed with the chunk state machine.
3. `Content-Length` reads exactly N bytes, preserving extra bytes in the
   connection's read buffer for the next response.
4. No length framing means read until upstream closes and do not reuse the
   connection.

Bad upstream status lines, invalid framing, timeouts, oversized bodies,
oversized headers (>64KB), and incomplete bodies mark the upstream connection
bad so it is not returned to the idle pool.

`Content-Length` and `Transfer-Encoding` are never forwarded as-is: the
gateway owns response framing after de-chunking and size limits, and
recomputes `Content-Length` from the body it actually has.

`status_text` (`201 Created`, `418 I'm a teapot`) is preserved end-to-end
through `HttpContext::response_status_text`. Hardcoding `OK` for any
non-200 code is forbidden.

The gateway writes responses to clients with a timeout. It controls
`Content-Length` and filters upstream hop-by-hop headers.

On timeout or read failure (client side or upstream side), the coroutine
`co_return`s from `handle_connection`, which closes the client socket and
aborts the current dispatch loop iteration. The upstream connection (if any
was acquired) is returned or released via `ConnGuard` RAII.

## HTTP/1.0 vs 1.1 Defaults

```cpp
int upstream_minor_version = status_line.rfind("HTTP/1.0", 0) == 0 ? 0 : 1;
conn.connection_close = header_state.connection_close ||
    (upstream_minor_version == 0 && !header_state.connection_keep_alive);
```

On the client side, the dispatch loop closes the connection when:

```cpp
HeaderTokens tokens = split_connection_tokens(ctx.get_header("Connection"));
if (minor_version == 0 && !tokens.keep_alive) break;   // HTTP/1.0 default close
if (tokens.close) break;                                // explicit close
```

Failing to detect HTTP/1.0 default-close caused repeated reuse of sockets
the upstream had already closed, surfacing as 502 storms after the first
idle timeout.

## HttpPool

`HttpPool` is lazy-created and bounded:

- `max_size`: total idle + active upstream sockets.
- `max_concurrent`: active request cap; `0` disables this extra limit.
- `max_body_size`: response/request body limit.
- `connect_timeout_ms`, `read_timeout_ms`, `request_timeout_ms`.
- `idle_timeout_sec`: lazy idle eviction age.

Each acquired connection is represented by a `unique_ptr<HttpConn>` and is
tracked in `State::active` **before** `acquire` returns. Tracking is done
inside the pool lock — both for the idle-reuse path and the freshly-created
path — to eliminate the window between `acquire` returning and `ConnGuard`
registering the connection. Without this, a shutdown during that window
orphaned a freshly connected socket and leaked a fd.

`ConnGuard` is non-copyable and non-movable. This guarantees that the
`HttpConn*` stored in `State::active` remains valid for the lifetime of
the guard — the address inside the `unique_ptr` does not change. The
destructor calls `untrack_active`, then `release` (good) or `release_bad`
(on `set_bad()`, exception, or `connection_close`).

Pool state is held through `shared_ptr<State>` so shutdown and in-flight
guards cannot access a destroyed mutex. The `HttpPool` destructor only
flips `running=false` and closes sockets; actual counter cleanup happens
when each in-flight `ConnGuard` is destroyed.

`release` refuses to return a connection to the idle queue when:

- `socket.is_open()` is false, or `connection_close` is set.
- `read_buffer.size() > 64KB`, to prevent pipeline pre-read bytes from
  unbounded accumulation across requests.

Either case closes the socket and decrements `total` and `in_flight`.

### Shutdown

1. `running.exchange(false)` — `acquire` returns nullptr from now on.
2. Lock the pool; close every idle socket; cancel+close every active
   socket. Cancelling active sockets causes their in-flight
   `async_read_some` to fail; the handling coroutine's `ConnGuard` then
   runs `release_bad` along the exception path.
3. The shared `State` is kept alive by every `ConnGuard` until its
   coroutine unwinds.

## Timeouts And Body Limits

| Direction | Operation | Timeout |
|---|---|---|
| Client | header read, body read | 30s |
| Client | header size | 64KB hard cap |
| Upstream | resolve | `connect_timeout_ms` |
| Upstream | connect | `connect_timeout_ms` |
| Upstream | write request | `request_timeout_ms` |
| Upstream | read response header + body | `read_timeout_ms` |
| Both | body size | `max_body_size` (10MB default) |

Every `async_read_some` and `async_write` is paired with a `steady_timer`.
On timeout, the timer callback calls `socket.cancel()` which causes the
pending async operation to complete with `operation_aborted`; the
coroutine then treats that as failure. Each operation gets a fresh timer
— no shared timer state across reads.

Body size is checked inside read loops, not only at the end. A malicious
upstream advertising a small `Content-Length` then streaming more bytes
must not exhaust memory.

## Pipeline Support

A single `async_read_some` on a keep-alive connection can return bytes
belonging to multiple pipelined requests. Two pre-read buffers preserve
extra bytes:

- `client_preread` (per `handle_connection`): persists across iterations
  of the dispatch loop, holds bytes after the current request's body.
- `conn->read_buffer` (per pooled `HttpConn`): persists across requests
  on the same upstream socket, holds bytes after the current response's
  body.

Both must be moved (not copied) when handing off to the next iteration to
avoid O(N²) buffer growth.

## Configuration

Gateway configuration lives in `config.ini`:

```ini
[http_pool]
max_size = 256
max_concurrent = 0
max_body_size = 10485760
connect_timeout_ms = 1000
read_timeout_ms = 30000
request_timeout_ms = 60000
idle_timeout_sec = 60

[upstream]
config = 127.0.0.1:30001
member = 127.0.0.1:30003
goods = 127.0.0.1:30006
order = 127.0.0.1:30009
```

`[http_pool]` settings are shared by all upstream pools. Per-service pool
overrides are not implemented. Upstreams load at startup; hot reload and
stricter service-name validation (`[a-z][a-z0-9-]*`) are design
follow-ups.

## Known Limitations

| Item | Note |
|---|---|
| HTTPS / TLS termination | Plaintext only; downstream TLS needs OpenSSL integration |
| Load balancing | One service = one host:port; no multi-instance rotation or circuit breaker |
| Retries | 502/503/504 returned to client as-is |
| Streaming bodies | Body fully buffered before forwarding; bounded by `max_body_size` |
| Connection prewarming | First request pays connect latency |
| Observability | `LOG_INFO`/`LOG_WARN` only; no QPS/p99/pool gauges |
| Hot reload | `[upstream]` changes require restart |

## Build And Test

```bash
cmake -B build -S .
cmake --build build --target server
ctest --test-dir build --output-on-failure
```

Third-party directories such as `asio/` and `spdlog/` are ignored by Git and
must be present locally. If they are restored from IDE Local History, verify
they are not zero-byte placeholder files before building.

## Risk History

Issues found during design and implementation review, kept as a regression
reference. Each row represents a concrete bug that existed in code or
design before being caught.

| Category | Problem | Fix |
|---|---|---|
| Smuggling | CL + TE both present; downstream used CL, gateway used TE | Strip CL when any TE present (request side); close connection (upstream side) |
| Smuggling | Multiple CL headers with different values | Detect `duplicate_content_length`, reject |
| Framing | `Content-Length: 0` triggered "read until EOF" | Use `optional<size_t>` to distinguish "no CL" from "CL=0" |
| Framing | Chunked terminated via `find("0\r\n\r\n")` string match — fails on trailer-less or binary chunk data | Line-oriented state machine |
| Header parsing | `substr(colon + 2)` assumed a space after colon | `trim_copy` on both sides |
| Header parsing | Invalid CL value caused `stoul` to throw uncaught | `parse_decimal_size` returns `optional` |
| Pool | Shutdown between `acquire` return and `ConnGuard` construction orphaned a socket | Track inside `acquire` while holding the pool lock |
| Pool | `ConnGuard` movable; `active` set held dangling pointers after move | Non-movable guard; `unique_ptr` keeps `HttpConn*` stable |
| Resource | Upstream response body had no size limit | In-loop + post-loop `max_body_size` checks |
| Slowloris | Client reads had no timeout | `read_with_timeout` with 30s budget |
| Protocol | HTTP/1.0 defaults treated as keep-alive | Detect `HTTP/1.0` prefix, default to close |
| Header parsing | `Connection: closed` substring-matched `close` | `split_connection_tokens` with exact comparison |
| Status line | `201 Created` rendered as `201 OK` | Preserve `status_text` end-to-end |
| Forwarding | `X-Forwarded-For` overwritten when client already sent one | Chain existing value with client IP appended |
| Pool | Pre-read bytes accumulated unbounded across requests on the same pooled connection | Drop connection if `read_buffer > 64KB` on release |
