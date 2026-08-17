# 配置中心设计方案（asio_owen）

**版本**：v1.5.3（2026-08-17，GCC 11.4 admin 路由兼容修订，见文末修订记录）
**状态**：已实施；本文记录当前代码语义与验收边界
**前置阅读**：`docs/CONFIG_REFACTOR_PLAN.md`（section 与生效方式清单）、`docs/GATEWAY_DESIGN.md`、`DB_POOL_DESIGN.md`

---

## 1. 背景与目标

### 1.1 现状痛点

- 配置分散在各机器的 `config.d/*.ini`，变更需逐台登录手动编辑
- 多机部署时同一配置无法同时生效，容易出现机器间配置漂移
- 无变更审计、无统一视图，排查"哪台机器改了什么"只能翻 shell 历史

### 1.2 目标

1. **Redis 作为配置单一事实源**：所有机器的配置一致
2. **各机自动同步**：从 Redis 拉取更新本地文件（运行期轮询镜像；启动期语义见 §5.3/§6.5 的如实界定）
3. **读取链路零改动**：现有 `Config::load` → `ReloadService` fingerprint → prepare/publish 热加载机制原样复用，配置消费方（SecurityRules/UpstreamManager 等）无感知
4. **Web 管理页**：内嵌单页，多 tab 查看 + 修改配置，写 Redis
5. **本地文件保留兜底价值**：Redis 故障时机器可用上次同步的镜像继续运行/启动

### 1.3 非目标（明确不做，后置项见 §8 Phase 4）

- 不做配置即时推送（pub/sub）——现 RedisPool 架构无法承载订阅（订阅会永久占用共享连接与 worker 线程，且 `cmd_timeout_ms=500ms` 会中断阻塞读）；轮询间隔默认 5s 已满足运维时效
- 不做多环境/命名空间隔离（单套集群单份配置）
- 不做配置项加密存储
- 不做配置分发灰度（按机器分组分批生效）

---

## 2. 已确认决策

| # | 决策点 | 结论 | 理由 |
|---|---|---|---|
| D1 | 同步机制 | 轮询更新本地；**维持本地文件读取逻辑** | 复用全部现有热加载机制；Redis 故障有本地兜底 |
| D2 | 同步范围 | **除 `[redis]` 连接信息外全部**（含 `[server]`/`[mysql]`） | 集中管理一切；启动期配置落盘后重启生效（页面明确标注） |
| D3 | 页面形态 | 内嵌单页 HTML，多 tab，表单 + 源码双模式 | 无前端工程、无额外部署，符合项目零外部依赖风格 |
| D4 | 覆盖规则 | Redis 覆盖本地；`99-local.ini` 保留为单机逃生舱 | Redis 是唯一事实源；逃生舱用于单机临时 override（加载序最后天然覆盖） |
| D5 | Redis 写入原子性 | 种子与保存均走 Lua 脚本，且脚本**前置校验 + staging key + RENAME**（v1.2 强化） | 脚本"原子执行"≠"出错回滚"：后置命令报错时先前的写不回滚，必须把报错面消灭在写核心状态之前 |
| D6 | 部分失败语义 | 任一托管文件校验失败 → **不推进已同步版本**，同版本重试 | "跳过坏文件但报同步成功"会让坏文件永远不再被尝试 |
| D7 | admin 保护唯一执行点（v1.2 新增） | 授权只由 **admin handler 代码级**执行；托管配置**禁止**声明 `/admin`、`/api/admin` 相关的 auth_whitelist/path_blacklist 规则 | 配置层 guard 与 insecure 逃生口在安全链路里互斥（403 先于 handler）；且托管 path_blacklist 空值规则可在 handler 前把 admin API 全员封死（自 DoS） |
| D8 | admin 独立信任域（v1.5 新增） | admin 验证使用**独立 RS256 密钥对**（`jwt_keys/admin-*.pem`）与独立 issuer `asio-owen-admin`，由 `/api/admin/login`（用户名+PBKDF2 口令）签发短时 token；**业务 JWT 体系的 token 一律 401** | 现有 JWT 是业务 token（pixiu 体系签发，网关只持公钥验证）；v1.2-v1.4 的实现信任业务公钥验证的 principal——业务系统角色词汇表只要撞上 `role:admin`/`roles:["admin"]` 即可改写网关全部配置（跨信任域提权）。网关管理凭证必须与业务凭证分家 |

---

## 3. 总体架构与数据流

```
                ┌──────────────────────────────────────────┐
                │  浏览器 /admin（内嵌单页，多 tab）          │
                │  表单模式 / 源码模式，乐观锁保存            │
                └───────────────┬──────────────────────────┘
                                │ POST /api/admin/config
                                ▼
                ┌──────────────────────────────────────────┐
                │  Admin API（代码级 role:admin，唯一执行点） │
                │  JSON 解析 → 白名单/保留路径校验            │
                │  → 整目录 dry-run                          │
                │  → Lua 原子提交（前置校验+staging+RENAME    │
                │    + CAS + INCR + audit pcall）            │
                └───────────────┬──────────────────────────┘
                                │ 写（单脚本原子）
                                ▼
        ┌───────────────────────────────────────────────────────┐
        │ Redis                                                  │
        │  HASH asio_owen:config:files    (文件名 → ini 全文)     │
        │  STRING asio_owen:config:version（整数，脚本内 INCR）    │
        │  HASH asio_owen:config:machines （机器心跳/状态）        │
        │  LIST  asio_owen:config:audit   （修改审计，Phase 2）    │
        │  HASH asio_owen:config:files:staging（脚本内暂存，用毕   │
        │       RENAME 走，正常态不驻留）                          │
        └───────────────────────────────────────────────────────┘
                                │ 每 sync_interval_sec 轮询
             ┌──────────────────┼──────────────────┐
             ▼                  ▼                  ▼
      ┌────────────┐     ┌────────────┐     ┌────────────┐
      │ 机器 A      │     │ 机器 B      │     │ 机器 C      │
      │ ConfigSync │     │ ConfigSync │     │ ConfigSync │
      │ Service    │     │ Service    │     │ Service    │
      └─────┬──────┘     └─────┬──────┘     └─────┬──────┘
            │ tmp+rename 原子写  │                  │
            ▼                  ▼                  ▼
      各自本地 config.d/*.ini（读取链路不变）
            │
            ▼ ReloadService fingerprint（文件名+size+mtime）感知
            │
      ┌─────┴──────────────────────────┐
      ▼                                ▼
 热加载面：下个 reload tick 生效      启动期面：下次重启生效
 (upstream/http_pool/gateway/        ([server]/[mysql]/
  security/cors/限流/黑白名单)         [redis] 永不同步)
```

**核心不变式**：Redis → 本地文件是唯一写方向（D4）；本地文件 → 进程配置的路径与今天完全相同；Redis 侧任何 version/files 变更都是单脚本原子单元且报错面在写核心状态之前被消灭（D5）。

---

## 4. Redis 数据模型

