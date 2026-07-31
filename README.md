# ftp-lwip-lfs

[![Build](https://github.com/tim-hilt/ftp-lwip-lfs/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/tim-hilt/ftp-lwip-lfs/actions/workflows/ci.yml?query=branch%3Amain)
[![Coverage](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/tim-hilt/ftp-lwip-lfs/main/.github/badges/coverage.json)](https://github.com/tim-hilt/ftp-lwip-lfs/actions/workflows/ci.yml?query=branch%3Amain)
[![Tests](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/tim-hilt/ftp-lwip-lfs/main/.github/badges/tests.json)](https://github.com/tim-hilt/ftp-lwip-lfs/actions/workflows/ci.yml?query=branch%3Amain)

A minimal FTP server for embedded systems, built on the [lwIP](https://savannah.nongnu.org/projects/lwip/) raw TCP API and backed by a [LittleFS](https://github.com/littlefs-project/littlefs) filesystem. It exposes a small C API (`ftp_server.h`) intended to be dropped into a microcontroller firmware project.

## Features

- Event-driven, non-blocking implementation using lwIP's raw (callback) API — no RTOS or blocking sockets required.
- Serves files directly from an already-mounted LittleFS (`lfs_t`) instance.
- Supports both passive (PASV) and active (PORT) data connections.
- Optional username/password authentication, with a per-connection limit on failed logins.
- Configurable number of concurrent client sessions.
- Commands supported: `USER`, `PASS`, `SYST`, `FEAT`, `OPTS`, `TYPE`, `MODE`, `STRU`, `PWD`/`XPWD`, `CWD`/`XCWD`, `CDUP`/`XCUP`, `PASV`, `PORT`, `LIST`, `NLST`, `RETR`, `STOR`, `DELE`, `MKD`/`XMKD`, `RMD`/`XRMD`, `RNFR`, `RNTO`, `SIZE`, `NOOP`, `QUIT`, `ABOR` (the `X`-prefixed aliases are optional, see `FTP_SERVER_ENABLE_X_COMMANDS`).

## Files

| File            | Purpose                                      |
|-----------------|-----------------------------------------------|
| `ftp_server.h`  | Public API and compile-time configuration.    |
| `ftp_server.c`  | Server implementation (session/state machine, command dispatch, data transfer). |
| `DESIGN.md`     | Design/architecture notes — behaviour rationale and RFC references. |

## Requirements

- An lwIP stack (raw API) already initialized and running.
- A LittleFS instance already mounted (`lfs_mount()` succeeded).
- A working `tcp_*` callback-driven main loop (e.g. `sys_check_timeouts()` / lwIP's poll loop, or an RTOS lwIP port).

## Building

This is a plain C source/header pair with no build system of its own — add both files to your existing firmware build (Makefile, CMake, PlatformIO, etc.) alongside your lwIP and LittleFS sources, and ensure their include paths are visible:

```
your_project/
├── lwip/            (lwip/tcp.h, lwip/err.h, ...)
├── littlefs/         (lfs.h)
└── ftp_server.c/.h   (this project)
```

Example CMake snippet:

```cmake
target_sources(firmware PRIVATE ftp_server.c)
target_include_directories(firmware PRIVATE .)
target_link_libraries(firmware PRIVATE lwipcore littlefs)
```

## Configuration

Override any of these macros before including `ftp_server.h` (e.g. via a `-D` compiler flag or a project-wide config header):

| Macro                            | Default | Description                                      |
|-----------------------------------|---------|---------------------------------------------------|
| `FTP_SERVER_PORT`                 | `21`    | Control connection port.                          |
| `FTP_SERVER_PASV_PORT_MIN`        | `1024`  | First passive-mode data port.                     |
| `FTP_SERVER_PASV_PORT_MAX`        | `1039`  | Last passive-mode data port.                      |
| `FTP_SERVER_MAX_CLIENTS`          | `2`     | Maximum concurrent sessions.                      |
| `FTP_SERVER_USER`                 | `NULL`  | Required username, or `NULL` to skip auth.        |
| `FTP_SERVER_PASS`                 | `NULL`  | Required password, or `NULL` to skip auth.        |
| `FTP_SERVER_MAX_LOGIN_ATTEMPTS`   | `3`     | Failed logins tolerated on one control connection before it is dropped with `421`; `0` disables the limit (see [DESIGN.md](DESIGN.md#security)). |
| `FTP_SERVER_DATA_BUF_SIZE`        | `512`   | Per-session transfer buffer size (bytes); minimum 64. A LIST line is 47 bytes plus the name, so anything at or below `47 + LFS_NAME_MAX` truncates listings of the longest names. |
| `FTP_SERVER_CMD_BUF_SIZE`         | `256`   | Per-session command line buffer size (bytes); minimum 16. |
| `FTP_SERVER_PATH_MAX`             | `256`   | Maximum resolved path length; minimum 16.         |
| `FTP_SERVER_FILE_CACHE_SIZE`      | `256`   | LFS file cache per session — must be >= `lfs_config.cache_size` (checked at `ftp_server_init()`). |
| `FTP_SERVER_IDLE_TIMEOUT_POLLS`   | `60`    | `tcp_poll` intervals (~5 s each) before an idle session is disconnected (see [DESIGN.md](DESIGN.md#resource-model)). |
| `FTP_SERVER_ENABLE_X_COMMANDS`    | `1`     | Accept the obsolete RFC 775 aliases `XPWD`/`XCWD`/`XCUP`/`XMKD`/`XRMD`; `0` drops five dispatch-table entries. |

`ftp_server_init()` and `ftp_server_deinit()` are raw-API calls like every callback in this file: call them from the lwIP thread, or with the core lock held.

## Usage

```c
#include "lfs.h"
#include "ftp_server.h"

lfs_t lfs;

void app_init(void)
{
    /* 1. Mount your LittleFS filesystem. */
    int err = lfs_mount(&lfs, &lfs_cfg);
    if (err) {
        /* handle mount failure, e.g. lfs_format() + retry */
    }

    /* 2. Start the FTP server on top of the mounted filesystem. */
    if (ftp_server_init(&lfs) != ERR_OK) {
        /* handle startup failure */
    }

    /* 3. Run your normal lwIP loop; the server operates entirely
     *    through tcp_* callbacks and needs no extra polling. */
}

void app_shutdown(void)
{
    ftp_server_deinit();
}
```

Then connect with any standard FTP client:

```sh
ftp <device-ip>
```

If `FTP_SERVER_USER`/`FTP_SERVER_PASS` are defined, log in with those credentials; otherwise any `USER`/`PASS` is accepted. With both defined, a wrong username is not rejected until `PASS` — see the RFC 2577 §3 note in [DESIGN.md](DESIGN.md#security).

## Design

Behavioural details — transfer semantics, resource limits, the RFC 2577 data-connection and login hardening, and the lwIP/littlefs quirks they follow from — are documented in [DESIGN.md](DESIGN.md).

## Continuous Integration

`.github/workflows/ci.yml` (workflow name `Build`) runs on every push/PR to `main` with four jobs; `build`, `unit-tests`, and `clang-tidy` are required to pass:

| Job           | What it checks                                                          |
|---------------|--------------------------------------------------------------------------|
| `build`       | `ftp_server.c`/`.h` compile cleanly (`-Werror -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion` + unused-code warnings). |
| `unit-tests`  | All five Catch2 suites build and pass (see [Testing](#testing)).       |
| `clang-tidy`  | Static analysis is clean (see below).                                  |
| `coverage`    | Computes line coverage of `ftp_server.c` (via `gcovr`) after `build`/`unit-tests` pass; on `main` pushes it commits the badge data to `.github/badges/coverage.json`. |

The two badges above the project title read this directly: **Build** is GitHub's native workflow badge for the `Build` workflow on `main`; **Coverage** is a [shields.io endpoint badge](https://shields.io/badges/endpoint-badge) pointing at the raw `.github/badges/coverage.json` on `main`, which the `coverage` job keeps up to date.

## Testing

Unit tests exercise `ftp_server.c` against the mock lwIP/LittleFS headers in `tests/mock/` using [Catch2](https://github.com/catchorg/Catch2) (fetched automatically if not found locally):

```sh
cmake -S . -B build -DFTP_SERVER_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Several configuration macros are compile-time constants, so the paths they guard cannot all be reached from a single build. CMake therefore produces six executables, each linked against its own copy of the library:

| Executable             | Library configuration                 | Covers                                     |
|------------------------|---------------------------------------|--------------------------------------------|
| `ftp_tests`            | no credentials (the default)          | `tests/test_main.cpp` — the protocol        |
| `ftp_tests_auth`       | `FTP_SERVER_USER` + `FTP_SERVER_PASS` | `tests/test_auth.cpp` — USER/PASS flow      |
| `ftp_tests_user_only`  | `FTP_SERVER_USER` only                | `tests/test_auth.cpp` — USER logs in        |
| `ftp_tests_short_path` | `FTP_SERVER_PATH_MAX=64`              | `tests/test_path_limits.cpp` — path overflow, unreachable while `FTP_SERVER_PATH_MAX` equals `FTP_SERVER_CMD_BUF_SIZE` |
| `ftp_tests_short_data` | `FTP_SERVER_DATA_BUF_SIZE=64`         | `tests/test_list_truncation.cpp` — a LIST line too long for the transfer buffer |
| `ftp_tests_no_x`       | `FTP_SERVER_ENABLE_X_COMMANDS=0`      | `tests/test_no_x_commands.cpp` — the aliases are gone and nothing else with them |

The `coverage` job merges the `.gcda` files from all six, so the reported figure covers the credential and overflow paths too. `tests/ftp_test_support.hpp` holds the shared harness that drives ftp_server.c's registered lwIP callbacks.

## Static Analysis

`ftp_server.c`/`.h` are linted with [clang-tidy](https://clang.llvm.org/extra/clang-tidy/) (config: `.clang-tidy`). Ruleset covers `bugprone-*`, `cert-*`, `clang-analyzer-*`, `misc-*`, `performance-*`, `portability-*`, `readability-*`, plus `cppcoreguidelines-no-malloc`/`hicpp-no-malloc` to enforce the no-heap-allocation guideline. The build compiles `ftp_server.c` against lightweight mock headers (in `tests/mock/`) that provide the lwIP and LittleFS API surface — no submodules or vendored sources required.

Recommended — use the provided `Dockerfile`, which pins the exact toolchain (`ubuntu:24.04` + apt's `clang-tidy`) the `clang-tidy` job in the CI workflow (`.github/workflows/ci.yml`) builds and runs, so results are deterministic and match CI exactly — no local clang-tidy version skew:

```sh
docker build -t ftp-lwip-lfs-clang-tidy .
docker run --rm -v "$PWD":/repo ftp-lwip-lfs-clang-tidy
```

Without Docker:

```sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clang-tidy -p build ftp_server.c
```

On macOS with Homebrew LLVM, pass the SDK sysroot explicitly (Apple Clang's driver does this automatically, but `clang-tidy` does not); versions newer than CI's may also report findings CI does not (see `.github/workflows/ci.yml` for the pinned Ubuntu/apt version):

```sh
clang-tidy -p build --extra-arg=-isysroot --extra-arg=$(xcrun --show-sdk-path) ftp_server.c
```

Alternatively, build with `-DFTP_SERVER_ENABLE_CLANG_TIDY=ON` to run clang-tidy as part of `ftp_server`'s build step. CI runs it on every push/PR via the `clang-tidy` job in `.github/workflows/ci.yml`.
