# Changelog

All notable changes to this project will be documented in this file.

The format is loosely based on Keep a Changelog and the project follows Semantic Versioning where practical.

---

## [0.3.0] - 2026-08-21

### Changed

* Shrunk LTable to 80 bytes using a lazy LTableExt indirection (6594386)
* Trimmed the `create_table` hot path (b732bcf)
* Restored table byte accounting and keep small arrays inline (97b90e9)
* Implemented a slow path for table lookup with metamethods to fix the MSVC performance issue (34c0e42)
* Maintain the metatable list in `setmetatable` (335c2c1)
* Added metatable-list links, a dynamic inline cache, permanent roots, and a VM-proxy flag (5e3805c)
* Fixed LTable accounting symmetry, retain array buffers, and protect only metatabled tables (6772dd8)
* Track coroutine wrapper tables in the metatable list (b1dbf8a)
* Hardened integer arithmetic (428afc2)
* Precompute hashes for compile-time string literals in table codegen, avoiding repeated runtime hashing (2546bc4)
* Optimized table constructor emission logic in CodeEmitter (59a92d8)
* Emit `as_number()` for known numeric string-key table fields (6ee409b)
* Rewrote numeric inference (parameters, returns, table fields, native integer tracking, function locals) in the optimizer (7cc3e98)
* Upgraded clx to version 0.3.0 (eac8b8f)
* Removed old per-function CacheSlot mechanism (114 lines net deletion) — replaced by per-LTable inline cache
* Per-LTable inline cache: 4 entries indexed by `key ^ (key >> 17) ^ (key >> 33) % 4`, stores `(key_payload, entry_idx, table_ver)`. Uses `hash_version` (structural changes only) not per-entry version (every write), so read-then-write patterns hit — 64 bytes overhead per LTable
* Simplified MultiValue to be trivially destructible: `inline_vals[3]` (was 8), `LState*` bump allocator for overflow (was `new[]/delete[]`), added 2-arg and 3-arg constructors, size reduced from 144 to 72 bytes
* Enabled `[[clang::musttail]]` for Clang alongside existing GCC `[[gnu::musttail]]`
* Removed ScopeGuard from generated function bodies — caller-side shadow_top save/restore instead; block/for-loop ScopeGuards kept for GC correctness
* Replaced `wyhash64` with `key ^ (key >> 17) ^ (key >> 33)` for inline cache indexing — cheaper hash function
* Coroutine resume() return values now use stack buffer (3 inline) with bump allocator overflow instead of std::vector heap allocation
* Removed `version` field from `HashEntry` — per-entry versioning replaced by table-level `hash_version`
* Code formatting applied with WebKit style via clang-format
* Introduce AnalysisState as a shared data structure between Optimizer and CodeEmitter (7190cd0)
* Updated CodeEmitter constructor to accept an AnalysisState reference (7190cd0)
* Updated Optimizer constructor to accept an AnalysisState reference (19cf523)
* Changed Optimizer::run to be non-static and updated its implementation (19cf523)
* Modified yields_number to take AnalysisState as a parameter (19cf523)
* Removed unused static variables from CodeEmitter related to optimization analysis (7190cd0)
* Integrated AnalysisState into the CLI driver for code generation (e91b04d)
* Wired Optimizer into CLI driver — each file now runs escape/numeric analysis before codegen (35f83d4)
* Arena tables fall back to heap allocation on growth instead of refusing arena (e949fd2)
* Bumped wyhash to 64-bit and grew baked string header to 16 bytes (f70ac4e)
* Reduced cs_max from 20 to 4 in AnalysisState for less memory overhead and faster code (14750ed)
* Replaced bind_all with set_lazy_funcs for lazy registration of coroutine, os, string, table, and utf8 modules (48cb301)
* Record known_table_lengths for tables created with fixed constructors; detect setmetatable, table.insert, and table.remove to populate tables_with_dynamic_length, preventing incorrect # constant folding on mutated tables (bbf80ea)
* Optimized string interning at startup: strings ≤6 bytes now use inline `LValue::istr()` instead of going through the StringPool, eliminating ~60-80% of pool operations (e155f2c)
* Long strings (>6 bytes) now use pre-computed slot positions — codegen simulates the hash table insertion at transpile time and emits a `PrecomputedEntry` array, allowing `bulk_fill_precomputed()` to write all slots in a single pass with zero linear probing (9b7e6eb)
* Fixed hash function mismatch for 7-byte strings: codegen now uses `swar_hash_8` for strings ≤8 bytes to match the runtime `intern_string()` threshold (e155f2c)
* Updated the base module for better Lua 5.5 compatibility (d4b52d3)
* Enhanced `io.read_line()` to optionally keep end-of-line characters and improved Lua 5.5 format matching for `read` operations (d72c6b7)
* Shrunk the default coroutine stack from 1MB to 256KB (1b80c38)
* Recycled dead coroutine threads+stacks — GC sweep keeps the thread pool (56feaf3)
* Reused pooled fibers on Windows via a trampoline loop and a pre_unwind hook (960890a)
* Optimized intrinsic math function mappings in the parser for ceil, floor, abs, fmod, and log (b4ca81d)
* Used Lua-compatible `clx_format_double()` in `LValue::to_string`, `tostring`, and concat paths (55ed64d, 7387b88, 526775f)
* Enhanced `LValue::to_string()` to support UserData values (bde87cb)
* Enhanced math functions for better integer handling and precision (737e69a)
* Emitted `%lld` for integer concat operands and `%.17g`+`.0` for float literals (4e62924)
* Stopped treating whole-valued floats as integers via the new `is_integer_typed_expr()` helper (bf311f1)
* Extended table arithmetic codegen detection to `t[k]=t[k]-N` and `t[k]=t[k]/N` patterns (2c5b4ed)
* Detected `t[k]=t[k]+N` patterns in codegen, emitted native `#`, and skipped LValue for known-numeric for-loop bounds (61a1b4d)
* Added table_increment/table_multiply and table_decrement/table_divide helpers for `t[k]=t[k]±N` and `t[k]=t[k]*/N` (762db3b, c220d1a)
* Used correct runtime type sizes for arena optimization instead of bulk sizes (9dd1883)
* Replaced get_binary_string with get_string and switched to the StringBuilder arena (7867dda)
* Added standalone inline fallbacks for the CLX_INLINE macros so builds work without forced inlining (80314d6)
* Stripped ".exe" from output names via replace_extension on Windows (a55c485)

