// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  vm_convert.cpp · Conversion Machinery      │
// └─────────────────────────────────────────────┘

#include "vm_convert.h"
#include "dynamic_vm.h"
#include "vm_function_object.h"
#include "vm_native_bridge.h"
#include "vm_table_proxy.h"

#include <clx.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

extern "C" int clx_vm_userdata_index(lua_State* L);
extern "C" int clx_vm_type(lua_State* L);
extern "C" int clx_vm_rawget(lua_State* L);
extern "C" int clx_vm_rawset(lua_State* L);
extern "C" int clx_vm_next(lua_State* L);
extern "C" int clx_vm_rawlen(lua_State* L);
extern "C" int clx_vm_proxy_index(lua_State* L);
extern "C" int clx_vm_proxy_newindex(lua_State* L);
extern "C" int clx_vm_proxy_pairs(lua_State* L);
extern "C" int clx_vm_proxy_len(lua_State* L);
extern "C" int clx_vm_proxy_gc(lua_State* L);
extern "C" int clx_vm_proxy_close(lua_State* L);
extern "C" int clx_vm_proxy_call(lua_State* L);
extern "C" int clx_vm_env_index(lua_State* L);
extern "C" int clx_vm_env_newindex(lua_State* L);
extern "C" int clx_vm_env_pairs(lua_State* L);
extern "C" int clx_vm_env_pairs_iter(lua_State* L);

//------------------ Thread-local conversion cache - circular reference detection during Lua table-to-clx LTable conversion
namespace {
struct ConvertCache {
    std::unordered_map<const void*, clx::LValue> map;
    int depth = 0;
};

thread_local ConvertCache s_convert_cache;
}

namespace clx {

namespace {

//------------------ push_cached_object - re-push a previously created proxy for this clx object, if still alive
bool push_cached_object(LState* clx_L, lua_State* L, LHeader* header)
{
    if (!header)
        return false;
    int cache = DynamicVM::acquire(clx_L)->bridge_cache_registry_ref();
    if (cache == LUA_NOREF)
        return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, cache);
    lua_pushlightuserdata(L, static_cast<void*>(header));
    lua_rawget(L, -2);
    bool hit = !lua_isnil(L, -1);
    lua_remove(L, -2);
    if (!hit)
        lua_pop(L, 1);
    return hit;
}

//------------------ store_cached_object - publish the object on top of the stack under its clx header
void store_cached_object(LState* clx_L, lua_State* L, LHeader* header)
{
    if (!header)
        return;
    int cache = DynamicVM::acquire(clx_L)->bridge_cache_registry_ref();
    if (cache == LUA_NOREF)
        return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, cache);
    lua_pushlightuserdata(L, static_cast<void*>(header));
    lua_pushvalue(L, -3);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

}

namespace {

//------------------ s_sync_stack -- converted tables whose contents are being pushed back into the VM
//------------------ table they came from, so a cycle in the copied graph stops at the origin instead of
//------------------ recursing forever
thread_local std::vector<LTable*> s_sync_stack;

//------------------ sync_in_progress -- true when this table is already on the write-back stack
bool sync_in_progress(LTable* t)
{
    for (LTable* s : s_sync_stack) {
        if (s == t)
            return true;
    }
    return false;
}

//------------------ SyncGuard -- keeps the write-back stack balanced when a conversion below throws,
//------------------ so a table left on it would be skipped by every later crossing
struct SyncGuard {
    explicit SyncGuard(LTable* t) { s_sync_stack.push_back(t); }
    ~SyncGuard() { s_sync_stack.pop_back(); }
};

}

