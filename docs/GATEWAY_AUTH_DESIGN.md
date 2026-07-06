# 网关鉴权与安全模块设计

## 目标

为 ASIO HTTP 网关添加鉴权和访问控制能力，支持：

- IP 黑名单（按来源 IP 或 CIDR 段拦截）
- JWT 鉴权（验证 Bearer token 签名与有效期，可选角色检查）
- 路由黑名单（按路径或上游服务禁止访问）
- 免鉴权白名单（特定路径或服务可跳过 JWT 验证）
- 真实 IP 获取（支持网关直连和前置 LB 两种部署模式）

## 设计原则

1. **尽早拒绝**：鉴权和黑名单检查在 body 消费之前执行，减少不必要的资源消耗。
2. **可配置**：所有规则通过 `config.ini` 配置，不改代码即可调整安全策略。
3. **低侵入**：安全中间件是 check 函数链，不改变现有路由和转发逻辑。
4. **可观测**：拒绝的请求记录日志，包含原因和客户端信息。

## 执行顺序

> **设计原则：** 尽早拒绝，减少资源浪费。当前实现为保持 keep-alive/pipeline 正确性，先完整消费 body 再执行安全链，拒绝后强制 `Connection: close`。

```
客户端请求
  ↓
body 消费（含 chunked de-chunk）
  ↓
① 真实 IP 解析            → 从 XFF 取客户端真实 IP
  ↓
② IP 黑名单检查            → 403 Forbidden
  ↓
③ 路径规范化              → percent-decode + 去点段 + 统一大小写策略 + 剥离 query string
  ↓
④ 免鉴权白名单匹配         → 跳过 JWT 验证
  ↓
⑤ JWT 验证（非白名单）     → 401 Unauthorized
  ↓
⑥ 路由黑名单检查           → 403 Forbidden
  ↓
路由分发（本地 / 代理）
```

> **为什么 body 消费在先：** 如果先鉴权再消费 body，拒绝后 body 残留管道中，会被后续 pipelined 请求误读为下一个请求的起始。因此必须先完整消费 framing 确定边界，再执行安全链。代价是大 body 的无权限请求仍需先读完。

> **🔴 关键修正：** 真实 IP 解析必须在 IP 黑名单之前。有 LB 时 `socket.remote_endpoint()` 是 LB IP，必须先解析出客户端真实 IP 才能做黑名单判断。

---

## 风险分级

### 🔴 Critical（可直接绕过鉴权）

#### C1. 执行顺序：IP 黑名单在真实 IP 解析之前（已修）

**风险：** 有 LB 时 `socket.remote_endpoint()` 是 LB IP，不是客户端 IP。黑名单打的是 LB IP，攻击者真实 IP 在黑名单里也照样通过。

**修正：** 执行顺序改为 真实 IP 解析 → IP 黑名单。见上方流程图。

#### C2. XFF 取第一个 IP 可被伪造

**风险：** `X-Forwarded-For: 1.2.3.4, proxy1, proxy2` 最左边是客户端自报的。攻击者从 `trusted_proxies` 网段内的机器发请求，头里写任意 IP，网关就认为"真实 IP"是攻击者编的。

**修正：** 从右往左扫，跳过所有在 `trusted_proxies` 内的 IP，取第一个不在信任列表里的。全在信任列表内才取最左边。这是 Nginx realip 模块的标准算法。完整代码见下文「真实 IP 获取」节。

> **注意：** C2 早期版本在 `ips.front()` 全为空段时返回了空字符串。修正版在循环后兜底返回 `direct_ip`，见下文代码。

#### C3. trusted_proxies 网段太大

**风险：** 默认 `192.168.0.0/16` + `10.0.0.0/8` + `172.16.0.0/12` 等于整个 RFC1918 都是可信。任何在这几个网段里的容器/VM/同 VPC 服务都能伪造 XFF，IP 黑名单基本作废。

**修正：** `trusted_proxies` 收敛到具体 LB/反代的几个 IP，不要整段。

```ini
[trusted_proxies]
# 只写具体的 LB IP，不要用整段 RFC1918
192.168.1.10
192.168.1.11
10.0.0.5
```

#### C4. 路径匹配没有规范化

**风险：** `needs_auth` 用 `path.find(prefix) == 0`，但路径在到达前可能未做：

- URL 解码：`/api/%70ublic/admin` 解码后是 `/api/public/admin`（命中白名单），应被拦截
- 点段：`/api/public/../admin` 可能绕过白名单
- 大小写：`/API/health` vs `/api/health`
- //、./、../ 等未规范化

**修正：** 进入匹配前先规范化路径，然后所有黑/白名单都基于规范化后的 path。

```cpp
std::string normalize_path(std::string_view raw) {
    // 0. 先按 ? 剥离 query string
    // 1. percent-decode
    // 2. 去掉 // → /
    // 3. 解析 . 和 .. （不能超出 root）
    // 4. 统一小写（可配置）
    // 5. 去掉末尾 /（除非 path 就是 /）
}
```

