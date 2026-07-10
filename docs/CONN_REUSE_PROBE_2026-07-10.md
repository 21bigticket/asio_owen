# HttpPool idle probe 误判修复报告

> 验证日期：2026-07-10  
> 目标服务：zebra-config，127.0.0.1:30001  
> 结论：连接复用问题根因在网关 `HttpPool::is_reusable_idle` 的 probe 错误码被覆盖，不在上游 Go 服务，也不在 `Connection: keep-alive` 请求头。

## 背景

压测网关路径：

```text
POST /zebra-config/config.ConfigService/GetByAppAndKey
```

现象：

```text
reused=0
created 持续按请求数增长
probe_dropped 持续按请求数增长
released_idle 持续按请求数增长
released_closed=0
released_bad=0
```

典型旧数据：

| 配置 | reused | created | probe_dropped | released_idle |
|:-----|------:|--------:|--------------:|--------------:|
| `send_keep_alive_header=false` | 0 | 400k | 400k | 400k |
| `send_keep_alive_header=true` | 0 | 53k | 53k | 53k |

这说明连接被成功用于一次请求并回到 idle 池，但下一次 `acquire` 时又被 probe 判定不可复用，然后关闭并新建。

## 排除项

### 1. 不是上游响应 `Connection: close`

直连 zebra-config，带和不带 `Connection: keep-alive`，响应都没有 `Connection: close`：

```text
< HTTP/1.1 200 OK
< Accept-Encoding: gzip
< Content-Type: application/json
< Date: Fri, 10 Jul 2026 03:52:32 GMT
< Content-Length: 81
```

HTTP/1.1 默认 keep-alive，因此这个响应本身是可复用语义。

### 2. 不是上游依赖显式 `Connection: keep-alive`

打开网关开关：

```ini
[http_pool]
send_keep_alive_header = true
```

结果 `reused` 仍然是 0，`probe_dropped` 仍然与 `released_idle` 基本 1:1 增长。因此显式请求头不是根因。

### 3. 直连 curl 可以复用

同一个 curl 进程内连续请求直连 30001，可观察到连接复用。说明 zebra-config 的 HTTP/1.1 keep-alive 行为正常。

## 根因

旧代码：

```cpp
asio::error_code ec;
bool was_non_blocking = conn.socket.non_blocking();
conn.socket.non_blocking(true, ec);
if (ec) return false;

char byte = 0;
size_t n = conn.socket.receive(
    asio::buffer(&byte, 1), asio::socket_base::message_peek, ec);
conn.socket.non_blocking(was_non_blocking, ec);

if (!ec) return n > 0;
if (ec == asio::error::would_block || ec == asio::error::try_again) return true;
return false;
```

健康的 idle keep-alive socket 上没有数据可读，非阻塞 `peek` 的正确结果是：

```text
recv_ec = would_block 或 try_again
```

这表示连接仍然存活，只是当前没有上游字节可读，应该判定可复用。

旧代码的问题是：`receive(..., ec)` 之后，又用同一个 `ec` 调用 `non_blocking(was_non_blocking, ec)`。恢复 non-blocking 状态成功会把 `ec` 覆盖成 success。于是原本的 `would_block` 丢失，后续逻辑看到的是：

```text
ec = success
n = 0
```

最终走到：

```cpp
if (!ec) return n > 0;  // false
```

也就是把正常 idle 连接误判成 EOF/不可复用，导致：

```text
released_idle++
下一次 acquire probe false
probe_dropped++
created++
reused 永远为 0
```

## 修复

修复点：`receive` 和恢复 socket blocking mode 使用不同的 error_code。

```cpp
asio::error_code recv_ec;
size_t n = conn.socket.receive(
    asio::buffer(&byte, 1), asio::socket_base::message_peek, recv_ec);

asio::error_code restore_ec;
conn.socket.non_blocking(was_non_blocking, restore_ec);
if (restore_ec) return false;

if (!recv_ec) return n > 0;
if (recv_ec == asio::error::would_block || recv_ec == asio::error::try_again) return true;
return false;
```

语义：

| probe 结果 | 判定 |
|:-----------|:-----|
| `recv_ec == would_block` | socket 存活且当前无数据，可复用 |
| `recv_ec == try_again` | socket 存活且当前无数据，可复用 |
| `recv_ec == success && n > 0` | 已有预读字节，可复用 |
| `recv_ec == success && n == 0` | EOF，不可复用 |
| 其他错误 | 不可复用 |
| 恢复 non-blocking 失败 | 不可复用 |

## 验证

### 修复前

wrk 参数：

```bash
wrk -t15 -c50 -d60s --latency -s /tmp/post.lua \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey
```

结果：

```text
Requests/sec: 987.67
P50: 39.00ms
P99: 200.87ms
```

旧进程 stats：

```text
zebra-config={total=108, idle=64, active=44, in_flight=44,
reused=0, created=53573, probe_dropped=53465,
released_idle=53529, released_closed=0, released_bad=0}
```

### 修复后

确认事项：

```text
src/http/http_pool.hpp mtime: 2026-07-10 11:55
build/server mtime: 2026-07-10 12:00
```

重建并重启后，3 分钟稳态 wrk 参数：

```bash
wrk -t15 -c50 -d180s --latency -s /tmp/post.lua \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey
```

结果：

