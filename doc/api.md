# clx C++ API Reference

All functions are in `namespace clx`. Include `<clx.h>` to use the full API.
The API is value-based (no stack) — values are `LValue` objects, not stack indices.

## Lifecycle

```cpp
LState* L = open(argc, argv);  // create a clx state
luastd_base(L);                      // register base lib (_G, pcall, type, ...)
luastd_math(L);                      // register math lib
luastd_string(L);                    // register string lib
luastd_coroutine(L);                 // register coroutine lib
close(L);                           // cleanup
```

`L` owns all memory (string pool, GC objects, threads). Call `close()` exactly once.

## Values (`LValue`)

LValue is a 64-bit nan-boxed tagged union. Construct via factory functions:

| Function | Returns |
|---|---|
| `nil()` | LType::Nil |
| `boolean(bool)` | LType::Bool |
| `number(double)` | LType::Number |
| `integer(int64_t)` | LType::Integer |
| `string(L, s)` / `string(L, s, len)` | LType::String (interned; ≤5 bytes are TAG_ISTR inline) |
| `table(L, asize, hsize)` | LType::Table |
| `cfunction(L, func)` | LType::Function (CFunctionType) |
| `thread(LThread*)` | LType::Thread |
| `lightuserdata(void*)` | LType::Userdata |

Strings ≤5 bytes are stored directly in the LValue (TAG_ISTR) — no heap allocation, no interning.
`as_string()` returns a valid C string pointer for both inline and heap strings.

### Raw member access (from `clx_runtime.h`)

```cpp
LType t = v.type();
double n = v.as_number();
int64_t i = v.as_integer();
bool   b = v.as_bool();
const char* s = v.as_string();     // raw pointer (LType::String only)
void*  p = v.as_pointer();         // raw pointer (GC objects / userdata)
bool ok = v.to_number(double& out); // string-to-number conversion
std::string s = v.to_string(L);    // any type → string (respects __tostring)
uint32_t len = v.string_len();     // string length (inline or heap)
```

## Type Queries

```cpp
LType  type_of(v);
bool   is_nil(v), is_bool(v), is_number(v), is_integer(v);
bool   is_string(v), is_table(v), is_function(v), is_thread(v), is_userdata(v);
bool   is_none(v);       // always false — no stack sentinel
bool   is_noneornil(v);  // true if v is nil
```

## Type Names

```cpp
const char* type_name(LType t);     // e.g. "number", "string"
const char* type_name(const LValue& v);  // overload — same lookup
```

## State Queries

```cpp
bool isyieldable(L);  // true if current thread is not main
int   status(t);      // THREAD_SUSPENDED(0), RUNNING(1), DEAD(2), NORMAL(3)
```

## Lenient Conversions (use default on failure)

```cpp
double  to_number(v, def = 0.0);
int64_t to_integer(v, def = 0);
int64_t to_integerx(v, &isnum);  // *isnum = true/false
double  to_numberx(v, &isnum);
bool    to_boolean(v);
const char*  to_string(L, v, def = "");
void*        touserdata(v);
LThread*     tothread(v);
const void*  topointer(v);
LValue       stringtonumber(L, s);  // "3.14" → number(3.14), else nil
```

## Strict Conversions (throw `LRuntimeException` on failure)

```cpp
double      check_number(L, v);
int64_t     check_integer(L, v);
const char* check_string(L, v);
LTable*     check_table(L, v);
LCFunction* check_function(L, v);
```

## Checked Field Access (throw on type mismatch)

```cpp
int64_t     check_field_integer(L, v, "field_name");
double      check_field_number(L, v, "field_name");
const char* check_field_string(L, v, "field_name");
```

These validate that `v` has the expected type and return the value.
On mismatch they throw `LRuntimeException` with a message like
`field 'x' (integer expected, got string)` — unlike bare `as_integer()`
which silently produces garbage on the wrong type.

## Optional Conversions (nil → default, otherwise throw on mismatch)

```cpp
double      opt_number(L, v, def);
int64_t     opt_integer(L, v, def);
const char* opt_string(L, v, def);
```

## String Conversion (__tostring aware)

```cpp
LValue tolstring(L, v);  // string conversion with __tostring support
```

Returns the value as a string LValue. If `v` is already a string, returns `v` directly.
If `v` has a `__tostring` metamethod, calls it and returns the result.
Otherwise falls back to `to_string` + intern.

## Argument Validation

```cpp
void checktype(L, argnum, v, LType t);    // asserts v.type() == t
void checkany(L, v);                       // no-op
void argcheck(L, cond, argnum, msg);       // asserts cond
void argexpected(L, cond, argnum, v, wanted_type, extramsg = nullptr);  // asserts cond, reports actual type
```

