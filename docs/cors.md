# CORS（跨域）处理设计文档

> 状态：已实现并通过测试（`test_cors` 22/22、`test_security_chain` 13/13、`test_client_session` 10/10）
> 适用范围：`src/http` 下游 HTTP 网关层
> 涉及文件：
> - 新增 `src/http/cors.hpp`（策略结构 + 解析 + 注入/预检/去重）
> - 新增 `config.d/35-cors.ini`（默认 `enabled=false` 的模板）
> - 新增 `tests/test_cors.cpp` + `tests/CMakeLists.txt` 注册
> - 改 `src/security/security_rules.hpp`（加载策略、下移 OPTIONS 短路、`cors_policy()` 访问器）
> - 改 `src/http/client_session.hpp`（预检短路 + 正常响应注入）
> - `src/http/response_builder.hpp` **未改动**（见 §5.4，去重改由 `strip_cors_headers` 承担）

---

## 1. 设计目标

1. **配置驱动**：只有在配置文件中显式开启 `[cors]` 段时，网关才会输出 `Access-Control-*` 响应头、才会本地应答预检请求。
2. **默认安全（secure by default）**：不配置时，网关**不注入任何 CORS 头**，浏览器同源策略自然生效——这与 nginx、Envoy、Spring 等业界主流网关“未配置即不跨域”的行为一致。
3. **不破坏现有链路**：默认行为下，请求的鉴权、限流、代理转发路径与现状完全相同。

---

## 2. 现状（改动前）

| 位置 | 行为 |
|---|---|
| `security_rules.hpp:132-135` | `OPTIONS` 在安全链**第 0 步**无条件放行，跳过 IP 黑名单、限流、JWT |
| `client_session.hpp:251-391` | `OPTIONS` 命中不了本地 route，会被转发到上游或最终 404 |
| `response_builder.hpp:58-146` | 构造响应时**从不写** `Access-Control-*` 头 |

结论：当前既没有回写任何 CORS 头，浏览器跨域实际不可用；同时 `OPTIONS` 无条件放行构成一条限流/黑名单绕过面。

---

## 3. 两种运行模式

### 3.1 未配置（默认，`enabled = false`）

**行为定义 —— 与业界通用方案一致：**

- 响应中**不添加**任何 `Access-Control-*` 头。
- `OPTIONS` 请求**不做本地预检应答**，按普通请求走既有链路（route → proxy → 404）。
- 浏览器同源策略生效：同源请求正常，跨域请求被浏览器拦截。这是安全的默认态，无需任何显式拒绝逻辑。

> 换言之：不配置 = 当前行为 + `OPTIONS` 绕过面被收敛（见 §6），对同源调用方零影响。

### 3.2 已配置（`enabled = true`）

- 命中白名单的 `Origin`：预检请求由网关本地返回 `204` + 完整 `Access-Control-*` 头，不再转发上游；实际请求的响应注入 `Access-Control-Allow-Origin` 等头。
- 不命中白名单的 `Origin`：不注入 CORS 头，预检不伪造成功，交由正常链路自然处理（最终 403/404），即“对未授权来源表现为不跨域”。

---

## 4. 配置格式

```ini
[cors]
# 缺省为 false；整段不存在即视为关闭
enabled = true

# 逗号分隔白名单；单独一个 "*" 表示允许所有来源
allowed_origins = https://app.example.com, https://admin.example.com

# 预检回写的方法/头，可省略走默认值
allowed_methods = GET, POST, PUT, DELETE, OPTIONS
allowed_headers = Content-Type, Authorization

# 需要暴露给前端 JS 读取的响应头，可空
expose_headers =

# 是否允许携带 Cookie / Authorization 凭证
allow_credentials = true

# 预检结果缓存秒数
max_age = 600
```

