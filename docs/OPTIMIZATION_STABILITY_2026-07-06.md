# 优化与稳定性修复方案

> 日期：2026-07-06
> 范围：安全模块、配置加载、热加载并发安全、`main.cpp` 结构拆分
> 目标：先修复明确漏洞和稳定性风险，再做低风险结构优化。
> 语言标准：C++20。后续新增代码可以直接使用 `std::filesystem`、`std::span`、`std::string_view` 等 C++20 可用能力，但仍需保持 standalone ASIO，不引入 Boost.Asio。

## 总体结论

当前最新提交已经把运行时配置入口从 `config.ini` 调整到 `config.d/`，方向是对的，但仍存在几个需要优先处理的问题：

1. IPv4 CIDR 黑名单匹配逻辑错误，可能导致大范围误封。
2. 热加载和请求处理之间存在 `case_sensitive_paths_` 数据竞争。
3. RS256 JWT 手动校验分支没有完整校验 `exp` / `nbf`，和设计文档不一致。
4. `cfg.load(".")` 使用当前工作目录，和“加载二进制旁边的 `config.d/`”口径不一致。

这些问题都可以用小范围 patch 修复，不需要先做大重构。建议先修漏洞，再拆 `main.cpp`。

## 明确实施范围

本次优化按三个阶段推进：阶段 1 先修对齐和漏洞；阶段 2 拆 `main.cpp`；阶段 3 再拆 HTTP 大头文件。每个阶段都要求保持行为可验证，避免把安全修复和大规模结构调整混在一个 diff 里。

### 阶段 1：先修对齐和漏洞

#### 1. 修 IPv4 CIDR 匹配

`IpBlacklist::parse_cidr()` 对 IPv4 CIDR 转 v4-mapped IPv6 时，prefix 需要 `+96`。

同时给 `real_ip.hpp::ip_in_cidr()` 同步修正，或者如果确认该函数不再使用，则删除未用实现，避免以后踩坑。

本项要求第一批直接抽公共 helper，不允许先只修 `ip_blacklist.hpp`、把 `real_ip.hpp` 留到后面。否则中间态会保留两份 CIDR 语义，后续很容易误用旧实现。

建议新增：

```text
src/security/cidr.hpp
```

提供：

```cpp
struct ParsedCidr {
    asio::ip::address_v6 network;
    int prefix_len;
};

std::optional<ParsedCidr> parse_cidr_rule(std::string_view cidr);
bool match_cidr(const std::string& ip, const ParsedCidr& rule);
```

然后 `ip_blacklist.hpp` 和 `real_ip.hpp` 同时切到这个 helper。

#### 2. 修 SecurityRules 热加载数据竞争

把 `case_sensitive_paths_` 纳入 `rules_mu_` snapshot：

```cpp
bool case_sensitive_copy;
{
    std::lock_guard<std::mutex> lock(rules_mu_);
    proxies_copy = trusted_proxies_;
    jwt_copy = jwt_auth_;
    case_sensitive_copy = case_sensitive_paths_;
}
```

然后用 `case_sensitive_copy` 调 `normalize_path()`。

#### 3. 修 RS256 JWT 语义

明确要求 `exp` 存在并校验。

补齐 `nbf` 校验。

`iat` 不强制。

HS256 可以继续使用 `jwt-cpp verify()`，但 RS256 手动验签分支必须补齐同等 claim 校验。

#### 4. 修配置路径口径

有两个选择：

1. 继续要求“从当前工作目录加载”，文档全部改成 `cd build && ./server`。
2. 更推荐：启动时按 `argv[0]` 定位二进制目录，加载 `${bin_dir}/config.d`。这样 `./build/server` 也读 `build/config.d`，符合现有文档。

本方案选择第二种：按二进制目录加载配置。

当前项目 CMake 已配置 C++20，因此可以直接使用 `std::filesystem`。实现时建议 `Config::load(base_dir)` 统一使用 `std::filesystem::directory_iterator`，不要继续混用 `std::filesystem::path` 和 `opendir/readdir`。

推荐实现方向：

