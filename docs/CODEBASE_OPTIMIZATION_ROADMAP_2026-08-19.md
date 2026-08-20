# asio_owen 项目整体优化路线图

**日期**：2026-08-19
**评估对象**：`93ead3d` 及当前工作区
**评分口径**：对齐 `DESIGN_ASSESSMENT_CLAUDE_2026-07-19.md` 的 6 维度加权模型
**当前评分**：约 **8.1 / 10**
**近期目标**：在不改变外部行为的前提下稳定达到 **8.3～8.4 / 10**

## 1. 结论

项目当前没有需要立即停服处理的已知 P0/P1 问题。下一阶段的主要矛盾已从连接池性能和确定性安全缺陷，转为以下四类工程问题：

1. `src/app/routes.cpp` 聚合过多职责，Admin 两套 Operation 状态机重复维护 executor、Redis completion 和异常边界。
2. 应用关停与跨 executor 生命周期仍依赖 `Application::cleanup()` 的隐式顺序，缺少可等待的分阶段 drain 协议。
3. 配置中心测试数量充足，但最新 Admin、同步和历史状态机尚未进入 ASan/TSan CI。
4. HTTP 协议、Admin 凭证加载、可观测性和构建组织仍有若干低风险但有明确收益的改进点。

本路线图不建议进行一次性“大重构”。正确顺序是：先补行为锚点，再拆编译单元，然后收敛异步基础设施，最后调整生命周期模型。

## 2. 当前基线

### 2.1 已具备的能力

- standalone ASIO 单 `io_context` 多线程模型，请求连接运行在独立 strand。
- MySQL、Redis、Admin 认证和配置文件工作按阻塞性质进入不同 worker pool。
- SecurityRules 和 UpstreamManager 使用 prepare/publish 两阶段热加载。
- 配置中心使用 Redis Lua CAS、不可变历史快照、完整性检查、受限 GC 和显式修复流程。
- Admin 与业务 JWT 使用独立信任域；仓库默认不启用账号；密码轮换可撤销旧 token。
- HTTP framing、超时、幂等重试和连接池计数均有针对性回归测试。
- 当前 `ctest` 共 320 项，完整运行通过。

### 2.2 复杂度集中点

| 文件 | 当前行数 | 主要职责 |
|---|---:|---|
| `src/app/routes.cpp` | 2142 | 公共 API、Admin 鉴权、Admin 状态机、页面和路由注册 |
| `src/app/admin/config_admin.hpp` | 1199 | JSON 解析、凭证、JWT、响应模型和页面资源访问 |
| `src/app/admin/config_history.hpp` | 870 | 历史模型、序列化、Lua 脚本和校验 |
| `src/app/config_sync_service.hpp` | 1301 | 同步状态机、文件 IO、Redis 流程和状态持久化 |
| `src/app/config_history_service.hpp` | 610 | 健康检查、迁移和 GC 调度 |
| `tests/test_admin_config_routes.cpp` | 1485 | 全部 Admin API 行为测试 |

文件行数本身不是缺陷，但当前这些文件同时承担接口声明、实现、线程切换和错误映射，已经影响审查边界、增量编译和 sanitizer 覆盖。

## 3. 优先级总览