| 键 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `enabled` | bool | `false` | **总开关**。false 或整段缺失 → 默认模式 |
| `allowed_origins` | list / `*` | 空 | 精确来源白名单；`*` 置 `allow_all_origins=true` |
| `allowed_methods` | string | `GET, POST, PUT, DELETE, OPTIONS` | 预检 `Allow-Methods` |
| `allowed_headers` | string | `Content-Type, Authorization` | 预检 `Allow-Headers` **兜底值**（见下方说明） |

> **`allowed_headers` 语义说明**：当预检请求带 `Access-Control-Request-Headers` 时，网关**原样回显**浏览器请求的头列表（与 nginx/Envoy 默认一致），`allowed_headers` 仅在预检未声明任何头时作为兜底。它**不是强白名单**——CORS 头约束最终由浏览器侧 enforcement，服务端回显本身不泄露数据也不绕过鉴权。若需服务端强约束，需另行改为求交集回显。
| `expose_headers` | string | 空 | `Expose-Headers`，空则不输出 |
| `allow_credentials` | bool | `false` | 是否允许凭证 |
| `max_age` | int | `600` | `Access-Control-Max-Age` 秒数 |

---

## 5. 实现设计

### 5.1 新增 `src/http/cors.hpp`

```cpp
#pragma once
#include <optional>
#include <string>
#include <unordered_set>
#include "http_context.hpp"
#include "http_protocol.hpp"   // header_iequals

struct CorsPolicy {
    bool enabled = false;                             // 总开关，false = 默认模式
    bool allow_all_origins = false;                   // 配置为 "*"
    std::unordered_set<std::string> allowed_origins;  // 精确白名单
    std::string allowed_methods = "GET, POST, PUT, DELETE, OPTIONS";
    std::string allowed_headers = "Content-Type, Authorization";
    std::string expose_headers;
    bool allow_credentials = false;
    int max_age = 600;

    // 返回应写入 Access-Control-Allow-Origin 的值；nullopt = 不放行 / 未开启 / 非跨域
    std::optional<std::string> resolve_origin(const std::string& origin) const {
        if (!enabled || origin.empty()) return std::nullopt;
        if (allow_all_origins) {
            // 关键：带凭证时不能用 "*"，必须回显具体 Origin
            return allow_credentials ? origin : std::string("*");
        }
        if (allowed_origins.count(origin)) return origin;
        return std::nullopt;
    }
};

// 解析 [cors] 段。整段缺失或 enabled=false → 返回禁用策略（短路，不填白名单）
inline CorsPolicy load_cors_policy(const Config& cfg);  // 见实现，逗号分隔 origins，"*" 置 allow_all_origins

// 去重：清理上游可能自带的 Access-Control-* 头，保证网关是唯一来源
// （Vary 故意不动：上游可能按 Accept-Encoding 等 vary，多个 Vary 会合并）
inline void strip_cors_headers(std::vector<std::pair<std::string, std::string>>& headers);

// 普通响应：按策略把 CORS 头压入 ctx.response_headers（关闭时直接 no-op）
inline void apply_cors_headers(HttpContext& ctx, const CorsPolicy& policy) {
    if (!policy.enabled) return;                 // 默认态零副作用
    strip_cors_headers(ctx.response_headers);     // 先去重，避免重复/泄露未白名单的 origin
    auto allow = policy.resolve_origin(ctx.get_header("Origin"));
    if (!allow) return;
    ctx.response_headers.emplace_back("Access-Control-Allow-Origin", *allow);
    if (*allow != "*")  // 回显具体 origin 时必须声明 Vary，防共享缓存串味
        ctx.response_headers.emplace_back("Vary", "Origin");
    if (policy.allow_credentials)
        ctx.response_headers.emplace_back("Access-Control-Allow-Credentials", "true");
    if (!policy.expose_headers.empty())
        ctx.response_headers.emplace_back("Access-Control-Expose-Headers", policy.expose_headers);
}

// 预检：构造 204，仅在识别为 preflight 且 origin 合法时返回 true
inline bool build_preflight_response(HttpContext& ctx, const CorsPolicy& policy) {
    if (!policy.enabled) return false;
    auto allow = policy.resolve_origin(ctx.get_header("Origin"));
    if (!allow) return false;  // 非法 origin：不伪造 204，交回正常流程
    ctx.status_code = 204;
    ctx.response_status_text = "No Content";
    ctx.response_body.clear();
    ctx.response_headers.clear();
    ctx.response_headers.emplace_back("Access-Control-Allow-Origin", *allow);
    if (*allow != "*")
        ctx.response_headers.emplace_back("Vary", "Origin");
    ctx.response_headers.emplace_back("Access-Control-Allow-Methods", policy.allowed_methods);
    auto req_h = ctx.get_header("Access-Control-Request-Headers");
    ctx.response_headers.emplace_back(
        "Access-Control-Allow-Headers",
        req_h.empty() ? policy.allowed_headers : req_h);
    if (policy.allow_credentials)
        ctx.response_headers.emplace_back("Access-Control-Allow-Credentials", "true");
    ctx.response_headers.emplace_back("Access-Control-Max-Age", std::to_string(policy.max_age));
    return true;
}
```

