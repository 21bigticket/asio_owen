# 配置版本历史与回滚设计方案

**日期**：2026-08-18

**阶段**：Phase 4 已完成代码实现和本地回归（Clang 286/286、Phase 4 相关 ASan 54/54）；生产迁移、真实 Redis 故障演练和 Ubuntu GCC 11 门禁待执行

**依赖设计**：`docs/CONFIG_CENTER_DESIGN_2026-08-16.md`

**适用范围**：Redis 配置中心的版本历史、版本查看、版本比较、历史保留和一键回滚

## 1. 背景与结论

当前配置中心已经具备单调版本号、当前配置、乐观锁保存和简易 audit，但没有保存历史版本的配置内容。现状只能回答“当前是哪个版本、谁在什么时候提交过哪些文件名”，不能查看或恢复任意旧版本。

Phase 4 采用以下核心方案：

1. 每个成功发布的版本保存一份完整、不可变的配置快照。
2. `version` 只作为当前正式发布版本的单调指针，最后更新。
3. 回滚不降低版本号，而是把历史快照重新提交为一个新版本。
4. 历史按“最近 100 个版本或最近 90 天”保留，删除条件是同时超出两个范围。
5. 每个版本单独使用 Redis Hash，避免把全部历史堆积为一个大 key。
6. 对配置大小、文件数量和历史查询分页设置硬限制，防止 Lua 和 `HGETALL` 阻塞 Redis。
7. `config:files` 当前镜像永久维护，既兼容旧 Pod，也作为历史快照损坏时的受校验修复来源。
8. 任何已存在的历史版本都不自动覆盖；检测到 version 回退或历史高水位异常时冻结保存和 GC，优先保护正式历史。

## 2. 实施基线与现状

本节的“当前”数据模型描述的是 Phase 4 改造前的基线，用于说明迁移来源。Phase 4 代码已新增不可变 snapshot、meta、index、历史 API、单调回滚、人工修复和有界 GC；生产 Redis 在执行 §14 的迁移前仍可能处于本节所述旧形态。

### 2.1 当前 Redis 数据

| Key | 类型 | 当前作用 | 是否包含历史内容 |
|---|---|---|---|
| `asio_owen:config:version` | STRING | 当前版本号，每次保存递增 | 否 |
| `asio_owen:config:files` | HASH | 当前完整配置 | 否，保存时覆盖 |
| `asio_owen:config:audit` | LIST | 最近 200 次操作摘要 | 否 |
| `asio_owen:config:machines` | HASH | 各 Pod 同步状态 | 否 |

每个 Pod 的 `.config-sync-state` 只记录当前 `synced_version`、当前文件 hash、`last_ok` 和失败信息，也不是版本历史。

### 2.2 改造前 audit 语义

Phase 4 改造前 audit 实际记录：

```json
{
  "ts": 1787000000,
  "user": "admin",
  "base_version": 37,
  "files": ["20-upstream.ini", "30-security.ini"]
}
```

它存在以下限制：

- 不保存配置内容或 diff。
- 改造前代码没有写 `new_version`，与总设计文档 §4 的描述不一致。
- 使用 `pcall` 尽力写入，audit 失败不会阻断配置保存。
- `LTRIM 0 199` 只保留最近 200 条。
- 当前没有 audit 查询 API 或管理页面。

Phase 4 已将版本快照和版本元数据提升为核心提交的一部分；历史写入失败时不得发布新版本。兼容 audit 现在补充 `new_version`、`action` 和 `reason`，仍通过 `pcall` 尽力写入并保留最近 200 条，不能替代核心历史元数据。

## 3. 设计目标与非目标

### 3.1 目标

- 查询历史版本列表和提交信息。
- 查看任意保留版本的完整配置。
- 比较任意两个保留版本。
- 将历史版本恢复为一个新的单调版本。
- 历史数据与当前 version 保持可验证的一致关系。
- 保存、读取和清理操作资源有界，不对业务 Redis 造成明显阻塞。
- 兼容现有配置中心的 CAS、整目录 dry-run、保留路径和多 Pod 同步机制。

### 3.2 非目标

- 不支持把 Redis version 数字直接改小。
- 不实现按机器灰度回滚。
- 不把历史快照当作长期合规审计系统；强合规审计仍应进入独立数据库或不可变存储。
- 不保存 `[redis]`、`[admin]`、`[config_sync]`、`[config_history]` 和 `99-local.ini`，它们仍是 never-sync 本地配置。`[config_history]` 管辖同步读取器自身，不能由被同步和回滚的内容反向控制。
- 不在 Phase 4 中引入 Git、MySQL 或对象存储作为配置主存储。

## 4. Redis 数据模型

### 4.1 Key 设计