```cpp
bool load(const std::filesystem::path& base_dir) {
    auto dir_path = base_dir / "config.d";
    std::vector<std::filesystem::path> files;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_path, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".ini") {
            files.push_back(entry.path());
        }
    }
    if (ec) {
        LOG_ERROR("Config directory not found: ", dir_path.string());
        return false;
    }

    std::sort(files.begin(), files.end(),
        [](const auto& a, const auto& b) {
            return a.filename().string() < b.filename().string();
        });

    for (const auto& file : files) {
        load_file(file.string());
    }
    return true;
}
```

注意：配置路径修复要优先于 `main.cpp` 拆分。拆分后 `Application::run(argc, argv)` 必须继续拿到 `argv[0]`，用于定位二进制目录。

### 阶段 2：拆 main.cpp

目标是让 `main.cpp` 只剩启动胶水：

```cpp
int main(int argc, char* argv[]) {
    App app;
    return app.run(argc, argv);
}
```

建议新增目录：

```text
src/app/
  app_config.hpp        # 强类型配置结构 + from_config()
  application.hpp/.cpp  # 生命周期：init/start/stop/cleanup
  routes.hpp/.cpp       # 本地 API handler 和路由注册
  reload_service.hpp    # 热加载 timer
  snapshot_service.hpp  # rate limit snapshot timer
```

拆分职责：

- `app_config.hpp`：从 `Config` 读取 `server`、`mysql`、`redis`、`http_pool`、`security`、`rate_limit`，集中默认值和校验，避免 `main.cpp` 到处 `cfg.get_int()`。
- `application.hpp/.cpp`：持有 `io_context`、`MysqlPool`、`RedisPool`、`HttpServer`、`SecurityRules`，负责初始化顺序和 shutdown 顺序。
- `routes.hpp/.cpp`：注册 `/api/health`、`/api/build`、`/api/redis`、`/api/mysql`、`/api/combo`。handler 不再读全局 `g_mysql` / `g_redis`，而是通过 `AppServices&` 访问依赖。
- `reload_service.hpp`：封装当前 `schedule_reload` 递归 lambda，减少捕获引用的生命周期风险。
- `snapshot_service.hpp`：封装 rate limiter snapshot timer。现在 `snap_timer` 是局部 `shared_ptr` 自保持，逻辑能跑，但不够清晰。

### 阶段 3：拆 HTTP 大头文件

这个阶段放后面，不和 `main.cpp` 同一批做。

```text
src/http/
  http_server.hpp/.cpp          # accept + connection loop
  http_framing.hpp              # HeaderParseState, CL/TE/chunked
  proxy_forwarder.hpp           # 构造上游请求、读上游响应
  header_utils.hpp              # hop-by-hop/header compare/token split
```

目标是让 `http_server.hpp` 不再承担所有协议细节。

### 推荐执行顺序

1. 小 patch 修漏洞：CIDR、数据竞争、RS256 claims、配置路径。
2. 补最小单测：CIDR、normalize/query、JWT claim、Config load path。
3. 拆 `src/app/`，保持行为不变。
4. 再拆 `http/` 协议工具文件。

## 修复优先级

| 优先级 | 问题 | 风险 | 建议 |
|:---:|------|------|------|
| P0 | IPv4 CIDR 匹配错误 | IP 黑名单/CIDR 规则可能误封或失效 | 立即修 |
| P0 | `case_sensitive_paths_` 数据竞争 | 多线程热加载下 UB | 立即修 |
| P1 | RS256 JWT claim 校验不完整 | 无 `exp` token 可能通过，`nbf` 不生效 | 尽快修 |
| P1 | 配置加载路径不稳定 | 不同启动目录读到不同配置 | 尽快修 |
| P2 | `main.cpp` 职责过多 | 后续维护和 review 成本高 | 漏洞修完后拆 |

## 1. IPv4 CIDR 匹配修复

### 现状

`src/security/ip_blacklist.hpp` 中，IPv4 地址会被转成 v4-mapped IPv6：

```cpp
asio::ip::address_v6 v6 = addr.is_v6() ? addr.to_v6()
    : asio::ip::make_address_v6(asio::ip::v4_mapped, addr.to_v4());

return ParsedCidr{v6, prefix_len};
```

