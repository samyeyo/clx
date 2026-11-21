//--------- capi_lightud_mod: exercise the light userdata C API surface

#include <lauxlib.h>
#include <lua.h>
#include <stdint.h>

//---------- l_mkptr: mkptr(n) -> lightuserdata
static int l_mkptr(lua_State* L) {
    lua_pushlightuserdata(L, (void*)(uintptr_t)luaL_checkinteger(L, 1));
    return 1;
}

//---------- l_ptrval: ptrval(v) -> integer address
static int l_ptrval(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)(uintptr_t)lua_touserdata(L, 1));
    return 1;
}

//---------- l_type: type(v) -> lua_type
static int l_type(lua_State* L) {
    lua_pushinteger(L, lua_type(L, 1));
    return 1;
}

//---------- l_isuserdata: isuserdata(v) -> boolean
static int l_isuserdata(lua_State* L) {
    lua_pushboolean(L, lua_isuserdata(L, 1));
    return 1;
}

//---------- l_topointer: toptr(v) -> integer
static int l_topointer(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)(uintptr_t)lua_topointer(L, 1));
    return 1;
}

//---------- l_tostr: tostr(v) -> string
static int l_tostr(lua_State* L) {
    luaL_tolstring(L, 1, NULL);
    return 1;
}

//---------- l_hasmeta: hasmeta(v) -> boolean
static int l_hasmeta(lua_State* L) {
    lua_pushboolean(L, lua_getmetatable(L, 1));
    return 1;
}

//---------- l_setmeta: setmeta(v, mt) -> v
static int l_setmeta(lua_State* L) {
    lua_setmetatable(L, 1);
    lua_settop(L, 1);
    return 1;
}

//---------- l_rawpset: rawpset(t, n, v) — store v at the pointer key n
static int l_rawpset(lua_State* L) {
    lua_pushvalue(L, 3);
    lua_rawsetp(L, 1, (const void*)(uintptr_t)luaL_checkinteger(L, 2));
    return 0;
}

//---------- l_rawpget: rawpget(t, n) — read the value stored at the pointer key n
static int l_rawpget(lua_State* L) {
    lua_rawgetp(L, 1, (const void*)(uintptr_t)luaL_checkinteger(L, 2));
    return 1;
}

//---------- l_lightkeyget: lightkeyget(t, n) — read through a pushed light userdata key
static int l_lightkeyget(lua_State* L) {
    lua_pushlightuserdata(L, (void*)(uintptr_t)luaL_checkinteger(L, 2));
    lua_rawget(L, 1);
    return 1;
}

//---------- l_pushtype: pushtype(tname) — push the C type code of a named constant
static int l_codes(lua_State* L) {
    lua_newtable(L);
    lua_pushinteger(L, LUA_TLIGHTUSERDATA);
    lua_setfield(L, -2, "lightuserdata");
    lua_pushinteger(L, LUA_TUSERDATA);
    lua_setfield(L, -2, "userdata");
    lua_pushinteger(L, LUA_TTABLE);
    lua_setfield(L, -2, "table");
    lua_pushinteger(L, LUA_TSTRING);
    lua_setfield(L, -2, "string");
    return 1;
}

//---------- luaopen_capi_lightud_mod: module table
LUAMOD_API int luaopen_capi_lightud_mod(lua_State* L) {
    static const luaL_Reg funcs[] = {
        { "mkptr", l_mkptr },
        { "ptrval", l_ptrval },
        { "type", l_type },
        { "isuserdata", l_isuserdata },
        { "toptr", l_topointer },
        { "tostr", l_tostr },
        { "hasmeta", l_hasmeta },
        { "setmeta", l_setmeta },
        { "rawpset", l_rawpset },
        { "rawpget", l_rawpget },
        { "lightkeyget", l_lightkeyget },
        { "codes", l_codes },
        { NULL, NULL }
    };
    luaL_newlib(L, funcs);
    return 1;
}