| Key | 类型 | 内容 | 保留策略 |
|---|---|---|---|
| `asio_owen:config:version` | STRING | 当前正式发布版本 | 永久 |
| `asio_owen:config:files` | HASH | 当前配置永久镜像和受校验修复来源 | 仅当前版本，永久维护 |
| `asio_owen:config:history:<version>` | HASH | field=文件名，value=完整 INI 内容 | 100 版或 90 天 |
| `asio_owen:config:history:meta` | HASH | field=版本，value=元数据 JSON | 与快照一致 |
| `asio_owen:config:history:index` | ZSET | score=版本，member=版本字符串 | 与快照一致 |
| `asio_owen:config:audit` | LIST | 旧审计兼容记录 | 200 条 |

示例：

```text
asio_owen:config:history:37
  00-server.ini      -> <完整内容>
  10-mysql.ini       -> <完整内容>
  20-upstream.ini    -> <完整内容>
  ...
```

每个版本是独立 Hash。100 个版本意味着约 100 个小 Hash，而不是一个持续增长的大 Hash。

### 4.2 版本元数据

`history:meta` 的 value 使用 JSON：

```json
{
  "version": 38,
  "base_version": 37,
  "ts": 1787000000,
  "user": "ops",
  "action": "save",
  "reason": "调整上游连接超时",
  "file_count": 12,
  "total_bytes": 7115,
  "content_sha256": "...",
  "rollback_from": null
}
```

字段定义：

| 字段 | 含义 |
|---|---|
| `version` | 本次生成的新版本 |
| `base_version` | 提交时客户端看到的当前版本，用于 CAS |
| `ts` | 服务端 Unix 时间戳 |
| `user` | 独立 admin token 中的账号 |
| `action` | `seed`、`save`、`rollback` 或 `migration` |
| `reason` | 操作者填写的变更原因 |
| `file_count` | 完整快照文件数量 |
| `total_bytes` | 全部文件内容字节数，不含 Redis 开销 |
| `content_sha256` | 规范化文件名和内容计算出的全集 SHA-256 |
| `rollback_from` | rollback 时的目标历史版本，否则为 null |

`reason` 对普通保存建议必填，对自动 seed/migration 由服务生成固定说明。

### 4.3 内容 hash 规范

为避免 map 遍历、平台换行和拼接歧义导致 hash 不一致，按以下确定性规则序列化输入；这里的“确定性序列化”不修改或规范化文件名、文件内容：

1. 文件按文件名字节升序排列。
2. 每项输入为：`name_length || name || content_length || content`。
3. 长度使用固定 64-bit 大端整数。
4. 对完整字节流计算 SHA-256。
5. 不改写文件内容，不自动转换 CRLF/LF。

该 hash 用于完整性核对和展示，不替代 Redis CAS。

### 4.4 Redis Cluster 说明

当前 key 没有 Redis Cluster hash tag，现有 EVAL 设计默认 key 位于同一 Redis 实例。若未来迁移 Redis Cluster，应整体迁移为同一 hash tag，例如：

```text
{asio_owen:config}:version
{asio_owen:config}:history:37
```

不能只修改新增 history key，否则跨 slot Lua 无法执行。Phase 4 本身不承担该 key 迁移。

## 5. 原子保存与发布

### 5.1 Admin 请求

保存请求扩展为：

```json
{
  "base_version": 37,
  "reason": "调整上游连接超时",
  "files": [
    {"name": "20-upstream.ini", "content": "..."}
  ]
}
```

请求中的 `files` 仍是完整托管文件集合，不是增量文件列表。

### 5.2 API 前置处理

1. 验证 admin token。
2. 解析 JSON 并检查专用容量限制。
3. 校验文件名、never-sync section 和 admin 保留路径。
4. 使用完整目录执行 dry-run。
5. 计算 `total_bytes` 和 `content_sha256`。
6. 构造可信版本元数据；`ts`、`user`、`action` 不接受客户端覆盖。
7. 调用 Lua 进行 CAS 和核心提交。

### 5.3 Lua 核心流程

Lua 脚本遵循“先准备、最后发布 version 指针”：

1. 校验 ARGV 数量、文件数量、单文件和总字节数。
2. 校验 version、当前 files、history snapshot、meta、index 和 staging key 类型。
3. 读取当前 version，非数字返回专用错误码。
4. 当前 version 与 `base_version` 不同则返回 CAS 冲突。
5. 读取 `history:index` 最大版本；若大于当前 version，返回 `HISTORY_INCONSISTENT` 并停止写入，防止手工回退 version 后覆盖正式历史。
6. 计算 `new_version = base_version + 1`。
7. 检查 `history:<new_version>`、对应 meta 和 index 是否已存在；任一存在都返回 `HISTORY_CONFLICT`，禁止自动删除或覆盖。
8. 在 history staging Hash 中构建完整快照。
9. `RENAME` 为不可变的 `history:<new_version>`。
10. 写入 `history:meta` 和 `history:index`。
11. 使用独立 current staging Hash 构建当前永久镜像，再 `RENAME` 到 `config:files`，继续保持现有整集合原子替换语义。
12. 最后沿用现实现的 `INCR config:version`，并校验返回值等于 `new_version`，作为唯一正式发布点。CAS 已证明当前值等于 base，因此与 `SET new_version` 语义等价，同时减少实现差异。
13. 兼容 audit 可在核心发布后通过 `pcall` 写入，不影响历史正确性。

