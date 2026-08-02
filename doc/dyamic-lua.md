# Dynamic Lua

clx normally compiles Lua source ahead of time into native C++. Some applications also need to compile or execute Lua source at runtime—for example, scripts supplied by users, configuration files, plugins, or development tools.

The optional `--dynamic` switch adds an embedded Lua VM and the bridge needed to call it from clx code. Dynamic Lua is a separate execution path from the normal clx AOT runtime.

> The filename of this document is kept as `dyamic-lua.md` for compatibility with the existing documentation links.

## Enable dynamic Lua

Build clx and its embedded VM library:

```bash
cmake -S . -B build
cmake --build build
```

Compile an application with `--dynamic`:

```bash
./build/clx main.lua --dynamic --output myapp
./myapp
```

The installed compiler uses the same option:

```bash
clx main.lua --dynamic --output myapp
```

On POSIX systems, the generated program links the bridge archive `libclx_lua.a`. On Windows, it links `clx_lua.lib`. The archive contains the embedded Lua VM and clx’s bridge implementation.

`--dynamic` is a compiler-driver option. It controls whether the generated application links the VM and registers the dynamic-loading functions. There is no separate `CLX_ENABLE_DYNAMIC_LOADING` switch.

### Minimal builds

Do not combine `--minimal` with dynamic loading when the application needs `load`, `loadfile`, or `dofile`:

```bash
# Dynamic loading enabled and standard clx libraries registered
./build/clx main.lua --dynamic --output myapp

# Not suitable for the dynamic APIs: --minimal skips openlibs/registration
./build/clx main.lua --dynamic --minimal --output myapp
```

In a normal build, `--dynamic` adds the VM archive and registers the loader functions after `clx::openlibs`. In a `--minimal` build, the generated program does not perform that standard-library setup, so the loader functions are not registered even if the VM archive can be found.

If the compiler cannot find the bridge archive, rebuild the project:

```bash
cmake --build build
```

If the vendored Lua sources are not present, fetch them and rebuild:

```bash
./deps/fetch_lua.sh
cmake --build build
```

The compiler prints the directories it searched when `libclx_lua.a` or `clx_lua.lib` is missing.

## Runtime loading APIs

With a normal `--dynamic` build, clx registers these global functions:

```lua
local chunk, error_message = load(source [, chunkname [, mode [, env]]])
local chunk, error_message = loadfile([filename [, mode [, env]]])
local results = dofile([filename])
```

The current bridge accepts a string as the first argument to `load`. It does not implement the Lua reader-function form of `load`.

### `load`

`load` compiles a source string in the embedded VM and returns a callable clx function value:

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

The optional arguments are passed to Lua’s `luaL_loadbufferx`:

- `chunkname` identifies the chunk in diagnostics;
- `mode` defaults to `"bt"` and controls accepted text/binary input;
- `env` selects the chunk environment when it is a clx table.

Compilation errors are returned as two results, normally `nil` followed by an error string:

```lua
local chunk, error_message = load("this is not valid Lua")
assert(chunk == nil)
assert(type(error_message) == "string")
```

A missing source argument is reported by the current bridge as `false` plus an error string. Check the first result before calling it when handling untrusted input.

### `loadfile`

`loadfile` reads a file and compiles it in the embedded VM without executing it:

```lua
local chunk, error_message = loadfile("scripts/plugin.lua", "t")
assert(chunk, error_message)
local result = chunk("optional", "arguments")
```

The path is interpreted by the host process. Relative paths are relative to the process’s current working directory, not necessarily the directory containing the compiled executable.

File-open and compilation failures are returned as loader failure values. The `mode` and optional environment arguments are passed to Lua’s `luaL_loadfilex` path.

### `dofile`

`dofile` loads a file and immediately calls the resulting chunk:

```lua
local result = dofile("scripts/config.lua")
```

