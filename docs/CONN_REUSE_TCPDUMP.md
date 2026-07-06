# tcpdump 多重验证：HTTP 连接复用

> 目的：用一组 pcap 抓包，从 5 个不同维度交叉证明 `HttpPool` 的连接复用是真实工作的。
> 单看 TIME_WAIT=0 已经够强，但多维度证据能消除"会不会是某些巧合"的疑虑。

## 测试环境

参照 `docs/CONN_REUSE_VERIFY.md`：
- VM `192.168.139.230`，本地 lo 流量
- 网关端口 `8081`，Go 后端 `30001`（zebra-config）
- 压测工具 `/root/go/bin/plow`（也可用 wrk）

## 前置依赖

```bash
# 确认工具就位
which tcpdump tshark
# 没装的话：
apt install -y tcpdump tshark

# 确认网关 + 后端 + Redis 都在跑
curl -s http://127.0.0.1:8081/api/health
curl -s --max-time 3 http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey \
  -H 'Content-Type: application/json' \
  -d '{"appid":"member_03150715","config_key":"black_list"}' | head -c 100
echo
```

---

## 一次性抓包脚本

把整个验证流程封装成一个脚本，跑一次抓一段 30 秒 pcap，然后做 5 种分析。

```bash
# /tmp/tcpdump_reuse_verify.sh

#!/bin/bash
set -e

PCAP=/tmp/reuse_30001.pcap
DURATION=30
TARGET_PORT=30001            # Go 后端端口
GATEWAY_URL='http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey'
CONCURRENCY=50

echo "=== [1/6] 启动 tcpdump（端口 $TARGET_PORT，时长 ${DURATION}s） ==="
sudo rm -f $PCAP
sudo tcpdump -i lo -nn -tttt -w $PCAP "tcp port $TARGET_PORT" 2>/dev/null &
TCPDUMP_PID=$!
sleep 1

echo "=== [2/6] 触发压测（plow -c $CONCURRENCY -d ${DURATION}s） ==="
/root/go/bin/plow -c $CONCURRENCY -d ${DURATION}s \
  "$GATEWAY_URL" \
  -H 'Content-Type: application/json' \
  -m POST \
  -d '{"appid":"member_03150715","config_key":"black_list"}' \
  2>&1 | tail -20

echo "=== [3/6] 停止 tcpdump ==="
sudo kill -INT $TCPDUMP_PID 2>/dev/null
wait $TCPDUMP_PID 2>/dev/null || true
sleep 2

ls -lh $PCAP
echo "抓包完成，开始分析..."

echo ""
echo "=========================================="
echo "维度 1: SYN 数 vs HTTP 请求总数"
echo "=========================================="
REQ=$(tshark -r $PCAP -Y 'http.request and tcp.dstport == '"$TARGET_PORT" 2>/dev/null | wc -l)
SYN=$(tshark -r $PCAP -Y 'tcp.flags.syn == 1 and tcp.flags.ack == 0 and tcp.dstport == '"$TARGET_PORT" 2>/dev/null | wc -l)
echo "HTTP 请求总数（网关→后端方向）: $REQ"
echo "TCP SYN 数（新建连接数）: $SYN"
if [ "$REQ" -gt 0 ] && [ "$SYN" -gt 0 ]; then
  echo "复用率: $(awk "BEGIN {printf \"%.2f\", ($REQ-$SYN)*100/$REQ}") %"
  echo "  → > 95% = 完美复用"
  echo "  → 50~95% = 部分复用，需排查"
  echo "  → < 50% = 复用基本没工作"
fi

echo ""
echo "=========================================="
echo "维度 2: 每条 TCP 流承载的 HTTP 请求数（Top 20）"
echo "=========================================="
echo "右侧的数字 = 这条流上跑了多少个 HTTP 请求"
echo "→ 大于 10 = 复用正常"
echo "→ 等于 1 = 这条流没复用"
tshark -r $PCAP -Y 'http.request and tcp.dstport == '"$TARGET_PORT" 2>/dev/null \
  -T fields -e tcp.srcport -e tcp.dstport \
  | sort | uniq -c | sort -rn | head -20

echo ""
echo "=========================================="
echo "维度 3: TCP 流总数 vs HTTP 请求总数"
echo "=========================================="
FLOWS=$(tshark -r $PCAP -Y 'tcp and (tcp.srcport == '"$TARGET_PORT"' or tcp.dstport == '"$TARGET_PORT"')' 2>/dev/null \
  -T fields -e tcp.stream | sort -u | wc -l)
echo "独立 TCP 流总数: $FLOWS"
echo "HTTP 请求总数: $REQ"
if [ "$FLOWS" -gt 0 ]; then
  echo "平均每流承载请求: $(awk "BEGIN {printf \"%.1f\", $REQ/$FLOWS}") 个"
  echo "  → > 10 = 复用工作良好"
  echo "  → 接近 1 = 几乎没复用"
fi

echo ""
echo "=========================================="
echo "维度 4: FIN / RST 包数（连接关闭事件）"
echo "=========================================="
FIN=$(tshark -r $PCAP -Y 'tcp.flags.fin == 1 and (tcp.srcport == '"$TARGET_PORT"' or tcp.dstport == '"$TARGET_PORT"')' 2>/dev/null | wc -l)
RST=$(tshark -r $PCAP -Y 'tcp.flags.reset == 1 and (tcp.srcport == '"$TARGET_PORT"' or tcp.dstport == '"$TARGET_PORT"')' 2>/dev/null | wc -l)
echo "FIN 包数（优雅关闭）: $FIN"
echo "RST 包数（强制关闭）: $RST"
echo "  → 复用正常时，FIN 应 ≈ 2 × 流数（每条流两端各一个 FIN）"
echo "  → RST 应该接近 0，多了说明有异常断连"

echo ""
echo "=========================================="
echo "维度 5: 每条 TCP 流的存活时间分布"
echo "=========================================="
# 用 awk 算每条流的首包时间戳和末包时间戳，相减得到存活时长
tshark -r $PCAP -Y 'tcp and (tcp.srcport == '"$TARGET_PORT"' or tcp.dstport == '"$TARGET_PORT"')' 2>/dev/null \
  -T fields -e tcp.stream -e frame.time_epoch \
  | awk '
    {
      if (!($1 in start)) start[$1] = $2
      end[$1] = $2
    }
    END {
      for (s in start) {
        dur = end[s] - start[s]
        if (dur < 1) bucket = "  <1s   (短期/单次)"
        else if (dur < 5) bucket = "  1-5s  "
        else if (dur < 15) bucket = "  5-15s "
        else if (dur < 30) bucket = "  15-30s"
        else bucket = "  >30s  (长连接/复用)"
        count[bucket]++
      }
      for (b in count) print count[b], b
    }
  ' | sort -k2

echo ""
echo "  → 长连接（>5s）数量 ≈ HTTP 流数量 = 完美复用"
echo "  → 大量 <1s 流 = 每次请求都新建关闭，复用坏"

echo ""
echo "=========================================="
echo "维度 6: TIME_WAIT 系统级快照"
echo "=========================================="
echo "30001 端口的 TCP 状态分布："
ss -tan | awk '$4 ~ /:'"$TARGET_PORT"'$/ || $3 ~ /:'"$TARGET_PORT"'$/' \
  | awk '{print $1}' | sort | uniq -c | sort -rn

echo ""
echo "全系统 TIME-WAIT 总数："
ss -tan | grep TIME-WAIT | wc -l
echo "  → 复用正常 = 接近 0"
echo "  → 复用失败 = 数千到数万"
```