所有可预见错误必须在写核心数据前检查。Redis Lua 出错不会自动回滚，因此 version 必须是最后一个核心写操作。version 未推进时产生的 history/meta/index 记录属于“疑似 orphan”，但系统不能仅凭 `history_version > current_version` 自动判定并删除，因为也可能是有人手工调小了 version。

### 5.4 快照不可变约束

- 已正式发布且仍在保留期内的 `history:<version>` 禁止覆盖。
- 同一个 `new_version` 已存在但当前 version 仍是 base 时，保存必须 fail-closed；不得在保存脚本中自动清理或重建。
- 历史查询只返回 `version <= current_version`、index/meta/snapshot 三者齐全的版本。
- 直接使用 redis-cli 修改历史 key 不属于支持的运维操作。
- 后台服务持续记录进程启动后的最大观测 version，并结合各 Pod `.config-sync-state` 和 machines 心跳检测回退；发现远端 version 小于任一可信已同步版本时，立即告警并冻结历史保存、回滚和 GC。
- `history:index` 最大版本大于当前 version 时统一进入 `history_inconsistent`，可能原因包括 Lua 中途失败和手工回退。系统只报告和隔离，不自动决定删除哪一侧；由管理员核对 AOF、日志、snapshot hash 和 Pod 状态后执行显式修复。

## 6. 同步读取

Phase 4 后，版本快照成为配置同步的权威读取源：

```text
v1 = GET asio_owen:config:version
files = HGETALL asio_owen:config:history:<v1>
v2 = GET asio_owen:config:version
v1 != v2 -> 放弃本次，下一周期重试
v1 == v2 -> 校验并应用 files
```

不可变快照保证读取 v37 时不会被 v38 的保存覆盖。`config:files` 永久维护，但只作为当前镜像和修复来源，不替代不可变快照的权威地位。

### 6.1 当前版本快照缺失

`history:<current_version>` 因误删、GC bug 或不完整恢复而缺失时，Pod 行为必须明确：

1. 不得把空 HGETALL 当作合法空配置，也不得继续上报 `ok`。
2. 读取 `config:files` 当前镜像和 `history:meta[current_version]`。
3. 按 §4.3 重新计算镜像全集 SHA-256。
4. hash 与 meta 的 `content_sha256` 完全一致时，可以把镜像作为临时降级内容应用，但状态和心跳必须保持 `partial`，detail=`history_snapshot_missing_fallback`；不得静默标记成功。
5. meta 缺失或 hash 不一致时，不应用镜像：已有 Pod 保留最后一个本地好版本，新 Pod 的 blocking first pull 失败且不能通过 readiness。
6. 降级应用时本地 state 记录 `synced_version=current_version,status=partial`，下一轮即使版本相等也继续健康复核。
7. 由 Admin 修复操作把已验证镜像重建为同版本历史快照；操作前执行 §5.3 同款高水位检查，并且只允许目标 snapshot 确实不存在、镜像 hash 等于 meta，不能覆盖已有快照。修复完成后 Pod 才恢复 `ok`。

只有本地 `state.status=partial` 或处于降级恢复流程的 Pod，才在同版本轮询中持续复核当前历史快照；`status=ok` 的 Pod 保持现有轻量路径，只执行 `GET version`，版本变化时才读取快照。partial Pod 不能只因 `remote_version == synced_version` 就恢复 `ok`。

### 6.2 当前镜像维护约束

- `config:files` 不设置停写阶段，所有 Phase 4 保存和回滚永久同步更新它；当前约 7 KiB 的额外成本可以忽略。
- 这保证滚动期间残留的旧 Pod 仍能读取与新 version 对应的当前 files，不会把陈旧镜像错误标成新版本。
- 全部 Pod 升级完成后可以取消“旧读取路径”，但不能取消镜像写入和 hash 监控。
- history snapshot 与当前镜像的 hash 不一致时，版本保存返回失败或运行期告警，不能自动选择其中一份覆盖另一份。
- 若当前版本 snapshot/meta/index 三元组完整，且重新计算的 snapshot hash 等于 meta，但镜像缺失或 hash 不一致，Admin 可执行 `mirror-rebuild`：先执行高水位检查，再从 `history:<current_version>` 构建独立 staging Hash，最后 `RENAME` 到 `config:files`。该操作不修改 version、history 或 meta，并记录高优先级审计。

