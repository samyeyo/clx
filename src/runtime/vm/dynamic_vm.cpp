// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  dynamic_vm.cpp · Embedded Lua 5.5 VM       │
// └─────────────────────────────────────────────┘

#include "dynamic_vm.h"
#include "vm_convert.h"
#include "vm_function_object.h"
#include "vm_native_bridge.h"
#include "vm_table_proxy.h"

#include <clx_simd.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
int clx_vm_env_index(lua_State* L);
int clx_vm_env_newindex(lua_State* L);
int clx_vm_env_pairs(lua_State* L);
int clx_vm_env_pairs_iter(lua_State* L);
int clx_vm_env_collectgarbage(lua_State* L);
int clx_vm_type(lua_State* L);
int clx_vm_rawget(lua_State* L);
int clx_vm_rawset(lua_State* L);
int clx_vm_next(lua_State* L);
int clx_vm_rawlen(lua_State* L);
}

namespace clx {

void clx_mark_vm_proxies_(LState* clx_L, LState::GCStack& wl);

//------------------ clx_register_vm_proxy - link a VM proxy into clx GC
void clx_register_vm_proxy(LState* clx_L, LHeader* proxy, size_t bytes)
{
    //------------------ a wrapper is often the only allocation a crossing makes, so it drives the GC pacing itself
    clx_L->gc_maybe_collect();
    proxy->next = clx_L->allocated_objects;
    clx_L->allocated_objects = proxy;
    clx_L->gc_recent.push(proxy);
    clx_L->object_count++;
    clx_L->allocated_bytes += bytes;

    DynamicVM* vm = DynamicVM::find(clx_L);
    if (vm)
        vm->pace_vm_gc();
}

//------------------ clx_free_vm_proxy_ - free a VM proxy collected by clx GC
static void clx_free_vm_proxy_(LState* clx_L, LHeader* proxy)
{
    if (proxy->type == static_cast<uint8_t>(Table)) {
        clx_L->allocated_bytes -= sizeof(VMTableProxy);
        delete static_cast<VMTableProxy*>(proxy);
    } else {
        clx_L->allocated_bytes -= sizeof(VMFunction);
        delete static_cast<VMFunction*>(proxy);
    }
}

//------------------ clx_table_release_: drop a converted table's VM registry reference as the table is recycled
static void clx_table_release_(LState* clx_L, LTable* t)
{
    if (!t->ext || t->ext->vm_origin < 0)
        return;
    DynamicVM* vm = DynamicVM::find(clx_L);
    if (vm && vm->lua_L())
        luaL_unref(vm->lua_L(), LUA_REGISTRYINDEX, t->ext->vm_origin);
    t->ext->vm_origin = -1;
}

namespace {

