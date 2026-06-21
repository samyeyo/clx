# Migration Guide: Lua C API → clx C++ API

## Introduction

### Who this guide is for

This guide is intended for developers who have already written Lua binary modules using the Lua C API and want to port them to clx.

You are likely familiar with concepts such as:

- `lua_State`
- stack indices
- `lua_push*`
- `lua_to*`
- `luaL_check*`
- userdata
- metatables
- `luaL_Reg`
- `luaopen_*`

clx exposes a completely different API design. While it preserves Lua semantics, it does not expose the Lua VM stack.

As a result, most ports are straightforward but require a different mental model.

## The biggest difference

The Lua C API is stack-oriented. Everything happens through a virtual stack. clx removes this concept entirely and exposes values directly through `clx::LValue` objects.

Think of clx as moving from an assembly-style API to a modern C++ object-oriented API.

## Understanding LValue

`clx::LValue` is the fundamental type used throughout the clx runtime. An `LValue` can represent any Lua value:

- nil
- boolean
- number
- string
- table
- function
- userdata
- thread

Most APIs either accept or return `LValue`.

## Understanding MultiValue

Lua functions can return multiple values. The Lua C API does this by pushing multiple values and returning a count. clx uses `clx::MultiValue`, making intent explicit and eliminating stack bookkeeping.

## Migrating your mindset

Do not think in terms of stack slots.

Instead of:

- "Where is this value on the stack?"

Think:

- "Which `LValue` object contains this value?"

Most migrations become dramatically simpler after this shift.

---

# Original API mapping reference

## Key differences

| Lua C API | clx C++ API |
|---|---|
| Stack-based (`lua_push*`, `lua_to*`, `lua_get*`) | Value-based (values are `clx::LValue` objects, no stack) |
| `lua_State* L` | `clx::LState* L` |
| `lua_pushnumber(L, 3.14)` | `clx::number(3.14)` |
| `lua_tointeger(L, 1)` | `args[0].as_integer()` or `clx::check_integer(L, args[0])` |
| `luaL_checknumber(L, 1)` | `clx::check_number(L, args[0])` |
| `lua_settop(L, 0)` | Not needed — no stack |
| `lua_getfield(L, idx, "key")` | `clx::get_field(L, table, "key")` |
| `lua_setfield(L, idx, "key")` | `clx::set_field(L, table, "key", val)` |
| `lua_newtable(L)` | `clx::table(L)` (returns `LValue`) |
| `lua_pushcfunction(L, func)` | Wrapped in `clx::cfunction(L, func)` or bound via `set_function` |
| `lua_register(L, name, func)` | `clx::set_function(L, table, name, func)` or `L->bind(L, name, func)` |
| `luaL_newlib(L, regs)` | `clx::new_lib(L, regs)` |
| `lua_pushstring(L, s)` | `clx::string(L, s)` |
| `return n;` (return count) | `return MultiValue(values...)` (return values directly) |
| `lua_error(L)` | `throw clx::LRuntimeException(...)` or `clx::error(L, msg)` |
| `lua_len(L, idx)` | `clx::len(L, v)` |
| `lua_concat(L, n)` | `clx::concat(L, a, b)` |
| `lua_pcall(L, nargs, nresults)` | `clx::pcall(L, func, args, count)` |
| `lua_next(L, idx)` | `clx::next(L, table, key)` |
| `lua_getmetatable(L, idx)` | `clx::getmetatable(L, obj)` |
| `lua_setmetatable(L, idx)` | `clx::setmetatable(L, obj, mt)` |

## Function signature

### Lua C API

```c
static int my_func(lua_State* L) {
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    lua_pushnumber(L, x + y);
    return 1;
}
```

### clx C++ API

```cpp
static clx::MultiValue my_func(clx::LState* L, const clx::LValue* args, size_t count) {
    double x = clx::check_number(L, args[0]);
    double y = clx::check_number(L, args[1]);
    return clx::number(x + y);
}
```

Arguments are passed as an array `args[0..count-1]` instead of on a stack.
Return values are returned directly as a `MultiValue`, not as a return count.

## Stack → Values

Instead of manipulating a stack, you work directly with `LValue` objects. Arguments
arrive as `args[0]`, `args[1]`, ... `args[count-1]`. You construct new values with
factory functions and return them in a `MultiValue`.