```text
282728 requests in 3.00m
Requests/sec: 1569.90
P50: 26.44ms
P90: 51.33ms
P99: 101.38ms
```

新进程 stats：

```text
zebra-config={total=112, idle=68, active=44, in_flight=44,
reused=468825, created=225, probe_dropped=0,
released_idle=469006, released_closed=0, released_bad=0}
```

对比：

| 指标 | 修复前 | 修复后 |
|:-----|------:|------:|
| RPS | 987.67 | 1569.90 |
| 总请求 | 59311 | 282728 |
| P50 | 39.00ms | 26.44ms |
| P90 | 99.14ms | 51.33ms |
| P99 | 200.87ms | 101.38ms |
| reused | 0 | 468825 |
| created | 53573 | 225 |
| probe_dropped | 53465 | 0 |

补充：修复刚上线后的 60 秒 smoke 测试曾达到 `Requests/sec=4823.80`、`P99=24.69ms`、`reused=289782`、`created=113`、`probe_dropped=0`。不同压测窗口和上游负载会影响 RPS/延迟绝对值；连接复用是否恢复应优先看 `reused` 持续上涨、`created` 稳定在池容量量级、`probe_dropped=0`。

注意：历史文档中已有 Config Gateway 短压达到 8.8k-11k RPS 的记录（例如 `docs/VERIFY_2026-07-06.md`、`docs/VERIFY_2026-07-07.md`、`docs/VERIFY_2026-07-08.md`）。因此，本次修复不能表述为“首次达到 9k 短压”。本次结论应限定为：修复了当前旧二进制/回归状态下 `reused=0`、`probe_dropped≈released_idle` 的连接复用失效问题；性能绝对值受压测窗口、上游负载和同机资源竞争影响，不能单独作为复用是否正常的证据。

## tcpdump / tshark stream 级确认

为了确认 `HttpPool` 指标与真实 TCP 连接复用一致，又做了一轮抓包验证。

压测与抓包命令：

```bash
PCAP=/tmp/reuse_fixed.pcap
rm -f $PCAP
tcpdump -i lo -nn -w $PCAP "tcp port 30001" 2>/dev/null &
TPID=$!
sleep 2

wrk -t15 -c50 -d60s --latency -s /tmp/post.lua \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey

sleep 1
kill -INT $TPID
wait $TPID 2>/dev/null

tshark -r $PCAP -Y "http.request and tcp.dstport == 30001" \
  -T fields -e tcp.stream \
  | sort | uniq -c | sort -rn | head -20

tshark -r $PCAP -Y "http.request and tcp.dstport == 30001" \
  -T fields -e tcp.stream \
  | sort -u | wc -l

tshark -r $PCAP -Y "http.request and tcp.dstport == 30001" | wc -l
```

wrk 结果：

```text
109853 requests in 1.00m
Requests/sec: 1827.67
P50: 22.31ms
P90: 46.42ms
P99: 95.75ms
```

tshark 结果：

| 指标 | 修复前旧二进制 | 修复后新二进制 |
|:-----|---------------:|---------------:|
| 总 TCP stream 数 | 60922 | 113 |
| 总 HTTP request 数 | 60922 | 109853 |
| Top stream 承载请求数 | 1 | 1047-1116 |
| 平均每 stream 请求数 | 1 | ~972 |
| HttpPool `reused` | 0 | 296680+ |

Top 20 stream 的请求数均超过 1000：

```text
1116 120
1112 152
1103 159
1101 158
1098 187
1097 129
1093 161
1088 108
1086 163
1084 121
1078 197
1066 115
1063 128
1062 136
1059 193
1056 206
1056 184
1050 183
1050 111
1047 154
```

这个结果把两类数据对齐了：

- tshark 看到约 113 条 TCP stream。
- HttpPool stats 里 `total` 稳定在约 112-113。
- 约 11 万个 HTTP 请求被这些 stream 承载，单条 stream 可承载约 1000 个请求。
- `probe_dropped=0`，`reused` 持续上涨。

因此，修复后不仅计数器显示复用恢复，抓包层面也确认 TCP 连接被真实复用。

## 回归测试

新增测试：

```text
HttpPool.ReusesIdleConnectionWhenProbeWouldBlock
ProxyForwarder.DoesNotSendKeepAliveHeaderByDefault
ProxyForwarder.SendsKeepAliveHeaderWhenEnabled
ConfigLoad.ParsesBoolValues
```

验证命令：

```bash
ctest --test-dir build_codex_verify -R "HttpPool|ProxyForwarder|ConfigLoad|ProxyFraming" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 13
```

## 运维注意

1. 连接复用相关代码改动后，必须确认运行中的二进制已重建。只看源码不够。
2. 如果从仓库根目录启动 `./build/server`，默认日志路径是 `server.log`；如果从 `build/` 目录启动，日志路径可能是 `build/server.log`。
3. 判断复用是否正常，优先看 `reused`、`created`、`probe_dropped`、`released_idle` 的关系：
   - 正常：`reused` 持续上涨，`created` 只在池填充或重连时上涨，`probe_dropped` 低。
   - 异常：`reused=0`，`created≈released_idle≈probe_dropped`。
4. `send_keep_alive_header` 是诊断/兼容开关，默认应保持 `false`。本次修复后的复用恢复不依赖该开关。