> `response_builder.hpp:31` 的 `is_safe_header` 已对所有落盘头做 CRLF 校验，配置来源的值同样经此校验，无注入风险。

### 5.2 策略存放：随 SecurityRules 热重载（与设计稿不同）

> **偏差说明**：设计稿原计划把 `CorsPolicy` 作为 `HttpServerState` 的普通成员。实际实现改为**存放在 `SecurityRules` 里**，理由是复用既有的配置热重载链路（`ReloadService` 已经周期性调用 `SecurityRules::reload()`），让 `[cors]` 段无需重启即可生效，且与其它安全配置保持一致的加载/加锁语义。

- `SecurityRules` 新增成员：
  ```cpp
  std::shared_ptr<const CorsPolicy> cors_policy_ = std::make_shared<const CorsPolicy>();
  ```
- `load_from_config()` 第 7 步加载（整段缺失 → 禁用默认）：
  ```cpp
  cors_policy_ = std::make_shared<const CorsPolicy>(load_cors_policy(cfg));
  ```
- 对外暴露快照访问器（`rules_mu_` 下 cheap shared_ptr 拷贝，支持热重载时无撕裂读取）：
  ```cpp
  std::shared_ptr<const CorsPolicy> cors_policy() const {
      std::lock_guard<std::mutex> lock(rules_mu_);
      return cors_policy_;
  }
  ```

默认构造即“未配置”态，`HttpServerState` 与其它现有调用方无需改动。

### 5.3 `client_session.hpp` 两个接入点

先在安全检查之后取一次策略快照（`security_rules` 未设置时 `cors_policy` 为空指针，后续分支自然跳过）：

```cpp
std::shared_ptr<const CorsPolicy> cors_policy;
if (state_->security_rules) {
    cors_policy = state_->security_rules->cors_policy();
}
```

**① 预检本地短路** —— route 之前：

```cpp
// 预检请求本地应答，不转发上游；关闭时整段跳过
bool was_preflight = false;
if (!handled && cors_policy && cors_policy->enabled
        && method_str == "OPTIONS"
        && !ctx.get_header("Access-Control-Request-Method").empty()) {
    if (build_preflight_response(ctx, *cors_policy)) {
        handled = true;
        was_preflight = true;
    }
}
```

以 `Access-Control-Request-Method` 是否存在区分“真预检”与普通 OPTIONS，避免误吞非预检的 OPTIONS。

**② 正常响应注入** —— `build_downstream_response` 调用前：

```cpp
if (cors_policy && cors_policy->enabled && !was_preflight /* 预检已含完整头 */) {
    apply_cors_headers(ctx, *cors_policy);
}
```