读取模式通过 never-sync 本地配置显式控制，不能仅凭 snapshot 是否存在自动猜测，也不能由历史回滚改变：

```ini
[config_history]
read_mode = required
auto_migrate_legacy = true
```

| 模式 | 使用阶段 | snapshot 缺失行为 |
|---|---|---|
| `compat` | 关闭自动迁移后的特殊人工迁移窗口 | 允许读取旧 `config:files`，记录 migration warning；不用于正常长期运行 |
| `required` | 回填完成后的稳态 | 只允许 §6.1 的 meta hash 校验降级并上报 partial；无法校验则拒绝应用 |

`[config_history]` 必须加入 section 和文件级 never-sync 校验，保存 API、历史快照和回滚结果都不得包含它。`read_mode` 非法或缺失时始终采用代码安全默认值 `required`。`auto_migrate_legacy` 默认开启，但只识别 history index、meta 和当前 snapshot 全空且旧镜像非空的纯 legacy 状态；Lua 在写入前再次核对全空条件，迁移只新增 snapshot/meta/index/audit，不修改旧 version/files。任何 history 痕迹或并发冲突都 fail-closed，不能自动覆盖。

## 7. 回滚设计

### 7.1 基本语义

禁止执行：

```text
SET asio_owen:config:version 32
```

Pod 会把远端小于本地的版本视为回退攻击或数据损坏并拒绝应用。

正确回滚流程：

```text
当前 v37
-> 读取历史 v32 完整快照
-> 重新执行全部校验和 dry-run
-> 以 base_version=37 提交
-> 生成 v38，内容等于 v32
```

v38 元数据记录：

```json
{
  "version": 38,
  "base_version": 37,
  "action": "rollback",
  "rollback_from": 32,
  "reason": "恢复错误的上游配置"
}
```

### 7.2 回滚约束

- 目标版本必须仍在保留期内且 snapshot/meta/index 完整。
- 回滚前必须重新执行当前代码的校验，旧版本不能绕过新增安全规则。
- 回滚也使用 CAS；期间已有其他人保存配置则返回 409。
- 回滚不恢复 never-sync 本地文件。
- 回滚可能涉及 `[server]`、`[mysql]` 等启动期配置，API 必须返回 restart-required 提示。
- 若某文件在目标历史版本中属于托管文件、但当前规则已把它调整为 never-sync，回滚返回 409，并列出全部冲突文件名；不得静默跳过后生成不完整版本。
- 回滚提交 Lua 必须再次检查目标 snapshot/meta/index 存在且 hash 未变。若目标在“读取 -> dry-run -> 提交”期间被 GC 删除，返回 409 提示刷新重试，不使用内存中的旧副本继续提交。
- 回滚确认页高亮可能含凭证的 section/key，例如 `[mysql]`、`pass`、`password`、`secret`、`token`、`private_key`。恢复旧凭证可能让已经轮换的账号再次生效，必须二次确认并写入高优先级审计。
- 可进一步为回滚配置独立的 `config:rollback` 权限；未实施细粒度权限前，仅 admin 可回滚且必须填写 reason。

## 8. Admin API 与页面

### 8.1 API

| API | 方法 | 说明 |
|---|---|---|
| `/api/admin/config/history?before=38&limit=20` | GET | 倒序分页返回元数据，不返回配置内容 |
| `/api/admin/config/history/{version}` | GET | 返回指定版本完整快照和元数据 |
| `/api/admin/config/history/{version}/diff?to=38` | GET | 返回 from=路径版本、to=查询参数版本的新增、删除、修改及文本 diff；to 缺省为当前版本 |
| `/api/admin/config/rollback` | POST | 把目标历史版本重新提交为新版本 |
| `/api/admin/config/history/migrate` | POST | compat 冻结窗口把当前镜像回填为同版本首份历史 |
| `/api/admin/config/history/repair-snapshot` | POST | 用 hash 匹配的当前镜像重建缺失的当前 snapshot |
| `/api/admin/config/history/rebuild-mirror` | POST | 用完整且 hash 正确的当前 snapshot 原子重建镜像 |
| `/api/admin/config/history/resolve-orphan` | POST | 高危人工处置：恢复 version 指针或删除确认未发布的 current+1 orphan |

分页约束：

- 默认 `limit=20`。
- 最大 `limit=50`。
- 使用 version 游标，不使用 offset 扫描大量历史。
- 历史列表只读 meta/index，不执行 `KEYS history:*`。
- 列表通过一次只读 Lua 批量执行 ZSET 分页、HMGET meta 和 snapshot EXISTS，返回扁平结果；不能为 20 条记录产生 20 次 Redis 往返。
- diff 方向固定为 `from -> to`；新增表示只存在于 to，删除表示只存在于 from。

