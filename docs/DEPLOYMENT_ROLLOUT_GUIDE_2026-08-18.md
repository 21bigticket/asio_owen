# 配置中心首次上线与滚动部署指南

**日期**：2026-08-18

**适用范围**：Kubernetes 单 Pod 首次上线，验收通过后扩容；后续配置先行并冻结配置，再滚动发布代码。

**相关设计**：`docs/CONFIG_CENTER_DESIGN_2026-08-16.md`

**版本历史设计**：`docs/CONFIG_HISTORY_DESIGN_2026-08-18.md`（Phase 4 已实现，Ubuntu GCC 11 目标机门禁已通过）

**运行验收记录**：`docs/PHASE4_PERF_VALIDATION_2026-08-18.md`

## 1. 发布原则

1. 首次上线只启动一个 Pod，由该 Pod 完成配置中心播种和验收；验收通过前不得扩容。
2. 后续发布先完成配置修改和验收，再冻结 Redis 配置版本，冻结期间不得继续改配置。
3. 滚动发布沿用 `/api/health` 作为进程健康检查，发布完成后核对各 Pod 的配置版本和同步状态。
4. 配置先行的前提是新增或修改的配置兼容当前运行代码。
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
- `config.d/99-local.ini`（Ubuntu 部署脚本用于注入固定 Admin 账号和密钥路径；也可承载单实例临时覆盖）

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

- 仓库默认不启用管理员账号。Ubuntu 上直接运行 `rebuild_deploy.sh`：首次部署交互设置密码，后续部署不再询问；口令不会出现在进程参数或部署日志中。
- 脚本默认在 `/etc/asio-owen/admin` 首次生成并持久化 Admin 密钥和账号哈希，私钥权限为 `600`；后续部署复用同一凭证，并在 candidate 的 `99-local.ini` 中注入账号及密钥绝对路径，不复制私钥。
- 可通过 `ADMIN_SECRET_DIR` 和 `ADMIN_USERNAME` 修改固定目录与账号名；固定目录必须位于构建目录之外。非脚本部署可使用 `gen_admin_keys.sh` 和 `hash_admin_password.py <username>` 完成同等配置。
- 所有 Pod 必须挂载同一套 admin 公私钥和相同账号配置。
- 私钥权限保持 `600`，不得写入镜像、日志或 Git。
- 若各 Pod 使用不同密钥，登录 Pod 签发的 token 到其他 Pod 会验证失败，表现为随机 `401`。

单机首次部署直接执行：

```bash
./rebuild_deploy.sh
```

脚本会提示输入并确认密码。需要自定义固定账号或持久目录时，只能在首次创建凭证前指定，并在后续部署保持相同参数：

```bash
ADMIN_USERNAME=ops ADMIN_SECRET_DIR=/etc/asio-owen/admin ./rebuild_deploy.sh
```

`ADMIN_SECRET_DIR` 必须安全备份但不能进入代码仓库。目录丢失后再次执行会生成新密钥并要求设置密码，所有旧 Admin token 随即失效。多节点部署时，只允许一个节点首次生成；其他节点应先安全分发或挂载同一目录，再运行部署脚本，禁止各节点独立生成。

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
2. 运行 `rebuild_deploy.sh`，确认固定 Admin 凭证已创建或复用，candidate 已生成 `99-local.ini`。
3. 确认本地 Redis、config sync、admin 配置和密钥均可用。
4. 仅启动一个 Pod，副本数保持为 `1`。
5. 由该 Pod 在 Redis version 不存在时执行原子播种。
6. 首个 Pod 未完成验收前，不得扩容，也不得让空配置 Pod 先启动。

这里“Redis 无配置”特指 `asio_owen:config:version`、files 和 history 均不存在的全新环境。首个 Pod 会把随包本地托管配置一次性写成 v1 的 version/files/snapshot/meta/index；单 Pod 验收与扩容使用同一份本地专属配置，验收后不再修改开关。

`required` 模式会拒绝空托管集合播种。迁移冻结窗口的 `compat` 模式仍兼容旧空首版，但只应作为 legacy-empty 过渡状态；首次保存有效非空配置并生成完整历史后，必须切回 `required`。

### 3.2 首次验收

至少确认以下项目：

- `/api/health` 返回成功，确认进程 liveness。
- `/api/ready` 返回 HTTP 200 且 `data.ready=true`，确认 blocking 首拉、配置同步状态和 history 一致性均可用。
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
3. 新 Pod 的 `/api/health` 成功且 `/api/ready` 返回 200 后按计划继续扩容。
4. 达到目标副本数后，逐个验证关键接口和 admin token 跨 Pod 可用性。
5. 扩容完成后核对各 Pod 最终均收敛为 `status=ok`，version 等于首次验收版本。

