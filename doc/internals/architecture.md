# clx Architecture

## Overview

clx compiles Lua to C++, then compiles that C++ with your system compiler into a
native program. There is no interpreter in the normal path — that is what AOT
(ahead-of-time) means here.

An optional `--dynamic` mode additionally embeds a real Lua VM, so the program can
also `load` and run Lua source at runtime. The two runtimes stay separate (different
values, heaps, collectors, coroutine systems); a small bridge copies primitives and
wraps tables, userdata, and functions across the boundary.

## System Architecture

The following figure shows both paths. The compiler pipeline is shared; `--dynamic` changes the final link and generated runtime registration, not the compilation of the input source file.

```
                         Lua source file
                                │
                                ▼
┌──────────────────────────────────────────────────────────┐
│                      clx compiler                        │
│  CLI → Lexer → Parser → AST → Optimizer → Code generator │
│                                             │             │
│                                             ▼             │
│                                       generated C++       │
└─────────────────────────────────────────┬────────────────┘
                                          │
                                          ▼
                                  system C++ compiler
                                          │
                 ┌────────────────────────┴────────────────────────┐
                 │                                                 │
              normal                                             --dynamic
                 │                                                 │
                 ▼                                                 ▼
       ┌────────────────────┐                         ┌────────────────────┐
       │ libclx / clx.lib   │                         │ libclx / clx.lib   │
       │ clx AOT runtime    │                         │ clx AOT runtime    │
       └─────────┬──────────┘                         └─────────┬──────────┘
                 │                                             │
                 │                                  libclx_lua.a / clx_lua.lib
                 │                                  embedded Lua VM + bridge
                 │                                             │
                 └──────────────────┬──────────────────────────┘
                                    ▼
                         generated native executable

       AOT code ───────────────▶ clx runtime
          │                          │
          │ load/loadfile/dofile     │ --dynamic bridge calls
          │                          ▼
          └──────────────────▶ embedded Lua VM
                               VM libraries, heap,
                               GC, and coroutines
```

In a normal build, no embedded VM archive is linked and the dynamic loader functions are not registered. In a `--dynamic` build, the generated program creates a `DynamicVM` lazily when a loader or bridge operation first needs it. Runtime-loaded chunks execute in that VM, while the original clx program continues to execute as native code.

## Project layout

