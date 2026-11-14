# clx Runtime

`libclx.a` is the C++ library every compiled Lua program links against. When the
emitted code can't prove something at compile time, it calls into this library,
which behaves exactly like stock Lua (same values, same errors, same GC promises).

You don't need to read this page top to bottom. The first three sections (values, GC,
roots) explain ideas the rest of the docs assume; everything after that is a
per-module reference.

## Components

### 1. Core VM (src/runtime/)

The virtual machine core is split across several translation units (see the file list in
[Architecture](./architecture.md)): value representation and arithmetic in `values.cpp` +
`clx_runtime.h`, tables in `tables.cpp`, threads/fibers in `threads.cpp`, GC in `gc.cpp`,
state lifecycle in `state.cpp`, and call dispatch in `calls.cpp`. It provides:
- Value representation (separate `ValueType` tag + 8-byte payload, with Integer type and TAG_ISTR inline strings)
- Garbage collection (generational by default, incremental opt-out)
- Table operations with inline caching
- Metamethod handling
- Function calls
- A C stack guard: `clx::stack_check(L)` raises stock's `stack overflow` before the C stack
  runs out, against a limit derived from the calling thread's stack and swapped with the
  coroutine context (`Ls->stack_low` in `threads.cpp`), so a recycled fiber keeps its own
  threshold. Fast function bodies use a deferred form — they record the site and return —
  because a throw in that lambda costs ~1.9× on recursion-heavy code; the enclosing call
  wrapper raises at the recorded site (`raise_stack_overflow` in `state.cpp`).

#### Value System

clx uses a 16-byte `LValue` (8-byte payload + separate `ValueType` tag) to represent all Lua values:

```
┌─────────────────────────────────────┐
│ Double (numbers)                    │
│ ┌─────────────────────────────────┐ │
│ │ 64-bit IEEE 754 floating point  │ │
│ └─────────────────────────────────┘ │
├─────────────────────────────────────┤
│ Integer                             │
│ ┌─────────────────────────────────┐ │
│ │ 64-bit signed integer           │ │
│ └─────────────────────────────────┘ │
├─────────────────────────────────────┤
│ Pointer (strings, tables, etc.)     │
│ ┌─────────────────────────────────┐ │
│ │ 64-bit heap address             │ │
│ └─────────────────────────────────┘ │
├─────────────────────────────────────┤
│ TAG_ISTR (inline strings ≤6 bytes)  │
│ ┌─────────────────────────────────┐ │
│ │ [len:8][pad:8][char5..char0:48] │ │
│ │ bytes 0–5: up to 6 chars        │ │
│ │ byte 7: length (0–6)            │ │
│ └─────────────────────────────────┘ │
├─────────────────────────────────────┤
│ Special (nil, true, false)          │
└─────────────────────────────────────┘
```

