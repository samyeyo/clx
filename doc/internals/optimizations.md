# clx Optimizations

One idea drives everything here: **if the compiler can prove a value's type, it emits
plain C++ for it.** `a + b` on two proven numbers becomes `l_a + l_b` — no boxes, no
checks, no calls. When the proof fails, you get the safe generic path (`clx::add`),
which behaves exactly like the interpreter. Every section below is either a smarter
proof (optimizer) or a cheaper generic path (runtime).

Tip: compile any example with `clx prog.lua --cpp` and read the generated C++ next
to the Lua. That is the fastest way to understand this page.

## 1. Compile-Time optimizations

### Constant folding

Arithmetic on literals is left for the C++ compiler, which folds it for free:

```lua
local x = 1 + 2 + 3  -- the C++ compiler turns this into 6
```

Literal string concatenation is *not* folded — it emits a `StringBuilder` fill that
copies the pieces in at runtime (and the C++ compiler often folds that anyway).

### Numbers without boxes (numeric fast-path)

Lua has two number kinds: integers (`1`, exact) and floats (`1.5`, approximate).
`LValue` keeps them apart with a tag, and the optimizer tracks which variables hold
which. Proven integers become C++ `int64_t`, proven floats become `double`, and the
arithmetic between them is plain C++ — the `LValue` box never appears:

```lua
local i = 1 + 2      -- int64_t arithmetic, exact like Lua integers
local f = 1.5 + 2.3  -- double arithmetic
local m = 1 + 2.5    -- mixed: the integer converts to double, like Lua
```

If the optimizer *can't* prove the operands are numbers, you get the generic path
(`clx::add`), which checks types at runtime. Same result, just slower. When you read
"yields a number" in the code, it means "proven numeric, box skippable".

### Native integer tracking

Beyond plain numeric inference, the optimizer tracks which locals, function parameters, and
function return values are **integer**-typed (`int64_t` rather than `double`). This enables:

- `int64_t` loop counters for numeric `for` loops whose controls are proven numeric
  (on this path there is no floating-point counter):

```lua
-- Generated C++ (start/limit/step all proven numeric):
-- for (int64_t i_val = 1; i_val <= n; i_val++) { ... }
for i = 1, n do
    ...
end
```

  Controls that cannot be proven numeric at compile time dispatch at runtime on the
  control types, like stock `OP_FORPREP`: an integer loop when start/limit/step are
  all integers, a `double`-counter float loop otherwise. A runtime-integer limit (e.g.
  a bare parameter holding an integer) therefore keeps an integer loop variable —
  `math.type(i)` and `tostring(i)` match stock either way.

- unboxed integer locals (`int64_t l_x;`) instead of `double` when the optimizer proves the variable is only assigned integer values;
- integer-returning functions whose result is used directly in integer arithmetic;
- native `//`, `%`, bitwise, and shift operators when both sides are integer-typed.

The optimizer records `int_returning_funcs`, `int_typed_locals`, and `native_integers` in the analysis state (`analysis_state.h`), and `is_purely_integer_expr()` proves integer-ness for whole expressions so arithmetic stays in the integer domain end-to-end.

### Only lower a local when every assignment yields a number

A numeric fast path is only as sound as the coldest assignment to the variable. A
local that is numeric at one site but can later receive `nil`, a string, or a table
must stay boxed, otherwise the emitted `double` would have to fake a value:

- a later assignment of `nil`, a string, or an arbitrary expression keeps the local
  boxed (`l_x` as an `LValue`), so `x = nil` still reads back as `nil`;
- a parameter whose type the program inspects with `math.type`, or that is handed to a
  metamethod-capable operation, is not lowered to `double`;
- a local table that a call can reach keeps its boxed representation;
- vararg tables and `math.maxinteger` constants are tracked explicitly so a
  `double` slot never rounds them.

The analysis lives in `analysis_state.h` and the passes in `numbers.cpp` /
`functions.cpp`; when the proof fails, codegen simply emits the general `clx::` path.

