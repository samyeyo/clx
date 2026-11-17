# The high level object oriented C++ API: `clx.hpp`

`clx.hpp` is clx's high-level C++ binding: a small, value-semantic object layer on top of
the low-level [`clx.h`](./clx-api.md) API. It keeps Lua's semantics — number subtypes,
metamethods, multiple returns, coroutines — but exposes them as ordinary C++ objects:

```cpp
namespace lua = clx::lua;

lua::State L;                                   // opens clx and the standard libraries (if code is not a module)

lua::Table cfg { { "name", "clx" }, { "debug", true }, 1, 2, 3 };
cfg["answer"] = 42;
cfg["greet"]  = [](std::string who) { return "hello " + who; };

std::string hi = cfg["greet"]("world").as<std::string>();
for (auto [key, value] : cfg)
    L["print"](key, value);
```

There is no stack to manage. `lua::Value` holds any Lua value, `lua::Table`,
`lua::String`, `lua::Function`, `lua::Thread` and `lua::UserData` are thin typed handles
over it, and C++ types convert to and from Lua values automatically.

This page is the tour. For the stack-free C-style layer (`clx::LValue`, `clx::get_field`,
`clx::call`, ...) see [the C++ API](./clx-api.md); for stock Lua C modules see
[the Lua C API](./luacapi.md). To package native code as a module findable by
`require`, see [Lua modules](./modules.md).

## Setup

```cpp
#include "clx.hpp"

namespace lua = clx::lua;
```

The header is installed next to `clx.h`, so `-I <clx include dir>` (or the `include`
folder of a source checkout) is all a compiler needs. It requires C++20.

`lua::State` creates and owns a clx state:

```cpp
lua::State L;                       // fresh state, standard libraries loaded
lua::State L(argc, argv);           // same, with the host program's command line
lua::State L(existing);             // wrap an LState* you already own (not closed)
```

The most recent `lua::State` (or `lua::Scope`) is the *current* state. Objects built
without being handed a state use it, which is why `lua::Table t;` or
`lua::String("x")` need no arguments. Inside a callback there is always a current
state, so the API is symmetric there.

```cpp
clx::LState* raw = L.raw();                    // for the low-level API or C headers
clx::LState* cur = lua::State::current();      // current state, error if there is none
clx::LState* maybe = lua::State::try_current();// nullptr when there is none
```

`lua::Scope` re-installs a state for a block, which is what module entry points and
host code that interleaves two states use:

```cpp
{
    lua::Scope scope(L.raw());
    lua::Table t;       // built in L
}
```

## Values

`lua::Value` is any Lua value. It converts implicitly from anything Lua can hold:

```cpp
lua::Value a = 1;                       // integer
lua::Value b = 3.5;                     // float
lua::Value c = "text";                  // string
lua::Value d = true;                    // boolean
lua::Value e;                           // nil
lua::Value f = std::vector { 1, 2, 3 }; // table
lua::Value g = [](int n) { return n * 2; };   // Lua function
```

Inspect a value with the `is_*` predicates and Lua's truthiness:

```cpp
v.is_nil();  v.is_boolean(); v.is_number(); v.is_integer(); v.is_float();
v.is_string(); v.is_table(); v.is_function(); v.is_userdata(); v.is_thread();

v.truthy();          // false only for nil and false
if (v) { ... }       // same, as an explicit bool
v.type();            // ValueType::Table, ...
v.type_name();       // "table", "integer", "string", ...
v.size();            // the # operator (honours __len)
v.to_string();       // tostring(v)
```

Read typed values with `as<T>()`, or use the non-throwing alternatives:

```cpp
int n       = v.as<int>();
double d    = v.as<double>();
std::string s = v.as<std::string>();
lua::Table t  = v.as<lua::Table>();          // Lua error if v is not a table

if (auto i = v.get<int>())                   // std::optional
    use(*i);
int port = v.value_or(8080);                 // default when the type doesn't match
if (v.is<int>()) { ... }
```

