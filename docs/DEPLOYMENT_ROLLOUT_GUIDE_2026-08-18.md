# 配置中心首次上线与滚动部署指南

**日期**：2026-08-18

**适用范围**：Kubernetes 单 Pod 首次上线，验收通过后扩容；后续配置先行并冻结配置，再滚动发布代码。

**相关设计**：`docs/CONFIG_CENTER_DESIGN_2026-08-16.md`

**版本历史设计**：`docs/CONFIG_HISTORY_DESIGN_2026-08-18.md`（Phase 4 已实现，生产迁移和目标机门禁待执行）

## 1. 发布原则

1. 首次上线只启动一个 Pod，由该 Pod 完成配置中心播种和验收；验收通过前不得扩容。
2. 后续发布先完成配置修改和验收，再冻结 Redis 配置版本，冻结期间不得继续改配置。
3. 滚动发布中的新 Pod 必须拉到冻结版本并上报 `ok` 后才能接收流量。
4. 配置先行的前提是新配置兼容当前旧代码，回滚代码也必须能读取冻结版本。
5. 每个 Pod 使用独立、可写的本地配置目录和状态文件，不得由多个 Pod 共享写目录。

## 2. 部署前准备

### 2.1 Redis 与环境隔离

- 所有 Pod 连接同一个目标集群 Redis 实例和 DB。
- 生产、预发及其他集群必须使用不同 Redis 实例或 DB。当前 key 固定为
  `asio_owen:config:*`，没有环境命名空间，混用会互相覆盖。
- 首次上线前确认目标 Redis 中不存在其他环境遗留的以下 key：
  - `asio_owen:config:version`
  - `asio_owen:config:files`
  - `asio_owen:config:machines`
  - `asio_owen:config:history:meta`
  - `asio_owen:config:history:index`

若 version 已存在，还应按该确定版本检查 `asio_owen:config:history:<version>`，不要使用 `KEYS history:*` 扫描生产库。

不要在未核对归属的情况下直接删除 Redis key。

### 2.2 本地专属配置

以下文件永不从 Redis 同步，必须随每个 Pod 正确下发：

- `config.d/11-redis.ini`
- `config.d/12-config-sync.ini`
- `config.d/99-local.ini`（可选，仅用于单实例临时覆盖）

推荐同步配置：

```ini
[config_sync]
enabled = true
sync_interval_sec = 5
machine_name =
first_pull = blocking
first_pull_timeout_ms = 3000

[config_history]
read_mode = required
auto_migrate_legacy = true
```

`machine_name` 留空时使用 Pod hostname。扩容后每个 Pod hostname 不同，因此心跳不会互相覆盖。不要为所有副本配置相同的 `machine_name`。

`[config_history]` 与 `[config_sync]` 一样是 never-sync 本地配置。默认保持 `read_mode=required`；`auto_migrate_legacy=true` 只在确认旧 `version/files` 完整、history index/meta/当前 snapshot 全空时复制回填一次，不修改旧 version/files。两个参数都只在进程启动时读取。

单 Pod 上线验收与后续扩容必须使用同一份上述配置。验收通过后只调整副本数，不再切换 `enabled`、`first_pull`、`read_mode` 或自动迁移开关；否则新节点实际运行的是一套未经验收的启动配置。

### 2.3 Admin 凭证

- 上线前必须替换仓库示例中的默认 `admin/admin` 账号口令。
- 使用 `hash_admin_password.py` 生成强口令哈希。
- 所有 Pod 必须挂载同一套 admin 公私钥和相同账号配置。
- 私钥权限保持 `600`，不得写入镜像、日志或 Git。
- 若各 Pod 使用不同密钥，登录 Pod 签发的 token 到其他 Pod 会验证失败，表现为随机 `401`。

### 2.4 Pod 文件系统

- `config.d/` 必须可写，因为同步服务会原子替换托管 INI 文件。
- `.config-sync-state` 必须由每个 Pod 独立维护。
- 不得把运行后生成的 `.config-sync-state` 烘焙进镜像。
- 不得让多个 Pod 共享同一份 RWX `config.d/` 或 `.config-sync-state`。
- 使用只读根文件系统时，应在容器启动阶段把基础配置复制到每 Pod 独立的可写卷，再从该目录启动服务。

### 2.5 Ubuntu GCC 11 制品门禁

项目语言标准保持 C++20，生产兼容基线是 Ubuntu GCC/G++ 11.x 及对应 libstdc++ 11。候选制品上线前必须留存目标镜像内的完整编译和测试结果，不能只凭 macOS Clang 或更新版本 GCC 的结果放行：

```bash
CC=gcc-11 CXX=g++-11 cmake -B build-gcc11 -S .
cmake --build build-gcc11
ctest --test-dir build-gcc11 --output-on-failure
```