### Int64 arithmetic: wrap-on-overflow

Integer arithmetic (`add`, `sub`, `mul`) with two `Int64` operands uses native C++ integer
operations computed in `uint64_t` (well-defined wrap-around), staying in the integer domain —
matching stock Lua 5.5, which wraps: `math.maxinteger + 1 == math.mininteger`,
`-math.mininteger == math.mininteger`, `math.mininteger // -1 == math.mininteger`.

- **add / sub / mul**: raw two's-complement wrap via `int_add_lv`/`int_sub_lv`/`int_mul_lv`
- **unm (`-x`)**: wraps (`-math.mininteger == math.mininteger`)
- **`//` (idiv)**: integer floor division; `b == -1` wraps `INT64_MIN` to `INT64_MIN`
- **`%` (mod)**: Lua floor-modulo semantics (result takes the divisor's sign); `b == -1` returns 0
- **`pow`, and mixed int/float operands**: promote to double (float domain)

Do not reintroduce promote-on-overflow for int64 ops — a July 2026 fix that promoted on
overflow was factually wrong and was replaced with wrapping.

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

The `pass_table_fields` optimizer pass detects `local bi = bodies[i]` where `bodies` is a
function parameter, then traces field accesses (`bi.x`, `bi.y`) in BinaryOp nodes to infer
numeric fields. Both the parameter name and the local name are registered in
`numeric_table_fields` (on the analysis state) so `yields_number()` proves field reads yield
numbers.

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

### Struct-local promotion (native record structs)

A `local p = {x = 1, y = 2}` whose every use is a literal-key read (`p.x`) or write
(`p.x = ...`) in the same scope promotes to a native C++ struct instead of an `LTable`
(`pass_struct_locals`, `src/optimizer/structs.cpp`). Field kinds (`Int`/`Double`/`Str`)
commit monotonically through a fixpoint over constructor values and writes; fields
assigned after construction get a `has_` presence flag. Shapes are keyed by ordered
field/kind/late layout (`struct_shape_key`) and emitted once per distinct shape as e.g.
`struct S_x_i_y_i { int64_t x; double y; };` — same-TU name collisions from different
layouts get `_r` suffixes, and every struct lives in an anonymous namespace so
same-named shapes in different translation units never violate ODR. Field names that
are not valid C++ identifiers (including keywords like `class`) reject the candidate
back to a plain table instead of being mangled.

```lua
local v = {x = 1, y = 2, z = 3}
local area = v.x * v.y  -- native int64_t load + multiply, no table access
```

### Direct table field write (`table_set_direct`)

When the optimizer proves a record field exists (from `numeric_table_fields`), the codegen
emits `clx::table_set_direct(L, tbl, key, val)` instead of the general `table_set`. The direct
helper calls `settable()` directly, skipping `set_value()`'s redundant `gettable()` existence
check.

### Pure numeric array promotion

The optimizer promotes tables whose array part provably holds only numbers into
`pure_numeric_arrays`. The fast paths work **in place** on the existing `LTable`
array/type layout — a runtime type check with a generic `clx::table_get`/`table_set`
fallback — never by converting the table to a separate `std::vector` representation,
so nil holes and the integer subtype keep their semantics. This includes:

- numeric array constructors (`local t = {1, 2, 3}`) — verified element-wise;
- record-like constructors (`local v = {x=1, y=2, z=3}`) — numeric string-keyed fields are
  registered in `numeric_table_fields` so `v.x + v.y` emits native arithmetic.

Empty tables filled in a loop (`local t = {}; for i = 1, N do t[i] = v end`) are
*deliberately not* promoted: an earlier version did promote them, but reads took a fast
vector path while writes still went through generic `table_set`, which regressed
write-heavy loops (fannkuchredux, fasta). They stay plain tables until a matching
store fast path exists.

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

struct LTable : public LHeader {       // hot array fields sit in cache line 1 after LHeader
    TValue *array;                      // co-allocated block base (or small_array)
    ValueType *array_types;             // points inside the block, never freed separately
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

### Co-allocated array block

Larger array parts use one malloc holding `TValue[cap]` immediately followed by
`ValueType[cap]` (`table_alloc_array`/`table_free_array` in `include/clx_runtime.h`), instead
of two separate `new[]` allocations. Zeroed memory is already initialized (`TValue` u64=0,
`Nil`=0), so `calloc` replaces `new[]`+value-init. The split SoA layout is preserved — the
dense `ValueType` array keeps its SIMD type scans — but the two tag/payload lines are now
adjacent and every array alloc/grow pays one malloc/free instead of two. `table_heap_bytes`
is unchanged (`cap * (sizeof(TValue) + sizeof(ValueType))`), so GC pacing is unaffected.
Measured neutral on `binarytrees`/`fasta`/`hashtable` (within run-to-run noise); the win is
structural — half the allocator traffic on array growth, better locality under pressure.

### Fused hash block

Hash storage uses one `calloc` holding `entries[cap]` immediately followed by the occupancy
bitmap words (`table_alloc_hash`/`table_free_hash` in `include/clx_runtime.h`), instead of two
separate `calloc`s. `entries` sits at offset 0, so freeing it frees the whole block and
`hash_bitmap` is never freed separately (`cap * 24` is always 8-byte aligned, so no padding).
`table_heap_bytes` is unchanged (same summed bytes), so GC pacing is unaffected. Arena tables
keep their existing behavior (arena-owned entries + null bitmap until first resize, then a
heap fused block takes over and clears the flag). As a side effect, the GC finalizable drain
now releases the bitmap together with entries (previously leaked) and nulls the pointer, so a
later resize of a recycled table cannot double-free a stale bitmap. Measured neutral on
`canada` (0.132 vs 0.130–0.137, within noise) — tiny `calloc` pairs are nearly free from the
thread cache; the win is structural, like the array block above.

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

**Hash version guards**: `hash_version` is the *sole structural invalidator* of the inline
cache, and the only hash-part mutation that bumps it is a rehash (which relocates entries).
Plain inserts and deletes never move entries — key-payload + type checks reject stale slots —
so they intentionally leave the version alone; metatable set and table recycle also bump it.
Value-only overwrites never bump it. This means read-then-write patterns (e.g.,
`t[k] = t[k] + 1`) hit the cache on the read even when the value is overwritten, because the
structural shape hasn't changed.

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

The bitmap is maintained on insert, resize, and delete. Iteration is scalar: a 64-bit word
loop with `ctz` to skip empty runs (`hash_bitmap` scans in `next()` and the GC). SIMD is used
for the dense `ValueType` type-arrays above, not for the bitmap.

## 5. String optimizations

### StringArena + StringPool interning

Every distinct string is stored once in the `StringPool` (an open hash map), with
memory bump-allocated from 64 KB `StringArena` blocks — no per-string `new[]`, no
`std::string` overhead, one probe on hit. Each entry carries a 16-byte header in
front of the characters holding the pre-computed hash and length, so both are a
single 4-byte load instead of a recompute. The hash itself is 64-bit wyhash, with a
one-register fast path for short strings.

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

Literals known at compile time get their hash computed once by the compiler and
emitted as a constant — the runtime never re-hashes them.

### Pre-allocated interning

`intern_preallocated()` adopts an already-formatted buffer straight into the pool,
cutting concatenation from 3 allocations to 1 (or 0 on a pool hit).

### `%s*()` pattern fast path

`string.find` with the exact pattern `%s*()` (used at every JSON token boundary by
`dkjson`) skips `MatchState` setup, `max_expand`, and capture handling, and runs a bare
whitespace scan instead. `std::isspace` matches the pattern class table exactly, so the
match is identical and failure/edge behaviour (including `init > len`) is untouched.

### Pooled keys for global reads/writes

Global names are interned once at compile time, so generated code looks them up by a
pooled key instead of re-hashing the name on every single access.

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

`for _, v in ipairs(t)` over a promoted numeric array becomes a plain C++ loop —
no iterator function, no per-step triplet. Any other generic-`for` emits direct
`LCFunction` pointer calls in the body instead of going through call dispatch.

### Loop-invariant table header hoisting

If a local is *provably always a table* (one constructor, never reassigned, never
captured), indexing it inside a loop skips the "is this really a table?" check every
time: the emitter grabs the `LTable*` pointer once before the loop and reuses it.
Tables created fresh inside the loop, captured by closures, or skipped by a `goto`
keep the checked path. Wrong guess here would be a bug, so the rule is conservative —
when unsure, it just doesn't hoist.

```lua
local t = {}
t.tag = 0
for i = 1, n do
    sum = sum + t[i]   -- pointer derived once, not per iteration
end
```

### Snapshot roots keep locals in registers

Background: to stay safe from the garbage collector, every local is normally *rooted*
— the GC gets a pointer to its storage. But taking a local's address forces it out of
CPU registers into memory, which can cost ~2x in tight loops.

The fix: if a local is never reassigned, the GC only needs to *read* it, so codegen
roots a throwaway *copy* (snapshot) instead of the local itself — the real local stays
in a register. A second check (`scan_own`) makes sure no use passes the local by
reference to a helper call (which would force it into memory anyway and make the copy
pure overhead). Anything ambiguous keeps the plain root. `tests/regression/local_root.lua`
covers the tricky cases (GC as the only reference holder, capture, shadowing, `goto`).

### Unrooted locals (`pass_nogc_locals`)

One step further: a local that can only ever hold numbers, booleans, nil, or strings
needs *no* root at all — the collector only traces tables, threads, functions, and
userdata, and strings are never collected. The optimizer proves this per local (every
assignment reaching it must be "clean": literals, comparisons, arithmetic on clean
operands, numeric reads…; constructors, closures, globals, and captured variables
disqualify). Exempt locals emit no `_fr.push` whatsoever. The speedup today is small
(pushes were already cheap), but frames get smaller and the analysis unlocks bigger
per-call rooting cuts later.

### Tuple returns instead of `MultiValue` (`ClxTup`)

Returning several values normally means building a small dynamic container
(`MultiValue`: values + a count) and indexing into it at the call site. When the
optimizer can prove a function always returns 2–4 *numbers*, codegen instead returns
a fixed plain struct — `struct ClxTup2 { double v0; double v1; };` (and `...3/4`).
Raw `double` fields, no boxing, no count dispatch, and callers doing
`local x, y = f(...)` bind the fields directly. Anything that doesn't fit the shape
(globals, varargs, closures, recursion, non-numeric slots) quietly keeps the ordinary
`MultiValue` path.

### Builtin registry and intrinsic call lowering

Statically known std functions live in one registry (`src/builtins.h`: `BuiltinEntry`
per `module.name` — raw C++ expression for unboxed lowering, runtime `cfunc` symbol,
arity floor, and whether the result is always numeric). The optimizer uses it to prove
numeric results and the codegen uses it at every call shape:

- alias locals (`local strformat = string.format`) emit `clx::call_cfunc_rooted` instead
  of generic dispatch, including in `return` position;
- intrinsics lower to raw C++ expressions in double contexts (no `LValue` boxing);
- eligible calls emit unboxed-direct triple-bound `(double, int-flag, int-shadow)`
  overloads that skip boxing at the boundary;
- static lowering is write-guarded (`builtin_field_ok`): any assignment to the module
  field anywhere in the file disables it for that key.

### Exact integer/float comparisons

Comparisons whose integer operand may not be representable as a `double` do not go
through a `double` conversion. `==`, `~=`, `<`, `<=`, `>` and `>=` follow stock's
`LTintfloat`/`LEintfloat`/`luaV_equalobj` rules (`values.cpp`), so
`math.maxinteger < math.maxinteger - 1` and `2^63 > math.maxinteger` answer like the
reference implementation instead of comparing two rounded values.

### Growable call arguments (`DynArgs`)

A call whose argument list is only known at run time (a vararg expansion, or a
multi-value expression in a non-final position) builds its argument vector in
`clx::DynArgs` — a small inline buffer that grows on the heap when needed — instead
of the fixed 16-slot buffer that used to cap such calls.

### In-place integer-indexed array fast paths

Comparisons and stores on integer-indexed reads of hoisted tables stay on the existing
`LTable` array layout instead of building boxed values (`try_emit_array_cmp`,
`bce_safe_nodes` store path): bounds-checked raw payload compares (`i64` for
Int64/Int64, doubles otherwise, generic `clx::lt`/`clx::le` fallback for holes and
out-of-range keys) and direct payload+tag copies on `dst[i] = src[j]`. Both are gated
on provably-integer keys and hoisted table pointers, so `--cpp` output grows but
semantics do not change.

### Small peepholes

- Dead literal tables: a single-occurrence literal-only table declaration that is never
  observably read (`dead_stmts`) is dropped instead of emitted.
- Zero-index fusion: stores to a fresh `{__index = 0}`-metatable local
  (`zero_index_tables`) fuse the `t[k] = t[k] <op> const` sequence, skipping `__index`
  dispatch on a known miss.

### Throw-free fast bodies (deferred stack overflow)

A fast function is emitted as a Y-combinator lambda whose self-recursion is a direct
`self(self, …)` call, so it bypasses the call wrappers' depth check and needs a guard
of its own. A guard that *throws* makes the lambda unwind-capable, which blocks
shrink-wrapping and forces a frame on every leaf call: measured 41–43 ms on `fib(34)`
against 22.4 ms with no guard at all (1.9×).

The body therefore contains no call that can throw. On overflow it records the site
once and returns:

```cpp
if (clx::stack_probe() < _fr.owner->stack_low) [[unlikely]]
{
    if (!_fr.owner->stack_overflow_site)
        _fr.owner->stack_overflow_site = _fr.owner->current_site;
    return 0.0;
}
```

The enclosing `_fast_<name>` wrapper — entered once per Lua-level fast call and never
on self-recursion — clears any stale site, then calls `clx::raise_stack_overflow`,
which restores `current_site` to the recorded frame and throws. That costs 1.05×
(23.6 ms) and still reports stock's `stack overflow` at the offending line.

The store must stay **inline** in the body's cold block: routing it through a
`cold`/`noinline` helper, a `thread_local`, or a helper taking the `LState*`
re-measured the same 1.9× penalty. Frames above the limit complete with `0.0` values,
so their `<close>` handlers all run before the error is raised.

### Branch prediction hints

Fast paths are annotated with `[[likely]]` attributes and cold paths with
`[[unlikely]]` (the stack guard above, the deferred-overflow check in the wrapper),
while hot runtime helpers are marked `CLX_INLINE_HOT`
(`always_inline`/`__forceinline`):

```cpp
if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]]
    return LValue(a.val.payload.i64 + b.val.payload.i64);
```

### Inlining

Small functions are inlined at compile time through C++ compiler optimizations (-O3).
All arithmetic operators (`add`, `sub`, `mul`, etc.) are marked `CLX_INLINE_HOT`
with `always_inline` attributes.

### Tail-call optimization

True guaranteed tail calls (`[[gnu::musttail]]`) only exist on GCC/x86, so clx uses
the `CLX_MUSTTAIL` macro at exactly one safe site and otherwise relies on the C++
compiler's own tail-call elimination: `return f(...)` evaluates into a temporary and
returns it. In practice the C++ compiler handles ordinary tail recursion fine.

### SIMD vectorization

Simple loops over arrays are written so the C++ compiler can auto-vectorize them
(AVX/SSE) under `-O3 -march=native`. The build bakes in a conservative arch flag by
default (`sse2` on x86, `native` on ARM); opt into AVX2 with `CLX_ARCH=avx2`.

```lua
local function vector_add(a, b, n)
    for i = 1, n do
        a[i] = a[i] + b[i]   -- simple enough for the C++ compiler to vectorize
    end
end
```

### Dead Code Elimination (DCE)

Each function goes in its own linker section, so the linker can throw away everything
the program never calls. That is most of the runtime for small programs — the main
reason hello-world binaries stay small.

## 7. Link-Time optimizations

`--fast` turns on LTO (`-flto=auto` outside Windows), letting the C++ compiler inline
and prune *across* translation units. `--size` (the default) skips LTO for faster
builds. Your own flags are appended after clx's, so the last `-O` wins.

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

All metamethod names (`__index`, `__add`, `__len`, … — the full list lives in
`state.cpp`) are pre-interned once at startup, so dispatch never re-interns them.

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

### Table arithmetic (`t[k] += n`)

Compound assignment on a field does get-check-set in one helper instead of three
separate calls, with fast lanes for numbers and a generic fallback for metamethods.

### Shared per-FILE* output batching

Every `fwrite()` takes two locks, so millions of one-char `io.write` calls burn most
of their time locking. `io.cpp` instead collects output in 8KB userspace buffers (one
per open file, shared between `print`/`warn`/`io.write`) and flushes when full — ~1.3x
on write-heavy benchmarks. Every read, seek, flush, close, and `setvbuf` flushes first,
so ordering and stock semantics are preserved; `CLX_IO_NOBUF=1` turns batching off.

## Optimization levels

Three profiles, picked with a flag. The tables below are the exact compiler flags for
reference — you don't need to memorize them, just know `--size` builds small and fast
to compile, `--fast` builds fully optimized binaries, and `--debug` keeps symbols and
turns optimization off.

The default link mode is `--size` (optimize for size). `--fast` opts into the speed profile;
`--debug` forces the debug profile.

### Debug mode (`--debug`)
- No optimizations (`-O0 -g` on gcc/clang; `/Od /Zi /MDd /EHsc /utf-8` on MSVC)
- Full debug symbols
- `#line` directives in generated C++ map every statement to the original Lua file and line, enabling GDB/LLDB source-level debugging of `.lua` files (emitted unconditionally, not debug-only)
- Same code generation as release — there is no separate slow-path codegen
- DCE disabled

### Size mode (default, `--size`)
- gcc/clang: `-O1 -foptimize-sibling-calls -fno-inline-functions -fvisibility=hidden`
- MSVC: `/O1 /Ob0 /GL /GR- /MD /EHsc /GS- /fp:fast /Gw /Gy /utf-8`
- Dead code elimination, binary stripped

### Speed mode (`--fast`)
- gcc/clang (non-Windows): `-O3 -flto=auto -fvisibility=hidden`
- gcc/clang (Windows): `-O3 -fvisibility=hidden` (no LTO)
- MSVC: `/O2 /Ot /GL /GR- /MD /EHsc /GS- /fp:fast /Gw /Gy /utf-8`
- Dead code elimination, binary stripped

The baked architecture flag (`sse2`/`avx`/`avx2`/`native` on x86, `native`/`generic` on ARM)
is appended to whichever profile is active, unless the user passes their own
`-march=`/`-mcpu=`/`-msse*`/`-mavx*`/`/arch:` flag.

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

The tables above already list every flag. Two things worth knowing: DCE (dead code
elimination) needs function-level linking (`-ffunction-sections`/`-Wl,--gc-sections`
on gcc/clang, `/Gy` + `/OPT:REF` on MSVC), and your own flags are appended *after*
clx's defaults, so the last `-O`/`/O` on the command line wins.
