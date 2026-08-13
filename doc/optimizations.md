# clx Optimizations

clx applies multiple levels of optimization to generate fast native code:

## 1. Compile-Time optimizations

### Constant folding

clx emits arithmetic expressions and delegates to the C++ compiler, which performs constant folding at compile time:

```lua
local x = 1 + 2 + 3  -- Compiled as: local x = 6
local y = "hello" .. " " .. "world"  -- Compiled as: local y = "hello world"
```

### Numeric fast-path

clx distinguishes between `Integer` and `Number` types in its value representation (separate `ValueType` tag + 8-byte payload).
The runtime LValue arithmetic functions dispatch to native double or int64 operations internally.
At the codegen level, integer literals and variables provably holding integers are emitted as C++ `int64_t`, while float literals and mixed expressions use native `double`:

```lua
-- Integer literal:
local i = 1 + 2  -- Compiled as: int64_t arithmetic via LValue

-- Float literal:
local f = 1.5 + 2.3  -- Compiled as: double arithmetic via LValue

-- Mixed (promotes to double):
local m = 1 + 2.5  -- Integer converted to double
```

### Direct arithmetic fast-path

When all operands in an expression are known to be numeric (Integer or Number), clx generates
direct C++ arithmetic instead of going through the dynamic `LValue` system:

```lua
-- Slow path (dynamic):
local result = a + b  -- Uses clx::LValue arithmetic with type checks

-- Fast path (numeric):
local result = a + b  -- When a and b are known numbers, uses add directly
```

### Native integer tracking

Beyond plain numeric inference, the optimizer tracks which locals, function parameters, and
function return values are **integer**-typed (`int64_t` rather than `double`). This enables:

- `int64_t` loop counters for numeric `for` loops (no floating-point counter):

```lua
-- Generated C++:
-- for (int64_t i_val = 1; i_val <= n; i_val++) { ... }
for i = 1, n do
    ...
end
```

- unboxed integer locals (`int64_t l_x;`) instead of `double` when the optimizer proves the variable is only assigned integer values;
- integer-returning functions whose result is used directly in integer arithmetic;
- native `//`, `%`, bitwise, and shift operators when both sides are integer-typed.

The optimizer records `int_returning_funcs`, `int_typed_locals`, and `native_integers` in the analysis state (`analysis_state.h`), and `is_purely_integer_expr()` proves integer-ness for whole expressions so arithmetic stays in the integer domain end-to-end.

### Int64 overflow safety

Integer arithmetic (`add`, `sub`, `mul`) with two `Int64` operands uses native C++ integer
operations for speed. Each operation checks for overflow before returning:

- **add**: Detects same-sign inputs producing a different-sign result
- **sub**: Detects different-sign inputs where the result sign matches the subtrahend
- **mul**: Verifies `result / operand == other_operand`, with special-case for `INT64_MIN * -1`

On overflow, the result is promoted to `double` (matching Lua 5.5 semantics). `mod`, `idiv`,
`pow`, and `unm` always promote to double for all numeric inputs.

### Non-fast function parameter numeric promotion

Functions with mixed parameter types (e.g., `decode(str, pos, ...)` where `str` is a string
but `pos` is a number) cannot be fast functions — not all parameters are numeric. But every
`pos + 1`, `pos - 1` still went through `clx::add`/`clx::sub` runtime calls.

The optimizer detects parameters used in arithmetic operations (directly or through call chains)
and marks them as numeric. The codegen emits them as `double l_param = args[i].as_number()`
instead of `clx::LValue l_param = args[i]`, enabling native arithmetic even in non-fast
functions.

```lua
-- Before: pos + 1 uses clx::add runtime call
local function decode(str, pos, ...)
    return str:sub(pos, pos + 1)
end

-- After: pos is a native double, pos + 1 is C++ arithmetic
```

### Function parameter numeric-record array inference

When a function receives a table as a parameter (e.g., `advance(bodies, dt)` where `bodies`
is an array of record-like tables), the optimizer cannot prove that `bodies[i].x` yields a
number without tracing the pattern.