```
clx/
├── CMakeLists.txt              # Build configuration
├── build.sh / build.bat        # Convenience build scripts
├── include/
│   ├── clx.h                   # Public C++ API (value ctors, type queries, helpers)
│   ├── clx_runtime.h           # Internal runtime (types, GC, tables, inline ops)
│   ├── clx_simd.h              # Cross-platform SIMD helpers for type-array scans
│   ├── lua.h / lauxlib.h / lualib.h  # Lua 5.5 C API headers (for --modules C modules)
├── src/
│   ├── clx.cpp                  # Compiler driver / CLI
│   ├── syntax/
│   │   ├── lexer.h/cpp          # Tokenizer/scanner
│   │   ├── parser.h/cpp         # Recursive descent parser
│   │   └── nodes.h              # AST node definitions
│   ├── optimizer/
│   │   ├── optimizer.h/cpp      # Optimizer driver (thin orchestrator over the passes)
│   │   ├── passes.h             # PassScratch + pass declarations, traverse_node
│   │   ├── analysis_state.h     # AnalysisState shared across passes
│   │   ├── owners.cpp           # Ownership, goto/labels, name scans, hygiene
│   │   ├── numbers.cpp          # Constants/bounds, numeric params, int returns, nogc locals
│   │   ├── functions.cpp        # Callables, multi-native tuples
│   │   ├── tables.cpp           # Pure arrays, array safety, table fields, arenas
│   │   └── structs.cpp          # Struct-local optimization
│   ├── codegen/
│   │   ├── codegen.h/cpp        # Code generator driver (calls optimizer internally)
│   │   ├── helpers.h/cpp        # Shared free helpers for emission
│   │   ├── expressions.cpp      # Native ops, calls, literals, table access
│   │   ├── statements.cpp       # Blocks, functions, returns, control flow
│   │   ├── loops.cpp            # for/generic-for, loop-table hoists
│   │   └── assignments.cpp      # Assignments, table constructors, snapshots
│   └── runtime/                  # Runtime library (libclx.a)
│       ├── state.cpp             # LState lifecycle, globals, open/close
│       ├── values.cpp            # LValue conversions, comparisons, metamethods
│       ├── tables.cpp            # LTable core (array/hash, get/set, inline cache)
│       ├── threads.cpp           # LThread fibers, root_stash/root_load, coro switching
│       ├── gc.cpp                # Generational GC (mark/sweep, barriers, finalizers)
│       ├── calls.cpp             # call_function / pcall / call_direct, lazy registry
│       ├── internal.h            # Shared internal declarations
│       ├── base.cpp              # Base module (print, error, type, pcall, etc.)
│       ├── table.cpp             # Table module (insert, remove, concat, sort, etc.)
│       ├── math.cpp              # Math module
│       ├── strings.cpp           # String module
│       ├── coroutine.cpp         # Coroutine module
│       ├── io.cpp                # I/O module
│       ├── os.cpp                # OS module
│       ├── utf8.cpp              # UTF-8 module
│       ├── debug.cpp             # Debug module (metatables, uservalues, partial traceback)
│       ├── package.cpp           # AOT package/module system
│       ├── fulllibs.cpp          # openlibs() — opens the std modules
│       ├── openlibs.cpp          # AOT standard modules loader helpers
│       ├── luaapi.cpp            # Lua C API compatibility shim (libclx_capi*.a)
│       ├── coro_switch_aarch64.S # ARM64 fiber switch (Apple + Linux)
│       ├── coro_switch_x86_64.s  # x86_64 fiber switch (Linux)
│       └── vm/                   # Optional embedded Lua VM and bridge
│           ├── dynamic_vm.cpp    # VM lifetime, environment, and proxy roots
│           ├── load_builtin.cpp  # load/loadfile/dofile registration
│           ├── vm_convert.cpp    # clx ↔ VM value conversion
│           ├── vm_function_object.cpp # VM function wrapper
│           ├── vm_native_bridge.cpp   # VM-to-clx callable dispatch
│           └── lua/              # Vendored Lua VM sources
├── tests/                       # End-to-end test suite
├── examples/                    # Example clx projects using the C++ embedding API
│   ├── hello/                   # Minimal hello-world
│   ├── mandelbrot/              # Mandelbrot viewer
│   ├── pong/                    # Pong game
│   ├── sokol/                   # Sokol graphics module for clx
│   └── weather/                 # Weather app using a Lua C module
├── benchmarks/                  # Performance benchmarks with comparisons (lua / luajit / clx)
└── doc/                         # Comprehensive documentation
    ├── index.md
    ├── clx-api.md
    ├── cli.md
    ├── modules.md
    ├── luacapi.md
    ├── benchmarks.md
    ├── getting-started.md
    ├── install.md
    ├── compatibility.md
    ├── dynamic-lua.md
    ├── migration-guide.md
    └── internals/               # Internal design docs
        ├── architecture.md
        ├── optimizations.md
        └── runtime.md
```

## Components

### 1. CLI (src/clx.cpp)

The driver parses flags, compiles each `.lua` file to C++, invokes the system C++
compiler, and links the result. The C++ toolchain is fixed when clx itself is built
(CMake records it), so every user gets the same codegen. It also finds clx's own
runtime libraries and headers (in-tree `build/` dir first, then the install prefix),
manages temp files, and strips dead code by default (function sections + linker GC).