| Key | 类型 | Field/Value | 说明 |
|---|---|---|---|
| `asio_owen:config:files` | HASH | field=文件名（如 `20-upstream.ini`），value=ini 文本 | 托管文件全集；保存脚本内经 staging key `RENAME` 整体替换，删除文件=新集合无该 field |
| `asio_owen:config:files:staging` | HASH | 脚本内暂存 | 脚本先在此构建完整文件集，成功后 `RENAME` 到 files；正常态不驻留（每次脚本开头 DEL） |
| `asio_owen:config:version` | STRING | 十进制整数 | 保存脚本内 `INCR`；单调递增，各机以此判断是否需要拉取 |
| `asio_owen:config:machines` | HASH | field=**稳定 machine_name**（配置或 hostname，不含 pid），value=`<版本>\|<unix秒>\|<pid>\|<status:ok\|partial>` | 心跳。field 稳定 → 重启复用同一 field 不累积残留；陈旧由页面按 ts 判定；partial 携带失败文件与原因 |
| `asio_owen:config:audit` | LIST | JSON 行：`{ts, user, base_version, new_version, files:[...]}` | 保存脚本内 `pcall` 尽力而为写入（LPUSH+LTRIM 200 条）；audit 失败不阻断核心提交（Phase 2） |

**与现有 Redis 的关系**：复用现有 RedisPool（`[redis]` 配置的实例与 db）；`GET`/`HGETALL` 均在只读重试白名单内（redis_pool.hpp:459-473），连接级故障自动换连接重试，天然适合轮询。`EVAL` 属写命令不在重试白名单——对 CAS 语义正好正确（失败由调用方决策）。

**实现注意**：
- `parse_redis_reply` 会把数组扁平化（redis_reply.cpp:15-30），Lua 脚本**只返回标量整数**（-3..-1 表各类失败、0 表种子已被占、>0 表新版本/成功）；失败时需要的附加信息（如当前 version）由 API 层另行 GET。EVAL 整数回包的解析需在 Phase 1 实现时验证
- Lua 中 `redis.call('TYPE', key)` 返回带 `ok` 字段的 table（`.ok == 'hash'`），实现时注意写法
- `unpack` 为 Lua 5.1 全局名（Redis 内嵌版本），非 `table.unpack`

---

## 5. ConfigSyncService 规格（Phase 1 核心）

### 5.1 形态与生命周期契约

- 新文件 `src/app/config_sync_service.hpp`，仿 `ReloadService` 的 steady_timer 服务模式（ioc 上的 `asio::steady_timer` + 定时回调）
- Redis 访问仍走既有 `RedisPool::cmd_argv`（awaitable，worker 模式下切到 Redis 专用线程池，不阻塞 io_context——combo 路由同模式）；ConfigSyncService 自身使用普通回调状态机，通过 `co_spawn` 直接适配该既有 awaitable，不新增 helper coroutine
- 命名避开 `SnapshotService`（已被限流器快照占用，snapshot_service.hpp）

**GCC 11.4 兼容约束**：Ubuntu 22.04 / GCC 11.4 在复杂 ConfigSync helper coroutine 上会触发 `build_special_member_call, cp/call.c:10200` ICE（与 `docs/VERIFY_2026-07-08.md` 的历史问题同类）。不得把同步流程重新合并为承载 `State`、map、optional 等复杂对象的 `asio::awaitable<bool/void>`；异步边界保留为 `co_spawn(RedisPool::cmd_argv(...), completion)`，版本读取、双读校验、落盘、seed 与 heartbeat 由普通回调串行推进。

**生命周期与并发契约（不可省略）**——RedisPool 契约要求 "stop HTTP/coroutine scheduling before destroying RedisPool"（redis_pool.hpp:31），而底层 Redis 命令仍由 `cmd_argv` awaitable 执行，存在 in-flight 异步链访问已析构对象/已 shutdown 池的风险：

1. **tick 严格串行**：下一轮调度仅在当前 tick 完全结束后发出（ReloadService 同模式），无重入、无并发 tick
2. **shared_ptr 自持有**：服务由 `shared_ptr` 管理，每段 tick 回调捕获 `shared_from_this()`；`Application::cleanup()` reset 成员后，in-flight 异步链仍持有有效状态直至自然退出
3. **stop() 有界排空**：`running_=false` + `timer_.cancel()` + 等待 `in_flight_==0`（atomic+condition_variable，上限 `cmd_timeout_ms + 500ms` 余量，超时打 ERROR 放行——由 2 保证此时也只是延迟释放，不 UAF）
4. **cleanup 顺序**：ConfigSyncService 的 stop+drain 必须先于 `redis_->shutdown()`（application.cpp:202），加入现有 timer 批次（:190-192 停 / :196-198 reset）
5. Redis awaitable 异常在 `co_spawn` 完成适配层转换为失败 Reply；tick 启动异常也被捕获，不逃逸到 timer 完成回调（沿用 reload_service.hpp:127-134 的 noexcept 日志模式）

### 5.2 配置（本地文件 `config.d/12-config-sync.ini`，永不同步）

```ini
[config_sync]
enabled = false               ; 渐进 rollout，确认无误后手动打开
sync_interval_sec = 5         ; 轮询间隔
machine_name =                ; 空 = hostname（稳定，不含 pid）
first_pull = async            ; async(默认)=启动后异步首同步 | blocking=启动前阻塞首拉取(§5.3)
first_pull_timeout_ms = 3000  ; 仅 blocking 模式：池构建+首命令+同步的整体 deadline

# admin 面独立信任域（v1.5，D8）：账号+密钥+签发参数，全部本地、永不同步。
# 账号行格式：用户名 = pbkdf2_sha256$<迭代>$<salt_b64url>$<hash_b64url>
#   （用根目录 hash_admin_password.py 生成；迭代内嵌可逐账号调整，默认 100000）
# 无账号或无私钥 → 登录与全部 admin 端点 503 fail-closed（§7.1）
[admin]
ops = pbkdf2_sha256$100000$<salt>$<hash>
jwt_private_key = jwt_keys/admin-private-key.pem   ; 仅签发用，chmod 600；相对 config_base 解析
jwt_public_key  = jwt_keys/admin-public-key.pem    ; 验证用（gen_admin_keys.sh 生成）；相对 config_base 解析
token_ttl_min = 120
insecure_no_auth = false      ; 实验室逃生口（v1.5 起与业务 jwt_disabled 彻底脱钩，
                              ; admin 有自己的信任域；替代并移除 [config_sync].allow_insecure_admin）

# /admin HTML 页与 /api/admin/ 前缀放行（本文件永不同步，不违反 §6.3 保留路径规则）：
# 业务 JWT 对 admin 面整体退出——/api/admin/* 的授权由 admin handler 用 admin 密钥
# 独立验证（D8）；限流仍在白名单之前执行（A24），登录端点天然有 IP/全局限流兜底。
[auth_whitelist]
path = /admin
path = /api/admin/
```

### 5.3 启动流程

**默认（`first_pull = async`）——Phase 1 的实际语义是"运行期镜像同步"**：

```
Application::initialize 内、ReloadService.start() 之前启动；
首同步异步执行，不阻塞启动。启动期配置（[server]/[mysql]/[redis]）
一律取本地现状（新机器 = 预置文件或 AppConfig 默认值）；
首同步落盘的启动期配置在下次重启才生效（新机器部署程序见 §6.5）。
```