On success, the values returned by the loaded file are returned to the caller. The current bridge implements `dofile` as `loadfile(path)()`; therefore a load/file failure returns `false` plus an error value, while an execution error is propagated through clx’s protected call/error machinery. Code that must handle all failures explicitly should use `loadfile` and call the returned function with `pcall`.

For example, a missing file can be handled through the current bridge result:

```lua
local result, error_message = dofile("missing.lua")
if result == false and error_message ~= nil then
    print("could not load file:", error_message)
end
```

For execution errors, use `loadfile` and call the returned function with `pcall`:

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

## Which runtime executes a chunk?

A `--dynamic` executable contains two Lua execution environments:

```text
normal input .lua ── clx compiler ── native C++ ── clx runtime
                                                   │
                              load/loadfile/dofile │
                                                   ▼
                                      embedded Lua VM
```

- Input files passed to `clx` are compiled to native code and use clx’s runtime.
- Source loaded at runtime executes in the embedded Lua VM.
- The VM has its own stack, heap, global environment, standard libraries, and garbage collector.
- Values crossing the boundary are converted or wrapped by the bridge; the two runtimes do not share their internal value representation.

Dynamic calls are compatibility calls and include conversion and bridge-dispatch overhead. They are not equivalent to direct native calls generated by clx.

## Standard libraries and `require`

The embedded VM is initialized with Lua’s own standard libraries. Dynamic chunks therefore use the VM versions of:

- `string`,
- `math`,
- `table`,
- `os`,
- `io`,
- `utf8`,
- `coroutine`,
- `package` and `require`,
- and the VM’s base-library functions.

The embedded VM itself also opens Lua’s `debug` library. The default dynamic environment does not copy a `debug` global into loaded chunks, so do not assume that every VM global is directly visible through the clx bridge.

For example, this calls the embedded VM’s `string.sub`, not clx’s AOT `string.sub` implementation:

```lua
local chunk, error_message = load([[ return string.sub("abcdef", 2, 4) ]])
assert(chunk, error_message)
assert(chunk() == "bcd")
```

The package systems are separate:

- `require` in normal AOT code uses clx’s package runtime;
- `require` in a dynamically loaded chunk uses Lua VM `package.searchers`, `package.loaded`, and `package.preload`;
- clx-registered AOT modules are not automatically inserted into the VM’s `package.preload`;
- a dynamic chunk normally discovers source modules through the VM’s filesystem searchers.

When `load` is called, clx copies the current clx `package.path` string into the VM’s `package.path` before compiling the chunk. The current `loadfile` implementation does not perform this synchronization itself, and `dofile` delegates to `loadfile`; set the VM-side `package.path` from dynamic code when file-based loading must use a custom search path. This lets an application configure a common search pattern for code loaded with `load`:

```lua
package.path = package.path
    .. ";./scripts/?.lua"
    .. ";./scripts/?/init.lua"

local chunk, error_message = load([[ return require("plugin") ]])
assert(chunk, error_message)
local plugin = chunk()
```

The path is synchronized, but the module caches and searcher implementations remain independent.

## Environments and globals

### Default environment

When no `env` table is supplied, the chunk receives a VM-side environment table. It contains selected VM globals, including `require`, `package`, `string`, `math`, `table`, `os`, `io`, `coroutine`, and common base functions.

The environment also has bridge callbacks that allow reads and writes to reach the clx global table when a name is not already present in the VM-side environment:

```lua
-- AOT clx code
shared_value = 123

-- The dynamically loaded chunk can read the clx global
local chunk, error_message = load("return shared_value", "globals", "t")
assert(chunk, error_message)
assert(chunk() == 123)
```

The default environment is a bridge table, not the same table as clx’s `_G`.

### Custom environment

The fourth argument to `load`, or the third argument to `loadfile`, can be a clx table. Unlike the default environment, the custom environment is not pre-populated with the VM standard-library globals:

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

