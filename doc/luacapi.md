# The Lua C API in clx

clx ships the standard Lua 5.5 headers — `lua.h`, `lauxlib.h` and `lualib.h` —
plus a bridge that implements them on top of the clx runtime. C modules written
against the official API compile and run unchanged: no source port, no Lua
library to link against.

This page is for people *using* the C API from C or C++. It covers how to write
a module, what the bridge implements, and where it behaves differently from
stock Lua. For compiling, finding and linking archives, see
[Lua modules](./modules.md). If you would rather write native code in C++,
see the [C++ API](./api.md) instead.

## A first module

```c
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static int l_add(lua_State *L)
{
    lua_Integer a = luaL_checkinteger(L, 1);
    lua_Integer b = luaL_checkinteger(L, 2);
    lua_pushinteger(L, a + b);
    return 1;
}

int luaopen_demo(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"add", l_add},
        {NULL, NULL}
    };
    luaL_newlib(L, funcs);
    return 1;
}
```

```lua
-- main.lua
local demo = require("demo")
assert(demo.add(2, 3) == 5)
```

```bash
cc -c -O2 -I/path/to/clx/include demo.c -o demo.o
ar rcs demo.a demo.o

clx main.lua --modules demo -o main
./main
```

Three things follow from how clx links modules:

- The archive must be named after the module — `demo.a` (`demo.lib` on
  Windows) — and export a plain `extern "C" int luaopen_demo(lua_State*)`.
- Compile the module **as C**, or wrap its opener in `extern "C"` if you
  compile it as C++. clx decides how to call the opener by scanning the
  archive's symbols: a *mangled* `luaopen_demo` is treated as a C++ module and
  expected to have the signature `clx::LValue luaopen_demo(clx::LState*)`,
  which a stock Lua module does not have.
- The opener runs **once, eagerly, when the program starts** — before your
  entry chunk runs. `require("demo")` then returns the value already cached in
  `package.loaded`.