- 首同步在 `ReloadService.start()` 种子 fingerprint 之前或之后均正确：之前则种子即最新；之后则 fingerprint diff 触发一次正常 reload
- **漂移告警**：首同步及每次版本变化应用后，用落盘配置重新 `app_config_from` 并与运行中 `AppConfig` 比较启动期字段（server_port、mysql、redis），不一致 → `LOG_WARN "startup config drift detected, restart required"`（每版本一次，不刷屏）

**可选（`first_pull = blocking`）——启动前阻塞首拉取（v1.2 修订超时语义）**：

```
main 流程在 Config::load 之后、app_config_from 之前插入：
  1. 从本地配置解析 [redis] + [config_sync]（二者必在本地）
  2. 构建临时 RedisPool，**强制 Mode::Direct**：
     单连接、懒建连、无线程池、无 worker 模式的 min_size=4 预建连
     （worker 模式光建池就可能耗尽预算：min_size=4 × connect_timeout 1000ms，
       acquire_timeout 3000ms，见 config.d/11-redis.ini / A21）
     connect_timeout_ms = min(1000, first_pull_timeout_ms/3)
     cmd_timeout_ms     = min(500,  剩余预算)
  3. 整体 deadline = first_pull_timeout_ms，设三个检查点：
     建临时池前 / seed+sync_now 前 / 每条 Redis 命令前；
     任一检查点超预算 → 中止 + WARN + 回落本地文件继续启动
     （不因配置中心故障拒绝服务）
  4. 成功 → 临时池 shutdown → 重新 Config::load（文件已更新）→ 正常启动
```

### 5.4 同步状态机（每次 tick）

```
tick():
  1. v1 = GET asio_owen:config:version
     ├─ Redis 错误 → LOG_WARN（限频）+ 下周期重试；本地照常服务
     ├─ v1 不存在 → seed()（§5.5，Lua 原子，含播种资格判定）
     ├─ v1 非数字（key 存在但内容损坏，如 "abc"）→ LOG_ERROR
     │   "config version key corrupted, manual fix required"，
     │   本周期不播种不同步（注意：此时 TYPE=string，种子脚本会判"已存在"，
     │   必须由本判定拦住），等人工修复
     ├─ v1 < 本地已同步版本 → 忽略 + 告警（防回滚倒灌）
     ├─ v1 == 本地已同步版本 → 仅刷新心跳(status=ok) → 结束
     └─ v1 > 本地已同步版本 → 继续
  2. files = HGETALL asio_owen:config:files
  3. v2 = GET version；v1 ≠ v2 → 放弃本次（读到写方原子替换的跨界快照），
     不写任何文件/状态，下周期重试
     （写方是单脚本原子替换，故任一 HGETALL 结果内部自洽；
       双读只为给文件集钉上正确的版本标签）
  4. 空远端集保护（v1.4）：若 files 为空，且本地文件系统或状态文件中仍有
     托管文件 → 视为异常，不写不删不推进；状态/心跳 partial，失败项
     remote_files。空托管集只允许出现在"仅三件套新机器"的种子结果。
  5. 逐文件校验（§6）+ 差异落盘：
     文件名白名单不过 / 含 [redis] 节 / 违反保留路径规则(§6.3)
       → 记入 failed，保留本地旧文件
     其余与本地有差异的文件 → tmp+rename 原子写（rate_limiter.hpp:284-288 惯用法）
  6. failed 非空 → **不推进 synced_version、不执行删除、不更新清单**；
     状态文件 status=partial；心跳 status=partial（含失败文件名与原因）；
     下周期对同一 version 整体重试（diff 写幂等：已成功文件无差异不再写，
       坏文件修复后自动收敛）
  7. failed 空 → 推进 synced_version=v1；状态文件 status=ok；
     删除检测（上次全量清单有、本次无 → unlink）；
     更新状态文件 <config_base>/.config-sync-state：
       synced_version、status、托管文件清单、**last_ok 清单（文件名→内容 hash）**
       （非 .ini 后缀，ReloadService fingerprint 只扫 *.ini，reload_service.hpp:158）
  8. 心跳：HSET machines <name> "<v1>|<now>|<pid>|<ok|partial>"
```

**部分失败期间机器上的状态**：同版本的部分文件已落盘（热加载面已被 ReloadService 应用），坏文件保持本地旧值——短暂混合代，心跳 `partial` 可见，修复后收敛。混合代同时意味着该机器**丧失播种资格**（§5.5）。这是有意取舍：整版本回滚式"全成功才落盘"需要两阶段提交式暂存目录，复杂度不值。

### 5.5 首次种子导入（seed：资格判定 + 原子脚本）

**播种资格（客户端判定，防把混合代镜像种成事实源）**——仅以下两种机器可执行种子脚本：

- (a) 状态文件存在且 `status=ok`，且当前全部托管文件逐个 hash 匹配 `last_ok` 清单（本地确为某个完整好版本）
- (b) 状态文件不存在（全新机器，本地文件是运维预置的原始配置，从未发生过 partial）

其余（partial 中 / 状态文件与磁盘不符）→ 拒绝播种 + LOG_ERROR，等待集群中有资格的机器；全部无资格时需人工介入（拷贝文件或 Phase 2 的 API 导入）。注意：状态文件不应被随手删除——它是播种资格的凭据。

```
种子脚本 EVAL（KEYS: version, files, staging；ARGV: 文件名,内容 对）：

  if redis.call('TYPE', KEYS[1]).ok ~= 'none' then return 0 end     -- 已有 version
  if redis.call('TYPE', KEYS[2]).ok ~= 'hash'
     and redis.call('TYPE', KEYS[2]).ok ~= 'none' then return -3 end
  if (#ARGV) % 2 ~= 0 then return -2 end
  if #ARGV == 0 then                                                 -- 空托管集是合法状态：
    redis.call('DEL', KEYS[2])                                       -- 仅 never-sync 三件套的新机器
    redis.call('SET', KEYS[1], 1)
    return 1
  end
  redis.call('DEL', KEYS[3])
  redis.call('HSET', KEYS[3], unpack(ARGV))                          -- 先建完整集
  redis.call('RENAME', KEYS[3], KEYS[2])                             -- 原子替换
  redis.call('SET', KEYS[1], 1)
  return 1

返回 1 = 本机完成种子；0 = 他机已种子（脚本内原子判定，无竞态窗口）；
-2/-3 = ARGV/类型异常。
空集分支（v1.3）：#ARGV==0 时 staging 从未被创建，直接 RENAME 会因
source key 不存在报错中断——显式走 DEL files + SET version 分支。
读者侧 HGETALL 对不存在的 files hash 返回空集，语义自洽。
进程在脚本中途死亡：脚本半途不生效；若恰在
RENAME 后、SET version 前死亡，files 已建好但 version 仍不存在 →
下台机器重跑种子脚本整体覆盖，自愈（v1.0 的 SET NX + HSET 两步法
中间死亡会留下 "version 存在但 files 半写" 的不可自愈态，已废弃）。
```

### 5.6 失败与恢复语义

