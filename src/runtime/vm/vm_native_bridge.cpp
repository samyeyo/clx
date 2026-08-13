// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  vm_native_bridge.cpp · Bridge Dispatcher   │
// └─────────────────────────────────────────────┘

#include "vm_native_bridge.h"
#include "vm_convert.h"
#include <clx.h>

namespace clx {

//------------------ create_bridge
NativeBridge* create_bridge(LState* clx_L, lua_State* L, const LValue& callable)
{
    DynamicVM* vm = DynamicVM::acquire(clx_L);
    NativeBridge* nb = new NativeBridge();
    nb->vm = vm;
    nb->clx_callable = callable;
    void* ud = lua_newuserdata(L, sizeof(NativeBridge*));
    *static_cast<NativeBridge**>(ud) = nb;
    luaL_newmetatable(L, "clx_callable");
    lua_setmetatable(L, -2);
    lua_pushcclosure(L, clx_vm_native_bridge_call, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    nb->registry_ref = ref;
    return nb;
}

//------------------ write_back_table - sync clx LTable back to Lua VM table
static void write_back_table(
    LState* clx_L, lua_State* L, LTable* tbl, int reg_ref, size_t orig_len, bool sync_metatable = true)
{

    lua_rawgeti(L, LUA_REGISTRYINDEX, reg_ref);
    int tbl_idx = lua_gettop(L);

    for (size_t i = 0; i < tbl->array_size; ++i) {
        LValue val(tbl->array[i], tbl->array_types[i]);
        lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
        clx_to_vm_value_(clx_L, L, val);
        lua_rawset(L, tbl_idx);
    }

    for (size_t i = tbl->array_size + 1; i <= orig_len; ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(i));
        lua_pushnil(L);
        lua_rawset(L, tbl_idx);
    }

    if (tbl->ext && tbl->ext->hash_bitmap) {
        size_t bm_words = (tbl->ext->hash_size + 63) / 64;
        for (size_t word = 0; word < bm_words; ++word) {
            uint64_t bits = tbl->ext->hash_bitmap[word];
            while (bits) {
                size_t idx = word * 64 + clx_ctzll(bits);
                if (idx >= tbl->ext->hash_size)
                    break;
                LValue key(tbl->ext->entries[idx].key, tbl->ext->entries[idx].ktype);
                LValue val(tbl->ext->entries[idx].val, tbl->ext->entries[idx].vtype);
                clx_to_vm_value_(clx_L, L, key);
                clx_to_vm_value_(clx_L, L, val);
                lua_rawset(L, tbl_idx);
                bits &= bits - 1;
            }
        }
    }

    LTable* mt = tbl_metatable(tbl);
    if (mt && sync_metatable) {
        lua_newtable(L);
        int mt_idx = lua_gettop(L);

        for (size_t i = 0; i < mt->array_size; ++i) {
            if (mt->array_types[i] != Nil) {
                LValue val(mt->array[i], mt->array_types[i]);
                lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
                clx_to_vm_value_(clx_L, L, val);
                lua_rawset(L, mt_idx);
            }
        }

        if (mt->ext && mt->ext->hash_bitmap) {
            size_t bm_words = (mt->ext->hash_size + 63) / 64;
            for (size_t word = 0; word < bm_words; ++word) {
                uint64_t bits = mt->ext->hash_bitmap[word];
                while (bits) {
                    size_t idx = word * 64 + clx_ctzll(bits);
                    if (idx >= mt->ext->hash_size)
                        break;
                    LValue key(mt->ext->entries[idx].key, mt->ext->entries[idx].ktype);
                    LValue val(mt->ext->entries[idx].val, mt->ext->entries[idx].vtype);
                    clx_to_vm_value_(clx_L, L, key);
                    clx_to_vm_value_(clx_L, L, val);
                    lua_rawset(L, mt_idx);
                    bits &= bits - 1;
                }
            }
        }

        lua_setmetatable(L, tbl_idx);

        mt->settable(clx_L->str_gc, clx::LValue());
    }

    lua_pop(L, 1);
}

//------------------ clx_vm_native_bridge_call - Lua-visible entrypoint
extern "C" int clx_vm_native_bridge_call(lua_State* L)
{
    NativeBridge* nb = *static_cast<NativeBridge**>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!nb) {
        lua_pushnil(L);
        return 1;
    }
    LState* clx_L = nb->vm->clx_L();

    int nargs = lua_gettop(L);
    std::vector<LValue> cv_args(static_cast<size_t>(nargs));

    //------------------ Phase 1 - save registry refs for table arguments
    struct TableTrack {
        int reg_ref;
        LValue clx_table;
        size_t orig_len;
        LTable* saved_metatable;
    };

    std::vector<TableTrack> table_tracks;

    for (int i = nargs - 1; i >= 0; --i) {
        if (lua_type(L, i + 1) == LUA_TTABLE) {
            lua_pushvalue(L, i + 1);
            int reg_ref = luaL_ref(L, LUA_REGISTRYINDEX);
            size_t orig_len = static_cast<size_t>(lua_rawlen(L, i + 1));
            cv_args[i] = vm_to_clx_value_(clx_L, L, i + 1);
            LTable* initial_mt = nullptr;
            if (cv_args[i].type == Table && cv_args[i].as_pointer()) {
                LTable* ct = static_cast<LTable*>(cv_args[i].as_pointer());
                initial_mt = tbl_metatable(ct);
            }
            table_tracks.push_back({ reg_ref, cv_args[i], orig_len, initial_mt });
        } else {
            cv_args[i] = vm_to_clx_value_(clx_L, L, i + 1);
        }
    }

    size_t prev_top = clx_L->shadow_top;
    for (int i = 0; i < nargs; ++i) {
        if (cv_args[i].is_gc_obj())
            clx_L->shadow_stack[clx_L->shadow_top++] = TypedSlot(&cv_args[i].val, &cv_args[i].type);
    }

    try {
        MultiValue mv = call_function(clx_L, nb->clx_callable, cv_args.data(), cv_args.size(), __FILE__, __LINE__);

        //------------------ Phase 2 - write back modified tables
        for (size_t t = 0; t < table_tracks.size(); ++t) {
            auto& trk = table_tracks[t];
            LTable* tbl = static_cast<LTable*>(trk.clx_table.as_pointer());
            if (tbl) {
                bool mt_was_modified = (tbl_metatable(tbl) != trk.saved_metatable);
                write_back_table(clx_L, L, tbl, trk.reg_ref, trk.orig_len, mt_was_modified);
            }
            luaL_unref(L, LUA_REGISTRYINDEX, trk.reg_ref);
        }

        clx_L->shadow_top = prev_top;

        size_t nret = mv.count;
        for (size_t i = 0; i < nret; ++i)
            clx_to_vm_value_(clx_L, L, mv[i]);
        return static_cast<int>(nret);
    } catch (const LRuntimeException& e) {

        for (size_t t = 0; t < table_tracks.size(); ++t)
            luaL_unref(L, LUA_REGISTRYINDEX, table_tracks[t].reg_ref);
        clx_L->shadow_top = prev_top;
        LValue err = clx_L->intern_lvalue(e.what(), std::strlen(e.what()));
        clx_to_vm_value_(clx_L, L, err);
        return lua_error(L);
    }
}

}
