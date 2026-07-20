# CORS 改动 Code Review 报告

> 评审范围：当前未提交工作树（`git status` 中的 7 个文件）
> 评审日期：2026-07-18
> 评审人：Claude
> 关联设计文档：`docs/cors.md`

---

## 0. 总体评价

这是一套**完成度较高、设计扎实**的 CORS 网关层实现，遵循 "secure by default" 原则，配置驱动、默认零副作用，并顺手收敛了一个既有的 OPTIONS 绕过面。代码风格与项目现有约定一致，`shared_ptr<const>` 快照 + 热重载的处理方式与 `SecurityRules` 既有语义贴合，测试覆盖了策略原语、头注入、预检构造、配置加载四个维度。

但存在 **1 个会导致跨域功能在特定状态码下静默失效的真 bug**（P1）、**1 个配置语义偏差**（P2），以及若干测试缺口与文档笔误。建议合入前至少修复 P1。

| 等级 | 数量 | 说明 |
|---|---|---|
| P1（应修） | 1 | `status_code != 204` 启发式误判，上游/本地 204 跨域响应丢失 CORS 头 |
| P2（建议修） | 1 | 预检 `Allow-Headers` 原样回显，绕过 `allowed_headers` 白名单 |
| P3（可选） | 5 | 测试覆盖缺口、文档笔误、origin 大小写、`max_age` 未校验等 |

---

## 1. 改动清单

| 文件 | 性质 | 关键内容 |
|---|---|---|
| `src/http/cors.hpp` | 新增 | `CorsPolicy` 结构 + `resolve_origin` / `load_cors_policy` / `strip_cors_headers` / `apply_cors_headers` / `build_preflight_response` |
| `config.d/35-cors.ini` | 新增 | 默认 `enabled=false` 的配置模板 |
| `docs/cors.md` | 新增 | 设计文档 + 自我 review 记录 |
| `tests/test_cors.cpp` | 新增 | 4 个 suite，共 22 个用例 |
| `tests/CMakeLists.txt` | 修改 | 注册 `test_cors` target |
| `src/security/security_rules.hpp` | 修改 | 持有 `cors_policy_`、暴露 `cors_policy()` 访问器、把 OPTIONS 短路从第 0 步下移到第 5.5 步 |
| `src/http/client_session.hpp` | 修改 | 预检本地短路（route 前）+ 正常响应注入（`build_downstream_response` 前） |

---

## 2. 设计层面评估（亮点）

以下设计经代码核实，确认正确，无需改动：

1. **默认安全**。`CorsPolicy::enabled` 缺省 `false`，`apply_cors_headers` / `build_preflight_response` 开头均 `if (!policy.enabled) return ...;`，`cors_policy_` 默认构造也是禁用态（`security_rules.hpp:331`）。不配置等同现网行为。✅

2. **凭证 + 通配互斥的纵深防御**。`load_cors_policy`（`cors.hpp:74-78`）在加载阶段检测 `allow_all_origins && allow_credentials` 危险组合并强制降级；`resolve_origin`（`cors.hpp:32-34`）作为纯原语再次兜底 `allow_credentials ? origin : "*"`。两层防线，单点失误不会导致 reflect-any-origin-with-credentials。✅

3. **OPTIONS 短路下移是真实的安全加固**。核实 `security_rules.hpp:check()` 的完整顺序：根路径检查(137) → IP 提取(155) → 路径规范化(163) → service 提取(170) → **IP 黑名单(173) → 限流(178) → OPTIONS 放行(188) → JWT(198)**。OPTIONS 不再能绕过黑名单与限流，仍能跳过 JWT（预检不带 Authorization）。与 CORS 开关无关，两种模式都受益。✅

4. **非法 origin 不伪造成功**。`build_preflight_response`（`cors.hpp:125-128`）的两个 false 分支（`!enabled`、`!resolve_origin`）都在写 `ctx` 之前返回，caller 自然 fall-through 到正常链路 → 403/404，符合 "对未授权来源表现为不跨域"。✅

