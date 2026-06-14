# Changelog

All notable changes to this project will be documented in this file.

The format is loosely based on Keep a Changelog and the project follows Semantic Versioning where practical.

---

## [0.1.0] - 2026-06-17

First public release.

### Added

* Ahead-of-time Lua 5.5 compiler
* Native executable generation
* Object file generation
* Static library generation
* Native C++ clx API
* Cross-platform support through Clang, GCC, and MSVC
* NaN-boxed value representation
* Inline string optimization
* Fast-path table access caches
* Lightweight AOT-oriented runtime
* Real life examples

### Lua Features

* Functions and closures
* Tables and metatables
* Coroutines
* Modules and package system
* Most standard libraries

### Tooling

* Benchmark suite
* Conformance tests
* Regression tests
* Edge-case tests
* Stress tests

### Examples

* Hello World
* Native clx Sokol module example
* Mandelbrot example
* Pong game using the clx Sokol module

### Documentation

* Architecture overview
* Runtime internals
* Optimization pipeline
* Module system and migration guide
* CLI reference
* Compatibility guide
* FAQ

### Known limitations

The following features are intentionally unsupported due to the AOT compilation model:

* load()
* loadfile()
* dofile()
* string.dump()
* debug library