The bridge creates a VM-side table, copies the current contents of the clx table into it, and installs callbacks for access to the clx table. This is a boundary conversion, not shared native table identity. In particular, values that were copied into the VM table can be shadowed there by later assignments; do not rely on every mutation behaving as if both runtimes had the same table object. If the chunk needs library values, provide them explicitly in the environment—for example, assign clx-side `env.string`, `env.math`, or `env.require` values before calling `load`. These are bridged clx values, not automatically the embedded VM’s standard-library objects.

Only a table passed as the environment is used as a custom environment by the current bridge. Otherwise the default environment is used.

## Calling across the boundary

### Calling a loaded VM function from clx

`load` and `loadfile` return callable clx function values. Arguments are converted into Lua VM values and return values are converted back:

```lua
local chunk, error_message = load("return 1, 'two', true")
assert(chunk, error_message)

local number, text, flag = chunk()
assert(number == 1)
assert(text == "two")
assert(flag == true)
```

### Calling clx functions from a dynamic chunk

Clx functions visible through the VM environment are wrapped by `NativeBridge`:

```lua
function add(a, b)
    return a + b
end

local chunk, error_message = load("return add(19, 23)")
assert(chunk, error_message)
assert(chunk() == 42)
```

### Value conversion

The bridge handles values as follows:

- `nil`, booleans, numbers, and strings are copied by value;
- clx tables and clx userdata are exposed through VM-side proxy userdata and bridge metamethods;
- Lua VM tables returned to clx are converted into clx tables, including nested values subject to conversion limits;
- functions are wrapped as callable values in the other runtime;
- errors are captured and converted at the boundary;
- clx coroutines and Lua VM threads are rejected instead of being converted.

Tables, metatables, aliases, cycles, and userdata should not be assumed to have identical identity or behavior after crossing the boundary. Deep VM-table conversion also has a maximum nesting depth.

## Tables and userdata across the boundary

Tables and userdata do not cross as transparent native objects. Their behavior depends on the direction of the call.

### Passing clx tables to VM code

When a clx table is passed as an argument to a VM function, the VM receives a `clx_proxy` userdata object. It is not a native Lua table:

```lua
-- Conceptual example: `aot_table` is a clx table explicitly exposed
-- to the VM environment by the host application.
local chunk, error_message = load([[
    return type(aot_table), aot_table.answer
]], "inspect-table", "t")
assert(chunk, error_message)

local kind, answer = chunk()
assert(kind == "userdata")
```

The proxy provides bridge metamethods for:

- indexing and reading fields;
- assigning fields, with writes sent to the clx table;
- `pairs` iteration;
- the length operator;
- `__call`, when the clx table has a callable metatable;
- `__close`, when the clx table has a close metamethod.

The VM must therefore not use `type(value) == "table"` or require native Lua table identity for a clx table proxy. The proxy keeps the underlying clx object alive through the VM’s proxy tracking mechanism, but it is still a different VM-side object.

### Passing clx userdata to VM code

A clx userdata value can be passed to a VM function, but it is also represented by a VM-side proxy userdata object with the `clx_userdata` metatable:

```lua
-- Conceptual example: `aot_userdata` is a clx userdata value
-- explicitly exposed to the VM environment by the host application.
local chunk, error_message = load([[
    return type(aot_userdata), aot_userdata.name
]], "inspect-userdata", "t")
assert(chunk, error_message)

local kind, name = chunk()
assert(kind == "userdata")
```

The proxy’s `__index` handler looks up fields and methods in the clx userdata’s clx-side metatable. This allows VM code to call methods deliberately exposed by the clx userdata. The proxy has no `__newindex` handler, so direct VM-side assignment is not supported:

```lua
-- This is not a supported way to mutate clx userdata:
-- aot_userdata.name = "changed"
```

If userdata state must be changed, expose a clx method that performs the mutation and call that method from VM code. The VM never receives the original native userdata layout or a raw pointer to it.

A clx userdata proxy can be passed through VM functions and returned to clx; the bridge recognizes its `clx_userdata` metatable and recovers the underlying clx userdata value. This is a supported proxy round trip, not arbitrary sharing of the native object representation.

