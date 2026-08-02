// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  vm_convert.cpp · Conversion Machinery      │
// └─────────────────────────────────────────────┘

#include "vm_convert.h"
#include "dynamic_vm.h"
#include "vm_function_object.h"
#include "vm_table_proxy.h"
#include "vm_native_bridge.h"

#include <clx.h>

#include <cstring>
#include <cstdio>

extern "C" int clx_vm_userdata_index(lua_State *L);
extern "C" int clx_vm_proxy_index(lua_State *L);
extern "C" int clx_vm_proxy_newindex(lua_State *L);
extern "C" int clx_vm_proxy_pairs(lua_State *L);
extern "C" int clx_vm_proxy_len(lua_State *L);
extern "C" int clx_vm_proxy_gc(lua_State *L);
extern "C" int clx_vm_proxy_close(lua_State *L);
extern "C" int clx_vm_proxy_call(lua_State *L);
extern "C" int clx_vm_env_index(lua_State *L);
extern "C" int clx_vm_env_newindex(lua_State *L);
extern "C" int clx_vm_env_pairs(lua_State *L);
extern "C" int clx_vm_env_pairs_iter(lua_State *L);

//------------------ Thread-local conversion cache - circular reference detection during Lua table-to-clx LTable conversion
namespace {
struct ConvertCache {
    std::unordered_map<const void *, clx::LValue> map;
    int depth = 0;
};

thread_local ConvertCache s_convert_cache;
}

