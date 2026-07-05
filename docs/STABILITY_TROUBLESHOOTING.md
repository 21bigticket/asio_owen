# 稳定性与性能排查记录

## 背景

新版按 `DB_POOL_DESIGN.md` 实施连接池后，MySQL 性能已基本恢复，Redis 仍低于旧版，Health 基线也有下降。当前判断：Redis 剩余差距主要不是 RedisPool 单点问题，而是 HTTP 基线下降、运行环境波动、命令格式化开销等因素叠加。

## 当前压测结论

- MySQL：`worker_threads=64`、`release notify_one`、shutdown 防护后，从约 `-59%` 回升到约 `-9%`。
- Redis：预热后约 `26k RPS`，fast path 后约 `28k RPS`，相对旧版约 `-17%`。
- Health：存在明显波动，曾出现 `88k` 与 `70k` 两档，需用多轮中位数判断。

## Redis 排查结论

已排除或基本排除：

- `cmd_timeout_ms=0` 已生效，`redisSetTimeout` 不再是当前主因。
- `Redis cmd failed = 0`，无命令失败。
- `rebuilding = 0`，无频繁重连。
- `tls_map_` 已改回裸 `thread_local redisContext*`。
- 预热能提升首轮表现，但不能解释稳定缺口。

仍建议保留：

- `cmd_timeout_ms` 配置开关：压测/内网性能模式设 `0`，生产稳定性优先可设 `1000`。
- Redis GET fast path：固定命令避免走 `cmd()` 的 `vsnprintf + std::string` 分配。
- 启动预热：避免首轮压测包含 thread-local Redis 建连成本。

## HTTP 基线排查步骤

1. 同环境重跑旧版和新版  
   在同一台 VM、同一时间窗口，各跑 Health 5-10 次，取中位数，不用平均数。当前波动过大，单次数据不能定性。

2. 隔离 MySQL 后台资源  
   Health 不查 DB，但新版仍会启动 MySQL worker、连接池和 maintain 线程。临时配置：

   ```ini
   [mysql]
   min_size = 0
   max_size = 0
   worker_threads = 1
   ```

   只压 `/api/health`。如果 Health 回升，说明瓶颈来自后台线程/连接池资源竞争，不是 HTTP 代码。

3. 验证断连日志路径  
   当前 HTTP 断连走 `LOG_DEBUG`，INFO 级别下只做 `should_log`。如果压测工具频繁新建连接，仍可能进入热路径。可临时空 catch 再对比。

4. 确认 keep-alive 参数一致  
   旧版和新版必须使用完全相同的 plow 参数。若每请求新建 TCP，accept/close/异常路径会显著影响 Health。

5. 使用 perf 定位热点

   ```bash
   perf top -p $(pidof server)
   perf record -F 99 -p $(pidof server) -g -- sleep 10
   perf report
   ```

   重点看热点是否落在 `malloc/free`、`ostringstream`、`phr_parse_request`、`async_write`、系统调用、锁或调度。

## 建议的下一轮实验

1. 清空日志后启动服务，先预热 Redis：

   ```bash
   for i in $(seq 1 1000); do curl -s http://127.0.0.1:8081/api/redis >/dev/null; done
   ```

2. Health 连续跑 5 次，记录中位数。
3. 使用 MySQL 隔离配置再跑 Health。
4. Redis 使用 fast path 跑 3-5 次，记录中位数。
5. 如 Health 仍低于旧版 10% 以上，进入 perf 分析。

## 协程与真异步改造评估

当前项目已经在接口层使用 C++20 协程：

- HTTP handler 返回 `asio::awaitable<std::string>`，通过 `co_await` / `co_return` 编写。
- MySQL `execute()` 返回 `asio::awaitable<Result>`，调用方可 `co_await`。
- Redis `cmd()` / `get()` 返回 `asio::awaitable<Reply>`，调用方可 `co_await`。

但底层 I/O 并不完全是真异步：

- MySQL 使用同步 `libmysqlclient`，通过 `asio::thread_pool` 包装，worker 线程里阻塞执行 `mysql_query()`。
- Redis 使用同步 hiredis `redisCommand()`，直接在当前 `io_context` 线程执行。

### MySQL 真异步

不建议短期改造。原因：

- 常规 `libmysqlclient` 查询 API 是同步阻塞模型。
- 真异步需要更换客户端库、使用复杂 nonblocking C API，或自行实现 MySQL 协议，维护成本高。
- 当前 MySQL 压测已从约 `-59%` 回升到约 `-9%`，继续大改收益有限。

更务实的优化方向：

- 继续调优 `worker_threads`。
- 优化 SQL 和结果 JSON 构造。
- 保持 `char[4096]` SQL 跨线程传递方式，避免 `std::string` 跨 worker 边界。

### Redis 真异步

可作为中长期稳定性优化，但不应作为当前 RPS 回退的首要修复。原因：

- 当前 Redis fast path 已约 `28k RPS`，剩余差距与 Health 基线下降高度相关。
- 真异步 Redis 能避免 `redisCommand()` 阻塞 `io_context`，对故障隔离和尾延迟更有价值。
- RPS 收益不确定，预估约 `0-20%`，前提是 HTTP 基线先恢复。

短期建议：

- 保留 Redis `get()` fast path，避免固定 GET 走 `vsnprintf + std::string` 分配。
- 保留 `cmd_timeout_ms` 配置开关：压测设 `0`，生产按稳定性需求设 `1000`。
- 增加启动预热，减少首轮 thread-local 建连影响。

长期可评估：

- 用 ASIO socket 实现简单 RESP GET/SET。
- 或引入成熟 async Redis 客户端。

结论：MySQL 真异步改动大、收益低；Redis 真异步主要提升稳定性和尾延迟，RPS 收益需在 HTTP 基线恢复后再评估。
