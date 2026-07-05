# 验证 asio 网关 ↔ Go 服务 HTTP 连接复用

## 背景

刚修复了 `src/http/http_server.hpp:1008` 处的 `Connection: keep-alive` 头透传 bug（dubbo-go 链路 HTTP/2 报 `invalid Connection request header`）。需要验证：

1. 网关到 Go 后端的 TCP 连接**确实在被复用**（HTTP/1.1 默认 keep-alive 仍生效）
2. `HttpPool` 的 idle 队列正常工作，不会每次请求都新建连接
3. 后端 Go 服务没主动发 `Connection: close` 把连接废掉

复用率应 > 95%。如果只有几十 %，说明 idle 队列或后端响应处理有问题。

## 测试环境

| 项目 | 值 |
|:-----|:----|
| 虚拟机 | `192.168.139.230`，6 核 / 15GB RAM，Ubuntu 22.04 |
| 帐密 | `root` / `123456` |
| 代码挂载 | `/mnt/mac/Users/mac/code/croot/asio_owen`（macOS NFS 自动同步） |
| 压测工具 | `/root/go/bin/plow` |
| 构建命令 | `cd /mnt/mac/Users/mac/code/croot/asio_owen && cmake --build build --target server -j$(nproc)` |
| 网关端口 | `8081` |
| Go 后端 | `30001`（zebra-config）, `30004`（zebra-cart）, `30005`（zebra-stock）等 |

## 前置条件

### 1. 构建最新代码

```bash
cd /mnt/mac/Users/mac/code/croot/asio_owen
cmake --build build --target server -j$(nproc)
```

确认编译通过。如果失败，停下排查。

### 2. 启动服务

```bash
# Redis
redis-cli ping || redis-server --daemonize yes

# 杀旧 server
pgrep -x server | xargs kill -9 2>/dev/null
sleep 2

# 启动新 server
cd /mnt/mac/Users/mac/code/croot/asio_owen/build
rm -f server.log
./server > /dev/null 2>&1 &
sleep 3

# 健康检查
curl -s http://127.0.0.1:8081/api/health
# 期望: {"code":0,"msg":"ok","data":"running"}
```

### 3. 确认 Go 后端在线

挑一个上游做验证目标（推荐 `zebra-config`，简单 GET 类接口，负载稳定）：

```bash
curl -s --max-time 3 http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}' | head -c 200
echo
# 期望: 有 JSON 响应，不是 connection refused
```

如果上面这个接口不可用，换 `/api/health` 走网关本地路由（但这种情况下没有上游调用，不适合验证复用）。**目标接口必须经过网关代理到 Go 后端**。

---

## 验证方法（按推荐顺序，至少跑两个交叉验证）

### 方法 A：`ss` 看 ESTABLISHED 连接数（最快，30 秒）

**原理**：如果连接在被复用，并发 100 个请求不会产生 100 条 ESTABLISHED；连接数会稳定在一个小值（≈ 活跃并发数）。

```bash
# 终端 1：压测开始前基线
ss -tn '( dport = :30001 or sport = :30001 )' | grep ESTAB | wc -l
# 记录这个值，比如 0 或 1

# 终端 2：跑 30 秒压测（同一接口，路径需要走 /{service}/...）
curl -s -o /dev/null -w '%{http_code}\n' --max-time 3 \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}'
# 期望 200，如果失败停下排查接口可用性

/root/go/bin/plow -c 50 -d 30s \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey \
  -H 'Content-Type: application/json' \
  -m POST \
  -d '{"appid":"member_03150715","config_key":"black_list"}' &

# 终端 1：压测中持续观察（每秒采样，跑 30 次）
for i in $(seq 1 30); do
  echo -n "$(date +%H:%M:%S) ESTAB: "
  ss -tn '( dport = :30001 or sport = :30001 )' | grep ESTAB | wc -l
  sleep 1
done

# 等压测结束
wait
```

**判定**：
- ✅ **复用正常**：压测期间 ESTABLISHED 数量稳定在 1~50 之间（受并发数影响），不会随时间无限增长
- ❌ **复用坏**：ESTABLISHED 数量接近并发数（50），或者持续增长不收敛

补充检查 TIME_WAIT 堆积：

```bash
ss -tan | awk '$4 ~ /:30001$/' | awk '{print $1}' | sort | uniq -c
# 关注 TIME-WAIT 数量
# 如果 TIME-WAIT 几万 → 没复用，每次都新建
# 如果 TIME-WAIT 接近 0 → 复用工作正常
```

---

### 方法 B：tcpdump 数 SYN vs HTTP 请求（最直接证据）