| 优先级 | 项目 | 收益 | 风险 |
|---|---|---|---|
| P1 | 建立 `asio_owen_app` 应用层静态库 | 避免生产与测试重复编译 `routes.cpp` | 低 |
| P1 | 按职责拆分 `routes.cpp`，暂不改逻辑 | 降低单文件复杂度，明确模块边界 | 低 |
| P1 | 将配置中心测试加入 ASan/TSan CI | 覆盖最新跨线程和生命周期代码 | 低 |
| P1 | 修复网关裸服务路径 query 解析 | 消除 `/service?x=1` 被识别为未知服务的问题 | 低 |
| P1 | 严格校验上游 HTTP 状态行 | 拒绝非 HTTP 前缀和非法状态码 | 低 |
| P2 | 抽取 Admin 异步执行桥 | 删除两个 Operation 的重复 completion 脚手架 | 中 |
| P2 | 拆分大型 header-only 实现 | 改善编译时间、依赖和 ABI 边界 | 中 |
| P2 | 重构两阶段关停和 in-flight drain | 消除隐式生命周期约定 | 中高 |
| P2 | AdminCredentialStore 快照化 | 减少每请求读盘和 RSA PEM 解析 | 中 |
| P2 | 增加 readiness 和配置中心指标 | 提升滚动发布与故障定位能力 | 中 |
| P3 | HTTP/Admin parser fuzz | 覆盖组合输入和解析器边界 | 中 |
| P3 | Admin CSP 去除 `unsafe-inline` | 缩小 XSS 后 token 泄露风险 | 中 |
| P3 | 当前 revision 受控 A/B 压测 | 为性能分和后续优化提供新基线 | 低 |

## 4. routes.cpp 专项优化

### 4.1 第一步：只拆文件，不改变执行模型

建议目录：

```text
src/app/
├── routes.hpp
├── routes.cpp                         # 仅保留顶层注册，目标 <= 150 行
├── public_routes.hpp
├── public_routes.cpp                  # health/build/mysql/redis/combo
└── admin/
    ├── admin_routes.hpp
    ├── admin_routes.cpp               # Admin 路由注册
    ├── admin_route_support.hpp
    ├── admin_route_support.cpp        # 鉴权、限流、通用响应和异常转换
    ├── admin_login_routes.cpp
    ├── admin_config_routes.cpp        # config GET/SAVE、machines
    ├── admin_history_query_methods.inc # list/detail/diff
    ├── admin_history_common_methods.inc # Redis dispatch/completion
    └── admin_repair_routes.cpp        # repair/rebuild/migrate/orphan
```

第一步直接移动现有实现，保留以下内容不变：

- 路由路径、精确路由与 prefix 路由注册顺序。
- HTTP 状态码、业务 code、JSON 字段和错误文本。
- Redis 命令名称、KEYS/ARGV 顺序及 Lua 脚本。
- Admin worker 数量、在途上限和登录锁定语义。
- `HttpContext` 由当前请求协程独占的前提。

### 4.2 建立应用层静态库

当前 server、`test_combo_routes` 和 `test_admin_config_routes` 分别直接编译 `routes.cpp`。建议新增：

```cmake
add_library(asio_owen_app STATIC
    src/app/routes.cpp
    src/app/public_routes.cpp
    src/app/admin/admin_routes.cpp
    src/app/admin/admin_route_support.cpp
    src/app/admin/admin_login_routes.cpp
    src/app/admin/admin_config_routes.cpp
    src/app/admin/admin_history_operation.cpp
    src/app/admin/admin_history_query_methods.inc
    src/app/admin/admin_history_common_methods.inc
    src/app/admin/admin_repair_routes.cpp
)

target_link_libraries(asio_owen_app PUBLIC asio_owen_core)
add_dependencies(asio_owen_app asio_owen_generated_assets)
```

`server` 和相关测试只链接 `asio_owen_app`，不再把生产 `.cpp` 直接加入测试 executable。这样可以减少重复编译，并避免测试与生产目标使用不同编译源清单。

### 4.3 第二步：抽取 Admin 异步执行桥

`AdminRequestOperation` 和 `AdminHistoryOperation` 当前重复承担：

- Admin worker 投递。
- Redis completion 转换。
- executor 回切。
- `completed_.exchange()` 恰好一次保证。
- `exception_ptr` 到 HTTP 500 的映射。

建议使用组合对象而不是大型继承基类：

```cpp
class AdminAsyncContext {
public:
    asio::awaitable<AdminAuthorizationResult> authorize();
    asio::awaitable<RedisPool::Reply> redis(std::vector<std::string> args);

    template <typename Fn>
    asio::awaitable<std::invoke_result_t<Fn>> run_blocking(Fn fn);
};
```

