// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  vm_function_object.h · VMFunction Wrapper  │
// └─────────────────────────────────────────────┘

#ifndef CLX_VM_FUNCTION_OBJECT_H
#define CLX_VM_FUNCTION_OBJECT_H

#include "dynamic_vm.h"

namespace clx {

//------------------ VMFunction - a clx LValue that wraps a Lua VM closure
struct VMFunction : public LCFunction {
    DynamicVM *vm;
    int registry_ref;

    VMFunction(DynamicVM *vm_, int ref);
    ~VMFunction();

    static LValue wrap(LState *clx_L, lua_State *L, int idx);

    static LValue from_ref(LState *clx_L, int ref);

    MultiValue invoke(LState *clx_L, const LValue *args, size_t nargs);
};

//------------------ vm_to_clx_function_ - conversion helper (does the
LValue vm_to_clx_function_(LState *clx_L, lua_State *L, int stack_idx);

bool vm_pcall_function_(LState *clx_L, int lua_ref, const LValue *args, size_t nargs, MultiValue &out);

}

#endif
