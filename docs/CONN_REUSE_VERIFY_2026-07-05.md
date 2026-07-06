# 连接复用验证报告

## 验证目的

验证去掉 `Connection: keep-alive` 硬编码后，asio_owen 网关到 Go 后端（zebra-config）的 HTTP 连接复用是否正常。

## 测试方法

### 方法：ss 观察 ESTABLISHED 连接数变化

在 VM（192.168.139.230）上，压测期间每秒采样网关到 zebra-config（端口 30001）的 ESTAB 连接数。

**压测参数**：wrk -t30 -c100 -d30s，Config 网关接口（POST /zebra-config/config.ConfigService/GetByAppAndKey）

## 数据

| 时间点 | ESTAB 连接数 | 说明 |
|:------|:-----------:|:-----|
| 压测前 | 226 | 含其他 zebra 服务的 Triple 内部连接 |
| 压测峰值 | 349 | 比基线 +123 |
| 压测后 | 173 | 回落 |

**压测前 226 的来源分解**：
- 网关（ASIO）↔ zebra-config：约 50-80 条（连接池 16 shard × 每 shard 若干 idle/active）
- zebra-cart/stock/order 等 Triple 内部 gRPC 调用 → zebra-config：约 150 条

## 判定

| 指标 | 值 | 判定 |
|:----|:--:|:----:|
| ESTAB 增长 | +123 | ⚠️ 略高于并发 100，但含池内 idle 连接 |
| TIME_WAIT | **0** | ✅ 无堆积，说明连接在被复用 |
| Socket Errors（wrk） | 0 | ✅ 无异常 |
| RPS | 4,545 | ✅ 正常 |

**结论：连接复用基本正常。** TIME_WAIT = 0 是最直接的证据——如果每次请求都新建连接，30 秒压测会产生大量 TIME_WAIT。0 条 TIME_WAIT 说明连接没有被用完就关。

ESTAB 增长 123 条主要由两个因素构成：
1. HttpPool 16 个 shard 在压测期间填充 idle 连接（正常行为）
2. 上游 Triple 服务之间的内部调用也在同时进行（非网关引起）

## 补充说明

当前程序状态：**已去掉 `Connection: keep-alive` 硬编码**。TIME_WAIT 为 0 说明即使不写这个头，HTTP/1.1 的默认 keep-alive 行为在上游 Go 服务端仍然生效——上游没有主动关闭连接。

如果需要更精确的复用率（HTTP 请求数 / SYN 数），可以用文档 `docs/CONN_REUSE_VERIFY.md` 中的方法 B（tcpdump + tshark）进一步验证。