#### C5. JWT 算法未显式锁定

**风险：** 未显式拒绝 `alg: none` 和算法切换攻击（用 RSA 公钥当 HMAC 密钥）。

**修正：** `jwt-cpp` 必须显式列出允许的算法，不能由库自行推断。

```cpp
auto decoded = jwt::decode(token);
auto verifier = jwt::verify()
    .allow_algorithm(jwt::algorithm::hs256{secret})
    .with_issuer(issuer_)
    .leeway(60);  // ±60s 时钟偏移容差
verifier.verify(decoded);
```

> 禁止 `alg: none`，仅接受 `[security].jwt_algorithm` 配置的算法。

---

### 🟠 High（逻辑缺陷）

#### H1. 热加载实现：细粒度 mutex + shared_ptr 替换

**风险：** 当前实现不是 `atomic<shared_ptr>` 无锁读，而是 `rules_mu_` + 内部子模块各自锁。

**当前实现：**

```cpp
class SecurityRules {
    mutable std::mutex rules_mu_;
    IpBlacklist ip_blacklist_;
    AuthWhitelist auth_whitelist_;
    PathBlacklist path_blacklist_;
    std::shared_ptr<JWTAuth> jwt_auth_;

    void reload(const Config& cfg) {
        std::lock_guard<std::mutex> lock(rules_mu_);
        load_from_config(cfg);
    }

    CheckResult check(...) const {
        std::lock_guard<std::mutex> lock(rules_mu_);
        // 复制 proxies_copy 和 jwt_copy，然后释放锁
        // 子模块内部（IpBlacklist / AuthWhitelist / etc）各自有 mutex
    }
};
```

读侧有少量锁（`rules_mu_` + 子模块 mutex），但锁持有时间极短（仅复制 shared_ptr 和简单查询）。在 Config 免鉴权场景下，热路径上没有 JWT 验证，锁竞争对 RPS 影响有限。

#### H2. 解析失败的回退行为未定义

**风险：** 配置/header 解析出非法值时的行为不确定。

**规则：**

| 场景 | 行为 |
|------|------|
| XFF 含非法 IP（`foo, bar`、空段） | 跳过该段，继续解析下一个 |
| IP 黑名单配置错误（如 `10.0.0/8` 缺一段） | 跳过该条规则，不影响其他规则 |
| JWT Authorization 头格式错误 | 返回 401，不放过 |
| 配置段完全无法解析 | 保留旧规则，打 WARN 日志 |

#### H3. IPv6-mapped IPv4 未处理

**风险：** `::ffff:10.0.0.1` 是 v6 地址，CIDR 规则 `10.0.0.0/8` 是 v4，两者不匹配，可绕过 IP 黑名单。

**修正：** 统一调用 `normalize_ip()`，对 IPv6-mapped IPv4 调用 `to_v4()` 后再匹配。

```cpp
asio::ip::address normalize_ip(const asio::ip::address& addr) {
    if (addr.is_v6() && addr.to_v6().is_v4_mapped()) {
        auto b = addr.to_v6().to_bytes();
        asio::ip::address_v4::bytes_type v4{{b[12], b[13], b[14], b[15]}};
        return asio::ip::make_address_v4(v4);
    }
    return addr;
}

// normalize_ip_str：将 IP 地址规范化后转为字符串
// 对 IPv6-mapped IPv4（如 ::ffff:10.0.0.1）统一转为 "10.0.0.1"
std::string normalize_ip_str(const asio::ip::address& addr) {
    return normalize_ip(addr).to_string();
}

std::string normalize_ip_str(const std::string& ip_str) {
    asio::error_code ec;
    auto addr = asio::ip::make_address(ip_str, ec);
    if (ec) return ip_str;  // 非法的 IP 字符串原样返回
    return normalize_ip(addr).to_string();
}
```

#### H4. JWT 时钟偏移 / 过期边界

| 参数 | 策略 |
|:---|------|
| `exp` | 校验，容忍 ±60s 时钟偏移 |
| `nbf` | 校验，容忍 ±60s |
| `iat` | 不强制要求存在 |

#### H5. 服务名抽取规则缺失

`/{service}/...` 的 service 抽取规则：

| 输入 | service | 剩余 path |
|:---|:---|:---|
| `/foo/bar` | `foo` | `/bar` |
| `/foo` | `foo` | `/` |
| `/foo/` | `foo` | `/` |
| `/` | (空) | root，直接返回 404 不进鉴权 |
| `//foo` | 非法 | 返回 400 |
| `/foo%2Fbar` | `foo%2Fbar`（%2F 不解码） | `/bar` |

> 服务名只允许 `[a-z][a-z0-9-]*`，不符合返回 400。
> %2F 在路径规范化时不解码，保留为字面量，不作为路径分隔符。其余百分号编码正常解码。

#### H6. keep-alive 下鉴权状态

**每请求鉴权，禁止在 connection 级别缓存"已鉴权"状态。** 同一 TCP 连接上的多个请求路径不同，鉴权结果可能不同。

