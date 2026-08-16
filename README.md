<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="www/img/clx-logo-dark.png">
  <source media="(prefers-color-scheme: light)" srcset="www/img/clx-logo-light.png">
  <img alt="CLX Logo" src="doc/img/clx-logo-light.png" width="300">
</picture>
<br /><br />

Cross-platform ahead-of-time Lua compiler

![Lua 5.5](https://img.shields.io/badge/Lua-5.5-E65100?logo=lua&logoColor=white)
![C++20](https://img.shields.io/badge/C%2B%2B-20-1565C0?logo=cplusplus&logoColor=white)
![License: MIT](https://img.shields.io/badge/license-MIT-4CAF50?logo=opensourceinitiative&logoColor=white)
[![ko-fi](https://img.shields.io/badge/Ko--fi-Support%20me-FF5E5B?logo=kofi&logoColor=white)](https://ko-fi.com/samirtine)

</div>
<br /><br />

**clx** is a cross-platform ahead-of-time Lua compiler and runtime that generates standalone native executables through modern C++ toolchains.
**clx** is not trying to be the fastest Lua implementation in every workload.

## Quick Start

```bash
git clone https://github.com/samyeyo/clx.git
cd clx
./build.sh install       # or build.bat install on Windows
clx examples/hello/hello.lua
./hello

Hello clx !
```

## What makes clx useful

- **Just write Lua.** clx targets Lua 5.5, so your code works like ordinary Lua.
- **Fast native speed** — your scripts are compiled to native machine code.
- **Standalone binaries** — no interpreter or extra runtime to install alongside your program.
- **Small outputs** — size-friendly builds can create very compact executables.
- **Cross-platform** — works on Linux, macOS, and Windows.
- **Extensible** — add native C++ modules when you need extra performance.

## Examples built with clx

clx ships real, runnable examples that show native desktop application development using standard Lua code.

### Pong

![Pong](examples/pong/pong.gif)

A complete game written in Lua and compiled into a standalone native executable.

### Mandelbrot viewer

![Mandelbrot](examples/mandelbrot/mandelbrot.jpg)

A Mandelbrot viewer written in Lua and compiled into a standalone native executable.

## Project status

clx is currently in **beta**. It can already compile real Lua applications, and compatibility and performance work continues.

## Requirements

- **Linux**: `g++` or `clang++`
- **macOS**: `clang++` (Xcode) or `g++` via Homebrew
- **Windows**: `g++` (LLVM) or MSVC
- **CMake 3.15+** to build from source

> The compiler that builds clx is the same one used to compile your Lua scripts, keeping everything consistent. If you need a different compiler, just rebuild clx with it.

## Using clx

The easiest way to use clx is to compile a Lua file into a runnable program:

```bash
clx file.lua          # compiles file.lua and produces an executable
./file                # run it
```

A few common options when you need more control:

```bash
clx file.lua --size        # optimize for size (default)
clx file.lua --fast        # optimize for speed
clx file.lua --output app  # give the output a custom name
clx file.lua --dynamic     # allow loading Lua code at runtime
clx --help                 # see all options
```

- **`load()` / `loadfile()` / `dofile()`** run on an embedded Lua engine and are only available when you compile with `--dynamic`. See [Dynamic Lua](./doc/dynamic-lua.md).
- To write native modules, clx uses its own **C++ API** (the classic Lua C API is not supported). See [Modules](./doc/modules.md).

## Benchmarks

clx compares well against the reference Lua 5.5 interpreter:

| Script | lua 5.5 | LuaJIT | clx `--fast` |
|--------|---------|--------|--------------------------|
| fib.lua | 0.299s (1.00x) | 0.044s (6.80x) | **0.007s (42.71x)** |
| arraysum.lua | 0.317s (1.00x) | 0.098s (3.23x) | **0.083s (3.82x)** |
| spectralnorm.lua | 0.382s (1.00x) | **0.020s (19.10x)** | 0.036s (10.61x) |
| canada.lua | 0.347s (1.00x) | **0.134s (2.59x)** | 0.231s (1.50x) |
| warmup.lua | **0.003s (1.00x)** | 0.005s (0.60x) | 0.006s (0.50x) |

> Measured on an Intel® Core™ i5 Ultra 125U CPU @ 4.30GHz · Linux · GCC 13.3.0 · Average of 10 runs

> Full benchmarks are available in **[clx benchmarks](./doc/benchmarks.md)**

## Documentation

Detailed guides live in the `doc/` directory:

- [Getting Started](./doc/getting-started.md)
- [CLI Reference](./doc/cli.md)
- [Dynamic Lua](./doc/dynamic-lua.md)
- [Compatibility](./doc/compatibility.md)
- [Modules & C++ API](./doc/modules.md)
- [Migration Guide](./doc/migration-guide.md)
- [Benchmarks](./doc/benchmarks.md)

Start at the **[Documentation Index](./doc/index.md)**.

## License

**clx** is MIT Licensed — Copyright (c) 2026 Tine Samir