//------------------ clx_vm_sync_table_out_ - write a converted table's current entries back into the VM
//------------------ table it was copied from, so a write made on the clx side reaches the chunk that still
//------------------ holds the original. Mirrors the bridge's argument write-back: the array tail the copy no
//------------------ longer has is cleared, keys the copy dropped are left alone (stock's copy semantics),
//------------------ and the VM metatable is never replaced from here.
void clx_vm_sync_table_out_(LState* clx_L, lua_State* L, LTable* tbl, int reg_ref)
{
    //------------------ a proxy pushed for a nested value can step the collector; keep the table alive
    LValue self(Table, tbl);
    RootRange self_rr(clx_L, &self, 1);

    lua_rawgeti(L, LUA_REGISTRYINDEX, reg_ref);
    int tbl_idx = lua_gettop(L);
    if (!lua_istable(L, tbl_idx)) {
        lua_pop(L, 1);
        return;
    }
    size_t orig_len = static_cast<size_t>(lua_rawlen(L, tbl_idx));

    for (size_t i = 0; i < tbl->array_size; ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
        clx_to_vm_value_(clx_L, L, LValue(tbl->array[i], tbl->array_types[i]));
        lua_rawset(L, tbl_idx);
    }
    for (size_t i = tbl->array_size + 1; i <= orig_len; ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(i));
        lua_pushnil(L);
        lua_rawset(L, tbl_idx);
    }

    if (tbl->ext && tbl->ext->hash_size > 0) {
        for (size_t i = 0; i < tbl->ext->hash_size; ++i) {
            if (tbl->ext->entries[i].ktype == Nil || tbl->ext->entries[i].vtype == Nil)
                continue;
            clx_to_vm_value_(clx_L, L, LValue(tbl->ext->entries[i].key, tbl->ext->entries[i].ktype));
            clx_to_vm_value_(clx_L, L, LValue(tbl->ext->entries[i].val, tbl->ext->entries[i].vtype));
            lua_rawset(L, tbl_idx);
        }
    }
    lua_pop(L, 1);
}

//------------------ clx_to_vm_value_
void clx_to_vm_value_(LState* clx_L, lua_State* L, const LValue& v)
{
    switch (v.type) {
    case Nil:
        lua_pushnil(L);
        return;
    case Boolean:
        lua_pushboolean(L, v.as_bool() ? 1 : 0);
        return;
    case Int64: {
        //------------------ lua_Integer is 64-bit here, so the full clx range crosses losslessly;
        //------------------ never demote to double or integer-ness (and wrapping) is lost
        lua_pushinteger(L, static_cast<lua_Integer>(v.val.payload.i64));
        return;
    }
    case Double:
        if (v.val.payload.f64 != v.val.payload.f64) {

            lua_pushnumber(L, std::numeric_limits<double>::quiet_NaN());
        } else {
            lua_pushnumber(L, v.val.payload.f64);
        }
        return;
    case String: {
        const char* s = v.as_string();
        uint32_t len = v.string_len();
        lua_pushlstring(L, s ? s : "", len);
        return;
    }
    case Table:
        if (v.as_pointer()) {

            LHeader* h = v.as_pointer();
            //------------------ a table copied out of the VM goes back as the very table it came from:
            //------------------ its clx-side writes are synced into that table first, then it is pushed
            //------------------ again, so a chunk keeps one object across the crossing
            if (!(h->flags & LFLAG_VM_PROXY) && h->type == static_cast<uint8_t>(Table)) {
                LTable* origin_tbl = static_cast<LTable*>(h);
                if (origin_tbl->ext && origin_tbl->ext->vm_origin >= 0) {
                    int origin = origin_tbl->ext->vm_origin;
                    //------------------ only a copy clx changed is written back: an untouched copy is a
                    //------------------ snapshot, and pushing it over the VM table would undo whatever the
                    //------------------ VM did to its own table since the copy was made
                    if (origin_tbl->ext->vm_dirty && !sync_in_progress(origin_tbl)) {
                        {
                            //------------------ one write-back per table per crossing: the stack doubles as
                            //------------------ the visited set, so a shared or cyclic graph costs O(nodes)
                            SyncGuard guard(origin_tbl);
                            clx_vm_sync_table_out_(clx_L, L, origin_tbl, origin);
                        }
                        origin_tbl->ext->vm_dirty = 0;
                    }
                    lua_rawgeti(L, LUA_REGISTRYINDEX, origin);
                    return;
                }
            }
            if (push_cached_object(clx_L, L, h))
                return;
            ProxyNode* node = static_cast<ProxyNode*>(lua_newuserdata(L, sizeof(ProxyNode)));
            node->header = h;
            node->prev = nullptr;
            node->next = nullptr;

            DynamicVM* vm = DynamicVM::acquire(clx_L);
            vm->link_proxy(node);

            if (luaL_newmetatable(L, "clx_proxy")) {

                lua_pushlightuserdata(L, static_cast<void*>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_index, 1);
                lua_setfield(L, -2, "__index");

                lua_pushlightuserdata(L, static_cast<void*>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_newindex, 1);
                lua_setfield(L, -2, "__newindex");

                lua_pushlightuserdata(L, static_cast<void*>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_pairs, 1);
                lua_setfield(L, -2, "__pairs");

                lua_pushlightuserdata(L, static_cast<void*>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_len, 1);
                lua_setfield(L, -2, "__len");

                lua_pushlightuserdata(L, static_cast<void*>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_call, 1);
                lua_setfield(L, -2, "__call");

                lua_pushlightuserdata(L, static_cast<void*>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_close, 1);
                lua_setfield(L, -2, "__close");

                lua_pushlightuserdata(L, static_cast<void*>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_gc, 1);
                lua_setfield(L, -2, "__gc");

                lua_setmetatable(L, -2);

            } else {

                lua_setmetatable(L, -2);
            }
            store_cached_object(clx_L, L, h);
        } else {
            lua_pushnil(L);
        }
        return;
    case Function:
        if (v.as_pointer()) {

            LCFunction* cf = static_cast<LCFunction*>(v.as_pointer());
            if (cf->flags & LFLAG_VM_PROXY) {

                lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<VMFunction*>(cf)->registry_ref);
            } else {
                create_bridge(clx_L, L, v);
            }
        } else {
            lua_pushnil(L);
        }
        return;
    case UserData: {

        if (v.as_pointer()) {
            LHeader* h = v.as_pointer();
            if (push_cached_object(clx_L, L, h))
                return;
            ProxyNode* node = static_cast<ProxyNode*>(lua_newuserdata(L, sizeof(ProxyNode)));
            node->header = h;
            node->prev = nullptr;
            node->next = nullptr;
            DynamicVM* vm = DynamicVM::acquire(clx_L);
            vm->link_proxy(node);
            if (luaL_newmetatable(L, "clx_userdata")) {

                lua_pushlightuserdata(L, static_cast<void*>(clx_L));
                lua_pushcclosure(L, clx_vm_userdata_index, 1);
                lua_setfield(L, -2, "__index");

                lua_pushlightuserdata(L, static_cast<void*>(clx_L));
                lua_pushcclosure(L, clx_vm_proxy_gc, 1);
                lua_setfield(L, -2, "__gc");
                lua_setmetatable(L, -2);
            } else {
                lua_setmetatable(L, -2);
            }
            store_cached_object(clx_L, L, h);
        } else {
            lua_pushnil(L);
        }
        return;
    }
    case LightUserData:
        lua_pushlightuserdata(L, v.val.payload.ptr);
        return;
    case Thread:

        throw_runtime_error("cannot pass clx coroutine to Lua VM function: "
                            "use Lua VM's own coroutine library (copied to env table) instead");
    default:

        lua_pushnil(L);
        return;
    }
}

