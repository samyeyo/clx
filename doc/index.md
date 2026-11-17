# clx Documentation

Welcome to the clx documentation. This folder collects guides for using and
extending clx, the Lua to native compiler.

## Getting started

- **[Installation](./install.md)** - Build from source, pre-built binaries, and `CLX_ARCH` target selection
- **[Getting Started](./getting-started.md)** - Your first program and a tour of the language
- **[CLI Reference](./cli.md)** - All the command-line options
- **[Lua modules](./modules.md)** - Splitting your project across files, plus native C++ modules
- **[Dynamic Lua](./dynamic-lua.md)** - Loading and running Lua at runtime with `--dynamic`
- **[Lua 5.5 compatibility](./compatibility.md)** - What's supported and what isn't
- **[Speed Tips](./speedtips.md)** - How to structure Lua so clx's optimizer keeps everything native
- **[Benchmarks](./benchmarks.md)** - How clx compares with other Lua runtimes

## For developers

- **[High Level Object Oriented C++ API](./hl-api.md)** - The value-semantic `clx.hpp` layer for native C++ code
- **[C++ API](./clx-api.md)** - The low-level `clx.h` layer: write native C++ modules
- **[Lua C API](./luacapi.md)** - Use the standard Lua 5.5 C API from C/C++ modules
- **[Migration Guide](./migration-guide.md)** - Port existing Lua C modules to the clx C++ API

## Internals

Want to know how clx works under the hood? See **[Internals](./internals/index.md)** — intended for contributors.

## Contributing

Contributions are welcome! Before submitting a pull request, please ensure:

1. Tests pass (`./tests/run.sh` or `tests\run.bat` on Windows), and the official
   Lua 5.5.1 suite passes too (`./tests/lua55/run.sh` or `tests\lua55\run.bat`)
2. Code follows the existing style and uses C++20
3. Documentation in `./doc` (Markdown) is updated for any new features

## License

clx is MIT Licensed - Copyright (c) Tine Samir 2026