构建镜像中的编译器、系统库和运行镜像 ABI 必须与该验证结果匹配。Ubuntu GCC 11 门禁未通过时，不得进入单 Pod 首发验收。

## 3. 首次上线流程

### 3.1 部署单 Pod

1. 为首个 Pod 准备完整且已审核的托管 `config.d/*.ini`。
2. 确认本地 Redis、config sync、admin 配置和密钥均可用。
3. 仅启动一个 Pod，副本数保持为 `1`。
4. 由该 Pod 在 Redis version 不存在时执行原子播种。
5. 首个 Pod 未完成验收前，不得扩容，也不得让空配置 Pod 先启动。

这里“Redis 无配置”特指 `asio_owen:config:version`、files 和 history 均不存在的全新环境。首个 Pod 会把随包本地托管配置一次性写成 v1 的 version/files/snapshot/meta/index；单 Pod 验收与扩容使用同一份本地专属配置，验收后不再修改开关。

`required` 模式会拒绝空托管集合播种。迁移冻结窗口的 `compat` 模式仍兼容旧空首版，但只应作为 legacy-empty 过渡状态，必须通过首次有效保存或迁移补齐历史后切回 `required`。

### 3.2 首次验收

至少确认以下项目：

- `/api/health` 返回成功。
- Redis 中 `asio_owen:config:version` 存在且为正整数。
- Redis 中 `asio_owen:config:files` 是预期的完整文件集合，而不是意外空集合。
- `/api/admin/config` 返回的 version 和文件内容正确。
- `/api/admin/config/machines` 中首个 Pod 为 `status=ok`，版本与 Redis 一致。
- 网关路由、JWT、安全规则、Redis、MySQL 及关键业务接口验收通过。
- 日志中不存在 `ConfigSync partial sync`、版本回退、状态文件写失败或启动配置漂移未处理等错误。

记录验收版本，例如 `v1`。只有上述检查通过后，首次上线才算完成。

### 3.3 首次扩容

1. 保持 Redis 配置不变，开始扩容。
2. 新 Pod 使用 `first_pull=blocking`，启动时从 Redis 拉取完整配置。
3. 每个新 Pod 必须上报 `status=ok` 且 version 等于首次验收版本。
4. 达到目标副本数后，逐个验证关键接口和 admin token 跨 Pod 可用性。
5. 所有副本版本一致后结束扩容。

Pod 缩容或重建后，Redis 可能暂时保留旧 Pod 心跳记录。旧记录可根据时间戳判定为离线，不影响其他 Pod 同步。

## 4. 后续配置先行与滚动发布

### 4.1 配置修改与验收

1. 确认待修改配置与当前旧代码兼容。
2. 通过 Admin API 基于当前 version 保存配置，避免覆盖他人并发修改。
3. 等待现有全部 Pod 同步到新 version 并上报 `ok`。
4. 完成业务、网关、安全规则和依赖连接验收。
5. 记录本次发布的冻结版本，例如 `v37`。
6. 从此刻到滚动发布验收结束，禁止再次保存配置。

### 4.2 热加载与启动期配置的区别

- 热加载配置会在代码发布前应用到当前旧 Pod，因此必须保证旧代码可以接受。
- `[server]`、`[mysql]` 等启动期配置写入文件后不会完整改变当前进程；新 Pod 会以新值启动，旧 Pod 在被替换前仍使用旧值。
- 启动期配置变化时，滚动期间会短暂存在旧运行值和新运行值并存的状态。端口、依赖地址、凭证或协议变化必须允许这种并存。

如果一项配置只有新代码才能识别或会破坏旧代码，不得直接执行“配置先行”。应采用两阶段兼容发布：

```text
发布同时兼容新旧配置的代码
-> 修改并验收配置
-> 后续版本删除旧配置兼容逻辑
```

### 4.3 滚动发布

推荐 Deployment 策略：

```yaml
strategy:
  type: RollingUpdate
  rollingUpdate:
    maxUnavailable: 0
    maxSurge: 1
```

发布步骤：

1. 确认 Redis version 仍等于冻结版本，且发布窗口内没有配置写入。
2. 启动一个新 Pod，新 Pod 使用 `first_pull=blocking`。
3. 新 Pod 必须同时满足以下条件后才能接流量：
   - 进程健康检查成功；
   - 配置同步状态为 `ok`；
   - 同步 version 等于冻结版本；
   - 关键业务探针成功。
4. 按相同步骤逐个替换旧 Pod。
5. 确认所有存活 Pod 均为 `ok@冻结版本`。
6. 完成业务验收后解除配置冻结。

`terminationGracePeriodSeconds` 应覆盖服务的 5 秒连接排空时间，并预留 Redis 命令结束时间，建议至少设置为 15 秒，再根据线上超时配置调整。

## 5. 回滚流程