Pass 3 in the optimizer detects `local bi = bodies[i]` where `bodies` is a function parameter,
then traces field accesses (`bi.x`, `bi.y`) in BinaryOp nodes to infer numeric fields. Both
the parameter name and the local name are registered in `g_numeric_table_fields` so
`yields_number()` proves field reads yield numbers.

```lua
-- Before: bodies[i].x goes through clx::table_get → as_number()
local function advance(bodies, dt)
    for i = 1, #bodies do
        local bi = bodies[i]
        bi.x = bi.x + bi.vx * dt
    end
end

-- After: bi.x is a native double, bi.x + bi.vx * dt is C++ arithmetic
```

### Known-length `#t` constant folding

Tables created with a fixed constructor length (e.g. `{1, 2, 3}`) get a known length recorded in `known_table_lengths`. When the optimizer can prove the table is not mutated afterwards, `#t` is folded to a compile-time constant instead of a runtime `rawlen` call.

The optimizer also tracks tables whose length can change at runtime — `setmetatable(t, mt)`, `table.insert(t, ...)`, and `table.remove(t, ...)` populate tables_with_dynamic_length`,
preventing incorrect constant folding on mutated tables.

### Direct table field write (`table_set_direct`)

When the optimizer proves a record field exists (from `numeric_table_fields`), the codegen
emits `clx::table_set_direct(L, tbl, key, val)` instead of the general `table_set`. The direct
helper calls `settable()` directly, skipping `set_value()`'s redundant `gettable()` existence
check.

### Pure numeric array promotion

The optimizer promotes tables whose array part provably holds only numbers into
`pure_numeric_arrays`. Reads on such tables emit direct `std::vector<double>`-style indexing
instead of `clx::table_get`. This includes:

- numeric array constructors (`local t = {1, 2, 3}`) — verified element-wise;
- record-like constructors (`local v = {x=1, y=2, z=3}`) — numeric string-keyed fields are
  registered in `numeric_table_fields` so `v.x + v.y` emits native arithmetic;
- the empty-table-in-loop pattern (`local t = {}; for i = 1, N do t[i] = v end`) — promoted to
  `pure_numeric_arrays` so the loop writes through fast indexed codegen.

### Local variable optimization

Local variables that hold numbers are stored as unboxed C++ `int64_t` or `double`:

```lua
local function sum(n)
    local total = 0  -- Stored as int64_t
    for i = 1, n do
        total = total + i  -- Direct int64_t arithmetic
    end
    return total
end
```

## 2. Table memory layout

### Compact LTable (80 bytes) with lazy LTableExt

`LTable` is kept at **80 bytes** by moving hash, inline-cache, and metatable state behind a
lazily-allocated `LTableExt` pointer. The hot struct holds only the array part and the `ext`
pointer:

```cpp
struct LTableExt {
    HashEntry *entries = nullptr;       // hash part
    size_t hash_size, hash_count, hash_tombs;
    uint64_t *hash_bitmap = nullptr;    // slot-occupancy bitmap for fast scans
    uint32_t hash_version = 0;          // bumped on structural mutation
    LTableInlineCache *ic = nullptr;    // inline cache (8 entries, lazily allocated)
    LTable *metatable = nullptr;
    LTable *meta_next = nullptr;        // GC metatable-list link
};

struct LTable : public LHeader {
    TValue *array;
    ValueType *array_types;
    size_t array_size, array_cap;
    TValue small_array[2];              // inline buffer for tiny arrays
    ValueType small_array_types[2];
    LTableExt *ext;                     // null until hash/ic/metatable needed
};
```

Benefits:

- Tables that stay purely array-part (the common case in numeric workloads) never allocate
  an `LTableExt` — the hot read path touches a single cache line.
- The inline cache is allocated lazily only when string-key or hash accesses start happening.
- Arena-allocated tables carve their `LTableExt` out of the arena blob (freed with the arena);
  heap tables allocate it on first hash insertion or metatable set.
- `binarytrees` improved from 0.43s to 0.31s (~27%) with the 80B layout.

### Inline buffer for small arrays

Arrays of size ≤ 2 use `small_array`/`small_array_types` storage inside `LTable` itself — no
heap allocation and no accounting overhead for the common small-table case. Buffers retained
on the table free list are reused (avoids `new[]`/`delete[]` churn) and deallocated only when
too small for a growth.

## 3. Inline caching

### Per-table inline cache

Each table's `LTableExt` carries an 8-entry inline cache (when hash accesses occur). It is
indexed by a cheap bit-mix of the key's 64-bit payload:

```cpp
uint32_t ic_idx = static_cast<uint32_t>(key.payload.u64
                 ^ (key.payload.u64 >> 17) ^ (key.payload.u64 >> 33)
                 ^ (key.payload.u64 >> 5)  ^ (key.payload.u64 >> 11))
                 % LTABLE_IC_SIZE;  // 8
