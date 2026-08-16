# clx Documentation

Welcome to the clx documentation. This folder collects guides for using and
extending clx, the Lua to native compiler.

## Getting started

- **[Getting Started](./getting-started.md)** - Your first program and a tour of the language
- **[CLI Reference](./cli.md)** - All the command-line options
- **[Lua modules](./modules.md)** - Splitting your project across files, plus native C++ modules
- **[Dynamic Lua](./dynamic-lua.md)** - Loading and running Lua at runtime with `--dynamic`
- **[Lua 5.5 compatibility](./compatibility.md)** - What's supported and what isn't
- **[Benchmarks](./benchmarks.md)** - How clx compares with other Lua runtimes

## For developers

- **[C++ API](./api.md)** - Write native C++ modules
- **[Migration Guide](./migration-guide.md)** - Port existing Lua C modules to the clx C++ API

## Internals

Want to know how clx works under the hood? See **[Internals](./internals/index.md)** — intended for contributors.

## Contributing

Contributions are welcome! Before submitting a pull request, please ensure:

1. Tests pass (`./tests/run.sh` or `tests\run.bat` on Windows)
2. Code follows the existing style and uses C++20
3. Documentation in `./doc` (Markdown) is updated for any new features

## License

clx is MIT Licensed - Copyright (c) Tine Samir 2026