    std::unordered_map<LState*, DynamicVM*> g_dynamic_vms_;

}

//------------------ DynamicVM::DynamicVM
DynamicVM::DynamicVM(LState* clx_L)
    : clx_L_(clx_L)
    , L_(nullptr)
    , registry_root_(LUA_NOREF)
    , env_table_ref_(LUA_NOREF)
    , opened_libs_(false)
{
    L_ = luaL_newstate();
    if (!L_)
        throw_runtime_error("failed to create embedded Lua 5.5 vm_state");
    luaL_openlibs(L_);

    lua_gc(L_, LUA_GCRESTART);
    lua_gc(L_, LUA_GCGEN);
    opened_libs_ = true;
    //------------------ registered before the environment is built: a pushed clx table must find this VM, not build another
    g_dynamic_vms_[clx_L] = this;
    try {
        build_parking_lot();
        env_table_ref_ = build_default_env();
        build_bridge_cache();
        build_object_index();
    } catch (...) {
        g_dynamic_vms_.erase(clx_L);
        throw;
    }

    clx_mark_vm_proxies_ptr = clx_mark_vm_proxies_;
    clx_free_vm_proxy_ptr = clx_free_vm_proxy_;
    clx_table_release_ptr = clx_table_release_;
}

//------------------ DynamicVM::~DynamicVM
DynamicVM::~DynamicVM()
{
    bridge_head_ = nullptr;
    proxy_head_ = nullptr;

    if (L_) {

        if (registry_root_ != LUA_NOREF) {
            lua_pushnil(L_);
            lua_rawseti(L_, LUA_REGISTRYINDEX, registry_root_);
        }
        lua_close(L_);
        L_ = nullptr;
    }
}

//------------------ DynamicVM::default_env_registry_ref
int DynamicVM::default_env_registry_ref() const
{
    return env_table_ref_;
}

//------------------ DynamicVM::build_parking_lot
void DynamicVM::build_parking_lot()
{
    lua_newtable(L_);
    registry_root_ = luaL_ref(L_, LUA_REGISTRYINDEX);
}

//------------------ DynamicVM::build_default_env: the VM's own globals — _G is itself, missing names reach clx's globals
int DynamicVM::build_default_env()
{
    lua_pushglobaltable(L_);

    lua_pushstring(L_, "_G");
    lua_pushvalue(L_, -2);
    lua_rawset(L_, -3);

    int g_idx = lua_gettop(L_);

    lua_newtable(L_);
    int mt_idx = lua_gettop(L_);

    //------------------ names written through __newindex, shared by the two shims
    lua_newtable(L_);
    int mirrored_idx = lua_gettop(L_);

    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_->_G));
    lua_pushinteger(L_, 1);
    lua_pushvalue(L_, mirrored_idx);
    lua_pushcclosure(L_, clx_vm_env_index, 4);
    lua_setfield(L_, mt_idx, "__index");

    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_->_G));
    lua_pushinteger(L_, 0);
    lua_pushinteger(L_, 1);
    lua_pushvalue(L_, mirrored_idx);
    lua_pushcclosure(L_, clx_vm_env_newindex, 5);
    lua_setfield(L_, mt_idx, "__newindex");

    //------------------ pairs walks the clx globals, where __newindex keeps every chunk-written name
    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_->_G));
    lua_pushinteger(L_, 0);
    lua_pushcclosure(L_, clx_vm_env_pairs, 3);
    lua_setfield(L_, mt_idx, "__pairs");

    //------------------ a chunk asking for the environment's metatable gets a stand-in, so clearing __index keeps the fallback
    lua_newtable(L_);
    lua_setfield(L_, mt_idx, "__metatable");

    lua_pushvalue(L_, mt_idx);
    lua_setmetatable(L_, g_idx);
    lua_pop(L_, 2);

    //------------------ shims only widen the stock functions - every non-proxy value takes the stock path
    {
        static const struct {
            const char* name;
            lua_CFunction fn;
        } table_globals[] = { { "rawget", clx_vm_rawget }, { "rawset", clx_vm_rawset },
            { "next", clx_vm_next }, { "rawlen", clx_vm_rawlen } };
        for (const auto& g : table_globals) {
            lua_pushstring(L_, g.name);
            lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
            lua_pushcclosure(L_, g.fn, 1);
            lua_rawset(L_, -3);
        }
        lua_pushstring(L_, "type");
        lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
        lua_pushcclosure(L_, clx_vm_type, 1);
        lua_rawset(L_, -3);
    }

    //------------------ a chunk's collectgarbage must drive both collectors; the stock global is kept for the forwarded call
    {
        lua_getglobal(L_, "collectgarbage");
        gc_fn_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
        lua_pushstring(L_, "collectgarbage");
        lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
        lua_pushinteger(L_, gc_fn_ref_);
        lua_pushcclosure(L_, clx_vm_env_collectgarbage, 2);
        lua_rawset(L_, -3);
    }

    //------------------ modules load through clx: chunks get clx's require and package table (one shared path/loaded state)
    {
        //------------------ the VM's own package.loaded is captured before the shared table replaces it, for the real std libraries
        lua_getfield(L_, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
        int vm_loaded_ref = luaL_ref(L_, LUA_REGISTRYINDEX);

        //------------------ LUA_LOADED_TABLE stays the VM's own table: pushglobalfuncname walks it with lua_next
        LValue pack = get_global(clx_L_, "package");
        lua_pushglobaltable(L_);
        if (pack.type == Table) {
            clx_to_vm_value_(clx_L_, L_, pack);
            lua_setfield(L_, -2, "package");
        }
        LValue req = get_global(clx_L_, "require");
        if (req.type == Function) {
            lua_pushinteger(L_, vm_loaded_ref);
            clx_to_vm_value_(clx_L_, L_, req);
            lua_pushcclosure(L_, clx_vm_require, 2);
            lua_setfield(L_, -2, "require");
        }
        lua_pop(L_, 1);
    }

    int ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    return ref;
}