5. **去重策略内聚**。`strip_cors_headers` 在 `apply_cors_headers` 内部、且仅在 `enabled=true` 时调用，既防上游重复头又防泄露未白名单 origin；关闭时完全不触碰上游头。优于设计稿原计划的 "改 `response_builder.hpp` filtered 列表对所有 proxy 响应无条件过滤"。✅

6. **热重载一致性**。`cors_policy_` 为 `shared_ptr<const CorsPolicy>`，reload 整体替换，`cors_policy()` 在 `rules_mu_` 下返回拷贝，`client_session` 持有快照后无锁访问——快照语义无撕裂。✅

7. **配置解析健壮**。`load_cors_policy` 的 origins 解析循环（`cors.hpp:50-61`）边界正确：`start <= view.size()` 容忍尾逗号，`std::move(token)` 避免无谓拷贝，空 token 跳过。✅

---

## 3. 发现的问题

### 🔴 P1-1：`status_code != 204` 启发式误判，跨域 204 响应丢失 CORS 头

**位置**：`src/http/client_session.hpp:426`

```cpp
// Inject CORS headers on the actual response (the 204 preflight
// path already carries its own complete header set).
if (cors_policy && cors_policy->enabled && ctx.status_code != 204) {
    apply_cors_headers(ctx, *cors_policy);
}
```

**问题**：注释的推理前提是 "204 必然来自预检短路"，但该前提**不成立**。

已核实 `ctx.status_code` 在以下非预检路径都会被设为 204：
- **proxy 成功路径**：`client_session.hpp:375` `ctx.status_code = proxy_resp.status_code;` —— 上游若对普通跨域请求返回 `204 No Content`（DELETE/PUT 成功无正文、部分 POST 接口），状态码原样写入。
- **本地 route handler**：handler 可自行 `ctx.status_code = 204`（如某些成功无正文的 API）。

**触发场景**：开启 CORS + 跨域请求命中一个会返回 204 的上游接口 → 该响应被误判为预检 → `apply_cors_headers` 被跳过 → 响应不含 `Access-Control-Allow-Origin` → **浏览器拒绝跨域响应，前端拿不到结果**。

讽刺的是，该判断把 204 排除在外，而 204 恰好是一个合法的业务成功状态码；其它错误码（502/404/500）反而会正常注入 CORS 头。这使得该 bug 尤其隐蔽：错误响应跨域可见，唯独 204 成功响应跨域不可见。

**测试为何没拦住**：`test_cors.cpp` 只单测了 `cors.hpp` 的纯函数，`apply_cors_headers` 内部并不关心 `status_code`（status_code 的判断在 `client_session` 层）。`test_client_session` 的 10 个既有用例不覆盖 CORS 注入。所以这个 bug 处于测试盲区。

**修复建议**（改动最小，不改 `HttpContext` 公开结构）：在 `client_session` 的连接处理作用域内引入一个局部标志，显式标记 "本次响应由预检短路产生"：

```cpp
// 声明（预检短路之前，与 cors_policy 同作用域，约 client_session.hpp:252）
bool was_preflight = false;

// 预检短路处（client_session.hpp:260-266）
if (!handled && cors_policy && cors_policy->enabled
        && method_str == "OPTIONS"
        && !ctx.get_header("Access-Control-Request-Method").empty()) {
    if (build_preflight_response(ctx, *cors_policy)) {
        handled = true;
        was_preflight = true;   // ← 新增
    }
}

// 注入处（client_session.hpp:426）
if (cors_policy && cors_policy->enabled && !was_preflight) {  // ← 不再用 status_code
    apply_cors_headers(ctx, *cors_policy);
}
```

预检成功时 `was_preflight=true`，跳过注入（预检已有完整头）；上游/本地 204 时 `was_preflight=false`，正常注入。两点都在同一个 connection 处理 lambda 作用域内，局部变量可见。

**补测**：在 `test_cors.cpp` 或 `test_client_session` 增加一个用例，验证 "非预检的 204 响应仍应注入 CORS 头"（构造 `ctx.status_code=204` + 合法 origin，确认 `apply_cors_headers` 注入 `Allow-Origin`）。

