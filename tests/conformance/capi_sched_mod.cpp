//--------- capi_sched_mod: C scheduler driving coroutines through the Lua C API

#include <lauxlib.h>
#include <lua.h>
#include <stdlib.h>
#include <string.h>

typedef struct sched_task {
    lua_State* co;
    int tref;
    int pldref;
    int done;
    int failed;
    struct sched_task* waiting;
} sched_task;

static sched_task* g_heap[256];
static int g_nheap = 0;
static int g_mapref = LUA_NOREF;
static int g_gc_count = 0;

//---------- payload_gc: frees the task payload exactly like the reported scheduler
static int payload_gc(lua_State* L) {
    sched_task** slot = (sched_task**)lua_touserdata(L, 1);
    if (slot && *slot) {
        free(*slot);
        *slot = NULL;
        g_gc_count++;
    }
    return 0;
}

//---------- ensure_map: registry weak-key table mapping thread -> task pointer
static void ensure_map(lua_State* L) {
    if (g_mapref != LUA_NOREF)
        return;
    lua_newtable(L);
    lua_newtable(L);
    lua_pushstring(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    g_mapref = luaL_ref(L, LUA_REGISTRYINDEX);
}

//---------- push_task_key: push the thread object of co as a key onto L
static void push_task_key(lua_State* L, lua_State* co) {
    if (co == L)
        lua_pushthread(L);
    else {
        lua_pushthread(co);
        lua_xmove(co, L, 1);
    }
}

//---------- map_set: weak registry map[thread] = lightuserdata task
static void map_set(lua_State* L, lua_State* co, sched_task* t) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_mapref);
    push_task_key(L, co);
    lua_pushlightuserdata(L, t);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

//---------- push_pending: append a task to the run queue
static void push_pending(sched_task* t) {
    g_heap[g_nheap++] = t;
}

//---------- pop_pending: remove one task from the run queue
static sched_task* pop_pending(void) {
    if (g_nheap == 0)
        return NULL;
    sched_task* t = g_heap[0];
    memmove(g_heap, g_heap + 1, sizeof(g_heap[0]) * (size_t)(g_nheap - 1));
    g_nheap--;
    return t;
}

//---------- finish_task: drop both references and retire the task
static void finish_task(lua_State* L, sched_task* t) {
    luaL_unref(L, LUA_REGISTRYINDEX, t->tref);
    luaL_unref(L, LUA_REGISTRYINDEX, t->pldref);
    t->tref = LUA_NOREF;
    t->pldref = LUA_NOREF;
}

//---------- spawn: spawn(fn) -> task lightuserdata
static int spawn(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    ensure_map(L);
    lua_State* co = lua_newthread(L);
    int tref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);

    sched_task* t = (sched_task*)calloc(1, sizeof(*t));
    t->co = co;
    t->tref = tref;

    sched_task** slot = (sched_task**)lua_newuserdata(L, sizeof(*slot));
    *slot = t;
    luaL_getmetatable(L, "sched.payload");
    lua_setmetatable(L, -2);
    t->pldref = luaL_ref(L, LUA_REGISTRYINDEX);

    map_set(L, co, t);

    push_pending(t);
    lua_pushlightuserdata(L, t);
    return 1;
}

//---------- step: step([arg]) -> {status=, code=, task=, [1]=...}; resumes the head of the queue
static int step(lua_State* L) {
    sched_task* t = pop_pending();
    if (!t) {
        lua_pushnil(L);
        return 1;
    }
    lua_State* co = t->co;
    int narg = 0;
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        lua_pushvalue(L, 1);
        lua_xmove(L, co, 1);
        narg = 1;
    }

    int nres = 0;
    int st = lua_resume(co, L, narg, &nres);
    if (nres < 0)
        nres = 0;

    lua_newtable(L);
    lua_pushstring(L, st == LUA_OK ? "ok" : (st == LUA_YIELD ? "yield" : "error"));
    lua_setfield(L, -2, "status");
    lua_pushinteger(L, st);
    lua_setfield(L, -2, "code");
    lua_pushlightuserdata(L, t);
    lua_setfield(L, -2, "task");
    for (int i = 0; i < nres; ++i) {
        lua_pushvalue(co, i + 1);
        lua_xmove(co, L, 1);
        lua_rawseti(L, -2, i + 1);
    }
    if (st == LUA_OK || st == LUA_ERRRUN) {
        finish_task(L, t);
        t->done = 1;
        if (st == LUA_ERRRUN)
            t->failed = 1;
    } else {
        push_pending(t);
    }
    return 1;
}

//---------- complete: complete(task) -> remove the task from the queue and unref it
static int complete(lua_State* L) {
    sched_task* t = (sched_task*)lua_touserdata(L, 1);
    if (!t)
        return luaL_error(L, "bad task");
    for (int i = 0; i < g_nheap; ++i) {
        if (g_heap[i] == t) {
            memmove(g_heap + i, g_heap + i + 1, sizeof(g_heap[0]) * (size_t)(g_nheap - i - 1));
            g_nheap--;
            break;
        }
    }
    if (!t->done)
        finish_task(L, t);
    t->done = 1;
    return 0;
}

//---------- task_lookup: task_lookup(co) -> lightuserdata or nil
static int task_lookup(lua_State* L) {
    lua_State* co = lua_tothread(L, 1);
    if (!co) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_mapref);
    push_task_key(L, co);
    lua_rawget(L, -2);
    lua_remove(L, -2);
    return 1;
}

//---------- status_of: status_of(task) -> lua_status of the task's thread
static int status_of(lua_State* L) {
    sched_task* t = (sched_task*)lua_touserdata(L, 1);
    lua_pushinteger(L, t ? lua_status(t->co) : -1);
    return 1;
}

//---------- pending_count: pending_count() -> run-queue length
static int pending_count(lua_State* L) {
    lua_pushinteger(L, g_nheap);
    return 1;
}

//---------- gc_count: gc_count() -> payload finalizers run so far
static int gc_count(lua_State* L) {
    lua_pushinteger(L, g_gc_count);
    return 1;
}

//---------- luaopen_capi_sched_mod: module table
LUAMOD_API int luaopen_capi_sched_mod(lua_State* L) {
    luaL_newmetatable(L, "sched.payload");
    lua_pushcfunction(L, payload_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, spawn);
    lua_setfield(L, -2, "spawn");
    lua_pushcfunction(L, step);
    lua_setfield(L, -2, "step");
    lua_pushcfunction(L, complete);
    lua_setfield(L, -2, "complete");
    lua_pushcfunction(L, task_lookup);
    lua_setfield(L, -2, "task_lookup");
    lua_pushcfunction(L, status_of);
    lua_setfield(L, -2, "status_of");
    lua_pushcfunction(L, pending_count);
    lua_setfield(L, -2, "pending_count");
    lua_pushcfunction(L, gc_count);
    lua_setfield(L, -2, "gc_count");
    return 1;
}