### Added

* Added an embedded Lua 5.5 VM plus a runtime bridge, enabling `--dynamic`/`load()` execution (f8610bb, 5ea465e)
* Added a `--dynamic` proxy for the `clx_lua` library (c77bfae)
* Added VM proxy registration with the clx GC and pacing of the embedded VM's GC (`clx_register_vm_proxy`, `clx_free_vm_proxy_ptr`, `pace_vm_gc`) — VMTableProxy and VMFunction are now marked and registered (7db6d76, 2942a5d, 3247f9e, 77b580a, 50b5a58)
* Added standard io streams: `io.stdin`, `io.stderr`, and `io.stdout` (cb8ea45)
* Added faster coroutine context switching for x86_64 on Linux via `coro_switch_x86_64.s` (bd4faa1)
* Added SIMD runtime scans for nil/non-nil values (bab993b)
* Added `LState::alloc_overflow()` bump allocator for MultiValue overflow — no individual frees, reset at GC cycle
* Added `MultiValue(a, b)` and `MultiValue(a, b, c)` constructors for common 2/3-value returns without array allocation
* Added `file_line_prefix(L)` helper for consistent file:line error prefixes
* Added native int64_t generation support in CodeEmitter (7190cd0)
* Added new dedicated emit methods for each AST node type in CodeEmitter (7190cd0)
* Added analysis_state.h to centralize all optimizer analysis data (19cf523)
* Added `is_purely_integer_expr` shared function to analysis_state.h — single source of truth for integer detection, replacing 6 divergent inline lambdas in codegen and 1 in optimizer (ad72242)
* Added native int64 codegen for bitwise operations (BitAnd, BitOr, BitXor, Shl, Shr) and BNot when both operands are purely integer (ad72242)
* Added per-function memory arena allocator (FuncArena) to reduce GC pressure for short-lived tables (ad72242)
* Added escape analysis pass that classifies local tables as escaping (captured, returned, global-stored, function-arg, method-target, grown) vs arena-safe (e949fd2)
* Added CLX_ARENA_DEFAULT_FIELDS configurable define for arena table preallocation size (default 8) (e949fd2)
* Added arena analysis fields to AnalysisState (5950ccd)
* Added escape analysis and arena size computation to optimizer (6b3cef9)
* Added arena allocation for non-escaping tables in codegen (35f83d4)
* Added `_ENV` support for closures in LCFunction and LState (4d7cd48)
* Added `set_fenv()` and `get_fenv()` to manage a function's `_ENV` (209974b)
* Added `_ENV` support for closures in CodeEmitter (b586674)
* Enhanced `require()` to support custom `_ENV` as second parameter (56e2a0e)
* Added `StringPool::PrecomputedEntry` struct and `bulk_fill_precomputed()` method for zero-probing string pool initialization (9b7e6eb)
* Added AVX2 optimizations and new SIMD validation functions (9a2e0d7)
* Added hash_bitmap to LTable for efficient hash slot occupancy tracking (a9c65d1)
* Added bitmap-based `next()` hash-part iteration (7b527c8)
* Added mappings for pure numeric function parameters and node ownership in optimizer (45d4019)
* Added function parameter tracking as pure numeric in `yields_number` with scope validation (6305c91)
* Added Pass 4: detect function params used as integer-keyed numeric arrays; node_func_owner scope map (95ab663)
* Added AVX2 GC mark loop + protect_wl SIMD; bitmap alloc/update/free; bitmap-based GC hash scanning (cf62396)
* Added SIMD enhancements and fast paths for table concat, sort, and move (44a7697)
* Added StringArena for efficient string memory management in StringPool (f577737)
* Added `table_set_direct()` and `table_set_direct_cs()` for direct table writes bypassing `set_value()`'s redundant gettable check (3794233)
* Added `tables_with_dynamic_length` set and `known_table_lengths` map to AnalysisState (6924cb8)
* Added 3 new table optimizations in codegen (76a86d2)
* Added `StringPool::PrecomputedEntry` struct and `bulk_fill_precomputed()` for zero-probing startup interning (9b7e6eb)
* Added sets for integer-returning functions and typed locals in optimizer (40a966b)
* Added real-world compatibility tests (422a86d)
* Added tests for custom `_ENV` in child functions and in `require()` (c5f4def, 563f92a)
* Added `table.create()` to initialize tables with specified array and hash sizes (590e459)
* Added `coroutine.isyieldable()` and improved error handling in `coroutine_wrap` (dbc5d7a)
* Added float-to-integer conversion and numeric string parsing helpers; increased MultiValue INLINE_CAPS for `pairs()` (0a00883)
* Added a free_threads pool and growable ShadowStack to LState for coroutine recycling (3bb196d)
* Added fiber_started/pre_unwind flags and a fiber pool counter to LThread/LState (9e210de)