**原理**：一个 TCP SYN = 一次新建连接。如果 1000 个 HTTP 请求只看到几个 SYN，复用就是工作的。

```bash
# 终端 1：开始抓包（lo 上 30001 流量）
sudo tcpdump -i lo -nn -w /tmp/reuse_30001.pcap 'tcp port 30001' &
TCPDUMP_PID=$!
sleep 1

# 终端 2：跑已知数量的请求（比如 1000 个）
/root/go/bin/plow -c 20 -d 10s -N 1000 \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey \
  -H 'Content-Type: application/json' \
  -m POST \
  -d '{"appid":"member_03150715","config_key":"black_list"}'

# 停止抓包
sudo kill -INT $TCPDUMP_PID
wait $TCPDUMP_PID 2>/dev/null
sleep 1

# 分析
echo "=== 总 HTTP 请求数（网关→后端方向）==="
tshark -r /tmp/reuse_30001.pcap -Y 'http.request and tcp.dstport == 30001' 2>/dev/null | wc -l

echo "=== 总 SYN 数（新建连接数）==="
tshark -r /tmp/reuse_30001.pcap -Y 'tcp.flags.syn == 1 and tcp.flags.ack == 0 and tcp.dstport == 30001' 2>/dev/null | wc -l

echo "=== 复用率 ==="
REQ=$(tshark -r /tmp/reuse_30001.pcap -Y 'http.request and tcp.dstport == 30001' 2>/dev/null | wc -l)
SYN=$(tshark -r /tmp/reuse_30001.pcap -Y 'tcp.flags.syn == 1 and tcp.flags.ack == 0 and tcp.dstport == 30001' 2>/dev/null | wc -l)
if [ "$SYN" -gt 0 ]; then
  echo "scale=2; ($REQ - $SYN) * 100 / $REQ" | bc | xargs -I{} echo "{}% (目标 > 95%)"
fi

echo "=== 每条 TCP 流承载的 HTTP 请求分布（前 10）==="
tshark -r /tmp/reuse_30001.pcap -Y 'http.request and tcp.dstport == 30001' \
  -T fields -e tcp.srcport -e tcp.dstport 2>/dev/null \
  | sort | uniq -c | sort -rn | head -10
```

**判定**：
- ✅ **复用正常**：
  - 复用率 > 95%
  - 每条 TCP 流承载多次请求（10+）
  - SYN 数远小于 HTTP 请求总数
- ❌ **复用坏**：
  - SYN 数 ≈ HTTP 请求总数
  - 每条流只有 1 个请求

---

### 方法 C：HttpPool 内置统计（直接读计数器）

`HttpPool` 有 6 个原子计数器（`src/http/http_pool.hpp:60-65`）：

| 计数器 | 含义 |
|:------|:-----|
| `acquire_reused` | 累计：从 idle 队列复用成功次数 |
| `acquire_created` | 累计：新建连接次数 |
| `idle_probe_dropped` | 累计：取出来发现已断（socket 半关、对端 RST）次数 |
| `released_idle` | 累计：连接被放回 idle 队列次数 |
| `released_closed` | 累计：连接被关闭次数（后端发 Connection: close / HTTP/1.0） |
| `released_bad` | 累计：调用方标记 bad 的次数 |

**目前 stats 只在 error 路径打印**（`http_server.hpp:1079, 1083, 1099, 1105, 1136`）。两种方式拿到值：

#### C-1：触发一次错误路径

故意制造一次失败（比如压测中断开 Go 后端），日志会出现：

```
pool_stats={total=..., idle=..., active=..., in_flight=..., reused=NNN, created=NNN, ...}
```

#### C-2：临时加 stats 日志（推荐，更可靠）

修改 `src/http/http_server.hpp`，在 `handle_connection` 主循环里每 N 次请求打一次 stats。或者更简单——直接在响应返回前加一行（`http_server.hpp` 接近 1217 行 `co_await asio::async_write` 之后）：

```cpp
// 临时调试，验证完删掉
static thread_local size_t req_counter = 0;
if (++req_counter % 100 == 0) {
    LOG_INFO("HttpPool periodic stats: ", g_server->upstreams().pool_stats());
}
```

> **注意**：`UpstreamManager` 当前可能没有 `pool_stats()` 聚合接口，可能需要遍历每个 upstream 打印。如果改动复杂，跳过此方法，用方法 A/B 已够。

构建 + 重启 + 压测后，从 `build/server.log` grep：

```bash
grep "periodic stats" server.log | tail -20
```

