# Migration guide: Lua C API to clx C++ API

This guide is for developers who have Lua binary modules written against the
classic Lua C API and want to move them to clx.

## The big idea

If you've written Lua C modules, you're used to the Lua **stack**: you push
values, call functions, and balance the stack by hand.

clx removes the stack entirely. Instead of pushing values onto a stack, you work
with `clx::LValue` objects directly and return values in a `clx::MultiValue`.

Think of it as moving from an assembly-style workflow to ordinary, modern C++:

- Instead of *"where is this value on the stack?"*
- Think: *"which `LValue` object holds this value?"*

Most migrations get dramatically simpler once you make that shift.

## Side-by-side reference

Here's a quick mapping of the API you're used to to its clx equivalent.

### Handling values

| Lua C API | clx C++ API |
|---|---|
| `lua_pushnumber(L, 3.14)` | `clx::number(3.14)` |
| `lua_pushinteger(L, i)` | `clx::integer(i)` |
| `lua_pushboolean(L, b)` | `clx::boolean(b)` |
| `lua_pushstring(L, s)` | `clx::string(L, s)` |
| `lua_pushnil(L)` | `clx::nil()` |
| `lua_pushlightuserdata(L, p)` | `clx::lightuserdata(p)` |
| `lua_tointeger(L, 1)` | `args[0].as_integer()` or `clx::check_integer(L, args[0])` |
| `luaL_checknumber(L, 1)` | `clx::check_number(L, args[0])` |
| `lua_settop(L, 0)` | not needed — there's no stack |
| `return n;` | `return MultiValue(values...)` |

### Types

| Lua C API | clx C++ API |
|---|---|
| `lua_isnumber(L, 1)` | `clx::is_number(args[0])` |
| `lua_isstring(L, 1)` | `clx::is_string(args[0])` |
| `lua_istable(L, 1)` | `clx::is_table(args[0])` |
| `lua_isfunction(L, 1)` | `clx::is_function(args[0])` |
| `lua_isinteger(L, 1)` | `clx::is_integer(args[0])` |
| `lua_type(L, 1)` | `clx::type_of(args[0])` |
| `lua_typename(L, t)` | `clx::type_name(t)` |

### Tables

| Lua C API | clx C++ API |
|---|---|
| `lua_newtable(L)` | `clx::table(L)` |
| `lua_getfield(L, i, "key")` | `clx::get_field(L, t, "key")` |
| `lua_setfield(L, i, "key")` | `clx::set_field(L, t, "key", val)` |
| `lua_rawget(L, i)` / `lua_rawset(L, i)` | `clx::raw_get(L, t, key)` / `clx::raw_set(L, t, key, val)` |
| `lua_next(L, i)` | `clx::next(L, t, key)` — prefer `clx::iterate(L, t)` loop |
| `lua_len(L, i)` | `clx::len(L, v)` |
| `lua_concat(L, n)` | `clx::concat(L, a, b)` |

### Metatables

| Lua C API | clx C++ API |
|---|---|
| `lua_getmetatable(L, i)` | `clx::getmetatable(L, obj)` |
| `lua_setmetatable(L, i)` | `clx::setmetatable(L, obj, mt)` |
| `luaL_getmetafield(L, i, "__index")` | `clx::getmetafield(L, obj, "__index")` |

### Calls and errors

| Lua C API | clx C++ API |
|---|---|
| `lua_getglobal` + `lua_pcall` | `clx::pcall(L, func, args, count)` |
| `lua_error(L)` | `clx::error(L, msg)` or `throw clx::LRuntimeException(...)` |

## The same function, both ways

Here's a simple doubling function in each style.

**Lua C API:**

```c
static int my_func(lua_State* L) {
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    lua_pushnumber(L, x + y);
    return 1;
}
```

**clx C++ API:**

```cpp
static clx::MultiValue my_func(clx::LState* L, const clx::LValue* args, size_t count) {
    double x = clx::check_number(L, args[0]);
    double y = clx::check_number(L, args[1]);
    return clx::number(x + y);
}
```

The differences to notice:

- Arguments arrive as an array `args[0..count-1]` instead of from stack indices.
- Return values are returned directly as a `MultiValue`, not as a count.
- `check_*` both validates the type and reads the value in one step.

## Registering a module

**Lua C API:**

```c
static const struct luaL_Reg mylib[] = {
    {"add", my_add},
    {"sub", my_sub},
    {NULL, NULL}
};

int luaopen_mylib(lua_State* L) {
    luaL_newlib(L, mylib);
    return 1;
}
```