回滚请求：

```json
{
  "base_version": 38,
  "target_version": 32,
  "reason": "回滚错误配置"
}
```

### 8.2 页面

管理页增加 History 视图：

- 版本、时间、操作者、action、原因、文件数和总字节数。
- 当前版本明确标识。
- 选择两个版本比较。
- 查看完整文件内容。
- 回滚前展示目标版本、当前版本、diff 和 restart-required 文件。
- 回滚需要二次确认并填写原因。

## 9. 容量限制

### 9.1 建议硬限制

| 项目 | 告警阈值 | 拒绝阈值 |
|---|---:|---:|
| 单版本全部配置原始内容 | 256 KiB | 512 KiB |
| 单个 INI 文件 | 64 KiB | 128 KiB |
| 托管文件数量 | 80 | 100 |
| `reason` UTF-8 字节数 | 400 B | 512 B |
| 历史列表分页 | - | 50 条 |
| diff 响应大小 | 1 MiB | 2 MiB |

当前通用 HTTP body 上限为 10 MiB，这对配置中心过大。Phase 4 必须在 Admin API 和 Lua 内增加独立限制，不能只依赖 HTTP body 上限。

### 9.2 为什么使用完整快照

配置修改低频、当前体量很小。完整快照具有以下优势：

- 单版本可独立读取和校验。
- 回滚不需要从长 diff 链重放。
- 删除文件天然表达为新快照中不存在。
- 历史损坏只影响单版本，不会让后续全部版本无法重建。
- 实现和故障恢复复杂度低于增量链。

## 10. 内存与性能评估

### 10.1 当前仓库基线

2026-08-18 对 `config.d/*.ini` 的静态统计：

| 项目 | 数值 |
|---|---:|
| 全部 15 个 INI 文件 | 9,582 B |
| never-sync：`11-redis.ini` | 473 B |
| never-sync：`12-config-sync.ini` | 1,587 B |
| never-sync：`99-local.ini` | 407 B |
| 12 个 Redis 托管文件 | **7,115 B** |
| 全部 INI 行数 | 244 行 |

因此当前单个历史快照的原始 value 总量约为 7 KiB。

### 10.2 100 个版本的内存估算

仅计算文件原文：

```text
7,115 B/版本 × 100 版本 = 711,500 B，约 695 KiB
```

Redis 还会为 key、Hash table、field SDS、value SDS 和 allocator 对齐分配内存。具体倍数取决于 Redis 版本、编码阈值和文件长度，不能用原文大小代替 `MEMORY USAGE`。按原始内容的 1.5～3 倍做容量预算：

| 内容 | 估算内存 |
|---|---:|
| 100 个历史快照 | 1.0～2.1 MiB |
| meta/index/audit | 通常小于 0.2 MiB |
| 当前 files + 保存 staging | 通常小于 0.1 MiB |
| 当前规模合计预算 | **约 1.3～2.5 MiB** |

这是容量规划估算，不是实测值。上线后必须用 `MEMORY USAGE` 校准。

### 10.3 上限场景估算

若每版本达到 512 KiB 硬上限并保留 100 版：

```text
512 KiB × 100 = 50 MiB 原始内容
```

按 1.5～3 倍 Redis 内存放大估算：

| 内容 | 估算内存 |
|---|---:|
| 100 个满上限快照 | 75～150 MiB |
| 当前镜像、staging、meta/index | 约 2～5 MiB 峰值增量 |
| 总容量规划 | **约 80～160 MiB** |

因此 `512 KiB` 是异常增长保护上限，不是日常目标。单版本达到 256 KiB 时就应告警并检查是否把不适合的内容放入 INI。

### 10.4 AOF、复制和备份放大

每次保存需要写一份历史快照，并永久更新一份当前 files 镜像，写入字节量约为 `2 × snapshot_size`，另加命令、key 和元数据开销。

当前 7 KiB 基线下：

```text
约 14 KiB/次配置保存
100 次保存约 1.4 MiB 原始写入量
```

满 512 KiB 上限时：

```text
约 1 MiB/次配置保存
100 次保存约 100 MiB 原始写入量
```

AOF、主从复制和备份会承载这些写入。由于配置保存低频，当前规模影响很小；若配置修改变为高频或单版本长期超过 256 KiB，应评估独立 Redis 实例或外部历史存储。

### 10.5 命令复杂度

设 `F` 为文件数，`S` 为单版本总字节数，`N` 为保留版本数：

| 操作 | 命令 | 复杂度/数据量 |
|---|---|---|
| 常规轮询 | `GET version` | O(1)，不读历史 |
| 版本变化同步 | `HGETALL history:<v>` | O(F + S) |
| 查看单版本 | `HGETALL history:<v>` | O(F + S) |
| 比较两版本 | 两次 HGETALL + 应用层 diff | O(F + S1 + S2) |
| 历史列表 | `ZREVRANGEBYSCORE` + `HMGET meta` | O(log N + page_size) |
| 保存 | Lua + 完整快照 HSET | O(F + S) |
| 清理 | ZSET/HASH 删除 + `UNLINK snapshot` | 每批有界 |

