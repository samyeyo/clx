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

## 2. Inline caching

### CacheSlot for table access

Each string-keyed table access site in the source gets a dedicated `CacheSlot`:

```cpp
struct CacheSlot {
    uint64_t table_val;     // Cached table pointer (raw bits)
    uint32_t shape_version; // Table shape version for staleness detection
    LValue    cached;       // Cached value (non-GC only)
    bool      valid;        // Is the cache valid?
};
```

On repeated access to the same table key, the cache skips the hash probe entirely:

```lua
-- First access: full hash probe
local x = obj.value

-- Subsequent accesses: single pointer + version check
local y = obj.value  -- Cache hit if same table and shape unchanged
```

**Shape version guards**: Tables increment `shape_version` on every write. CacheSlots check
the version to detect stale cached values, preventing incorrect reads after mutations.

**GC safety**: Only non-GC values (numbers, integers, booleans, nil) are cached to avoid
dangling pointers after garbage collection.

**Non-global tables**: CacheSlot works for any identifier table — globals, locals, and function
parameters — not just globals. This enables caching for patterns like `bodies[i].x` where
`bodies` is a function parameter.

## 2.5 SIMD runtime scans

`ValueType` is a `uint8_t` enum, so 16 type tags fit in a single 128-bit SIMD register.
The runtime uses SSE2 (x64) or NEON (ARM64) to accelerate hot type-array scans:

| Site | What it does |
|---|---|
| `rawlen()` | Finds first nil in table array — determines array length |
| `next()` | Finds first non-nil after a given index |
| `table_concat` validation | Validates all elements are String/Double/Int64 in 16-byte chunks |
| GC mark loop | Skips nil entries in the array part, only marking non-nil values |

Scalar tail handles remaining elements for non-16-byte-aligned sizes.
Portability: SSE2 (all x86_64), NEON (all ARM64), scalar fallback for others.

## 3. String optimizations

### StringPool interning

Open-addressed hash map for string interning. Each slot owns a baked allocation:
`[uint32_t hash][uint32_t len][char data...\0]`. LValue stores a pointer to the char data
(8 bytes past alloc start). Benefits:

- One probe on hit
- No `std::string` overhead
- No side map or double lookup
- Hash is pre-computed and baked into the allocation

### Baked hashes

For interned strings, the wyhash is baked into the allocation header at `ptr[-8..ptr[-5]]`.
Reading the hash costs a single 4-byte load — zero recompute:

```cpp
uint32_t h = string_baked_hash(str_ptr);  // Single load
```

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
producing a single interned string with baked hash.

### wyhash

Fast, high-quality hash function used for table keys and string interning. Uses compile-time
constant secrets with 128-bit multiply (`__uint128_t` or `_umul128` on MSVC) for excellent
avalanche. Processes 8-byte chunks with safe tail handling for 1-7 byte remainders.

### Pre-allocated interning

`intern_preallocated()` adopts a pre-formatted buffer directly into the StringPool, cutting
string concat from 3 heap allocations to 1 (or 0 on pool hit).

## 4. Code oeneration optimizations

### Loop transformations

Numeric for loops are transformed to C++ for loops:

```lua
-- Lua source
for i = 1, 100 do
    print(i)
end

-- Generated C++
for (double i_val = 1; i_val <= 100; i_val += 1) {
    std::cout << i_val << std::endl;
}
```

Generic for loops emit direct `LCFunction` pointer calls in the loop body, avoiding
indirect call overhead.

### Branch prediction hints

Fast paths are annotated with `[[likely]]` attributes:

```cpp
if (a.type() == LType::Integer && b.type() == LType::Integer) [[likely]]
    return LValue(a.as_integer() + b.as_integer());
```

This helps the compiler optimize branch prediction for the common case.

### Inlining

Small functions are inlined at compile time through C++ compiler optimizations (-O3).
All arithmetic operators (`add`, `sub`, `mul`, etc.) are marked `CLX_INLINE`
with `always_inline` attributes.

### SIMD vctorization

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

## 5. Link-Time optimizations

When using `-flto=auto` (Link Time Optimization), the compiler can:
- Inline functions across translation units
- Eliminate dead code across the entire program
- Perform whole-program analysis

```bash
clx program.lua -flto=auto -O3
```

Note: `-flto=auto` is enabled by default in release mode.

## 6. Runtime optimizations

### Table pre-sizing

Tables with known structure are pre-allocated:

```lua
local t = { x = 0, y = 0, z = 0 }  -- Pre-sized to 3 elements
```

### Table layout optimization

Tables use a cache-line-optimized layout:

- **Cache line 0** (64 bytes): array pointer, sizes, hash bucket, metatable, hash count
- **Cache line 1+**: parallel arrays for keys, values, and chain next pointers

All gettable fields fit in one 64-byte cache line for fast access.

### Upvalue fast-path

Closure variables that aren't captured are stored directly, avoiding heap allocation.

### Metamethod caching

Frequently used metamethod strings (`__index`, `__newindex`, `__gc`, `__call`, `__len`,
`__tostring`, `__pairs`) are pre-interned in LState at initialization, avoiding repeated
string interning on every dispatch.

### Length operator optimization

String length is read from the baked allocation header (`ptr[-4..ptr[-1]]`), avoiding
`strlen` entirely. For tables with dense arrays, `#` returns `array_size` directly.

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