//------------------ vm_to_clx_value_
LValue vm_to_clx_value_(LState* clx_L, lua_State* L, int idx)
{
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
        const char* s = lua_tolstring(L, idx, &len);
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
            const void* tp = lua_topointer(L, idx);
            std::snprintf(buf, sizeof(buf), "vm_to_clx_value_ maximum conversion depth (%d) exceeded at table %p",
                MAX_CONVERT_DEPTH, tp);
            throw clx::LRuntimeException(clx::LValue(clx_L->intern_string(buf)));
        }

        if (s_convert_cache.depth == 0)
            s_convert_cache.map.clear();
        s_convert_cache.depth++;

        const void* tbl_key = lua_topointer(L, idx);
        if (tbl_key) {
            auto it = s_convert_cache.map.find(tbl_key);
            if (it != s_convert_cache.map.end()) {
                s_convert_cache.depth--;
                return it->second;
            }
        }

        lua_pushvalue(L, idx);
        int abs_idx = lua_gettop(L);

        //------------------ a table clx already holds a copy for maps back to that copy, so a value that
        //------------------ crosses again does not fork into a second clx table. The index holds copies
        //------------------ weakly, so its entries disappear with them and a dead one is never returned.
        if (tbl_key) {
            if (DynamicVM* vm = DynamicVM::find(clx_L)) {
                LValue existing = vm->object_index_get(tbl_key);
                if (existing.type == Table && existing.as_pointer()) {
                    s_convert_cache.map[tbl_key] = existing;
                    lua_pop(L, 1);
                    s_convert_cache.depth--;
                    return existing;
                }
            }
        }

        LValue t = clx_L->create_table();
        LTable* tbl = static_cast<LTable*>(t.as_pointer());

        if (tbl_key)
            s_convert_cache.map[tbl_key] = t;

        clx::RootRange t_rr(clx_L, &t, 1);

        //------------------ pin the VM table this copy was made from: pushing the copy back to the VM
        //------------------ returns that same table, so identity survives a round trip through clx
        if (tbl->ext == nullptr || tbl->ext->vm_origin < 0) {
            const size_t hb_ext = table_heap_bytes(tbl);
            lua_pushvalue(L, abs_idx);
            tbl_ensure_ext(tbl)->vm_origin = luaL_ref(L, LUA_REGISTRYINDEX);
            clx_L->account_delta(hb_ext, table_heap_bytes(tbl));
        }
        if (tbl_key) {
            if (DynamicVM* vm = DynamicVM::find(clx_L))
                vm->object_index_put(tbl_key, t);
        }

        if (lua_getmetatable(L, abs_idx)) {
            LValue meta_copy = vm_to_clx_value_(clx_L, L, -1);
            lua_pop(L, 1);
            if (meta_copy.type == Table) {
                LTable* meta_tbl = static_cast<LTable*>(meta_copy.as_pointer());

                meta_tbl->settable(clx_L->str_gc, LValue());
                tbl_set_metatable(tbl, meta_tbl);
                gc_barrier(clx_L, tbl, LValue(Table, meta_tbl));
            }
        }

        size_t len = static_cast<size_t>(lua_rawlen(L, abs_idx));
        if (len > 0) {
            size_t cap = 8;
            while (cap < len)
                cap *= 2;
            const size_t hb_vm = table_heap_bytes(tbl);
            table_alloc_array(tbl->array, tbl->array_types, cap);
            tbl->array_cap = cap;
            clx_L->account_delta(hb_vm, table_heap_bytes(tbl));
            size_t run = 0;
            for (size_t i = 0; i < len; ++i) {
                lua_rawgeti(L, abs_idx, static_cast<int>(i + 1));
                LValue val = vm_to_clx_value_(clx_L, L, -1);
                lua_pop(L, 1);
                tbl->array[i] = val.val;
                tbl->array_types[i] = val.type;
                tbl->array_size = i + 1;
                if (i == run && val.type != Nil)
                    ++run;
            }
            tbl->array_lim = run;
        }

        lua_pushnil(L);
        while (lua_next(L, abs_idx) != 0) {
            LValue kv[2];
            clx::RootRange kv_rr(clx_L, kv, 2);
            kv[0] = vm_to_clx_value_(clx_L, L, -2);
            kv[1] = vm_to_clx_value_(clx_L, L, -1);
            tbl->settable(kv[0], kv[1]);
            lua_pop(L, 1);
        }

        lua_pop(L, 1);

        //------------------ filling the copy goes through settable, so drop the write mark: the copy now
        //------------------ matches its origin and has nothing of clx's own to push back yet
        if (tbl->ext)
            tbl->ext->vm_dirty = 0;

        s_convert_cache.depth--;
        if (s_convert_cache.depth == 0)
            s_convert_cache.map.clear();

        return t;
    }
    case LUA_TFUNCTION: {

        LValue vf = vm_to_clx_function_(clx_L, L, idx);
        return vf;
    }
    case LUA_TLIGHTUSERDATA:
        return LValue::light_udata(lua_touserdata(L, idx));
    case LUA_TUSERDATA: {

        if (lua_getmetatable(L, idx)) {

            luaL_getmetatable(L, "clx_proxy");

            if (lua_rawequal(L, -1, -2) == 1) {
                lua_pop(L, 2);
                void* ud = lua_touserdata(L, idx);
                if (ud) {
                    LHeader* h = *static_cast<LHeader**>(ud);
                    return LValue(h);
                }
                return LValue();
            }

            lua_pop(L, 1);

            luaL_getmetatable(L, "clx_thread");

            if (lua_rawequal(L, -1, -2) == 1) {
                lua_pop(L, 2);
                void* ud = lua_touserdata(L, idx);
                if (ud) {
                    LHeader* h = *static_cast<LHeader**>(ud);
                    return LValue(Thread, h);
                }
                return LValue();
            }
            lua_pop(L, 1);

            luaL_getmetatable(L, "clx_userdata");

            if (lua_rawequal(L, -1, -2) == 1) {
                lua_pop(L, 2);
                void* ud = lua_touserdata(L, idx);
                if (ud) {
                    LHeader* h = *static_cast<LHeader**>(ud);
                    return LValue(UserData, h);
                }
                return LValue();
            }
            lua_pop(L, 1);

            luaL_getmetatable(L, "clx_callable");

            if (lua_rawequal(L, -1, -2) == 1) {
                lua_pop(L, 2);
                NativeBridge* nb = nullptr;
                void* ud = lua_touserdata(L, idx);
                if (ud)
                    nb = *static_cast<NativeBridge**>(ud);
                if (nb)
                    return nb->clx_callable;
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
LValue clx_vm_capture_error_(LState* clx_L, lua_State* L)
{
    const char* msg = lua_tostring(L, -1);
    if (!msg)
        return LValue();
    size_t len = std::strlen(msg);
    LValue v = clx_L->intern_lvalue(msg, len);
    lua_pop(L, 1);
    return v;
}

//------------------ clx_vm_require - a loaded chunk's require: the VM's own package.loaded answers first,
//------------------ so `require"debug"` reaches the VM's real library instead of the compiled-code stub,
//------------------ and everything else falls through to clx's require (shared path/loaded state)
extern "C" int clx_vm_require(lua_State* L)
{
    int vm_loaded_ref = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    if (lua_type(L, 1) == LUA_TSTRING) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, vm_loaded_ref);
        lua_pushvalue(L, 1);
        lua_rawget(L, -2);
        if (!lua_isnil(L, -1)) {
            lua_remove(L, -2);
            return 1;
        }
        lua_pop(L, 2);
    }
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_insert(L, 1);
    int nargs = lua_gettop(L) - 1;
    lua_call(L, nargs, LUA_MULTRET);
    return lua_gettop(L);
}

//------------------ clx_vm_install_traceback_
int clx_vm_install_traceback_(lua_State* L)
{

    return 0;
}

}