namespace clx {

//------------------ clx_to_vm_value_
void clx_to_vm_value_(LState *clx_L, lua_State *L, const LValue &v) {
    switch (v.type) {
    case Nil:
        lua_pushnil(L);
        return;
    case Boolean:
        lua_pushboolean(L, v.as_bool() ? 1 : 0);
        return;
    case Int64: {
        int64_t i = v.val.payload.i64;

        if (i >= INT32_MIN && i <= INT32_MAX)
            lua_pushinteger(L, static_cast<lua_Integer>(i));
        else
            lua_pushnumber(L, static_cast<lua_Number>(static_cast<double>(i)));
        return;
    }
    case Double:
        if (v.val.payload.f64 != v.val.payload.f64) {

            lua_pushnumber(L, 0.0 / 0.0);
        } else {
            lua_pushnumber(L, v.val.payload.f64);
        }
        return;
    case String: {
        const char *s = v.as_string();
        uint32_t len = v.string_len();
        lua_pushlstring(L, s ? s : "", len);
        return;
    }
    case Table:
        if (v.as_pointer()) {

            LHeader *h = v.as_pointer();
            ProxyNode *node = static_cast<ProxyNode *>(lua_newuserdata(L, sizeof(ProxyNode)));
            node->header = h;
            node->prev = nullptr;
            node->next = nullptr;

            DynamicVM *vm = DynamicVM::acquire(clx_L);
            vm->link_proxy(node);

            if (luaL_newmetatable(L, "clx_proxy")) {

                lua_pushlightuserdata(L, static_cast<void *>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_index, 1);
                lua_setfield(L, -2, "__index");

                lua_pushlightuserdata(L, static_cast<void *>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_newindex, 1);
                lua_setfield(L, -2, "__newindex");

                lua_pushlightuserdata(L, static_cast<void *>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_pairs, 1);
                lua_setfield(L, -2, "__pairs");

                lua_pushlightuserdata(L, static_cast<void *>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_len, 1);
                lua_setfield(L, -2, "__len");

                lua_pushlightuserdata(L, static_cast<void *>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_call, 1);
                lua_setfield(L, -2, "__call");

                lua_pushlightuserdata(L, static_cast<void *>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_close, 1);
                lua_setfield(L, -2, "__close");

                lua_pushlightuserdata(L, static_cast<void *>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_gc, 1);
                lua_setfield(L, -2, "__gc");

                lua_setmetatable(L, -2);

            } else {

                lua_setmetatable(L, -2);
            }
        } else {
            lua_pushnil(L);
        }
        return;
    case Function:
        if (v.as_pointer()) {

            NativeBridge *nb = create_bridge(clx_L, L, v);
            lua_rawgeti(L, LUA_REGISTRYINDEX, nb->registry_ref);
        } else {
            lua_pushnil(L);
        }
        return;
    case UserData: {

        if (v.as_pointer()) {
            LHeader *h = v.as_pointer();
            ProxyNode *node = static_cast<ProxyNode *>(lua_newuserdata(L, sizeof(ProxyNode)));
            node->header = h;
            node->prev = nullptr;
            node->next = nullptr;
            DynamicVM *vm = DynamicVM::acquire(clx_L);
            vm->link_proxy(node);
            if (luaL_newmetatable(L, "clx_userdata")) {

                lua_pushlightuserdata(L, static_cast<void *>(clx_L));
                lua_pushcclosure(L, clx_vm_userdata_index, 1);
                lua_setfield(L, -2, "__index");

                lua_pushlightuserdata(L, static_cast<void *>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_gc, 1);
                lua_setfield(L, -2, "__gc");
                lua_setmetatable(L, -2);
            } else {
                lua_setmetatable(L, -2);
            }
        } else {
            lua_pushnil(L);
        }
        return;
    }
    case Thread:

        throw_runtime_error("cannot pass clx coroutine to Lua VM function: "
                            "use Lua VM's own coroutine library (copied to env table) instead");
    default:

        lua_pushnil(L);
        return;
    }
}

//------------------ vm_to_clx_value_
LValue vm_to_clx_value_(LState *clx_L, lua_State *L, int idx) {
    int t = lua_type(L, idx);
    switch (t) {
    case LUA_TNIL:
        return LValue();
    case LUA_TBOOLEAN:
        return LValue(static_cast<bool>(lua_toboolean(L, idx)));
    case LUA_TNUMBER: {

        if (lua_isinteger(L, idx)) {
            return LValue(static_cast<int64_t>(lua_tointeger(L, idx)));
        }
        return LValue(static_cast<double>(lua_tonumber(L, idx)));
    }
    case LUA_TSTRING: {
        size_t len = 0;
        const char *s = lua_tolstring(L, idx, &len);
        if (!s)
            return LValue();
        if (len <= 6) {
            return LValue::istr(s, static_cast<uint32_t>(len));
        }
        return LValue(clx_L->intern_string(s, len));
    }
    case LUA_TTABLE: {
        //------------------ Circular reference detection
        static constexpr int MAX_CONVERT_DEPTH = 128;
        if (s_convert_cache.depth >= MAX_CONVERT_DEPTH) {
            char buf[256];
            const void *tp = lua_topointer(L, idx);
            std::snprintf(buf, sizeof(buf), "vm_to_clx_value_ maximum conversion depth (%d) exceeded at table %p",
                MAX_CONVERT_DEPTH, tp);
            throw clx::LRuntimeException(clx::LValue(clx_L->intern_string(buf)));
        }

        if (s_convert_cache.depth == 0)
            s_convert_cache.map.clear();
        s_convert_cache.depth++;

        const void *tbl_key = lua_topointer(L, idx);
        if (tbl_key) {
            auto it = s_convert_cache.map.find(tbl_key);
            if (it != s_convert_cache.map.end()) {
                s_convert_cache.depth--;
                return it->second;
            }
        }

        LValue t = clx_L->create_table();
        LTable *tbl = static_cast<LTable *>(t.as_pointer());

        if (tbl_key)
            s_convert_cache.map[tbl_key] = t;

        size_t prev_top = clx_L->shadow_top;
        clx_L->shadow_stack[clx_L->shadow_top++] = TypedSlot(&t.val, &t.type);

        lua_pushvalue(L, idx);
        int abs_idx = lua_gettop(L);

        if (lua_getmetatable(L, abs_idx)) {
            LValue meta_copy = vm_to_clx_value_(clx_L, L, -1);
            lua_pop(L, 1);
            if (meta_copy.type == Table) {
                LTable *meta_tbl = static_cast<LTable *>(meta_copy.as_pointer());

                meta_tbl->settable(clx_L->str_gc, LValue());
                tbl->metatable = meta_tbl;
                tbl->hash_version++;
            }
        }

        size_t len = static_cast<size_t>(lua_rawlen(L, abs_idx));
        if (len > 0) {
            size_t cap = 8;
            while (cap < len)
                cap *= 2;
            tbl->array = new TValue[cap]();
            tbl->array_types = new ValueType[cap]();
            tbl->array_cap = cap;
            for (size_t i = 0; i < len; ++i) {
                lua_rawgeti(L, abs_idx, static_cast<int>(i + 1));
                LValue val = vm_to_clx_value_(clx_L, L, -1);
                lua_pop(L, 1);
                tbl->array[i] = val.val;
                tbl->array_types[i] = val.type;
            }
            tbl->array_size = len;
        }

        lua_pushnil(L);
        while (lua_next(L, abs_idx) != 0) {
            LValue key = vm_to_clx_value_(clx_L, L, -2);
            LValue val = vm_to_clx_value_(clx_L, L, -1);
            tbl->settable(key, val);
            lua_pop(L, 1);
        }

        lua_pop(L, 1);

        clx_L->shadow_top = prev_top;

        s_convert_cache.depth--;
        if (s_convert_cache.depth == 0)
            s_convert_cache.map.clear();

        return t;
    }
    case LUA_TFUNCTION: {

        LValue vf = vm_to_clx_function_(clx_L, L, idx);
        return vf;
    }
    case LUA_TUSERDATA: {

        if (lua_getmetatable(L, idx)) {

            luaL_getmetatable(L, "clx_proxy");

            if (lua_rawequal(L, -1, -2) == 1) {
                lua_pop(L, 2);
                void *ud = lua_touserdata(L, idx);
                if (ud) {
                    LHeader *h = *static_cast<LHeader **>(ud);
                    return LValue(h);
                }
                return LValue();
            }

            lua_pop(L, 1);

            luaL_getmetatable(L, "clx_thread");

            if (lua_rawequal(L, -1, -2) == 1) {
                lua_pop(L, 2);
                void *ud = lua_touserdata(L, idx);
                if (ud) {
                    LHeader *h = *static_cast<LHeader **>(ud);
                    return LValue(Thread, h);
                }
                return LValue();
            }
            lua_pop(L, 1);

            luaL_getmetatable(L, "clx_userdata");

            if (lua_rawequal(L, -1, -2) == 1) {
                lua_pop(L, 2);
                void *ud = lua_touserdata(L, idx);
                if (ud) {
                    LHeader *h = *static_cast<LHeader **>(ud);
                    return LValue(UserData, h);
                }
                return LValue();
            }
            lua_pop(L, 1);

            luaL_getmetatable(L, "clx_callable");

            if (lua_rawequal(L, -1, -2) == 1) {
                lua_pop(L, 2);
                LHeader *h = nullptr;
                void *ud = lua_touserdata(L, idx);
                if (ud)
                    h = *static_cast<LHeader **>(ud);
                if (h)
                    return LValue(h);
                return LValue();
            }
            lua_pop(L, 2);
        }
        return LValue();
    }
    case LUA_TTHREAD:

    {
        LValue err_msg(clx_L->intern_string("cannot pass Lua VM coroutine to clx function: "
                                            "use Lua VM's own coroutine library within the loaded chunk"));
        throw LRuntimeException(err_msg);
    }
    default:
        return LValue();
    }
}

//------------------ clx_vm_capture_error_
LValue clx_vm_capture_error_(LState *clx_L, lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    if (!msg)
        return LValue();
    size_t len = std::strlen(msg);
    LValue v = clx_L->intern_lvalue(msg, len);
    lua_pop(L, 1);
    return v;
}

//------------------ clx_vm_install_traceback_
int clx_vm_install_traceback_(lua_State *L) {

    return 0;
}

//------------------ env-table upvalues (extern "C")
}