当前基线 `F=12`、`S=7,115 B`，一次 HGETALL 或保存处理的数据很小。轮询每 5 秒只执行 GET；只有 version 改变才读取快照，所以历史版本数量不会增加常规同步开销。

### 10.6 延迟与慢查询风险

Redis Lua 在执行期间会阻塞同实例其他命令。当前约 7 KiB 的保存脚本通常不会构成慢查询，但不能在没有线上基准数据时承诺固定毫秒数。

建议运行指标：

- 配置保存 Lua p95 小于 10 ms。
- 单次超过 20 ms 告警。
- history HGETALL p95 小于 5 ms。
- `cmd_timeout_ms=500` 仍作为网络/命令故障上限，不能用提高超时掩盖 Lua 变慢。
- 若 Redis 与高流量业务共享实例，DB 隔离只隔离 keyspace，不隔离 CPU；持续慢 Lua 时需要独立实例。

### 10.7 大 key 判定

本设计不会把 100 版内容放入一个 key。当前每个快照约 7 KiB，不属于有实际风险的大 key。

建议监控：

- 单快照 `MEMORY USAGE` 超过 512 KiB 告警。
- 单快照原始 `total_bytes` 超过 256 KiB 告警。
- 原始内容超过 512 KiB 在写入前拒绝。
- 禁止通过 `KEYS asio_owen:config:history:*` 枚举历史，统一走 index。

## 11. 历史清理

### 11.1 保留规则

默认同时配置：

```text
retention_versions = 100
retention_days = 90
```

一个版本只有在以下两个条件同时满足时才可删除：

1. 不在最近 100 个版本内；
2. 创建时间早于 90 天。

当前正式版本永不删除。这样既能保留发布密集期的最近 100 版，也能在发布稀疏期保留至少 90 天。

### 11.2 清理实现

清理由独立低频后台任务调度，不放入保存核心 Lua。每个候选版本的三元组删除使用一个短 Lua 原子执行：

1. 从 `history:index` 分页取得候选版本。
2. 读取 meta 时间并应用双条件判断。
3. 每批最多清理 10～20 个版本。
4. 删除 Lua 再次读取当前 version，确认候选不是当前版本且仍满足调用方传入的版本/时间边界。
5. 在同一 Lua 中执行 `UNLINK history:<version>`、`HDEL history:meta <version>` 和 `ZREM history:index <version>`。
6. 每批完成后让出执行权，避免连续占用 Redis。

禁止使用 `KEYS` 扫描，避免在大 keyspace 中阻塞 Redis。即使当前快照很小，也统一使用 `UNLINK`，为将来容量增长保留安全边界。

### 11.3 疑似 Orphan 与 version 回退

Lua 因 OOM 可能留下 version 未发布但 snapshot/meta/index 已存在的记录；手工调小 version 也会产生完全相同的外观。因此后台任务不得仅按“历史版本大于当前 version 且超过 1 小时”自动删除。

处理规则：

1. `max(history:index) > current_version` 时进入 `history_inconsistent`。
2. 暂停新配置保存、回滚和全部历史 GC，防止覆盖或删除正式历史。
3. 汇总进程最大观测 version、machines 心跳、各 Pod 本地 state、AOF/日志和目标 snapshot hash。
4. 管理员显式选择“恢复 version 指针”或“删除确认未发布的 orphan”。
5. 修复操作走专用 Admin API/Lua 并记录审计，不提供自动超时删除。
6. 当前 snapshot 和 meta 同时缺失时，不得用无法独立验真的镜像自动重建。管理员只能结合 AOF/RDB/离线备份、可信 Pod 的 last-good 文件及审计日志确定内容，执行显式灾难恢复；恢复期间继续冻结保存、回滚和 GC，并在重建 snapshot、meta、index、镜像后重新校验全集 hash。

专用处置 API 为 `POST /api/admin/config/history/resolve-orphan`，必须填写 `current_version`、`target_version`、`action`、`reason` 和动作对应的强确认文本：

- `action=restore-version`：确认文本为 `RESTORE_VERSION_POINTER`。目标必须是 index 最高版本，snapshot/meta/index 三元组完整且逐文件内容、全集 hash 均复核通过；目标不得低于 machines 心跳或本进程最大观测版本。Lua 先从目标 snapshot 原子重建当前镜像，最后 `SET version=target_version`。
- `action=delete-orphan`：确认文本为 `DELETE_UNPUBLISHED_ORPHAN`。只允许删除 `target_version=current_version+1`，且 machines 心跳和本进程都没有观察到高于 current 的版本；Lua 再次精确核对 snapshot/meta/index 的存在状态和全部文件内容后原子三删。任何已发布证据存在时拒绝删除。
- 两种动作都写高优先级日志和兼容 audit；成功后立即触发健康复核，不等待下一次 300 秒周期。若删除后仍有更高异常记录，`history_consistent=false` 且继续冻结，必须逐项核查。