| 场景 | 行为 |
|---|---|
| Redis 启动时不可用 | WARN + 沿用本地现状运行，服务照常起；每周期重连 |
| Redis 运行中不可用 | 同上，本地配置继续生效 |
| 某文件校验不过 | **该文件保留本地旧值、版本不推进、心跳 partial、同版本每周期重试**（D6；v1.0 "跳过但报成功" 已废弃） |
| Redis 被清空（version 消失） | 仅**有播种资格**的机器重新执行种子脚本（§5.5：status=ok 且文件与 last_ok 清单匹配，或无状态文件的全新机器）；partial/混合代机器不参与，防止把混合镜像种成事实源 |
| version 回退（< 本地） | 忽略 + 告警（防 Redis 回滚倒灌旧配置） |
| 读到写方替换中的跨界快照 | 双读版本不一致 → 放弃本次，下周期重试（§5.4 步骤 3） |
| 半写文件 | 不存在——tmp+rename 原子性 + ReloadService 两 tick 防抖双保险 |

---

## 6. 保护规则

### 6.1 配置源自毁防御（最重要）

同步内容**禁止 `[redis]`、`[admin]`、`[config_sync]` 节**——否则一次误配置可让全部机器失去连接配置源或配置中心自我保护能力：

- Admin API 保存前校验拒绝（第一道）
- ConfigSyncService 落盘前逐文件检测，含上述本地专属 section 的文件按 §5.4 步骤 5 处理：不落盘、版本不推进、partial 告警（第二道，防绕过 API 直写 Redis）

### 6.2 文件名白名单

`^[0-9]{2}-[a-z0-9_-]+\.ini$`——防路径穿越、防野文件混入 fingerprint。

### 6.3 保留路径规则（v1.2 新增，D7）

托管文件**禁止声明** `[auth_whitelist]` 或 `[path_blacklist]` 中路径等于 `/admin`、`/api/admin`，或以它们为段前缀（`/admin/...`、`/api/admin/...`）的条目：

- **path_blacklist 侧是硬漏洞**：空值即"该前缀全封"（path_blacklist.hpp:16-17），且 `check()` 先查 block-all 列表、命中即返（:46-57），role 列表和 handler 都没机会跑——托管配置一条 `/api/admin/ = ` 就能把 admin API 在代码级检查之前全员 403（配置中心自 DoS）
- **auth_whitelist 侧是纵深防御**：把 admin 路径加白会跳过 JWT，handler 对"无 principal"一律拒绝（§7.1），仍安全，但一并保留以免语义混乱

此规则在两处强制执行：Admin API 保存校验（拒绝整次提交）+ ConfigSyncService 落盘校验（违反 → 该文件跳过 + partial）。运维如确需调整 admin 相关白/黑名单，只能改**本地**文件（99-local.ini 等，永不同步）。`/admin` HTML 页与 `/api/admin/` 前缀的放行同样落在本地层：`12-config-sync.ini` 出厂自带的 `[auth_whitelist]`（§5.2；v1.5 起 `/api/admin/` 前缀也在此——业务 JWT 对 admin 面退出，授权由 admin handler 独立验证），与托管配置无关。

**已知边界**：托管配置声明 `/ = `（封全部路径）属于一般性自残（整个服务可见地拒绝服务，reload 立即生效、影响面全站），不属于配置中心自我锁定范畴，不在本规则内——页面源码模式对 block-all 全局规则给出二次确认即可（Phase 3）。

### 6.4 永不同步清单（本地专属）

| 文件 | 原因 |
|---|---|
| `11-redis.ini` | `[redis]` 连接信息（配置源自身） |
| `12-config-sync.ini` | 同步服务自身参数 + `[admin]` 节（账号/密钥/insecure_no_auth 逃生口，v1.5）——配置中心的自我保护全部落在此文件 |
| `99-local.ini` | 单机逃生舱：加载序最后，可临时覆盖 Redis 值做单机调整；admin 相关白/黑名单规则的合法落点（§6.3） |

（v1.1 的 `13-admin-guard.ini` 已移除：它与 allow_insecure_admin 在安全链路里互斥——jwt_disabled 时无 claims，role 路径在 handler 之前就 403，逃生口永远走不到；且按 §6.3 它本身可被托管配置架空。admin 授权唯一执行点收敛为代码级，见 §7.1。）

### 6.5 新机器部署

**默认 async 模式下，"裸启即用"不成立**——启动期配置先于首同步被消费。三种部署方式：

1. **推荐**：从现有机器拷贝整份 `config.d`（含托管文件）→ 直接启动，后续以 Redis 为准
2. 仅放置永不同步清单三件套启动 → 首同步拉全量（热加载面即刻生效）→ **手动重启一次**使启动期配置生效
3. `first_pull = blocking` 模式：启动前阻塞拉取（direct 模式临时池 + 整体 deadline，§5.3），无需重启

`AppConfig` 默认值可起服务（MysqlPool 连接惰性不阻塞），故方式 2 不会起不来，只是启动期配置要重启才正确。

Rollout 顺序：首次开启 `enabled=true` 时，先在一台已具备完整托管配置且状态健康的机器上完成种子/同步，再扩到其他机器。空托管集只作为全新三件套机器的合法种子结果；已有托管文件的机器遇到远端空集会 fail-closed 为 partial，不会删除本地托管文件。

---

## 7. 管理面（Phase 2/3）

### 7.1 Admin API

| 端点 | 方法 | 语义 |
|---|---|---|
| `/api/admin/login` | POST | `{"username","password"}` → `{"token","expires_in"}`；401 统一错误信息（不区分未知用户/密码错） |
| `/api/admin/config` | GET | 返回 `{version, files:[{name, content, restart_required}]}`（读 Redis） |
| `/api/admin/config` | POST | 保存：`{base_version, files:[{name, content}]}` |
| `/api/admin/config/machines` | GET | 心跳 hash → 各机器 `{machine, version, ts, pid, status}` |
| `/admin` | GET | 内嵌 HTML 页（登录框 + 机器状态条）。放行：本地 never-sync 的 `12-config-sync.ini` `[auth_whitelist]`（`/admin` + `/api/admin/` 前缀，§5.2）——业务 JWT 对 admin 面退出，授权由 admin handler 用 admin 密钥独立验证（D8）；删掉白名单条目即整体封死 admin 面，操作者自选 |

**POST 流水线（全部通过才触碰 Redis；最终提交为单个 Lua 原子脚本）**：