## Error Helpers (all `[[noreturn]]`)

```cpp
void error(L, msg);          // throw LRuntimeException(string)
void arg_error(L, n, expected);   // "bad argument #N (expected expected)"
void type_error(L, n, expected);  // "bad argument #N (expected expected, got ...)"
```

## Globals

```cpp
LValue get_global(L, name);
void   set_global(L, name, val);        // val: LValue
void   set_global(L, name, double);     // convenience — wraps number
void   set_global(L, name, int64_t);    // convenience — wraps integer
void   set_global(L, name, const char*);// convenience — auto-interns string
```

`_G` is accessible via `get_global(L, "_G")` or the internal `L->_G`.

```cpp
clx::set_global(L, "pi",    3.14);         // double
clx::set_global(L, "count", int64_t(42));  // integer
clx::set_global(L, "name",  "hello");      // string (interned)
```

## Table Helpers

### With metamethod respect (`__index`, `__newindex`)

```cpp
LValue get_field(L, table, "key");
void   set_field(L, table, "key", val);
```

### Raw access (no metamethods)

```cpp
LValue raw_get(L, table, key);       // any key type (LValue, const char*, double, int64_t)
void   raw_set(L, table, key, val);
LValue raw_get_i(L, table, idx);     // integer key (1-based)
void   raw_set_i(L, table, idx, val);
```

Convenience overloads accept native C++ types directly:

```cpp
clx::raw_get(L, t, "hello");        // const char* key
clx::raw_get(L, t, 42.0);           // double key
clx::raw_get(L, t, int64_t(7));     // integer key (not implicitly converted from int)
clx::raw_set(L, t, "key", val);     // same for write
```

### Binding helpers

```cpp
void set_function(L, table, "name", func);    // binds a CFunctionType (renamed from bind_function)
void set_value(L, table, "name", val);         // binds an LValue raw (renamed from bind_value)
void set_functions(L, table, {reg1, reg2, ...}); // binds initializer_list<LReg> (renamed from bind_all)
void set_functions(L, table, regs);              // binds LReg[] (nullptr-terminated)

LValue new_lib(L, regs);         // create table + bind LReg[] (nullptr-terminated)
```

### Lazy function registration (no eager closures)

```cpp
void set_lazy_funcs(L, table, lazy_regs, count);
```

`set_lazy_funcs` attaches a `__index` metamethod that creates `LCFunction` closures on first
access and caches them on the table. Uses `constexpr`-friendly `LazyReg` arrays:

```cpp
static constexpr LazyReg my_funcs[] = {
    {"sin", my_sin},
    {"cos", my_cos},
};
clx::set_lazy_funcs(L, table, my_funcs, 2);
```

Subsequent lookups hit the cache directly (no metamethod call).

### `LReg` struct

```cpp
struct LReg {
    const char* name;
    CFunctionType func;  // std::function<MultiValue(LState*, const LValue*, size_t)>
};
```

### `LazyReg` struct

```cpp
struct LazyReg {
    const char* name;
    RawCFunction func;  // raw function pointer — constexpr-friendly
};
using RawCFunction = MultiValue(*)(LState*, const LValue*, size_t);
```

`LazyReg` uses raw function pointers instead of `std::function`, making the array `constexpr`.
`set_lazy_funcs` stores the `LazyReg*` as light userdata on the metatable; the array must
persist in static storage.

## Metatable Helpers

```cpp
LValue getmetatable(L, obj);          // returns metatable or nil (respects __metatable)
void   setmetatable(L, obj, mt);      // sets metatable (nil or table)
bool   rawequal(a, b);                // equality without __eq
MultiValue next(L, table, key);       // iterator — call with nil to start
LValue getmetafield(L, obj, field);   // get metatable[field] or nil
bool   callmeta(L, obj, event);       // call metatable[event](obj), returns true if called
```

## Table Iteration

```cpp
table_iterator iterate(L, table);  // range-style iteration over table entries
```

Returns a `table_iterator` yielding `{key, value}` pairs via `operator*`.
The iterator is truthy while there are more entries:

```cpp
for (auto it = iterate(L, my_table); it; ++it) {
    auto [key, value] = *it;  // structured binding (C++17)
    // use key, value
}
```

The iterator calls `next()` internally and handles all table types (array + hash).

## Length / Concat

```cpp
int64_t len(L, v);        // # operator, respects __len
int64_t rawlen(v);        // raw #, bypasses __len (string/table only)
LValue concat(L, a, b);   // .. operator, respects __concat
```