---

### 🟡 P2-1：预检 `Allow-Headers` 原样回显，绕过 `allowed_headers` 白名单

**位置**：`src/http/cors.hpp:139-142`

```cpp
auto req_h = ctx.get_header("Access-Control-Request-Headers");
ctx.response_headers.emplace_back(
    "Access-Control-Allow-Headers",
    req_h.empty() ? policy.allowed_headers : req_h);
```

**问题**：当浏览器预检带上 `Access-Control-Request-Headers: X-Evil, X-Whatever` 时，网关原样回显，等价于允许任意请求头。`allowed_headers` 配置项仅在 "浏览器没发 Request-Headers" 时起兜底作用，**不构成真正的白名单约束**。

这与配置文档呈现的语义有偏差：用户配置 `allowed_headers = Content-Type, Authorization` 会合理地以为这是强约束，实际并非如此。`cors.hpp:137-138` 的注释也承认 "Echoing the browser's requested headers is more permissive than a fixed list"。

**严重性评估**：这不是服务器端安全漏洞——CORS 的本意就是浏览器侧的同源策略放宽，头白名单最终由浏览器 enforcement，服务器回显本身不直接泄露数据或绕过鉴权。但它降低了配置的可预期性，且与文档/配置模板的措辞不一致。

**修复建议**（二选一）：
- **方案 A（更安全，推荐）**：对 `req_h` 做大小写不敏感解析，与 `policy.allowed_headers` 求交集后回显交集（交集为空则回显配置默认）。
- **方案 B（最小改动）**：保持回显行为，但在 `docs/cors.md` §4 的 `allowed_headers` 行与 `config.d/35-cors.ini` 注释里**显式说明** "仅在预检未声明 Request-Headers 时作为兜底；预检会原样回显浏览器请求的头列表"，避免误导。

---

### 🟢 P3：可选改进与文档笔误

**P3-1 文档笔误（`docs/cors.md:3`）**
第 3 行写 `test_cors 21/21`，但 §9（第 299 行）写 `22/22 PASSED`，§9 表格累加 `6+6+5+5 = 22`，`test_cors.cpp` 实际 `TEST` 数也是 22。开头应改为 `test_cors 22/22`。

**P3-2 测试覆盖缺口**
- `strip_cors_headers` 实现覆盖 6 个头，但 `ApplyHeaders.StripsUpstreamCorsBeforeInjecting` 只验证了 `Allow-Origin` 一个被清理。建议补测对其余 5 个（`Allow-Credentials` / `Allow-Methods` / `Allow-Headers` / `Expose-Headers` / `Max-Age`）的清理，防止后续误删 `strip` 中的某一项而无回归保护。
- `client_session` 两个接入点（预检短路、响应注入）无端到端测试覆盖，P1-1 正是因此漏网。
- Vary 累积场景（上游已有 `Vary: Accept-Encoding`）未测——文档 §5.4 注明 "多个 Vary 会合并" 是有意为之，但缺一个锁定该行为的用例。

**P3-3 origin 精确匹配未归一化（`cors.hpp:36`）**
`allowed_origins.count(origin)` 是大小写敏感精确匹配。浏览器发送的 Origin 通常已归一化（小写 scheme/host），但若管理员在配置里写 `HTTPS://App.Example.com`，将永不命中。建议在 `load_cors_policy` 解析时对每个 origin 做小写归一化（scheme + host 部分），提升配置容错。属于边角健壮性问题，非 bug。

**P3-4 `max_age` 未做范围校验（`cors.hpp:67`）**
`cfg.get_int("cors", "max_age", 600)` 结果直接存入 `int max_age`，负数或极端值会原样写入 `Access-Control-Max-Age`。浏览器对非法值通常会忽略，无功能危害，但配置层加一句 `policy.max_age = std::max(0, policy.max_age);` 更稳妥。

---

## 4. 逐文件审查备注