| Lua C API | clx C++ API |
|---|---|
| `lua_pushnumber` | `clx::number(d)` |
| `lua_pushinteger` | `clx::integer(i)` |
| `lua_pushboolean` | `clx::boolean(b)` |
| `lua_pushstring(L, s)` | `clx::string(L, s)` (auto-interns) |
| `lua_pushliteral(L, s)` | `clx::string(L, s)` |
| `lua_pushnil(L)` | `clx::nil()` |
| `lua_pushvalue(L, idx)` | Copy the `LValue` directly |
| `lua_pushlightuserdata(L, p)` | `clx::lightuserdata(p)` |

## Argument checking

### Lua C API

```c
double d = luaL_checknumber(L, 1);
const char* s = luaL_checkstring(L, 2);
luaL_checktype(L, 3, LUA_TTABLE);
```

### clx C++ API

```cpp
double d = clx::check_number(L, args[0]);
const char* s = clx::check_string(L, args[1]);
clx::checktype(L, 3, args[2], clx::LType::Table);
```

Optional arguments use `opt_*` functions:

```cpp
double d = clx::opt_number(L, args[0], 1.0);
const char* s = clx::opt_string(L, args[0], "default");
```

## Type queries

| Lua C API | clx C++ API |
|---|---|
| `lua_isnumber(L, 1)` | `clx::is_number(args[0])` |
| `lua_isstring(L, 1)` | `clx::is_string(args[0])` |
| `lua_istable(L, 1)` | `clx::is_table(args[0])` |
| `lua_isfunction(L, 1)` | `clx::is_function(args[0])` |
| `lua_isinteger(L, 1)` | `clx::is_integer(args[0])` |
| `lua_isnil(L, 1)` | `clx::is_nil(args[0])` |
| `lua_isboolean(L, 1)` | `clx::is_bool(args[0])` |
| `lua_isuserdata(L, 1)` | `clx::is_userdata(args[0])` |
| `lua_type(L, 1)` | `args[0].type()` or `clx::type_of(args[0])` |
| `lua_typename(L, t)` | `clx::type_name(t)` |

## Table operations

### Lua C API

```c
lua_getfield(L, 1, "key");        // push table["key"]
lua_setfield(L, 1, "key");        // table["key"] = top
lua_rawget(L, idx);
lua_rawset(L, idx);
lua_rawgeti(L, idx, 4);
lua_rawseti(L, idx, 4);
lua_next(L, idx);
```

### clx C++ API

```cpp
clx::LValue v = clx::get_field(L, table, "key");   // table["key"] with __index
clx::set_field(L, table, "key", val);               // table["key"] = val with __newindex

clx::LValue v = clx::raw_get(L, table, key);        // raw get (any key type)
clx::raw_set(L, table, key, val);                    // raw set (any key type)

clx::LValue v = clx::raw_get_i(L, table, 4);        // raw get by int key (1-based)
clx::raw_set_i(L, table, 4, val);                    // raw set by int key (1-based)

auto mv = clx::next(L, table, key);                  // iteration — call with nil to start
```

For iteration, prefer the iterator:

```cpp
for (auto it = clx::iterate(L, table); it; ++it) {
    auto [key, value] = *it;
    // ...
}
```

## Module registration

### Lua C API

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

### clx C++ API

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

Or with eager closures:

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

## Metatable operations

### Lua C API

```c
lua_getmetatable(L, 1);
lua_setmetatable(L, 1);
luaL_getmetafield(L, 1, "__index");
```

### clx C++ API

```cpp
clx::LValue mt = clx::getmetatable(L, obj);
clx::setmetatable(L, obj, mt);
clx::LValue f = clx::getmetafield(L, obj, "__index");
```

## Calling Lua functions

### Lua C API

```c
lua_getglobal(L, "myfunc");
lua_pushnumber(L, 1.0);
lua_pushnumber(L, 2.0);
lua_pcall(L, 2, 1, 0);
double result = lua_tonumber(L, -1);
lua_pop(L, 1);
```

### clx C++ API

```cpp
clx::LValue func = clx::get_global(L, "myfunc");
clx::LValue args[] = {clx::number(1.0), clx::number(2.0)};
clx::MultiValue r = clx::pcall(L, func, args, 2);
double result = clx::check_number(L, r[0]);
```

## Userdata

### Lua C API

```c
typedef struct { double x, y; } Point;

// Create
Point* p = (Point*)lua_newuserdata(L, sizeof(Point));
p->x = 1.0; p->y = 2.0;

// Check
Point* p = (Point*)luaL_checkudata(L, 1, "Point");
```

### clx C++ API

In clx, userdata is allocated via `LState` and stored as a raw `clx::LValue`:

```cpp
struct Point { double x, y; };

// Create — allocate userdata and set metatable
clx::LUserdata* ud = L->allocate_userdata(sizeof(Point));
Point* p = static_cast<Point*>(ud->data());
p->x = 1.0; p->y = 2.0;
ud->metatable = point_mt;
clx::LValue v(clx::LType::Userdata, ud);

// Retrieve
clx::LUserdata* ud = clx::check_userdata(L, args[0]);
Point* p = static_cast<Point*>(ud->data());
```

## Error handling

### Lua C API

```c
return luaL_error(L, "bad argument #1 to 'foo' (number expected, got %s)",
    lua_typename(L, lua_type(L, 1)));
```

### clx C++ API

```cpp
clx::error(L, "something went wrong");
// or
throw clx::LRuntimeException(clx::string(L, "bad argument #1 (number expected)"));
```

Error helpers:

```cpp
clx::arg_error(L, 1, "number");       // "bad argument #1 (number expected)"
clx::type_error(L, 1, "number");       // "bad argument #1 (number expected, got ...)"
clx::argcheck(L, cond, 1, "msg");     // assert cond, throw "bad argument #1 (msg)"
```

Strings in errors are auto-interned; `LRuntimeException` contains the error value
in `.error_obj`.

## Complete migration example

### Before (Lua C API)

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

### After (clx C++ API)

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

### Building

```bash
# Lua C API module (needs Lua headers + shared lib)
gcc -shared -fPIC vector.c -o vector.so -llua

# clx C++ module — compile to static lib, then link
g++ -c -std=c++20 -I/path/to/clx/include vector.cpp -o vector.o
ar rcs vector.a vector.o                           # Linux/macOS
rem or: lib /OUT:vector.lib vector.obj              # Windows MSVC
clx main.lua --modules vector
```

## Common patterns

### Multi-return

**C API:** `lua_push*` for each value, `return N`.
**clx API:** `return clx::MultiValue({v1, v2, v3})`.

### Reading optional fields from a table argument

**C API:** `lua_getfield(L, 1, "opt"); if (lua_isnil(L, -1)) ...`
**clx API:** `clx::LValue opt = clx::raw_get(L, args[0], "opt"); if (clx::is_nil(opt)) ...`

### Checking argument count

**C API:** `int n = lua_gettop(L);`
**clx API:** The `count` parameter gives the argument count directly.

## Not supported

The following Lua C API features have no equivalent in clx:

- **`lua_load` / `luaL_loadfile` / `luaL_loadstring`** — clx compiles Lua to C++ ahead of time; runtime code loading is not supported.
- **`lua_dump` / `luac`** — No bytecode format.
- **`lua_pushcclosure`** — Use `clx::cfunction(L, func)` which wraps a `std::function`.
- **`lua_upvalueindex` / `lua_getupvalue` / `lua_setupvalue`** — Use lambda captures instead.
- **`lua_newstate` / `luaL_newstate`** — Use `clx::open()`.
- **`lua_gc` / `lua_setallocf`** — GC is automatic; no manual control exposed.
- **`luaL_openlibs`** — Use `clx::openlibs(L)` or individual `luastd_*()`.
- **`lua_tothread`** — Use `clx::tothread(v)`.
- **`lua_isyieldable`** — Use `clx::isyieldable(L)`.
- **`lua_pushvfstring` / `lua_pushfstring`** — Use `std::string` + `clx::string(L, ...)`.
- **`luaL_Buffer` / `luaL_add*`** — Use `clx::StringBuilder` or `std::string` + `clx::string(L, ...)`.

## Why clx does not expose a stack

The Lua C API was designed around the Lua VM. clx executes native C++ code generated ahead-of-time and therefore does not require a VM stack interface.

Benefits:

- fewer API calls
- fewer stack balancing bugs
- simpler native code
- easier debugging
- better optimization opportunities

## Common migration mistakes

### Using Lua argument numbers directly

Lua argument #1 becomes:

```cpp
args[0]
```

because clx uses standard C++ indexing.

### Returning primitive types

Incorrect:

```cpp
return 42;
```

Correct:

```cpp
return clx::integer(42);
```

### Recreating stack temporaries

Prefer storing values directly as variables rather than emulating stack pushes and pops.

## Performance notes

clx is designed around direct value manipulation rather than VM stack interaction. This typically results in:

- simpler generated code
- fewer temporary operations
- better compiler optimization opportunities

## Recommended porting strategy

1. Replace function signatures.
2. Replace argument extraction.
3. Replace return values.
4. Replace table operations.
5. Replace metatable operations.
6. Replace userdata.
7. Replace module registration.
8. Remove stack management code.

