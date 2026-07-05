# 去掉 Nginx，ASIO 网关直接处理域名

> ⚠️ 本文档仅为讨论方案，最终评估收益不大，**不落地实施**。保留 Nginx 在当前链路中。

## 背景

当前链路：`客户端 → Nginx (api.zebra.com:80) → ASIO 网关 (127.0.0.1:8081) → zebra-* Go 服务`

Nginx 在链路中承担的职责：
1. 域名 `api.zebra.com` 监听 80 端口
2. CORS 跨域头（OPTIONS 预检 + 响应头注入）
3. `proxy_buffer_size 64k` 应对 Triple 协议大响应头
4. 透传 `Authorization`、`Content-Type` 等请求头
5. 透传客户端 IP（`X-Real-IP`、`X-Forwarded-For`）

去掉 Nginx 后，以上职责全部由 ASIO 网关接管。

---

## 改动清单

### 1. 配置层 — `config.ini` 新增 `[server]` 段配置

```ini
[server]
port = 80                    # 改为 80，直接监听 HTTP 端口
# 或者保留 8081，下层用 DNS/CNAME 指向
server_name = api.zebra.com  # 可选，不加则匹配所有 Host

# CORS 配置
cors_allowed_origins = *     # 允许的 Origin，多个逗号分隔
cors_allowed_methods = GET,POST,PUT,DELETE,OPTIONS
cors_allowed_headers = *
cors_expose_headers = *
cors_max_age = 86400
```

### 2. HTTP 解析层 — `http_server.hpp`

#### 2.1 域名路由（handle_connection 入口）

在解析完请求头后，读取 `Host` header：

```cpp
auto host = get_header_value(ctx.headers, "host");
// 当前只服务 api.zebra.com，其他域名返回 404
if (!host.empty() && host != "api.zebra.com" && host != "localhost:8081") {
    ctx.status_code = 404;
    ctx.response_body = R"({"code":404,"msg":"not found"})";
    handled = true;
}
```

### 2.2 OPTIONS 预检 + CORS 头（关键改动）

当前 Nginx 对 OPTIONS 不转发到网关，直接返回 204 + CORS 头。去掉 Nginx 后，OPTIONS 请求会打到网关。

当前网关行为：
- `security_rules` 放行（返回 `{0, ""}`）
- 走到路由匹配 → 路径 `/{任意}` 没有 OPTIONS 路由 → **404** ❌

修复：在 `security_rules` check 放行后、路由匹配之前，判断如果是 OPTIONS 就直接短路返回 204 + CORS 头，**不关心请求路径是什么**。

```cpp
// 在 security_rules check 之后、路由匹配之前
if (!handled && method_str == "OPTIONS") {
    ctx.status_code = 204;
    ctx.response_headers.emplace_back("Access-Control-Allow-Origin", "*");
    ctx.response_headers.emplace_back("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
    ctx.response_headers.emplace_back("Access-Control-Allow-Headers", "*");
    ctx.response_headers.emplace_back("Access-Control-Max-Age", "86400");
    handled = true;
}
```

#### 非 OPTIONS 请求的 CORS 头

所有正常响应注入 4 个跨域头：

```cpp
ctx.response_headers.emplace_back("Access-Control-Allow-Origin", "*");
ctx.response_headers.emplace_back("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
ctx.response_headers.emplace_back("Access-Control-Allow-Headers", "*");
ctx.response_headers.emplace_back("Access-Control-Expose-Headers", "*");
```

> 注意：生产环境应按域名区分，只对 `api.zebra.com` 注入跨域头。

#### 2.3 CORS 响应头注入

对所有响应注入跨域头（当前由 Nginx 的 `add_header` 完成）。在 `handle_connection` 写响应前增加：

```cpp
ctx.response_headers.emplace_back("Access-Control-Allow-Origin", "*");
ctx.response_headers.emplace_back("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
ctx.response_headers.emplace_back("Access-Control-Allow-Headers", "*");
ctx.response_headers.emplace_back("Access-Control-Expose-Headers", "*");
```

> 注意：需要**按域名区分**——生产环境只对 `api.zebra.com` 注入 CORS 头，对内网请求不注入。

### 3. 安全层 — 已有逻辑无需改动

- JWT 验证已在 `security_rules.hpp` 中完成
- IP 黑名单、限流等不变

### 4. OPTIONS 预检处理

当前 Nginx 对 OPTIONS 直接返回 204 + CORS 头，不转发到网关。去掉 Nginx 后，OPTIONS 会打到网关，当前行为：
- `security_rules` 放行（返回 `{0, ""}`）
- 但没有注册路由 → 最终返回 **404** ❌（应该是 204 + CORS 头）

修复：在 `handle_connection` 中，`security_rules` 放行后增加对 OPTIONS 的短路处理：