extern "C" {

int clx_vm_env_index(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    clx::LTable* tbl = static_cast<clx::LTable*>(lua_touserdata(L, lua_upvalueindex(2)));
    if (!clx_L || !tbl) {
        lua_pushnil(L);
        return 1;
    }
    clx::LValue key = vm_to_clx_value_(clx_L, L, 2);

    clx::LValue val = tbl->get_value(clx_L, key);
    clx_to_vm_value_(clx_L, L, val);
    return 1;
}

extern "C" int clx_vm_env_newindex(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    clx::LTable* tbl = static_cast<clx::LTable*>(lua_touserdata(L, lua_upvalueindex(2)));
    if (!clx_L || !tbl)
        return 0;

    //------------------ the default environment keeps every chunk-written global in clx and none in the VM
    //------------------ table: a name stored in the VM table would be assigned there directly next time and
    //------------------ never reach __newindex, leaving compiled code with the first value it mirrored
    if (lua_tointeger(L, lua_upvalueindex(4)) == 1) {
        if (!lua_isnoneornil(L, lua_upvalueindex(5))) {
            lua_pushvalue(L, lua_upvalueindex(5));
            lua_pushvalue(L, 2);
            lua_pushboolean(L, 1);
            lua_rawset(L, -3);
            lua_pop(L, 1);
        }
    }

    clx::LValue kv[2];
    clx::RootRange kv_rr(clx_L, kv, 2);
    kv[0] = vm_to_clx_value_(clx_L, L, 2);
    kv[1] = vm_to_clx_value_(clx_L, L, 3);
    tbl->settable(kv[0], kv[1]);
    return 0;
}

extern "C" int clx_vm_env_pairs(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    clx::LTable* tbl = static_cast<clx::LTable*>(lua_touserdata(L, lua_upvalueindex(2)));
    if (!clx_L || !tbl) {

        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushnil(L);
        return 3;
    }

    lua_pushlightuserdata(L, static_cast<void*>(clx_L));
    lua_pushlightuserdata(L, static_cast<void*>(tbl));
    lua_pushcclosure(L, clx_vm_env_pairs_iter, 2);

    //------------------ the environment itself is the iterator's state, so each step can prefer the value
    //------------------ the VM table holds; stock's pairs hands out a nil control value, so the first
    //------------------ next() call starts the walk
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

extern "C" int clx_vm_env_pairs_iter(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    clx::LTable* tbl = static_cast<clx::LTable*>(lua_touserdata(L, lua_upvalueindex(2)));
    if (!clx_L || !tbl)
        return 0;

    //------------------ walk the whole clx table (array and hash) the way stock pairs does; the state
    //------------------ passed by the for loop is the environment table itself
    clx::LValue t(clx::Table, tbl);
    clx::LValue k = clx::vm_to_clx_value_(clx_L, L, 2);
    clx::MultiValue res = clx::next(clx_L, t, k);
    if (res.count == 0 || res[0].type == clx::Nil)
        return 0;
    clx_to_vm_value_(clx_L, L, res[0]);
    //------------------ a name the VM table itself holds is handed back as the VM holds it, so a walk of
    //------------------ the environment yields the same value a read of that name does
    if (lua_type(L, 1) == LUA_TTABLE) {
        lua_pushvalue(L, 1);
        lua_pushvalue(L, -2);
        lua_rawget(L, -2);
        if (!lua_isnil(L, -1)) {
            lua_remove(L, -2);
            return 2;
        }
        lua_pop(L, 2);
    }
    clx_to_vm_value_(clx_L, L, res[1]);
    return 2;
}

//------------------ clx_userdata __index
extern "C" int clx_vm_userdata_index(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    void* ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw) {
        lua_pushnil(L);
        return 1;
    }
    clx::LHeader* h = *static_cast<clx::LHeader**>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::UserData)) {
        lua_pushnil(L);
        return 1;
    }
    clx::LUserdata* ud = static_cast<clx::LUserdata*>(h);
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
extern "C" int clx_vm_proxy_index(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    void* ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw) {
        lua_pushnil(L);
        return 1;
    }
    clx::LHeader* h = *static_cast<clx::LHeader**>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table)) {
        lua_pushnil(L);
        return 1;
    }
    clx::LTable* tbl = static_cast<clx::LTable*>(h);
    clx::LValue key = clx::vm_to_clx_value_(clx_L, L, 2);

    clx::LValue val = tbl->get_value(clx_L, key);
    if (val.type == clx::Nil) {
        if (clx::LTable* mt = tbl_metatable(tbl))
            val = mt->gettable(key);
    }
    clx::clx_to_vm_value_(clx_L, L, val);
    return 1;
}

