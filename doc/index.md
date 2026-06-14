# clx Documentation

Welcome to the clx documentation. This folder contains comprehensive guides for understanding, using, and contributing to the clx Lua compiler.

## Getting Started

- **[Getting Started](./getting-started.md)** - Quick start guide, basic examples, and common patterns
- **[CLI Reference](./cli.md)** - Command-line interface documentation
- **[Lua 5.5 compatibility](./compatibility.md)** - Lua compatibility status
- **[Modules](./modules.md)** - Lua source modules, C++ native modules, and dynamic libraries
- **[Migration Guide](./migration-guide.md)** - Porting Lua C API modules to clx C++ API

## Deep Dives

- **[Architecture](./architecture.md)** - System architecture, components, and data flow
- **[Optimizations](./optimizations.md)** - Compile-time and runtime optimizations
- **[Benchmarks](./benchmarks.md)** - Benchmarks descriptions and results
- **[Runtime](./runtime.md)** - Runtime library implementation details

## Contributing

Contributions are welcome! Please ensure before any Pull Request :
1. Tests pass (`./tests/run.sh` or `tests\run.bat` on Windows)
2. Code follows existing coding style and uses C++20
3. Documentation (in `./doc` folder, in Markdown format) is updated for PR with new features

## License

clx MIT Licensed - Copyright (c) Tine Samir 2026