**clx C++ API:**

```cpp
static constexpr clx::LazyReg my_funcs[] = {
    {"add", my_add},
    {"sub", my_sub},
};

clx_API clx::LValue luaopen_mylib(clx::LState* L) {
    clx::LValue t = clx::table(L);
    clx::set_lazy_funcs(L, t, my_funcs, 2);
    return t;
}
```

Or, if you prefer eager closures:

```cpp
static const clx::LReg mylib[] = {
    {"add", my_add},
    {"sub", my_sub},
    {nullptr, nullptr}
};

clx_API clx::LValue luaopen_mylib(clx::LState* L) {
    return clx::new_lib(L, mylib);
}
```

## Complete example

**Before (Lua C API):**

```c
#include "lua.h"
#include "lauxlib.h"

static int vec_add(lua_State* L) {
    double x1 = luaL_checknumber(L, 1);
    double y1 = luaL_checknumber(L, 2);
    double x2 = luaL_checknumber(L, 3);
    double y2 = luaL_checknumber(L, 4);
    lua_pushnumber(L, x1 + x2);
    lua_pushnumber(L, y1 + y2);
    return 2;
}

static int vec_len(lua_State* L) {
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    lua_pushnumber(L, sqrt(x*x + y*y));
    return 1;
}

static const struct luaL_Reg vec_lib[] = {
    {"add", vec_add},
    {"len", vec_len},
    {NULL, NULL}
};

int luaopen_vector(lua_State* L) {
    luaL_newlib(L, vec_lib);
    return 1;
}
```

**After (clx C++ API):**

```cpp
#include <clx.h>

static clx::MultiValue vec_add(clx::LState* L, const clx::LValue* args, size_t count) {
    double x1 = clx::check_number(L, args[0]);
    double y1 = clx::check_number(L, args[1]);
    double x2 = clx::check_number(L, args[2]);
    double y2 = clx::check_number(L, args[3]);
    return clx::MultiValue({clx::number(x1 + x2), clx::number(y1 + y2)});
}

static clx::MultiValue vec_len(clx::LState* L, const clx::LValue* args, size_t count) {
    double x = clx::check_number(L, args[0]);
    double y = clx::check_number(L, args[1]);
    return clx::number(std::sqrt(x*x + y*y));
}

static constexpr clx::LazyReg vec_funcs[] = {
    {"add", vec_add},
    {"len", vec_len},
};

clx_API clx::LValue luaopen_vector(clx::LState* L) {
    clx::LValue t = clx::table(L);
    clx::set_lazy_funcs(L, t, vec_funcs, 2);
    return t;
}
```

**Building:**

```bash
# Lua C version (needs Lua headers + shared lib)
gcc -shared -fPIC vector.c -o vector.so -llua

# clx version — compile to a static lib, then link
g++ -c -std=c++20 -I/path/to/clx/include vector.cpp -o vector.o
ar rcs vector.a vector.o                          # Linux/macOS
clx main.lua --modules vector
```

## Common pitfalls

- **Off-by-one indices.** Lua argument #1 becomes `args[0]`, because clx uses
  normal C++ indexing.
- **Wrap return values.** `return 42` becomes `return clx::integer(42)`.
- **Don't rebuild the stack.** Just store values in variables instead of
  emulating pushes and pops.

## Features with no direct equivalent

Some Lua C API features don't map to clx, because clx compiles Lua ahead of time
to native code (there's no bytecode and no VM stack):

| Lua C API | What to use instead |
|---|---|
| `lua_load` / `luaL_loadfile` | Runtime loading via the optional `--dynamic` engine — see [Dynamic Lua](./dynamic-lua.md) |
| `lua_dump` / `luac` | No bytecode format exists |
| `lua_pushcclosure` | `clx::cfunction(L, func)` |
| `lua_upvalueindex` / get/setupvalue | Lambda captures |
| `lua_newstate` / `luaL_newstate` | `clx::open()` |
| `lua_gc` / `lua_setallocf` | GC is automatic |
| `luaL_openlibs` | `clx::openlibs(L)` |
| `luaL_Buffer` / `luaL_add*` | `std::string` + `clx::string(L, ...)` |

## A suggested porting order

1. Replace function signatures.
2. Replace argument extraction (`args[i]` + `check_*`).
3. Replace return values (`MultiValue`).
4. Replace table and metatable operations.
5. Replace userdata.
6. Replace module registration.
7. Delete all stack-management code.

For the full API reference, see the [C++ API](./api.md).