extern "C" int clx_vm_proxy_newindex(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    void* ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw)
        return 0;
    clx::LHeader* h = *static_cast<clx::LHeader**>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table))
        return 0;
    clx::LTable* tbl = static_cast<clx::LTable*>(h);

    clx::LValue kv[2];
    clx::RootRange kv_rr(clx_L, kv, 2);
    kv[0] = clx::vm_to_clx_value_(clx_L, L, 2);
    kv[1] = clx::vm_to_clx_value_(clx_L, L, 3);
    tbl->settable(kv[0], kv[1]);
    return 0;
}

extern "C" int clx_vm_proxy_len(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    void* ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw) {
        lua_pushinteger(L, 0);
        return 1;
    }
    clx::LHeader* h = *static_cast<clx::LHeader**>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table)) {
        lua_pushinteger(L, 0);
        return 1;
    }
    clx::LValue t(clx::Table, static_cast<clx::LTable*>(h));
    lua_pushinteger(L, static_cast<lua_Integer>(clx::rawlen(t)));
    return 1;
}

extern "C" int clx_vm_proxy_pairs_iter(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    void* ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw)
        return 0;
    clx::LHeader* h = *static_cast<clx::LHeader**>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table))
        return 0;
    clx::LTable* tbl = static_cast<clx::LTable*>(h);
    clx::LValue t(clx::Table, tbl);
    clx::LValue k = clx::vm_to_clx_value_(clx_L, L, 2);
    clx::MultiValue res = clx::next(clx_L, t, k);
    if (res.count == 0 || res[0].type == clx::Nil)
        return 0;
    clx::clx_to_vm_value_(clx_L, L, res[0]);
    clx::clx_to_vm_value_(clx_L, L, res[1]);
    return 2;
}

