# Repository Guidelines

## Project Structure & Module Organization

`asio_owen` is a C++20 standalone ASIO HTTP server with HTTP reverse-proxy gateway. Core source lives in `src/`: `src/main.cpp` wires config, routes, pools, logging, and shutdown; `src/http/` handles parsing/responses, gateway proxy (`http_server.hpp`, `http_pool.hpp`, `upstream_manager.hpp`); `src/db/` contains MySQL and Redis pools; `src/common/` holds utilities. Tests live in `tests/`. Dependencies: `asio/` 1.38.0, `spdlog/` v1.17.0, `picohttpparser.c/.h`, and system packages (`mysqlclient`, `hiredis`, `openssl`). Read `DB_POOL_DESIGN.md`, `GATEWAY_DESIGN.md`, and `PERF_REPORT.md` before DB, Redis, logging, gateway, or threading changes.

## Build, Test, and Development Commands

- `cmake -B build -S .`: configure CMake and copy `config.d/` into `build/`.
- `cmake --build build`: build the `server` target.
- `./build/server`: run the local server (loads config from `build/config.d/`).
- `CXX=g++ CC=gcc cmake -B build -S .`: configure on Linux with GCC.
- `./build.sh`: build, restart, and smoke-test `/api/health`, `/api/redis`, and `/api/mysql`.

Dependencies: `asio/` and `spdlog/` are fetched via CMake FetchContent (or use local copy when present, gitignored). MySQL, hiredis, and OpenSSL use Homebrew paths on macOS, `pkg-config` on Linux. First configure may download GoogleTest (skipped if `googletest/` absent).

## Architecture & Performance Constraints

HTTP uses one multi-threaded `asio::io_context`, `async_accept`, and one coroutine per keep-alive connection. MySQL wraps synchronous libmysqlclient in an `asio::thread_pool`; never query MySQL on the `io_context` thread. Keep `MysqlPool::execute` switching executor with `co_await asio::post(worker_pool_, asio::use_awaitable)` before using the coroutine-frame SQL string; do not reintroduce a posted lambda that captures request data. Redis is dual-mode: `direct` keeps the thread-local fast path, while the checked-in config runs `worker` mode with a dedicated thread pool, shared idle pool, maintain PING, and command-level idempotent retry. Logging must stay on spdlog async sinks.

## Coding Style & Naming Conventions

Use C++20, 4-space indentation, and same-line braces. Types use `PascalCase`; functions, variables, and config fields use `snake_case`; globals in `main.cpp` use `g_`. Preserve standalone ASIO (`ASIO_STANDALONE`, no Boost.Asio).

## Testing Guidelines

Tests use GoogleTest and should be named `test_<component>.cpp`. Add executables to `tests/CMakeLists.txt` and register with `gtest_discover_tests`. Keep unit tests service-free; mark integration tests needing MySQL or Redis. Run `ctest --test-dir build --output-on-failure` before submitting.

## Runtime & Configuration Notes

Configuration is split across `config.d/*.ini` files (loaded in sorted order, later overrides earlier). Key sections: `[server]`, `[mysql]`, `[redis]`, `[http_pool]`, and `[upstream]`. Endpoints include `/api/health`, `/api/redis`, `/api/mysql`, `/api/combo`, and `/{service}/...` (gateway reverse proxy, for example `/zebra-config/...`). Pool defaults are intentional: MySQL uses min/max size, idle recycling, timeouts, and `mysql_reset_connection()`; Redis worker mode isolates synchronous hiredis from `io_context`, while direct mode reconnects via `ctx->err`.

## Commit & Pull Request Guidelines

Recent history uses short imperative or descriptive subjects, for example `rename project to asio_owen`. Keep commits focused. PRs should include a summary, test results, linked issues, and curl output or screenshots for HTTP changes.

## Security & Configuration Tips

Treat config files under `config.d/` as environment-specific because they contain database credentials. Do not hard-code secrets in source, tests, docs, or logs. `99-local.ini` is gitignored for local overrides. Document any pool sizing, timeout, or shutdown-ordering change.