```

On a hit (key payload matches, `table_ver` matches `hash_version`, entry non-nil), the hash
probe is skipped entirely:

```lua
-- First access: full hash probe
local x = obj.value

-- Subsequent accesses: single payload + version check
local y = obj.value  -- Cache hit if same key and no structural mutation
```

**Hash version guards**: Tables increment `hash_version` only on structural changes (inserts
and deletes that alter the hash probe sequence), not on value-only overwrites. This means
read-then-write patterns (e.g., `t[k] = t[k] + 1`) hit the cache on the read even when the
value is overwritten, because the structural shape hasn't changed.

**Works for any hash key**: The inline cache operates on raw key payload bits, not on
identifier names. It works for globals, locals, function parameters, and computed string
keys alike.

### Fast-path / slow-path `table_get` split

The hot `table_get` path is kept inline: integer/double array indexing first, then the inline
cache. If both miss, the call falls through to an out-of-line `table_get_slow` that performs
the full hash probe and metamethod (`__index`) dispatch. This keeps the common numeric/array
read on the inline path while moving the rare hash-and-metamethod machinery out of the hot
inline function, reducing code bloat and improving the fast path's cache footprint.

## 4. SIMD runtime scans

`ValueType` is a `uint8_t` enum, so 16 type tags fit in a single 128-bit SIMD register and 32
fit in an AVX2 register. The runtime uses AVX2 (32-byte) with SSE2 (x64) or NEON (ARM64)
fallbacks to accelerate hot type-array scans:

| Site | What it does |
|---|---|
| `rawlen()` | Finds first nil in table array — determines array length |
| `next()` | Finds first non-nil after a given index |
| `table_concat` validation | Validates all elements are String/Double/Int64 in 32/16-byte chunks |
| GC mark loop | Skips nil entries in the array part, only marking non-nil values |

### Bitmap-based hash part scanning

Each `LTableExt` carries a `hash_bitmap` recording which hash slots are occupied. This makes:

- `next()` hash-part iteration skip empty slots directly (no per-slot probe);
- GC hash-part marking scan only occupied slots;
- hash occupancy checks O(1).

The bitmap is maintained on insert, resize, and delete, and SIMD-accelerated scans read it in
32-byte chunks with scalar tails for non-aligned sizes. Portability: AVX2 where available,
else SSE2/NEON, else scalar.

## 5. String optimizations

### StringArena + StringPool interning

String storage is backed by a block-based **StringArena** (64 KB blocks, bump-allocated) inside
the open-addressed `StringPool` hash map. Each pool slot owns a baked allocation:

```text
[uint16 hash hi][uint32 len][char data...\0]
[  8 bytes  ][ 4 bytes ]
```

Benefits:

- One probe on hit
- No `std::string` overhead
- No side map or double lookup
- Bump allocation from the arena — no per-string `new[]`
- Hash is pre-computed and baked into the allocation header

### Baked hashes (16-byte header)

For interned strings, the 64-bit wyhash is baked into the allocation header 16 bytes before
the data pointer (`ptr[-16..-9]`). Reading the hash costs a single 8-byte load — zero recompute:

```cpp
uint64_t h = string_baked_hash(str_ptr);  // Single load
```

### 64-bit wyhash

The hash is a 64-bit wyhash (`wyhash64`) using compile-time constant secrets and 128-bit
multiply (`clx_umul128` / `_umul128` on MSVC) for excellent avalanche. Short strings
(≤ 8 bytes) use a SWAR path (`swar_hash_8`) that hashes the raw bytes plus length in one
128-bit multiply; longer strings process 8-byte chunks with a safe tail.

### StringBuilder (O(n) concatenation)

Avoids the O(n²) quadratic blow-up of repeated `s = s .. part` patterns:

```lua
-- Slow: O(n²) with naive concatenation
local s = ""
for i = 1, 1000 do s = s .. tostring(i) end

