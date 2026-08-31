# Installation

clx is available as source code (build from any platform) and as pre-built binaries for **Linux (x86_64)**, **macOS (ARM64)**, and **Windows (x86_64)** from [GitHub Releases](https://github.com/samyeyo/clx/releases), built automatically via CI. Linux binaries require glibc ≥ 2.39.

## Build from source

Clone the repository and run the build script on macOS or Linux:

```sh
git clone https://github.com/samyeyo/clx
cd clx
./build.sh install
```

This installs the `clx` compiler to `/usr/local/bin`, the runtime libraries (`libclx.a`, `libclx_size.a`, `libclx_lua.a`) to `/usr/local/lib`, and the headers to `/usr/local/include`. Run `./build.sh uninstall` to remove it.

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

This installs the compiler to `%ProgramFiles%\clx\bin`, the libraries (`clx.lib`, `clx_size.lib`) to `%ProgramFiles%\clx\lib`, and the headers to `%ProgramFiles%\clx\include`. Run `build.bat uninstall` to remove it.

On either platform, override the install location with `-DCMAKE_INSTALL_PREFIX=<dir>` when configuring.

### Target architecture (`CLX_ARCH`)

By default clx targets the widest compatibility baseline:

* **x86_64** → `sse2` (`-msse2` / `/arch:SSE2`) — runs on any x86_64 CPU
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
| `sse2` (x86 default) | `-msse2` | `/arch:SSE2` | — |
| `avx` | `-mavx` | `/arch:AVX` | — |
| `avx2` | `-mavx2` | `/arch:AVX2` | — |
| `native` | `-march=native` | `/arch:AVX2` | `-mcpu=native` (ARM default) |
| `generic` | — | — | *(no flag, portable)* |

If you pass an explicit arch flag to `clx` itself, it takes precedence and the `CLX_ARCH` default is not added:

```sh
clx file.lua -march=native   # replaces the default -msse2
clx file.lua -mavx2
```

## Pre-built binaries

The archives from GitHub Releases ship the same layout as a source install, inside a `clx-<platform>/` folder (`bin/`, `include/`, `lib/`), so you can extract them directly into the install prefix of your choice:

```sh
tar xzf clx-linux-x86_64.tar.gz
sudo cp -r clx-linux-x86_64/* /usr/local/
clx --version
```

After this, `clx` is on your PATH with the libraries and headers under `/usr/local`.

## Verify installation

```sh
clx --version
# clx 0.3.0
# MIT License - Copyright (c) 2026 Tine Samir
```