---

### 🟡 Medium（健壮性）

#### M1. 没有 token 失效/吊销机制

JWT 无状态 → 泄露的 token 在 exp 前一直可用。**当前不接受 token 吊销，泄露后只能等 token 过期或更换 secret。** 如需吊销能力，后续引入 token blacklist（Redis 或本地 bloom filter）。

#### M2. OPTIONS / HEAD 未特殊处理

CORS 预检（OPTIONS）不带 `Authorization`，会被 401。**OPTIONS 默认加入免鉴权白名单。**

#### M3. 配置文件 mtime 粒度问题

`mtime` 在某些文件系统（overlayfs、NFS、ext3）精度为 1 秒，1 秒内多次改可能漏掉。**改为同时记录 size + mtime 做 fingerprint。**

#### M4. 日志可能泄露敏感信息

`path` 含 query string 可能带 `?token=...`。**日志前剥掉 query string，只记录 path 部分。**

#### M5. 拒绝日志级别

安全拒绝事件至少 `LOG_WARN`。JWT 验签失败连发、黑名单频繁命中同一 IP 应考虑 `LOG_ERROR` + 触发限流。

---

### 🔵 设计层面缺失

| 项 | 说明 |
|:---|------|
| 限流 | 合法 JWT 也能 DoS，当前设计未包含 |
| 审计 | 只记拒绝不记敏感操作的允许，事后查不到 |
| TLS | HTTP 明文下 JWT 可被中间人嗅探，建议前置 TLS |
| TraceId | 鉴权链路没有贯穿的 request id，排障难 |
| 测试 | 缺少 fuzz/负向用例（XFF 伪造、路径绕过、JWT none alg） |

---

## 真实 IP 获取

### 问题

网关 `socket.remote_endpoint()` 获取的 IP 取决于部署架构：

```
直连：客户端 → 网关                    → remote_endpoint = 客户端真实 IP
有 LB：客户端 → Nginx/ELB → 网关      → remote_endpoint = LB 的 IP
```

### 算法

Nginx realip 模块标准算法：从右往左扫 XFF，跳过可信代理，取第一个非信任 IP。

![XFF parsing flow](XFF 解析从右往左跳过可信代理取第一个非信任IP全在信任列表则取最左边)

```cpp
std::string get_client_ip(
    asio::ip::tcp::socket& socket,
    HttpContext& ctx,
    const TrustedProxyConfig& trusted_proxies) {

    auto direct_ip = normalize_ip_str(socket.remote_endpoint().address());

    if (!is_ip_in_cidrs(direct_ip, trusted_proxies.cidrs)) {
        return direct_ip;  // 直连，无需解析 XFF
    }

    auto xff = ctx.get_header("X-Forwarded-For");
    if (xff.empty()) return direct_ip;

    // 从右往左遍历，跳过可信代理 IP
    std::vector<std::string> ips = split_xff(xff);
    for (auto it = ips.rbegin(); it != ips.rend(); ++it) {
        auto ip = normalize_ip_str(trim_copy(*it));
        if (ip.empty()) continue;
        if (!is_ip_in_cidrs(ip, trusted_proxies.cidrs)) {
            return ip;  // 第一个非信任 IP 即为客户端真实 IP
        }
    }
    // 全在信任列表内或全为空段，退回直连 IP
    return direct_ip;
    // XFF 完全不可信（全为空或全在信任列表内），退回直连 IP
    return direct_ip;
}
```

### 配置

```ini
[trusted_proxies]
# 只写具体 LB IP，不要用整段 RFC1918
192.168.1.10
192.168.1.11
```

---

## IP 黑名单

### 功能

- 支持单 IP 拦截
- 支持 CIDR 段拦截
- 基于真实 IP（经过 XFF 解析后）

### 实现

```cpp
class IPBlacklist {
    std::unordered_set<std::string> exact_ips_;
    std::vector<std::pair<asio::ip::address, unsigned int>> cidr_rules_;
public:
    bool is_blocked(const std::string& ip) const;
};
```

启动时从 `config.ini` 加载。配置错误跳过该条，不影响其他规则。

### 配置

```ini
[ip_blacklist]
10.0.0.1
192.168.1.100
10.0.0.0/8
```

---

## 路径规范化

**所有黑/白名单匹配前必须经过路径规范化。**

> 输入为 picohttpparser 解析后的 path 部分（不含 host、不含 query string）。
> query string 在规范化过程中被剥离，单独保留供日志脱敏使用。

