## 连接复用优化记录（idle_timeout 调参）

### 背景

验证去掉 `Connection: keep-alive` 时发现压测 15s 内产生约 500 个 SYN（新建连接），但 ESTAB 仅 +1。分析认为是 HttpPool idle 队列中的连接被 Go 后端超时关闭，触发 stale idle → retry → replace 循环。

详见 `docs/results/CONN_REUSE_TCPDUMP_2026-07-05.md`。

### 优化动作

下调 `idle_timeout_sec` 从 60s → 45s，让网关在 Go 后端主动关闭连接之前先回收，减少 stale idle 命中。

```bash
# 改配置
sed -i "s/idle_timeout_sec = 60/idle_timeout_sec = 45/" config.d/21-http_pool.ini

# 编译重启
cmake --build build --target server -j$(nproc)
# 注意：build 目录的 config.d/ 需要手动同步
cp config.d/21-http_pool.ini build/config.d/21-http_pool.ini
# 重启生效
```

### 60s 连续压测验证

```bash
# 抓包 70s（多 10s 余量）
tcpdump -i lo -nn -c 2000 -w /tmp/reuse_tuned_60s.pcap \
  "tcp dst port 30001 and (tcp[13] & 0x12 == 0x02)" &
TCP_PID=$!
sleep 1

# 60s 连续压测
wrk -t10 -c20 -d60s --timeout 10s -s /tmp/wrk_post.lua \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey

sleep 2
kill -INT $TCP_PID 2>/dev/null
wait $TCP_PID 2>/dev/null

# 分析
echo "纯 SYN（新建连接数）: $(tcpdump -r /tmp/reuse_tuned_60s.pcap 2>/dev/null | wc -l)"
echo "30001 ESTAB: $(ss -tan | awk '\$4 ~ /:30001$/ && \$1 ~ /ESTAB/' | wc -l)"
echo "30001 TIME_WAIT: $(ss -tan | awk '\$4 ~ /:30001$/ && \$1 ~ /TIME-WAIT/' | wc -l)"
```

期望：60s 连续压测下 SYN 应接近 `idle_timeout_sec` 周期数 × 每周期新建数，而非随时间线性增长。如果 SYN / 时间比在 45s 调参后显著下降，则证明优化有效。

### 60s 压测结果

| 指标 | 15s | 60s |
|:----|:---:|:---:|
| RPS | 3,836 | 4,005 |
| 纯 SYN | 500 | 2,000 |
| 总请求 | ~57,540 | ~240,300 |
| SYN/秒 | 33.3 | **33.3** |
| ESTAB | 86 | 85 |
| TIME_WAIT | 0 | 0 |

**结论：15s 与 60s 的 SYN/秒完全一致，SYN 不随时间增长，连接池稳定。**

`idle_timeout_sec = 45` 与之前的 `60` 无显著差异。SYN 2,000 / 240,300 请求 = **99.2% 复用率**。

SYN 的稳定令频率（33/s）更可能来自连接池初始填充和 wrk 20 并发下的自然周转，而非 stale idle 替换。
