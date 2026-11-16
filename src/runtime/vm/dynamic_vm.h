// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  dynamic_vm.h · Embedded Lua 5.5 VM         │
// └─────────────────────────────────────────────┘

#ifndef CLX_VM_DYNAMIC_H
#define CLX_VM_DYNAMIC_H

#include <clx_runtime.h>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace clx {

//------------------ ProxyNode - intrusive doubly-linked list node
struct ProxyNode {
    LHeader* header;
    ProxyNode* prev;
    ProxyNode* next;
};

struct NativeBridge;

//------------------ BridgeNode - intrusive list node for live NativeBridge callables
struct BridgeNode {
    NativeBridge* bridge;
    BridgeNode* prev;
    BridgeNode* next;
};

//------------------ DynamicVM - one per clx::LState
class DynamicVM {
public:
    explicit DynamicVM(LState* clx_L);
    ~DynamicVM();

    DynamicVM(const DynamicVM&) = delete;
    DynamicVM& operator=(const DynamicVM&) = delete;

    bool load_buffer(const char* src, size_t len, const char* name, const char* mode_str, int env_ref, int* out_ref,
        std::string* error_msg);

    bool load_file(const char* path, const char* mode_str, int env_ref, int* out_ref, std::string* error_msg);

    LValue capture_error();

    lua_State* lua_L() const { return L_; }

    LState* clx_L() const { return clx_L_; }

    static DynamicVM* acquire(LState* clx_L);
    static void release(LState* clx_L);

    int build_env_table(const LValue& preferred);
    int default_env_registry_ref() const;

    int bridge_cache_registry_ref() const { return bridge_cache_ref_; }

    //------------------ object_index -- clx table mapping a VM object (a table or closure, as its pointer)
    //------------------ to the clx value already made from it, weakly held: the collector drops an entry
    //------------------ when its clx value dies, so an object crossing back finds its own wrapper again
    //------------------ and a dead one is never handed out
    LValue object_index_get(const void* vm_obj) const;
    void object_index_put(const void* vm_obj, const LValue& value);

    static DynamicVM* find(LState* clx_L);

    //------------------ Proxy list maintenance
    void link_proxy(ProxyNode* node);
    void unlink_proxy(ProxyNode* node);

    void mark_proxy_roots(struct LState::GCStack& wl);

    //------------------ Bridge list maintenance (keeps clx callables alive)
    void link_bridge(BridgeNode* node);
    void unlink_bridge(BridgeNode* node);

    void pace_vm_gc();

    static constexpr unsigned PROXY_GC_PACE_THRESHOLD = 32;

private:
    LState* clx_L_;
    lua_State* L_;
    int registry_root_;
    int env_table_ref_;
    int bridge_cache_ref_ = LUA_NOREF;
    int gc_fn_ref_ = LUA_NOREF;
    LValue object_index_ = LValue();
    bool opened_libs_;
    ProxyNode* proxy_head_ = nullptr;
    BridgeNode* bridge_head_ = nullptr;
    unsigned proxy_alloc_since_gc_ = 0;

    void build_parking_lot();
    int build_default_env();
    void build_bridge_cache();
    void build_object_index();
};

}

#endif
