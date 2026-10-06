//--------- capi_mod: Lua C API test module probing lua_pcallk / lua_callk continuations

#include <lauxlib.h>
#include <lua.h>

//---------- g_yieldable: lua_isyieldable observed inside a pcallk callee (-1 = callee never ran)
static int g_yieldable = -1;

//---------- g_k_calls: invocation count of my_k (run_pcall's continuation)
static int g_k_calls = 0;

//---------- g_finish_calls: invocation count of finish
static int g_finish_calls = 0;

//---------- g_finish_status: status passed to the most recent finish invocation
static int g_finish_status = -1;

//---------- g_callk_calls: invocation count of the lua_callk continuation
static int g_callk_calls = 0;

//---------- g_callk_status: status passed to the most recent lua_callk continuation
static int g_callk_status = -1;

//---------- finish: stock lbaselib continuation; results on success, false + message on failure
static int finish(lua_State* L, int status, lua_KContext extra) {
    ++g_finish_calls;
    g_finish_status = status;
    if (status != LUA_OK && status != LUA_YIELD) {
        lua_pushboolean(L, 0);
        lua_pushvalue(L, -2);
        return 2;
    }
    return lua_gettop(L) - (int)extra;
}

//---------- mypall: stock pcall idiom; after a yield its continuation finish replaces it entirely
static int mypall(lua_State* L) {
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1);
    lua_insert(L, 1);
    int st = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finish);
    return finish(L, st, 0);
}

//---------- my_k: continuation handed to run_pcall's lua_pcallk
static int my_k(lua_State* L, int status, lua_KContext ctx) {
    (void)L;
    (void)status;
    (void)ctx;
    ++g_k_calls;
    return 0;
}

//---------- yielder: run_pcall callee; records lua_isyieldable, then yields 42
static int yielder(lua_State* L) {
    g_yieldable = lua_isyieldable(L);
    lua_pushinteger(L, 42);
    return lua_yield(L, 1);
}

//---------- run_pcall: run_pcall(use_k) -> status, isyieldable-in-callee, message, k_calls
static int run_pcall(lua_State* L) {
    int use_k = lua_toboolean(L, 1);
    g_yieldable = -1;
    g_k_calls = 0;
    lua_pushcfunction(L, yielder);
    int st = lua_pcallk(L, 0, 0, 0, 0, use_k ? my_k : NULL);
    const char* msg = (st == LUA_OK) ? "" : lua_tostring(L, -1);
    lua_pushinteger(L, st);
    lua_pushinteger(L, g_yieldable);
    lua_pushstring(L, msg ? msg : "");
    lua_pushinteger(L, g_k_calls);
    return 4;
}

//---------- c_yielder: C callee for mypall; yields 42 once, returns the values passed to the resume
static int c_yielder(lua_State* L) {
    lua_pushinteger(L, 42);
    lua_yield(L, 1);
    return lua_gettop(L);
}

//---------- callk_cont: continuation handed to lua_callk; counts calls and records the status
static int callk_cont(lua_State* L, int status, lua_KContext ctx) {
    (void)L;
    (void)ctx;
    ++g_callk_calls;
    g_callk_status = status;
    return 0;
}

//---------- run_callk: run_callk(callee, ...) -> results of lua_callk with a continuation
static int run_callk(lua_State* L) {
    lua_callk(L, lua_gettop(L) - 1, LUA_MULTRET, 0, callk_cont);
    return lua_gettop(L);
}

//---------- isyieldable: isyieldable() -> lua_isyieldable on the running thread
static int isyieldable(lua_State* L) {
    lua_pushinteger(L, lua_isyieldable(L));
    return 1;
}

//---------- reset_counts: clears every continuation counter
static int reset_counts(lua_State* L) {
    (void)L;
    g_yieldable = -1;
    g_k_calls = 0;
    g_finish_calls = 0;
    g_finish_status = -1;
    g_callk_calls = 0;
    g_callk_status = -1;
    return 0;
}

//---------- counts: -> finish_calls, finish_status, callk_calls, callk_status, k_calls, yieldable
static int counts(lua_State* L) {
    lua_pushinteger(L, g_finish_calls);
    lua_pushinteger(L, g_finish_status);
    lua_pushinteger(L, g_callk_calls);
    lua_pushinteger(L, g_callk_status);
    lua_pushinteger(L, g_k_calls);
    lua_pushinteger(L, g_yieldable);
    return 6;
}

//---------- thread_isyieldable: thread_isyieldable(co) -> lua_isyieldable on that thread (pcall_depth == 0 test)
static int thread_isyieldable(lua_State* L) {
    lua_State* co = lua_tothread(L, 1);
    lua_pushinteger(L, co ? lua_isyieldable(co) : -1);
    return 1;
}

//---------- luaopen_capi_mod: module table with the pcallk / callk continuation probes
LUAMOD_API int luaopen_capi_mod(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, run_pcall);
    lua_setfield(L, -2, "run_pcall");
    lua_pushcfunction(L, thread_isyieldable);
    lua_setfield(L, -2, "thread_isyieldable");
    lua_pushcfunction(L, mypall);
    lua_setfield(L, -2, "mypall");
    lua_pushcfunction(L, c_yielder);
    lua_setfield(L, -2, "c_yielder");
    lua_pushcfunction(L, run_callk);
    lua_setfield(L, -2, "run_callk");
    lua_pushcfunction(L, isyieldable);
    lua_setfield(L, -2, "isyieldable");
    lua_pushcfunction(L, reset_counts);
    lua_setfield(L, -2, "reset_counts");
    lua_pushcfunction(L, counts);
    lua_setfield(L, -2, "counts");
    return 1;
}