### Passing VM tables and userdata to clx code

When a VM function calls a clx function through `NativeBridge`, a VM table argument is deep-converted into a new clx table. After the clx function returns successfully, the bridge writes the modified table contents back to the original VM table. This write-back is specific to tracked table arguments in that bridge call; it is not a general shared-table mechanism:

```lua
-- `aot_update` is a clx function visible to the VM.
local vm_table = { value = 1 }
aot_update(vm_table)
-- Changes made by aot_update are synchronized back here.
```

This write-back applies to table arguments tracked for the call. It does not make the two runtimes share table identity, and references retained by clx code point to the converted clx table rather than the original VM table.

VM-created userdata has no general conversion into a clx userdata value. For example, an `io` file handle or other ordinary Lua VM userdata passed to a clx function is currently converted to `nil` unless it is one of the bridge’s recognized proxy types. VM lightuserdata is likewise not a portable cross-runtime value. The bridge explicitly rejects Lua VM threads instead of converting them.

The recognized VM-side proxy types are internal bridge representations:

- `clx_proxy` — converts back to the underlying clx table;
- `clx_userdata` — converts back to the underlying clx userdata;
- `clx_callable` — an internal wrapper used when clx callables are exposed to VM code; do not treat it as a portable VM↔clx value;
- `clx_thread` — identifies an internal clx thread proxy, but coroutine crossing is rejected by the boundary rules.

Actual Lua VM thread values (`LUA_TTHREAD`) are explicitly rejected when passed to clx functions. All other VM userdata, including ordinary VM userdata and lightuserdata, should be treated as VM-local and must not be returned to or passed into clx code.

## Coroutines

Coroutines are runtime-local. A clx coroutine cannot be passed to a VM function, and a Lua VM coroutine cannot be passed back to clx code:

```lua
-- This is valid inside a dynamically loaded chunk.
local co = coroutine.create(function()
    coroutine.yield("paused")
    return "done"
end)
```

Keep creation, resume, yield, and close operations inside the same runtime:

- clx code uses clx’s `coroutine` library;
- dynamic code uses the embedded VM’s `coroutine` library.

The bridge also rejects calling a VM function from inside a clx coroutine. A yield cannot cross from one runtime to the other. If dynamic code needs coroutine behavior, run the entire coroutine lifecycle inside the loaded VM chunk.

## Testing

The normal AOT test suite does not enable dynamic loading:

```bash
./tests/run.sh
```

Run the dedicated load-mode suite to compile and execute the harness with `--dynamic`:

```bash
./tests/test-load.sh
```

On Windows:

```bat
tests\test-load.bat
```

The harness in `tests/run_via_load.lua` reads selected test files, loads them through the embedded VM, and executes each file in a fresh child process. The separate process per test avoids accumulating bridge/VM state across a long sequence of dynamic loads.

For a small manual test:

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

To test `dofile`, create a source file and load it from the application’s working directory:

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

## Current limitations

The current dynamic bridge does not provide:

- dynamic loading in a program compiled without `--dynamic`;
- loader registration in a `--minimal` generated program;
- the Lua reader-function form of `load`;
- `string.dump` or loading dumped bytecode in ordinary AOT clx code; these belong to the embedded VM path when `--dynamic` is enabled;
- automatic exposure of clx AOT modules through VM `package.preload`;
- transparent identity between all clx and VM tables, metatables, and userdata;
- passing or resuming a coroutine created by the other runtime;
- yielding across the clx/VM boundary;
- shared garbage collection between the two runtimes.

## Related documentation

- [CLI reference](./cli.md) — compiler options, including `--dynamic`;
- [Modules](./modules.md) — AOT source modules and native clx modules;
- [Runtime](./runtime.md) — clx values, tables, garbage collection, and coroutines;
- [Lua 5.5 compatibility](./compatibility.md) — language and standard-library status;
- `tests/conformance/load.lua` — focused dynamic-loading tests;
- `tests/run_via_load.lua` — load-mode integration harness.
