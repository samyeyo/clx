# clx Runtime

The clx runtime library (`libclx.a`) implements Lua's core semantics in C++. It provides the necessary runtime support for compiled Lua programs.

## Components

### 1. Core VM (src/runtime/runtime.cpp)

The virtual machine core implements:
- Value representation (nan-boxing with Integer type and TAG_ISTR inline strings)
- Garbage collection (stop-the-world mark-and-sweep)
- Table operations with inline caching
- Metamethod handling
- Function calls

#### Value System

clx uses nan-boxing to represent all Lua values in 64 bits:

```
┌─────────────────────────────────────┐
│ Double (numbers)                    │
│ ┌─────────────────────────────────┐ │
│ │ 64-bit IEEE 754 floating point  │ │
│ └─────────────────────────────────┘ │
├─────────────────────────────────────┤
│ Integer (small integers)            │
│ ┌─────────────────────────────────┐ │
│ │ 62-bit integer + 2-bit type tag │ │
│ └─────────────────────────────────┘ │
├─────────────────────────────────────┤
│ Pointer (strings, tables, etc.)     │
│ ┌─────────────────────────────────┐ │
│ │ 62-bit pointer + 2-bit tag      │ │
│ └─────────────────────────────────┘ │
├─────────────────────────────────────┤
│ TAG_ISTR (inline strings ≤5 bytes)  │
│ ┌─────────────────────────────────┐ │
│ │ 16-bit tag + 48-bit char data   │ │
│ │ tag = 0xFFF9 + len (0-5)        │ │
│ └─────────────────────────────────┘ │
├─────────────────────────────────────┤
│ Special (nil, true, false)          │
└─────────────────────────────────────┘
```

The `LType` enum distinguishes between Number (double), Integer (native int64), and other types. Integer arithmetic uses native operations without floating-point conversion.

##### TAG_ISTR — Inline Strings

Strings ≤5 bytes are stored directly in the LValue's 64-bit `val` field (no heap allocation, no interning):

- **Tag**: top 16 bits (63-48) = `0xFFF9 + len`, where len = 0-5
- **Data**: bytes 0-5 (bits 0-47) store characters contiguously from byte 0 upward, null-terminated at byte `len`
- **`as_string()`**: returns `reinterpret_cast<const char*>(&val)` — works for both TAG_ISTR (inline) and TAG_STRING (heap interned pointer)
- **`string_len()`**: reads length from tag for TAG_ISTR, or from baked header for TAG_STRING
- **Equality**: `val == val` works for identically-typed strings with same content; cross-type (TAG_ISTR vs TAG_STRING) falls back to `memcmp`
- **Hashing**: `lvalue_hash` for TAG_ISTR uses `swar_hash_8()` (loads all ≤8 bytes into one register with a single `memcpy`) so hashes match TAG_STRING for the same content
- **`clx::string(L, s)`**: returns `LValue::istr()` for `len ≤ 5`, falls back to interned string for longer

#### Garbage Collection

Stop-the-world mark-and-sweep collector:
- **Mark phase**: Traverses all reachable objects from global state and stack
- **Sweep phase**: Deallocates objects not marked as reachable
- **Finalizers**: `__gc` metamethod called for objects before collection
- Uses a reusable worklist vector to avoid repeated allocations

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

Implementation uses `ucontext` on Linux/macOS and Windows Fibers on Windows.

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

### 6. Lazy Function Registration (src/runtime/runtime.cpp)

`set_lazy_funcs(L, table, lazy_regs, count)` attaches a `__index` metamethod that creates `LCFunction` closures lazily on first access and caches them on the table. Uses `constexpr`-friendly `LazyReg` arrays (raw function pointers, no `std::function`) so registration tables live in static read-only storage.

### 7. Table Library (src/runtime/table.cpp)

Table manipulation:

| Function | Description |
|----------|-------------|
| `table.insert` | Insert element |
| `table.remove` | Remove element |
| `table.concat` | Concatenate elements |
| `table.sort` | Sort array elements |
| `table.unpack` | Unpack array to values |

### 8. Module Loading

Module loading via `require` is implemented in the package library (package.cpp):
- `require` with `package.path` support
- `package.preload` table support

### 9. Closures and Upvalues

clx supports lexical scoping with full closure capture:
- Local variables captured by inner functions become upvalues
- Shared upvalues (multiple closures sharing the same captured variable)
- Loop variable capture (closures created in a for loop each capture the correct iteration value)
- Triple nesting and arbitrary capture depth
- Tail call optimization (TCO) for recursive calls — no stack growth

### 10. Goto and Labels

Full `goto` / `::label::` support with proper lexical scoping:
- Forward and backward jumps
- Duplicate labels in different scopes resolve correctly
- Goto can create loops (backward jumps)

### 11. String Pool & Inlining (src/runtime/runtime.cpp + LValue)

Strings ≤5 bytes use **TAG_ISTR** — stored inline in the LValue (no heap allocation):