保存为 `/tmp/tcpdump_reuse_verify.sh` 然后：

```bash
chmod +x /tmp/tcpdump_reuse_verify.sh
sudo /tmp/tcpdump_reuse_verify.sh 2>&1 | tee /tmp/tcpdump_reuse_report.txt
```

---

## 结果判定矩阵

跑完后对照下表，**5 项全部命中 = 复用铁证**：

| 维度 | 复用正常的判定 | 复用失败的判定 |
|:---|:---|:---|
| 1. 复用率 | > 95% | < 50% |
| 2. Top 流的请求数 | 单流几十~几百个请求 | 单流都是 1 |
| 3. 平均每流请求数 | > 10 | ≈ 1 |
| 4. FIN/RST | FIN ≈ 2×流数，RST ≈ 0 | FIN ≈ 2×请求数，可能大量 RST |
| 5. 流存活时间 | 主要分布在 >5s 桶 | 主要分布在 <1s 桶 |
| 6. TIME_WAIT | 系统级接近 0 | 数千以上 |

---

## 进阶：单条连接的请求时间线（可视化复用过程）

如果想"看到"复用过程（同一条 TCP 上多个 HTTP 请求按时间排列）：

```bash
# 挑一个跑得最多的流，看它上面所有 HTTP 请求的时间分布
TOP_STREAM=$(tshark -r /tmp/reuse_30001.pcap -Y 'http.request and tcp.dstport == 30001' \
  -T fields -e tcp.stream 2>/dev/null | sort | uniq -c | sort -rn | head -1 | awk '{print $2}')

echo "考察 tcp.stream == $TOP_STREAM 上的所有 HTTP 请求："
tshark -r /tmp/reuse_30001.pcap \
  -Y "http.request and tcp.stream == $TOP_STREAM" \
  -T fields -e frame.time_relative -e http.request.method -e http.request.uri \
  2>/dev/null | head -50

echo ""
echo "该流首尾包间隔（秒）："
tshark -r /tmp/reuse_30001.pcap -Y "tcp.stream == $TOP_STREAM" \
  -T fields -e frame.time_epoch 2>/dev/null \
  | awk 'NR==1{first=$1} {last=$1} END{printf "%.2f 秒\n", last-first}'
```