Pod 缩容或重建后，Redis 可能暂时保留旧 Pod 心跳记录。旧记录可根据时间戳判定为离线，不影响其他 Pod 同步。

## 4. 后续配置先行与滚动发布

### 4.1 配置修改与验收

1. 新增或修改的配置必须兼容当前运行代码。
2. 通过 Admin API 基于当前 version 保存配置，避免覆盖他人并发修改。
3. 等待现有全部 Pod 同步到新 version，并完成业务、网关、安全规则和依赖连接验收。
4. 记录本次发布的冻结版本，例如 `v37`。
5. 从此刻到滚动发布验收结束，禁止再次保存配置。

### 4.2 滚动发布

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
2. 按既有策略滚动部署代码，以 `/api/health` 作为 liveness、`/api/ready` 作为 readiness 检查。
3. 滚动完成后验证关键业务接口，并核对所有存活 Pod 最终均为 `ok@冻结版本`。
4. 完成业务验收后解除配置冻结。

当前服务采用确定性 hard-stop：收到终止信号后停止接入并立即取消存量客户端
socket；应用内的 5 秒 deadline 只限制 session drain 等待，不限制阻塞 worker 的
`join()`。`terminationGracePeriodSeconds` 必须大于 MySQL `query_timeout_ms`、Redis
命令超时和其他 worker 最坏收尾时间之和并留出调度余量。按当前 MySQL 30 秒默认值，
建议从 45 秒起配，并在修改相关超时后同步调整；它不应再被描述为“5 秒连接排空”。

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

当前实现会记录错误并回退到本地配置继续启动，不会自动让进程退出。发布平台保持既有 `/api/health` 检查；运维在扩容或滚动完成后通过日志、心跳状态和 version 核对各 Pod 是否最终收敛。

### 6.5 疑似 orphan 或 version 回退

- 保持配置冻结，不执行普通保存、回滚或 GC，也不要直接用 redis-cli 删除历史 key。
- 汇总 AOF/RDB、应用日志、machines 心跳、各 Pod state 和目标 snapshot hash，判断目标是已发布版本还是未发布残留。
- 已发布版本使用 `resolve-orphan` 的 `restore-version` 动作；仅确认未发布、目标恰为 `current+1` 且无 Pod/进程观察到更高版本时，才可使用 `delete-orphan`。
- 两种动作都要求精确强确认文本，完成后检查响应 `history_consistent=true`、当前镜像 hash 及全部 Pod 状态。

### 6.6 本地配置漂移

远端 version 等于本地 `synced_version` 时，同步器仍会扫描本地托管文件并与 `last_ok` hash 校验。若文件缺失、内容漂移或上次状态不是 `ok`，同步器才重新读取并应用当前远端快照；只有文件集合与 hash 都匹配时才直接上报 `ok`。仍应避免人工修改 Redis 托管文件，以免触发不必要的自动覆盖。

`99-local.ini` 会在托管文件之后加载。`rebuild_deploy.sh` 生成的文件包含固定 Admin 账号和密钥路径，必须保留；若人工加入其他单 Pod 紧急覆盖，问题处理完毕后只删除对应覆盖项并重新验收，不要删除整个文件。

## 7. 发布检查清单

### 首次上线

- [ ] 候选制品已通过 Ubuntu GCC/G++ 11 完整编译和测试
- [ ] 仅部署一个 Pod
- [ ] 首个 Pod 本地托管配置完整
- [ ] Redis 环境和 DB 隔离正确
- [ ] `/etc/asio-owen/admin`（或 `ADMIN_SECRET_DIR`）位于构建目录之外且权限正确
- [ ] candidate 的 `99-local.ini` 已注入强口令 Admin 账号和固定密钥绝对路径
- [ ] 所有副本将使用同一套 admin 密钥
- [ ] `config.d/` 和 `.config-sync-state` 每 Pod 独立可写
- [ ] Redis version/files 正确
- [ ] 旧 Redis 首次升级已自动生成当前 snapshot/meta/index，且 version/files 未改变
- [ ] 首个 Pod 状态为 `ok@验收版本`
- [ ] 业务验收完成后才扩容
- [ ] 扩容后所有 Pod 版本一致

### 后续滚动发布

- [ ] 新配置兼容当前运行代码
- [ ] 固定 Admin 凭证目录仍存在，部署日志显示复用而非重新生成
- [ ] 配置已在现有 Pod 上验收
- [ ] 已记录并冻结 Redis version
- [ ] 发布期间禁止修改配置
- [ ] 新 Pod 使用 blocking 首拉取
- [ ] `/api/health` 正常后按既有策略继续滚动
- [ ] 滚动完成后核对所有 Pod 最终收敛为 `ok@冻结版本`
- [ ] 所有 Pod 验收完成后才解除配置冻结