问题是 IPv4 的 `/8`、`/24`、`/32` 是 IPv4 语义。转成 v4-mapped IPv6 后，真实可比较前缀应是：

```text
IPv6 mapped prefix = 96 + IPv4 prefix
10.0.0.0/8   -> ::ffff:10.0.0.0/104
10.0.0.1/32  -> ::ffff:10.0.0.1/128
```

当前实现没有 `+96`，会按 IPv6 地址开头字节比较，导致 CIDR 规则语义错误。

`src/security/real_ip.hpp` 中的 `ip_in_cidr()` 也存在同类问题。即使当前未被主链路使用，也应在第一批直接替换为同一个 `cidr.hpp` helper，或者删除该函数。不要留下两份 CIDR 实现。

### 修复方案

在 CIDR 解析阶段区分 IPv4 / IPv6，并将逻辑收敛到 `src/security/cidr.hpp`：

```cpp
static std::optional<ParsedCidr> parse_cidr(const std::string& cidr_str) {
    auto slash = cidr_str.find('/');
    if (slash == std::string::npos) return std::nullopt;

    auto ip_part = cidr_str.substr(0, slash);
    int prefix_len = 0;
    try {
        prefix_len = std::stoi(cidr_str.substr(slash + 1));
    } catch (...) {
        return std::nullopt;
    }

    asio::error_code ec;
    auto addr = asio::ip::make_address(ip_part, ec);
    if (ec) return std::nullopt;

    if (addr.is_v4()) {
        if (prefix_len < 0 || prefix_len > 32) return std::nullopt;
        auto v6 = asio::ip::make_address_v6(asio::ip::v4_mapped, addr.to_v4());
        return ParsedCidr{v6, prefix_len + 96};
    }

    if (prefix_len < 0 || prefix_len > 128) return std::nullopt;
    return ParsedCidr{addr.to_v6(), prefix_len};
}
```

`ip_blacklist.hpp` 不再自己定义 `ParsedCidr` 和 `parse_cidr()`，只保存 `std::vector<ParsedCidr>`。

`real_ip.hpp::ip_in_cidr()` 若保留，也只包装 `parse_cidr_rule()` + `match_cidr()`。

### 最小测试

新增 `tests/test_ip_blacklist.cpp`：

```cpp
TEST(IpBlacklist, IPv4Cidr8) {
    IpBlacklist bl;
    bl.reload({"10.0.0.0/8"});
    EXPECT_TRUE(bl.is_blocked("10.1.2.3"));
    EXPECT_FALSE(bl.is_blocked("11.0.0.1"));
}

TEST(IpBlacklist, IPv4Cidr32) {
    IpBlacklist bl;
    bl.reload({"10.0.0.1/32"});
    EXPECT_TRUE(bl.is_blocked("10.0.0.1"));
    EXPECT_FALSE(bl.is_blocked("10.0.0.2"));
}

TEST(IpBlacklist, IPv6MappedIPv4) {
    IpBlacklist bl;
    bl.reload({"10.0.0.1/32"});
    EXPECT_TRUE(bl.is_blocked("::ffff:10.0.0.1"));
}
```

## 2. 热加载数据竞争修复

### 现状

`src/security/security_rules.hpp` 中：

```cpp
return check(socket, method, raw_path, xff_header, auth_header, case_sensitive_paths_);
```

`case_sensitive_paths_` 在请求线程无锁读取，热加载线程会写入：

```cpp
case_sensitive_paths_ = cfg.get("security", "case_sensitive_paths", "false") == "true";
```

这属于 C++ 数据竞争，行为未定义。即使 `bool` 在硬件上看起来是原子读写，标准层面仍然不安全。

### 修复方案

和 `trusted_proxies_`、`jwt_auth_` 一样，在 `rules_mu_` 内做快照：

