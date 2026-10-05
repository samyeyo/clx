# Modules in clx

Modules help you split your Lua project across multiple files. clx supports three
ways to organize and load them, all used through Lua's familiar `require()`:

- **Lua source modules** — other `.lua` files compiled together with your entry point
- **Native C++ modules** — precompiled code you link with `--modules`
- **Native C modules** — existing C code using the standard Lua 5.5 C API, also
  linked with `--modules`

## Lua source modules

Pass multiple `.lua` files to clx — the **first** one is the entry point, the
rest become modules you can load with `require`:

```bash
clx main.lua mymodule.lua utils.lua --output myapp
```

Inside `main.lua`, load them by name (the filename without `.lua`):

```lua
-- main.lua
local mymodule = require("mymodule")
local utils = require("utils")

mymodule.say_hello()
utils.help()
```

When Lua calls `require("mymodule")`:

1. If `mymodule` was already loaded, the cached value is returned immediately.
2. Otherwise the module's code runs just once so your module can set up
   functions and return whatever it wants (typically a table).

There's nothing special to write — just ordinary Lua files. clx bundles them
into a single native binary, so there's nothing to copy alongside your program.

## Runtime-loaded Lua modules (`package.path`)

With `--dynamic`, `require()` can also load Lua files **at runtime** through
`package.path`, using the same searcher chain as stock Lua
(`package.searchers` = preload → Lua file → C). The file is compiled and
executed on the embedded Lua 5.5 VM — it is *not* AOT-compiled:

```bash
clx main.lua --dynamic --output myapp
```

```lua
-- main.lua
package.path = package.path .. ";./lib/?.lua;./lib/?/init.lua"
local greet = require("greet")   -- ./lib/greet.lua, runs on the embedded VM
```

Because required files execute on the VM:

- their code is interpreted, not compiled to native code — keep hot loops in
  your AOT-compiled files;
- the value they return crosses the clx/VM boundary (copied or proxied), the
  same as any `load()`-ed chunk;
- they run on the VM's own standard libraries and globals.

Modules compiled into the binary (the previous section) and native C++ modules
(`--modules`) are found by the preload searcher first, so they always take
precedence and never touch the VM.

In plain AOT builds `package.path`, `package.searchers`, and
`package.searchpath` still exist — `package.searchpath()` can locate files —
but `require` of a file module reports that `--dynamic` is required.

See [Dynamic Lua](./dynamic-lua.md) for the boundary details.

## Native C++ modules

If you have existing C++ code (or want extra performance), you can compile it
into a module and link it in:

```bash
clx main.lua --modules my_native_mod
```

Your module is a C++ file that exposes a single registration function:

```cpp
// my_native_mod.cpp
#include <clx.h>

CLX_API clx::LValue luaopen_my_native_mod(clx::LState* L) {
    clx::LValue t = L->create_table();
    clx::LTable* mod = static_cast<clx::LTable*>(t.as_pointer());

    mod->bind(L, "add", [](clx::LState* L, const clx::LValue* args, size_t n) -> clx::MultiValue {
        double a = args[0].as_number();
        double b = args[1].as_number();
        return clx::MultiValue(clx::LValue(a + b));
    });

    return t;
}
```

Then compile it to a static library and link it:

```bash
# Linux/macOS
g++ -c -std=c++20 -I/path/to/clx/include my_native_mod.cpp -o my_native_mod.o
ar rcs my_native_mod.a my_native_mod.o

# Windows (MSVC)
cl /c /std:c++20 /I.\path\to\clx\include my_native_mod.cpp /Fomy_native_mod.obj
lib /OUT:my_native_mod.lib my_native_mod.obj

# Link with your Lua program
clx main.lua --modules my_native_mod
```

clx looks for the module (`.a` / `.lib`) in the current directory first, then a
few standard install locations. The `--modules` name is the filename without
its extension. Need more details on where it searches or how to write native
modules? See the [C++ API](./api.md).

If your native module depends on external libraries, pass the link flags too:

```bash
clx main.lua --modules my_native_mod -lm -lz
```

## Native C modules (standard Lua 5.5 C API)

clx ships the standard Lua 5.5 C API headers — `lua.h`, `lauxlib.h` and
`lualib.h` — plus a bridge that implements that API on top of the clx runtime,
so existing C modules written against the official API compile and run
unchanged:

```bash
# fetch a module such as luafilesystem, then:
cc -c -O2 -I/path/to/clx/include lfs.c -o lfs.o
ar rcs lfs.a lfs.o

clx main.lua --modules lfs -o main
./main
```

The `--modules` argument is the archive filename without its extension, and
the module must export a C `luaopen_<name>` symbol:

```c
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

int luaopen_lfs(lua_State *L);   /* declared by lfs.h */
```

