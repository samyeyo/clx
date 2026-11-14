# Dynamic Lua

Usually clx compiles your Lua source **ahead of time** into a native program.
Sometimes you also want to run Lua source **at runtime** — for example a
user-supplied script, a config file, a plugin, or a small built-in console.

The optional `--dynamic` switch enables runtime loading by embedding a Lua 5.5
engine inside your program. Code loaded this way runs in that embedded engine.

## Enabling dynamic Lua

Compile your program with `--dynamic`:

```bash
clx main.lua --dynamic --output myapp
```

> Don't combine `--dynamic` with `--minimal` if you need runtime loading.
> A minimal build leaves out the library setup that enables the loaders.

## What you can do

A `--dynamic` build adds three familiar global functions from Lua 5.5:

| Function | What it does |
|----------|--------------|
| `load(source)` | Compiles a string of code and returns it as a callable function |
| `loadfile(filename)` | Reads a file, compiles it, returns it as a callable function |
| `dofile(filename)` | Loads a file and immediately runs it |

It also completes `require()`: `package.searchers[2]` loads Lua file modules found via
`package.path`, so multi-file programs and libraries work as they do in stock Lua.
Keep in mind that these modules are **executed on the embedded Lua 5.5 VM** — they
are not AOT-compiled, exactly like chunks loaded with `loadfile`:

```lua
package.path = package.path .. ";./lib/?.lua;./lib/?/init.lua"
local greet = require("greet")       -- ./lib/greet.lua
local util = require("util")        -- ./lib/util/init.lua
```

Concretely, this means:

- **Module code runs on the VM.** Every function defined in a required file executes
  on the embedded Lua 5.5 engine each time you call it, with the usual cross-boundary
  overhead — keep hot loops in your compiled code, not in a required module.
- **The returned value crosses the boundary.** Whatever the module returns is
  converted (copied or proxied) into the compiled environment, like any value
  returned by a loaded chunk.
- **AOT modules take precedence.** Modules bundled at compile time or registered in
  `package.preload` are found by the preload searcher before the file searcher is
  consulted.

In plain AOT builds `package.path`, `package.searchers`, and `package.searchpath`
still exist, but the file searcher only reports that `--dynamic` is required.
Modules registered in `package.preload` (including statically linked native modules
via `--modules`) load in every build mode.

### `load`

```lua
local source = [[
    local left = 20
    local right = 22
    return left + right
]]

local chunk, error_message = load(source, "runtime-expression", "t")
assert(chunk, error_message)
assert(chunk() == 42)
```

When the source can't be compiled, you get `nil` plus an error string:

```lua
local chunk, error_message = load("this is not valid Lua")
assert(chunk == nil)
assert(type(error_message) == "string")
```

Always check the first result before calling the chunk, especially if the source
comes from an untrusted user.

### `loadfile`

```lua
local chunk, error_message = loadfile("scripts/plugin.lua", "t")
assert(chunk, error_message)
local result = chunk("optional", "arguments")
```

Paths are resolved by the program's current working directory.

### `dofile`

```lua
local result = dofile("scripts/config.lua")
```

`dofile` is just `loadfile` followed by a call. The values your file returns
come back to the caller.

### `string.dump`

Dumping a function created by `load()` returns real Lua 5.5 bytecode as a
string, exactly as stock Lua produces it — `load()` reads it back, with or
without `strip`, and text-mode loads reject it like stock:

```lua
local fn = load("return function(x) return x * 2 + 1 end")()
local bytes = string.dump(fn)      -- a binary string
assert(load(bytes)(20) == 41)
assert(load(string.dump(fn, true))(20) == 41)
assert(load(bytes, nil, "t") == nil)
```

Compiled (AOT) and C functions have no bytecode, so dumping one returns a
callable snapshot instead: calling it runs the original, with upvalues
intact. This works in every build mode, not just `--dynamic` (bare builds
return the function itself). Anything else raises stock's error:

```lua
local function add(a, b) return a + b end
assert(string.dump(add)(3, 4) == 7)
assert(pcall(string.dump, {}) == false)  -- bad argument #1 to 'string.dump'
```

For careful error handling, prefer `loadfile` and call the function with `pcall`:

```lua
local chunk, error_message = loadfile("scripts/plugin.lua", "t")
if not chunk then
    return false, error_message
end

local ok, result = pcall(chunk)
if not ok then
    return false, result
end
return true, result
```

## Notes on how it works