```cpp
using namespace std::string_view_literals;

// 将 hex 字符转为 0-15，非法返回 -1
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            // %2F (%2f) 不解码，保留字面量
            if ((s[i+1] == '2' || s[i+1] == '2') &&
                (s[i+2] == 'f' || s[i+2] == 'F')) {
                out += '%'; out += s[i+1]; out += s[i+2];
                i += 2;
                continue;
            }
            // 其他百分号编码正常解码
            auto h1 = hex_val(s[i+1]);
            auto h2 = hex_val(s[i+2]);
            if (h1 >= 0 && h2 >= 0) {
                out += static_cast<char>((h1 << 4) | h2);
                i += 2;
                continue;
            }
            // 非法百分号编码（如 %GG），保留原字符
        }
        out += s[i];
    }
    return out;
}

NormalizedPath normalize_path(std::string_view path_only) {
    // 输入是 path 部分（不含 host / query）

    // 3. 路径段解析：去掉 . 和 ..（不能超出 root）
    std::vector<std::string_view> segments;
    size_t start = 0;
    while (start < decoded.size()) {
        auto slash = decoded.find('/', start);
        auto seg = (slash == std::string::npos)
            ? std::string_view(decoded).substr(start)
            : std::string_view(decoded).substr(start, slash - start);
        if (seg == ".." && !segments.empty()) {
            segments.pop_back();  // /foo/.. → /
        } else if (seg != "." && seg != "") {
            segments.push_back(seg);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }

    // 4. 重组路径
    std::string normalized = "/";
    for (size_t i = 0; i < segments.size(); i++) {
        if (i > 0) normalized += "/";
        normalized += segments[i];
    }
    if (decoded.back() == '/' && normalized != "/")
        normalized += "/";

    // 5. 统一小写（可配置）
    for (auto& c : normalized) c = std::tolower(c);

    return {std::move(normalized), std::string(query_str)};
}
```

---

## 免鉴权白名单

### 功能

某些路径或上游服务不需要 JWT 验证。

### 配置

```ini
[auth_whitelist]
# 精确路径免鉴权（基于规范化后 path）
/api/health
/api/build

# 前缀免鉴权
/api/public/

# 上游服务名免鉴权
zebra-public
zebra-open
```

### 路由匹配

```cpp
bool needs_auth(const NormalizedPath& path, const std::string& service_name,
                const AuthWhitelist& whitelist) {
    if (whitelist.paths.contains(path.path)) return false;
    for (auto& prefix : whitelist.path_prefixes) {
        if (path.path.find(prefix) == 0) return false;
    }
    if (whitelist.services.contains(service_name)) return false;
    return true;
}
```

---

## JWT 鉴权

### 功能

- 从 `Authorization: Bearer <token>` 提取 JWT
- 验证签名（仅接受配置指定的算法，**拒绝 `alg: none`**）
- 验证 `exp`（容忍 ±60s 时钟偏移）、`iss`、`nbf`（容忍 ±60s）
- 可选验证角色/权限
- 验证结果写入 `HttpContext`，下游 handler 可直接使用

### 实现

集成 `jwt-cpp` header-only 库。

```cpp
struct JWTClaims {
    std::string user_id;
    std::string username;
    std::vector<std::string> roles;
    int64_t exp = 0;
    int64_t iat = 0;
    std::string raw_token;
};

class JWTAuth {
    std::string secret_;
    std::string issuer_;
    std::string algorithm_;
public:
    std::optional<JWTClaims> verify(const std::string& auth_header) const {
        // 提取 Bearer token
        // jwt::decode(token)
        // 显式指定允许算法，拒绝 none
        // 校验 exp/nbf（±60s skew）
        // 校验 iss
    }
};
```

### 配置

```ini
[security]
jwt_secret = your-secret-key
jwt_issuer = asio_owen
jwt_algorithm = HS256           ; 仅接受此算法，拒绝 none
```

---

## 路由黑名单

### 功能

- 按路径禁止访问（基于规范化后 path）
- 可选基于角色的访问控制

### 配置

```ini
[path_blacklist]
/api/internal
/admin/ = role:admin
```

---

## 配置汇总

```ini
[trusted_proxies]
192.168.1.10
192.168.1.11

[ip_blacklist]
10.0.0.1
192.168.1.100
10.0.0.0/8

[auth_whitelist]
/api/health
/api/build
/api/public/
zebra-public
zebra-open

[security]
jwt_secret = your-secret-key
jwt_issuer = asio_owen
jwt_algorithm = HS256
config_reload_interval_sec = 30

[path_blacklist]
/api/internal
/admin/ = role:admin
```

---

## 对性能的影响

| 功能 | 开销 | 说明 |
|:---|:---:|------|
| IP 黑名单 | 极低 | `unordered_set` 查找 O(1)，CIDR 匹配 O(n) |
| 白名单匹配 | 低 | 精确 O(1)，前缀 O(path_len) |
| 路径规范化 | 低 | 一次扫描，O(path_len) |
| JWT 验证 | 中等 | base64 解码 + 签名计算 |
| 路由黑名单 | 极低 | 前缀匹配 O(path_len) |

---

## 热加载

### 支持范围

| 配置 | 支持 | 方式 |
|:---|:---:|------|
| IP 黑名单 | ✅ | 热加载 |
| 路由黑名单 | ✅ | 热加载 |
| 免鉴权白名单 | ✅ | 热加载 |
| JWT 密钥/算法 | ❌ | 走正式轮转流程 |
| 信任代理列表 | ❌ | 部署架构确定后不变 |