//------------------ DynamicVM::build_env_table
int DynamicVM::build_env_table(const LValue& preferred)
{
    if (preferred.type == Table && preferred.as_pointer()) {
        //------------------ a VM table is already a usable environment; wrapping it would hide its own __index/__newindex
        LHeader* h = preferred.as_pointer();
        if (h->flags & LFLAG_VM_PROXY) {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, static_cast<VMTableProxy*>(h)->registry_ref);
            return luaL_ref(L_, LUA_REGISTRYINDEX);
        }

        lua_newtable(L_);
        LTable* t = static_cast<LTable*>(preferred.as_pointer());
        lua_newtable(L_);

        lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
        lua_pushlightuserdata(L_, static_cast<void*>(t));
        lua_pushinteger(L_, 0);
        lua_pushnil(L_);
        lua_pushcclosure(L_, clx_vm_env_index, 4);
        lua_setfield(L_, -2, "__index");

        lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
        lua_pushlightuserdata(L_, static_cast<void*>(t));
        lua_pushinteger(L_, 0);
        lua_pushinteger(L_, 0);
        lua_pushnil(L_);
        lua_pushcclosure(L_, clx_vm_env_newindex, 5);
        lua_setfield(L_, -2, "__newindex");

        lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
        lua_pushlightuserdata(L_, static_cast<void*>(t));
        lua_pushinteger(L_, 0);
        lua_pushcclosure(L_, clx_vm_env_pairs, 3);
        lua_setfield(L_, -2, "__pairs");
        lua_setmetatable(L_, -2);

        for (size_t i = 0; i < t->array_size; ++i) {
            if (t->array_types[i] != Nil) {
                LValue key(static_cast<int64_t>(i + 1));
                LValue val(t->array[i], t->array_types[i]);
                clx_to_vm_value_(clx_L_, L_, val);
                lua_rawseti(L_, -2, static_cast<int>(i + 1));
            }
        }
        if (t->ext && t->ext->hash_bitmap) {
            size_t bm_words = (t->ext->hash_size + 63) / 64;
            for (size_t word = 0; word < bm_words; ++word) {
                uint64_t bits = t->ext->hash_bitmap[word];
                while (bits) {
                    size_t i = word * 64 + clx_ctzll(bits);
                    if (i >= t->ext->hash_size)
                        break;
                    if (t->ext->entries[i].ktype != Nil) {
                        LValue key(t->ext->entries[i].key, t->ext->entries[i].ktype);
                        LValue val(t->ext->entries[i].val, t->ext->entries[i].vtype);
                        clx_to_vm_value_(clx_L_, L_, key);
                        clx_to_vm_value_(clx_L_, L_, val);
                        lua_rawset(L_, -3);
                    }
                    bits &= bits - 1;
                }
            }
        } else if (t->ext) {
            for (size_t i = 0; i < t->ext->hash_size; ++i) {
                if (t->ext->entries[i].ktype != Nil) {
                    LValue key(t->ext->entries[i].key, t->ext->entries[i].ktype);
                    LValue val(t->ext->entries[i].val, t->ext->entries[i].vtype);
                    clx_to_vm_value_(clx_L_, L_, key);
                    clx_to_vm_value_(clx_L_, L_, val);
                    lua_rawset(L_, -3);
                }
            }
        }
        return luaL_ref(L_, LUA_REGISTRYINDEX);
    }

    lua_rawgeti(L_, LUA_REGISTRYINDEX, env_table_ref_);
    return luaL_ref(L_, LUA_REGISTRYINDEX);
}

//------------------ DynamicVM::build_object_index: the weak-valued VM-object -> clx-wrapper index, rooted
void DynamicVM::build_object_index()
{
    LValue index = clx_L_->create_table();
    RootRange index_rr(clx_L_, &index, 1);
    LValue weak = clx_L_->create_table();
    RootRange weak_rr(clx_L_, &weak, 1);
    static_cast<LTable*>(weak.as_pointer())->settable(
        LValue(clx_L_->intern_string("__mode")), LValue::istr("v", 1));
    LTable* index_tbl = static_cast<LTable*>(index.as_pointer());
    tbl_set_metatable(index_tbl, static_cast<LTable*>(weak.as_pointer()));
    //------------------ the collector only runs the weak pass over tables on its metatabled list, so the index joins it
    meta_list_add(clx_L_, index_tbl);
    gc_barrier(clx_L_, index_tbl, weak);
    clx_L_->root_value(index);
    object_index_ = index;
}

