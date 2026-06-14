# Modules in clx

clx supports two ways to organize and load modules:

- **Lua source modules** compiled alongside your entry point (static preload)
- **Statically linked C++ modules** with `--modules`

Both are consumed via Lua's `require()` function.

## Lua Source Modules

Pass multiple `.lua` files to clx — the first is the entry point, the rest become
modules loadable via `require`:

```bash
clx main.lua mymodule.lua utils.lua --output myapp
```

Inside `main.lua`, require them by name (filename without `.lua`):

```lua
-- main.lua
local mymodule = require("mymodule")
local utils = require("utils")

mymodule.say_hello()
utils.help()
```

### How it works

clx compiles each `.lua` file into a C++ function `luaopen_<module>` with `extern`
linkage (the name is unmangled since it's a plain C++ function). The generated `main()` calls `L->register_module()`
for non-entry modules — this stores the function in `package.preload[name]` **without
calling it**. The function runs only on the first `require("name")` from Lua code:

```
main() {
    clx_open()
    openlibs(L)

    register_module("mymodule", luaopen_mymodule)  // stored in preload, NOT called
    register_module("utils",    luaopen_utils)     // stored in preload, NOT called

    luaopen_main(L)                                 // entry point runs immediately

    clx_close(L)
}
```

When Lua calls `require("mymodule")`:

1. Checks `package.loaded["mymodule"]` — if present, returns it immediately
2. Checks `package.preload["mymodule"]` — calls the registered function

### Linking

All builds link statically against `libclx.a`. No shared library is needed at runtime.

## C++ Native Modules (Statically Linked)

You can link precompiled C++ code exposing a `luaopen_*` function using `--modules`:

```bash
clx main.lua --modules my_native_mod
```

The function must have this signature with `CLX_API` (which provides `extern`
linkage and proper symbol visibility):

```cpp
CLX_API clx::LValue luaopen_my_native_mod(clx::LState* L);
```

The generated `main()` calls `register_module("my_native_mod", luaopen_my_native_mod)`,
which stores the wrapper in `package.preload` — the function runs only on first
`require("my_native_mod")`, not at startup.

### Writing a C++ native module

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

Compile to a static library:

```bash
# Linux/macOS (g++/clang++):
g++ -c -std=c++20 -I/path/to/clx/include my_native_mod.cpp -o my_native_mod.o
ar rcs my_native_mod.a my_native_mod.o

# Windows (MSVC):
cl /c /std:c++20 /I.\path\to\clx\include my_native_mod.cpp /Fomy_native_mod.obj
lib /OUT:my_native_mod.lib my_native_mod.obj
```

Then link with your Lua script:

```bash
clx main.lua --modules my_native_mod
```

clx looks for `my_native_mod.a` (or `my_native_mod.lib` on Windows) in the current directory,
then in `<clx-install-dir>/lib/clx/` (i.e. `../lib/clx/` relative to the clx binary),
and on POSIX also in `/usr/local/lib/clx/`.

### Linking with external libraries

If your native module depends on external libraries, pass link flags directly:

```bash
clx main.lua --modules my_native_mod -lm -lz
```

## Compiling Lua to Libraries

### Static Library

```bash
clx mylib.lua --static --output mylib
# Produces libmylib.a (Linux/macOS) or mylib.lib (Windows)
```

### Object File

```bash
clx mylib.lua --object --output mylib
# Produces mylib.o (Linux/macOS) or mylib.obj (Windows)
```

All export a `luaopen_mylib` function. A host C++
program can link against the library and call it via `register_module`:

```cpp
#include <clx.h>

CLX_API clx::LValue luaopen_mylib(clx::LState* L);

int main() {
    clx::LState* L = clx::clx_open();
    clx::openlibs(L);

    // Register in package.preload (NOT called yet)
    L->register_module("mylib", luaopen_mylib);

    clx::clx_close(L);
    return 0;
}
```

## Combining Lua and Native Modules

All approaches combine in a single build:

```bash
clx main.lua utils.lua --modules native_processor --output app
```

In `main.lua`:

```lua
local utils = require("utils")             -- Lua source module (preload)
local proc = require("native_processor")   -- C++ native module (preload)
local extra = require("extra_plugin")      -- Static module (preload)
```

The generated `main()` registers all modules in `package.preload`.

## Options Reference

| Option | Description |
|--------|-------------|
| `--modules <list>` | Comma-separated list of precompiled C++ modules to link |
| `--minimal` | Exclude non-essential modules (string, table, io, os, math, utf8, coroutine); keeps base + package |
| `--static` | Compile to static library (exports `luaopen_*`) |
| `--object` | Compile to object file (exports `luaopen_*`) |

## API Reference

```cpp
// Register a native module for lazy loading via require()
// Stores a wrapper in package.preload[name]; does NOT call luaopen_*
void LState::register_module(const std::string& name, LValue (*func)(LState*));

// Initialize clx runtime
LState* clx_open(int argc = 0, char* argv[] = nullptr);

// Open optional standard libraries (string, table, io, os, math, utf8, coroutine)
void openlibs(LState* L);

// Destroy clx runtime
void clx_close(LState* L);
```