> **为何用 `was_preflight` 而非 `ctx.status_code != 204`**：上游或本地业务接口可能对普通跨域请求返回 `204 No Content`（DELETE/PUT 成功无正文等），用状态码判断会把这类合法响应误判为预检而跳过注入，导致浏览器拒绝跨域。`was_preflight` 局部标志只在本地预检短路成功时置 true，精确区分「预检」与「业务 204」。

关闭时 `enabled == false`（或 `cors_policy` 为空），两处均为 no-op，链路与现状完全一致。

### 5.4 防上游重复头：`strip_cors_headers`（与设计稿不同）

> **偏差说明**：设计稿原计划在 `response_builder.hpp` 的 proxy `filtered` 列表里加入六个 `access-control-*`。实际实现**未改动 `response_builder.hpp`**，改为在 `apply_cors_headers` 内部先调用 `strip_cors_headers(ctx.response_headers)` 清理上游 CORS 头再注入。
>
> 两种方式功能等效，但 `strip_cors_headers` 更优：
> - **仅在网关接管 CORS（`enabled=true`）时才去重**，默认态完全不触碰上游头；proxy `filtered` 方案会对所有 proxy 响应无条件过滤。
> - 去重逻辑与注入逻辑同处一函数，读写内聚，且对本地 route 响应同样生效（不限于 proxy 分支）。

```cpp
inline void strip_cors_headers(std::vector<std::pair<std::string, std::string>>& headers) {
    headers.erase(std::remove_if(headers.begin(), headers.end(),
        [](const std::pair<std::string, std::string>& kv) {
            const std::string& n = kv.first;
            return header_iequals(n, "access-control-allow-origin") ||
                header_iequals(n, "access-control-allow-credentials") ||
                header_iequals(n, "access-control-allow-methods") ||
                header_iequals(n, "access-control-allow-headers") ||
                header_iequals(n, "access-control-expose-headers") ||
                header_iequals(n, "access-control-max-age");
        }), headers.end());
}
```

有 `ApplyHeaders.StripsUpstreamCorsBeforeInjecting` 用例守护该行为。

---

## 6. 安全加固：收敛 OPTIONS 绕过面

`security_rules.hpp:132` 现将 `OPTIONS` 在**任何检查之前**放行，攻击者可用 `OPTIONS` 洪泛绕过限流与黑名单。改为把短路**下移到 IP 黑名单、限流之后、JWT 之前**——预检天然不带 `Authorization`，只需跳过鉴权，不应跳过黑名单与限流：

```cpp
// 4. IP 黑名单
if (ip_blacklist_.is_blocked(normalized_ip)) return {403, "ip blocked"};
// 5. 限流
if (rate_limiter_copy) { /* ... */ }
// 5.5 预检放行鉴权（黑名单/限流已生效）
if (method == "OPTIONS") return {0, ""};
// 6. JWT ...
```

此加固与 CORS 是否开启无关，两种模式下都应生效。

> **注意（免鉴权范围）**：该短路按 `method == "OPTIONS"` 判断，因此**所有 OPTIONS 请求**（不仅是带 `Access-Control-Request-Method` 的合法预检）都跳过 JWT，但仍受 IP 黑名单与限流约束。这是有意的简化：预检本就不带 `Authorization`，而非预检 OPTIONS 极少承载敏感操作。若要收紧为「仅合法预检免鉴权」，需把 `is_allowed_preflight` 标志从 `client_session` 传入 `check()`，不能只按 method 判断。

---

## 7. 行为对照表

| 场景 | 未配置（默认） | 已配置且 Origin 命中 | 已配置但 Origin 不命中 |
|---|---|---|---|
| 预检 `OPTIONS` | 走正常链路，无 CORS 头 | 本地 `204` + 完整 CORS 头 | 无 CORS 头，交正常链路（403/404） |
| 实际请求响应 | 无 `Access-Control-*` | 注入 `Allow-Origin` 等 | 不注入 |
| 浏览器结果 | 同源可用，跨域被拦 | 跨域可用 | 跨域被拦 |
| 鉴权/限流/黑名单 | 照常 | 照常（预检仅免鉴权） | 照常 |

