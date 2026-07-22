# clx Architecture

## Overview

clx is a Lua-to-C++ compiler that transpiles Lua source code into optimized C++ code, which is then compiled using the system's C++ compiler to produce native machine code. This architecture provides performance near (most of the times faster) than standard Lua 5.5 interpreter.

## System Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                     clx Compiler                             │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────┐     ┌──────────┐    ┌────────────┐              │
│  │  CLI    │───▶ │  Parser  │───▶│   AST      │              │
│  │         │     │          │    │            │              │
│  └─────────┘     └──────────┘    └─────┬──────┘              │
│                                       │                      │
│                                       ▼                      │
│                                ┌────────────┐                │
│                                │ Optimizer  │                │
│                                │            │                │
│                                └─────┬──────┘                │
│                                      │                       │
│                                      ▼                       │
│                                ┌────────────┐                │
│                                │  Codegen   │                │
│                                │            │                │
│                                └─────┬──────┘                │
│                                      │                       │
│                                      ▼                       │
│                                ┌────────────┐                │
│                                │     C++    │                │
│                                │   Source   │                │
│                                └─────┬──────┘                │
│                                      │                       │
└──────────────────────────────────────│───────────────────────┘
                                       │
                   ┌───────────────────┴───────────────────────┐
                   │                                           │
                   ▼                                           ▼
          ┌──────────────────┐                     ┌───────────────────────┐
          │   C++ Compiler   │                     │      Runtime Lib      │
          │ (gcc/clang/msvc) │                     │  (libclx / clx.lib)   │
          └────────┬─────────┘                     └───────────────────────┘
                   │
                   ▼
          ┌──────────────────┐
          │  Native Binary   │
          └──────────────────┘
```

## Project layout

```
clx/
├── CMakeLists.txt              # Build configuration
├── build.sh / build.bat        # Convenience build scripts
├── include/
│   ├── clx.h                   # Public C++ API (value ctors, type queries, helpers)
│   ├── clx_runtime.h           # Internal runtime (types, GC, tables, inline ops)
│   └── clx_simd.h              # Cross-platform SIMD helpers for type-array scans
├── src/
│   ├── clx.cpp                  # Compiler driver / CLI
│   ├── syntax/
│   │   ├── lexer.h/cpp          # Tokenizer/scanner
│   │   ├── parser.h/cpp         # Recursive descent parser
│   │   └── nodes.h              # AST node definitions
│   ├── optimizer/
│   │   ├── optimizer.h           # Optimization passes
│   │   └── optimizer.cpp         # Optimization implementation
│   ├── codegen/
│   │   ├── codegen.h             # Code generator interface
│   │   └── codegen.cpp           # C++ code emission (calls optimizer internally)
│   └── runtime/                  # Runtime library (libclx.a)
│       ├── runtime.cpp           # VM core (GC, types, state, metamethods)
│       ├── base.cpp              # Base module (print, error, type, pcall, etc.)
│       ├── table.cpp             # Table module (insert, remove, concat, sort, etc.)
│       ├── math.cpp              # Math module
│       ├── strings.cpp           # String module
│       ├── coroutine.cpp         # Coroutine module
│       ├── io.cpp                # I/O module
│       ├── os.cpp                # OS module
│       ├── utf8.cpp              # UTF-8 module
│       ├── package.cpp           # Package/module system
│       └── openlibs.cpp          # Standard modules loader
├── tests/                       # End-to-end test suite
├── examples/                    # Example clx projects using the C++ embedding API
│   ├── mandelbrot/              # Mandelbrot viewer
│   ├── pong/                    # Pong game
│   └── sokol/                   # Sokol graphics module for clx
├── benchmarks/                  # Performance benchmarks with comparisons (lua / luajit / clx)
└── doc/                         # Comprehensive documentation
    ├── architecture.md
    ├── api.md
    ├── cli.md
    ├── optimizations.md
    ├── runtime.md
    ├── modules.md
    ├── benchmarks.md
    ├── getting-started.md
    └── migration-guide.md