extern "C" int clx_vm_proxy_pairs(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!clx_L)
        return 0;
    lua_pushlightuserdata(L, static_cast<void*>(clx_L));
    lua_pushcclosure(L, clx_vm_proxy_pairs_iter, 1);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

//------------------ clx_vm_proxy_table_of - the clx LTable behind a clx_proxy userdata, or null
static clx::LTable* clx_vm_proxy_table_of(lua_State* L, int idx)
{
    void* ud = luaL_testudata(L, idx, "clx_proxy");
    if (!ud)
        return nullptr;
    clx::LHeader* h = *static_cast<clx::LHeader**>(ud);
    if (!h || h->type != static_cast<uint8_t>(clx::Table))
        return nullptr;
    return static_cast<clx::LTable*>(h);
}

//------------------ clx_vm_type - type() that reports a clx proxy as the value it stands for
extern "C" int clx_vm_type(lua_State* L)
{
    int t = lua_type(L, 1);
    if (t == LUA_TNONE)
        luaL_argerror(L, 1, "value expected");
    if (t == LUA_TUSERDATA) {
        if (luaL_testudata(L, 1, "clx_proxy")) {
            lua_pushstring(L, "table");
            return 1;
        }
        if (luaL_testudata(L, 1, "clx_thread")) {
            lua_pushstring(L, "thread");
            return 1;
        }
    }
    lua_pushstring(L, lua_typename(L, t));
    return 1;
}