关键约束：

- `run_blocking()` 的结果必须显式投递回原请求 executor。
- worker 上不直接执行 socket IO。
- `HttpContext` 的写入只能发生在请求仍处于挂起且不会并发访问的阶段。
- Redis completion、worker 投递失败和 handler 异常都只能完成请求一次。
- 不使用捕获请求引用的 detached lambda。

先迁移 `machines` 和 history list 这类单 Redis 命令端点，再迁移 detail/diff，最后迁移 rollback 和 repair。不要从 orphan resolution 开始验证抽象。

### 4.4 第三步：拆除巨型 Kind 状态机

最终目标不是把两个大类移动到两个新文件，而是消除 `Kind` 分派：

```text
handle_admin_history_list()
handle_admin_history_detail()
handle_admin_history_diff()
handle_admin_rollback()
handle_admin_snapshot_repair()
handle_admin_mirror_rebuild()
handle_admin_history_migration()
handle_admin_orphan_resolution()
```

共享能力通过以下小组件提供：

- `AdminAuthorizer`
- `AdminRedisClient`
- `HistoryRecordReader`
- `AdminResponseWriter`
- `AdminLoginThrottle`

不建议建立包含大量 virtual 方法的 `AdminOperationBase`，否则只是把现有状态机换成更难追踪的继承层级。

### 4.5 routes.cpp 验收标准

- `routes.cpp` 仅负责顶层注册，建议不超过 150～200 行。
- 任一 Admin route 实现文件建议不超过 600～700 行。
- Redis 异常转换、Admin worker 投递和 completion-once 各只有一份实现。
- 测试目标不再直接编译 `src/app/routes.cpp`。
- 320 项测试全部通过，Admin 测试 repeat 两轮通过。
- 结构重构提交不包含接口行为、Lua、配置或性能参数变化。

## 5. 生命周期与并发模型

### 5.1 显式 RouteRuntime

`AppServices` 当前包含 MySQL、Redis 和 `thread_pool` 裸指针。它们在现有 `Application::cleanup()` 顺序下有效，但类型本身无法表达生命周期要求。

建议引入 `RouteRuntime`，至少明确：

- 依赖是 owning、shared owning 还是 non-owning。
- 是否接受新请求。
- 当前 active handler 数量。
- shutdown 后 handler 应返回 503、取消还是继续完成。

不要只把裸指针机械替换为 `shared_ptr`。如果没有显式 shutdown 状态，`shared_ptr` 只会延长资源寿命并掩盖关停错误。

### 5.2 分阶段关停

建议关停顺序：

1. `Running -> Draining` 原子状态迁移。
2. 关闭 acceptor，停止接收新连接。
3. 停止 reload、snapshot、config sync、history GC 等周期任务。
4. 在 `io_context` 仍运行时等待请求与服务 in-flight completion，设置总 drain deadline。
5. deadline 到达后取消仍可取消的 IO，并记录未完成任务分类。
6. 停止 `io_context` 并 join IO 线程。
7. join Admin/file worker，再 shutdown MySQL/Redis worker。
8. 按依赖逆序销毁对象。

`stop()` 最终应返回可等待结果或由统一 `ShutdownCoordinator` 汇总，而不是每个服务各自阻塞一个 `condition_variable`。尤其不能在唯一能够派发 completion 的 executor 线程上同步等待。

### 5.3 关停测试

新增不依赖真实数据库的测试：

- Redis completion 已产生但尚未回到 io executor 时触发 shutdown。
- Admin file worker 正在 dry-run 时触发 shutdown。
- acceptor stop、timer cancel 和重复 signal 同时发生。
- drain deadline 到达后进程能够有界退出。
- 任一 handler 抛异常后 exit code 和 systemd restart 语义正确。

## 6. Admin 安全与性能

### 6.1 AdminCredentialStore

当前 Admin 请求会重新读取本地配置，保证改密和删账号立即生效，但会产生重复文件读取、配置解析和 PEM 加载。它已经移出 io 线程，不是事件循环阻塞问题，但仍可优化。

