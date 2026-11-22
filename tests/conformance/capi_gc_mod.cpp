//--------- capi_gc_mod: userdata finalizer observation module

#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <string.h>

#define GC_NAME_MAX 32
#define GC_LOG_MAX 1024

static int g_gc_runs = 0;
static char g_gc_log[GC_LOG_MAX];
static size_t g_gc_log_len = 0;
static char g_gc_marker[512];

//---------- gc_record: append name to the run log and bump the run counter
static void gc_record(const char* name) {
    size_t len = strlen(name);
    g_gc_runs++;
    if (g_gc_log_len + len + 2 >= GC_LOG_MAX)
        return;
    if (g_gc_log_len > 0)
        g_gc_log[g_gc_log_len++] = ',';
    memcpy(g_gc_log + g_gc_log_len, name, len);
    g_gc_log_len += len;
    g_gc_log[g_gc_log_len] = '\0';
}

//---------- gc_name: read the name stored in a full userdata's payload
static const char* gc_name(lua_State* L) {
    const char* name = (const char*)lua_touserdata(L, 1);
    return name ? name : "";
}

//---------- ud_gc: __gc metamethod, records the name and optionally writes the marker file
static int ud_gc(lua_State* L) {
    const char* name = gc_name(L);
    gc_record(name);
    if (g_gc_marker[0]) {
        FILE* f = fopen(g_gc_marker, "w");
        if (f) {
            fputs(name, f);
            fclose(f);
        }
    }
    return 0;
}

//---------- ud_gc_alt: second finalizer, records the name with a ! suffix
static int ud_gc_alt(lua_State* L) {
    const char* name = gc_name(L);
    char tagged[GC_NAME_MAX + 1];
    snprintf(tagged, sizeof(tagged), "%s!", name);
    gc_record(tagged);
    return 0;
}

//---------- push_ud: push a userdata carrying name with a fresh metatable holding __gc = fn
static void push_ud(lua_State* L, const char* name, lua_CFunction fn) {
    char* slot = (char*)lua_newuserdatauv(L, GC_NAME_MAX, 0);
    strncpy(slot, name, GC_NAME_MAX - 1);
    slot[GC_NAME_MAX - 1] = '\0';
    lua_newtable(L);
    if (fn) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
}

//---------- l_new: new(name) -> userdata with a __gc metamethod already in place
static int l_new(lua_State* L) {
    push_ud(L, luaL_checkstring(L, 1), ud_gc);
    return 1;
}

//---------- l_new_open: new_open(name) -> userdata whose metatable has no __gc yet
static int l_new_open(lua_State* L) {
    push_ud(L, luaL_checkstring(L, 1), NULL);
    return 1;
}

//---------- set_ud_gc: install fn as the __gc of the userdata's current metatable, NULL clears it
static int set_ud_gc(lua_State* L, lua_CFunction fn) {
    luaL_checktype(L, 1, LUA_TUSERDATA);
    if (!lua_getmetatable(L, 1))
        return 0;
    if (fn)
        lua_pushcfunction(L, fn);
    else
        lua_pushnil(L);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    return 0;
}

//---------- l_arm: arm(ud) — add __gc to the metatable after it was installed
static int l_arm(lua_State* L) {
    return set_ud_gc(L, ud_gc);
}

//---------- l_arm_alt: arm_alt(ud) — replace __gc with the alternative finalizer
static int l_arm_alt(lua_State* L) {
    return set_ud_gc(L, ud_gc_alt);
}

//---------- l_disarm: disarm(ud) — clear __gc on the metatable
static int l_disarm(lua_State* L) {
    return set_ud_gc(L, NULL);
}

//---------- l_arm_value: arm_value(ud, v) — install v, callable or not, as the __gc of the current metatable
static int l_arm_value(lua_State* L) {
    luaL_checktype(L, 1, LUA_TUSERDATA);
    if (!lua_getmetatable(L, 1))
        return 0;
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    return 0;
}

//---------- l_marker: marker(path) — write the finalized name to path
static int l_marker(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    strncpy(g_gc_marker, path, sizeof(g_gc_marker) - 1);
    g_gc_marker[sizeof(g_gc_marker) - 1] = '\0';
    return 0;
}

//---------- l_count: count() -> number of finalizer runs since the last reset
static int l_count(lua_State* L) {
    lua_pushinteger(L, g_gc_runs);
    return 1;
}

//---------- l_log: log() -> comma separated names of the finalized userdata
static int l_log(lua_State* L) {
    lua_pushlstring(L, g_gc_log, g_gc_log_len);
    return 1;
}

//---------- l_reset: reset() — clear the counter, the log and the marker path
static int l_reset(lua_State* L) {
    g_gc_runs = 0;
    g_gc_log_len = 0;
    g_gc_log[0] = '\0';
    g_gc_marker[0] = '\0';
    return 0;
}

//---------- luaopen_capi_gc_mod: module table
LUAMOD_API int luaopen_capi_gc_mod(lua_State* L) {
    static const luaL_Reg funcs[] = {
        { "new", l_new },
        { "new_open", l_new_open },
        { "arm", l_arm },
        { "arm_alt", l_arm_alt },
        { "disarm", l_disarm },
        { "arm_value", l_arm_value },
        { "marker", l_marker },
        { "count", l_count },
        { "log", l_log },
        { "reset", l_reset },
        { NULL, NULL }
    };
    luaL_newlib(L, funcs);
    return 1;
}