### 触发方式

定时轮询：每 `config_reload_interval_sec` 秒重新加载配置并替换规则。

```ini
[security]
config_reload_interval_sec = 30   ; 定时刷新间隔（秒），0 表示关闭
```

**定时刷新：** `asio::steady_timer` 每 N 秒调用 `Config::load()` 重新加载所有配置，再调用 `SecurityRules::reload()` 原子替换规则。

> 当前未实现 SIGHUP 触发，只有定时轮询。

### 实现方式

```cpp
class SecurityRules {
    mutable std::mutex rules_mu_;  // 保护 trusted_proxies_ 和 jwt_auth_ 写操作
    IpBlacklist ip_blacklist_;
    AuthWhitelist auth_whitelist_;
    PathBlacklist path_blacklist_;
    std::shared_ptr<JWTAuth> jwt_auth_;
public:
    void reload(const Config& cfg) {
        std::lock_guard<std::mutex> lock(rules_mu_);
        load_from_config(cfg);
    }
    CheckResult check(...) const {
        // 1. 复制 proxies_copy 和 jwt_copy（持 rules_mu_ 极短时间）
        // 2. 释放 rules_mu_ 后执行 IP 黑名单 / 白名单 / JWT 验证
        //    子模块各自有内部 mutex（IpBlacklist 无锁，AuthWhitelist 有 mu_）
    }
};
```

读侧有少量 mutex，但持有时间极短。在免鉴权场景下热路径无 JWT 验证，不影响 RPS。

---

## 热重启（第二期）

热重启指不停止进程、不丢失连接的前提下重建全部服务。**本期不做。**

---

## 已确认的设计决策

| 项目 | 决策 |
|------|------|
| JWT 库 | `jwt-cpp` header-only |
| JWT 算法 | 显式指定，拒绝 `alg: none` |
| JWT 时钟偏移 | exp/nbf 容忍 ±60s |
| 黑名单日志级别 | `LOG_WARN`（安全事件） |
| 黑名单日志格式 | 剥掉 query string 后记录 path |
| OPTIONS | 默认免鉴权 |
| token 吊销 | 本期不支持，泄露后只能等过期或换 secret |
| 配置错误 | 跳过该条规则，不影响其他规则，打 WARN 日志 |
| 服务名规则 | `[a-z][a-z0-9-]*`，不符合返回 400 |
| keep-alive | 每请求鉴权，禁止 connection 级别缓存 |
| 限流算法 | 令牌桶 |
| 限流分片数 | 32 片 |
| 限流多维度策略 | 任一失败即 429，Retry-After 取最大值 |
| 全局限流 | sub-counter 分片，不走单 key |
| snapshot 路径 | `/var/lib/asio_owen/rate_limit.bin` |
| 非法百分号编码 | 保留原字符，不做报警 |
| global burst 默认值 | `= global_rps` |

## 实现注意清单

以下为实现阶段需确认的边界条件和默认决策。

| ID | 项 | 默认值 / 决策 |
|:---|------|:---:|
| B6 | INI 裸值格式兼容性：`[ip_blacklist]` 下直接写 IP，非 key=value 格式。现有 `Config` 类不支持，需改用逐行 parser 或改为 `ip = xxx` 兼容格式 | TBD |
| B7 | jwt-cpp `leeway` 参数 | 60s |
| B8 | JWT secret 最小长度，HS256 推荐 ≥ 32 字节，启动时校验并 WARN 提示 | 32B |
| B8 | secret 存储：`config.d/` 下文件明文存（已 gitignored 99-local.ini），生产建议走环境变量或受限权限文件 | 明文（开发）/ 环境变量（生产） |
| B9 | `Authorization` 解析：`Bearer` 前缀大小写不敏感；多余空格用 trim；非 `Bearer` scheme 直接 401；token 为空直接 401 | insensitive |
| B10 | 最大 header 大小（含 JWT 大 token 场景） | 16KB，超限返回 431 |
| B11 | 鉴权失败时（401/403）：是否消费完 body 再关连接，还是直接关闭？鉴权失败时 body 未消费，保持连接可能导致 pipeline 错位。建议直接 `Connection: close` 后断连 | `Connection: close` + 断连 |
| B12 | `Expect: 100-continue` 处理：picohttpparser 不处理该头，网关应忽略或直接回 401 拒绝等待 body | 忽略 Expect，按正常流程消费 body 后鉴权 |
| B13 | 路径黑名单 `= role:admin` 语法中 `=` 两侧空格：现有 `Config` 类 `trim(key)`，`/admin/` 会被 trim 成 `/admin/`（前后无空格），但 `/admin/ = role:admin` 的 key 是 ` /admin/ `（含空格），trim 后得到 `/admin/`，不影响。确认即可 | 现有 parser 兼容 |
| B14 | 根路径 `/`：service 抽取为空，直接返回 404，不进鉴权流程 | 404 |
| B15 | JWT 日志脱敏：失败的 token 不记录完整 payload，只记 fingerprint（SHA256 前 8 字节）| 仅 fingerprint |
| B16 | `%2F` 处理：路径规范化时不解码，保留字面量，不作为路径分隔符 | 保留 |
| B16 | `%2F` 处理：路径规范化时不解码，保留字面量，不作为路径分隔符 | 保留 |
| B20 | `%2F` upstream 契约：gateway 转发时使用 raw path（含 `%2F`），upstream **必须不解码 `%2F`**，否则攻击者可通过 `%2F` 绕过 gateway 的路径黑名单 | upstream NOT decode %2F |
| B21 | 路径大小写匹配：`case_sensitive_paths=false`（默认）时路径统一转小写做 ACL 匹配；设为 `true` 时保留原始大小写，适用于 upstream 大小写敏感的场景 | `case_sensitive_paths = false` |
| B17 | 限流落盘间隔 | 30s |
| B18 | 限流桶 LRU 上限 | 100,000 条 |
| B19 | 限流响应含 `Retry-After` 头 | 是 |
| L3 | path 限流匹配策略 | 精确匹配 `path`，前缀匹配写 `path/` 后缀 |
| L4 | path/service 默认 burst | `burst = rate`（可配置覆盖） |
| L5 | `rate=0` 语义 | 无限制，跳过此规则 |
| L5 | `burst=0` 语义 | 配置错误，跳过此规则并 WARN |
| L7 | `Retry-After` 头转换 | `ceil(retry_after_ms / 1000.0)` 秒 |
| L10 | `Decision` 字段语义 | `allowed` 是唯一判定依据；`retry_after_ms` 仅在 `!allowed` 时有意义 |

