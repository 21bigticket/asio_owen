# config.ini 配置重组方案

## 现状问题

当前 `config.ini` 所有配置堆积在一起，没有清晰的分组：

- 安全和限流混在一起
- 上游路由和连接池混在一起
- 没有标注哪些需要重启、哪些可以热加载
- 随着功能增加（插件、日志、追踪等），会越来越乱

## 重组原则

1. **按功能域分组** — 每个功能区独立一个区块，互不干扰
2. **标注生效方式** — 每个区块头注明"重启生效"或"定时器热加载"

## 最终分组方案（10 个区块）

```
1. 基础服务      — 端口/日志/进程         重启生效      改动极少
2. 数据库        — MySQL/Redis 连接池     重启生效      改动极少
3. 上游路由      — zebra-* 服务地址       定时器热加载  经常改
4. 上游连接池    — 超时/并发/大小限制     重启生效      改动极少
5. 安全 JWT      — 密钥/算法/签发者       定时器热加载  偶尔改（密钥轮换）
6. 信任代理      — XFF 信任 IP            定时器热加载  偶尔改
7. IP 黑名单     — 封禁 IP/CIDR            定时器热加载  经常改
8. 免鉴权白名单  — 跳过 JWT 的路径        定时器热加载  经常改
9. 路由黑名单    — 禁止路径+角色要求      定时器热加载  偶尔改
10. 限流         — IP/全局/路径限流        定时器热加载  经常改
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
- 生效方式：`重启生效` / `定时器热加载（30s）`

## 生效方式

```
改动这个配置需要重启吗？
├── 是（重启生效）
│   ├── 端口 / 日志级别           → [server]
│   ├── 数据库连接串 / 池大小    → [mysql] [redis]
│   ├── 上游连接池超时/并发       → [http_pool]
│
└── 否（定时器热加载（30s））
    ├── 上游路由                  → [upstream]
    ├── JWT 密钥/算法/公钥        → [security]  ⚠ 密钥轮换建议维护窗口期重启以确保一致性
    ├── 限流阈值 / 桶大小         → [rate_limit]
    ├── 信任代理 IP               → [trusted_proxies]
    ├── IP 黑名单                 → [ip_blacklist]
    ├── 免鉴权路径                → [auth_whitelist]
    └── 路由黑名单                → [path_blacklist]
```

> 热加载机制：server 每 30s（`config_reload_interval_sec`）轮询一次 config.d/ 目录，
> 重新加载全部配置。不是 SIGHUP 信号触发，不需要手动操作。
> 定时器间隔本身也支持热加载：每次 reload 都会读取最新的 `config_reload_interval_sec` 并重新设定定时器。
> 
> 热加载生效范围：`[upstream]` `[security]` `[rate_limit]` `[trusted_proxies]`
> `[ip_blacklist]` `[auth_whitelist]` `[path_blacklist]`。
> 其余区块（`[server]` `[mysql]` `[redis]` `[http_pool]`）仍需重启。

## 独立文件方案（已实施）

配置已拆分为独立文件，按功能域分散管理。

### 目录结构

```
config.d/
├── 00-server.ini          # 基础服务（重启生效）
├── 10-mysql.ini           # MySQL 连接池（重启生效）
├── 11-redis.ini           # Redis 连接池（重启生效）
├── 20-upstream.ini        # 上游路由（定时器热加载（30s））
├── 21-http_pool.ini       # 上游连接池（重启生效）
├── 30-security.ini        # JWT 密钥/算法（定时器热加载（30s））
├── 31-trusted_proxies.ini # 信任代理 IP（定时器热加载（30s））
├── 32-ip_blacklist.ini    # IP 黑名单（定时器热加载（30s））
├── 33-auth_whitelist.ini  # 免鉴权白名单（定时器热加载（30s））
├── 34-path_blacklist.ini  # 路由黑名单（定时器热加载（30s））
├── 40-rate_limit.ini      # 限流（定时器热加载（30s））
└── 99-local.ini           # 本地覆盖（.gitignore，不提交）
```

### 文件名规范

- 两位数字前缀：`00-` 基础、`10-` 数据库、`20-` 网关、`30-` 安全、`40-` 限流
- 短横线 + 英文名称：`server`、`mysql`、`upstream`
- 后缀 `.ini`
- `99-local.ini` 保留给本地配置覆盖，已被 `.gitignore` 排除

### 加载方式（已实现）

`Config::load(".")` 从当前目录下的 `config.d/` 自动发现所有 `*.ini` 文件，按文件名排序加载。`config.ini` 文件本身**已废弃**（纯占位，不加载任何配置）。

实际行为：自动发现 `config.d/` 目录，按文件名排序加载所有 `*.ini`，后加载覆盖先加载：

```cpp
load_file("config.d/00-server.ini");
load_file("config.d/10-mysql.ini");
...
load_file("config.d/99-local.ini");  // 如果存在，优先级最高
```

**覆盖规则：** 后加载的文件覆盖先加载的同名 key。`99-local.ini` 数字最大，优先级最高。

> ⚠️ `config.ini` 不参与加载。不要把配置写进 `config.ini`，会被静默忽略。

### 热加载实现

server 启动时创建 30s 定时器（`config_reload_interval_sec`），到期后：

```cpp
Config new_cfg;
	if (new_cfg.load(".")) {
    // 安全模块热加载（IP黑名单/白名单/限流/路由黑名单/信任代理）
    if (g_security_rules) g_security_rules->reload(new_cfg);

    // 上游路由热加载（shared_ptr 替换，飞行请求不受影响）
    if (g_server) g_server->upstreams().reload(new_cfg, http_pool_cfg);
}
```

### 已完成的代码改动

| 文件 | 改动 | 状态 |
|:-----|:------|:----:|
| `src/common/config.hpp` | `load()` 自动扫描 `config.d/`，按文件名排序加载 | ✅ |
| `src/http/upstream_manager.hpp` | `reload()` 增/改/删上游；`shared_ptr` 延迟销毁旧池 | ✅ |
| `src/main.cpp` | reload 回调中调用 `upstreams().reload()` + `stoi` 异常保护 | ✅ |
| `src/http/http_server.hpp` | `ConnGuard` 持 `shared_ptr<HttpPool>` 保活 | ✅ |

### 不需要改的代码

- `security_rules.hpp` — `reload` → `load_from_config` 覆盖限流/IP 黑白名单/信任代理/JWT ✅
- `http_pool.hpp` — 连接池配置重启生效，不热加载
- `rate_limiter.hpp` — `update_config` 已被 `load_from_config` 调用 ✅