```cpp
CheckResult check(
    asio::ip::tcp::socket& socket,
    const std::string& method,
    const std::string& raw_path,
    const std::string& xff_header,
    const std::string& auth_header) const
{
    if (method == "OPTIONS") {
        return {0, ""};
    }
    if (raw_path.empty() || raw_path == "/") {
        return {404, "not found"};
    }

    std::vector<std::string> proxies_copy;
    std::shared_ptr<const JWTAuth> jwt_copy;
    bool case_sensitive_copy = false;
    {
        std::lock_guard<std::mutex> lock(rules_mu_);
        proxies_copy = trusted_proxies_;
        jwt_copy = jwt_auth_;
        case_sensitive_copy = case_sensitive_paths_;
    }

    auto client_ip = get_client_ip(socket, xff_header, proxies_copy);
    auto normalized_ip = normalize_ip_str(client_ip);
    auto norm = normalize_path(raw_path, case_sensitive_copy);
    ...
}
```

之后可以删除额外的 `check(..., bool case_sensitive)` 重载，减少状态绕行。

删除前必须全仓确认调用点：

```bash
rg "check\\(.*case_sensitive|case_sensitive_paths_|SecurityRules.*check" src tests
```

如果外部测试或后续代码直接调用 `check(..., bool case_sensitive)`，需要改成通过配置加载 `case_sensitive_paths`，或者保留一个仅测试可见的 helper。当前静态检查显示主要调用链来自 `HttpServer` 的 `security_rules_->check(socket, method, path, xff, auth)`。

### 后续增强

如果后续希望进一步降低锁粒度，可以引入不可变规则快照：

```cpp
struct SecuritySnapshot {
    bool case_sensitive_paths;
    std::vector<std::string> trusted_proxies;
    std::shared_ptr<JWTAuth> jwt_auth;
};
```

热加载时构造新 snapshot，然后一次性替换。当前阶段不必一次到位，先消除 UB。

## 3. RS256 JWT Claim 校验修复

### 现状

HS256 分支使用 `jwt-cpp` verifier：

```cpp
auto verifier = jwt::verify()
    .allow_algorithm(jwt::algorithm::hs256{secret_})
    .with_issuer(issuer_)
    .leeway(60);
verifier.verify(decoded);
```

RS256 分支为了绕过 PEM 解析兼容问题，改成手动验签：

```cpp
if (!verify_rs256(msg, sig, pkey_.get())) {
    throw std::runtime_error("failed to verify signature");
}
auto algo = decoded.get_algorithm();
if (algo != "RS256") ...
auto iss = decoded.get_issuer();
if (iss != issuer_) ...
if (decoded.has_expires_at()) {
    auto exp = decoded.get_expires_at();
    if (now > exp + std::chrono::seconds(60)) ...
}
```

问题：

- `exp` 不存在时不会拒绝。
- `nbf` 没有校验。
- RS256 和 HS256 的 claim 语义不一致。

当前默认配置是 RS256，因此这不是冷门路径。

### 修复方案

有两种可选实现，二选一，不要混着做。

方案 A：HS256 继续完全交给 `jwt-cpp verifier`，RS256 手动分支补齐同等 claim 校验。这样 HS256 不会重复校验。

方案 B：HS256 和 RS256 都调用公共 `verify_common_claims()`。如果选这个方案，HS256 的 `jwt-cpp verifier` 只保留算法/签名校验，不再配置 `with_issuer()` / `leeway()`，避免 issuer / exp / nbf 被重复校验。

推荐采用方案 A，改动最小。

RS256 手动分支需要新增公共 claim 校验函数：

```cpp
void verify_common_claims(const jwt::decoded_jwt<jwt::traits::kazuho_picojson>& decoded) const {
    constexpr auto leeway = std::chrono::seconds(60);
    auto now = std::chrono::system_clock::now();

    if (decoded.get_issuer() != issuer_) {
        throw std::runtime_error("wrong issuer");
    }

    if (!decoded.has_expires_at()) {
        throw std::runtime_error("missing exp");
    }
    if (now > decoded.get_expires_at() + leeway) {
        throw std::runtime_error("token expired");
    }

    if (decoded.has_not_before()) {
        if (now + leeway < decoded.get_not_before()) {
            throw std::runtime_error("token not active yet");
        }
    }
}
```

如果当前 `jwt-cpp` 版本没有 `has_not_before()` / `get_not_before()`，使用 payload claim 兜底：

```cpp
if (decoded.has_payload_claim("nbf")) {
    auto nbf = decoded.get_payload_claim("nbf").as_integer();
    auto nbf_time = std::chrono::system_clock::time_point{
        std::chrono::seconds(nbf)
    };
    if (now + leeway < nbf_time) {
        throw std::runtime_error("token not active yet");
    }
}
```