**判定**：
- ✅ 复用率 = `reused / (reused + created)` > 95%
- ✅ `released_closed` 增长缓慢（如果增长快，说明 Go 后端在发 `Connection: close` 或回 HTTP/1.0）
- ⚠️ `probe_dropped` 偏高（> 5%）→ `idle_timeout_sec` 可能太长，对端已经把连接关了

---

### 方法 D：Go 服务侧交叉验证

如果 Go 服务有暴露 metrics（expvar / prometheus / 内部日志），看后端视角的连接统计。

```bash
# 看看 zebra-config 进程打开的 socket 数
ss -tnp | grep 30001 | grep ESTAB | wc -l

# 看看 Go 进程的 ConnState（如果代码里 instrumented 了）
# 通常需要 Go 端配合暴露 /debug/vars 或类似端点
curl -s http://127.0.0.1:30001/debug/vars 2>/dev/null | head -50
```

如果 Go 服务有 `net/http.Server` 的 ConnState 回调或 prometheus `http_server_connections_total` 指标，能直接看到 Accept 总数 vs 处理请求总数。

---

## 综合判定

跑完至少 **方法 A + 方法 B**：

| 检查项 | 通过条件 |
|:------|:------|
| ESTABLISHED 数量稳定 | 压测期间不无限增长，<并发数 × 2 |
| TIME_WAIT 数量 | < 100（说明连接在被复用，不是用完就关） |
| 复用率（方法 B） | > 95% |
| 每条流平均请求数（方法 B） | > 10 |
| HttpPool::acquire_reused 增长 | 大于 acquire_created（如能拿到） |

**全部满足 → 修复验证通过**。

任意一项不达标 → 进入下面排查。

---

## 复用坏了的排查清单

如果发现复用率低，按顺序检查：

### 1. Go 后端是否在发 `Connection: close`

```bash
# 用 nc / curl -v 看响应头
curl -sv http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}' 2>&1 | grep -i 'connection\|http/'
```

如果看到 `Connection: close` 或 `HTTP/1.0`，问题在 Go 服务端：
- Go 的 `http.Server` 默认 keep-alive，但被某个 middleware / handler 强制 `w.Header().Set("Connection", "close")`
- 或 `SetKeepAlivesEnabled(false)`
- 或 Go 服务的 `WriteTimeout` 太短触发 `http: timeout` 后关闭

### 2. 网关是否在错误标记 `connection_close`

`src/http/http_server.hpp:572-573`：

```cpp
conn.connection_close = header_state.connection_close ||
    (upstream_minor_version == 0 && !header_state.connection_keep_alive);
```

如果响应解析逻辑误判（比如某条响应头没正确解析），会让连接被错误标记 close。

排查：开 DEBUG 日志（`config.ini` 把 logger level 调到 `debug`），grep：

```bash
grep "connection_close=1" build/server.log | head -20
grep "Proxy response header parsed" build/server.log | head -5
```

### 3. HttpPool idle_timeout 配置

`config.ini` 的 `[http_pool]` 段：

```ini
[http_pool]
max_size = 256
max_concurrent = 0
connect_timeout_ms = 1000
read_timeout_ms = 30000
request_timeout_ms = 60000
idle_timeout_sec = 60   # 太短会频繁回收空闲连接
```

`idle_timeout_sec` 默认 60 秒。如果 Go 后端的 keep-alive timeout 短于这个（比如 Nginx 默认 60s，有些服务 15s），会出现"网关以为还能用，对端已经关了"——`probe_dropped` 会涨。

排查：对比 Go 后端的 keep-alive timeout（Go `http.Server.IdleTimeout`），让网关 idle_timeout 比它短 5~10 秒。

### 4. 是否有 socket 错误未被捕获

`HttpPool::is_reusable_idle`（`http_pool.hpp`）会做 SO_ERROR 探测，但某些情况（对端发 RST 后立即 close）可能没及时反馈。

排查：开 DEBUG，看 `idle_probe_dropped` 是否快速上涨：

```bash
grep "HttpPool periodic stats" build/server.log | tail -10
# 看 probe_dropped 的增量
```

---

## 清单：执行后产出

执行 agent 跑完后应该输出：

1. **方法 A 的 ESTAB 时间序列**（30 个采样点）
2. **方法 A 的 TIME_WAIT 统计**
3. **方法 B 的 pcap 分析结果**（HTTP 请求总数、SYN 总数、复用率、每流请求数 Top 10）
4. **方法 C 的 pool_stats 输出**（如果加了临时日志）
5. **综合判定**：通过 / 不通过 + 不通过时的排查清单命中项
6. **`server.log` 中所有 WARN/ERROR** 的 grep 结果（确认无新增异常）

把结果汇总到 `docs/results/CONN_REUSE_VERIFY_<日期>.md`。
