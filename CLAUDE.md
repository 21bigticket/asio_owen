# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

`asio_owen` is a C++20 full-stack HTTP server built on standalone ASIO coroutines (`-DASIO_STANDALONE -DASIO_HAS_CO_AWAIT`, **no Boost**). It exposes a small JSON API backed by a MySQL connection pool and a Redis pool, plus an HTTP reverse-proxy gateway (`/{service}/...`, for example `/zebra-config/...`). Performance characteristics and past incidents are documented in `PERF_REPORT.md` — read it before touching the DB or logging layers.

## Build & Run

CMake 3.20+, C++20. Single-config generators default to `Release` unless `-DCMAKE_BUILD_TYPE=...` is provided. Out-of-source build directory convention is `build/` (CLion uses `cmake-build-debug/`).

```bash
# Configure + build
cmake -B build -S .
cmake --build build

# Run (config.ini must sit next to the binary — configure_file() copies it at build time)
./build/server
```

External dependencies are **fetched via CMake FetchContent** (`asio/` 1.38.0, `spdlog/` v1.17.0, `googletest/` v1.14.0), or used from local vendored directories when present (gitignored). macOS Homebrew paths for `mysql-client`, `hiredis`, and `openssl@3` are probed; Linux uses `pkg-config`.

**Test targets exist** in `tests/` (placeholder, mysql, redis) but require local googletest/ directory — disabled automatically when absent.

## Architecture

The runtime is a **single `asio::io_context` driven by N threads** (N = `std::thread::hardware_concurrency()`, fallback 4). All coroutines share that one `io_context`. See `src/main.cpp:142-148` for the spawn loop.

### HTTP layer (`src/http/`)

- `HttpServer` accepts with `async_accept` and `co_spawn`s a `handle_connection` coroutine per connection. Each coroutine loops over `async_read_some` to support **HTTP keep-alive** — one TCP connection serves multiple requests.
- HTTP requests are parsed with `picohttpparser.c/h` (vendored at repo root, compiled into the `server` target).
- Routes are a flat `unordered_map<string, Handler>` keyed by exact path. Handlers are `asio::awaitable<std::string>(body, path)` and return the JSON body only — the server wraps it in `HTTP/1.1 200 ... Content-Length` regardless of handler outcome, so error codes live inside the JSON envelope (`response.hpp`'s `HttpCode` enum), not in HTTP status.

### DB layer (`src/db/`)

The two pools use **deliberately different async strategies** — this is the core architectural decision and the source of past crashes:

- **`MysqlPool`** wraps the *synchronous* libmysqlclient API (`mysql_query`/`mysql_store_result`) by `asio::post`-ing each query onto a dedicated `asio::thread_pool` (sized to `pool_size`). The calling coroutine `co_await`s the result. Connections are shared across worker threads and protected by a `mutex` + `condition_variable` acquire/release queue.
- **`RedisPool`** calls synchronous `redisCommand` **directly on the io_context thread** (no thread_pool). This is safe because (a) hiredis calls are microseconds-fast and (b) each io_context thread holds its own `thread_local redisContext*` (`tls_conn_`), so there are no locks and no cross-thread sharing.

### Cross-thread SQL passing — do not regress

`MysqlPool::execute` (`src/db/mysql_pool.hpp:54`) deliberately copies the SQL into a `char sql_buf[4096]` stack array and captures **that** in the `asio::post` lambda, not the `std::string`. The earlier code that captured `std::string` by value caused `double free` crashes under load — see PERF_REPORT.md "根因分析". When modifying this code, never let a `std::string` cross the `asio::post` boundary into the worker pool.

### Logging

`Logger` is a singleton wrapping spdlog's **async logger** with a rotating file sink (50MB × 3) and a colored console sink. The `LOG_INFO(...)` / `LOG_WARN(...)` / `LOG_ERROR(...)` / `LOG_DEBUG(...)` macros stream into a per-call `ostringstream` before forwarding, so they accept `<<`-style variadic args (`LOG_INFO("port=", port)`). Switching back to a sync logger regresses RPS significantly (see PERF_REPORT.md).

### Lifecycle

- Globals in `main.cpp`: `g_mysql`, `g_redis`, `g_server` (all `unique_ptr`).
- `SignalExit` listens on `SIGINT`/`SIGTERM` and runs a callback that calls `stop()`/`shutdown()` on each subsystem before `ioc.stop()`. Add new subsystems with shutdown ordering here.
- `with_timeout` (`src/common/timeout.hpp`) is a template that races a child coroutine against a `steady_timer` and returns `std::optional<T>` — used by the `/api/combo` handler for cache-then-DB fallback.

## Configuration

`config.ini` (INI format, parsed by `src/common/config.hpp`) has `[server]`, `[mysql]`, `[redis]`, `[http_pool]`, and `[upstream]` sections. The config file is **gitignored** because it carries DB credentials, but a reference copy is checked in at `config.ini`. It is copied next to the binary at configure time via `configure_file(... COPYONLY)`, so edit the source-tree copy and re-run CMake to update the runtime copy.

## Known runtime endpoints

`/api/health`, `/api/redis`, `/api/mysql`, `/api/combo` (Redis cache → MySQL fallback with 500ms timeout and detached cache-fill write-back), plus gateway routes `/{service}/...` configured by `[upstream]`.