建议建立不可变凭证快照：

- 快照包含账号哈希、token TTL、insecure 开关和预解析 EVP key。
- 使用配置 fingerprint 或文件元数据触发后台重载。
- 新快照完整解析和校验后一次性发布。
- 配置读取失败时保持 fail-closed，不自动回退到 insecure。
- 密码轮换后显式发布新 account version，旧 token 立即失效。

如果只能做到周期轮询而不能保证即时撤销，则暂不启用缓存。安全语义优先于减少低频 Admin IO。

### 6.2 登录限流对象实例化

`AdminLoginThrottle` 和 `AdminAuthWorkLimiter` 当前是进程级 static。建议由 `Application` 或 `RouteRuntime` 持有并注入：

- 测试不再依赖进程内执行顺序和不同 IP 规避状态。
- 阈值、锁定时间和最大在途量可以进入启动配置。
- 锁定过期后应重置失败窗口；登录成功继续清除该 IP 记录。
- 保留独立于通用 `[rate_limit]` 的登录防护，因为仓库默认通用 IP/global RPS 为 0。

### 6.3 密钥和口令内存

- Unix 部署时检查私钥是否为普通文件、owner 是否正确、group/other 是否可读。
- 运行时不得记录 PEM、口令、Authorization 或完整管理配置。
- PBKDF2 完成后尽早清理口令临时缓冲；使用 OpenSSL 提供的清理函数或不会被优化删除的等价实现。
- 保持部署脚本的固定外部密钥目录，不把私钥复制进 build、candidate 或 backup。

### 6.4 CSP 收紧

当前 Admin 页面 CSP 仍允许 `script-src 'unsafe-inline'` 和 `style-src 'unsafe-inline'`。建议二选一：

1. 将 JS/CSS 作为独立内嵌资源端点提供，只允许 `script-src 'self'`。
2. 每次响应生成 nonce，并只允许带 nonce 的内联脚本和样式。

修改 CSP 前必须确认 login/settings 两页全部事件处理器、动态 style 和资源加载路径，避免以页面不可用换取表面上的严格策略。

## 7. HTTP 协议与网关边界

### 7.1 裸服务路径携带 query

本地路由已经先分离 query，但 `UpstreamManager::route()` 仍直接从完整 path 提取 service。`/zebra-config?x=1` 会把 `zebra-config?x=1` 当成服务名。

修复语义应为：

```text
/zebra-config?x=1      -> service=zebra-config, upstream=/?x=1
/zebra-config/a?x=1    -> service=zebra-config, upstream=/a?x=1
```

先添加真实 TCP session 回归测试，再调整 route 解析。

### 7.2 上游状态行严格校验

当前代理层只检查状态行中是否存在两个空格以及中间字段能否解析为数字，非 `HTTP/1.0` 前缀会被按 HTTP/1.1 处理。

建议要求：

- 前缀只能是 `HTTP/1.0` 或 `HTTP/1.1`。
- 状态码只能是三位数，并限制在 100～599。
- reason phrase 允许为空时应按明确规则处理。
- 非法状态行返回 502，并将上游连接标记为不可复用。

补充 `ICY 200 OK`、`XYZ 200 OK`、两位/四位状态码和非法 HTTP version 测试。

### 7.3 Connection 语义闭环

增加矩阵测试，不先假设当前实现错误：

| 下游请求 | 上游响应 | 预期 |
|---|---|---|
| keep-alive | 有 Content-Length | 两侧可复用 |
| keep-alive | `Connection: close` | 上游连接丢弃，下游按完整响应决定是否保留 |
| `Connection: close` | keep-alive | 响应后关闭下游，上游可正常归还 |
| HTTP/1.0 无 keep-alive | 任意完整响应 | 响应后关闭下游 |
| EOF framing | 无明确长度 | 上游连接丢弃 |

矩阵测试通过后再决定是否需要改代码，避免把推测当成缺陷。

