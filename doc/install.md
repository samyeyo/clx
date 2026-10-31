# Installation

clx is available as source code (build from any platform) and as pre-built binaries for **Linux (x86_64)**, **macOS (ARM64)**, and **Windows (x86_64)** from [GitHub Releases](https://github.com/samyeyo/clx/releases), built automatically via CI. Linux binaries require glibc ≥ 2.39.

## Build from source

Clone the repository and run the build script on macOS or Linux:

```sh
git clone https://github.com/samyeyo/clx
cd clx
./build.sh install
```

This installs the `clx` compiler to `/usr/local/bin`, the core runtime libraries (`libclx.a`, `libclx_size.a`, `libclx_capi.a`, `libclx_capi_size.a`, `libclx_lua.a`) to `/usr/local/lib`, the per-module std archives (`libclx_<module>.a` / `libclx_<module>_size.a` for base, package, string, table, math, io, os, utf8, coroutine) to `/usr/local/lib/clx/`, and the headers (`clx.h`, `clx_runtime.h`, `clx_simd.h`, plus the Lua 5.5 C API headers `lua.h`, `lauxlib.h`, `lualib.h`) to `/usr/local/include`. Run `./build.sh uninstall` to remove it.

### Windows

```bat
git clone https://github.com/samyeyo/clx
cd clx
build.bat install
```

```bat
:: with a specific architecture (and any other CLX_* option the same way):
set CLX_ARCH=avx2 && build.bat
set CLX_ARCH=avx2 && build.bat install
```

This installs the compiler to `%ProgramFiles%\clx\bin`, the libraries (`clx.lib`, `clx_size.lib`, `clx_capi.lib`, `clx_capi_size.lib`, `clx_lua.lib`, plus the per-module `clx_<module>.lib` / `clx_<module>_size.lib` archives in `lib\clx\`) to `%ProgramFiles%\clx\lib`, and the headers to `%ProgramFiles%\clx\include`. Run `build.bat uninstall` to remove it.

On either platform, override the install location with `-DCMAKE_INSTALL_PREFIX=<dir>` when invoking `cmake` directly. The `build.sh` / `build.bat` wrappers forward `CLX_*` environment variables but not arbitrary `-D` options.

### Target architecture (`CLX_ARCH`)

By default clx targets the widest compatibility baseline:

* **x86_64** → `sse2` (`-msse2`; no `/arch:` flag on MSVC — SSE2 is the x64 baseline) — runs on any x86_64 CPU
* **ARM64** → `native` (`-mcpu=native`) — optimized for the build machine

The same flag is baked into the runtime libraries **and** injected into every binary clx compiles (including `--debug` builds), unless you override it with an explicit compiler flag.

Override at configure time with `CLX_ARCH` (and any other `CLX_*` CMake option) as an environment variable before the wrapper script:

```sh
# x86: sse2 (default) | avx | avx2 | native
CLX_ARCH=avx2 ./build.sh
CLX_ARCH=avx2 ./build.sh install

# ARM: native (default) | generic (portable, no -mcpu flag)
CLX_ARCH=generic ./build.sh
CLX_ARCH=generic ./build.sh install

# Optimize for the build machine specifically
CLX_ARCH=native ./build.sh
CLX_ARCH=native ./build.sh install
```

When invoking `cmake` directly, use `-D` instead:

```sh
cmake -S . -B build -DCLX_ARCH=avx2
cmake --build build
```

| `CLX_ARCH` | x86 flag (Clang/GCC) | x86 flag (MSVC) | ARM flag |
|------------|----------------------|-----------------|----------|
| `sse2` (x86 default) | `-msse2` | *(none — SSE2 is the x64 baseline)* | — |
| `avx` | `-mavx` | `/arch:AVX` | — |
| `avx2` | `-mavx2` | `/arch:AVX2` | — |
| `native` | `-march=native` | `/arch:AVX2` | `-mcpu=native` (ARM default) |
| `generic` | *(fatal on x86 — only valid on ARM)* | — | *(no flag, portable)* |

If you pass an explicit arch flag to `clx` itself, it takes precedence and the `CLX_ARCH` default is not added:

```sh
clx file.lua -march=native   # replaces the default -msse2
clx file.lua -mavx2
```

## Pre-built binaries

The archives from GitHub Releases contain `bin/clx`, the three clx headers, and the core
runtime libraries (`libclx.a` / `libclx_size.a`, plus `libclx_lua.a` when present) inside a
`clx-<platform>/` folder:

```sh
tar xzf clx-linux-x86_64.tar.gz
sudo cp -r clx-linux-x86_64/* /usr/local/
clx --version
```

**Note:** the release archives do **not** currently include the per-module std archives
(`lib/clx/libclx_*.a`), the C API wrapper (`libclx_capi*.a`), or the Lua 5.5 C API headers
that a full source install provides. An extracted prebuilt can print `--version`/`--help`,
but compiling a program requires those module archives (clx hard-errors without them).
Until the packaging catches up, build from source with `./build.sh install` if you need to
compile programs — or copy the missing archives from a source build into
`<prefix>/lib/clx/`.

## Verify installation

```sh
clx --version
# clx 0.5.0
# MIT License - Copyright (c) 2026 Tine Samir
```