```cpp
// 在 security_rules check 之后、路由匹配之前
if (!handled && method_str == "OPTIONS") {
    ctx.status_code = 204;
    ctx.response_headers.emplace_back("Access-Control-Allow-Origin", "*");
    ctx.response_headers.emplace_back("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
    ctx.response_headers.emplace_back("Access-Control-Allow-Headers", "*");
    ctx.response_headers.emplace_back("Access-Control-Max-Age", "86400");
    handled = true;
}
```

### 5. CORS 响应头注入

所有非 OPTIONS 响应注入 4 个跨域头（当前 Nginx 的 `add_header` 干的事）：

```cpp
// 在写响应前统一注入
ctx.response_headers.emplace_back("Access-Control-Allow-Origin", "*");
ctx.response_headers.emplace_back("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
ctx.response_headers.emplace_back("Access-Control-Allow-Headers", "*");
ctx.response_headers.emplace_back("Access-Control-Expose-Headers", "*");
```

> 注意：生产环境应按域名区分，只对 `api.zebra.com` 注入跨域头。

### 6. 客户端 IP 透传 — 已有实现

`real_ip.hpp` 的 `get_client_ip` 已支持 `X-Forwarded-For` 解析。Nginx 之前注入的 `X-Real-IP` 在去掉 Nginx 后变成客户端直连 IP，`X-Forwarded-For` 由 ASIO 网关自己注入。

### 6. 超时与大小限制对齐

去掉 Nginx 后，ASIO 网关的超时和限制需要和 Nginx 当前配置对齐：

| Nginx 配置 | ASIO 配置 (`[http_pool]`) | 当前值 | 需要改为 |
|:-----------|:-------------------------|:------:|:--------:|
| `proxy_connect_timeout 60s` | `connect_timeout_ms` | 1000 | **60000** |
| `proxy_read_timeout 60s` | `read_timeout_ms` | 5000 | **60000** |
| `proxy_send_timeout 60s` | `request_timeout_ms` | 5000 | **60000** |
| `proxy_buffer_size 64k` | 无（直接读到 string） | 无限制 | ✅ 天然满足 |
| 请求头大小 | `kMaxHeaderSize` = 16384 | 16KB | ✅ 够用 |
| 请求体大小 | `max_body_size` | 10MB | ✅ 够用 |
| 无 | `idle_timeout_sec` | 60 | ✅ 保持 |

```ini
[http_pool]
connect_timeout_ms = 60000    # 对齐 Nginx proxy_connect_timeout 60s
read_timeout_ms = 60000       # 对齐 Nginx proxy_read_timeout 60s
request_timeout_ms = 60000    # 对齐 Nginx proxy_send_timeout 60s
```

### 7. 端口权限

Linux 上监听 80 端口需要 root 权限。当前 `asio-owen.service` 已经是 root 运行，所以可以直接改 port=80。

---

## 影响评估

| 维度 | 影响 |
|:-----|:------|
| **性能** | 去掉一层代理，减少 ~0.5ms 延迟，吞吐略微提升 |
| **安全** | 网关直接暴露 80 端口，攻击面不变（之前 nginx 也只做透传） |
| **部署** | 无需维护 Nginx 配置，少一个依赖 |
| **风险** | 如果 CORS 或 Host 校验有 bug，影响面比 Nginx 大（无法通过 reload nginx 快速切换） |

---

## 回滚方案

保留 Nginx 配置不动（`proxy_pass http://127.0.0.1:8081`）。如果 ASIO 网关的域名处理有问题，只需：
1. `nginx -s reload` 恢复流量
2. 把 ASIO 网关端口改回 8081

两分钟内可回滚。

---

## 不落地的原因

1. **收益微薄**：Nginx 只做透传，延迟 ~0.1ms，去掉后几乎无感知
2. **维护风险**：CORS / OPTIONS / Host 校验需要改代码，上线后 bug 回滚麻烦
3. **丧失灵活性**：保留 Nginx 可随时切回 pixiu 网关（改 `proxy_pass` 即可）
4. **大文件场景**：Nginx 的 tmp 缓冲是现成的，ASIO 网关需要自己实现流式读写

---

## 实施顺序（留档参考）

1. Config 层 — `[server]` 段加 `server_name`、CORS 配置；`[http_pool]` 超时对齐 Nginx
2. OPTIONS 预检 + CORS 头 — `handle_connection` 中 OPTIONS 短路返回 204 + CORS 头
3. 所有响应注入 CORS 头 — 写响应前统一注入 4 个头
4. 域名路由 — 读取 `Host` header，非 `api.zebra.com` 返回 404
5. 改端口 80 — 更新 `asio-owen.service`
6. 测试 — curl 验证各接口（Health / Config / CORS 预检）
7. 压测 — 去掉 Nginx 前后对比

---

## 不做的事

- **HTTPS**：Nginx 之前也没做，保持 HTTP
- **多域名**：当前只服务 `api.zebra.com`，不搞虚拟主机
- **速率限制**：Nginx 没配限流，ASIO 网关已有 `rate_limiter`
- **请求体大小限制**：当前 `http_pool` 已有 `max_body_size=10MB`
