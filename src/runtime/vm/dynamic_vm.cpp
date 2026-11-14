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
    //------------------ registered before the environment is built: pushing a clx table into the VM
    //------------------ allocates a proxy through acquire(), which must find this VM, not build another
    g_dynamic_vms_[clx_L] = this;
    try {
        build_parking_lot();
        env_table_ref_ = build_default_env();
        build_bridge_cache();
    } catch (...) {
        g_dynamic_vms_.erase(clx_L);
        throw;
    }

    clx_mark_vm_proxies_ptr = clx_mark_vm_proxies_;
    clx_free_vm_proxy_ptr = clx_free_vm_proxy_;
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

//------------------ DynamicVM::build_default_env - the VM's own globals, so chunks see one real table:
//------------------ _G is itself, raw globals stay stock, and only missing names reach clx's globals
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

    //------------------ a chunk's collectgarbage must drive both collectors; keep the stock one for the
    //------------------ forwarded call, since overwriting the global makes lua_getglobal find the shim
    {
        lua_getglobal(L_, "collectgarbage");
        gc_fn_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
        lua_pushstring(L_, "collectgarbage");
        lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
        lua_pushinteger(L_, gc_fn_ref_);
        lua_pushcclosure(L_, clx_vm_env_collectgarbage, 2);
        lua_rawset(L_, -3);
    }

    //------------------ modules load through clx: chunks get clx's require and clx's package table, so
    //------------------ package.path/loaded are one state and a chunk that stores package.loaded[...]
    //------------------ itself is visible to the require() that is running it
    {
        //------------------ the VM's own package.loaded, captured before the shared table replaces it: a
        //------------------ chunk's require must still reach the VM's real standard libraries, notably
        //------------------ debug, whose introspection the compiled-code stub cannot serve
        lua_getfield(L_, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
        int vm_loaded_ref = luaL_ref(L_, LUA_REGISTRYINDEX);

        //------------------ LUA_LOADED_TABLE stays the VM's own table: the VM's internal name lookup
        //------------------ (pushglobalfuncname) walks it with lua_next, and a clx-table proxy there
        //------------------ crashes; modules resolve through the require shim below instead
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
        //------------------ a VM table is already a usable environment; wrapping it would hide its own
        //------------------ metatable (__index/__newindex) from the chunk
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

//------------------ DynamicVM::build_bridge_cache - weak-valued table keyed by clx callable address,
//------------------ so the same clx function always maps to the same VM closure (identity and %p)
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
        //------------------ lua_setupvalue only pops when upvalue #1 exists; a chunk with no
        //------------------ upvalues (e.g. a dumped function body referencing no globals) must not
        //------------------ leave the env table behind for luaL_ref to capture instead of the function
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

//------------------ DynamicVM::mark_proxy_roots - walk list into clx GC worklist. A proxied table is
//------------------ only a VMTableProxy when it carries LFLAG_VM_PROXY: clx tables handed to the VM
//------------------ (the _G environment wrapper, for one) are wrapped as plain LTables, so casting
//------------------ those to VMTableProxy reads past the object and marks a garbage clx_underlying.
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

//------------------ gc_option_index - the option tables of luaB_collectgarbage (src/lbaselib.c), kept here
//------------------ so a bad option is reported by the shim itself: raised inside the forwarded call it
//------------------ would lose both the 'collectgarbage' name and the caller's line to the extra C frame
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

//------------------ clx_vm_env_collectgarbage - forwards to the VM's collectgarbage so every option keeps
//------------------ stock semantics (isrunning, step, count, mode, param), then drives the clx collector too
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