这种 fail-closed 会让极少见的 OOM 残留需要人工介入，但能避免手工回退 version 后把已经正式发布的历史当作 orphan 覆盖。

## 12. 安全与备份

历史快照可能包含 MySQL 密码、JWT 业务配置和内部地址，保留历史会延长旧凭证的存活时间。

要求：

- Redis 只允许受控网络访问并启用 ACL。
- Redis 连接优先使用 TLS；无法使用时必须依赖可信内网和网络策略。
- AOF、RDB、主从节点和离线备份按敏感数据管理并加密。
- 历史 API 继续使用独立 admin token，不复用业务 JWT。
- 日志只记录 version、用户、reason、文件名和 hash，不打印完整文件内容。
- 凭证轮换后，旧凭证仍存在于保留期历史中；高敏环境需要更短保留期或字段级脱敏/外部加密存储。
- 回滚可能重新激活历史中的旧凭证，不只是读取泄露风险。含敏感 section/key 的版本必须在 UI 高亮，并通过额外确认和审计。
- Redis 历史不等同于灾备。需要恢复 Redis 整体故障时，仍依赖 AOF/RDB 和备份恢复演练。

## 13. 监控与运维

### 13.1 Redis 检查

```bash
redis-cli MEMORY USAGE asio_owen:config:history:37
redis-cli ZCARD asio_owen:config:history:index
redis-cli SLOWLOG GET 20
redis-cli LATENCY DOCTOR
```

不建议在生产高峰执行全库 `--bigkeys` 扫描；应由应用根据 index 对历史 key 做有界采样。

### 13.2 应用指标

建议增加：

- `config_history_versions_total`
- `config_history_bytes_total`
- `config_history_snapshot_bytes_current`
- `config_history_save_duration_ms`
- `config_history_read_duration_ms`
- `config_history_gc_deleted_total`
- `config_history_gc_failures_total`
- `config_history_orphans_total`
- `config_history_inconsistent`
- `config_history_snapshot_fallback_total`
- `config_rollback_total`
- `config_rollback_failures_total`

建议告警：

- 历史数量或内存持续超过保留策略。
- 保存 Lua 超过 20 ms。
- 出现 orphan 或 index/meta/snapshot 不一致。
- GC 连续失败。
- 当前 version 对应历史快照不存在。
- 远端 version 小于进程或任一可信 Pod 已观测版本。
- `max(history:index) > current_version`，保存和 GC 已被冻结。
- 单版本原始内容超过 256 KiB。

## 14. 迁移与发布步骤

Phase 4 自身按以下顺序上线：

1. 冻结配置修改，只启动一个新 Pod。
2. 使用默认 `read_mode=required, auto_migrate_legacy=true`。后台仅在纯 legacy 状态下自动复制当前 files，并用一次性 CAS/Lua 生成同版本历史，meta action=`migration`；version/files 保持不变。
3. 验证 snapshot、meta、index、镜像 hash 和 version 一致，再验证 Admin 配置读取和当前版本历史详情。
4. 确认首 Pod 配置同步状态和业务探针正常。
5. 启用新版保存、历史 API 和回滚 API并完成单 Pod 验收。
6. 解除配置冻结后才能扩容。
7. 自动迁移拒绝时保持冻结，按 §11.3 区分已有历史损坏、orphan 和 key 类型错误；不得把异常状态删除后伪装成首次迁移。

配置冻结是迁移成立的关键。滚动期间若旧 Admin writer 仍可保存，新版本可能只更新 current files 而不生成历史快照。

## 15. 测试与验收

### 15.1 单元测试

- 正常保存生成 snapshot/meta/index 和新 version。
- CAS 冲突不生成正式历史版本。
- 非数字 version、错误 key 类型和超限内容全部拒绝。
- version 是最后发布点；未发布 orphan 不被历史 API 返回。
- 历史快照不可覆盖。
- seed 和 migration 正确生成第一份历史。
- rollback v32 在当前 v37 上生成 v38，而不是降低 version。
- rollback 重新执行保留路径校验和 dry-run。
- 目标文件已演进为 never-sync 时 rollback 返回 409 和冲突文件名。
- version 被手工调小时，不覆盖现有 history，保存和 GC fail-closed。
- 完整高水位 orphan 仅可通过强确认恢复 version 指针；只有 snapshot/meta/index 部分残留的 current+1 orphan 可在无已发布证据时强确认删除。
- 当前 snapshot 缺失时，镜像 hash 匹配只能 partial 降级；hash 不匹配拒绝应用。
- `read_mode` 缺失或非法时默认为 required，保存和回滚均不能写入 never-sync 的 `[config_history]`。
- `status=ok` 的同版本轮询只读 version；仅 partial/降级状态持续复核 snapshot。
- 当前 snapshot/meta/index 完整且 hash 正确时，`mirror-rebuild` 原子重建镜像且不修改 version/history/meta。
- 分页游标、最大 limit 和不存在版本处理正确。
- 历史列表一次 Redis 调用批量验证 snapshot/meta/index 三元组。
- GC 同时遵守 100 版本和 90 天条件。
- GC 三元组删除原子、不删除当前版本，并使用有界批次。