### 7.4 热路径分配

低优先级优化：

- `local_route_path` 和 service 提取优先使用 `string_view`。
- proxy request 根据 method、path、header 和 body 大小提前 `reserve()`。
- 避免为了日志构建完整 header/body preview，继续保持日志级别短路。

这类修改必须以受控 benchmark 或 allocation profile 为依据，不为了少一次 `substr` 引入悬垂 view。

## 8. 配置中心实现边界

### 8.1 大型 header-only 实现下沉

建议将以下实现拆为 `.hpp + .cpp`：

- `config_admin.hpp`
- `config_history.hpp`
- `config_sync_service.hpp`
- `config_history_service.hpp`

头文件保留公开类型、配置结构和必要模板；JSON parser、OpenSSL、文件 IO、Lua 字符串和状态机实现下沉到 `.cpp`。收益包括：

- 减少增量编译和测试目标重复实例化。
- 缩小 include 传播范围。
- 明确公开 API 与内部实现。
- 便于 sanitizer 和覆盖率按生产对象文件统计。

不要在同一个提交里同时下沉实现并改 Lua 内容。

### 8.2 文件扫描双周期

当前同步器每 5 秒读取托管文件并计算 hash，已在独立 file worker 上执行，不阻塞 io 线程。可以在确认实际文件规模后考虑：

- 快周期使用文件集合、size 和 mtime 判断明显变化。
- 慢周期继续执行完整内容 hash，防止元数据未变化或人工篡改漏检。
- 任何元数据异常、状态非 ok 或启动首次检查立即执行完整校验。

该优化只有在采样证明文件 worker 或磁盘 IO 有压力时实施；配置完整性优先于减少几十次小文件读取。

### 8.3 readiness

当前 `/api/health` 只返回进程运行状态。建议新增独立 `/api/ready`，至少考虑：

- blocking first pull 是否成功。
- 本地同步状态是否为 ok。
- history 是否 inconsistent。
- required 模式下当前 snapshot/meta/index 是否健康。
- 是否已经进入 shutdown draining。

不要把 Redis 或 MySQL 的短暂波动直接等同于 liveness 失败，避免编排系统形成重启风暴。

## 9. 测试、CI 与故障注入

### 9.1 扩大 sanitizer 覆盖

当前 ASan/TSan CI 未构建以下关键目标：

- `test_admin_config_routes`
- `test_config_sync_service`
- `test_config_history`
- `test_reload_service`
- `test_client_session`
- `test_http_protocol`
- `test_proxy_forwarder`
- `test_proxy_framing`

建议分组运行，避免单个 job 内存过高：

```text
sanitizer-core     -> pool/security/protocol/client session
sanitizer-config   -> admin/config sync/history/reload
```

TSan 下保留 repeat，重点覆盖 reload、shutdown、completion-once 和 worker 回切。

### 9.2 拆分测试文件

将 `test_admin_config_routes.cpp` 拆为：

```text
test_admin_login_routes.cpp
test_admin_config_routes.cpp
test_admin_history_query_routes.cpp
test_admin_history_write_routes.cpp
test_admin_repair_routes.cpp
test_admin_pages.cpp
```

测试 fixture、临时配置和 fake Redis command 放入 `tests/support/admin_route_test_support.hpp/.cpp`，避免复制测试工具。

### 9.3 Redis Lua 集成测试

现有测试大量验证命令序列和返回码映射，仍应增加可选 Docker Redis 集成测试：

- 两个并发 save 的 CAS 胜负。
- rollback 与 save 冲突。
- GC 与 history detail/rollback 同时发生。
- snapshot/meta/index 部分残留。
- wrong-type key、Redis restart 和脚本返回 nil。
- machine TTL 清理与高水位冻结。

这些测试标记为 integration，不进入无服务单测默认路径。

### 9.4 fuzz

优先 fuzz 纯函数边界：

