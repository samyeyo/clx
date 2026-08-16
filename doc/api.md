# clx C++ API

If you want to extend clx with native C++ code — for example a high-performance
library module — this guide shows you the API.

Everything lives in `namespace clx` and starts with `#include <clx.h>`.

One thing sets clx apart from the classic Lua C API: **there's no stack**. You
work directly with `LValue` objects, which is closer to normal C++. That makes
the API smaller and less error-prone.

## Setting up and tearing down

```cpp
clx::LState* L = clx::open();        // create a runtime
clx::openlibs(L);                    // load the standard libraries
// ... your code ...
clx::close(L);                       // clean up
```

`L` owns all the memory (strings, tables, threads). Call `close()` exactly once
when you're done.

## Values: `LValue`

An `LValue` can hold any Lua value: `nil`, boolean, number, string, table,
function, userdata, or thread. You create values with factory functions rather
than pushing them onto a stack:

| Function | Gives you |
|---|---|
| `nil()` | `nil` |
| `boolean(bool)` | a boolean |
| `number(double)` | a floating-point number |
| `integer(int64_t)` | an integer |
| `string(L, s)` | a string (short strings are stored in place, no allocation) |
| `table(L)` | a table |
| `cfunction(L, func)` | a function |
| `lightuserdata(void*)` | an opaque pointer |

Read values back with the `as_*` helpers:

```cpp
double n = v.as_number();      // as a number
int64_t i = v.as_integer();    // as an integer
const char* s = v.as_string(); // as a string
```

### Quick type checking

```cpp
clx::is_number(v);   clx::is_string(v);   clx::is_table(v);
clx::is_function(v); clx::is_integer(v);  clx::is_nil(v);
clx::is_bool(v);     clx::is_userdata(v);
clx::type_name(v);   // "number", "string", ...
```

## Conversions

Two families help you read arguments safely:

**`to_*`** — lenient. Use a default when the value isn't what you expect:

```cpp
double d = clx::to_number(v, 0.0);   // v, or 0.0 if v isn't a number
```

**`check_*`** — strict. Throw a `LRuntimeException` on mismatch:

```cpp
double d = clx::check_number(L, args[0]);
const char* s = clx::check_string(L, args[1]);
```

**`opt_*`** — treat `nil` as the default, otherwise throw on mismatch:

```cpp
double d = clx::opt_number(L, args[0], 1.0);
```

## Globals

```cpp
clx::LValue g = clx::get_global(L, "name");
clx::set_global(L, "name", val);
clx::set_global(L, "price", 3.14);        // numbers auto-wrapped
clx::set_global(L, "count", int64_t(42)); // integers
clx::set_global(L, "greeting", "hi");     // strings auto-interned
```

## Tables

Read and write with the `get_field` / `set_field` helpers (which respect
`__index` / `__newindex`), or use `raw_get` / `raw_set` to bypass metatables:

```cpp
clx::LValue v = clx::get_field(L, t, "key");     // t["key"], with __index
clx::set_field(L, t, "key", val);                 // t["key"] = val
clx::LValue v = clx::raw_get(L, t, 7);            // raw access, any key type
```

Iterate a table with a C++ range loop:

```cpp
for (auto it = clx::iterate(L, t); it; ++it) {
    auto [key, value] = *it;
    // use key and value
}
```

Length and concatenation:

```cpp
clx::len(L, v);      // # operator
clx::concat(L, a, b); // string concatenation
```

## Calling functions

```cpp
clx::LValue f = clx::get_global(L, "myfunc");
clx::LValue args[] = { clx::number(1.0), clx::number(2.0) };
clx::MultiValue r = clx::call(L, f, args, 2);   // throws on error
```

Or use `pcall` to catch errors instead of throwing:

```cpp
clx::MultiValue r = clx::pcall(L, f, args, 2);  // {true, ...} or {false, err}
```

A variadic form accepts native C++ values directly:

```cpp
clx::MultiValue r = clx::call(L, f, clx::number(1), "hello", 3.14);
```

## Coroutines

```cpp
clx::LValue t = clx::create_thread(L, func);
clx::MultiValue r = clx::resume(L, t, args, 1);  // resume a coroutine
clx::MultiValue r = clx::yield(L, args, 1);      // yield from a coroutine
```

`resume` returns `{true, ...results}` or `{false, error_message}`.

## Errors

Throw errors as exceptions:

```cpp
clx::error(L, "something went wrong");
// or
throw clx::LRuntimeException(clx::string(L, "oops"));
```

There are helpers for common argument errors:

```cpp
clx::arg_error(L, 1, "number"); // "bad argument #1 (number expected)"
clx::type_error(L, 1, "number"); // "bad argument #1 (number expected, got X)"
```

## Writing a module

A native module is a C++ source file that exports one function
(`luaopen_<name>`) which returns a table. See [Modules](./modules.md) for the
full worked example and how to compile and link it.

A complete example using lazy registration:

```cpp
#include <clx.h>

static clx::MultiValue add(clx::LState* L, const clx::LValue* args, size_t n) {
    double sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += clx::check_number(L, args[i]);
    return {clx::number(sum)};
}

static constexpr clx::LazyReg my_funcs[] = {
    {"add", add},
};

CLX_API clx::LValue luaopen_mylib(clx::LState* L) {
    clx::LValue t = clx::table(L);
    clx::set_lazy_funcs(L, t, my_funcs, 1);
    clx::set_global(L, "mylib", t);
    return clx::LValue();
}
```

Compile this into an object or library, then link it with:

```bash
clx main.lua --modules mylib
```

Your module then becomes available as `require("mylib")`.

## Porting an existing Lua C module?

The [Migration Guide](./migration-guide.md) walks you through converting a module
written against the classic Lua C API.