# Lua 5.5 Compatibility

clx aims for Lua 5.5.1 compatibility. The tables below summarize the current
status. clx is under active development, so this is a snapshot — real-world
testing is still ongoing. The `--dynamic` bridge embeds the 5.5.1 VM byte for
byte, unpatched.

## Core language

| Feature | Status |
| --- | --- |
| Variables | ✅ |
| Arithmetic operators | ✅ |
| Logical operators | ✅ |
| Comparisons | ✅ |
| Functions | ✅ |
| Closures | ✅ |
| `_ENV` | ✅ |
| Varargs | ✅ |
| Multiple returns | ✅ |
| Local variables | ✅ |
| Global variables | ✅ |

## Control flow

| Feature | Status |
| --- | --- |
| `if` / `elseif` / `else` | ✅ |
| `while` | ✅ |
| `repeat` / `until` | ✅ |
| Numeric `for` | ✅ |
| Generic `for` | ✅ |
| `break` | ✅ |
| `goto` | ✅ |
| Labels | ✅ |

## Tables

| Feature | Status |
| --- | --- |
| Table constructors | ✅ |
| Array part | ✅ |
| Hash part | ✅ |
| Mixed tables | ✅ |
| Table iteration | ✅ |

## Metatables

| Feature | Status |
| --- | --- |
| `__index`, `__newindex` | ✅ |
| `__add`, `__sub`, `__mul`, `__div` | ✅ |
| `__mod`, `__pow`, `__unm` | ✅ |
| `__len`, `__concat` | ✅ |
| `__eq`, `__lt`, `__le` | ✅ |
| `__call`, `__tostring` | ✅ |
| `__pairs` | ✅ |
| `__ipairs` | ✅ (legacy Lua 5.1 extension — not part of Lua 5.5; clx keeps it for compatibility) |

## Coroutines

| Feature | Status |
| --- | --- |
| `coroutine.create`, `resume`, `yield` | ✅ |
| `coroutine.status`, `wrap`, `close` | ✅ |

## Modules

| Feature | Status |
| --- | --- |
| `require` | ✅ (file modules need `--dynamic`) |
| `package.path` | ✅ field; used by `require` file searcher with `--dynamic` |
| `package.searchers` | ✅ preload always; Lua-file searcher needs `--dynamic` |
| `package.searchpath` | ✅ |
| Native modules | ✅ |
| Module linking | ✅ via static linking (`--modules`); no dynamic library loading |

## Standard libraries

