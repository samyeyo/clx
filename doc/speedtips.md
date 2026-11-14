# Speed Tips

clx compiles your Lua to C++, and the C++ compiler does the rest. The catch:
whatever the clx optimizer cannot *prove* about your code stays boxed — 16-byte
`LValue`s, runtime dispatch, generic calls. This page lists the patterns that
unlock the native fast paths and the habits that silently disable them.

Every claim below can be inspected: compile with `--cpp` and read the generated
C++ for yourself.

```bash
clx prog.lua --cpp        # writes prog.cpp to the current directory
```

## Numbers

A local that is *provably* numeric becomes a plain native `double` (or exact
`int64_t`) — no boxing, no runtime calls, full C++ optimizer visibility.

```lua
local x = 1.5        -- native double
local s = x * 2 + 1  -- native arithmetic, inlined by clang

local y              -- unknown
if cond then y = 1 end  -- still unknown: not every path assigns a number
```

Rules that keep a local numeric:

- Initialize it from a number or numeric expression.
- **Never assign a non-number to it.** One `y = "oops"` anywhere permanently
  disqualifies the name — even if every later assignment is a number again.
  Use a fresh name for the other type.
- Don't close over it. A local that is *captured* by a nested function becomes
  an upvalue and loses numeric treatment — pass it as a parameter instead.
- Don't compare it with `nil`. Inside a function, `if x == nil` de-proves the
  parameter `x` for the whole body. Validate at the call site, or split into a
  wrapper.

Numeric `for` variables are always native:

```lua
for i = 1, n do  -- i is a native loop counter
    acc = acc + i * i
end
```

Variables from `ipairs`/`pairs` are not (the loop can yield anything). If you
need a numeric iteration variable, use a numeric `for`.

Integer semantics match Lua 5.5 exactly — `math.maxinteger + 1` wraps to
`math.mininteger`, and the optimizer keeps an exact 64-bit shadow so the wrap
is not lost to double rounding. Use `//` for floor division that must stay
integer.

## Functions

A function whose parameters are used arithmetically and whose returns are all
numbers compiles to a native C++ function — a direct `double f(double, …)`
call with zero dispatch:

```lua
local function mix(a, b)   -- both params proven numeric, returns a number
    return a * 0.75 + b
end
```

To keep that status:

- **Call it with numbers, with the same arity, at every call site.** One call
  with a string argument — or with a missing/extra argument — de-proves the
  parameter everywhere.
- **Don't let the function escape.** Storing it in a table, returning it, or
  passing it as a callback makes the optimizer give up on all its parameters.
  Call it directly.
- Don't reassign the function name.
- **Don't inspect a parameter with `math.type`.** Asking for the subtype forces
  that parameter to stay boxed for the whole body — check the values at the call
  site instead.
- Returning 2–4 numbers consistently (`return a, b`) gets a native tuple call
  instead of a multi-value box.

## Tables

### Numeric arrays

A table that only ever holds numbers with integer keys is stored as a
`std::vector<double>` — indexing is a pointer offset, not a hash/array slot
lookup:

```lua
local t = {10, 20, 30}   -- pure numeric array
local x = t[2]           -- native load
```

- The constructor form (or writes that the optimizer can see) must be
  all-number, integer-keyed. A string key or a non-number value disqualifies.
- **Don't pass the array to a function as an argument** if you want the vector
  representation — the optimizer cannot see through the call, so the table
  falls back to the generic boxed layout. Inline the work, or use the
  record-field knowledge below.
- `#t` folds to a compile-time constant when the table is constructor-built
  and never grown. `table.insert`/`table.remove` (including through an
  alias), `t[#t+1] = v`, and `setmetatable` all mark the length as dynamic,
  disabling the fold.
- `ipairs` over a pure numeric array lowers to a plain C++ `for` loop — no
  iterator closures, no triplet protocol. Keep `ipairs` the global builtin;
  aliasing or shadowing it disables this lowering:

```lua
local t = {1, 2, 3}
local sum = 0
for _, v in ipairs(t) do   -- native loop over vector<double>
    sum = sum + v
end
```

### Record tables

A table of named numeric fields gets field-level proof — field reads become
direct loads instead of `table_get` + subtype checks:

```lua
local p = {x = 1.0, y = 2.0, z = 3.0}
local d = (p.x - q.x) ^ 2 + (p.y - q.y) ^ 2   -- native field loads
```

This also works through array elements the optimizer can track:

```lua
for i = 1, #bodies do
    local b = bodies[i]    -- bodies[i].x / .y proven numeric
    b.vx = b.vx + acc
end
```

Field writes with a non-literal key, or writing a non-number into a proven
numeric field, disqualify the whole table.

## Standard library calls

Direct module calls and local aliases lower to C++ intrinsics or direct
runtime calls — no table lookup, no dispatch:

```lua
local v  = math.sqrt(x)          -- std::sqrt
local s  = string.format("%d", n) -- direct runtime call
local n2 = #list + #map           -- constant-folded when lengths are known

local sqrt = math.sqrt            -- alias: same lowering, don't reassign
local a = sqrt(x1) + sqrt(x2)
```

- Constants fold at compile time: `math.huge` → an `INFINITY` literal,
  `math.pi` → a double literal, `math.maxinteger`/`math.mininteger` → int64
  literals.
- `math.floor`, `string.*`, `table.insert/remove/concat/sort` and friends all
  have direct-call entries; `math.floor` in particular stays an integer-
  subtype-preserving runtime call rather than `std::floor`.
- **Prefer the module form or an alias in hot code.** Method syntax
  (`s:sub(i, j)`, `seq:gsub(...)`) works and costs about the same as stock
  Lua — the call resolves through the string metatable at runtime — but it is
  *not* intrinsified. In a tight loop, `string.sub(s, i, j)` or
  `local sub = string.sub` is the faster spelling.
- **Don't monkey-patch the standard library in a file you want optimized.**
  Assigning `math.sqrt = f` disables the optimizer's knowledge of `math` for
  the entire compilation unit — direct calls stop being intrinsified.
- Aliases must not be reassigned: `local floor = math.floor; floor = something_else`
  reverts every `floor(...)` call to generic dispatch.

## Strings

Repeated concatenation into a local is recognized and compiled to an
in-place `StringBuilder` — no quadratic copying:

```lua
local out = ""
for i = 1, n do
    out = out .. pieces[i]   -- appended in place
end
```

For joining an array of pieces, `table.concat(pieces)` is a single direct
call. `string.format` for templating.

## Loops and calls

- Prefer numeric `for` over `while` with a manual counter — the bounds are
  visible to the optimizer.
- Keep call sites fixed-arity. Vararg calls (`f(t, ...)`) go through a dynamic
  argument buffer (`clx::DynArgs`) that grows on the heap as needed, so a call
  with an unpredictable argument count stays correct — it just isn't unboxed.
- **Hoist globals out of loops.** A global read is an environment-table lookup
  on every access:

```lua
local log = math.log            -- hoist
for i = 1, n do
    acc = acc + log(x[i])       -- local alias, direct call
end
```

## Inspect the lowering

When in doubt, read the generated C++:

```bash
clx prog.lua --cpp
grep -c 'call_function_rooted' prog.cpp   # generic dispatch — should be few
grep -c 'std::sqrt\|std::sin\|std::cos' prog.cpp  # math intrinsics
grep 'vector<double>' prog.cpp            # pure numeric arrays (in runtime headers)
```

A `call_function_rooted` in a hot loop is the signature of a lost fast path —
check what the callee is and which of the rules above it violates.

Build with `--fast` to link the speed-optimized runtime (the default `--size`
runtime trades some inlining for binary size).

## Checklist

- Numeric locals hold numbers from start to finish — nothing else.
- Numeric helpers are called directly, with numeric args, fixed arity — and
  never stored in tables or passed as callbacks.
- Hot arrays are constructor-built, all-number, and not passed into functions.
- Hot records use fixed numeric field names.
- Builtins are called as `math.f(...)` / `string.f(...)` or through a
  never-reassigned alias — not monkey-patched, not aliased and then reassigned.
- String methods in tight loops use the module form, not `s:method()`.
- Globals used in loops are hoisted to locals.
- `#t` used in hot code is on a table that is never grown or re-metatabled.