RS256 分支应变成：

```cpp
if (decoded.get_algorithm() != "RS256") {
    throw std::runtime_error("wrong algorithm");
}
if (!verify_rs256(msg, sig, pkey_.get())) {
    throw std::runtime_error("failed to verify signature");
}
verify_common_claims(decoded);
```

如果决定让 HS256 也强制 `exp` 必填，需要确认 jwt-cpp 默认是否允许无 `exp`。若允许，则应采用方案 B，统一由 `verify_common_claims()` 强制 `exp`。这个语义变更需要在文档中明确。

### 最小测试

新增 `tests/test_jwt_auth.cpp`，覆盖：

| 用例 | 期望 |
|------|------|
| RS256 正常 token，含 `exp`，`iss` 正确 | 通过 |
| RS256 缺少 `exp` | 拒绝 |
| RS256 已过期超过 60s | 拒绝 |
| RS256 `nbf` 在未来超过 60s | 拒绝 |
| RS256 `nbf` 在未来 30s | 通过 |
| `alg=none` | 拒绝 |
| `alg=HS256` 但配置 RS256 | 拒绝 |

## 4. 配置加载路径修复

### 现状

当前 `main.cpp`：

```cpp
Config cfg;
if (!cfg.load(".")) {
    ...
}
```

`Config::load(".")` 会从当前工作目录找 `config.d/`。这导致不同启动方式读到不同配置：

```bash
cd /Users/mac/code/croot/asio_owen
./build/server
# 实际读取 ./config.d

cd /Users/mac/code/croot/asio_owen/build
./server
# 实际读取 ./build/config.d
```

文档口径是“运行 `./build/server`，加载 `build/config.d/`”，当前实现不严格成立。

### 修复方案

让 `Config::load()` 的参数语义明确为“配置基准目录”，然后在 `main.cpp` 中按 `argv[0]` 定位二进制目录。

项目使用 C++20，建议统一使用 `std::filesystem::directory_iterator` 加载配置文件，去掉 `DIR*` / `readdir`。

`src/common/config.hpp`：

```cpp
#include <filesystem>

bool load(const std::filesystem::path& base_dir) {
    auto dir_path = base_dir / "config.d";
    std::vector<std::filesystem::path> files;
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator(dir_path, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".ini") {
            files.push_back(entry.path());
        }
    }
    if (ec) {
        LOG_ERROR("Config directory not found: ", dir_path.string());
        return false;
    }

    std::sort(files.begin(), files.end(),
        [](const auto& a, const auto& b) {
            return a.filename().string() < b.filename().string();
        });

    for (const auto& file : files) {
        if (!load_file(file.string())) {
            LOG_WARN("Failed to load config: ", file.string());
        }
    }
    return true;
}
```

`src/main.cpp`：

```cpp
static std::string executable_dir(const char* argv0) {
    std::error_code ec;
    auto p = std::filesystem::absolute(argv0, ec);
    if (ec) return ".";
    return p.parent_path().string();
}
```

启动加载：

```cpp
auto config_base = executable_dir(argv[0]);
Config cfg;
if (!cfg.load(config_base)) {
    std::cerr << "Load config failed from " << config_base << std::endl;
    return 1;
}
```

热加载也必须使用同一个 `config_base`，不能再写 `load(".")`：

```cpp
*schedule_reload = [
    this_reload = schedule_reload,
    timer = g_reload_timer.get(),
    &http_pool_cfg,
    config_base
]() {
    Config new_cfg;
    if (!new_cfg.load(config_base)) {
        ...
    }
    ...
};
```

### 需要同步调整的文档

修复后文档可以统一为：

```text
server 启动时加载二进制所在目录下的 config.d/*.ini。
例如 ./build/server 会加载 ./build/config.d/。
```

旧文档中残留的 `config.ini`、`kill -HUP` 说明应逐步清理。

## 5. main.cpp 结构优化方案

### 现状问题

`src/main.cpp` 当前承担了过多职责：

