# Modules in clx

Modules help you split your Lua project across multiple files. clx supports two
ways to organize and load them, both used through Lua's familiar `require()`:

- **Lua source modules** — other `.lua` files compiled together with your entry point
- **Native C++ modules** — precompiled code you link with `--modules`

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
| `--modules <list>` | Link prebuilt C++ modules (comma-separated) |
| `--minimal` | Leave out non-essential libraries (string, table, io, os, math, utf8, coroutine) |
| `--static` | Build a static library that exports `luaopen_*` |
| `--object` | Build an object file that exports `luaopen_*` |

## More on the C++ API

For a full look at the native C++ API — values, tables, calling functions,
coroutines, and error handling — see the [C++ API reference](./api.md). If
you're porting an existing Lua C (C API) module, the [Migration Guide](./migration-guide.md)
walks you through it step by step.