# Dynamic Lua

clx normally compiles Lua source ahead of time into native C++. Some programs also need to compile or execute Lua source at **runtime** — for example user-supplied scripts, configuration files, plugins, or a built-in console.

The optional `--dynamic` compiler switch adds an embedded Lua 5.5 VM and the bridge needed to call it from clx code. Dynamic chunks execute in that embedded VM rather than in the AOT code path.

## Enabling dynamic Lua

Build clx and the embedded VM library:

```bash
cmake -S . -B build
cmake --build build
```

Compile an application with `--dynamic`:

```bash
./build/clx main.lua --dynamic --output myapp
./myapp
```

The installed compiler accepts the same option:

```bash
clx main.lua --dynamic --output myapp
```

On POSIX the generated program links the bridge archive `libclx_lua.a`; on Windows it links `clx_lua.lib`. The archive contains the embedded Lua VM and the clx bridge. The compiler finds the archive the same way it finds the runtime libraries: the in-tree `build/clx_lua/` output first, then the install library dir(s) resolved from CMake/GNUInstallDirs — on POSIX `<prefix>/lib/` (e.g. `/usr/local/lib/`), on Windows `%ProgramFiles%\clx\lib\`. If the compiler cannot find the archive, rebuild the project (`cmake --build build`).

> Do **not** combine `--minimal` with dynamic loading if the program needs `load`, `loadfile`, or `dofile`. A minimal build skips the standard-library setup that registers the loader functions.

```bash
# OK: dynamic loading enabled, loader functions registered
./build/clx main.lua --dynamic --output myapp

# NOT usable for load/loadfile/dofile
./build/clx main.lua --dynamic --minimal --output myapp
```

## The runtime loading functions

A `--dynamic` build registers three globals:

```lua
local chunk, error_message = load(source [, chunkname [, mode [, env]]])
local chunk, error_message = loadfile([filename [, mode [, env]]])
local result = dofile([filename])
```

All three follow the Lua 5.5 semantics of the same-named standard functions.

### `load`

`load` compiles a source string and returns a callable function:

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

The optional arguments are:

- `chunkname` — identifies the chunk in diagnostics;
- `mode` — defaults to `"bt"` and controls whether text and/or binary input is accepted;
- `env` — selects the chunk environment when it is a clx table (see [Environments](#environments)).

On a compile error the result is normally `nil` plus an error string:

```lua
local chunk, error_message = load("this is not valid Lua")
assert(chunk == nil)
assert(type(error_message) == "string")
```

If `source` is missing, the result is `false` plus an error string. Always check the first result before calling the chunk, especially with untrusted input.

> The reader-function form of `load` (a function returning chunks of source) is not implemented; `source` must be a string.

### `loadfile`

`loadfile` reads a file and compiles it without executing it:

```lua
local chunk, error_message = loadfile("scripts/plugin.lua", "t")
assert(chunk, error_message)
local result = chunk("optional", "arguments")
```

The path is resolved by the host process, so relative paths are relative to the current working directory, not the directory containing the executable. `mode` and `env` behave as in `load`.

### `dofile`

`dofile` loads a file and immediately calls the resulting chunk:

```lua
local result = dofile("scripts/config.lua")
```

On success, the values returned by the file are returned to the caller. `dofile` is implemented as `loadfile(path)()`: a load or file failure returns `false` plus an error value, while an execution error is raised through clx's protected-call machinery.

For failure handling, prefer `loadfile` and call the returned function with `pcall`:

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

## Which runtime runs what?

A `--dynamic` executable contains two Lua execution environments:

```text
input .lua ── clx compiler ── native C++ ── clx runtime
                                             │
                        load/loadfile/dofile │
                                             ▼
                                   embedded Lua VM
```

- Files passed to `clx` are compiled to native code and run on clx's runtime.
- Source loaded at runtime executes in the embedded Lua VM.
- The two runtimes have separate stacks, heaps, globals, standard libraries, and garbage collectors.

Calling a loaded chunk is not free: values cross the boundary with conversion and dispatch overhead. Dynamic calls are therefore slower than direct native calls and should not be used for performance-critical inner loops.

## Standard libraries and `require`

Dynamic chunks run in the embedded VM and use its own libraries: `string`, `math`, `table`, `os`, `io`, `utf8`, `coroutine`, `package`/`require`, and the base functions. The VM also opens its `debug` library, but the default dynamic environment does not expose a `debug` global to loaded chunks.

The package systems are independent:

- `require` in normal AOT code uses clx's package runtime;
- `require` in a dynamic chunk uses the VM's `package.searchers`, `package.loaded`, and `package.preload`;
- clx AOT modules are **not** automatically visible through the VM's `package.preload`;
- a dynamic chunk normally finds source modules through the VM's filesystem searchers.

When `load` is called, clx copies the current clx `package.path` into the VM's `package.path` before compiling the chunk. `loadfile` (and therefore `dofile`) does not perform this synchronization, so set the VM-side `package.path` from dynamic code when file-based loading needs a custom search path. This pattern lets an application configure a common search path for code loaded with `load`:

```lua
package.path = package.path
    .. ";./scripts/?.lua"
    .. ";./scripts/?/init.lua"

