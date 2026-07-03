# Repository Guidelines

## Project Structure & Module Organization

`asio_owen` is a C++20 standalone ASIO HTTP server with HTTP reverse-proxy gateway. Core source lives in `src/`: `src/main.cpp` wires config, routes, pools, logging, and shutdown; `src/http/` handles parsing/responses, gateway proxy (`http_server.hpp`, `http_pool.hpp`, `upstream_manager.hpp`); `src/db/` contains MySQL and Redis pools; `src/common/` holds utilities. Tests live in `tests/`. Dependencies: `asio/` 1.38.0, `spdlog/` v1.17.0, `picohttpparser.c/.h`, and system packages (`mysqlclient`, `hiredis`, `openssl`). Read `DB_POOL_DESIGN.md`, `GATEWAY_DESIGN.md`, and `PERF_REPORT.md` before DB, Redis, logging, gateway, or threading changes.

## Build, Test, and Development Commands

- `cmake -B build -S .`: configure CMake and copy `config.ini` into `build/`.
- `cmake --build build`: build the `server` target.
- `./build/server`: run the local server using `build/config.ini`.
- `CXX=g++ CC=gcc cmake -B build -S .`: configure on Linux with GCC.
- `./build.sh`: build, restart, and smoke-test `/api/health`, `/api/redis`, and `/api/mysql`.

Dependencies: `asio/` and `spdlog/` are fetched via CMake FetchContent (or use local copy when present, gitignored). MySQL, hiredis, and OpenSSL use Homebrew paths on macOS, `pkg-config` on Linux. First configure may download GoogleTest (skipped if `googletest/` absent).

## Architecture & Performance Constraints

HTTP uses one multi-threaded `asio::io_context`, `async_accept`, and one coroutine per keep-alive connection. MySQL wraps synchronous libmysqlclient in an `asio::thread_pool`; never query MySQL on the `io_context` thread. Keep `MysqlPool::execute` copying SQL into a fixed stack buffer before `post`, avoiding `std::string` across workers. Redis uses `thread_local redisContext*` plus direct `redisCommand` with timeouts; do not replace it with a locked pool or keepalive PINGs. Logging must stay on spdlog async sinks.

## Coding Style & Naming Conventions

Use C++20, 4-space indentation, and same-line braces. Types use `PascalCase`; functions, variables, and config fields use `snake_case`; globals in `main.cpp` use `g_`. Preserve standalone ASIO (`ASIO_STANDALONE`, no Boost.Asio).

## Testing Guidelines

Tests use GoogleTest and should be named `test_<component>.cpp`. Add executables to `tests/CMakeLists.txt` and register with `gtest_discover_tests`. Keep unit tests service-free; mark integration tests needing MySQL or Redis. Run `ctest --test-dir build --output-on-failure` before submitting.

## Runtime & Configuration Notes

`config.ini` has `[server]`, `[mysql]`, `[redis]`, `[http_pool]`, and `[upstream]` sections and is copied beside the binary during configure; re-run CMake after edits. Endpoints include `/api/health`, `/api/redis`, `/api/mysql`, `/api/combo`, and `/{service}/...` (gateway reverse proxy, for example `/zebra-config/...`). Pool defaults are intentional: MySQL uses min/max size, idle recycling, timeouts, and `mysql_reset_connection()`; Redis reconnects via `ctx->err`.

## Commit & Pull Request Guidelines

Recent history uses short imperative or descriptive subjects, for example `rename project to asio_owen`. Keep commits focused. PRs should include a summary, test results, linked issues, and curl output or screenshots for HTTP changes.

## Security & Configuration Tips

Treat `config.ini` as environment-specific because it contains database credentials. Do not hard-code secrets in source, tests, docs, or logs. Document any pool sizing, timeout, or shutdown-ordering change.