extern "C" {

int clx_vm_env_index(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    clx::LTable *tbl = static_cast<clx::LTable *>(lua_touserdata(L, lua_upvalueindex(2)));
    if (!clx_L || !tbl) {
        lua_pushnil(L);
        return 1;
    }
    clx::LValue key = vm_to_clx_value_(clx_L, L, 2);

    clx::LValue val = tbl->get_value(clx_L, key);
    clx_to_vm_value_(clx_L, L, val);
    return 1;
}

extern "C" int clx_vm_env_newindex(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    clx::LTable *tbl = static_cast<clx::LTable *>(lua_touserdata(L, lua_upvalueindex(2)));
    if (!clx_L || !tbl)
        return 0;
    clx::LValue key = vm_to_clx_value_(clx_L, L, 2);
    clx::LValue val = vm_to_clx_value_(clx_L, L, 3);
    tbl->settable(key, val);
    return 0;
}

extern "C" int clx_vm_env_pairs(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    clx::LTable *tbl = static_cast<clx::LTable *>(lua_touserdata(L, lua_upvalueindex(2)));
    if (!clx_L || !tbl) {

        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushnil(L);
        return 3;
    }

    lua_pushlightuserdata(L, static_cast<void *>(clx_L));
    lua_pushlightuserdata(L, static_cast<void *>(tbl));
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, clx_vm_env_pairs_iter, 3);

    lua_pushnil(L);
    lua_pushinteger(L, 0);
    return 3;
}

