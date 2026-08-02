// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  vm_table_proxy.h · Forwarded Lua Tables    │
// └─────────────────────────────────────────────┘

#ifndef CLX_VM_TABLE_PROXY_H
#define CLX_VM_TABLE_PROXY_H

#include "dynamic_vm.h"

namespace clx {

//------------------ VMTableProxy - clx LValue that wraps a Lua VM table
struct VMTableProxy : public LHeader {
    DynamicVM *vm;
    int registry_ref;
    LTable *clx_underlying;

    VMTableProxy(DynamicVM *vm_, int ref, LTable *clx_under = nullptr);
    ~VMTableProxy();

    static LValue wrap(LState *clx_L, const LValue &src);
    static LValue wrap(LState *clx_L, lua_State *L, int idx);
};

LValue vm_to_clx_table_(LState *clx_L, lua_State *L, int idx);

}

#endif