```

## Components

### 1. CLI (src/clx.cpp)

The command-line interface handles:
- Argument parsing (`--executable`, `--object`, `--static`, `--debug`, `--size`, `--fast`, `--cpp`, `--modules`, `--output`)
- File I/O and multiple lua files compilation
- Invoking the C++ compiler (fixed at build time via CMake)
- Output file management and temp file cleanup
- Dead code elimination by default via `-ffunction-sections -Wl,--gc-sections` (gcc/clang) or `/Gy /link /OPT:REF /OPT:ICF` (MSVC)
- Default optimization flags if none provided : `-O3 -flto=auto -fno-rtti -fvisibility=hidden` (gcc/clang) or `/O2 /Ot /GL /GR- /MD /EHsc /GS- /fp:fast /Gw /Gy` (MSVC)

### 2. Lexer (src/syntax/lexer.cpp)

The tokenizer converts raw source code into a stream of tokens:
- Keywords (`if`, `while`, `function`, etc.)
- Identifiers
- Literals (numbers, strings)
- Operators (+, -, *, /, etc.)
- Delimiters (parentheses, braces, brackets)

### 3. Parser (src/syntax/parser.cpp)

The recursive descent parser builds an Abstract Syntax Tree (AST):
- `parse_statement()` - parses statements
- `parse_expression()` - parses expressions
- `parse_block()` - parses statement blocks
- `parse_function()` - parses function definitions

### 4. AST Nodes (src/syntax/nodes.h)

Core AST node types:
- `Block` - statement block
- `Identifier` - variable reference
- `BinaryOp` / `UnaryOp` - operations
- `FunctionDef` - function definition
- `TableConstructor` - table literals
- `ForStatement` / `WhileStatement` - loops
- `IfStatement` - conditional
- `CallExpression` - function calls

### 5. Optimizer (src/optimizer/optimizer.cpp)

Optimization passes analyze the AST and annotate nodes with optimization hints:
- Numeric expression detection (Integer and Number fast paths)
- Variable scope resolution
- Table access purity analysis
- Constant folding preparation
- Table version tracking (`hash_version`/`array_version`) for inline cache invalidation
- `yields_number` analysis for numeric for loops
- Non-fast function parameter numeric detection (marks params used in arithmetic as native doubles)
- Function parameter numeric-record array inference (traces `local bi = bodies[i]` + field accesses to prove numeric fields)

### 6. Code Generator (src/codegen/codegen.cpp)

The code generator produces C++ code:
- Fast-path code for numeric operations (Integer and Number)
- Slow-path code for dynamic operations
- Loop transformation (numeric for, generic for with direct `LCFunction` pointer calls)
- Expression emission with `[[likely]]` branch prediction hints
- Per-LTable `InlineCache` (4 entries) for string-keyed table access, checked inside `LTable::gettable()`/`LTable::settable()`
- StringBuilder-based string concatenation
- Wyhash-based string hashing

### 7. Runtime Library (src/runtime/)

The runtime library implements Lua semantics:
 - `runtime.cpp` - Core runtime (GC, tables, metamethods, arithmetic, bitwise ops, lazy function registration via `set_lazy_funcs`)
 - `base.cpp` - Base library (print, error, type, rawequal, rawget, rawset, rawlen, warn, _VERSION)
 - `table.cpp` - Table library (insert, remove, concat, sort, unpack, pack, move)
 - `math.cpp` - Math library
 - `strings.cpp` - String library (len, sub, reverse, lower, upper, rep, byte, char, format, find, match, gmatch, gsub)
 - `coroutine.cpp` - Coroutine support (OS-level fibers; hand-written ARM64 assembly on macOS Apple Silicon replacing buggy ucontext)
 - `io.cpp` - I/O library
 - `os.cpp` - OS library (clock, time, date, difftime, execute, tmpname, getenv)
 - `utf8.cpp` - UTF-8 library
 - `package.cpp` - Package/module system

## Data Flow

### Key Runtime Components

#### StringPool

Open-addressed hash map for string interning. Each slot owns a baked allocation:
`[uint32_t hash][uint32_t len][char data...\0]`. LValue stores a pointer to the char data
(8 bytes past alloc start). One probe on hit, no `std::string`, no side map, no double lookup.
Supports `intern_preallocated()` for zero-allocation string concatenation.

#### wyhash

Fast, high-quality hash function used for table keys and string interning. Uses compile-time
constant secrets with 128-bit multiply for excellent avalanche. For interned strings, the hash
is baked into the allocation header, making `lvalue_hash()` a single 4-byte load.

For strings ≤8 bytes, `swar_hash_8()` replaces `wyhash_str` — loads all bytes into one register
with a single `memcpy` and mixes via `wyhash64`. Used consistently for both TAG_ISTR inline
strings and short interned strings so cross-type hash compatibility is maintained.

#### Per-LTable Inline Cache

Each `LTable` embeds a small fixed-size cache (`InlineCache ic[4]`) that accelerates repeated
string-keyed reads. Each entry caches the key payload, entry index, and the table's
`hash_version` at the time of the last successful probe. On the next read, the cache is checked
first — if key and version match, the cached entry index is used directly, skipping the full
hash-probe path. Cache invalidation is structural: `hash_version` increments only on inserts,
deletes, or rehashes, not on value updates, so read-then-write patterns still hit the cache.
Only non-GC value types are cached to avoid dangling pointers after collection.

#### StringBuilder

O(n) string concatenation that avoids the O(n²) quadratic blow-up of repeated `s = s .. part`
patterns. Uses inline storage for up to 8 parts, grows to heap allocation when needed. Produces
a single interned string with baked hash on `to_string()`.

#### Table Version Tracking

Tables track `hash_version` and `array_version` that increment on structural changes (inserts,
deletes, rehashes, array resizing). The per-LTable inline cache checks `hash_version` to detect
stale entries after table mutations. Value updates do not bump the version, allowing efficient
read-then-write patterns.

## Data Flow

### Compilation Process

1. **Input**: Lua source file
2. **Lexing**: Source → Token stream
3. **Parsing**: Token stream → AST
4. **Optimization**: AST → Annotated AST
5. **Codegen**: Annotated AST → C++ source
6. **Compilation**: C++ source → Native binary

### Runtime Process

1. **Initialization**: Create LState, load standard libraries
2. **Execution**: Run compiled code using native arithmetic where possible
3. **Fallback**: Use Lua value representation when types are unknown
4. **Cleanup**: Garbage collection of unused objects

## Memory Management

### Value Representation

clx uses a 16-byte `LValue` (8-byte payload + separate `ValueType` tag) to store Lua values:
- Numbers: Direct double representation (IEEE 754)
- Integers: 64-bit signed integer in TValue payload
 - Strings: Either TAG_ISTR inline (≤6 bytes, stored directly in `val`) or pointer to heap-allocated interned string (via StringPool)
- Tables: Pointer to heap-allocated table
- Functions: Pointer to function closure
- Nil/Boolean: Special sentinel values
- Threads: Pointer to LThread (coroutine/fiber)
- Userdata: Pointer to user-defined data

### Garbage Collection

Stop-the-world mark-and-sweep collector:
- **Mark phase**: Traverse reachable objects from roots via worklist
- **Sweep phase**: Deallocate unreachable objects, recycle freed LTable/LCFunction nodes into free lists