```
Bits: [16-bit tag (0xFFF9 + len)] [char0] [char1] [char2] [char3] [char4] [pad\0]
                             63←48  47←40  39←32  31←24  23←16  15←8    7←0
```

Longer strings are interned via the StringPool:

- **Baked allocation**: `[uint32_t hash][uint32_t len][char data...\0]`
- **One probe on hit**: No double lookup or side map
- **Pre-hashed strings**: Hash stored at `ptr[-8..ptr[-5]` for zero recompute
- **Intern preallocated**: Supports adopting pre-formatted buffers (zero-allocation concat)

Cross-type safety: `lvalue_hash` and `lvalue_eq_fast` in `runtime.cpp` handle TAG_ISTR↔TAG_STRING lookups correctly — hashes match because both content-types use the same `swar_hash_8()` for strings ≤8 bytes.
- For strings ≤8 bytes, `swar_hash_8()` loads all bytes into a single register (one unaligned `memcpy`) and mixes via `wyhash64(data ^ len)` — no branching, no function call overhead.
- For strings >8 bytes, `wyhash_str()` is used (processes in 8-byte chunks).

### 12. StringBuilder (include/clx.h)

O(n) string concatenation avoiding quadratic blow-up:

- Inline storage for up to 8 parts
- Grows to heap allocation when needed
- Produces single interned string with baked hash
- Used internally by codegen for multi-part string expressions

### 13. CacheSlot Inline Caching (include/clx.h)

Per-call-site cache for string-keyed table access:

```cpp
struct CacheSlot {
    uint64_t table_val;     // Cached table pointer
    uint32_t shape_version; // Table version for staleness detection
    LValue    cached;       // Cached value (non-GC only)
    bool      valid;        // Cache validity flag
};
```

Caches are only valid when:
1. The table pointer matches
2. The `shape_version` hasn't changed (table hasn't been mutated)
3. The cached value is not a GC object (to avoid dangling pointers)

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
| `__metatable` | getmetatable() |

## Memory Layout

### Table Layout

```
┌───────────────────────────────────────────────────────┐
│ LHeader (metadata)                                    │
│   - type, marked, next                               │
├───────────────────────────────────────────────────────┤
│ Cache line 0 (64 bytes - all gettable touches here)  │
│   - array pointer (8)                                │
│   - array_size, array_cap (16)                      │
│   - bucket, hash_size (24)                          │
│   - metatable, hash_count (40)                      │
│   - padding to 56 bytes                             │
├───────────────────────────────────────────────────────┤
│ Parallel arrays (cache-line optimized)               │
│   - keys (hash_size * 8 bytes)                      │
│   - vals (hash_size * 8 bytes)                      │
│   - nexts (hash_size * 2 bytes)                    │
│   - shape_version (4 bytes)                        │
│   - free_head (2 bytes)                             │
└───────────────────────────────────────────────────────┘
```

Shape version is incremented on every table write to enable CacheSlot invalidation.

### String Layout

Interned strings are stored as:
- 4 bytes: baked wyhash
- 4 bytes: length
- char data: string content
- 1 byte: null terminator

The LValue stores a pointer to the char data (skipping the 8-byte header).
Length is at `ptr[-4..ptr[-1]` and hash is at `ptr[-8..ptr[-5]]`.

## Performance Characteristics

| Operation | Complexity |
|-----------|------------|
| Table access (key known) | O(1) average |
| Table access (CacheSlot hit) | O(1) with single pointer check |
| Table iteration | O(n) |
| String concatenation | O(n) per operation |
| Numeric for loop | O(n) with SIMD |
| Function call | O(1) setup + body |
| GC pause | Incremental, 512-byte step budget |
| String interning | O(1) average, one probe |
| Pattern matching | O(n*m) worst case |

## Pre-interned Metamethods

To avoid repeated string interning on every metamethod dispatch, clx pre-interns common metamethod strings at LState initialization:

- `str_index`, `str_newindex`, `str_gc`, `str_call`, `str_close`, `str_pairs`, `str_tostring`

These are stored directly in LState and used for fast metamethod lookup.

## GC Options

The `collectgarbage()` function accepts these options:

| Option | Description |
|--------|-------------|
| `"collect"` | Performs a full garbage-collection cycle (default) |
| `"stop"` | Stops automatic GC — the collector runs only on explicit `collectgarbage("collect")` or `"step"` until `"restart"` |
| `"restart"` | Restarts automatic GC |
| `"count"` | Returns total memory in use by Lua in Kbytes (fractional part gives exact bytes when multiplied by 1024) |
| `"step"` | Performs a single GC sweep step. Optional integer arg pretends that many extra bytes were allocated. Returns `true` if the step finished a collection cycle |
| `"isrunning"` | Returns `true` if the collector is running (not stopped) |
| `"incremental"` | Switches to incremental mode (already the default). Returns `"incremental"` |
| `"generational"` | Not supported — returns `"incremental"` (stays incremental) |
| `"param"` | Gets/sets GC parameters. Requires a parameter name (`"pause"`, `"stepmul"`, `"stepsize"`) and an optional new integer value (0–100000). Returns the previous value |