-- Fast: O(n) with StringBuilder (used internally by codegen)
local parts = {}
for i = 1, 1000 do parts[i] = tostring(i) end
local s = table.concat(parts)
```

The codegen emits `StringBuilder`-based concatenation for multi-part string expressions,
producing a single interned string with baked hash. StringBuilders are also arena-backed.

### Pre-computed hashes for compile-time string literals

The codegen pre-computes the wyhash for compile-time string literals (via `swar_hash_8` for
short strings) and emits it as a constant, avoiding repeated runtime hashing of the same
literal at every access.

### Pre-allocated interning

`intern_preallocated()` adopts a pre-formatted buffer directly into the StringPool, cutting
string concat from 3 heap allocations to 1 (or 0 on pool hit).

## 6. Code generation optimizations

### Loop transformations

Numeric for loops are transformed to native C++ for loops, with `int64_t` counters when the
optimizer proves integer bounds:

```lua
-- Lua source
for i = 1, 100 do
    print(i)
end

-- Generated C++ (integer bounds known)
for (int64_t i_val = 1; i_val <= 100; i_val++) {
    std::cout << i_val << std::endl;
}
```

Generic for loops emit direct `LCFunction` pointer calls in the loop body, avoiding
indirect call overhead. Inline `ipairs(t)` patterns are detected and emitted as direct
`while` loops over `table_get_int` with no iterator function calls.

### Branch prediction hints

Fast paths are annotated with `[[likely]]` attributes and hot runtime helpers are marked
`CLX_INLINE_HOT` (`always_inline`/`__forceinline`):

```cpp
if (a.type() == LType::Integer && b.type() == LType::Integer) [[likely]]
    return LValue(a.as_integer() + b.as_integer());
```

### Inlining

Small functions are inlined at compile time through C++ compiler optimizations (-O3).
All arithmetic operators (`add`, `sub`, `mul`, etc.) are marked `CLX_INLINE_HOT`
with `always_inline` attributes.

### Tail-call optimization

The `CLX_MUSTTAIL` attribute enforces guaranteed tail calls on supported compilers:

- **Clang**: `[[clang::musttail]]` via `__has_builtin(__builtin_musttail)`
- **GCC**: `[[gnu::musttail]]` via `__has_cpp_attribute(gnu::musttail)`
- **MSVC / other**: TCO should be autodetected by the compiler

This is used for tail-recursive function calls and certain dispatch paths, converting O(n) stack usage into O(1) for tail-recursive patterns.

### SIMD vectorization

The C++ compiler can vectorize simple loops when using `-O3 -march=native`:

```lua
local function vector_add(a, b, n)
    for i = 1, n do
        a[i] = a[i] + b[i]
    end