| Library | Status |
| --- | --- |
| base | ✅ |
| math | ✅ |
| string | ✅ |
| table | ✅ |
| coroutine | ✅ |
| io | ✅ |
| os | ✅ |
| utf8 | ✅ |
| package | ✅ |
| debug | ⚠️ Partial — metatable, uservalue, `traceback` and the upvalue read/write entries work, the frame-level ones and `upvaluejoin` raise (see [The `debug` library](#the-debug-library)) |

## Things that need `--dynamic`

These entry points are only available when your program is compiled with
`--dynamic`. They run the source in an embedded Lua engine rather than in the
compiled runtime. See [Dynamic Lua](./dynamic-lua.md).

| Function | Notes |
| --- | --- |
| `load()` | Requires `--dynamic`; accepts source strings, not reader functions |
| `loadfile()` | Requires `--dynamic` |
| `dofile()` | Requires `--dynamic` |
| `require` of file modules | Requires `--dynamic`; `package.searchers[2]` loads `package.path` matches through the embedded VM. In plain AOT builds the searcher locates the file but reports that `--dynamic` is required. `package.preload` searchers and statically linked native modules (`--modules`) work everywhere. |

## The `debug` library

`debug` is a standard module and a global, like the other libraries. Its
value-inspection half works on compiled code; the half that needs an
interpreter's frame and line tables does not, because a compiled function has no
bytecode to describe.

| Entry | Status |
| --- | --- |
| `debug.getmetatable(v)` | ✅ — ignores the `__metatable` guard, like stock |
| `debug.setmetatable(v, mt)` | ✅ — any type, including numbers, strings and `nil` |
| `debug.getuservalue` / `debug.setuservalue` | ✅ |
| `debug.getupvalue` / `debug.setupvalue` / `debug.upvalueid` | ⚠️ Partial — the three number a closure's capture cells from 1 in the same order as the Lua C API adapter, so they agree with each other and with `lua_getupvalue`: reading a shared capture, and writing one through `setupvalue`, behave like stock. The names come back as `""`, a local the compiler captured by value cannot be changed through `setupvalue`, and `upvalueid` returns a `userdata: 0x…`-shaped string rather than a light userdata (equal for closures that share a cell) |
| `debug.upvaluejoin` | ❌ Raises `debug.upvaluejoin is not available in compiled code`. A compiled closure reads its captures through the C++ lambda it was emitted as, so rewiring the recorded cell could not reach it |
| `debug.getregistry` | ⚠️ Returns an empty, permanently rooted table — it does not hold stock's `_LOADED` / `_PRELOAD` / `_CLIBS` entries |
| `debug.traceback([msg])` | Shape only — `msg`, a newline and `stack traceback:` with no frame list, since compiled frames carry no call chain |
| `debug.getinfo(f)` | ✅ for a function created by `load`/`loadfile` in a `--dynamic` build (answered by the embedded VM); a compiled closure raises |
| `debug.getlocal` / `debug.setlocal` / `debug.sethook` / `debug.gethook` | Raise `debug.getinfo/getlocal/setlocal/sethook/gethook are not available in compiled code` |

In a `--dynamic` build a loaded chunk's `require "debug"` returns the embedded
VM's own complete library for the code running on that VM; the compiled
program's own `debug` global is the partial one above.

## Semantics aligned with stock 5.5.1

Beyond the tables above, the runtime now follows the reference implementation on
the points most likely to be noticed in a port:

- `coroutine.close` — no argument closes the running coroutine, the main thread
  refuses with `cannot close main thread`, a self-close unwinds and runs its
  `<close>` handlers even during error unwinding, and a raising `__close`
  returns `false, err`.
- `os.execute` and `popen():close()` return the
  `(true|nil, "exit"|"signal", code)` triple; `os.time` validates and normalizes
  its fields to an integer; `os.date` walks the format like stock (embedded NULs,
  `!`, `%%`, invalid specifiers); `os.remove` reports a missing file as an error.
- `io.lines` / `f:lines` keep their formats and file in traced cells, `io.close`
  on a standard handle reports stock's error, and a failed read raises from
  inside the iterator.
- `table.sort` keeps `Int64` elements as integers, and `unpack` / `...` expand
  through the named vararg table.
- `math.random` is stock's xoshiro256** generator with the same seeding rules.
- A cyclic `__call` chain stops at 15 metamethod levels, like stock.
- `next` skips a fractional key instead of truncating it into the array part, and
  `getmetatable` answers for strings and for types with metatable entries.
- Integer/float comparisons are exact beyond 2^53 (`LTintfloat`/`LEintfloat`
  rules) instead of rounding the integer through `double`.
- Weak tables, finalizers and `collectgarbage("step", size)` follow stock's rules
  (a `__mode` string is never cleared, and `collectgarbage()` inside a finalizer
  returns `nil`).
- A C stack overflow raises stock's `stack overflow` with the offending frame's
  location, measured against the current fiber's own stack.
- Numerals parse like stock: hex literals wrap modulo 2^64 and decimal overflow
  falls back to a float; `{}.x` and `function() end()` are syntax errors.

> clx is under active development. Additional Lua 5.5 compatibility work,
> regression testing, and edge-case handling are still needed. A precise list of
> remaining behavioral differences is kept in `deviations.md` at the repository
> root — a maintainer working note that is intentionally not committed.
