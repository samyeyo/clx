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

A `--dynamic` program ends up running two Lua environments: your compiled
(code) and any runtime-loaded code. They keep separate globals and libraries,
and values passed between them are converted at the boundary.

What this means in practice:

- **Your compiled code is usually the faster path.** Dynamic calls cross between
  the two environments with some overhead, so don't put performance-critical
  loops inside a loaded chunk.
- **Libraries are self-contained.** Runtime-loaded code uses its own copy of
  `string`, `math`, `table`, and friends. A compiled module isn't automatically
  visible to a loaded chunk through `require`.
- **Values are converted, not shared.** A table passed across the boundary is a
  copy or a proxy, so don't rely on identity or shared metatable behavior.
- **Coroutines stay on one side.** Create, resume, and yield a coroutine either
  in your compiled code or inside the loaded chunk — not across the boundary.

## Sharing values with loaded code

The default environment lets a loaded chunk read your program's globals:

```lua
-- In your compiled code
shared_value = 123

-- The loaded chunk can see it
local chunk, error_message = load("return shared_value", "globals", "t")
assert(chunk, error_message)
assert(chunk() == 123)
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
- `string.dump` and loading dumped bytecode aren't provided by the compiled
  runtime; under `--dynamic` they belong to the embedded engine.
- Tables, metatables, userdata, and coroutines don't share identity across the
  two environments.

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