The `ValueType` enum distinguishes between Number (double), Integer (native int64), and other
types. Integer arithmetic uses native `int64_t` operations without floating-point conversion.
Overflow in `add`, `sub`, `mul` **wraps** (two's-complement, computed in `uint64_t`), matching
stock Lua 5.5 (`math.maxinteger + 1 == math.mininteger`). Integer results stay in the integer
domain; only mixed int/float operands and `pow` promote to double.

##### TAG_ISTR — Inline Strings

Strings ≤6 bytes are stored directly in the LValue's 64-bit `val` field (no heap allocation, no interning):

- **Layout**: the top **byte** (bits 63–56) stores the length (0–6); byte 6 (bits 55–48) is padding; bytes 0–5 store characters contiguously from byte 0 upward
- **`as_string()`**: returns `reinterpret_cast<const char*>(&val)` — works for both TAG_ISTR (inline) and TAG_STRING (heap interned pointer)
- **`string_len()`**: reads length from the top byte for TAG_ISTR, or from the baked 4-byte header field for TAG_STRING
- **Equality**: `val == val` works for identically-typed strings with same content; cross-type (TAG_ISTR vs TAG_STRING) falls back to `memcmp`
- **Hashing**: `lvalue_hash` for TAG_ISTR uses `swar_hash_8()` (loads all ≤8 bytes into one register with a single `memcpy`) so hashes match TAG_STRING for the same content
- **`clx::string(L, s)`**: returns `LValue::istr()` for `len ≤ 6`, falls back to interned string for longer

#### Garbage Collection

Generational collector, enabled by default:
- **Young generation**: new objects start here; each minor collection marks the reachable ones and frees the rest
- **Aging**: survivors graduate from young to survivor to old over successive minors
- **Remembered set**: write barriers record old-to-young references, so minors never walk old subtrees
- **Major collection**: a full mark-and-sweep fallback that runs when the old generation outgrows its budget
- **Finalizers**: `__gc` metamethod called for objects before collection
- Uses raw pointer stacks (no per-operation vector bookkeeping) and recycled free lists for tables and closures

Set `CLX_GC_MODE=incremental` to opt back out to the legacy incremental collector. Tune the generations with `CLX_GC_MINOR_KB` and `CLX_GC_MAJOR_KB`.

#### GC roots: root frames

The GC must never free a value the program still uses. Every generated function opens
with `clx::Frame<16> _fr(L)` and pins each local with `_fr.push(x)`. Frames chain
together; when a C++ scope exits, its frame unlinks itself — no manual bookkeeping,
and exceptions unsubscribe automatically. (Blocks use a `FrameGuard` watermark for the
same idea at smaller scope.) Coroutine switches stash and reload the whole chain
instead of copying memory around.

### 2. Base Library (src/runtime/base.cpp)

Implements core Lua functions:

| Function | Description |
|----------|-------------|
| `print` | Output to stdout |
| `error` | Raise runtime error |
| `assert` | Assert condition |
| `type` | Get value type |
| `tostring` | Convert to string |
| `tonumber` | Convert to number |
| `pairs` | Iterator for key-value pairs |
| `ipairs` | Iterator for array elements |
| `next` | Table traversal |
| `pcall` | Protected call |
| `xpcall` | Protected call with error handler |
| `select` | Select arguments |
| `collectgarbage` | Garbage collection control — see [GC options](#gc-options) below |
| `setmetatable` | Set metatable |
| `getmetatable` | Get metatable |
| `rawequal` | Raw equality test |
| `rawget` | Raw table get |
| `rawset` | Raw table set |
| `rawlen` | Raw length (for strings/tables) |
| `warn` | Warning message |
| `_VERSION` | Lua version string |

### 3. Math Library (src/runtime/math.cpp)

Uses `set_lazy_funcs` for lazy function registration — functions are registered via `constexpr LazyReg[]` arrays and created as `LCFunction` closures on first access, then cached on the table. No eager closures or `std::function` overhead at startup.

Implements mathematical functions:

| Function | Description |
|----------|-------------|
| `math.sin`, `math.cos`, `math.tan` | Trigonometric |
| `math.asin`, `math.acos`, `math.atan` | Inverse trigonometric |
| `math.exp`, `math.log` | Exponential/Logarithmic |
| `math.sqrt` | Square root |
| `math.frexp`, `math.ldexp` | Decompose/compose floating-point |
| `math.modf` | Integer and fractional parts |
| `math.abs`, `math.ceil`, `math.floor` | Rounding |
| `math.random`, `math.randomseed` | Random numbers |
| `math.deg`, `math.rad` | Angle conversion |
| `math.min`, `math.max` | Min/Max of arguments |
| `math.type` | Returns `"integer"`, `"float"`, or nil |
| `math.ult` | Unsigned less-than comparison |
| `math.tointeger` | Convert to integer or nil |

### 4. Coroutine Library (src/runtime/coroutine.cpp)

Full coroutine support using OS-level fibers/context:

| Function | Description |
|----------|-------------|
| `coroutine.create` | Create new coroutine |
| `coroutine.resume` | Resume coroutine execution |
| `coroutine.yield` | Suspend coroutine |
| `coroutine.status` | Get coroutine status |
| `coroutine.wrap` | Create callable wrapper |
| `coroutine.isyieldable` | Check if coroutine can yield |
| `coroutine.close` | Close a suspended coroutine |

Implementation uses hand-written assembly on the common platforms — Windows Fibers on
Windows, ARM64 asm on Apple **and** Linux (`src/runtime/coro_switch_aarch64.S`), x86_64 asm
on Linux (`coro_switch_x86_64.s`) — with `ucontext` as the fallback elsewhere (e.g. macOS
x86_64). The stock `ucontext` shim on Darwin ARM64 is deprecated and corrupts the stack,
which is why the asm path exists.

### 5. String Library (src/runtime/strings.cpp)

Full string manipulation with pattern matching:

| Function | Description |
|----------|-------------|
| `string.len` | String length (uses `string_len()` — works for both TAG_ISTR and baked header) |
| `string.sub` | Substring extraction |
| `string.reverse` | Reverse string |
| `string.lower`, `string.upper` | Case conversion |
| `string.rep` | String repetition |
| `string.byte`, `string.char` | Character conversion |
| `string.format` | Formatted string (most specifiers) |
| `string.find` | Find pattern (plain or pattern) |
| `string.match` | Match and extract captures |
| `string.gmatch` | Global match iterator |
| `string.gsub` | Global substitution |

Pattern matching features:
- Character classes: `%a`, `%c`, `%d`, `%g`, `%l`, `%p`, `%s`, `%u`, `%w`, `%x`, `%z`
- Captures and back-references
- Pattern modifiers: `*`, `+`, `-`, `?`, `^`, `$`
- Balanced matching with `%b`
- Frontier pattern with `%f`

### 6. Lazy Function Registration (src/runtime/calls.cpp)

`set_lazy_funcs(L, table, lazy_regs, count)` attaches a `__index` metamethod that creates `LCFunction` closures lazily on first access and caches them on the table. Uses `constexpr`-friendly `LazyReg` arrays (raw function pointers, no `std::function`) so registration tables live in static read-only storage.

### 7. Table Library (src/runtime/table.cpp)

Table manipulation:

| Function | Description |
|----------|-------------|
| `table.insert` | Insert element |
| `table.remove` | Remove element |
| `table.concat` | Concatenate elements |
| `table.sort` | Sort array elements |
| `table.pack` | Pack varargs into a table |
| `table.unpack` | Unpack array to values |
| `table.move` | Move elements between tables |
| `table.create` | Pre-allocated array table |

### 8. Module Loading

Module loading via `require` is implemented in the package library (package.cpp):
- `require` with `package.path` support
- `package.preload` table support

### 9. Closures and Upvalues

Locals captured by inner functions become heap upvalues, shared correctly between
closures (including the classic loop-variable capture). `return f(...)` tail calls
compile to temporaries and rely on the C++ compiler's tail-call elimination (see
[Optimizations](./optimizations.md)).

### 10. Goto and Labels

Full `goto` / `::label::` with lexical scoping — forward, backward, and duplicate
labels in different scopes all work.

### 11. String Pool & Inlining (include/clx_runtime.h + LValue)

Short strings (≤6 bytes) live *inside* the `LValue` itself (`TAG_ISTR`): the top byte
holds the length, the low bytes hold the characters — no allocation at all. Longer
strings are interned in the `StringPool`: one shared copy per distinct string,
with the hash and length baked into a 16-byte header in front of the characters, so
`len` and hash are each a single 4-byte load. Storage comes from a bump-allocated
`StringArena` (64 KB blocks). Inline and interned strings hash identically, so
comparisons work across the two forms.

```
Byte layout (8 bytes): [len] [pad] [char5] [char4] [char3] [char2] [char1] [char0]
Bits:                   63←56 55←48  47←40   39←32   31←24   23←16   15←8    7←0
```

### 12. StringBuilder (include/clx_runtime.h)

O(n) string concatenation avoiding quadratic blow-up:

- Inline storage for up to 8 parts
- Grows to heap allocation when needed
- Produces single interned string with baked hash
- Used internally by codegen for multi-part string expressions

### 13. Per-Table Inline Cache (include/clx_runtime.h)

Per-LTable inline cache for string-keyed hash access. The cache lives in the table's lazily
allocated `LTableExt` (so pure array-part tables never allocate it) and holds **8 entries**
that remember key bits, entry index, and table version. A hit (same key, same version,
entry still live) skips the hash probe; a miss probes normally and fills the slot. The
cache is invalidated by `hash_version`, which moves only on structural change (rehash,
metatable set, recycle). See [Optimizations](./optimizations.md#3-inline-caching) for
the full story.

### 14. Debug Library (src/runtime/debug.cpp)

The `debug` library is a std module like the others (`libclx_debug.a`) and is
registered as a global by `luastd_debug`. The entries split by what they can answer:

| Entries | Implementation |
|---------|----------------|
| `getmetatable`, `setmetatable`, `getuservalue`, `setuservalue` | Native. `debug.getmetatable` skips the `__metatable` guard; `debug.setmetatable` accepts every type, including numbers and `nil`, and the value metatables live in the same per-type slots the operator paths read. |
| `getupvalue`, `setupvalue`, `upvalueid` | Walk the closure's `LUpValue` cells (`LCFunction::gc_cells`), numbered from 1 in that order — the same numbering `luaapi_upvalue_cell` gives the C API adapter, so `debug.getupvalue(f, n)` and `lua_getupvalue(L, f, n)` describe the same slot. The cells are the in-scope capture cells the closure was created with: their names are not recorded (every name is `""`), and a local the codegen captured by value has a snapshot cell that `setupvalue` cannot push back into the function. `upvalueid` interns `"userdata: <cell address>"`, which is equal for two closures sharing a capture. |
| `upvaluejoin` | Raises `debug.upvaluejoin is not available in compiled code`: the function body reads its captures through the emitted C++ lambda, so replacing the recorded cell would only change what `getupvalue` reports. |
| `getregistry` | An empty table created on first use and rooted for the life of the state; it carries none of stock's `_LOADED` / `_PRELOAD` / `_CLIBS` keys. |
| `traceback` | `msg`, a newline and `stack traceback:`, with no frame list — compiled frames carry no call chain. |
| `getinfo`, `getlocal`, `setlocal`, `sethook`, `gethook` | Raise `debug.getinfo/getlocal/setlocal/sethook/gethook are not available in compiled code`, since there are no line/frame tables. Both this and the `upvaluejoin` raise go through `stub_error`, so the message is prefixed like every other runtime error. |

Two hooks soften that last row under `--dynamic`: `clx_vm_debug_getinfo_hook` and
`clx_vm_debug_upvalue_hook` (declared in `internal.h`, assigned by
`src/runtime/vm/load_builtin.cpp`) let `debug.getinfo`/`debug.getupvalue` answer for a
function created by `load`/`loadfile`, described by the embedded VM's own debug data. A
compiled closure leaves the hook returning `false`, so the caller errors as above.

## Metamethods

clx supports all standard Lua metamethods:

| Metamethod | Operator |
|------------|----------|
| `__add` | + |
| `__sub` | - |
| `__mul` | * |
| `__div` | / |
| `__mod` | % |
| `__pow` | ^ |
| `__unm` | unary - |
| `__len` | # |
| `__eq` | == |
| `__lt` | < |
| `__le` | <= |
| `__concat` | .. |
| `__index` | table.key |
| `__newindex` | table.key = value |
| `__call` | table() |
| `__tostring` | tostring() |
| `__gc` | garbage collection |
| `__close` | to-be-closed variable (`<close>`) |
| `__metatable` | getmetatable() |

### To-be-closed Variables (`<close>`)

`local x <close> = ...` runs the `__close` metamethod when `x` goes out of scope, via a `CloseGuard` in the generated code. The guard holds the value itself, so it stays alive exactly as long as needed.

## Memory Layout

`LTable` is **80 bytes**: the hot array part inline (plus a 2-slot inline buffer for
tiny arrays), with hash entries, the occupancy bitmap, the inline cache, and the
metatable moved behind a lazily allocated `LTableExt` pointer. Array-only tables
never allocate the extension, keeping the hot read path on one cache line; the
tag/payload arrays and the hash entries/bitmap each share one allocation. Strings
carry a 16-byte header (baked hash + length) in front of the characters. See
[Optimizations §2](./optimizations.md#2-table-memory-layout) for the rationale and
the ASCII diagrams.

## Performance Characteristics

| Operation | Complexity |
|-----------|------------|
| Table access (key known) | O(1) average |
| Table access (InlineCache hit) | O(1) with single key payload check |
| Table iteration | O(n) |
| String concatenation | O(n) per operation |
| Numeric for loop | O(n) plain C++ counter (int64_t or double) |
| Function call | O(1) setup + body |
| GC pause | Generational minors (young-only), full mark-and-sweep majors, SIMD array scan |
| String interning | O(1) average, one probe |
| Pattern matching | O(n*m) worst case |

## SIMD Runtime Scans

Type tags are single bytes, so 16–32 of them fit in one SIMD register. `rawlen`,
`next`, `table_concat` validation, and the GC mark loop all scan tag arrays that
way (AVX2 / SSE2 / NEON with scalar fallback). Details in
[Optimizations §4](./optimizations.md#4-simd-runtime-scans).

## Pre-interned Metamethods

To avoid repeated string interning on every metamethod dispatch, clx pre-interns the full metamethod set at LState initialization (`state.cpp`):

- `__index`, `__newindex`, `__gc`, `__call`, `__close`, `__pairs`, `__tostring`, `__len`,
  `__eq`, `__lt`, `__le`, `__concat`, `__add`, `__sub`, `__mul`, `__div`, `__mod`,
  `__idiv`, `__pow`, `__unm`, `__band`, `__bor`, `__bxor`, `__shl`, `__shr`, `__bnot`,
  `__metatable`, `__name`, `__ipairs`

These are stored directly in LState (`str_index`, `str_add`, …) and used for fast metamethod lookup.

## GC Options

The `collectgarbage()` function accepts these options:

| Option | Description |
|--------|-------------|
| `"collect"` | Performs a full garbage-collection cycle. A no-argument `collectgarbage()` returns `0.0` and does **not** collect (unlike stock Lua, where no-arg == `"collect"`) |
| `"stop"` | Stops automatic GC — the collector runs only on explicit `collectgarbage("collect")` or `"step"` until `"restart"` |
| `"restart"` | Restarts automatic GC |
| `"count"` | Returns total memory in use by Lua in Kbytes (fractional part gives exact bytes when multiplied by 1024) |
| `"step"` | Performs a single GC sweep step. Optional integer arg pretends that many extra bytes were allocated. Returns `true` if the step finished a collection cycle |
| `"isrunning"` | Returns `true` if the collector is running (not stopped) |
| `"incremental"` | Switches to incremental mode (opt-out from the generational default). Returns `"incremental"` |
| `"generational"` | Switches to generational mode (the default). Returns `"generational"` |
| `"mode"` | Returns the current mode: `"generational"` or `"incremental"` |
| `"stats"` | Returns a table with GC stats: `mode`, `collects`, `minors`, `minor_freed_kb`, `major_freed_kb`, `old_kb`, `minor_kb`, `remembered_high` |
| `"param"` | Gets/sets GC parameters. Requires a parameter name (`"pause"`, `"stepmul"`, `"stepsize"`, `"minormul"`, `"majorminor"`, `"minormajor"`) and an optional new integer value (0–100000). Returns the previous value |

Inside a `__gc` finalizer, `collectgarbage()` returns `nil` instead of running a
collection, matching stock 5.5. Weak tables follow stock's clearable-kinds rule
(strings are never cleared), and `collectgarbage("step", size)` treats its
argument as the number of bytes to pretend were allocated.