clx classifies each archive before code generation by scanning its symbols:

- an Itanium/MSVC-mangled `luaopen_<name>` is treated as a **C++ module** and
  declared as `clx::LValue luaopen_<name>(clx::LState*)`.
- a plain, unmangled `luaopen_<name>` is treated as a **C module**; clx emits
  `extern "C" int luaopen_<name>(struct lua_State*)` and a small adapter that
  runs it and seeds `package.loaded` for `require`.
- `clx_luaopen_<name>` (the prefixed form some wrappers emit) is also treated
  as a **C module**, declared under that symbol.

Dotted require names work too: for `--modules socket.core` clx looks for the
archive `socket.core.a` and the symbol `luaopen_socket_core` — dots become
underscores, exactly like stock Lua's `luaopen_a_b_c` naming for
`require("a.b.c")`.

Names are resolved in the current directory first, then in clx's own `lib/clx`
install locations and any `-L` flags you pass. An input `.lua` file whose stem
collides with a precompiled C module name is rejected.

Two things to know when writing C modules:

- `luaL_checkoption` returns the **index** of the chosen option in `lst`, not
  the string (this is a Lua 5.5 change).
- `luaL_openlibs` and `luaL_openselectedlibs` live in `lualib.h`, not
  `lauxlib.h` — include it if you call them.

### Linking: the C API wrapper is its own archive

The C API bridge is **not** part of `libclx.a` / `libclx_size.a`. It is built
into a dedicated archive installed alongside them:

| archive | contents |
|---------|----------|
| `libclx.a` / `clx.lib` | core runtime — values, tables, GC, stdlibs, coroutines |
| `libclx_size.a` / `clx_size.lib` | the same core runtime, `-Os` (default) |
| `libclx_capi.a` / `clx_capi.lib` | Lua 5.5 C API wrapper — `lua_*`, `luaL_*`, `luaopen_*` adapters |
| `libclx_capi_size.a` / `clx_capi_size.lib` | the same wrapper, `-Os` (default) |

clx links `libclx_capi*.a` **only** when `--modules` pulls in an archive that
classifies as a C module. C++ modules and plain Lua programs never pull it in,
so they pay nothing for the wrapper.

The core archive and the wrapper always come as a matched pair — pick the
variant once with `--size` (default) or `--fast`:

```bash
clx main.lua --modules lfs -o main          # libclx_size.a + libclx_capi_size.a
clx main.lua --modules lfs --fast -o main   # libclx.a      + libclx_capi.a
```

Both archives are resolved together, in the same places, in order: `build/`,
`lib/` and `lib64/` next to the `clx` binary, then the install prefix's libdir
(the one used at `cmake --install` time). If the wrapper archive cannot be
found the link stops with a "library not found" error; if it is found but
stale, with undefined `lua_*` or `clx::luaapi_c_module_open` symbols.

If you link by hand instead of going through the driver:

```bash
# default (--size) build
c++ main.o lfs.a -L/path/to/clx/lib -lclx_size -lclx_capi_size -o main

# --fast build
c++ main.o lfs.a -L/path/to/clx/lib -lclx -lclx_capi -o main
```

Keep the pair consistent: `-Os` and `-O3` objects are interchangeable at link
time, but only one core/wrapper variant should be pulled into a given binary.

## Building Lua as a library

You can also compile a `.lua` file into a static library or object file that
other programs can link:

```bash
clx mylib.lua --static --output mylib   # libmylib.a / mylib.lib
clx mylib.lua --object --output mylib   # mylib.o / mylib.obj
```

These export a `luaopen_mylib` function that a host C++ program can register and
call. See the [C++ API](./api.md) for the details.

## Combining approaches

You can mix all of these in one build:

```bash
clx main.lua utils.lua --modules native_processor --output app
```

```lua
local utils = require("utils")            -- Lua source module
local proc = require("native_processor")  -- native C++ module
```

## Options at a glance

| Option | What it does |
|--------|--------------|
| `--modules <list>` | Link prebuilt C++/C modules (comma-separated) |
| `--fast` | Link `libclx.a` + `libclx_capi.a` (optimize for speed) |
| `--size` | Link `libclx_size.a` + `libclx_capi_size.a` (default) |
| `--minimal` | Leave out non-essential libraries (table, io, os, math, utf8, coroutine); keeps base + package + string |
| `--static` | Build a static library that exports `luaopen_*` |
| `--object` | Build an object file that exports `luaopen_*` |

## More on the C++ API

For a full look at the native C++ API — values, tables, calling functions,
coroutines, and error handling — see the [C++ API reference](./api.md). If
you're porting an existing Lua C (C API) module, the [Migration Guide](./migration-guide.md)
walks you through it step by step.