//------------------ clx_vm_rawget - rawget accepting a real table or a clx_proxy
extern "C" int clx_vm_rawget(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (clx::LTable* tbl = clx_vm_proxy_table_of(L, 1)) {
        if (lua_type(L, 2) == LUA_TNIL)
            luaL_argerror(L, 2, "table index is nil");
        clx::LValue key = clx::vm_to_clx_value_(clx_L, L, 2);
        clx::LValue val = tbl->gettable(key);
        clx::clx_to_vm_value_(clx_L, L, val);
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
    lua_rawget(L, 1);
    return 1;
}

//------------------ clx_vm_rawset - rawset accepting a real table or a clx_proxy
extern "C" int clx_vm_rawset(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (clx::LTable* tbl = clx_vm_proxy_table_of(L, 1)) {
        clx::LValue kv[2];
        clx::RootRange kv_rr(clx_L, kv, 2);
        kv[0] = clx::vm_to_clx_value_(clx_L, L, 2);
        kv[1] = clx::vm_to_clx_value_(clx_L, L, 3);
        if (kv[0].type == clx::Nil)
            luaL_argerror(L, 2, "table index is nil");
        if (kv[0].type == clx::Double && kv[0].val.payload.f64 != kv[0].val.payload.f64)
            luaL_argerror(L, 2, "table index is NaN");
        tbl->settable(kv[0], kv[1]);
        lua_pushvalue(L, 1);
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 3);
    lua_rawset(L, 1);
    lua_pushvalue(L, 1);
    return 1;
}

//------------------ env_clx_table_of - the clx table behind a chunk environment written by clx, or null for
//------------------ any other table. Such an environment keeps every chunk-written name in clx and none in
//------------------ the VM table, so next() has to walk the clx table or a global a chunk assigned is
//------------------ invisible to the walk (nextvar.lua walks _G looking for the names it just wrote)
static clx::LTable* env_clx_table_of(lua_State* L, int idx)
{
    if (!lua_getmetatable(L, idx))
        return nullptr;
    lua_pushliteral(L, "__index");
    lua_rawget(L, -2);
    clx::LTable* tbl = nullptr;
    if (lua_iscfunction(L, -1) && lua_tocfunction(L, -1) == clx_vm_env_index) {
        if (lua_getupvalue(L, -1, 2))
            tbl = static_cast<clx::LTable*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    return tbl;
}

//------------------ clx_vm_next - next accepting a real table or a clx_proxy
extern "C" int clx_vm_next(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    clx::LTable* tbl = clx_vm_proxy_table_of(L, 1);
    bool env = false;
    if (!tbl) {
        tbl = env_clx_table_of(L, 1);
        env = (tbl != nullptr);
    }
    if (tbl) {
        clx::LValue k;
        if (lua_gettop(L) >= 2 && lua_type(L, 2) != LUA_TNIL)
            k = clx::vm_to_clx_value_(clx_L, L, 2);
        clx::LValue t(clx::Table, tbl);
        clx::MultiValue res = clx::next(clx_L, t, k);
        if (res.count == 0 || res[0].type == clx::Nil) {
            lua_pushnil(L);
            return 1;
        }
        clx::clx_to_vm_value_(clx_L, L, res[0]);
        //------------------ a name the environment's own table holds keeps the value a read of that name
        //------------------ yields, so a walk and a lookup agree on identity
        if (env) {
            lua_pushvalue(L, 1);
            lua_pushvalue(L, -2);
            lua_rawget(L, -2);
            if (!lua_isnil(L, -1)) {
                lua_remove(L, -2);
                return 2;
            }
            lua_pop(L, 2);
        }
        clx::clx_to_vm_value_(clx_L, L, res[1]);
        return 2;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
    if (lua_next(L, 1))
        return 2;
    lua_pushnil(L);
    return 1;
}

//------------------ clx_vm_rawlen - rawlen accepting a real table, a string or a clx_proxy
extern "C" int clx_vm_rawlen(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (clx::LTable* tbl = clx_vm_proxy_table_of(L, 1)) {
        lua_pushinteger(L, static_cast<lua_Integer>(clx::rawlen(clx::LValue(clx::Table, tbl))));
        return 1;
    }
    int t = lua_type(L, 1);
    if (t != LUA_TTABLE && t != LUA_TSTRING)
        luaL_argerror(L, 1, "table or string expected");
    lua_pushinteger(L, static_cast<lua_Integer>(lua_rawlen(L, 1)));
    return 1;
}

//------------------ clx_vm_proxy_call - Lua __call metamethod
extern "C" int clx_vm_proxy_call(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    void* ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw)
        return 0;
    clx::LHeader* h = *static_cast<clx::LHeader**>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table))
        return 0;
    clx::LTable* tbl = static_cast<clx::LTable*>(h);
    clx::LTable* mt = tbl_metatable(tbl);
    //------------------ lua_error() unwinds with longjmp, so the root frame opened below never runs its
    //------------------ destructor: the chain must be cut back to the head we entered with, or it keeps
    //------------------ pointing into this abandoned stack frame and a later mark walks garbage
    clx::RootRange* saved_top_root = clx_L->top_root;
    //------------------ the proxy metatable carries a __call entry, so the VM gets here for every table
    //------------------ call; without a real __call that call must fail like stock, not return nothing
    if (!mt)
        return luaL_error(L, "attempt to call a table value");
    clx::LValue call_func = mt->gettable(clx_L->str_call);
    if (call_func.type != clx::Function)
        return luaL_error(L, "attempt to call a %s value", call_func.type == clx::ValueType::Nil
                ? "table"
                : clx::stock_type_name(call_func));

    int nargs = lua_gettop(L) - 1;
    const size_t clx_nargs = static_cast<size_t>(nargs) + 1;
    clx::LValue* clx_args;
    clx::LValue stack_buf[16];
    bool heap = clx_nargs > 16;
    if (heap)
        clx_args = new clx::LValue[clx_nargs];
    else
        clx_args = stack_buf;

    clx::RootRange args_rr(clx_L, clx_args, clx_nargs);
    clx_args[0] = clx::LValue(clx::Table, tbl);
    for (int i = 0; i < nargs; ++i)
        clx_args[i + 1] = clx::vm_to_clx_value_(clx_L, L, i + 2);

    clx::MultiValue ret;
    try {
        static constexpr clx::CallSite call_site = { "__call", 0, false, false };
        ret = clx::call_function(clx_L, call_func, clx_args, clx_nargs, &call_site);
    } catch (const clx::LRuntimeException& e) {
        if (heap)
            delete[] clx_args;
        lua_pushlstring(L, e.error_obj.as_string(), e.error_obj.string_len());
        clx_L->top_root = saved_top_root;
        lua_error(L);
        return 0;
    } catch (const std::exception& e) {
        if (heap)
            delete[] clx_args;
        lua_pushstring(L, e.what());
        clx_L->top_root = saved_top_root;
        lua_error(L);
        return 0;
    } catch (...) {
        if (heap)
            delete[] clx_args;
        lua_pushstring(L, "unknown error in __call");
        clx_L->top_root = saved_top_root;
        lua_error(L);
        return 0;
    }
    if (heap)
        delete[] clx_args;

    for (size_t i = 0; i < ret.count; ++i)
        clx::clx_to_vm_value_(clx_L, L, ret[i]);
    return static_cast<int>(ret.count);
}