```
1. JSON 解析（手写窄解析器，仓库风格，无第三方依赖；
   现仓库无 JSON body 解析能力，唯一先例 extract_first_string_value
   是单值状态机，不够用）
2. 文件名白名单 + [redis] 节拒绝 + 保留路径规则(§6.3) + 非空文件集
3. 整目录 dry-run 校验（不是单文件）：
   临时目录 = 永不同步清单文件拷贝 + 提交文件集 → Config::load(dir)
   （覆盖排序加载、后文件覆盖先文件、跨文件重复 key 的真实语义）
   a. SecurityRules(/*staging=*/true).load_from_config(cfg)
      （staging 构造已存在 security_rules.hpp:386；staging 下 RateLimiter
       以 !staging_ 参数构造、不加载/不持久化快照 security_rules.hpp:130-133）
   b. throwaway asio::io_context 上
      UpstreamManager::prepare_reload(cfg, http_pool_config_from(cfg))
   注：upstream 条目为强校验（A14）；[http_pool] 参数仅类型级把关
     （get_int/get_bool 非法值抛错 + 有限 clamp），无语义范围校验
4. Lua 原子提交（v1.2 强化：前置校验 + staging + RENAME + audit pcall）：
   KEYS: version, files, audit, staging
   ARGV: base_version, audit_json, 文件名,内容...

   -- 返回: >0 新版本 | -1 CAS 冲突 | -2 ARGV 错(奇偶/非数字/空集)
   --       | -3 key 类型异常 | -4 version 内容非数字
   if (#ARGV - 2) % 2 ~= 0 then return -2 end
   if #ARGV < 4 then return -2 end        -- 空文件集：API 层已拒，脚本双层拒绝
   local base = tonumber(ARGV[1])
   if base == nil then return -2 end
   local t = redis.call('TYPE', KEYS[1]).ok
   if t ~= 'string' and t ~= 'none' then return -3 end
   t = redis.call('TYPE', KEYS[2]).ok
   if t ~= 'hash' and t ~= 'none' then return -3 end
   t = redis.call('TYPE', KEYS[3]).ok
   if t ~= 'list' and t ~= 'none' then return -3 end
   local cur = tonumber(redis.call('GET', KEYS[1]) or '0')
   if cur == nil then return -4 end       -- key 是 string 但内容如 "abc"
   if cur ~= base then return -1 end
   redis.call('DEL', KEYS[4])
   redis.call('HSET', KEYS[4], unpack(ARGV, 3, #ARGV))     -- 先建完整集
   redis.call('RENAME', KEYS[4], KEYS[2])                   -- 原子替换核心状态
   local newv = redis.call('INCR', KEYS[1])
   pcall(function()                                          -- audit 尽力而为
     redis.call('LPUSH', KEYS[3], ARGV[2])
     redis.call('LTRIM', KEYS[3], 0, 199)
   end)
   return newv

   设计要点（"原子执行"不等于"出错回滚"，Redis Lua 后续命令报错时
   先前的写不回滚，故把报错面消灭在写核心状态之前）：
   - 三个 key 的 TYPE 前置校验：audit 被误改成 string 这类错误在
     任何写发生之前就以 -3 拒绝（需要人工修 key，API 回 500+原因）
   - **-4 独立错误码（v1.3）**：TYPE=string 但内容非数字（如 "abc"）时
     tonumber 为 nil——v1.2 写法会落入 `nil ~= base` 被误报成 CAS 冲突
     （-1/409），管理员会无休止重试一个永远"冲突"的假象；
     -4 → 500 + "version key 内容非数字，需人工修复"，与冲突彻底区分
   - **空文件集双层拒绝（v1.3）**：`#ARGV < 4 → -2`——API 层已拒空集，
     脚本侧兜底；空托管集只可能合法来自种子脚本（§5.5 显式分支），
     保存永远不允许清空全部托管文件
   - staging 先建完整文件集再 RENAME：files 永远不会出现半写态
     （DEL+逐条 HSET 的旧写法在 OOM 中断时会留半写 hash）
   - audit 用 pcall 且排在核心状态之后：audit 失败不阻断提交
   - 残余（可接受）：OOM 类错误若发生在 RENAME 之后、INCR 之前，
     状态为 "files 新 / version 旧"——对读者不可见（版本门未动），
     且管理员 base_version 仍有效、重试即自愈；危险方向
     "version 新 / files 旧" 在此写法下不可能出现
   - API 返回码映射：-1 → 409（另行 GET 当前 version 回给页面）；
     -2/-3/-4 → 500 + 原因；EVAL 不在重试白名单，正合 CAS 语义
