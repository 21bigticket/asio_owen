# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

`asio_owen` is a C++20 full-stack HTTP server built on standalone ASIO coroutines (`-DASIO_STANDALONE -DASIO_HAS_CO_AWAIT`, **no Boost**). It exposes a small JSON API backed by a MySQL connection pool and a Redis pool, plus an HTTP reverse-proxy gateway (`/{service}/...`, for example `/zebra-config/...`). Current architecture constraints live in `DB_POOL_DESIGN.md` and `GATEWAY_DESIGN.md`; `PERF_REPORT.md` is a historical performance and incident log.

## Build & Run

CMake 3.20+, C++20. Single-config generators default to `Release` unless `-DCMAKE_BUILD_TYPE=...` is provided. Out-of-source build directory convention is `build/` (CLion uses `cmake-build-debug/`).

```bash
# Configure + build
cmake -B build -S .
cmake --build build

# Run (loads config from `config.d/` next to the binary — configure_file() copies it at build time)
./build/server
```

External dependencies are **fetched via CMake FetchContent** (`asio/` 1.38.0, `spdlog/` v1.17.0, `googletest/` v1.14.0), or used from local vendored directories when present (gitignored). macOS Homebrew paths for `mysql-client`, `hiredis`, and `openssl@3` are probed; Linux uses `pkg-config`.

**Test targets exist** in `tests/`. GoogleTest is used from local `googletest/` when present, otherwise CMake FetchContent downloads v1.14.0 when `BUILD_TESTING=ON`.

## Architecture

The runtime is a **single `asio::io_context` driven by N threads** (N = `std::thread::hardware_concurrency()`, fallback 4). All coroutines share that one `io_context`. `src/main.cpp` is only bootstrap glue; runtime wiring lives in `src/app/application.cpp`.

### HTTP layer (`src/http/`)

- `HttpServer` accepts with `async_accept` and `co_spawn`s a `handle_connection` coroutine per connection. Each coroutine loops over `async_read_some` to support **HTTP keep-alive** — one TCP connection serves multiple requests.
- HTTP requests are parsed with `picohttpparser.c/h` (vendored at repo root, compiled into the `server` target).
- Routes are stored in exact and prefix route tables. Handlers receive a mutable `HttpContext`, return `asio::awaitable<void>`, and set the response body, headers, and real HTTP status. JSON responses also retain the application-level code from `response.hpp`; transport failures and authorization errors use matching HTTP status lines such as 401, 429, and 502.

### DB layer (`src/db/`)

The DB pools isolate synchronous client libraries from the HTTP event loop where the current configuration requires it:

- **`MysqlPool`** wraps the synchronous libmysqlclient API (`mysql_query`/`mysql_store_result`) by switching the coroutine to a dedicated `asio::thread_pool` before `do_query()`. Connections are shared across worker threads and protected by a `mutex` + `condition_variable` acquire/release queue.
- **`RedisPool`** is dual-mode. The checked-in config uses `mode = worker`, so `cmd_argv()` / `get()` switch to a dedicated Redis `asio::thread_pool` and use a shared idle pool plus maintain thread. `mode = direct` remains available as a thread-local fast path where each calling thread owns a `TlsRedisConn`.

### Cross-thread SQL passing — do not regress

`MysqlPool::execute` switches to its dedicated worker executor with `co_await asio::post(..., use_awaitable)` before using the coroutine-frame SQL string. Do not reintroduce a lambda that captures non-POD request data across the executor boundary.

### Logging

`Logger` is a singleton wrapping spdlog's **async logger** with a rotating file sink (50MB × 3) and a colored console sink. The `LOG_INFO(...)` / `LOG_WARN(...)` / `LOG_ERROR(...)` / `LOG_DEBUG(...)` macros stream into a per-call `ostringstream` before forwarding, so they accept `<<`-style variadic args (`LOG_INFO("port=", port)`). Switching back to a sync logger regresses RPS significantly (see PERF_REPORT.md).

### Lifecycle

- `Application` owns `MysqlPool`, `RedisPool`, `HttpServer`, `SecurityRules`, `ReloadService`, and `SnapshotService`.
- `SignalExit` listens on `SIGINT`/`SIGTERM` and asks `Application` to stop accepting, then a drain timer stops the `io_context`. Add new subsystems to `Application::cleanup()` with explicit shutdown ordering.
- `/api/combo` returns a 504 after its configurable `server.combo_deadline_ms` soft deadline (default 500ms) on MySQL fallback. The synchronous MySQL operation is not cancelled; it retains one of `server.combo_max_in_flight_queries` fallback-query permits (default 8) until it completes, then returns its connection normally. Do not document this deadline as cancellation.

## Configuration

Configuration is split across `config.d/*.ini` files (loaded in sorted order, later overrides earlier), see `config.d/00-server.ini` ~ `99-local.ini`. The config files are **gitignored** because they carry DB credentials, but reference copies are checked in at `config.d/*.ini`. The directory is copied next to the binary at configure time via `file(COPY ...)`, so edit the source-tree copy and re-run CMake to update the runtime copy.

## Known runtime endpoints

`/api/health`, `/api/redis`, `/api/mysql`, `/api/combo` (Redis cache → MySQL fallback with configurable soft deadline/in-flight cap and detached cache-fill write-back), plus gateway routes `/{service}/...` configured by `[upstream]`.