local chunk, error_message = load([[ return require("plugin") ]])
assert(chunk, error_message)
local plugin = chunk()
```

Module caches and searcher implementations remain independent between the two runtimes.

## Environments

### Default environment

With no `env` argument, a chunk receives a VM-side environment table with the common globals (`require`, `package`, `string`, `math`, `table`, `os`, `io`, `coroutine`, and base functions). The default environment is a bridge table: reads and writes of names not present in the VM-side environment reach the clx global table.

```lua
-- AOT clx code
shared_value = 123

-- The dynamically loaded chunk can read the clx global
local chunk, error_message = load("return shared_value", "globals", "t")
assert(chunk, error_message)
assert(chunk() == 123)
```

The default environment is not the same table as clx's `_G`.

### Custom environment

The environment argument can be a clx table. Unlike the default environment, a custom environment is **not** pre-populated with the VM standard-library globals:

```lua
local env = { answer = 41 }
local chunk, error_message = load(
    "return answer + 1",
    "custom-env",
    "t",
    env
)
assert(chunk, error_message)
assert(chunk() == 42)
```

The current contents of the clx table are copied into the VM environment at load time, and later writes to the VM side can shadow those copies; the two sides do not share table identity. If the chunk needs library values, provide them explicitly in the environment — for example by assigning clx-side `env.string`, `env.math`, or `env.require` values before calling `load`.

## Passing values across the boundary

Values move between runtimes with the following rules:

- `nil`, booleans, numbers, and strings are copied by value.
- Functions are wrapped so they can be called from the other runtime.
- A clx table or clx userdata passed to the VM becomes a **proxy** userdata with bridge metamethods for indexing, assignment, `pairs`, the length operator, and (for tables) `__call`/`__close`. The proxy is not a native Lua table and keeps the underlying clx object alive while it exists.
- A VM table passed to a clx function is deep-converted into a new clx table; on a successful return, changes made by the clx function are written back to the original VM table. This write-back applies only to table arguments tracked for that call — it is not shared table identity.
- Ordinary VM userdata and lightuserdata are VM-local and convert to `nil` when passed to clx. Lua VM threads are explicitly rejected.
- Errors are captured and converted at the boundary.

Because values are converted or proxied, do not rely on identity, metatable behavior, cycles, or deep structure surviving a boundary crossing unchanged. Deep VM-table conversion is also subject to a maximum nesting depth.

### Calling a loaded function from clx

`load` and `loadfile` return callable clx function values; arguments and results are converted automatically:

```lua
local chunk, error_message = load("return 1, 'two', true")
assert(chunk, error_message)

local number, text, flag = chunk()
assert(number == 1)
assert(text == "two")
assert(flag == true)
```

### Calling clx functions from a dynamic chunk

clx functions visible through the VM environment are callable from dynamic code:

```lua
function add(a, b)
    return a + b
end

local chunk, error_message = load("return add(19, 23)")
assert(chunk, error_message)
assert(chunk() == 42)
```

## Coroutines

Coroutines are runtime-local. A clx coroutine cannot be passed to a VM function, and a VM coroutine cannot be passed back to clx:

```lua
-- Valid inside a dynamically loaded chunk
local co = coroutine.create(function()
    coroutine.yield("paused")
    return "done"
end)
```

Keep creation, resume, yield, and close operations inside a single runtime: clx code uses clx's `coroutine` library, dynamic code uses the VM's. A yield cannot cross the boundary, and calling a VM function from inside a clx coroutine is rejected. If dynamic code needs coroutine behavior, run the whole coroutine lifecycle inside the loaded chunk.

## Limitations

- Dynamic loading requires compilation with `--dynamic`; loader registration is skipped in `--minimal` builds.
- `load` accepts only a string source — the reader-function form is not implemented.
- `string.dump` and loading dumped bytecode are not provided by the clx AOT runtime; in a `--dynamic` program they belong to the embedded VM path.
- clx AOT modules are not automatically exposed through the VM's `package.preload`.
- Tables, metatables, and userdata do not share identity across the boundary.
- Coroutines cannot cross the boundary, and yields cannot cross it.
- The two runtimes do not share garbage collection.

## Testing

Run the load-mode suite to compile and execute the harness with `--dynamic`:

```bash
./tests/test-load.sh        # POSIX
tests\test-load.bat         # Windows
```

The harness in `tests/run_via_load.lua` loads selected test files through the embedded VM and runs each in a fresh child process.

For a quick manual smoke test:

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

To test `dofile`, create a source file and load it from the working directory:

```bash
cat >/tmp/loaded-config.lua <<'LUA'
return { answer = 42 }
LUA

cat >/tmp/dofile-smoke.lua <<'LUA'
local value = dofile("/tmp/loaded-config.lua")
assert(value.answer == 42)
print("dofile OK")
LUA

./build/clx /tmp/dofile-smoke.lua --dynamic --output /tmp/dofile-smoke
/tmp/dofile-smoke
```

## Related documentation

- [CLI reference](./cli.md) — compiler options, including `--dynamic`;
- [Modules](./modules.md) — AOT source modules and native clx modules;
- [Runtime](./runtime.md) — clx values, tables, garbage collection, and coroutines;
- [Lua 5.5 compatibility](./compatibility.md) — language and standard-library status.