1. 停止继续滚动，不修改冻结配置。
2. 确认待回滚代码能够读取当前冻结 version 的配置。
3. 按正常滚动策略部署旧镜像。
4. 每个回滚 Pod 仍必须达到 `ok@冻结版本` 后才能接流量。
5. 全部 Pod 回滚并验收完成后，再决定是否单独回滚配置。

不要同时回滚代码和 Redis 配置。两者应分开操作、分别验收，避免无法判断故障来源。

## 6. 异常处理

### 6.1 Phase 4 首次迁移

旧 Redis 只有 `version/files`、history 命名空间完全为空时，保持单 Pod和配置冻结，以默认 `read_mode=required, auto_migrate_legacy=true` 启动。后台会读取旧镜像、校验容量和内容，在 Lua 中再次确认 history 仍完全为空后，原子生成当前版本 snapshot/meta/index；旧 version/files 不变。迁移成功日志包含 `automatically migrated legacy version/files`。

启动后核对 `/api/admin/config`、历史列表和当前版本详情。若已经存在任意 history 痕迹、镜像为空、key 类型错误、容量超限或迁移期间发生并发写入，自动迁移会拒绝并保持 `history_inconsistent`，不得通过删除 key 绕过。只有显式设置 `auto_migrate_legacy=false` 的特殊环境才使用 `compat` 加 `/api/admin/config/history/migrate` 的人工流程。

### 6.2 首个 Pod 播种失败

- 保持副本数为 `1`，禁止扩容。
- 检查 Redis 连通性、DB、key 类型及本地托管文件校验错误。
- 若 Redis 已存在 version，先确认它是否属于当前环境；不得盲目重新播种。
- 修复后重新验收 version、files 和机器心跳。

### 6.3 新 Pod 为 `partial`

- 不让该 Pod 接流量，不继续扩大滚动范围。
- 根据 machines 心跳 detail 和日志定位失败文件。
- 修复 Redis 配置或 Pod 可写目录问题，等待同一 version 自动重试。
- 不要删除 `.config-sync-state` 来绕过 partial。

### 6.4 Blocking 首拉失败

当前实现会记录错误并回退到本地配置继续启动，不会自动让进程退出。因此只检查 `/api/health` 不足以证明配置正确。发布平台必须把 `status=ok` 且 version 等于冻结版本作为额外 readiness 门禁。

### 6.5 疑似 orphan 或 version 回退

- 保持配置冻结，不执行普通保存、回滚或 GC，也不要直接用 redis-cli 删除历史 key。
- 汇总 AOF/RDB、应用日志、machines 心跳、各 Pod state 和目标 snapshot hash，判断目标是已发布版本还是未发布残留。
- 已发布版本使用 `resolve-orphan` 的 `restore-version` 动作；仅确认未发布、目标恰为 `current+1` 且无 Pod/进程观察到更高版本时，才可使用 `delete-orphan`。
- 两种动作都要求精确强确认文本，完成后检查响应 `history_consistent=true`、当前镜像 hash 及全部 Pod 状态。

### 6.6 本地配置漂移

当前实现发现远端 version 等于本地 `synced_version` 时会直接上报 `ok`，不会重新校验全部本地文件 hash。应避免人工修改 Redis 托管文件；发现漂移时，通过发布新 Redis version 触发重新同步，或下线并重建该 Pod。

`99-local.ini` 会在托管文件之后加载，可用于单 Pod 紧急覆盖，但会有意造成实例差异。问题处理完毕后应及时删除覆盖并重新验收。

## 7. 发布检查清单

### 首次上线

- [ ] 候选制品已通过 Ubuntu GCC/G++ 11 完整编译和测试
- [ ] 仅部署一个 Pod
- [ ] 首个 Pod 本地托管配置完整
- [ ] Redis 环境和 DB 隔离正确
- [ ] 默认 admin 口令已替换
- [ ] 所有副本将使用同一套 admin 密钥
- [ ] `config.d/` 和 `.config-sync-state` 每 Pod 独立可写
- [ ] Redis version/files 正确
- [ ] 旧 Redis 首次升级已自动生成当前 snapshot/meta/index，且 version/files 未改变
- [ ] 首个 Pod 状态为 `ok@验收版本`
- [ ] 业务验收完成后才扩容
- [ ] 扩容后所有 Pod 版本一致

### 后续滚动发布

- [ ] 新配置兼容当前代码和回滚代码
- [ ] 配置已在现有 Pod 上验收
- [ ] 已记录并冻结 Redis version
- [ ] 发布期间禁止修改配置
- [ ] 新 Pod 使用 blocking 首拉取
- [ ] 新 Pod 达到 `ok@冻结版本` 后才接流量
- [ ] 滚动期间启动期配置允许新旧值并存
- [ ] 所有 Pod 验收完成后才解除配置冻结
