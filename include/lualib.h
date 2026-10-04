// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  lualib.h · Lua 5.5 stdlib openers for clx  │
// └─────────────────────────────────────────────┘

#ifndef lualib_h
#define lualib_h

#include "lauxlib.h"
#include "lua.h"

#define LUA_VERSUFFIX          "_" LUA_VERSION_MAJOR "_" LUA_VERSION_MINOR

//------------------ Symbol prefix: standard-library openers map onto clx globals
#define luaopen_base clx_luaopen_base
#define luaopen_package clx_luaopen_package
#define luaopen_coroutine clx_luaopen_coroutine
#define luaopen_debug clx_luaopen_debug
#define luaopen_io clx_luaopen_io
#define luaopen_math clx_luaopen_math
#define luaopen_os clx_luaopen_os
#define luaopen_string clx_luaopen_string
#define luaopen_table clx_luaopen_table
#define luaopen_utf8 clx_luaopen_utf8

#define LUA_GLIBK		1
LUAMOD_API int (luaopen_base) (lua_State *L);

#define LUA_LOADLIBNAME	"package"
#define LUA_LOADLIBK	(LUA_GLIBK << 1)
LUAMOD_API int (luaopen_package) (lua_State *L);

#define LUA_COLIBNAME	"coroutine"
#define LUA_COLIBK	(LUA_LOADLIBK << 1)
LUAMOD_API int (luaopen_coroutine) (lua_State *L);

#define LUA_DBLIBNAME	"debug"
#define LUA_DBLIBK	(LUA_COLIBK << 1)
LUAMOD_API int (luaopen_debug) (lua_State *L);

#define LUA_IOLIBNAME	"io"
#define LUA_IOLIBK	(LUA_DBLIBK << 1)
LUAMOD_API int (luaopen_io) (lua_State *L);

#define LUA_MATHLIBNAME	"math"
#define LUA_MATHLIBK	(LUA_IOLIBK << 1)
LUAMOD_API int (luaopen_math) (lua_State *L);

#define LUA_OSLIBNAME	"os"
#define LUA_OSLIBK	(LUA_MATHLIBK << 1)
LUAMOD_API int (luaopen_os) (lua_State *L);

#define LUA_STRLIBNAME	"string"
#define LUA_STRLIBK	(LUA_OSLIBK << 1)
LUAMOD_API int (luaopen_string) (lua_State *L);

#define LUA_TABLIBNAME	"table"
#define LUA_TABLIBK	(LUA_STRLIBK << 1)
LUAMOD_API int (luaopen_table) (lua_State *L);

#define LUA_UTF8LIBNAME	"utf8"
#define LUA_UTF8LIBK	(LUA_TABLIBK << 1)
LUAMOD_API int (luaopen_utf8) (lua_State *L);

#define luaL_openselectedlibs clx_luaL_openselectedlibs
LUALIB_API void (luaL_openselectedlibs) (lua_State *L, int load, int preload);

#define luaL_openlibs(L)	luaL_openselectedlibs(L, ~0, 0)

#endif