`as<T>()` follows Lua's own conversion rules (a number is acceptable where a string is
expected, an integral value must have an exact integer representation) and raises a Lua
error on mismatch — the same error a Lua script would see, catchable as
[`lua::Error`](#errors).

Every object derived from `Value` registers itself as a GC root while it is alive, so a
local `lua::Table` never needs a "keep alive" call.

## Indexing

Indexing produces a **proxy** — an lvalue that reads on use and assigns on write:

```cpp
lua::Table t;
t["name"] = "clx";              // honours __newindex
int n = t["n"];                 // honours __index
t["nested"] = lua::Table {};    // intermediate tables must exist...
t["nested"]["k"] = 5;          // ...then chained assignment works

t["k"].exists();                // true when the read is not nil
t["k"].is_nil();
t["k"].as<int>();               // typed read
t["f"](1, 2);                   // call the stored function (no self)
t["obj"].invoke("run", 1);      // t["obj"]:run(1)
```

Reading a `Proxy` in a context that needs a C++ type converts it: `int n = t["n"]`,
`std::string s = t["name"]`. `lua::State::operator[]` is the same proxy on the globals
table, so `L["print"](x)` and `L["count"] = 0` work as expected.

## Numbers

`lua::Number` is a Lua number: an integer (64-bit) or a float, with Lua's rules for
promotion, wrapping integer arithmetic, floor division and floor modulo. All of it is
`constexpr` and needs no state, so it can be used in pure C++ code.

```cpp
lua::Number a = 7, b = 2;
a + b;                 // 9   (integer)
a / b;                 // 3.5 (Lua: integer / integer is a float)
a % b;                 // 1
lua::idiv(a, b);       // 3   (`//`)
lua::pow(a, b);        // 49  (`^`, always a float)
a & b; a | b; a ^ b;   // bitwise, like Lua
a.floor(); a.ceil(); a.abs(); a.sqrt();
```

Interrogate and convert:

```cpp
n.is_integer(); n.is_float();
n.to_double();                 // always works
n.to_integer();                // std::optional<int64_t>
n.integer();                   // truncating, never fails
n.to_string();                 // "3", "3.5", "1e+100"
```

Comparison is exact between the two representations (`1 == 1.0` is true, and
`operator<=>` is total for C++ containers), and `Boolean` / `nil` are the matching
tags:

```cpp
lua::Boolean flag = true;
if (!flag) { ... }
lua::Value nothing = lua::nil;
```

## Tables

Build tables the way Lua writes them — positional values and `{ key, value }` pairs can
be mixed:

```cpp
lua::Table cfg { { "name", "clx" }, { "debug", true }, "extra" };
lua::Table empty;
lua::Table sized(lua::Table::reserve_t {}, 1000);   // preallocate
lua::Table from_range(std::vector { 1, 2, 3 });     // arrays from any C++ range
lua::Table from_map(std::map<std::string, int> { { "a", 1 } });
```

The range constructors are `explicit`, so write `lua::Table t(vec);`, not
`lua::Table t = vec;`.

Access, sizes and mutation:

```cpp
t.get("k");                    // metamethod aware
t.set("k", 1);
t.raw_get("k");                // no metamethods
t.raw_set("k", 1);
t.field<int>("port", 8080);    // typed read with a default
t.push_back(v);                // append at #t + 1
t.append(range);               // append every element of a C++ range
t.raw_size();                  // #t, raw
t.empty();
t.to_vector<int>();            // sequence -> std::vector
t.clone();                     // shallow copy, keeps the metatable
```

Iteration covers both Lua loops:

```cpp
for (auto [key, value] : t)           // pairs()
    std::cout << key.to_string() << '\n';

for (auto [index, value] : t.ipairs()) // ipairs(), stops at the first nil
    std::cout << index << '\n';
```

The `table` library is available as methods:

```cpp
t.insert(v);  t.insert(pos, v);  t.remove();  t.remove(pos);
t.concat(", ");                    // -> lua::String
t.sort();  t.sort(comp);           // comp: any C++ callable returning bool
t.move(1, 3, 5);                   // table.move
t.unpack();                        // -> Results
lua::Table packed = lua::Table::pack(1, 2, 3);
```

## Strings

`lua::String` builds from anything string-like, and behaves like `std::string_view`
plus the whole `string` library:

```cpp
lua::String s = "Hello, world";
std::string_view view = s;     // valid while s lives and is not moved
s.str();                       // owning std::string copy
s.c_str();  s.length();  s.size();  s.empty();
s.starts_with("He");  s.ends_with("ld");  s.contains("wor");
```

```cpp
s.sub(1, 5);        // "Hello"
s.upper();  s.lower();  s.reverse();  s.rep(3);  s.rep(3, ", ");

std::optional<int64_t> c = s.byte(1);
lua::Results codes = s.byte(1, 3);
lua::String ch = lua::String::chr(76, 117, 97);   // string.char
lua::String f = lua::String("%d items").format(3);

s.find("world");                 // -> Results: start, end, captures
s.match("(%a+)");                // -> Results: captures
for (auto captures : s.gmatch("%a+")) { ... }
auto [replaced, count] = s.gsub("o", "0");       // repl: string, table or callable

lua::String data = lua::String("i4").pack(7);     // string.pack
lua::Results values = lua::String("i4").unpack(data);
lua::String("i4").packsize();                     // 4
```

`pack`, `unpack` and `packsize` are methods on the *format* string, mirroring
`string.pack`, `string.unpack` and `string.packsize`.

The literal operator avoids the extra conversion at call sites:

```cpp
using namespace clx::lua::literals;
lua::String name = "clx"_s;
```

## Functions and calls

Any C++ callable becomes a Lua function: lambdas, function pointers, `std::function`,
member functions through [`lua::Class`](#userdata-objects-and-classes).

```cpp
lua::Function add = [](int a, int b) { return a + b; };
lua::Function noisy = [&calls](std::string msg) { ++calls; return msg; };
```

Arguments are converted with the same rules as `as<T>()`; a missing argument reads as
`nil`, extra arguments are ignored, and a failed conversion raises a Lua error that
`pcall` in Lua can catch.

Return values decide how many values Lua sees:

| C++ return | Lua sees |
|---|---|
| `void` | no values |
| a single value | one value |
| `std::pair` / `std::tuple` | one value per element |
| `lua::Results` | the listed values |
| `clx::MultiValue` | passed through untouched |
| a `std::vector` / range | a table |

Calling is symmetric:

```cpp
lua::Value r = add(2, 3);                  // first result
auto [x, y] = some_fn.call<2>(1);          // exactly two, padded with nil
lua::Results all = some_fn.call_multi(1);  // everything
lua::PResult safe = some_fn.pcall(1);      // never throws
t["obj"].invoke("run", 1);                 // method call with self
```

`Results` behaves like a small vector of `lua::Value`: `size()`, `empty()`, `operator[]`
(missing indices read as nil), range-for, and `unpack<N>()` for structured bindings. It
converts implicitly to the first value's type, so `if (s.find("x"))` and
`std::string w = results;` both work.

## Errors

A Lua error thrown by a call propagates as a C++ exception:

```cpp
using lua::Error;      // = clx::LRuntimeException

try {
    L["boom"](1);
} catch (const lua::Error& e) {
    std::cerr << e.what() << '\n';
}
```

From inside a C++ function exposed to Lua, raise one with `lua::error` or `lua::check`;
a `std::exception` thrown by a callback is converted to a Lua error automatically, and
`lua::Error` passes through unchanged.

```cpp
lua::error("boom " + std::to_string(code));
lua::check(n > 0, "n must be positive");
```

Use `pcall` when a failure is expected or recoverable — it reports instead of throwing:

```cpp
lua::PResult r = L["boom"].pcall(7);
if (!r) {                                  // or r.ok == false
    std::cerr << r.message() << '\n';      // "boom 7"
}
lua::Value v = r.value();                  // first result on success
lua::Value e = r.error();                  // error object on failure
```

## Operators

Arithmetic and comparison on `lua::Value` / `lua::Table` / `lua::String` go through the
runtime, so metamethods (`__add`, `__eq`, `__lt`, ...) behave exactly as in Lua, and C++
values on either side are converted first:

```cpp
lua::Value sum = lua::Value(1) + 2;
lua::Value prod = a * b;        // a, b: any value
lua::idiv(a, b);  lua::pow(a, b);  lua::concat(a, b);
a == b;  a < b;  a <= b;  a > b;  a >= b;
auto order = a <=> b;           // std::partial_ordering, for sorting and maps
```

`+` is arithmetic, except that it concatenates as soon as one operand is a
`lua::String`; the result is then a `lua::String`:

```cpp
lua::String both = lua::String("x") + lua::String("y");   // "xy"
```

Comparing a `lua::String` with a `std::string` or string literal compares the bytes
directly, without creating a Lua string.

## Conversions

These C++ types map onto Lua values and back:

| C++ type | Lua value |
|---|---|
| `bool`, `lua::Boolean` | boolean |
| integral types, `std::is_enum` | integer |
| floating-point types | float |
| `lua::Number` | number, keeping the subtype |
| `std::string`, `std::string_view`, `const char*` | string |
| `std::optional<T>` | `T`, or nil when empty |
| any input range | array table |
| a range of `std::pair` | map-style table |
| `std::vector<T>` | array table, and back |
| callables | function |
| `lua::nil` | nil |
| `Value` and its subclasses | the value itself |

Two concepts name the two directions: `lua::ToLua<T>` and `lua::FromLua<T>`. The
underlying entry points are usable directly when a template needs them:

```cpp
clx::LValue raw = lua::to_lvalue(L, some_cpp_value);
lua::Table t = lua::from_lvalue<lua::Table>(L, raw);
lua::Value v = lua::Value::make(L, some_cpp_value);   // explicit state, no current state needed
```

(`clx::LValue` is the raw tagged value from `clx.h`; `L` above is a `clx::LState*`.)

### Teaching the API about your own type

Specialize `clx::lua::Convert` for a type that has a natural table representation:

```cpp
struct Vec2 { double x = 0, y = 0; };

template<> struct clx::lua::Convert<Vec2> {
    static clx::LValue to_lua(clx::LState* L, const Vec2& v)
    {
        lua::Table t(L, 2);        // array part preallocated
        t.push_back(v.x);
        t.push_back(v.y);
        return t.raw();
    }

    static Vec2 from_lua(clx::LState* L, const clx::LValue& v, int argn)
    {
        lua::Table t = lua::Table::cast(L, v, argn);
        return Vec2 { t[1].as<double>(), t[2].as<double>() };
    }
};
```

After that the type works everywhere a built-in does — as an argument, a return value,
a table entry:

```cpp
L["vec_len"] = [](Vec2 p) { return p.x * p.x + p.y * p.y; };
lua::Value v = Vec2 { 1.5, 2.5 };
Vec2 back = v.as<Vec2>();
```

## Userdata: objects and classes

`lua::UserData` creates a Lua userdata holding a C++ object, and `lua::Class<T>`
describes how that object looks from Lua. Construction and destruction of `T` are
handled by the API — `__gc` is installed automatically for non-trivially-destructible
types.

```cpp
struct Counter {
    int n = 0;
    explicit Counter(int start) : n(start) { }
    void inc() { ++n; }
    int get() const { return n; }
};

lua::Class<Counter>("Counter")
    .constructor<int>(L.globals(), "new_counter")      // Counter.new... as a factory
    .method("inc", &Counter::inc)                      // c:inc()
    .method("get", &Counter::get)                      // c:get()
    .function("__tostring", [](const lua::UserData& self) {
        return "Counter(" + std::to_string(self.as_ref<Counter>(1).n) + ")";
    });
```

```lua
local c = new_counter(5)
c:inc()
print(c:get())      --> 6
print(c)            --> Counter(6)
```

`method` binds member functions, `function` binds anything under a metatable key
(`__tostring`, `__eq`, `__add`, `__index`, plain functions), and `constructor` registers
a factory that builds a `T`.

The name given to `Class<T>` is stored as `__name` in the metatable, so an object with
no `__tostring` prints as `Counter: 0x...` instead of bare `userdata` (the object is
always still `"userdata"` to `type`).

C++ side access:

```cpp
lua::UserData ud = lua::UserData::create<Counter>(3);   // standalone object
Counter* p = ud.get<Counter>();      // nullptr when it holds something else
ud.holds<Counter>();
Counter& c = ud.as_ref<Counter>();   // raises a Lua error on mismatch
lua::Class<Counter>("Counter").metatable();
```

Member functions are called with the object as first argument, exceptions behave like
any other callback, and the constructor's argument types are converted like all others.

> Over-aligned types (`alignof(T) > alignof(void*)`) are rejected at compile time.

## Coroutines

`lua::Thread` wraps a Lua thread:

```cpp
lua::Function body = []() {
    for (int i = 0; i < 3; ++i)
        lua::Thread::yield(i);
    return 3;
};

lua::Thread co(body);                 // suspended, not started
while (!co.dead()) {
    lua::PResult step = co.resume();  // { ok, yielded values... }
    if (!step) {
        std::cerr << step.message() << '\n';
        break;
    }
}

co.status();      // "suspended" | "running" | "normal" | "dead"
co.suspended();  co.dead();
co.close();       // coroutine.close
lua::Thread::yieldable();
```

A coroutine body receives whatever `resume` passes it, so the first call must supply
any required arguments (`resume(1)` for a body taking an argument, `resume()` for none).
`lua::Thread::yield` is only valid inside a running coroutine.

## Writing a native module

A module is a function returning a table, exported under the name
`luaopen_<module>` with C linkage. `lua::Scope` installs the state for the duration of
the call:

```cpp
// counter.cpp
#include "clx.hpp"

namespace lua = clx::lua;

//------------------ Module opener: `clx app.lua --modules counter` looks for this symbol
CLX_API clx::LValue luaopen_counter(clx::LState* L)
{
    lua::Scope scope(L);
    lua::Table module;
    module["next"] = [](int n) { return n + 1; };
    return module.raw();
}
```

Build it into an archive, then link it by putting the archive in the working directory
and passing `--modules`:

```bash
c++ -std=c++20 -O2 -fPIC -I <clx include dir> -c counter.cpp -o counter.o
ar rcs counter.a counter.o

clx app.lua --modules counter -o app
```

Inside `app.lua`, `require "counter"` returns the table. See
[Lua modules](./modules.md) for archive naming and search paths, and
[`examples/fs`](../examples/fs) for a larger, commented module.

With `--minimal` the program links only the base and package libraries, so a module
that uses the `string` or `table` library must ask for those archives too:

```bash
clx app.lua --minimal --modules counter,string,table -o app
```

Methods such as `String::find`, `String::gsub`, `String::format`, `String::pack` are
thin wrappers over the string library, and `Table::sort`, `Table::concat`,
`Table::insert`, `Table::remove`, `Table::move`, `Table::unpack`, `Table::pack` over the
table library; without the matching `--modules` entry the link fails with an undefined
`clx::str_*` or `clx::table_*` reference. The rest of the API — indexing, `size`,
`push_back`, iteration, number/string conversions — needs nothing beyond base.

Do not instantiate `lua::State` inside a module: its constructor calls `clx::openlibs`,
which references every standard library, so a `--minimal` link fails with undefined
`clx::luastd_*` symbols. A module receives the host state and wraps it with
`lua::Scope`; `lua::State` belongs to programs, not modules.

## Lifetime and GC rules

* Every `lua::Value` (and subclass) roots what it refers to while alive, so locals and
  members are safe between calls. Rooting is not free — keep long-lived handles, not
  one handle per element.
* Objects must be destroyed **before** the `lua::State` that created them. Declare
  `lua::State` first (or outermost), then the objects.
* Copying mimics Lua: copying a `lua::Table` copies the handle, not the table. Use
  `clone()` for a copy.
* `String::view()` and `c_str()` point into the `String` object itself (short strings
  live inline in the handle): keep the object alive, and don't move it while using the
  view.
* Each state is separate: never pass a value created in one state to another.
* Calling `lua::error` or a conversion that fails without a current state is a
  programming error and reports "no active lua::State".

## Output helpers

Values print and format like their Lua text:

```cpp
std::cout << t << '\n';                    // operator<< on Value / Proxy / Number / nil
std::string s = std::format("{}", t);      // std::formatter for every Value subclass
                                           // and for Number
```

## Pitfalls

* Don't combine `using namespace clx;` with `using namespace clx::lua;`: `clx::Table`,
  `clx::String`, ... are `ValueType` constants in the low-level namespace and collide
  with the classes. The alias `namespace lua = clx::lua;` is the intended style.
* `as<T>()` is strict and raises a Lua error; use `get<T>()`, `is<T>()` or `value_or`
  when a value may be absent.
* `Table`'s range constructors are `explicit`.
* Chained assignment (`t["a"]["b"] = 1`) needs `t["a"]` to already be a table — the
  proxy dispatches through it rather than creating it.
* This API is a binding, not an optimizer: a C++ callback per Lua element is
  usually slower than the equivalent Lua loop compiled by clx. Batch work in C++ and
  cross the boundary once.

## Binary size

All of this comes at a tiny cost: about **6% of the final executable's size**.
Compiling the module with `-Os` removes about 40% of that difference; if a few kilobytes 
matter more than the convenience, use the [C++ API](./clx-api.md) directly.

## See also

- [C++ API (`clx.h`)](./clx-api.md) — the low-level, stack-free layer this builds on
- [Lua C API](./luacapi.md) — stock Lua 5.5 modules compiled unchanged
- [Lua modules](./modules.md) — packaging and linking native modules
- [Migration guide](./migration-guide.md) — porting C modules
- [`examples/fs`](../examples/fs) — a complete module using this API