### Fixed

* Root the string metatable (6459b4e)
* Root io `default_input`/`default_output` userdata to prevent GC collection (29b7698)
* Replaced NaN representation with `std::numeric_limits` in `clx_to_vm_value_()` (4f36640)
* Fixed mask calculation in `clx_find_first_nil()` for SIMD operations (f0be578)
* Defined NOMINMAX for Windows and streamlined the CLX_MUSTTAIL definition (de02f22)
* Fixed global declaration issue (0be2e3c)
* Fixed temp directory path formatting when compiling multiple files on Windows (b2536d2)
* Fixed gcc error using CLX_INLINE_HOT for `table_get()` and `table_set()` (db60d96)
* Fixed `type_error` to extract actual type name from the argument at the given position (was hardcoded to "nil")
* Fixed duplicate `hash_count = 0` assignment in `create_table`
* Fixed redundant `!= Nil && == Function` checks in GC finalizer invocation (3 sites)
* Fixed `CLX_MUSTTAIL` was empty on Clang — now enabled via `[[clang::musttail]]`
* Fixed missing file/line in "attempt to call a nil value" error (7459b98)
* Fixed issues #17 and #18 with MSVC compiler (e6640c7)
* Fixed #16 by splitting `get_string`/`intern_string` to prevent uninitialized len in `utf8.codes()` (168ac0c)
* Fixed #15 by correcting the inverted SSE2 operands in `table_concat` type range check (1030340)
* Fixed annoying MSVC warning message D9025 when compiling clx_size.lib (d716fa1)
* Fixed GC quadratic hash scan, CacheSlot stability, sub-alloc tracking, double-free (5a1f442)
* Fixed CacheSlot reads for numeric fields and int64 inner loops for `j=i+1` (e89a8e9)
* Fixed `tonumber` returning nil for integer values (#28) (ca309e2)
* Improved error handling in `next()` for invalid keys (f0c20a1)
* Fixed `next()` transitions to the hash part after the array pivot key (2a1ccbf)
* Fixed hex integer literal parsing at base 16 (6dddb1d)
* Fixed floor-modulo and UB-unsafe int64 arithmetic in the fast path (4cfa559)
* Fixed a function recursion crash in emitted code (244d540)
* Improved LValue::eq() equality checks to handle non-Double types (7c1e103)
* Avoided zero divisor by substituting infinity on MSVC (8a2a822)
* Added UTF-8 support in MSVC optimization flags and BOM handling in source files (af03628)
* Moved the fiber declaration under a conditional so POSIX builds compile (c247aa8)

### Tests

* Added a VM proxy RSS leak regression test (4c74675)
* Added a table sweep stress regression test (586a7df)
* Relaxed the gc.lua pool-reuse assertion to non-expansion (cf4d9b8)
* Excluded load.lua from Lua file processing in run.bat (cdd7d6a)
* Added VM boundaries tests — coroutine boundary, load()-aware assertions, and VM/clx incompatibility checks (8a4829f)
* Added a test suite runner for dynamic loading of Lua code (a0ba7c9)
* Added real-world compatibility tests for Lua scripts (c76153b)
* Corrected the case of the Lua version string in an assertion (6a63d0e)

### Refactored

* Extracted `LState::invoke_gc_finalizer()` helper — replaces 3 copy-pasted __gc finalizer invocation patterns
* Extracted `file_read_args()`, `file_write_args()`, `make_lines_iter()` in io.cpp — replaces duplicated lambdas between `make_file` and `get_std_file`
* Removed dead code: `get_binary_string` (pass-through to `get_string`), `visitor.cpp`/`visitor.h` (unused traverse module)
* Removed all non-header, non-section-divider single-line comments from `src/` and `include/` files
* Removed unused includes: `#include "../codegen/codegen.h"` from runtime.cpp, `#include "../syntax/parser.h"` from optimizer.h, `#include <functional>` and `#include <unordered_map>` from codegen.h
* Added `call_table_op` template in codegen for table_increment/decrement/multiply/divide — eliminates 4 structural clone functions
* Extracted shared `emit_table_op` helper in codegen — eliminates duplicated table-op optimization in last_is_call vs generic paths
* Added `file_line_prefix(L)` inline helper in clx_runtime.h — eliminates duplicate string construction in base.cpp and runtime.cpp
* Refactored CodeEmitter for better readability and code organization (7190cd0)
* Refactored Optimizer code to improve readability (19cf523)
* Improved pure integers detection in the optimizer (19cf523)
* Consolidated 6 duplicated integer-detection lambdas in codegen into calls to the shared `is_purely_integer_expr` function (ad72242)
* Replaced optimizer's incomplete `is_int_expr` lambda with the shared function, enabling detection of Mod, And, Or, FloorDiv, ParenExpression, UnaryOp(Minus) as integer-producing (ad72242)
* Marked several functions as `CLX_INLINE_HOT` for performance improvements (6c84a86, fa67b2f)
* Replaced hardcoded minimum fields with `CLX_ARENA_DEFAULT_FIELDS` constant for consistency (247d16b)
* Enhanced for-loop and while-loop detection in optimizer (62fdd9a)
* Enhanced vector path qualification by disqualifying non-simple keys in array optimizations (37c49e7)
* Reverted fib.lua to tree-recursive (O(2^n)) for benchmarking — tail-recursive version is O(n) and too trivial to show CLX speedup
* Rewrote coro.lua benchmark to 5M resume/yield cycles for meaningful comparison
* Cleaned up helper comments in runtime code (08d8000)
* Simplified error handling by using `clx::check_table()` (d89a7e8)
* Switched string functions, `read_all()`, and `make_file()` to `make_string_pooled()` for pooled memory management (c523b46, a18b04f)
* Replaced direct table field setting with `clx::set_field()` for consistency (881dbf2)

### Documentation

* Corrected spelling (86b5c03)
* Updated installation instructions and enhanced runtime documentation (3fffdae)
* Added installation path functions and updated library root logic for improved module loading (788342c)
* Updated runtime documentation with details on string allocation, inline cache structure, and performance optimizations (ff90a6a)
* Enhanced optimizations documentation — native integer tracking, table memory layout, and runtime optimizations (5b78a07)
* Updated modules documentation with search paths for native modules (ef25def, 1c50b6e)
* Added dynamic Lua VM documentation and corrected the index link (e784c30)
* Updated the README with improved build instructions and output details (ec70a2e)
* Fixed a typo in dynamic Lua documentation references (6df7da5)
* Documented the `--dynamic` compiler option and the dynamic Lua feature (c99009b, 6d324dd, b7dc422, 6313cdd)
* Fixed a typo in LICENSE (ba841cf)
* Updated the website and documentation for API, architecture, benchmarks, CLI, and optimizations (f10535e, 0388524, 5d0ada9)
* Refactored the documentation for better readability for end users (139b121)
* Removed example projects from the README (221deaf)
* Fixed the repository URL and updated feature descriptions in the README (383fb7c)

### Build

* Installed `clx_simd.h` alongside the other public headers (9f0df50)
* Derived `--version` from the CMake project version (c838d99)
* Improved the artifact creation process for Linux and Windows (e48fd7d)
* Enhanced the uninstall process and updated default install paths for Windows (6d2ed0d)
* Added Lua runtime fetch scripts and embedded Lua VM CMake files (8a6ef1e, f03de7a)

### Benchmarks

* Updated default N values in fannkuchredux and fasta benchmarks (78d8a7e)
* Updated performance metrics for the docs and web page (49c5276)
* Skip run_load_shim.lua in the benchmark scripts and the load() benchmark loop (5b7c756, 18b75b4)
* Increased benchmark parameters for more meaningful times (934efc4)
* Enhanced output formatting and added load()-specific benchmarking scripts, including on Windows (cfcbf70, c3b1920, ccff011)
* Added a skynet coroutine benchmark (44c00d5)
* Updated benchmark results (da0f7f0, c7d34e3)
* Removed a leftover debug script (61577de)

### Examples

* Added the clx examples directory (1c77cad)
* Improved Mandelbrot rendering and zoom functionality (2433588)

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