- 配置加载和默认值解析。
- logger 初始化。
- MySQL / Redis / HttpServer / SecurityRules 创建。
- 本地路由 handler 实现。
- 本地路由注册。
- 上游配置加载。
- 热加载 timer。
- rate limit snapshot timer。
- signal graceful shutdown。
- 全局对象清理顺序。

这会带来几个问题：

1. review 时不容易区分“业务路由改动”和“生命周期改动”。
2. lambda 捕获引用较多，生命周期需要人工推理。
3. 后续增加模块时只会继续堆到 `main.cpp`。
4. 单测很难直接测初始化和 reload 逻辑。

### 目标结构

新增 `src/app/`：

```text
src/app/
  app_config.hpp        # 强类型配置结构和 from_config()
  application.hpp
  application.cpp       # 生命周期：init/start/stop/cleanup
  routes.hpp
  routes.cpp            # 本地 API handler 和路由注册
  reload_service.hpp
  reload_service.cpp    # config.d 热加载 timer
  snapshot_service.hpp
  snapshot_service.cpp  # rate limit snapshot timer
```

拆分后 `main.cpp` 目标形态：

```cpp
int main(int argc, char* argv[]) {
    try {
        Application app;
        return app.run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
```

### app_config

职责：

- 从 `Config` 读取强类型配置。
- 集中处理默认值。
- 集中处理端口、超时、池大小的边界校验。

示例：

```cpp
struct AppConfig {
    LogLevel log_level = INFO;
    std::string log_file = "server.log";
    int server_port = 8081;

    MysqlPool::Config mysql;
    RedisPool::Config redis;
    HttpPool::Config http_pool;

    int config_reload_interval_sec = 30;
    int snapshot_interval_sec = 30;
};

AppConfig load_app_config(const Config& cfg);
```

收益：

- `main.cpp` 不再散落几十个 `cfg.get_int()`。
- 默认值变更集中可审。
- 后续可以对 `max_size < min_size`、负超时等做统一修正。

### application

职责：

- 持有核心对象。
- 管理初始化顺序。
- 管理停止顺序。

示例：

```cpp
class Application {
public:
    int run(int argc, char* argv[]);
    void stop();

private:
    asio::io_context ioc_;
    std::unique_ptr<MysqlPool> mysql_;
    std::unique_ptr<RedisPool> redis_;
    std::unique_ptr<HttpServer> server_;
    std::unique_ptr<SecurityRules> security_rules_;
    std::unique_ptr<ReloadService> reload_service_;
    std::unique_ptr<SnapshotService> snapshot_service_;
};
```

### routes

职责：

- 实现 `/api/health`、`/api/build`、`/api/redis`、`/api/mysql`、`/api/combo`。
- 通过依赖注入访问 `MysqlPool`、`RedisPool`，不再读全局变量。

示例：

```cpp
struct AppServices {
    MysqlPool& mysql;
    RedisPool& redis;
};

void register_routes(HttpServer& server, AppServices services);
```

收益：

- 本地 API handler 可独立测试。
- 避免 `g_mysql`、`g_redis` 这类全局变量继续扩散。

生命周期要求：

- `AppServices` 可以用引用聚合，但必须保证 `Application` 析构顺序是先停止并销毁 `HttpServer`，再销毁 `MysqlPool` 和 `RedisPool`。
- 当前 `cleanup_runtime_objects()` 已经是先 `server` 后 `mysql/redis`，拆分时保持这个顺序。

### reload_service

职责：

- 封装当前递归 `schedule_reload` lambda。
- 统一持有 `config_base`。
- 处理 reload 失败后的重试间隔。

示例：

```cpp
class ReloadService {
public:
    ReloadService(
        asio::io_context& ioc,
        std::string config_base,
        SecurityRules& security_rules,
        UpstreamManager& upstreams,
        HttpPool::Config http_pool_cfg);

    void start(int interval_sec);
    void stop();
};
```

### snapshot_service

职责：

- 封装 rate limiter snapshot 定时落盘。
- 停止时 cancel timer。
- 避免 `main.cpp` 中局部 shared timer 自保持逻辑。

```cpp
class SnapshotService {
public:
    SnapshotService(asio::io_context& ioc, SecurityRules& security_rules);
    void start(int interval_sec);
    void stop();
};
```