extern "C" int clx_vm_env_pairs_iter(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    clx::LTable *tbl = static_cast<clx::LTable *>(lua_touserdata(L, lua_upvalueindex(2)));
    if (!clx_L || !tbl)
        return 0;
    size_t idx = static_cast<size_t>(lua_tointeger(L, lua_upvalueindex(3)));
    size_t array_size = tbl->array_size;
    while (idx < array_size) {
        if (tbl->array_types[idx] != clx::Nil) {
            lua_pushinteger(L, static_cast<lua_Integer>(idx + 1));
            clx::LValue val(tbl->array[idx], tbl->array_types[idx]);
            clx_to_vm_value_(clx_L, L, val);
            lua_pushinteger(L, static_cast<lua_Integer>(idx + 2));
            lua_replace(L, lua_upvalueindex(3));
            return 2;
        }
        ++idx;
    }
    return 0;
}

//------------------ clx_userdata __index
extern "C" int clx_vm_userdata_index(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    void *ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw) {
        lua_pushnil(L);
        return 1;
    }
    clx::LHeader *h = *static_cast<clx::LHeader **>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::UserData)) {
        lua_pushnil(L);
        return 1;
    }
    clx::LUserdata *ud = static_cast<clx::LUserdata *>(h);
    if (!ud->metatable) {
        lua_pushnil(L);
        return 1;
    }
    clx::LValue key = clx::vm_to_clx_value_(clx_L, L, 2);
    clx::LValue val = ud->metatable->gettable(key);
    clx::clx_to_vm_value_(clx_L, L, val);
    return 1;
}

//------------------ clx_proxy metamethods
extern "C" int clx_vm_proxy_index(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    void *ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw) {
        lua_pushnil(L);
        return 1;
    }
    clx::LHeader *h = *static_cast<clx::LHeader **>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table)) {
        lua_pushnil(L);
        return 1;
    }
    clx::LTable *tbl = static_cast<clx::LTable *>(h);
    clx::LValue key = clx::vm_to_clx_value_(clx_L, L, 2);

    clx::LValue val = tbl->get_value(clx_L, key);
    if (val.type == clx::Nil && tbl->metatable) {

        val = tbl->metatable->gettable(key);
    }
    clx::clx_to_vm_value_(clx_L, L, val);
    return 1;
}

extern "C" int clx_vm_proxy_newindex(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    void *ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw)
        return 0;
    clx::LHeader *h = *static_cast<clx::LHeader **>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table))
        return 0;
    clx::LTable *tbl = static_cast<clx::LTable *>(h);
    clx::LValue key = clx::vm_to_clx_value_(clx_L, L, 2);
    clx::LValue val = clx::vm_to_clx_value_(clx_L, L, 3);
    tbl->settable(key, val);
    return 0;
}

extern "C" int clx_vm_proxy_len(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    void *ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw) {
        lua_pushinteger(L, 0);
        return 1;
    }
    clx::LHeader *h = *static_cast<clx::LHeader **>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table)) {
        lua_pushinteger(L, 0);
        return 1;
    }
    clx::LValue t(clx::Table, static_cast<clx::LTable *>(h));
    lua_pushinteger(L, static_cast<lua_Integer>(clx::rawlen(t)));
    return 1;
}

extern "C" int clx_vm_proxy_pairs_iter(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    void *ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw)
        return 0;
    clx::LHeader *h = *static_cast<clx::LHeader **>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table))
        return 0;
    clx::LTable *tbl = static_cast<clx::LTable *>(h);
    clx::LValue t(clx::Table, tbl);
    clx::LValue k = clx::vm_to_clx_value_(clx_L, L, 2);
    clx::MultiValue res = clx::next(clx_L, t, k);
    if (res.count == 0 || res[0].type == clx::Nil)
        return 0;
    clx::clx_to_vm_value_(clx_L, L, res[0]);
    clx::clx_to_vm_value_(clx_L, L, res[1]);
    return 2;
}