### `src/http/cors.hpp`
- 头文件均为 `inline` 自由函数 + POD-like 结构，无全局可变状态，符合 header-only 风格，无 ODR 风险。
- `resolve_origin` 返回 `optional<string>`，调用点（`apply_cors_headers`、`build_preflight_response`）均正确处理 `nullopt`。
- `build_preflight_response` 的 `ctx.response_headers.clear()`（`cors.hpp:132`）是防御性清空——预检是本地短路、未经过 proxy/route，此时 headers 本应为空，clear 无副作用，但保留了未来重构的安全边际。
- **见 P1-1（apply 的 status_code 判断不在此文件，但在下游）、P2-1（Allow-Headers 回显）、P3-3 / P3-4。**

### `src/security/security_rules.hpp`
- OPTIONS 短路下移（`security_rules.hpp:185-190`）位置正确，紧跟在限流之后、JWT/auth whitelist 之前。
- `cors_policy()` 访问器（`security_rules.hpp:235-239`）在 `rules_mu_` 下返回 `shared_ptr` 拷贝，与 `rate_limiter_snapshot()` 模式一致。
- `cors_policy_` 默认初始化为 `make_shared<CorsPolicy>()`（`security_rules.hpp:331`），即使从未 `load_from_config` 也返回禁用快照，`client_session` 中 `cors_policy->enabled` 为 false，两处接入点自然 no-op。
- **无问题。**

### `src/http/client_session.hpp`
- 两处接入点位置合理：预检短路在 route 查找前（`client_session.hpp:260-266`），响应注入在 `build_downstream_response` 前（`client_session.hpp:424-428`）。
- 预检短路的四元条件（`enabled && method_str == "OPTIONS" && !empty(Access-Control-Request-Method)`）正确区分了真预检与普通 OPTIONS，避免误吞非预检 OPTIONS。
- `method_str == "OPTIONS"` 依赖 picohttpparser 解析出的方法名大写——这与既有 OPTIONS 短路（旧代码 `method == "OPTIONS"`）的假设一致，HTTP 方法名按 RFC 7230 本就大写，无新增风险。
- **见 P1-1（注入点的 status_code 启发式）。**

### `config.d/35-cors.ini`
- 模板默认 `enabled = false`，注释清晰，与 `docs/cors.md` §4 字段表一致。
- **见 P2-1（`allowed_headers` 注释可补充实际语义）。**

### `tests/test_cors.cpp`
- 用例组织清晰，分 4 个 suite；辅助函数 `make_ctx` / `has_header` / `make_temp_config_dir` / `load_config` 复用合理。
- `make_temp_config_dir` 用 `steady_clock::now().count()` 做唯一性后缀避免并发冲突，且用例末尾 `remove_all` 清理。
- **见 P3-2（覆盖缺口）。**

---

## 5. 建议的修复优先级

| 优先级 | 项 | 改动量 | 阻塞合入？ |
|---|---|---|---|
| 1 | **P1-1**：`client_session.hpp` 用 `was_preflight` 局部标志替换 `status_code != 204` + 补测试 | ~6 行 + 1 测试 | **是** |
| 2 | **P2-1**：`Allow-Headers` 求交集（方案 A）或补充文档/配置注释（方案 B） | 方案 A ~15 行 / 方案 B 仅文档 | 否 |
| 3 | **P3-1**：`docs/cors.md:3` 改 `21/21` → `22/22` | 1 行 | 否 |
| 4 | **P3-2**：补 `strip_cors_headers` 全头清理测试 + client_session 接入点端到端测试 | ~30 行 | 否 |
| 5 | **P3-3 / P3-4**：origin 归一化 + `max_age` 钳到非负 | ~3 行 | 否 |

---

## 6. 结论

整体实现质量高，设计文档与代码的偏差已自觉记录（`docs/cors.md` §10），安全意识到位（凭证/通配互斥、OPTIONS 绕过收敛、非法 origin 不伪造成功）。**唯一阻塞项是 P1-1**：跨域 204 响应丢失 CORS 头是用户可感知的功能缺陷，且处于测试盲区，建议修后再合入。其余为改进项，可在后续迭代处理。