注意：

- `SnapshotService` 持有 `SecurityRules&` 可以接受，但必须保证它的生命周期短于 `SecurityRules`。
- 热加载只会更新或重建 `RateLimiter` 内部状态，不应替换 `SecurityRules` 对象本身。
- 如果后续改成替换整个 `SecurityRules` 或规则快照，`SnapshotService` 需要改为回调或 `weak_ptr`，避免 dangling reference。

## 6. HTTP 目录后续拆分

`src/http/http_server.hpp` 后续也可以拆，但不建议和本次漏洞修复同一批做。

建议结构：

```text
src/http/
  http_server.hpp/.cpp       # accept loop + client connection loop
  http_framing.hpp           # HeaderParseState, CL/TE/chunked 状态机
  http_headers.hpp           # header compare, hop-by-hop, Connection token
  proxy_forwarder.hpp/.cpp   # 构造上游请求、读上游响应
  http_pool.hpp              # 上游连接池，保持现有职责
  upstream_manager.hpp       # 上游路由和 reload
```

拆分顺序：

1. 先抽纯函数：header 工具、framing 工具。
2. 再抽 proxy forwarder。
3. 最后缩小 `HttpServer`，只保留连接生命周期和路由分发。

## 7. 推荐落地顺序

### 第一批：安全和稳定性修复

1. 修 `Config::load(base_dir)`，用 C++20 `std::filesystem::directory_iterator` 加载配置目录，并让 `main.cpp` 用 `argv[0]` 定位二进制目录。
2. 修 `SecurityRules` 中 `case_sensitive_paths_` snapshot，消除数据竞争；删除 `check(..., bool case_sensitive)` 前先确认调用点。
3. 新增 `src/security/cidr.hpp`，修 IPv4 CIDR prefix 转换，并同时替换 `ip_blacklist.hpp` 和 `real_ip.hpp` 的 CIDR 逻辑。
4. 修 RS256 JWT `exp` / `nbf` 校验。HS256 保持 jwt-cpp verifier，或统一 claim 校验但避免重复校验。
5. 更新文档中残留的 `config.ini` / `SIGHUP` 旧口径。

### 第二批：补最小测试

1. `test_ip_blacklist.cpp`
2. `test_path_normalize.cpp`
3. `test_jwt_auth.cpp`
4. `test_config_load.cpp`

当前仓库测试目标不完整是已知问题，但这几类是安全边界，建议优先补。

### 第三批：拆 main.cpp

1. 新增 `src/app/app_config.hpp`，迁移配置解析。
2. 新增 `src/app/routes.hpp/.cpp`，迁移本地 API handler。
3. 新增 `src/app/reload_service.hpp/.cpp`。
4. 新增 `src/app/snapshot_service.hpp/.cpp`。
5. 新增 `src/app/application.hpp/.cpp`，迁移生命周期管理。
6. 缩小 `main.cpp` 为启动入口。

### 第四批：拆 http_server.hpp

等前面稳定后再进行，避免协议层大文件拆分和安全修复混在同一个 diff 里。

## 验证清单

修复后至少执行：

```bash
cmake -B build_codex_verify -S .
cmake --build build_codex_verify
ctest --test-dir build_codex_verify --output-on-failure
```

如果运行服务验证：

```bash
./build_codex_verify/server
curl -s http://127.0.0.1:8081/api/health
```

重点验证：

- `./build/server` 从仓库根目录启动时，读取的是 `build/config.d/`。
- 配置热加载期间并发请求不触发 TSAN 数据竞争。
- `10.0.0.0/8` 只拦截 `10.*`，不误拦截其他 IPv4。
- RS256 token 缺 `exp`、过期、`nbf` 未来过久时返回 401。
- 默认路径大小写策略和 `case_sensitive_paths = true` 的行为不回退。

并发热加载需要额外跑 TSAN：

```bash
rm -rf build_tsan
cmake -B build_tsan -S . \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build_tsan
```

启动 TSAN 版本后，在 30s 热加载窗口内压测请求，并修改 `config.d/30-security.ini` 或 `config.d/33-auth_whitelist.ini` 触发 reload。期望：无 data race 报告。