//------------------ DynamicVM::object_index_get - the clx value recorded for a VM object, or nil
LValue DynamicVM::object_index_get(const void* vm_obj) const
{
    if (object_index_.type != Table || !object_index_.as_pointer())
        return LValue();
    LTable* index = static_cast<LTable*>(object_index_.as_pointer());
    return index->gettable(LValue(static_cast<int64_t>(reinterpret_cast<uintptr_t>(vm_obj))));
}

//------------------ DynamicVM::object_index_put - record the clx value made from a VM object
void DynamicVM::object_index_put(const void* vm_obj, const LValue& value)
{
    if (object_index_.type != Table || !object_index_.as_pointer() || !value.is_gc_obj())
        return;
    LTable* index = static_cast<LTable*>(object_index_.as_pointer());
    int64_t key = static_cast<int64_t>(reinterpret_cast<uintptr_t>(vm_obj));
    gc_barrier_table(clx_L_, index, value);
    index->settable(LValue(key), value);
}

//------------------ DynamicVM::build_bridge_cache: weak-valued by clx callable address, so one function maps to one closure
void DynamicVM::build_bridge_cache()
{
    lua_newtable(L_);
    lua_newtable(L_);
    lua_pushstring(L_, "v");
    lua_setfield(L_, -2, "__mode");
    lua_setmetatable(L_, -2);
    bridge_cache_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
}

//------------------ DynamicVM::load_buffer
bool DynamicVM::load_buffer(const char* src, size_t len, const char* name, const char* mode_str, int env_ref,
    int* out_ref, std::string* error_msg)
{
    int rc = luaL_loadbufferx(L_, src, len, name ? name : "[chunk]", mode_str);
    if (rc != LUA_OK) {
        if (error_msg) {
            const char* msg = lua_tostring(L_, -1);
            *error_msg = msg ? msg : "<unknown>";
        }
        lua_pop(L_, 1);
        return false;
    }
    if (env_ref != LUA_NOREF) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, env_ref);
        //------------------ lua_setupvalue only pops when upvalue #1 exists; a chunk with no upvalues must not leave env behind
        if (!lua_setupvalue(L_, -2, 1))
            lua_pop(L_, 1);
    }
    *out_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    return true;
}

//------------------ DynamicVM::load_file
bool DynamicVM::load_file(const char* path, const char* mode_str, int env_ref, int* out_ref, std::string* error_msg)
{
    int rc = luaL_loadfilex(L_, path, mode_str);
    if (rc != LUA_OK) {
        if (error_msg) {
            const char* msg = lua_tostring(L_, -1);
            *error_msg = msg ? msg : "<unknown>";
        }
        lua_pop(L_, 1);
        return false;
    }
    if (env_ref != LUA_NOREF) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, env_ref);
        //------------------ same no-upvalue guard as load_buffer: never ref the env table itself
        if (!lua_setupvalue(L_, -2, 1))
            lua_pop(L_, 1);
    }
    *out_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    return true;
}

//------------------ DynamicVM::capture_error
LValue DynamicVM::capture_error()
{
    const char* msg = lua_tostring(L_, -1);
    if (!msg)
        return LValue();
    LValue v = clx_L_->intern_lvalue(msg, std::strlen(msg));
    lua_pop(L_, 1);
    return v;
}

//------------------ DynamicVM::acquire
DynamicVM* DynamicVM::acquire(LState* clx_L)
{
    auto it = g_dynamic_vms_.find(clx_L);
    if (it != g_dynamic_vms_.end())
        return it->second;
    DynamicVM* vm = new DynamicVM(clx_L);
    g_dynamic_vms_[clx_L] = vm;
    return vm;
}

