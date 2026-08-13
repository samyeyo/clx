// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  vm_convert.h · Value Conversion Layer      │
// └─────────────────────────────────────────────┘

#ifndef CLX_VM_CONVERT_H
#define CLX_VM_CONVERT_H

#include <clx_runtime.h>
#include <vector>

struct lua_State;

namespace clx {

void clx_to_vm_value_(LState* clx_L, lua_State* L, const LValue& v);

LValue vm_to_clx_value_(LState* clx_L, lua_State* L, int idx);

LValue clx_vm_capture_error_(LState* clx_L, lua_State* L);

int clx_vm_install_traceback_(lua_State* L);

extern void (*clx_mark_vm_proxies_ptr)(LState* clx_L, std::vector<LHeader*>& wl);

void clx_register_vm_proxy(LState* clx_L, LHeader* proxy, size_t bytes);

extern void (*clx_free_vm_proxy_ptr)(LState* clx_L, LHeader* proxy);

}

extern "C" {

int clx_vm_env_index(lua_State* L);
int clx_vm_env_newindex(lua_State* L);
int clx_vm_env_pairs(lua_State* L);
int clx_vm_env_pairs_iter(lua_State* L);

int clx_vm_proxy_index(lua_State* L);
int clx_vm_proxy_newindex(lua_State* L);
int clx_vm_proxy_pairs(lua_State* L);
int clx_vm_proxy_pairs_iter(lua_State* L);
int clx_vm_proxy_len(lua_State* L);

//------------------ clx_vm_proxy_gc - pure pointer-surgery unlink from DynamicVM's proxy list
int clx_vm_proxy_gc(lua_State* L);
}

#endif