A `--dynamic` program runs two Lua environments: your compiled code and any
runtime-loaded code. Each has its own globals and standard libraries, values are
converted at the boundary, and the two globals tables are bridged.

What this means in practice:

- **Your compiled code is usually the faster path.** Dynamic calls cross between
  the two environments with some overhead, so don't put performance-critical
  loops inside a loaded chunk.
- **Loaded code keeps its own libraries.** A chunk runs on the embedded VM's
  `string`, `math`, `table`, and friends, so a compiled module isn't
  automatically visible to it through `require`. Bundled modules and anything in
  `package.preload` do resolve. Inside a chunk, `require` consults the VM's own
  `package.loaded` first — so `require "debug"` reaches the VM's complete
  library — and falls through to clx's `require` for everything else, which
  means `package.path` and `package.loaded` are shared state.
- **The globals tables are bridged.** A chunk's `_ENV` is the VM's own globals
  table (`_G == _ENV`), so a raw access sees the chunk's own names only. A name
  missing there is looked up in the compiled program's globals through a
  metatable `__index` — `rawget` does not follow it — and a global either side
  writes is visible to the other (see
  [Sharing values](#sharing-values-with-loaded-code)).
- **Bridged values get a stable proxy.** Reading a compiled table or function
  from a chunk yields the same proxy each time for as long as it lives, so
  `rawequal` and `tostring` agree inside the chunk and a write through the proxy
  is a write to the original table. Values travelling *back* are converted
  again, so don't compare a returned value with one you still hold.
- **Coroutines stay on one side.** Create, resume, and yield a coroutine either
  in your compiled code or inside the loaded chunk — not across the boundary.

## Sharing values with loaded code

The default environment lets a loaded chunk read your program's globals: any
name missing from the chunk's own globals table falls through to the compiled
program's, and a global the chunk assigns is mirrored back.

```lua
-- In your compiled code
shared_value = 123

-- The loaded chunk can see it
local chunk, error_message = load("return shared_value", "globals", "t")
assert(chunk, error_message)
assert(chunk() == 123)

-- And a global the chunk sets is visible to the compiled code
load("written_by_chunk = 7", "globals", "t")()
assert(written_by_chunk == 7)
```

You can also give a chunk its own environment by passing a table:

```lua
local env = { answer = 41 }
local chunk, error_message = load("return answer + 1", "custom-env", "t", env)
assert(chunk, error_message)
assert(chunk() == 42)
```

If the chunk needs libraries, put them in the environment yourself (for example
`env.string`, `env.math`, or `env.require`).

## Coroutines in loaded code

Coroutines work normally **inside** a loaded chunk:

```lua
local co = coroutine.create(function()
    coroutine.yield("paused")
    return "done"
end)
```

Just keep the whole coroutine lifecycle (create/resume/yield) inside one
environment — either the compiled one or the loaded one.

## Limitations

- Runtime loading requires compiling with `--dynamic`. It's skipped in
  `--minimal` builds.
- `load` accepts a string of source only (the reader-function form isn't
  implemented).
- `string.dump` of a compiled or C function returns a callable snapshot,
  not bytecode (see above); only VM-loaded functions dump to real bytecode.
- Metatables and coroutines do not cross the boundary. A table or userdata is
  handed over as a stable proxy for its lifetime, but a value returned from a
  chunk is converted again on arrival, so identity is not preserved in that
  direction.
- `debug` exists on both sides but is not the same library: the compiled
  program's global offers the metatable, uservalue, `traceback` and upvalue
  entries (upvalue names are empty and `upvaluejoin` raises), while a chunk's own
  `debug` — or its `require "debug"` — is the VM's complete one. See
  [Lua 5.5 compatibility](./compatibility.md#the-debug-library).

## Testing

You can run the load-mode test suite:

```bash
./tests/test-load.sh        # POSIX
tests\test-load.bat         # Windows
```

For a quick manual check:

```bash
cat >/tmp/dynamic.lua <<'LUA'
local chunk, error_message = load("return 6 * 7", "smoke", "t")
assert(chunk, error_message)
assert(chunk() == 42)
print("dynamic Lua OK")
LUA

./build/clx /tmp/dynamic.lua --dynamic --output /tmp/dynamic
/tmp/dynamic
```

## Related documentation

- [CLI reference](./cli.md) — compiler options, including `--dynamic`;
- [Modules](./modules.md) — Lua source and native C++ modules;
- [Lua 5.5 compatibility](./compatibility.md) — language and library status.