Build profiles: `--size` (default, small binaries), `--fast` (optimized, with LTO
outside Windows), `--debug` (debug info, no optimization). See
[Optimizations](./optimizations.md#optimization-levels) for the exact flags.

### 2. Lexer (src/syntax/lexer.cpp)

Turns raw source into a token stream (keywords, names, numbers, strings, operators).
String escapes (`\n`, `\x41`, …) are decoded here, once, with `file:line` errors —
by the time codegen runs, string contents are final.

### 3. Parser (src/syntax/parser.cpp)

A hand-written recursive-descent parser that builds the AST (`parse_statement`,
`parse_expression`, `parse_block`, `parse_funcbody`, …). It also resolves every name
to global / local / captured while parsing, so later passes never guess at scoping.

### 4. AST Nodes (src/syntax/nodes.h)

Plain structs, one per Lua construct: `Block`, `Identifier`, `BinaryOp`/`UnaryOp`,
`FunctionDef`, `TableConstructor`, `ForStatement`, `CallExpression`, and so on.
If you add a Lua feature, you add a node here first.

### 5. Optimizer (src/optimizer/)

`Optimizer::run()` is a thin orchestrator over 15 small passes (declared in
`passes.h`, grouped by topic into `owners.cpp`, `numbers.cpp`, `functions.cpp`,
`tables.cpp`, `structs.cpp`). Passes share scratch state via `PassScratch` /
`AnalysisState` and only ever *annotate* — they record facts like "this variable is
numeric" or "this table is only indexed with integers". Codegen decides what to do
with those facts. The two questions every pass answers are: *what type does this hold?*
and *can this value escape?* (Rule of thumb: `hash_version` is *runtime* table state,
not an optimizer fact — don't mix the two.)

### 6. Code Generator (src/codegen/)

Walks the annotated AST and prints C++ (one file per contributor topic:
`expressions.cpp`, `statements.cpp`, `loops.cpp`, `assignments.cpp`). For every
operation it picks one of two shapes: the *native* shape (plain C++ on `double` /
`int64_t`, used when the optimizer proved the types) or the *generic* shape
(`clx::add(L, …)` and friends, which check types at runtime). When in doubt it picks
generic — the failure mode is slower code, never wrong code.

### 7. Runtime Library (src/runtime/)

The runtime library implements Lua semantics:
 - `state.cpp` - LState lifecycle, slab allocation, globals, open/close
 - `values.cpp` - LValue conversions/comparisons, metamethod dispatch, arithmetic
 - `tables.cpp` - LTable core (array/hash storage, get/set, inline cache)
 - `threads.cpp` - LThread fibers, root_stash/root_load, coro switching
 - `gc.cpp` - Generational GC (mark/sweep, barriers, finalizers, GCStack)
 - `calls.cpp` - call_function/pcall/call_direct, lazy function registry, `set_lazy_funcs`
 - `base.cpp` - Base library (print, error, type, rawequal, rawget, rawset, rawlen, warn, _VERSION)
 - `table.cpp` - Table library (insert, remove, concat, sort, unpack, pack, move)
 - `math.cpp` - Math library
 - `strings.cpp` - String library (len, sub, reverse, lower, upper, rep, byte, char, format, find, match, gmatch, gsub)
 - `coroutine.cpp` - Coroutine support (OS-level fibers; hand-written assembly on ARM64 for Apple+Linux and x86_64 Linux, ucontext fallback elsewhere)
 - `io.cpp` - I/O library
 - `os.cpp` - OS library (clock, time, date, difftime, execute, tmpname, getenv)
 - `utf8.cpp` - UTF-8 library
 - `debug.cpp` - Debug library (metatables for every type, uservalues and a frame-less `traceback`; the frame-level entries raise on compiled closures and are answered by the VM hooks for loaded chunks, while the upvalue entries walk internal capture cells and `getregistry` returns an empty table)
 - `package.cpp` - AOT package/module system
 - `luaapi.cpp` - Lua C API compatibility shim (built as `libclx_capi*.a`)

The optional dynamic path is kept separate from these AOT modules:

- `runtime/vm/lua/` - Vendored Lua VM sources and its standard libraries
- `runtime/vm/dynamic_vm.cpp` - Creates and owns one embedded VM per clx `LState`, including its VM environment and proxy roots. The default environment is the VM's own globals table (`_ENV`/`_G` are itself) with a metatable whose `__index` falls through to clx's globals and whose `__newindex` mirrors writes, so both sides see one set of names
- `runtime/vm/load_builtin.cpp` - Registers `load`, `loadfile`, and `dofile` when the generated program was linked with `--dynamic`, and installs the `clx_vm_debug_getinfo_hook` / `clx_vm_debug_upvalue_hook` bridges used by the `debug` module
- `runtime/vm/vm_convert.cpp` - Converts primitive values and creates proxies for supported clx tables and userdata
- `runtime/vm/vm_function_object.cpp` - Represents a VM function as a callable clx value
- `runtime/vm/vm_native_bridge.cpp` - Exposes clx callables to the VM and dispatches calls back into the AOT runtime
- `runtime/vm/vm_table_proxy.cpp` - Implements VM-side access to proxied clx tables

`--dynamic` does not replace clx's AOT `string`, `math`, `table`, `io`, `os`, `coroutine`, or `package` modules. Dynamically loaded chunks use the embedded VM's own libraries. `require` is the one shared seam: a chunk's `require` shim (`clx_vm_require` in `vm_convert.cpp`) answers from the VM's own `package.loaded` when it can — so `require "debug"` reaches the VM's complete library — and otherwise calls clx's `require` against the shared `package.path` / `package.loaded`. Coroutine systems remain runtime-local.

## Data Flow

### Key Runtime Components

#### StringPool / StringArena

Open-addressed hash map for string interning, backed by a block-based `StringArena` (64 KB
blocks, bump-allocated). Each slot owns a baked allocation with a **16-byte header**
`[uint32 hash_low @0][pad @4][uint32 len @8][pad @12][char data...\0 @16]`. LValue stores a
pointer to the char data (16 bytes past alloc start). One probe on hit, no `std::string`, no
side map, no double lookup. Supports `intern_preallocated()` for zero-allocation string
concatenation.

#### wyhash

Fast, high-quality 64-bit hash function used for table keys and string interning. Uses
compile-time constant secrets with 128-bit multiply (`clx_umul128`) for excellent avalanche.
For interned strings, the low 32 bits of the hash are baked into the 16-byte allocation
header, making `lvalue_hash()` a single 4-byte load.

For strings ≤8 bytes, `swar_hash_8()` replaces `wyhash_str` — loads all bytes into one register
with a single `memcpy` and mixes via `wyhash64`. Used consistently for both TAG_ISTR inline
strings and short interned strings so cross-type hash compatibility is maintained.

#### Per-LTable Inline Cache

Each `LTable`'s lazily-allocated `LTableExt` holds an 8-entry inline cache that accelerates
repeated string-keyed reads. Each entry caches the key payload, entry index, and the table's
`hash_version` at the time of the last successful probe — no values are stored, so there is
nothing to dangle after collection. On the next read, the cache is checked first — if key and
version match, the cached entry index is used directly, skipping the full hash-probe path.
Cache invalidation is structural: `hash_version` bumps only on hash **rehash** (the sole
invalidator that relocates entries), plus metatable set and table recycle — plain inserts and
deletes never move entries and intentionally leave the version alone, and value updates never
bump it, so read-then-write patterns still hit the cache.

The cache lives in `LTableExt` (lazily allocated), so pure array-part tables never pay for it.
The hot `table_get` fast path (array indexing + inline cache) stays inline; misses fall through
to an out-of-line `table_get_slow` for the hash probe and `__index` dispatch.

#### StringBuilder

O(n) string concatenation that avoids the O(n²) quadratic blow-up of repeated `s = s .. part`
patterns. Uses inline storage for up to 8 parts, grows to heap allocation when needed. Produces
a single interned string with baked hash on `to_string()`.

#### Table Version Tracking

Tables track `hash_version`, which bumps on hash **rehash** (relocating entries), metatable
set, and table recycle. The per-LTable inline cache checks `hash_version` to detect stale
entries after those structural changes. Plain inserts/deletes and value updates do not bump
the version, allowing efficient read-then-write patterns.

## Data Flow

### Compilation Process

1. **Input**: Lua source file
2. **Lexing**: Source → Token stream
3. **Parsing**: Token stream → AST
4. **Optimization**: AST → Annotated AST
5. **Codegen**: Annotated AST → C++ source
6. **Compilation**: C++ source → Native binary

### Runtime Process

1. **Initialization**: Create the clx `LState` and register the AOT standard libraries
2. **Execution**: Run compiled code using native arithmetic where possible
3. **Fallback**: Use the clx `LValue` representation when types are unknown
4. **Cleanup**: Garbage collection of unused clx objects

### Dynamic Runtime Process (`--dynamic`)

1. **Linking**: Add the optional embedded VM archive and dynamic bridge registration to the generated executable
2. **Initialization**: Keep the embedded VM dormant until `load`, `loadfile`, `dofile`, or another VM bridge path is used
3. **VM setup**: Create one `DynamicVM` for the clx `LState`, open the VM's own standard libraries, and build the default VM-side environment
4. **Loading**: Compile source or binary chunks with the VM's loader; `load` accepts the current bridge's string form and uses the VM's `loadbufferx` path
5. **Calling**: Wrap loaded VM functions as clx-callable values, convert arguments into VM values, and convert returned values back into clx values
6. **Reverse calls**: Expose clx functions to VM code through `NativeBridge`, with table proxies and tracked table-argument write-back where supported
7. **Cleanup**: Destroy VM-owned values with the VM collector and release clx-side proxy roots with the clx collector

The dynamic path is a boundary, not an alternate code generator. A loaded chunk is interpreted by the embedded VM; it is never turned into AOT C++ during program execution.

## Memory Management### Value Representation

Every Lua value that can't stay a native number travels in a 16-byte `LValue` box:
a type tag plus 8 bytes of payload (a `double`, an `int64_t`, a pointer — or, for
strings of ≤6 bytes, the characters themselves, `TAG_ISTR`). Long-lived values (tables,
functions, threads, userdata) live on the GC heap; the box just points at them. See
[Runtime](./runtime.md) for the full layout.

### Runtime Ownership and Bridge Objects

The clx runtime owns `LValue`, `LTable`, clx functions, clx threads, and clx userdata. The embedded VM owns `lua_State`, Lua tables, Lua closures, Lua threads, and VM userdata. A value crossing the boundary is either copied, wrapped in bridge userdata, or rejected:

- nil, booleans, numbers, and strings are copied;
- clx tables become `clx_proxy` userdata in the VM, with bridge metamethods for supported access;
- clx userdata becomes `clx_userdata` proxy userdata; it is not exposed as a raw native pointer;
- clx functions become VM-side callable bridge closures;
- VM tables passed to a clx function are converted into clx tables, with write-back for tracked table arguments;
- ordinary VM-created userdata and lightuserdata are VM-local and convert to `nil` when passed to clx; actual VM threads are rejected, while only recognized internal bridge proxies have special handling;
- clx and VM coroutines cannot be resumed across the boundary, and a yield cannot cross it.

Proxy nodes keep the referenced clx object alive from the VM side. The two collectors still run independently; the bridge explicitly marks proxy targets when the clx collector builds its root set.

### Garbage Collection

Generational collector by default (`CLX_GC_MODE=incremental` opts out to the legacy
collector). Young objects die in cheap *minor* collections; survivors graduate to
older generations; a full mark-and-sweep *major* runs only when the old generation
overgrows. Write barriers record old-to-young edges so minors never walk old subtrees.
See [Runtime](./runtime.md) for the details, including the root-frame rooting model
and per-function table arenas (bump-allocated tables for values proven not to escape).