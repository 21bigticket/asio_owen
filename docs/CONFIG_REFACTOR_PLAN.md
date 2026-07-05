# config.ini 配置重组方案

## 现状问题

当前 `config.ini` 所有配置堆积在一起，没有清晰的分组：

- 安全和限流混在一起
- 上游路由和连接池混在一起
- 没有标注哪些需要重启、哪些可以热加载
- 随着功能增加（插件、日志、追踪等），会越来越乱

## 重组原则

1. **按功能域分组** — 每个功能区独立一个区块，互不干扰
2. **标注生效方式** — 每个区块头注明"重启生效"或"SIGHUP 热加载"
3. **改动频率分离** — 经常改的（上游路由、黑名单、限流）放在显眼位置
4. **安全隔离** — 安全相关配置集中但内部按热加载/重启再拆分
5. **预留扩展** — 为新功能留空白区域

## 最终分组方案（10 个区块）

```
1. 基础服务      — 端口/日志/进程         重启生效      改动极少
2. 数据库        — MySQL/Redis 连接池     重启生效      改动极少
3. 上游路由      — zebra-* 服务地址       待实现热加载  经常改
4. 上游连接池    — 超时/并发/大小限制     重启生效      改动极少
5. 安全 JWT      — 密钥/算法/签发者       重启生效      偶尔改（密钥轮换）
6. 信任代理      — XFF 信任 IP            SIGHUP 热加载  偶尔改
7. IP 黑名单     — 封禁 IP/CIDR            SIGHUP 热加载  经常改
8. 免鉴权白名单  — 跳过 JWT 的路径        SIGHUP 热加载  经常改
9. 路由黑名单    — 禁止路径+角色要求      SIGHUP 热加载  偶尔改
10. 限流         — IP/全局/路径限流        SIGHUP 热加载  经常改
```

## 区块格式规范

每个区块使用以下模板：

```ini
# ============================================================================
# N. 区块名称 — 功能描述（生效方式）
# ============================================================================
[section_name]
# 字段说明
key = value
```

### 标题栏规则

- `=` 号分隔线长度：76 个
- 编号从 1 开始递增
- 生效方式二选一：`重启生效` / `SIGHUP 热加载`

## 生效方式决策树

```
改动这个配置需要重启吗？
├── 是（重启生效）
│   ├── 端口 / 日志级别 / 线程数 → [server]
│   ├── 数据库连接串 / 池大小    → [mysql] [redis]
│   ├── 上游地址 / 路由          → [upstream]
│   ├── 超时 / 并发限制          → [http_pool]
│   ├── JWT 密钥 / 算法          → [security]
│   └── 限流阈值 / 桶大小        → [rate_limit]
│
└── 否（SIGHUP 热加载）
    ├── 信任代理 IP               → [trusted_proxies]
    ├── IP 黑名单                 → [ip_blacklist]
    ├── 免鉴权路径                → [auth_whitelist]
    └── 路由黑名单                → [path_blacklist]
```

## 独立文件方案（已实施）

配置已拆分为独立文件，按功能域分散管理。

### 目录结构

```
config.d/
├── 00-server.ini          # 基础服务（重启生效）
├── 10-mysql.ini           # MySQL 连接池（重启生效）
├── 11-redis.ini           # Redis 连接池（重启生效）
├── 20-upstream.ini        # 上游路由（热加载）
├── 21-http_pool.ini       # 上游连接池（重启生效）
├── 30-security.ini        # JWT 密钥/算法（重启生效）
├── 31-trusted_proxies.ini # 信任代理 IP（热加载）
├── 32-ip_blacklist.ini    # IP 黑名单（热加载）
├── 33-auth_whitelist.ini  # 免鉴权白名单（热加载）
├── 34-path_blacklist.ini  # 路由黑名单（热加载）
├── 40-rate_limit.ini      # 限流（热加载）
└── 99-local.ini           # 本地覆盖（.gitignore，不提交）
```

### 文件名规范

- 两位数字前缀：`00-` 基础、`10-` 数据库、`20-` 网关、`30-` 安全、`40-` 限流
- 短横线 + 英文名称：`server`、`mysql`、`upstream`
- 后缀 `.ini`
- `99-local.ini` 保留给本地配置覆盖，已被 `.gitignore` 排除

### 加载方式（已实现）

`Config::load("config.ini")` 自动做两件事：

```cpp
// 1. 加载 config.ini（已清空，只留注释，未来可删除）
load_file("config.ini");

// 2. 自动发现 config.d/ 目录，按文件名排序加载所有 *.ini
//    后加载的文件覆盖先加载的同名 section.key
load_file("config.d/00-server.ini");
load_file("config.d/10-mysql.ini");
load_file("config.d/11-redis.ini");
...
load_file("config.d/99-local.ini");  // 如果存在的话
```

**覆盖规则：** 后加载的文件覆盖先加载的同名 key。`99-local.ini` 数字最大，优先级最高。

### 热加载适配

热加载定时器目前只调用了 `g_security_rules->reload(new_cfg)`，需要扩展为：

```cpp
Config new_cfg;
if (new_cfg.load("config.ini")) {
    // 限流热加载（update_config 已在 load_from_config 中调用）
    if (g_security_rules) g_security_rules->reload(new_cfg);

    // 上游路由热加载（需要新增 UpstreamManager::reload）
    if (g_server) g_server->upstreams().reload(new_cfg);
}
```

### 需要修改的代码

| 文件 | 改动 |
|:-----|:------|
| `src/common/config.hpp` | `load()` 新增 `load_file()` + 自动扫描 `config.d/` ✅ 已完成 |
| `src/http/upstream_manager.hpp` | 新增 `reload(const Config&)` 方法 |
| `src/main.cpp` | reload 回调中新增 `g_server->upstreams().reload(new_cfg)` |

### 不需要改的代码

- `security_rules.hpp` — `reload` → `load_from_config` → `update_config`（限流）已经正常工作
- `http_pool.hpp` — 连接池配置重启生效，不热加载
- `rate_limiter.hpp` — `update_config` 已被 `load_from_config` 调用 ✅
- `IP 黑名单 / 白名单 / 信任代理` — 在 `load_from_config` 中已被 reload 覆盖 ✅