## 限流

### 位置

```
客户端请求
  ↓
① 真实 IP 解析
  ↓
② 路径规范化         ← 前置，否则 path限流无意义
  ↓
③ IP 黑名单检查
  ↓
④ 限流检查
  ↓
⑤ 免鉴权白名单
  ↓
⑥ JWT 验证
  ↓
⑦ 路由黑名单
  ↓
body 消费
  ↓
路由分发
```

路径规范化必须在限流之前。攻击者用 `/api/login`、`/api/./login`、`/api/%6cogin` 各打一份，不规范化则 path 限流全废。

### 算法：令牌桶（支持 burst）

```cpp
struct Decision {
    bool allowed;
    int retry_after_ms;  // 仅在 !allowed 时有意义；allowed=true 时此值为 0
};

struct TokenBucket {
    int64_t last_refill_ms;
    double tokens;  // 当前令牌数，double 保证毫秒级 refill 精度
};

Decision allow(TokenBucket& b, double rate, double burst) {
    auto now = now_ms();
    auto elapsed = now - b.last_refill_ms;
    b.last_refill_ms = now;
    // 补充令牌，不超过 burst
    b.tokens = std::min(burst, b.tokens + elapsed * rate / 1000.0);
    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return {true, 0};
    }
    int retry_after = std::ceil((1.0 - b.tokens) / rate * 1000);
    return {false, retry_after};
}
```

> 令牌桶相比固定窗口的优势：burst 放行突发、平滑限流、无窗口边界打穿问题。

### 分片结构

10k QPS 下单 mutex 保护全局 `unordered_map` 锁竞争会严重拖垮 RPS。**必须分片。**

```cpp
class RateLimiter {
    static constexpr size_t kShards = 32;

    struct Shard {
        std::mutex mu;  // 仅分片级锁，粒度小
        std::unordered_map<std::string, TokenBucket> buckets;
        // LRU 链表（只跟踪分片内部，不跨分片）
        std::list<std::string> lru_list;
        std::unordered_map<std::string,
            std::list<std::string>::iterator> lru_index;
    };

    std::vector<Shard> shards_{kShards};
    const size_t max_buckets_per_shard_ = 100000 / kShards;  // ~3125

    // 限流配置引用
    struct Config {
        double ip_rps, ip_burst;
        double global_rps;
        std::unordered_map<std::string, Rule> path_limits;
        std::vector<std::pair<std::string, Rule>> path_prefix_limits;
        std::unordered_map<std::string, Rule> service_limits;
    };
    Config cfg_;

    Shard& shard(const std::string& key) {
        return shards_[std::hash<std::string>{}(key) % kShards];
    }

public:
    Decision check(const std::string& key, double rate, double burst) {
        auto& s = shard(key);
        std::lock_guard lock(s.mu);
        auto& bucket = s.buckets[key];
        lru_touch(s, key);
        evict_if_needed(s);
        return allow(bucket, rate, burst);
    }

private:
    void lru_touch(Shard& s, const std::string& key) {
        auto it = s.lru_index.find(key);
        if (it != s.lru_index.end()) {
            s.lru_list.erase(it->second);
        }
        s.lru_list.push_front(key);
        s.lru_index[key] = s.lru_list.begin();
    }

    void evict_if_needed(Shard& s) {
        while (s.buckets.size() > max_buckets_per_shard_) {
            auto lru_key = s.lru_list.back();
            s.buckets.erase(lru_key);
            s.lru_index.erase(lru_key);
            s.lru_list.pop_back();
        }
    }

    // 多规则组合：任一维度失败即 429，Retry-After 取最大值
    // 注意：全局限流不走 check(key)，改用 sub-counter 分片
    Decision check_all(
        const std::string& ip,
        const std::string& path,
        const std::string& service) {

        auto ip_d = check(ip, cfg_.ip_rps, cfg_.ip_burst);
        auto path_d = check_path(path);
        auto svc_d = check_service(service);
        auto global_d = check_global(cfg_.global_rps, cfg_.global_rps);
        // 注意：global burst 默认 = global_rps，即允许瞬间消耗完整秒的配额

        if (ip_d.allowed && path_d.allowed && svc_d.allowed && global_d.allowed) {
            return {true, 0};
        }
        int max_retry = std::max({ip_d.retry_after_ms,
            path_d.retry_after_ms, svc_d.retry_after_ms,
            global_d.retry_after_ms});
        return {false, max_retry};
    }
};
```

