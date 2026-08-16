# Lua 5.5 Compatibility

clx aims for Lua 5.5 compatibility. The tables below summarize the current
status. clx is under active development, so this is a snapshot — real-world
testing is still ongoing.

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
| `__ipairs`, `__pairs` | ✅ |

## Coroutines

| Feature | Status |
| --- | --- |
| `coroutine.create`, `resume`, `yield` | ✅ |
| `coroutine.status`, `wrap`, `close` | ✅ |

## Modules

| Feature | Status |
| --- | --- |
| `require` | ✅ |
| `package.path` | ✅ |
| Native modules | ✅ |
| Module linking | ✅ |

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
| debug | Compiled code: ❌ · Dynamic code: ✅ with `--dynamic` |

## Things that need `--dynamic`

These three functions are only available when your program is compiled with
`--dynamic`. They run the source in an embedded Lua engine rather than in the
compiled runtime. See [Dynamic Lua](./dynamic-lua.md).

| Function | Notes |
| --- | --- |
| `load()` | Requires `--dynamic`; accepts source strings, not reader functions |
| `loadfile()` | Requires `--dynamic` |
| `dofile()` | Requires `--dynamic` |

## Things that aren't provided

| Feature | What's the story |
| --- | --- |
| `string.dump()` | Not available in compiled code; works inside the embedded engine with `--dynamic` |
| `debug` library | Not exposed as a compiled-code global; behavior in dynamic code is documented separately |

> clx is under active development. Additional Lua 5.5 compatibility work,
> regression testing, and edge-case handling are still needed.