//------------------ DynamicVM::release
void DynamicVM::release(LState* clx_L)
{
    auto it = g_dynamic_vms_.find(clx_L);
    if (it == g_dynamic_vms_.end())
        return;

    if (it->second->object_index_.is_gc_obj())
        clx_L->unroot_value(it->second->object_index_);

    //------------------ every registry reference dies with this VM: clear each live table's origin so no later VM gets it
    for (int pass = 0; pass < 2; ++pass) {
        for (LHeader* h = (pass == 0) ? clx_L->allocated_objects : clx_L->old_objects; h; h = h->next) {
            if (h->type == static_cast<uint8_t>(Table) && !(h->flags & LFLAG_VM_PROXY)) {
                LTable* t = static_cast<LTable*>(h);
                if (t->ext)
                    t->ext->vm_origin = -1;
            }
        }
    }

    ProxyNode* n = it->second->proxy_head_;
    while (n) {
        ProxyNode* nx = n->next;
        n->prev = nullptr;
        n->next = nullptr;
        n->header = nullptr;
        n = nx;
    }
    it->second->proxy_head_ = nullptr;
    BridgeNode* b = it->second->bridge_head_;
    while (b) {
        BridgeNode* bn = b->next;
        b->prev = nullptr;
        b->next = nullptr;
        b = bn;
    }
    it->second->bridge_head_ = nullptr;
    delete it->second;
    g_dynamic_vms_.erase(it);
}

//------------------ DynamicVM::find - non-lazy lookup
DynamicVM* DynamicVM::find(LState* clx_L)
{
    auto it = g_dynamic_vms_.find(clx_L);
    return (it != g_dynamic_vms_.end()) ? it->second : nullptr;
}

//------------------ DynamicVM::pace_vm_gc - throttle embedded VM GC
void DynamicVM::pace_vm_gc()
{
    if (L_ && ++proxy_alloc_since_gc_ >= PROXY_GC_PACE_THRESHOLD) {
        proxy_alloc_since_gc_ = 0;
        lua_gc(L_, LUA_GCSTEP, 200);
    }
}

//------------------ DynamicVM::link_proxy - O(1) head insert
void DynamicVM::link_proxy(ProxyNode* node)
{
    if (!node)
        return;
    node->prev = nullptr;
    node->next = proxy_head_;
    if (proxy_head_)
        proxy_head_->prev = node;
    proxy_head_ = node;
    pace_vm_gc();
    if (clx_L_)
        clx_L_->gc_step();
}

//------------------ DynamicVM::unlink_proxy - O(1) pointer surgery
void DynamicVM::unlink_proxy(ProxyNode* node)
{
    if (!node)
        return;
    if (node->prev)
        node->prev->next = node->next;
    else if (proxy_head_ == node)
        proxy_head_ = node->next;
    if (node->next)
        node->next->prev = node->prev;
    node->prev = nullptr;
    node->next = nullptr;
}

//------------------ DynamicVM::link_bridge - O(1) head insert
void DynamicVM::link_bridge(BridgeNode* node)
{
    if (!node)
        return;
    node->prev = nullptr;
    node->next = bridge_head_;
    if (bridge_head_)
        bridge_head_->prev = node;
    bridge_head_ = node;
}

//------------------ DynamicVM::unlink_bridge - O(1) pointer surgery
void DynamicVM::unlink_bridge(BridgeNode* node)
{
    if (!node)
        return;
    if (node->prev)
        node->prev->next = node->next;
    else if (bridge_head_ == node)
        bridge_head_ = node->next;
    if (node->next)
        node->next->prev = node->prev;
    node->prev = nullptr;
    node->next = nullptr;
}

//------------------ DynamicVM::mark_proxy_roots: only an LFLAG_VM_PROXY table is a VMTableProxy; LTables stay plain
void DynamicVM::mark_proxy_roots(LState::GCStack& wl)
{
    for (BridgeNode* b = bridge_head_; b; b = b->next) {
        NativeBridge* nb = b->bridge;
        if (!nb || !nb->clx_callable.is_gc_obj())
            continue;
        LHeader* ch = nb->clx_callable.as_pointer();
        if (ch && ch->marked == 0) {
            ch->marked = 1;
            wl.push(ch);
        }
    }

    for (ProxyNode* n = proxy_head_; n; n = n->next) {
        LHeader* h = n->header;
        if (!h)
            continue;
        if (h->marked == 0) {
            h->marked = 1;
            wl.push(h);
        }
        if ((h->flags & LFLAG_VM_PROXY) && h->type == static_cast<uint8_t>(Table)) {
            VMTableProxy* tp = static_cast<VMTableProxy*>(h);
            if (tp->clx_underlying && tp->clx_underlying->marked == 0) {
                tp->clx_underlying->marked = 1;
                wl.push(reinterpret_cast<LHeader*>(tp->clx_underlying));
            }
        }
    }
}

