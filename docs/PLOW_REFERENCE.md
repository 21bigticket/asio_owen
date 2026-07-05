# plow 压测命令参考

## 参数速查

| 参数 | 说明 | 示例 |
|------|------|------|
| `-c N` | 并发数 | `-c 100` |
| `-d DUR` | 压测时长（⚠️ `-d` 是 duration，不是 body） | `-d 30s`、`-d 5m` |
| `-m METHOD` | HTTP 方法 | `-m POST` |
| `-H 'K: V'` | 请求头，可重复 | `-H 'Content-Type: application/json'` |
| `-b BODY` | 请求 body 字符串（❌ 不支持 `-b @file`） | `-b '{"k":"v"}'`、`-b "$VAR"` |
| `-T TYPE` | 快捷设置 Content-Type | `-T 'application/json'` |
| `--json` | 输出 JSON 格式 | `--json` |

## GET 请求

```bash
plow -c 100 -d 30s http://127.0.0.1:8081/api/health
plow -c 100 -d 30s http://127.0.0.1:8081/api/redis
plow -c 100 -d 30s http://127.0.0.1:8081/api/mysql
```

## POST 请求

```bash
BODY='{"appid":"member_03150715","config_key":"black_list"}'

# 直连上游
plow -c 100 -d 30s -m POST \
  -H 'Content-Type: application/json' \
  -b "$BODY" \
  http://127.0.0.1:30001/config.ConfigService/GetByAppAndKey

# 通过网关（带 JWT）
TOKEN=$(cat /tmp/jwt.txt)
plow -c 100 -d 30s -m POST \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $TOKEN" \
  -b "$BODY" \
  http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey
```

## 坑记录

| 错误用法 | 结果 | 正确用法 |
|---------|------|---------|
| `-d '{"key":"val"}'` | ❌ `-d` 是 duration，body 会变成 `d` 的剩余启动参数 | `-b '{"key":"val"}'` |
| `-b @/tmp/body.json` | ❌ 发文件名字符串 `/tmp/body.json` 而非内容，上游 404 | `BODY=$(cat /tmp/body.json); -b "$BODY"` |
| `--body=@/tmp/body.json` | ❌ 同上 | 同上 |