//------------------ clx_vm_proxy_close - Lua __close metamethod
extern "C" int clx_vm_proxy_close(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    void* ud_raw = lua_touserdata(L, 1);
    if (!clx_L || !ud_raw)
        return 0;
    clx::LHeader* h = *static_cast<clx::LHeader**>(ud_raw);
    if (!h || h->type != static_cast<uint8_t>(clx::Table))
        return 0;
    clx::LTable* tbl = static_cast<clx::LTable*>(h);
    clx::LTable* mt = tbl_metatable(tbl);
    if (!mt)
        return 0;
    clx::LValue close_func = mt->gettable(clx_L->str_close);
    if (close_func.type != clx::Function)
        return 0;
    clx::LValue args[2] = { clx::LValue(clx::Table, tbl), clx::LValue() };
    clx::RootRange close_rr(clx_L, args, 2);
    try {
        static constexpr clx::CallSite close_site = { "__close", 0, false, false };
        clx::call_function(clx_L, close_func, args, 2, &close_site);
    } catch (const clx::LRuntimeException& e) {
        std::fprintf(stderr, "error in __close metamethod: %s\n", e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error in __close metamethod: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "error in __close metamethod\n");
    }
    return 0;
}

//------------------ clx_vm_proxy_gc - Lua __gc metamethod
extern "C" int clx_vm_proxy_gc(lua_State* L)
{
    clx::ProxyNode* node = static_cast<clx::ProxyNode*>(lua_touserdata(L, 1));
    if (!node || !node->header)
        return 0;

    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!clx_L)
        return 0;
    clx::DynamicVM* vm = clx::DynamicVM::find(clx_L);
    if (vm)
        vm->unlink_proxy(node);
    return 0;
}
}