//------------------ clx_mark_vm_proxies_ - free-function facade
void clx_mark_vm_proxies_(LState* clx_L, LState::GCStack& wl)
{
    DynamicVM* vm = DynamicVM::find(clx_L);
    if (!vm)
        return;
    vm->mark_proxy_roots(wl);
}

//------------------ gc_option_index: the option tables of luaB_collectgarbage, so a bad option is reported by the shim
static int gc_option_index(const char* name, bool param)
{
    static const char* const opts[] = { "stop", "restart", "collect", "count", "step", "isrunning",
        "generational", "incremental", "param", nullptr };
    static const char* const params[]
        = { "minormul", "majorminor", "minormajor", "pause", "stepmul", "stepsize", nullptr };
    const char* const* list = param ? params : opts;
    for (int i = 0; list[i]; ++i)
        if (strcmp(list[i], name) == 0)
            return i;
    return -1;
}

//------------------ clx_vm_env_collectgarbage: forwards to the VM's collectgarbage, then drives the clx collector
extern "C" int clx_vm_env_collectgarbage(lua_State* L)
{
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));
    int nargs = lua_gettop(L);

    int gc_ref = static_cast<int>(lua_tointeger(L, lua_upvalueindex(2)));

    //------------------ argument checking happens here, in the shim's own frame (see gc_option_index)
    if (!lua_isnoneornil(L, 1)) {
        if (lua_type(L, 1) != LUA_TSTRING)
            return luaL_typeerror(L, 1, "string");
        const char* name = lua_tostring(L, 1);
        if (gc_option_index(name, false) < 0)
            return luaL_argerror(L, 1, lua_pushfstring(L, "invalid option '%s'", name));
        if (strcmp(name, "param") == 0) {
            if (lua_isnoneornil(L, 2))
                return luaL_argerror(L, 2, "string expected, got no value");
            if (lua_type(L, 2) != LUA_TSTRING)
                return luaL_typeerror(L, 2, "string");
            const char* pname = lua_tostring(L, 2);
            if (gc_option_index(pname, true) < 0)
                return luaL_argerror(L, 2, lua_pushfstring(L, "invalid option '%s'", pname));
        }
    }

    //------------------ copy the option first: the pcall below replaces the argument slots with results
    std::string opt_s;
    if (nargs >= 1 && lua_type(L, 1) == LUA_TSTRING) {
        size_t len = 0;
        const char* s = lua_tolstring(L, 1, &len);
        opt_s.assign(s, len);
    }
    const char* opt = opt_s.empty() ? nullptr : opt_s.c_str();

    //------------------ the saved reference holds the VM's own collectgarbage (luaB_collectgarbage)
    lua_rawgeti(L, LUA_REGISTRYINDEX, gc_ref);
    lua_insert(L, 1);
    if (lua_pcallk(L, nargs, LUA_MULTRET, 0, 0, nullptr) != LUA_OK)
        return lua_error(L);
    int nres = lua_gettop(L);

    if (clx_L) {
        if (nargs == 0 || (opt && strcmp(opt, "collect") == 0)) {
            clx_L->collect_garbage();
        } else if (opt && strcmp(opt, "stop") == 0) {
            clx_L->gc_running = false;
        } else if (opt && strcmp(opt, "restart") == 0) {
            clx_L->gc_running = true;
        } else if (opt && strcmp(opt, "step") == 0) {
            clx_L->gc_step();
        } else if (opt && strcmp(opt, "incremental") == 0) {
            clx_L->gc_mode = clx::LState::GCMode::Incremental;
        } else if (opt && strcmp(opt, "generational") == 0) {
            clx_L->gc_mode = clx::LState::GCMode::Generational;
        }
    }

    return nres;
}

}