输出会长这样（示例）：

```
0.000000    POST    /config.ConfigService/GetByAppAndKey
0.012345    POST    /config.ConfigService/GetByAppAndKey
0.024789    POST    /config.ConfigService/GetByAppAndKey
0.037012    POST    /config.ConfigService/GetByAppAndKey
...
29.876543   POST    /config.ConfigService/GetByAppAndKey
29.889012   POST    /config.ConfigService/GetByAppAndKey

该流首尾包间隔（秒）：29.89 秒
```

→ 一条 TCP 承载了 ~30 秒内数千个请求，**这就是复用的活生生证据**。

---

## 报告产出

执行 agent 把以下文件汇总输出到 `docs/results/CONN_REUSE_TCPDUMP_<日期>.md`：

1. 完整脚本输出 `/tmp/tcpdump_reuse_report.txt`
2. 原始 pcap 文件大小（不需要附 pcap，太大了）
3. 判定矩阵每项的实际数值 + 是否命中复用正常标准
4. 单条 Top 流的请求时间线（前 30 行）
5. 综合结论：复用工作 / 部分工作 / 失败

---

## 常见疑问

**Q: 为什么不抓 8081 端口（网关入站）？**
A: 那段是 client → 网关，client 是 plow 压测工具，它有自己的连接管理。我们关心的是 **网关 → Go 后端**这段，HttpPool 的复用是这一段的事。

**Q: 为什么用 lo 接口？**
A: 网关和 Go 后端都在同一台 VM（127.0.0.1），走 lo。如果是分布式部署，改成实际网卡的接口名（`eth0` 之类）。

**Q: SYN 数竟然不为 0，正常吗？**
A: 正常。压测开始时连接池要从空状态建几个连接（取决于并发数和 max_size 配置），所以会有 N 个 SYN。N 应该远小于 HTTP 请求总数，且不随时间增长。

**Q: 复用率刚好 100% 是不是有问题？**
A: 不一定。如果 idle pool 里所有连接都没被 LRU 回收，且后端一直保持连接，理论上可以 100%。但更常见是 95~99%，因为：
- 后端偶尔主动关连接（HTTP/1.1 max requests per connection 限制）
- HttpPool idle_timeout 触发清理
- 偶发的网络异常

如果**完全没复用**（0%），SYN 数 = HTTP 请求数，每个请求都新建连接。

**Q: RST 出现了几个，需要担心吗？**
A: 关键看比例。如果 RST < 总包数的 0.1%，正常网络抖动。如果 RST > 1%，说明连接在异常断开，需要排查：
- 后端是否设了 `MaxHeaderBytes` / `ReadTimeout` 触发关闭
- 是否有 proxy / firewall 在中间干预
- HttpPool 的 `read_buffer` 是否超 64KB cap 导致主动关连接（参考 `http_pool.hpp:268`）