## Function Call

```cpp
MultiValue call(L, func, args, count);        // direct call, throws on error
MultiValue call(L, func, arg1, arg2, ...);    // variadic — native C++ types accepted
MultiValue pcall(L, func, args, count);       // protected call — returns {true, ...} or {false, err}
MultiValue pcall(L, func, arg1, arg2, ...);   // variadic — native C++ types accepted
```

`args` is a pointer to `count` LValues. Use a single LValue for 1 arg, an array for many:

```cpp
LValue arg = number(42);
MultiValue r = call(L, myfunc, &arg, 1);
```

The variadic overload accepts any mix of LValue and native C++ types:

```cpp
MultiValue r = call(L, myfunc, number(10), integer(20), "hello", 3.14);
// Equivalent to: myfunc(10, 20, "hello", 3.14)
```

## Coroutines

```cpp
LValue      create_thread(L, func, stack_size = 1<<20);    // create a coroutine
MultiValue  resume(L, thread, args, count);  // resume suspended coroutine
MultiValue  yield(L, args, count);           // yield from current coroutine (non-main only)
```

`resume` returns `{true, ...yielded_values}` or `{false, error_msg}`.
`yield` returns the MultiValue passed to the next `resume`.

## Core Types (from `clx_runtime.h`, exposed via `<clx.h>`)

```
LType         — tagged type enum
LValue        — nan-boxed value (with TAG_ISTR for strings ≤5 bytes)
MultiValue    — multi-return container (count, operator[])
LState        — VM state (opaque)
LThread       — coroutine (opaque, use create_thread/resume/yield)
LTable        — table (gettable/settable for raw access)
LCFunction    — C function closure
LReg          — {name, CFunctionType} for module registration
LazyReg       — {name, RawCFunction} for constexpr lazy registration
RawCFunction  — raw C function pointer type
CFunctionType — std::function<MultiValue(LState*, const LValue*, size_t)>
LRuntimeException — thrown on Lua errors; .error_obj holds the error LValue
```

### `LValue` constructors (raw, from `clx_runtime.h`)

```cpp
LValue();                    // nil
LValue(bool);
LValue(double);
LValue(int64_t);
LValue(const char*);         // raw interned string pointer (or string literal)
LValue(LType, LHeader*);     // GC object
LValue::istr(s, len);        // static — inline string (≤5 bytes, no interning)
```

### `MultiValue`

```cpp
struct MultiValue {
    size_t count;
    LValue  operator[](size_t i) const;  // access by index
    // construct from initializer list, single LValue, or array+size
};
```

## Thread Status Constants

```cpp
constexpr int THREAD_SUSPENDED = 0;
constexpr int THREAD_RUNNING   = 1;
constexpr int THREAD_DEAD      = 2;
constexpr int THREAD_NORMAL    = 3;
```

## Module Registration

Each standard module has a `luastd_*` function that creates and sets the global table:

```cpp
void luastd_base(LState* L);       // registers _G, print, pcall, type, error, ...
void luastd_math(LState* L);       // registers math table
void luastd_string(LState* L);     // registers string table
void luastd_coroutine(LState* L);  // registers coroutine table
```

Or call `openlibs(L)` to register all libraries at once. Call individual `luastd_*` functions after `open()` before using the corresponding Lua features.

## Global Helpers

```cpp
LValue get_global(L, name);
void   set_global(L, name, val);
```

## `LValue` to-string (raw, from `clx_runtime.h`)

```cpp
std::string v.to_string(L);  // any type → string (respects __tostring)
```

## Example: C++ module with lazy registration

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

Compile this file into an object/library, then link it with `clx main.lua --modules mylib`.
clx's generated `main()` calls `register_module("mylib", luaopen_mylib)` at startup.
The module becomes available via `require("mylib")` at runtime.

## Example: coroutine

```cpp
clx::LValue co_func = clx::cfunction(L, [](clx::LState* L2, const clx::LValue* a, size_t n) {
    double v = clx::check_number(L2, a[0]);
    clx::LValue y = clx::number(v * 2);
    auto resumed = clx::yield(L2, &y, 1);          // yield twice the input
    double r = clx::check_number(L2, &resumed[0], 1);
    return {clx::number(r * 3)};                    // triple the resume value
});

clx::LValue th = clx::create_thread(L, co_func);
clx::LValue arg = clx::number(10);
auto r = clx::resume(L, th, &arg, 1);              // {true, 20}
clx::LValue arg2 = clx::number(7);
auto r2 = clx::resume(L, th, &arg2, 1);            // {true, 21}
```