#### 全局限流：单 atomic CAS 令牌桶

全局桶不走 `check(key)`，也不走 sub-counter 分片。sub-counter 的 `now % kShards` 选择方式在同一毫秒内有聚集问题，且 sub-counter 加和判断超限需要遍历 32 片，不划算。

改用单 `atomic` 毫令牌桶，10k QPS 下 CAS 操作 ~100ns，总 CPU 开销约 0.1%，完全可接受。

```cpp
struct GlobalBucket {
    std::atomic<int64_t> tokens_milli;    // 毫令牌，1 请求 = 1000 毫令牌
    std::atomic<int64_t> last_refill_ms;
};

GlobalBucket global_;

Decision check_global(double rate, double burst) {
    int64_t now = now_ms();
    int64_t last = global_.last_refill_ms.load(std::memory_order_relaxed);
    int64_t elapsed = now - last;

    // CAS refill：只有一个线程负责推进时间并补令牌
    if (elapsed > 0 &&
        global_.last_refill_ms.compare_exchange_strong(last, now)) {
        int64_t add_milli = static_cast<int64_t>(elapsed * rate);  // rate=100 req/s
        int64_t old_t = global_.tokens_milli.load(std::memory_order_relaxed);
        int64_t burst_milli = static_cast<int64_t>(burst * 1000);
        do {
            int64_t new_t = std::min(burst_milli, old_t + add_milli);
            if (global_.tokens_milli.compare_exchange_weak(
                    old_t, new_t, std::memory_order_relaxed))
                break;
        } while (true);
    }

    int64_t before = global_.tokens_milli.fetch_sub(1000, std::memory_order_relaxed);
    if (before >= 1000) return {true, 0};
    int64_t retry_ms = static_cast<int64_t>(std::ceil(1000.0 / rate));
    return {false, retry_ms};
}
```

特点：
- 真令牌桶语义，支持 burst
- 与 per-IP 的令牌桶语义一致
- 无锁，CAS 失败重试，10k QPS 下竞争可忽略
- `retry_after` 基于实际 rate 计算，不是硬编码
### 粒度

| 粒度 | 场景 | 配置 |
|:---|------|:---|
| 单 IP | 防单个客户端刷量 | `ip_rps = 100` / `ip_burst = 200` |
| 单 IP + 路径 | 防特定接口被刷 | `[rate_limit_paths]` |
| 全局 | 防总流量过载 | `global_rps = 50000` |
| 上游服务 | 保护某个上游 | `[rate_limit_services]` |

> 路径维度和服务维度基于规范化后的 path。

**`check_path` 实现：**

```cpp
Decision check_path(const std::string& path) {
    // 精确匹配
    auto it = cfg_.path_limits.find(path);
    if (it != cfg_.path_limits.end()) {
        return check("path:" + path, it->rate, it->burst);
    }
    // 前缀匹配：path 以配置的前缀开头
    for (auto& [prefix, rule] : cfg_.path_prefix_limits) {
        if (path.starts_with(prefix)) {
            return check("path:" + prefix, rule.rate, rule.burst);
        }
    }
    return {true, 0};
}
```

**`check_service` 实现：**

```cpp
Decision check_service(const std::string& service) {
    auto it = cfg_.service_limits.find(service);
    if (it == cfg_.service_limits.end()) return {true, 0};
    return check("svc:" + service, it->rate, it->burst);
}
```

> path 匹配策略：精确匹配优先；若配置以 `/` 结尾则前缀匹配。
> path/service 的默认 `burst = rate`，可配置覆盖。

### 内存估算

1 万 QPS 场景：

| 活跃 IP 数 | 每 IP 路径数 | 总记录数 | 内存（32 分片） |
|:---:|:---:|:---:|:---:|
| 1,000 | 5 | 5,000 | **~1.5 MB** |
| 10,000 | 5 | 50,000 | **~15 MB** |
| 100,000 | 3 | 300,000 | ~90 MB（LRU 淘汰至 10 万） |

