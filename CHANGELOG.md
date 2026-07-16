# Changelog

All notable changes to this project will be documented in this file.

The format is loosely based on Keep a Changelog and the project follows Semantic Versioning where practical.

---

## [Work in progresss]

### Changed

* Introduce AnalysisState as a shared data structure between Optimizer and CodeEmitter (7190cd0)
* Updated CodeEmitter constructor to accept an AnalysisState reference (7190cd0)
* Updated Optimizer constructor to accept an AnalysisState reference (19cf523)
* Changed Optimizer::run to be non-static and updated its implementation (19cf523)
* Modified yields_number to take AnalysisState as a parameter (19cf523)
* Removed unused static variables from CodeEmitter related to optimization analysis (7190cd0)
* Integrated AnalysisState into the CLI driver for code generation (e91b04d)

### Added

* Added native int64_t generation support in CodeEmitter (7190cd0)
* Added new dedicated emit methods for each AST node type in CodeEmitter (7190cd0)
* Added analysis_state.h to centralize all optimizer analysis data (19cf523)

### Refactored

* Refactored CodeEmitter for better readability and code organization (7190cd0)
* Refactored Optimizer code to improve readability (19cf523)
* Improved pure integers detection in the optimizer (19cf523)

---

## [0.2.0] - 2026-07-08

### Changed

* Replaces NaN tagging based Value to shadow types memory with inline string support (55824b4)
* Added full support for 64 bits integers (55824b4)
* New numeric dispatch helpers (add/sub/mul/div with Int64 fast path) (55824b4)
* Updates CacheSlot for table inline caching (55824b4)
* shadow type Values migration (badb4d2)
* improved native arithmetic with int64_t code emission (badb4d2)
* improved CacheSlot implementation (badb4d2)
* faster function arguments implementation (badb4d2)
* C++ compiler is now the one used to build clx (4d87603)
* use std::from_chars() for integer parsing (fb2c741)
* Refactored LTable to use a new hash table implementation with improved memory management (5409c3b)
* Updated memory allocation and deallocation strategies for hash table entries (5409c3b)
* Removed clx_* prefix in C++ API for better clarity and consistency (5409c3b)
* Updated with new clx C++ API (b06f157)
* Updated codegen to use new clx C++ API (ec4df84)
* clx C++ API now support coroutine management functions (5409c3b)

### Added

* Enables assembler ARM64 context switching to replace the broken ucontext shim on Darwin ARM64 (55824b4)
* add ARM64 coroutine context switching via clx_coro_switch(), conditional on __APPLE__ && __aarch64__ (9a37ebd)
* coroutines implementation now in runtime.cpp (e1afaf1)
* added support for macOS ARM64 context switching (e1afaf1)
* add SIMD helpers for cross-platform performance optimization (b72701a)
* Add clx -o shorthand for --output flag (4d87603)
* Add coroutine.close() support and update documentation for coroutine functions (7a882e5)
* Add tests for coroutine.close() functionality (d3d6594)
* Add coroutine close functionality and refactor coroutine management using new clx C++ API (bc07f91)
* Implement read_line and read_all functions for improved file handling in io module (d8f6fc4)
* Add built-in function lookup and refactor intrinsic calls (44fcbb9)
* Introduced `lookup_builtin` function to map module and function names to C++ function names (44fcbb9)
* Updated `IntrinsicCall` structure to use C++ function names instead of integer identifiers (44fcbb9)
* Refactored intrinsic function parsing in the `Parser` to utilize a map for better readability and maintainability (44fcbb9)
* Enhanced the `yields_number` function to exclude specific intrinsic functions from number checks (44fcbb9)
* nil-comparison disqualification in function parameters (47e3d00)
* pure numeric array detection before disqualification (47e3d00)
* optimization for simple record tables and function parameter record arrays (47e3d00)
* stack-based traversal of function bodies detecting arithmetic on params, with call-chain propagation (47e3d00)
* adds param_names filter so function parameters don't leak into g_native_numbers (47e3d00)
* enhanced string handling functions to optimize for inline strings (4cb6c2e)
* introduced SIMD optimizations for strings (4cb6c2e)
* Add tests for coroutine.close functionality (d3d6594)
* Add executable permission to shell scripts (2c9f468)
* Add .gitattributes to enforce LF line endings (f008cf6)
* Add robots.txt to allow all user agents (7990e69)

### Fixed

* Remove CLX_MUSTTAIL macro definition for Clang compatibility (7a60bd2)
* remove CLX_MUSTTAIL macro definitions from code emitter (a2a56e6)
* enhance compiler detection, path searching, and linking for Apple platforms (7e84049)
* add _XOPEN_SOURCE definition for Apple builds (21dbcad)
* suppress deprecated declaration warnings for Apple builds (1f466fd)
* Fix function call in math_tointeger for consistency with naming conventions (e69d38b)
* Fix GitHub link in footer to correct URL (bcdb0a4)
* fix minifier: drop inline css/js minification (9809d19)

### Refactored

* Refactor table functions to improve consistency in version handling (94bc75e)
* Refactor string handling functions for improved consistency and performance (57c94dd)
* Refactor pack_require and luastd_package functions for improved readability and consistency (44d2742)
* Refactor time wrapper functions to remove 'clx_' prefix for consistency (7b527cf)
* Refactor function names to use 'lua_' prefix for consistency in base module (cf24988)
* Remove commented line in test_api function for clarity (a93e67b)
* Remove Bitwise Operations section from index.html (old implementation) (0b365b3)
* Remove bitwise operations section from runtime.md (old implementation) (0f813e7)

### Documentation

* updated with new clx ValueTypes aliases (26ef75b)
* updated for clx 0.2.0 (396f269 and 9ff2daa)
* fixed the command to run tests (4a20cae)
* Updated clx C++ API documentation and webpage (1d342bb)

### Build

* add ARM64 coroutine assembly for macOS and set defines for current used compiler (6cb22d8)
* update script permissions and add platform-specific linker flags (1f66c72)
* Use NMakefile generator on Windows (a2c60e2)
* Now static CRT is used for clx.exe when using MSVC, libs use dynamic CRT (2296a17)

### Benchmarks

* Benchmark batch script now uses only luajit for *_luajit.lua files (00503d0)
* Add --fast option in benchmarks/run.bat (00503d0)
* Increase default value of N in benchmarks for better consistency (0507629)
* Updated benchmarks result (5a51606)

### Website

* Publish website (51c5433)
* use python minifier instead of html-minifier-terser (ae5a711)
* version inline next to clx logo (5e7afe4)
* Fixed github link (a5f3ab0)

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