### 15.2 集成测试

- 多 Pod 同步同一个不可变历史快照。
- 保存期间执行并发 GET，不产生跨版本文件集合。
- 保存失败留下疑似 orphan 时进入 `history_inconsistent`；显式修复后才能重试发布。
- Redis 重启/AOF恢复后 version、snapshot、meta、index 一致。
- 管理页查看历史、比较、回滚后所有 Pod 收敛到新版本。
- rollback 读取目标后与 GC 并发删除，提交时返回 409，不生成新版本。
- snapshot 被误删后现有 Pod 保留最后好版本、新 Pod readiness 失败；校验通过的当前镜像仅以 partial 降级。
- 镜像在 version 发布前超前或损坏时，检测到 hash 不一致；`mirror-rebuild` 后恢复与当前历史快照一致。
- 当前 snapshot 和 meta 同时缺失时保持冻结，不允许仅凭镜像自动修复。
- 512 KiB 边界压测并记录 Lua/HGETALL 延迟和 Redis slowlog。

### 15.3 验收标准

- 当前约 7 KiB 配置下，保存和历史读取不进入 Redis slowlog。
- 100 个当前规模版本的 `MEMORY USAGE` 实测总量与 1.3～2.5 MiB 规划同量级；若偏差明显，按实测修正容量预算。
- 超过 512 KiB 的配置在进入 Redis Lua 前被 API 拒绝，Lua 也有第二道拒绝。
- rollback 始终生成新版本，所有 Pod 正常收敛。
- 历史清理不使用 `KEYS` 和同步大 key `DEL`。

### 15.4 Ubuntu GCC 11 兼容性门禁

项目语言标准保持 C++20；这里的“兼容 g++11”指 Ubuntu 上的 GCC/G++ 11.x 和对应 libstdc++ 11，不是把语言标准降为 C++11。

- Phase 4 新增代码必须在 Ubuntu 目标镜像中使用 `CC=gcc-11 CXX=g++-11` 配置、完整编译并运行测试，不能只以 macOS Clang 或更新版本 GCC 的结果作为验收依据。
- 只使用已在 libstdc++ 11 实编译验证的 C++20 标准库能力。不得直接引入依赖较新实现的 `std::format`、C++23 API 或未验证的 chrono/ranges/atomic 扩展；格式化沿用项目已有 spdlog/fmt 能力或现有兼容写法。
- 不引入仅支持新 GCC ABI、仅在 macOS 存在的系统调用或依赖隐式传递链接的实现；Linux 下的 MySQL、hiredis、OpenSSL 和 pthread 链接必须由 CMake target 明确表达。
- 合并门禁至少执行：

```bash
CC=gcc-11 CXX=g++-11 cmake -B build-gcc11 -S .
cmake --build build-gcc11
ctest --test-dir build-gcc11 --output-on-failure
```

- 若开发环境没有 GCC 11，可先做本地验证，但在 Ubuntu GCC 11 门禁通过前不得判定 Phase 4 实现完成。

## 16. 待确认参数

实施前需要最终确认以下默认值；本方案推荐值如下：

| 参数 | 推荐值 |
|---|---:|
| `retention_versions` | 100 |
| `retention_days` | 90 |
| `read_mode`（never-sync 本地配置） | required |
| `auto_migrate_legacy`（never-sync 本地配置） | true |
| `warn_snapshot_bytes` | 256 KiB |
| `max_snapshot_bytes` | 512 KiB |
| `warn_file_bytes` | 64 KiB |
| `max_file_bytes` | 128 KiB |
| `warn_files` | 80 |
| `max_files` | 100 |
| `max_reason_bytes` | 512 B |
| `history_page_size` | 20 |
| `history_page_size_max` | 50 |
| `max_diff_response_bytes` | 2 MiB |
| `gc_batch_size` | 20 |

`read_mode` 和 `auto_migrate_legacy` 必须放在 never-sync 本地配置，其余参数可按职责放入本地或托管配置；所有容量参数都应设置代码硬上限，管理页面不能自行放宽。