end
```

Can be compiled to use AVX/SSE instructions for parallel addition. Default build includes
`-march=native` flag.

### Dead Code Elimination (DCE)

In executable mode (non-debug), clx enables function-section-level DCE:

- **gcc/clang**: `-ffunction-sections -fdata-sections -Wl,--gc-sections`
- **MSVC**: `/Gy /link /OPT:REF /OPT:ICF`

This eliminates unused functions and data from the final binary.

## 7. Link-Time optimizations

When using `-flto=auto` (Link Time Optimization), the compiler can:
- Inline functions across translation units
- Eliminate dead code across the entire program
- Perform whole-program analysis

```bash
clx program.lua -flto=auto -O3
```

Note: `-flto=auto` is enabled by default in release mode.

## 8. Runtime optimizations

### Per-function arena allocation (escape analysis)

The optimizer performs escape analysis on table constructors. Tables that never escape their
function (no return, no assignment to globals/upvalues/fields, no passing to calls, no use in
growing loops) are allocated from a per-function bump-pointer arena instead of the GC heap:

- The optimizer computes `arena_safe_table_nodes` and a byte budget per function
  (`arena_table_sizes`), covering the `LTable` header, array part, type part, hash part, and
  `LTableExt`.
- The codegen emits `clx::arena_init(&_arena, size)` at function entry and routes qualifying
  `table_create` calls through `arena_create_table`, which placement-news the whole table
  (header + array + types + hash + ext) into one arena blob.
- Tables that might escape (assigned to function parameters, returned, captured, or stored in
  fields) are excluded from the arena and allocated normally.

This eliminates per-table `new`/`free` for the hot temporary-table case and improves locality.

### Table pre-sizing

Tables with known structure are pre-allocated:

```lua
local t = { x = 0, y = 0, z = 0 }  -- Pre-sized to 3 elements
```

### Upvalue fast-path

Closure variables that aren't captured are stored directly, avoiding heap allocation.

### Metamethod caching

Frequently used metamethod strings (`__index`, `__newindex`, `__gc`, `__call`, `__len`,
`__tostring`, `__pairs`) are pre-interned in LState at initialization, avoiding repeated
string interning on every dispatch.

### Metatable list and permanent roots

Tables with metatables are linked into a GC metatable list (`meta_next`), so the mark phase
scans only metatabled tables for finalizer references instead of every table. Permanent roots
(a small set of long-lived objects) are marked directly each cycle, and the string/io
metatables and default io handles are rooted so they are never collected. VM proxies are
registered with the clx GC and paced to the embedded VM's collection.

### Length operator optimization

String length is read from the baked allocation header, avoiding `strlen` entirely. For tables
with dense arrays, `#` returns `array_size` directly; tables with known constructor lengths
and no dynamic mutation are folded to a compile-time constant (see Known-length `#t` above).

### Table arithmetic (table_op)

The `+=`, `-=`, `*=`, `/=` operators on table fields (`t[k] += n`) are handled by a
templated `table_op` helper in the runtime. It inlines the get-type-check-set sequence
for `Double`, `Int64`, and `Nil` cases, falling back to full `clx::table_get`/`clx::table_set`
for metamethod or non-numeric values. The codegen emits `table_increment`, `table_decrement`,
`table_multiply`, or `table_divide` via `emitTableOp`, avoiding duplicated emit logic for
each compound assignment operator.

## Optimization levels

### Debug mode (`--debug`)
- No optimizations (`-O0`)
- Full debug symbols (`-g`)
- `#line` directives in generated C++ map every statement to the original Lua file and line, enabling GDB/LLDB source-level debugging of `.lua` files
- Slow-path code generation
- DCE disabled

### Release mode (default)
- `-O3 -flto=auto -fno-rtti -fvisibility=hidden`
- All optimizations enabled
- Dead code elimination
- Binary stripped (`-s`)

## Performance Tips

1. **Use local variables**: Faster than global access
2. **Prefer integer arithmetic**: `1 + 2` is faster than `1.0 + 2.0`
3. **Avoid mixed types**: `1 + "2"` is slower than `1 + 2`
4. **Use for loops over while**: Better optimization potential
5. **Enable compiler flags**: `-O3 -march=native -flto=auto` for maximum performance
6. **Use string interning**: Repeated string literals are automatically interned

```bash
# Maximum performance
clx program.lua -O3 -march=native -flto=auto
```

## Compiler remarks

### GCC/Clang (Linux/macOS)

Default release flags:
```
-O3 -flto=auto -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections
-Wl,--gc-sections -s -ldl
```

### MSVC (Windows)

Default release flags:
```
/O2 /Ot /GL /GR- /MD /EHsc /GS- /fp:fast /Gw /Gy
/link /OPT:REF /OPT:ICF
```

Key MSVC optimizations:
- `/GL` - Whole program optimization (LTO equivalent)
- `/OPT:REF` - Remove unused functions (/ link)
- `/OPT:ICF` - Identical COMDAT folding
- `/Gy` - Function-level linking (for /OPT:REF)
- `/arch:AVX2` - Enable AVX2 SIMD instructions (opt-in, not in defaults)
- `/fp:fast` - Fast floating-point semantics

### Cross-Platform tips

- Use `-O3 -march=native -flto=auto` on gcc/clang for maximum performance
- On MSVC, `/O2` is the primary optimization flag; `/GL` enables link-time optimization
- Both compilers support SIMD vectorization when loops are simple enough
- Dead code elimination requires function-level linking on both platforms