- HTTP status line、header framing、chunk size。
- Admin JSON 请求 parser。
- INI 文件名与 never-sync 校验。
- history snapshot/meta 解析。
- JWT Authorization header 和 claims 类型。

fuzz 目标不得连接 MySQL、Redis 或网络，确保 CI 可重复。

## 10. 构建与静态检查

### 10.1 编译告警基线

当前 CMake 没有统一开启常用 warning。建议分阶段加入：

```text
-Wall -Wextra -Wpedantic
```

先生成并清理告警基线，再考虑 `-Werror`。`-Wconversion` 和 `-Wsign-conversion` 噪声较高，应单独评估，不直接全局启用。

第三方依赖必须使用 SYSTEM include 或独立 target，避免把依赖告警算作项目失败。

### 10.2 clang-tidy

先限定在新增或正在重构的 `src/app/admin/*.cpp`，重点规则：

- use-after-move 和悬垂引用。
- exception escape from destructor。
- 不必要复制和错误的 move。
- narrowing conversion。
- 容易误用的裸 owning pointer。

不要一次对整个历史代码库启用大规模自动重写。

## 11. 可观测性

建议增加结构化 snapshot 或 metrics 端点，至少覆盖：

- Admin auth worker：active、rejected、duration。
- file worker：active、queue delay、scan duration、bytes hashed。
- login throttle：failures、locked IP 数量、work-limit rejection。
- config sync：remote version、local version、status、last success、apply duration。
- history：inconsistent、max observed version、GC candidates/deleted/failures。
- shutdown：active sessions、各服务 in-flight、drain start/completion/duration、forced stop 数量。
- HTTP pool：现有 created/reused/released 指标继续保留。

Admin save、rollback 和 repair 的 audit id 应贯穿 HTTP 日志、Redis audit row 和错误响应，便于一次操作跨层追踪。日志中的 `reason` 必须保持换行清理。

## 12. 性能验证

当前 Phase 4 记录证明 `523b69c` 未出现可观察回归，但不是当前 `93ead3d` 的严格 A/B。

结构重构完成后执行：

1. 同一目标机交替部署重构前后 revision。
2. 固定编译类型、配置、上游数据和日志级别。
3. 每个 revision 至少 5 轮、每轮 60 秒。
4. 比较中位 RPS、p95/p99、CPU、iowait、RSS、FD 和连接池冷却值。
5. 单独压测 Admin 登录、config GET 和 history list，验证 worker queue 上限与 io 热路径隔离。

代码移动阶段的目标是“无回归”，不是宣称吞吐提升。

## 13. 建议提交顺序

1. `test(http): cover bare gateway query and invalid upstream status lines`
2. `fix(http): preserve query on bare service routes and validate status lines`
3. `build(app): add shared application route library`
4. `refactor(routes): split public and admin route modules`
5. `test(admin): split route suites and add sanitizer jobs`
6. `refactor(admin): centralize auth worker and redis completion bridge`
7. `refactor(admin): migrate query endpoints to linear awaitables`
8. `refactor(admin): migrate rollback and repair endpoints`
9. `refactor(config): move header-only implementations into source files`
10. `refactor(shutdown): add explicit draining lifecycle`
11. `feat(ops): add readiness and config-center metrics`
12. `perf: record controlled before-after validation`

每个提交只承担一种性质的变更。纯移动、行为修复、测试扩展和性能调整不得混在同一个提交中。

## 14. 每阶段验收门禁

基础门禁：

```bash
cmake --build build
ctest --test-dir build --output-on-failure
./build/tests/test_admin_config_routes --gtest_repeat=2 --gtest_break_on_failure
git diff --check
```

涉及线程、生命周期或 parser 时追加：

```bash
ctest --test-dir build-address --output-on-failure
ctest --test-dir build-thread --output-on-failure
```

涉及网关热路径时追加：

```bash
PROFILE=1 ROUNDS=5 DURATION=60s ./bench.sh config
```

最终验收：

