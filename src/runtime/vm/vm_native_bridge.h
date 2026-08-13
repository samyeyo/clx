// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  vm_native_bridge.h · Native Function Bridge│
// └─────────────────────────────────────────────┘

#ifndef CLX_VM_NATIVE_BRIDGE_H
#define CLX_VM_NATIVE_BRIDGE_H

#include "dynamic_vm.h"

namespace clx {

//------------------ NativeBridge - opaque handle exposed to Lua as a C closure
struct NativeBridge {
    DynamicVM* vm;
    LValue clx_callable;
    int registry_ref;
};

//------------------ create_bridge - wrap a clx LValue as a Lua C closure
NativeBridge* create_bridge(LState* clx_L, lua_State* L, const LValue& callable);

}

extern "C" {
int clx_vm_native_bridge_call(lua_State* L);
}

#endif