---

## 8. 关键陷阱（实现务必落实）

1. **凭证与通配互斥**：`allow_credentials=true` 时严禁 `Allow-Origin: *`，须回显具体 Origin——`resolve_origin` 已处理。
6. **通配 + 凭证 = 加载层降级**：`allowed_origins=*` 与 `allow_credentials=true` 同时配置属高危误配（reflect-any-origin-with-credentials，等于任意站点可带用户 Cookie 跨域打接口）。`load_cors_policy` 在加载阶段检测到该组合会 **`LOG_WARN` 告警并强制 `allow_credentials=false`**，而非静默照做。精确白名单 + 凭证是合法用法，不受影响。由 `LoadCorsPolicy.WildcardWithCredentialsIsDowngraded` 守护。
2. **`Vary: Origin`**：只要回显具体 origin 就必须带，否则共享缓存会把 A 站响应喂给 B 站。
3. **预检不能被 JWT 拦**：浏览器预检不带 `Authorization`，短路必须在 JWT 之前。
4. **非法 Origin 的预检不伪造 204**：`build_preflight_response` 返回 false 时走正常流程，避免把“未授权”伪装成功。
5. **默认态零副作用**：`enabled=false` 时所有 CORS 分支必须是 no-op，确保不配置就是现网行为。

---

## 9. 测试结果（GoogleTest，已实现）

`tests/test_cors.cpp`（已在 `tests/CMakeLists.txt` 注册 `test_cors` target）——**22/22 PASSED**，分四个 suite：

| Suite | 数量 | 覆盖点 |
|---|---|---|
| `CorsPolicy` | 6 | 禁用/空 origin → `nullopt`、精确白名单命中/拒绝、通配无凭证返回 `*`、通配带凭证回显 origin（纯原语行为，危险组合在加载层拦截） |
| `ApplyHeaders` | 6 | 禁用/非法源 no-op、精确源加 `Vary`、通配不加 `Vary`、凭证+expose 头、注入前 `strip_cors_headers` 清理上游头 |
| `Preflight` | 5 | 禁用/非法源返回 false、204+`Vary`+`Max-Age`、回显请求头、无请求头时回退配置默认 |
| `LoadCorsPolicy` | 5 | 缺失段禁用、启用段解析、通配置 `allow_all_origins`、禁用段短路不填白名单、**通配+凭证降级护栏** |

受本次改动影响的既有测试同样全绿（真实运行）：

- `test_security_chain` **13/13** —— `OPTIONS` 短路下移未破坏“预检免鉴权”，黑名单/限流对 OPTIONS 生效。
- `test_client_session` **10/10** —— 端到端链路（404 / 502 / framing / keep-alive）不受注入点影响。

---

## 10. 自我 review 记录：文档与实现的偏差

落地过程中相对本设计稿有两处刻意偏差，均已在上文对应小节标注，此处汇总：

| # | 设计稿 | 实际实现 | 原因 |
|---|---|---|---|
| 1 | `CorsPolicy` 作为 `HttpServerState` 普通成员（§5.2） | 存放于 `SecurityRules`，`shared_ptr<const>` 快照 + `cors_policy()` 访问器 | 复用既有热重载链路，`[cors]` 无需重启即可生效 |
| 2 | 改 `response_builder.hpp` 的 proxy `filtered` 列表去重（§5.4） | `response_builder.hpp` 未改，去重由 `apply_cors_headers` 内的 `strip_cors_headers` 承担 | 仅开启时去重、逻辑内聚、覆盖本地 route 响应 |

另一处需留意的**行为变化**（§6 加固的副作用，非偏差）：`OPTIONS` 短路从安全链第 0 步下移后，OPTIONS 请求现在也会经过路径规范化与根路径检查——即 `OPTIONS /` 现返回 404、`OPTIONS` 到非法编码路径返回 400，而非旧行为的一律放行。对合法路径的真实预检无影响。