5. 本机同样走轮询路径收敛（写侧与同步侧统一，无特权快路径）
```

**认证与授权（v1.5 重做——admin 独立信任域，D8；修复 v1.2-v1.4 的跨信任域提权）**：

> v1.2-v1.4 实现信任业务公钥验证的 principal（`ctx.principal`）+ `role:admin`——但现有 JWT 是**业务 token**（pixiu 体系签发，网关只持公钥），业务系统的角色词汇表与网关管理角色无关：任何业务 token 只要 claims 撞上 `role:admin`/`roles:["admin"]` 即可改写网关全部配置。v1.5 起业务凭证对 admin 面一律无效。

1. **独立密钥对与 issuer**：`jwt_keys/admin-private-key.pem`（仅签发）+ `jwt_keys/admin-public-key.pem`（验证），issuer 常量 `asio-owen-admin`（与业务 `pixiu-gateway` 不同——业务 token 即使角色撞名，issuer/签名也不匹配 → 401）。admin key 路径相对 `config_base`（server 所在目录）解析；验证直接复用 `JWTAuth`（RS256 裸 EVP + claims 提取，jwt_auth.hpp:142/167/322-353）；**签发**镜像 `verify_rs256` 的裸 EVP 模式实现 `sign_rs256`（EVP_PKEY 启动加载一次 + EVP_DigestSign）——不用 jwt-cpp 的 PEM 签名路径（其 PEM 处理在 macOS Homebrew OpenSSL 3.x 有已知问题，A25）
2. **登录端点 `POST /api/admin/login`**：账号在 never-sync 本地文件 `[admin]` 节（§5.2），PBKDF2-SHA256 口令哈希（`PKCS5_PBKDF2_HMAC`，OpenSSL::Crypto 已链接；格式内嵌迭代数，默认 100k）+ `CRYPTO_memcmp` 恒时比较 + 未知用户/密码错统一 401；每次登录尝试重读本地 admin 配置（登录低频，无热加载需求），admin API 鉴权也每请求重读同一份本地配置，保证账号/密钥/逃生口语义一致
3. **签发 token**：`iss=asio-owen-admin, sub/name=<username>, roles=["admin"], exp=now+token_ttl_min, iat`；页面登录框获取后存 localStorage（原"token 粘贴框"保留为调试入口）。口令轮换不会让已签发的 stateless token 立即失效；立即失效需轮换 admin 密钥对或等待 `exp`
4. **authorize_admin 新判定序**：本地 admin 配置不可读取 → **503 fail-closed**；`insecure_no_auth=true` → 放行（实验室逃生口，与业务 `jwt_disabled` 脱钩）；非 insecure 且未配置（无账号或无密钥）→ **503 fail-closed**；Bearer 缺失/验证失败 → **401**；无 admin 角色 → 403（自签 token 必有 role，纯防御）。`ctx.principal`（业务链路管道）保留但**不再用于 admin 授权**
5. **防爆破**（叠加在现有 IP/全局限流之上，A24）：内存锁定表 `<(client_ip, username), {失败数, 锁定截止}>`，5 次失败锁 15 分钟，成功清零，容量上限 + 过期清扫；`client_ip` 复用安全链路 real-IP（XFF+trusted proxies）——`CheckResult` 增加 `client_ip` 字段（check_snapshot 已算出，暴露即可），client_session 存入 `ctx.client_ip`（principal 同模式）；登录成功/失败 LOG（用户名+IP）进审计
6. **业务链路对 admin 面退出**：本地 `[auth_whitelist]` 放行 `/admin` 与 `/api/admin/` 前缀（§5.2）——业务 JWT 不再拦在 admin handler 之前；托管文件仍被 §6.3 保留路径规则禁止触碰这些条目；限流/IP 黑名单在白名单之前照常生效
7. **运维配套**（根目录，沿用 bench.sh 惯例）：`gen_admin_keys.sh`（openssl 生成密钥对，私钥 chmod 600）、`hash_admin_password.py`（python3 stdlib pbkdf2，无 pip 依赖）

### 7.2 Web 页面（内嵌单页）

```
┌─────────────────────────────────────────────────────────────┐
│ asio-owen 配置中心            当前版本 v37 ▏我的同步 v37 ✓    │
├──────────┬──────────────────────────────────────────────────┤
│ ▸ 路由    │  [upstream]                              热生效 ● │
│   连接池  │  ┌──────────────┬────────────────┬──────┐        │
│   网关    │  │ service      │ upstream        │ 操作 │        │
│   安全    │  │ zebra-config │ 127.0.0.1:30001  │  ✕  │        │
│   CORS   │  │ zebra-passport│127.0.0.1:30002  │  ✕  │        │
│   限流    │  │ ＋ 添加上游…                      │        │
│   黑白名单│  └──────────────┴────────────────┴──────┘        │
│ ──────── │  [gateway]                                       │
│ ▸ 服务○   │  json_keys_snake_to_camel        [开●──]         │
│          │  ──────────────────────────────────────          │
│          │  [ 保存到 Redis ]        [ 源码模式 ]              │
├──────────┴──────────────────────────────────────────────────┤
│ ● 热生效 ○ 重启生效    机器: A ✓v37  B ✓v37  C ⚠partial(v35)  │
└─────────────────────────────────────────────────────────────┘
```

- 顶部**登录框**（用户名+口令 → `/api/admin/login` → token 存 localStorage，v1.5）；原"粘贴 token"输入框保留为调试入口；token 过期后的 401 自动弹回登录态
- tab 按 section 分组，徽标区分热生效/重启生效（D2 范围含启动期配置，防误判）
- 表单模式：bool→开关、端口→数字输入校验；源码模式：直接编辑 ini 文本；**block-all 级黑名单规则（如 `/ =`）保存时二次确认**（§6.3 已知边界）
- 保存携带 base_version（乐观锁）；409 时提示刷新重试
- 底部机器同步状态轮询 `/api/admin/config/machines`；`partial` 状态标 ⚠ 并展示失败文件与原因（D6）
- 单文件原生 JS，无 CDN 依赖（内网可用）；handler 直接回 `Content-Type: text/html`（仓库无静态文件服务，这是最贴现状的实现）

---

## 8. 分阶段计划

| 阶段 | 内容 | 交付物 | 验收 |
|---|---|---|---|
| Phase 0（本设计） | 设计评审 + 三轮评审修订 | 本文档 v1.3 | 已定稿（2026-08-16） |
| **Phase 1** | ConfigSyncService：Lua 原子种子（含播种资格判定、**空托管集分支**）、轮询（双读版本、**version 非数字判定**）、原子落盘、**部分失败不推进版本+partial 心跳**、删除检测、心跳、`[config_sync]` 配置、Application 接线（生命周期契约 §5.1）、保留路径校验（§6.3）、可选 blocking 首拉取（direct 临时池+整体 deadline）+ 漂移告警 | 机器侧闭环 | 单测 + 集成（本地 Redis）：种子（含并发种子竞争、**混合代机器拒绝播种**、**仅三件套新机器空集种子成功**）→ redis-cli 改 files+INCR → 本地文件更新 → 日志 "upstreams hot-reloaded" → curl 新路由生效；坏文件场景：版本不推进、心跳 partial、修复后收敛；**托管文件声明 /api/admin 黑名单规则 → 被同步层拒绝**；version key 写入 "abc" → ERROR 不误报 |
| **Phase 2** | 手写 JSON parser + Admin API（整目录 dry-run+保留路径校验、Lua 前置校验+staging+RENAME+CAS 提交、audit pcall）——已实施（v1.4 语义）。**待实施 v1.5 增量（D8）**：admin 独立信任域（密钥对+issuer+裸 EVP 签发）、`/api/admin/login` + PBKDF2 账号、防爆破锁定、`insecure_no_auth` 替代 `allow_insecure_admin`、`ctx.client_ip` 管道、页面登录框 | 写路径闭环 + 管理凭证闭环 | API 级测试：非法配置被拦、乐观锁 409、audit key 类型破坏 → -3 拒绝且核心状态未动、**version key 非数字 → -4 不误报 409**、**空文件集 → API+脚本双层拒绝**、保存后 Redis 原子更新；**v1.5：业务公钥签的 token 带 `roles:["admin"]` → 401（提权回归测试）、登录成功/锁定/未配置 503、PBKDF2 正误+恒时、admin token 过期 401** |
| **Phase 3** | 内嵌多 tab 页面（含 partial 状态展示、block-all 二次确认） | 管理界面 | 手工验收：双机同步演示、表单/源码双模式、机器状态条 |
| Phase 4（后置可选） | 版本历史/一键回滚（audit 扩展为版本快照）、按机器分组灰度、pub/sub 即时推送（专用常连接订阅组件，现池架构无法承载，独立工程评估） | 增强 | 另行设计 |

各阶段独立可交付、可单独回滚（Phase 1 有 `enabled=false` 默认关闭开关）。

---

## 9. 风险与对策

| 风险 | 对策 | 残余风险 |
|---|---|---|
| Redis 整体故障 | 本地镜像兜底（D1 方案天然优势）：运行不受影响，仅停止同步 | 新机器无法首次部署（需人工放置本地文件，§6.5） |
| 坏配置进 Redis | Admin API 整目录 dry-run + 保留路径校验拦截；绕过 API 直写时：该文件不落盘 + **版本不推进 + partial 心跳 + 同版本每周期重试告警**（告警责任在同步层——被跳过的文件不落盘，ReloadService 不会替它告警） | 坏文件在修复前该机器此文件保持旧值（可见、可接受） |
| Lua 脚本中途报错（如 key 类型被误改） | TYPE 前置校验把可预见报错消灭在写核心状态之前（-3 拒绝）；staging+RENAME 保证 files 无半写；audit pcall 不阻断提交 | OOM 类发生在 RENAME 后 INCR 前：files 新/version 旧，读者不可见 + 同 base_version 重试自愈；"version 新/files 旧"方向不可能出现 |
| 进程在种子/保存中途死亡 | 脚本原子性 + staging/RENAME 顺序：要么整体生效、要么 version 未动可重跑 | 无 |
| 多机首启种子竞争 | 脚本内 TYPE/EXISTS 原子判定 | 无 |
| **混合代镜像被重新种成事实源** | 播种资格判定：仅 status=ok 且文件 hash 匹配 last_ok 清单、或无状态文件的全新机器可播种（§5.5） | 全集群均无资格时需人工介入（可检测：ERROR 日志 + 无心跳推进） |
| 空远端文件集误清空本地托管文件 | 同步层 v1.4 守卫：remote files 为空且本地/状态中存在托管文件时，拒绝删除、状态/心跳 partial、同版本重试 | 首次 rollout 仍应先由满配置机器种子，避免 Redis 事实源为空 |
| 管理员并发修改互踩 | Lua 脚本内 CAS（base_version）→ 409 | 无 |
| in-flight Redis 异步链 vs RedisPool 析构 | §5.1 生命周期契约：串行 tick + shared_ptr 自持有 + 有界排空 + 先于 redis shutdown | 排空超时上限后延迟释放（不 UAF），ERROR 日志可见 |
| admin 权限被配置中心自身削弱/自 DoS | 唯一执行点=代码级（v1.5 起用 admin 密钥独立验证，D8）+ 保留路径规则禁止托管文件触碰 /admin、/api/admin 白/黑名单（§6.3） | 托管配置封全站（`/ =`）属一般性自残，页面二次确认缓解（§6.3 已知边界） |
| **业务 token 撞名提权（v1.5 修复对象）** | admin 独立信任域：业务公钥签的 token 对 admin API 一律 401（issuer/密钥均不匹配）；提权回归测试入 Phase 2 验收 | 无 |
| 登录端点暴力破解 | PBKDF2(100k) + 恒时比较 + 统一 401 + (IP,账号) 维度 5 次锁 15 分钟 + 现有 IP/全局限流前置兜底（A24） | 跨 IP 分布式喷洒仅有限流兜底（无 per-account 全局锁；规模小可接受，Phase 4 再评估） |
| blocking 首拉取超时被建池吃掉 | 临时池强制 direct 模式（无 worker 预建连）+ 整体 deadline 三检查点（§5.3） | 无 |
| dry-run 构造 UpstreamManager 开销 | HttpPool 绑 throwaway io_context、惰性建连不 run | 若实测 prepare_reload 分配超预期，抽静态校验函数（二期优化项） |
| 轮询风暴（机器多） | GET version 每机 5s 一次 O(1)；HGETALL 仅版本变化时发生 | 千台级再评估（当前规模无压力） |

---

## 10. 附录：已核对事实清单（设计依据，全部读码验证，2026-08-16/17）

| # | 事实 | 位置 |
|---|---|---|
| A1 | fingerprint = 文件名+size+mtime；两 tick 防抖；load 中再变弃本次；失败不确认、下周期重试 | `src/app/reload_service.hpp:47-113, 136-180` |
| A2 | fingerprint 只扫 `*.ini`（状态文件须非 .ini 后缀） | `reload_service.hpp:158` |
| A3 | 热加载两阶段 prepare/publish，security 先于 upstreams publish | `reload_service.hpp:82-87` |
| A4 | 原子写惯用法：`.tmp` + `std::rename` | `src/security/rate_limiter.hpp:284-288` |
| A5 | `SecurityRules(bool staging)` 存在；staging 下 RateLimiter 不加载/不持久化快照 | `src/security/security_rules.hpp:386, 130-133` |
| A6 | `build_jwt_auth` static、纯函数（dry-run 可复用） | `security_rules.hpp:395-398` |
| A7 | GET/HGETALL 在只读重试白名单；写命令（HSET/INCR/EVAL）不重试 | `src/db/redis_pool.hpp:455-473` |
| A8 | 服务启停顺序：timers → server → mysql/redis shutdown；新服务须在 redis shutdown 前停 | `src/app/application.cpp:189-208` |
| A9 | 认证复用：auth_whitelist 放行、JWT 默认、path_blacklist `role:admin` 角色门槛 | `security_rules.hpp:205-289` |
| A10 | 仓库无 JSON body 解析器（admin API 需自建） | 全仓 grep，仅 `extract_first_string_value` 单值状态机 |
| A11 | 仓库无静态文件服务（页面用 handler 回 HTML 字符串） | `src/http/response_builder.hpp:143-145` 默认 JSON |
| A12 | pub/sub 无法骑在现池上（阻塞占连接/线程 + 500ms 超时杀阻塞读） | `redis_pool.hpp` 架构 + `config.d/11-redis.ini` |
| A13 | 热加载面/启动期面 section 清单已有文档 | `docs/CONFIG_REFACTOR_PLAN.md` |
| A14 | UpstreamManager::prepare_reload 对 **upstream 条目**强校验（缺冒号/空 host/端口范围/尾随字符等，任一非法抛异常拒绝整次）；**[http_pool] 参数仅类型级把关**——`http_pool_config_from` 只做 get_int/get_bool 类型解析（非法值抛错）与有限 clamp，无语义范围校验 | `src/http/upstream_manager.hpp:79-151`、`src/app/app_config.hpp:33-44` |
| A15 | SnapshotService 已被限流器快照占用，新服务命名 ConfigSyncService | `src/app/snapshot_service.hpp` |
| A16 | RedisPool 契约原文："stop HTTP/coroutine scheduling before destroying RedisPool"（生命周期设计依据） | `src/db/redis_pool.hpp:31` |
| A17 | `[path_blacklist]` 默认节为空（仅注释示例）——admin 授权不能只靠该可变配置 | `config.d/34-path_blacklist.ini` |
| A18 | 角色检查机制（claims.roles 与路径要求比对、`has_role`）已存在，代码级 admin 检查可复用同款 | `security_rules.hpp:271-284` |
| A19 | `parse_redis_reply` 扁平化数组回复 → Lua 脚本只返回标量整数 | `src/db/redis_reply.cpp:15-30` |
| A20 | PathBlacklist：**空值 = 该前缀全封**（reload 时 val.empty → block-all 列表）；`check()` 先查 block-all 列表命中即返，role 列表其后——托管配置可用 `/api/admin/ = ` 在 handler 之前封死 admin API（保留路径规则 §6.3 的依据） | `src/security/path_blacklist.hpp:14-17, 46-57` |
| A21 | worker 模式建池开销：min_size=4、connect_timeout_ms=1000、acquire_timeout_ms=3000——blocking 首拉取不可用 worker 临时池（direct 模式依据） | `config.d/11-redis.ini:8,13,18` |
| A22 | Redis Lua 语义：脚本对并发原子（不交错），但**不回滚**——中途命令报错时先前写保留（脚本前置校验 + staging/RENAME 的依据） | Redis 官方语义（非本仓库代码） |
| A23 | AuthWhitelist 前缀语义：`/` 开头且 `/` 结尾的条目按纯前缀匹配（`path.find(p)==0`，无边界检查）——`path = /api/admin/` 覆盖全部 `/api/admin/*` 子路径（v1.5 业务 JWT 退出 admin 面的依据） | `src/security/auth_whitelist.hpp:16-26, 41-43` |
| A24 | 安全链路顺序：IP 黑名单 → **限流** → OPTIONS → **白名单** → JWT → 路径黑名单——白名单路径仍被限流，登录端点天然有 IP/全局限流兜底 | `src/security/security_rules.hpp:246-302` |
| A25 | jwt-cpp 已链接进主程序（可签名），但其 PEM 处理在 macOS Homebrew OpenSSL 3.x 有已知问题（RS256 验证因此绕开 jwt-cpp 用裸 EVP）——admin token 签发须镜像同模式；`JWTAuth` 类可直接复用做 admin token 验证 | `CMakeLists.txt:97-117, 216-225`；`src/security/jwt_auth.hpp:239-265, 322-353` |
| A26 | 现有 JWT 为业务 token：公钥自 pixiu/dubbo-go-pixiu 拷入、issuer `pixiu-gateway`、登录端点在业务侧（zebra-passport）——网关公钥-only 是部署现状，非文档化设计；admin 凭证与业务凭证必须分家（D8 依据） | `jwt_keys/README.md:7-8`、`config.d/30-security.ini:10`、`config.d/33-auth_whitelist.ini:16` |
| A27 | OpenSSL::SSL/Crypto 已 PUBLIC 链接进 core；admin 密钥路径相对 `config_base` 解析，CMake 在 build 阶段拷贝可选 `jwt_keys/` 到 build 目录；业务 JWT 密钥路径仍沿用安全链路既有 CWD 语义 | `CMakeLists.txt`；`src/app/admin/config_admin.hpp:554-563`；`src/security/security_rules.hpp:440-455` |
| A28 | 仓库无 tools/ 目录，运维脚本惯例在根目录（bench.sh 等）——admin 密钥生成/口令哈希脚本放根目录 | 仓库根 |

---

## 修订记录

- **v1.5.3（2026-08-17）**：GCC 11.4 在 ConfigSyncService 回调化后继续于 `handle_api_admin_config` 复杂协程触发同一 `build_special_member_call` ICE。管理配置 GET 双读、POST CAS/冲突查询及 machines 查询改为普通 `AdminRequestOperation` 回调状态机；HTTP handler 通过 `async_initiate(use_awaitable)` 返回框架所需 awaitable，本身不再是 C++ coroutine。Redis 仍在请求 executor 上 `co_spawn` 既有 `cmd_argv`，不阻塞 io_context。
- **v1.5.2（2026-08-17）**：Ubuntu 22.04 / GCC 11.4 兼容修订。ConfigSyncService 的复杂 helper coroutine 两次触发 `build_special_member_call, cp/call.c:10200` ICE；按仓库既有 GCC 11 规避经验改为普通回调状态机，仅通过 `co_spawn` 直接调用已验证的 `RedisPool::cmd_argv`，并移除 admin Redis 调用的无意义包装协程、把同步登录逻辑移出协程帧。业务顺序、双读、seed、heartbeat 与生命周期语义不变。
- **v1.5.1（2026-08-17）**：实现复核修订。①托管文件新增禁止 `[admin]`、`[config_sync]`，与既有 `[redis]` 禁止一起保护配置源与 admin 信任域；API 保存与同步落盘双侧校验 ②`authorize_admin` 与登录端点统一每请求重读本地 admin 配置，消除登录签发与 API 验签的热变更不一致 ③admin key 相对路径改为按 `config_base` 解析，`jwt_keys/` 改 build 阶段拷贝，补充 stateless token 失效边界
- **v1.5（2026-08-17）**：admin 鉴权重设计（用户指出业务 token 与网关管理凭证是两套体系；选定"完整登录端点"方案）。①新增 D8：admin 独立信任域——独立 RS256 密钥对 + issuer `asio-owen-admin`，业务 JWT 的 token 一律 401，修复 v1.2-v1.4 信任业务公钥 principal 导致的**跨信任域提权**（业务 token 撞名 `role:admin` 即可改网关配置）②§7.1 认证段重写：`POST /api/admin/login`（PBKDF2-SHA256 账号、恒时比较、统一 401、(IP,账号) 5 次锁 15 分钟）、裸 EVP 签发（镜像 verify_rs256，避开 jwt-cpp PEM 已知问题 A25）、`ctx.client_ip` 管道、未配置 503 fail-closed③§5.2：`[config_sync].allow_insecure_admin` 移除，改为 `[admin] insecure_no_auth`（与业务 jwt_disabled 脱钩）；本地白名单增 `/api/admin/` 前缀——业务 JWT 对 admin 面整体退出④§8 Phase 2 列 v1.5 增量与提权回归验收⑤新增 A23-A28。
- **v1.4（2026-08-16）**：实现评审修订。空远端托管集保护：`HGETALL files` 为空且本地文件系统或状态文件仍有托管文件时，不执行删除、不推进 `synced_version`，写 partial 状态并通过心跳暴露 `remote_files` 失败项；补 rollout 顺序说明，要求首次开启先由满配置机器种子，空托管集仅保留为全新三件套机器的合法种子结果。
- **v1.3（2026-08-16）**：第三轮评审修订。①种子脚本空托管集分支：`#ARGV==0` 时 staging 从未创建，直接 RENAME 会因 source key 不存在报错中断——显式 `DEL files + SET version 1 + return 1`（仅三件套新机器是合法空集场景）；保存脚本反向加 `#ARGV < 4 → -2` 空集双层拒绝（保存不允许清空全部托管文件）②`/admin` 页面放行策略统一（§5.2/§6.3/§7.1）：本地 never-sync 的 12-config-sync.ini 出厂自带 `[auth_whitelist] path = /admin`，消除 v1.2 中 §6.3（禁托管 whitelist）与 §7.1（"whitelist 放行"）的矛盾；删除该条即连页面一起封③SAVE 脚本新增 -4 invalid_version：version key TYPE=string 但内容非数字时 tonumber 为 nil，v1.2 写法会误报 CAS 冲突（-1/409 假象）；同步侧 §5.4 增加"非数字 → ERROR 不播种不同步"判定（此时 TYPE=string，种子脚本会误判"已存在"，须由该判定拦住）④Phase 1/2 验收项补充对应场景
- **v1.2（2026-08-16）**：第二轮评审修订。①Lua 脚本补"出错不回滚"防御：TYPE 前置校验（-3 拒绝）、files 经 staging key 构建 + RENAME 原子替换（杜绝半写）、audit 改 pcall 尽力而为并排在核心状态之后；明确残余 OOM 语义（只可能是 files 新/version 旧，同 base 重试自愈）②移除 13-admin-guard.ini：与 allow_insecure_admin 在安全链路互斥（jwt_disabled 无 claims 时 role 路径先于 handler 403），admin 授权唯一执行点收敛为代码级（含 principal 缺失一律拒绝）③新增保留路径规则（§6.3/D7）：托管文件禁止声明 /admin、/api/admin 相关 auth_whitelist/path_blacklist 条目，堵住"空值黑名单在 handler 前全封 admin API"的自 DoS（A20）④blocking 首拉取超时语义收口：临时池强制 direct 模式（规避 worker min_size=4 预建连，A21）+ 整体 deadline 三检查点⑤播种资格判定（§5.5）：仅 status=ok 且文件 hash 匹配 last_ok 清单、或无状态文件的全新机器可播种，防混合代镜像成为事实源；状态文件增加 last_ok 清单⑥新增 A20–A22
- **v1.1（2026-08-16）**：评审意见修订。①种子与保存改 Redis Lua 单脚本原子执行（消除两步写中途死亡的撕裂态），同步读加 version 双读防跨界快照 ②部分同步失败不推进 synced_version + 心跳 partial 状态 + 同版本幂等重试（替代"跳过但报成功"）；相应修正 §9 中"由 ReloadService 反复告警"的错误表述 ③启动语义如实界定：默认 async = 运行期镜像同步，新机器部署三选一（拷贝/裸启+重启/blocking）；新增可选 blocking 首拉取与启动配置漂移告警 ④admin 授权改三层：代码级 role:admin（HttpContext 携带已验证 claims）为主、jwt_disabled 默认 503、never-sync 本地兜底规则 13-admin-guard.ini ⑤新增 §5.1 生命周期契约（串行 tick、shared_ptr 自持有、有界排空、先于 RedisPool shutdown）⑥dry-run 改整目录校验（含 never-sync 文件，覆盖排序/覆盖语义）⑦machine 心跳 field 稳定化（hostname，pid 移入 value，重启不累积）⑧A14 表述修正，新增 A16–A19
- **v1.0（2026-08-16）**：初稿