- 全量测试不减少，失败分支断言不降级为 smoke test。
- ASan/TSan 覆盖 Admin、配置同步、历史和协议核心路径。
- shutdown 在正常、依赖超时和 handler 异常下均有界完成。
- 路由、HTTP 状态、JSON schema、Lua 和配置格式保持兼容。
- 文档、部署指南和代码行为同步更新。

## 15. 预期评分变化

| 完成阶段 | 预期综合分 | 主要提升维度 |
|---|---:|---|
| 当前 | 8.1 | 性能工程较强，结构和流程仍有债务 |
| routes 拆分 + app library + sanitizer 扩面 | 8.2 | 架构、工程纪律 |
| Admin 异步桥 + header 实现下沉 | 8.3 | 架构、内存安全、并发正确性 |
| 显式 drain + readiness + 故障注入 | 8.4 | 异常安全、并发正确性、工程纪律 |
| fuzz + 严格 A/B + 持续流程门禁 | 8.4～8.5 | 工程纪律、性能证据、协议健壮性 |

评分只是结果。真正的验收标准是：新增功能不再扩大隐式生命周期表面，高风险状态机能够进入持续动态检查，结构调整可以由小提交逐步完成并随时回退。

## 16. 2026-08-19 实现进度

当前工作区已完成本路线图首批 P1，以及多项可独立验收的 P2 子集：

- `routes.cpp` 已按 public/admin 拆分，并建立 `asio_owen_app` 静态库。
- 网关裸服务 query、上游状态行严格校验及回归测试已完成。
- ASan/TSan CI 已增加配置中心测试分组。
- Admin worker 投递和异常边界已集中到 `admin_route_support`，两个 Operation 保持原有 Redis completion 与 completion-once 语义。
- 新增 `/api/ready`：区分 liveness，检查配置同步状态、history 一致性和 draining 状态；未就绪返回 503。
- Admin 凭证已迁移到 `AdminCredentialStore` 不可变快照：配置/密钥原子替换时重载，旧 token 通过 auth-version 立即失效；公钥由快照内复用的 `JWTAuth` 预解析，加载失败保持 fail-closed。
- Admin 登录页和设置页使用随机 CSP nonce，移除 `unsafe-inline`，并补充页面响应断言。
- 新增 `ShutdownCoordinator`：连接进入/退出持有 session lease，停止时进入 draining，等待活跃连接有界清空后再停止 io_context。
- 新增显式 `RouteRuntime`：统一持有 MySQL、Redis、Admin/config worker、凭证快照、鉴权 limiter/throttle 和 shutdown 生命周期。
- `config_admin`、`config_history`、`ConfigSyncService` 已改为轻量 public header + out-of-line implementation/PImpl，减少大型 header-only 编译边界。
- Admin 登录 throttle 和 auth work limiter 已通过 `AppServices`/`RouteRuntime` 注入，移除函数内 static 状态。
- `/api/metrics` 现输出 drain start/completion、timeout/forced stop、Admin 鉴权和 config file-worker jobs/busy/scan duration 指标。
- 增加 `-Wall -Wextra -Wpedantic` 基线、HTTP status/header/chunk、INI、history、JWT、Admin JSON 和 path normalize 的 fuzz 目标（默认关闭），以及对应 sanitizer 构建入口。
- 增加可选 `.clang-tidy` 门禁、Docker Redis wrong-type/nil/restart/CAS 故障矩阵和 `bench/controlled_ab.sh` 受控 A/B 采集脚本。
- `admin_history_routes.cpp` 已缩减为路由模块边界，查询方法和公共完成逻辑分别移到 `admin_history_query_methods.inc`、`admin_history_common_methods.inc`，状态机实现保留在 `admin_history_operation.cpp`。

仍需在线上/专用压测机执行的证据任务：用同一配置交替运行基线和当前 revision 的至少 5 轮 60 秒 A/B，运行 Docker Redis 矩阵并归档结果；本地沙箱不允许 bind 的网络测试需在 CI 或专用 runner 完成。