> 实际条目可能比纯数学估算多 1.5-2x（unordered_map load factor + 节点开销）。实现后跑 massif 验证。

### 持久化

```ini
[rate_limit]
ip_rps = 100
max_buckets = 100000
snapshot_interval_sec = 30
snapshot_path = /var/lib/asio_owen/rate_limit.bin   ; 非 /tmp，权限 0600
```

**落盘：** 逐分片拷贝（锁粒度小），写入临时文件后 rename 保证原子性。文件带 magic + version + checksum。

```cpp
void persist_worker() {
    while (running_) {
        sleep(cfg_.snapshot_interval_sec);
        Snapshot snap;
        for (auto& s : shards_) {
            std::lock_guard lock(s.mu);
            snap.shards.push_back(s.buckets);
        }
        auto tmp = cfg_.snapshot_path + ".tmp";
        write_snapshot(tmp, snap);  // 带 magic + version + checksum
        rename(tmp.c_str(), cfg_.snapshot_path.c_str());
    }
}
```

**加载：** 校验 magic/version/checksum，失败则丢弃并 WARN。加载时检查 `header.written_at_ms`，O(1) 判断是否过期。

```cpp
struct SnapshotHeader {
    char magic[10];         // "RATE_LIMIT"
    uint32_t version;       // 1
    uint32_t checksum;      // 内容校验和
    int64_t written_at_ms;  // 落盘时刻
    int64_t total_buckets;  // 总条目数
};

void load_snapshot() {
    auto data = read_file(cfg_.snapshot_path);
    if (data.size() < sizeof(SnapshotHeader)) return;
    auto& hdr = *reinterpret_cast<const SnapshotHeader*>(data.data());
    if (memcmp(hdr.magic, "RATE_LIMIT", 10) != 0 || hdr.version != 1) {
        LOG_WARN("rate_limit: snapshot corrupted, starting empty");
        return;
    }
    if (hdr.written_at_ms + 120 * 1000 < now_ms()) {  // 2 分钟上限
        LOG_WARN("rate_limit: snapshot expired, starting empty");
        return;
    }
    // 校验 checksum...
    auto snap = parse_snapshot(data, sizeof(SnapshotHeader));
    // 恢复各分片，同步重建 LRU 链表
    for (size_t i = 0; i < kShards; i++) {
        std::lock_guard lock(shards_[i].mu);
        shards_[i].buckets = std::move(snap.shards[i]);
        shards_[i].lru_list.clear();
        shards_[i].lru_index.clear();
        for (auto& [k, _] : shards_[i].buckets) {
            shards_[i].lru_list.push_front(k);
            shards_[i].lru_index[k] = shards_[i].lru_list.begin();
        }
    }
}
```

**收益分析：**

| 场景 | 恢复有意义吗？ |
|:---|:---:|
| 多实例 + LB | ❌ 其他实例还在跑，流量平滑切换 |
| 单实例计划内重启 | ✅ 防止重启后窗口重置被打穿 |
| 崩溃重启 | ⚠️ 有限，最后 30 秒数据可能丢 |

### 已知限制

| 限制 | 说明 |
|------|------|
| 多实例不共享 | 限流是单实例本地计数，不防多实例叠加。多实例场景需要 Redis 共享计数（本期不做） |
| IPv6 按 /64 聚合 | IPv6 单用户可生成 2^64 地址，LRU 会被打满。默认按 /64 聚合（可配置 `/48`） |
| NAT 共享 IP 误伤 | 公司出口 NAT 几千用户共享一个 IP，`ip_rps` 过低会误封。建议 `ip_rps` 设较高阈值（如 1000），用 path/service 维度做精细控制 |
| 哈希碰撞 | 攻击者构造同 hash 字符串塞满 `unordered_map` 单 bucket，查找退化 O(n)。建议 `std::hash` 配合随机 seed |
| 全局限流 CAS 竞争 | 单 atomic CAS 在 10k QPS 下重试率极低（< 0.1% CPU）。50k+ QPS 时如出现瓶颈，可回到 sub-counter + 每 counter 独立 refill 方案 |
| 非法百分号编码 | `percent_decode` 遇到非法 hex（如 `%GG`）保留原字符，不会崩溃，但可能导致路径匹配异常。属于输入合法性范畴，不做额外报警 |
| 分片 LRU 不均衡 | 热门 key 可能扎堆某一片，降低有效容量。每片独立 LRU 接受此不均 |
| 多维度扣令牌顺序 | `check_all` 顺序扣减 4 个桶的令牌，导致首次失败时所有桶都被扣过，Retry-After 略偏长，可接受 |

### 限流响应

```http
HTTP/1.1 429 Too Many Requests
Retry-After: 30
Content-Type: application/json

{"code":429,"msg":"too many requests","retry_after":30}
```

### 429 日志采样

同一 key 30s 内只打一次 `LOG_WARN`，后续 `LOG_DEBUG`，避免爆日志。