extern "C" int clx_vm_proxy_pairs(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!clx_L)
        return 0;
    lua_pushlightuserdata(L, static_cast<void *>(clx_L));
    lua_pushcclosure(L, clx_vm_proxy_pairs_iter, 1);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

//------------------ clx_vm_proxy_call - Lua __call metamethod
extern "C" int clx_vm_proxy_call(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    void *ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw)
        return 0;
    clx::LHeader *h = *static_cast<clx::LHeader **>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table))
        return 0;
    clx::LTable *tbl = static_cast<clx::LTable *>(h);
    if (!tbl->metatable)
        return 0;
    clx::LValue call_func = tbl->metatable->gettable(clx_L->str_call);
    if (call_func.type != clx::Function)
        return 0;

    int nargs = lua_gettop(L) - 1;
    const size_t clx_nargs = static_cast<size_t>(nargs) + 1;
    clx::LValue *clx_args;
    clx::LValue stack_buf[16];
    bool heap = clx_nargs > 16;
    if (heap)
        clx_args = new clx::LValue[clx_nargs];
    else
        clx_args = stack_buf;

    clx_args[0] = clx::LValue(clx::Table, tbl);
    for (int i = 0; i < nargs; ++i)
        clx_args[i + 1] = clx::vm_to_clx_value_(clx_L, L, i + 2);

    size_t prev_shadow = clx_L->shadow_top;
    for (size_t i = 0; i < clx_nargs; ++i)
        clx_L->shadow_stack[clx_L->shadow_top++] = clx::TypedSlot(&clx_args[i].val, &clx_args[i].type);

    clx::MultiValue ret;
    try {
        ret = clx::call_function(clx_L, call_func, clx_args, clx_nargs, "__call", 0);
    } catch (const clx::LRuntimeException &e) {
        clx_L->shadow_top = prev_shadow;
        if (heap)
            delete[] clx_args;
        lua_pushstring(L, e.what());
        lua_error(L);
        return 0;
    } catch (const std::exception &e) {
        clx_L->shadow_top = prev_shadow;
        if (heap)
            delete[] clx_args;
        lua_pushstring(L, e.what());
        lua_error(L);
        return 0;
    } catch (...) {
        clx_L->shadow_top = prev_shadow;
        if (heap)
            delete[] clx_args;
        lua_pushstring(L, "unknown error in __call");
        lua_error(L);
        return 0;
    }
    clx_L->shadow_top = prev_shadow;
    if (heap)
        delete[] clx_args;

    for (size_t i = 0; i < ret.count; ++i)
        clx::clx_to_vm_value_(clx_L, L, ret[i]);
    return static_cast<int>(ret.count);
}

//------------------ clx_vm_proxy_close - Lua __close metamethod
extern "C" int clx_vm_proxy_close(lua_State *L) {
    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    void *ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw)
        return 0;
    clx::LHeader *h = *static_cast<clx::LHeader **>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table))
        return 0;
    clx::LTable *tbl = static_cast<clx::LTable *>(h);
    if (!tbl->metatable)
        return 0;
    clx::LValue close_func = tbl->metatable->gettable(clx_L->str_close);
    if (close_func.type != clx::Function)
        return 0;
    clx::LValue args[2] = { clx::LValue(clx::Table, tbl), clx::LValue() };
    size_t prev_shadow = clx_L->shadow_top;
    clx_L->shadow_stack[clx_L->shadow_top++] = clx::TypedSlot(&args[0].val, &args[0].type);
    clx_L->shadow_stack[clx_L->shadow_top++] = clx::TypedSlot(&args[1].val, &args[1].type);
    try {
        clx::call_function(clx_L, close_func, args, 2, "__close", 0);
    } catch (const clx::LRuntimeException &e) {
        std::fprintf(stderr, "error in __close metamethod: %s\n", e.what());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error in __close metamethod: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "error in __close metamethod\n");
    }
    clx_L->shadow_top = prev_shadow;
    return 0;
}

//------------------ clx_vm_proxy_gc - Lua __gc metamethod
extern "C" int clx_vm_proxy_gc(lua_State *L) {
    clx::ProxyNode *node = static_cast<clx::ProxyNode *>(lua_touserdata(L, 1));
    if (!node || !node->header)
        return 0;

    clx::LState *clx_L = static_cast<clx::LState *>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!clx_L)
        return 0;
    clx::DynamicVM *vm = clx::DynamicVM::find(clx_L);
    if (vm)
        vm->unlink_proxy(node);
    return 0;
}
}
