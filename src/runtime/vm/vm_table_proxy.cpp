// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  vm_table_proxy.cpp · VMTableProxy Drain    │
// └─────────────────────────────────────────────┘

#include "vm_table_proxy.h"
#include "vm_convert.h"
#include <clx.h>

namespace clx {

//------------------ VMTableProxy::VMTableProxy - table proxy constructor
VMTableProxy::VMTableProxy(DynamicVM *vm_, int ref, LTable *clx_under)
    : vm(vm_)
    , registry_ref(ref)
    , clx_underlying(clx_under) {
    type = static_cast<uint8_t>(Table);
    marked = 0;
    flags = LFLAG_VM_PROXY;
    next = nullptr;
}

//------------------ VMTableProxy::~VMTableProxy - release the registry reference
VMTableProxy::~VMTableProxy() {
    if (vm && registry_ref != LUA_NOREF) {
        luaL_unref(vm->lua_L(), LUA_REGISTRYINDEX, registry_ref);
        registry_ref = LUA_NOREF;
    }
}

//------------------ VMTableProxy::wrap - wrap a clx table for the VM
LValue VMTableProxy::wrap(LState *clx_L, const LValue &src) {
    DynamicVM *vm = DynamicVM::acquire(clx_L);
    lua_State *L = vm->lua_L();

    lua_newtable(L);

    if (src.type == Table && src.as_pointer()) {
        LTable *src_tbl = static_cast<LTable *>(src.as_pointer());
        for (size_t i = 0; i < src_tbl->array_size; ++i) {
            if (src_tbl->array_types[i] != Nil) {
                LValue key(static_cast<int64_t>(i + 1));
                LValue val(src_tbl->array[i], src_tbl->array_types[i]);
                clx_to_vm_value_(clx_L, L, val);
                lua_rawseti(L, -2, static_cast<int>(i + 1));
            }
        }
    }

    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    VMTableProxy *proxy
        = new VMTableProxy(vm, ref, src.type == Table ? static_cast<LTable *>(src.as_pointer()) : nullptr);
    clx_register_vm_proxy(clx_L, proxy, sizeof(VMTableProxy));
    return LValue(Table, proxy);
}

//------------------ VMTableProxy::wrap - wrap a Lua table for clx
LValue VMTableProxy::wrap(LState *clx_L, lua_State *L, int idx) {
    DynamicVM *vm = DynamicVM::acquire(clx_L);
    lua_pushvalue(L, idx);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    VMTableProxy *proxy = new VMTableProxy(vm, ref);
    clx_register_vm_proxy(clx_L, proxy, sizeof(VMTableProxy));
    return LValue(Table, proxy);
}

//------------------ vm_to_clx_table_ - convert a Lua table to a clx table proxy
LValue vm_to_clx_table_(LState *clx_L, lua_State *L, int idx) {
    return VMTableProxy::wrap(clx_L, L, idx);
}

}