The headers work from C99 onwards and from C++. Put clx's `include/`
directory on your C compiler's include path and do **not** link `-llua` — the
`lua_*`/`luaL_*` symbols come from clx's own wrapper archive (see
[Linking](./modules.md#linking-the-c-api-wrapper-is-its-own-archive)).

## The state

There is exactly one `lua_State` per program, created by `clx::open()` and
destroyed by `clx::close()`.

| Stock Lua | clx |
|---|---|
| `lua_newstate(alloc, ud, seed)` | returns `NULL` — you cannot create a state |
| `luaL_newstate()` | returns `NULL` |
| `lua_close(L)` | does nothing; the runtime owns the lifetime |

Everything else about the state is real. The registry, the global table and
`package.loaded` / `package.preload` are the *same tables* the compiled Lua
code uses:

- `lua_pushglobaltable(L)` gives you `_G`;
- `registry[LUA_RIDX_GLOBALS]` is `_G`, `registry[LUA_RIDX_MAINTHREAD]` is the
  main thread;
- `registry["_LOADED"]` is `package.loaded` and `registry["_PRELOAD"]` is
  `package.preload`, so `luaL_requiref()` and Lua's `require()` see each
  other's entries;
- `lua_getextraspace(L)` works — clx reserves `LUA_EXTRASPACE` bytes in front
  of every `lua_State`.

## What is implemented

Every function in `lua.h`, `lauxlib.h` and `lualib.h` is declared and has a
definition (147 API names in total). The list below is by area; the
[Not implemented](#not-implemented) section names every stub explicitly.

| Area | Status |
|---|---|
| Stack: `lua_gettop`, `lua_settop`, `lua_pushvalue`, `lua_rotate`, `lua_copy`, `lua_absindex`, `lua_pop`, `lua_insert`, `lua_remove`, `lua_replace` | ✅ |
| `lua_checkstack` | ⚠️ always returns `1` — see [Other stubs](#other-stubs) |
| Types and conversion: `lua_type`, `lua_typename`, `lua_is*`, `lua_tonumberx`, `lua_tointegerx`, `lua_toboolean`, `lua_tolstring`, `lua_rawlen`, `lua_tocfunction`, `lua_touserdata`, `lua_tothread`, `lua_topointer` | ✅ |
| Pushing: `lua_pushnil/number/integer/lstring/string/fstring/vfstring/boolean/lightuserdata/cclosure/thread/pushexternalstring` | ✅ |
| Tables: `lua_createtable`, `lua_newtable`, `lua_get{table,field,i,global}`, `lua_set{table,field,i,global}`, `lua_raw{get,geti,getp,set,seti,setp}`, `lua_getmetatable`, `lua_setmetatable`, `lua_next`, `lua_len` | ✅ |
| Userdata: `lua_newuserdatauv`, `lua_getiuservalue`, `lua_setiuservalue` | ✅ |
| Arithmetic and comparison: `lua_arith`, `lua_compare`, `lua_rawequal`, `lua_concat` | ✅ |
| Calls: `lua_callk` / `lua_call`, `lua_pcallk` / `lua_pcall` | ✅ — but the continuation argument is ignored, see [Differences](#what-differs-from-stock-lua) |
| Coroutines: `lua_newthread`, `lua_closethread`, `lua_xmove`, `lua_tothread`, `lua_pushthread`, `lua_resume`, `lua_yieldk` / `lua_yield`, `lua_status`, `lua_isyieldable` | ✅ — see [Coroutines](#coroutines) |
| Registry and references: `luaL_ref`, `luaL_unref`, `luaL_getsubtable`, `luaL_requiref` | ✅ |
| Auxiliary library: `luaL_check*/opt*`, `luaL_argerror`, `luaL_typeerror`, `luaL_error`, `luaL_where`, `luaL_tolstring`, `luaL_newmetatable`, `luaL_testudata`, `luaL_checkudata`, `luaL_setfuncs`, `luaL_setmetatable`, `luaL_callmeta`, `luaL_getmetafield`, `luaL_traceback` (header only), `luaL_fileresult`, `luaL_execresult` | ✅ |
| Strings and buffers: `luaL_buffinit`, `luaL_prepbuffsize`, `luaL_buffinitsize`, `luaL_addstring`, `luaL_addlstring`, `luaL_addvalue`, `luaL_addgsub`, `luaL_pushresult`, `luaL_pushresultsize`, `luaL_gsub`, `lua_numbertocstring`, `lua_stringtonumber`, `lua_pushvfstring` | ✅ |
| GC: `lua_gc` with `LUA_GCSTOP`, `LUA_GCRESTART`, `LUA_GCCOLLECT`, `LUA_GCCOUNT{,B}`, `LUA_GCSTEP`, `LUA_GCISRUNNING`, `LUA_GCGEN`, `LUA_GCINC`, `LUA_GCPARAM` | ✅ |
| Warnings: `lua_setwarnf`, `lua_warning` | ✅ (only reachable from C code — see [Differences](#what-differs-from-stock-lua)) |
| Standard library openers: `luaopen_base`, `luaopen_package`, `luaopen_coroutine`, `luaopen_debug`, `luaopen_io`, `luaopen_math`, `luaopen_os`, `luaopen_string`, `luaopen_table`, `luaopen_utf8` | ✅ — they push the table clx already loaded |
| Upvalues: `lua_upvalueindex`, `lua_getupvalue`, `lua_setupvalue`, `lua_upvalueid`, `lua_upvaluejoin` | ✅ |

## Not implemented

These functions exist but do nothing useful. A module that relies on any of
them will misbehave silently rather than fail to link.

### No state creation

| Function | Behaviour |
|---|---|
| `lua_newstate` | returns `NULL` |
| `luaL_newstate` | returns `NULL` |
| `lua_close` | no-op — the runtime stays alive |
| `lua_setallocf` | no-op |
| `lua_getallocf` | always returns `luaL_alloc`, writes `NULL` to `ud` |

### Loading and dumping bytecode

clx compiles Lua to native code ahead of time, so there is no runtime compiler
and no bytecode.

| Function | Behaviour |
|---|---|
| `lua_load` | pushes `"clx: lua_load is not supported (chunk '...')"` and returns `LUA_ERRSYNTAX` |
| `luaL_loadbufferx` | pushes `"clx: cannot load '<name>' at runtime"` and returns `LUA_ERRSYNTAX` |
| `luaL_loadstring` | same as `luaL_loadbufferx` |
| `luaL_loadfilex` | returns `LUA_ERRFILE` if the file cannot be opened, otherwise `LUA_ERRSYNTAX` as above — the file is never compiled |
| `lua_dump` | always returns `1` (failure); `writer` is never called |

If you need to run Lua source at runtime, build with `--dynamic` and use
`load()` from Lua — see [Dynamic Lua](./dynamic-lua.md).

### Debug and hooks

| Function | Behaviour |
|---|---|
| `lua_getinfo` | always returns `0` |
| `lua_getlocal` | always returns `NULL` |
| `lua_setlocal` | always returns `NULL` |
| `lua_sethook` | no-op |
| `lua_gethook` | returns `NULL` |
| `lua_gethookmask` | returns `0` |
| `lua_gethookcount` | returns `0` |
| `lua_getstack` | returns `1` **only** for `level == 0` on a running thread, and zeroes `lua_Debug`; every other level returns `0`. Since `lua_getinfo` then returns `0`, there is no usable frame inspection |
| `luaL_traceback` | pushes only `msg` (if given), a newline and the literal `stack traceback:` — no frames |

This matches the compiled-code side of clx: the `debug` library is not exposed
as a compiled-code global either (see
[Lua 5.5 compatibility](./compatibility.md)).

### To-be-closed variables

| Function | Behaviour |
|---|---|
| `lua_toclose` | no-op — the slot is never closed |
| `lua_closeslot` | no-op |

### Other stubs

| Function | Behaviour |
|---|---|
| `lua_checkstack` | always returns `1`; there is no fixed stack limit, so runaway recursion can exhaust memory instead of raising `"stack overflow"` |
| `luaL_openselectedlibs` | no-op, which makes the `luaL_openlibs(L)` macro a no-op. Standard libraries are already open (unless you build with `--minimal`) |
| `luaL_register` | not present — removed from Lua in 5.2 |

## Coroutines

The coroutine API works: `lua_newthread`, `lua_resume`, `lua_yieldk`,
`lua_status`, `lua_xmove` and friends are covered by clx's test suite. A
minimal round trip:

```c
static int l_work(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    lua_pushinteger(L, n + 1);
    lua_yield(L, 1);            /* hand 42 to whoever resumed us */
    lua_pushinteger(L, 100);    /* runs on the second resume */
    return 1;
}

static int l_run(lua_State *L)
{
    lua_State *co = lua_newthread(L);        /* L: [co] */

    lua_pushcfunction(co, l_work);           /* co: [l_work] */
    lua_pushinteger(co, 41);                 /* co: [l_work, 41] */

    int nres = 0;
    int st = lua_resume(co, L, 1, &nres);    /* LUA_YIELD, nres == 1 (42) */
    if (st == LUA_YIELD)
        st = lua_resume(co, L, 0, &nres);    /* LUA_OK,    nres == 1 (100) */

    lua_xmove(co, L, nres);                  /* L: [co, 100] */
    lua_remove(L, -2);                       /* L: [100] — drop the thread */
    lua_pushinteger(L, st);
    lua_pushinteger(L, nres);
    return 3;
}

int luaopen_coro(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"run", l_run},
        {NULL, NULL}
    };
    luaL_newlib(L, funcs);
    return 1;
}
```

```lua
local coro = require("coro")
local v, st, nres = coro.run()
assert(st == 0 and nres == 1 and v == 100)
```

As in stock Lua, `lua_yield(L, n)` hands its top `n` values to the resumer and
the values passed to the next `lua_resume` become the values `lua_yield`
"returns" (their count is what `lua_yield` evaluates to).

Notes on the contract:

- **`lua_resume(L, from, narg, nres)` expects the arguments to already be on
  `L`'s stack.** Stock usage moves them there first with
  `lua_xmove(from, L, narg)`; `from` is only used to check that both states
  belong to the same runtime.
- Results and error objects are pushed onto `L`'s stack; `*nres` is set to
  their count. On `LUA_ERRRUN` exactly one value is pushed.
- `lua_status` returns `LUA_OK` for the main thread and for a freshly created
  thread, `LUA_YIELD` after a yield, `LUA_OK` for a coroutine that finished
  normally, and `LUA_ERRRUN` for one that died with an error.
- `lua_isyieldable` returns `0` on the main thread and inside `lua_pcallk`,
  matching stock's "cannot yield across a C-call boundary" rule.
- `lua_yieldk(L, n, ctx, k)` yields like `lua_yield`, then runs
  `k(L, LUA_YIELD, ctx)` after the coroutine is next resumed and returns
  whatever `k` returns. If `k` is `NULL`, it returns the number of values the
  resumer passed.
- `lua_newthread` shares the single runtime — same globals, same registry, same
  GC. `lua_xmove` between threads of *different* states is impossible because
  there is only one state; it raises
  `"moving among independent states is not supported"`.
- `lua_closethread(L, from)` on a still-suspended coroutine runs clx's
  `close_thread`, which releases the fiber rather than silently dropping it;
  the thread ends up `DEAD`.
- Threads created by clx's own `coroutine.create` are resumable from C too:
  `lua_tothread` on a Lua thread value gives you a `lua_State*` whose function
  is already set.

### Known deviations

| Behaviour | Stock Lua | clx |
|---|---|---|
| `lua_yieldk` with a continuation | never returns to the caller of `lua_yieldk` | returns `k(L, LUA_YIELD, ctx)`'s value; with `k == NULL` it returns the number of resumed values, so it "returns" like `lua_yield` does in Lua code |
| Resuming the main thread | runs the main function if not yet started | `"cannot resume non-suspended coroutine"` |
| `lua_resume` on a never-started thread with neither a stack function nor `t->function` | error | `"cannot resume dead coroutine"` |

## Errors

Errors are C++ exceptions (`clx::LRuntimeException`) unwinding through your
C frames, not `longjmp`:

- `lua_error(L)` and `luaL_error(L, fmt, ...)` never return — they throw. Both
  `return luaL_error(...)` and bare `luaL_error(...)` work.
- `lua_pcallk` / `lua_pcall` catch the exception, run your error handler if you
  passed one, and return `LUA_ERRRUN` (or `LUA_ERRERR` if the handler itself
  fails). Use them exactly as you would in stock Lua.
- An error escaping an unprotected `lua_call` propagates out to the nearest
  `pcall` in your Lua code, or to the program's top level if there is none.
- **`lua_atpanic` is stored but never called.** clx has no panic path: an
  unrecoverable error prints the message and exits with status 1. Do not write
  a panic function that is expected to `longjmp` or `exit` for you.
- `luaL_where(L, 1)` prefixes the message with `file:line: ` taken from the
  most recent *Lua* call site, when clx knows it; otherwise it pushes `""`.
  Inside a `luaopen_*` at startup there is usually no position yet, so
  `luaL_error` messages come out unprefixed.
- Error *objects* behave as in stock: any Lua value can be raised, and
  `lua_tolstring` on a raised value gives you the message. `lua_pushvfstring`
  supports the same conversions as stock — `%s %c %d %I %f %p %U %%` — and
  copies unknown specifiers through unchanged.

## What differs from stock Lua

### You must rebuild against clx's headers

`lua.h` renames every `lua_*` and `luaL_*` symbol to `clx_lua_*` through
`#define`s placed above the declarations. That keeps the bridge from colliding
with a real Lua library that some other dependency might link.

The consequence: an object file compiled against stock Lua's headers **cannot**
be linked into a clx program. Rebuild every C module from source with
`-I/path/to/clx/include`. Your own `luaopen_<name>` symbols are *not* renamed,
which is how `--modules` finds them.

`luaopen_base`, `luaopen_math` and friends **are** renamed (`clx_luaopen_math`)
because clx defines them itself; include `lualib.h` and call them normally.

### The stack is per-bundle and per-frame

Each `lua_State*` owns its own value stack. Inside a `lua_CFunction` you see
only your own arguments, as in stock Lua. Strings returned by
`lua_pushlstring`/`lua_pushstring`/`lua_tolstring` point into clx's interned
string arena and stay valid as long as the value is reachable — and
`lua_tolstring` converts numbers in place, exactly like stock.

Short strings (six bytes or fewer) are stored inline in the value. `lua_tolstring`
re-interns them before returning a pointer, so the usual "the pointer is valid
while the value stays on the stack" guarantee holds.

### Notes and differences

| Area | Note |
|---|---|
| `luaL_checkoption` | returns the **`int` index** of the chosen option, not a pointer — this is a Lua 5.5 change, not a clx quirk |
| `lua_callk` / `lua_pcallk` | the `lua_KFunction` continuation argument is accepted and **never called**. clx coroutines are stackful, so a C frame suspended by a yield resumes where it left off and the call simply completes instead of running `k(L, LUA_YIELD, ctx)`. `lua_yieldk`'s own continuation *does* work and is the one to use |
| yield through `lua_callk` | stock allows it only when `k != NULL`; clx always allows it |
| yield through `lua_pcallk` | stock allows it when `k != NULL`; clx never allows it — you get `"attempt to yield across a C-call boundary"` |
| `luaL_pushfail` | a header macro, so identical to stock: pushes `nil`, or `false` if you define `LUA_FAILISFALSE` |
| `lua_newuserdatauv(L, sz, nuvalue)` | `nuvalue` is ignored — you can store any number of user values, and out-of-range `lua_getiuservalue`/`lua_setiuservalue` indices return `LUA_TNONE` / `0` instead of failing |
| `lua_pushexternalstring(L, s, len, falloc, ud)` | clx copies the bytes into its own arena and calls `falloc(ud, s, len+1, 0)` **immediately**, so your buffer must be freeable right away and must not be reused |
| `luaL_testudata(L, idx, "FILE*")` | returns a pointer to a **single shared scratch slot** in the state, overwritten by the next such call. Copy the `FILE*` out before calling it again |
| `luaL_openlibs` | no-op; libraries are opened by the runtime. With `--minimal` they are absent and the C API cannot bring them back |
| Warnings | `lua_setwarnf`/`lua_warning` work for C-to-C calls, but clx's own runtime warnings do not route through them |
| Module initialisation | openers run at program startup, before the entry chunk, and all of them run regardless of whether anything calls `require` |
| `lua_getextraspace` | backed by real reserved bytes in front of every `lua_State`, so the macro behaves as in stock |

## Checking your module

A quick smoke test is enough to catch most wiring problems:

```bash
# 1. build the module archive
cc -c -O2 -I/path/to/clx/include lfs.c -o lfs.o && ar rcs lfs.a lfs.o

# 2. link and run
clx check.lua --modules lfs -o check && ./check
```

```lua
-- check.lua
local m = require("lfs")
assert(type(m) == "table", "opener did not return a module table")
print("ok")
```

If clx reports `input module "lfs" collides with the precompiled C module`,
your `.lua` file shares its stem with the archive — rename one of them.
If the archive is treated as a C++ module, check that `luaopen_lfs` was not
mangled (`nm -g lfs.a | grep luaopen`).

## See also

- [Lua modules](./modules.md) — building, finding and linking module archives,
  and the `libclx_capi` wrapper archive
- [C++ API](./api.md) — the native, stack-free API for new code
- [Migration guide](./migration-guide.md) — porting a C module to the C++ API
- [Lua 5.5 compatibility](./compatibility.md) — language and library support
- [Dynamic Lua](./dynamic-lua.md) — running Lua source at runtime
