// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  vm_function_object.cpp · VMFunction Drain  │
// └─────────────────────────────────────────────┘

#include "vm_function_object.h"
#include "vm_convert.h"
#include <cstring>

namespace clx {

//------------------ VMFunction::VMFunction
VMFunction::VMFunction(DynamicVM* vm_, int ref)
    : LCFunction([this](LState* clx_L, const LValue* args, size_t nargs) { return this->invoke(clx_L, args, nargs); })
    , vm(vm_)
    , registry_ref(ref)
{
    type = static_cast<uint8_t>(Function);
    marked = 0;
    flags = LFLAG_VM_PROXY;
    next = nullptr;
}

//------------------ VMFunction::~VMFunction - release the registry ref
VMFunction::~VMFunction()
{
    if (vm && registry_ref != LUA_NOREF) {

        luaL_unref(vm->lua_L(), LUA_REGISTRYINDEX, registry_ref);
        registry_ref = LUA_NOREF;
    }
}

//------------------ VMFunction::wrap - push aside Lua stack and ref
LValue VMFunction::wrap(LState* clx_L, lua_State* L, int idx)
{
    DynamicVM* vm = DynamicVM::acquire(clx_L);

    lua_pushvalue(L, idx);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    VMFunction* vf = new VMFunction(vm, ref);
    clx_register_vm_proxy(clx_L, vf, sizeof(VMFunction));
    return LValue(Function, vf);
}

//------------------ VMFunction::from_ref - already a registry ref
LValue VMFunction::from_ref(LState* clx_L, int ref)
{
    DynamicVM* vm = DynamicVM::acquire(clx_L);
    VMFunction* vf = new VMFunction(vm, ref);
    clx_register_vm_proxy(clx_L, vf, sizeof(VMFunction));
    return LValue(Function, vf);
}

//------------------ VMFunction::invoke - clx calls this
MultiValue VMFunction::invoke(LState* clx_L, const LValue* args, size_t nargs)
{

    bool inside_coro = (clx_L->running_thread && !clx_L->running_thread->is_main);
    if (inside_coro) {
        throw_runtime_error("cannot call Lua VM function from inside a clx coroutine: "
                            "use Lua VM's own coroutine library (copied to env table) "
                            "or call the function outside the coroutine");
    }

    MultiValue out;
    if (!vm_pcall_function_(clx_L, registry_ref, args, nargs, out)) {

        LValue err = (out.count > 0) ? out[0] : LValue(clx_L->intern_string("VM error"));
        throw LRuntimeException(err);
    }
    return out;
}

//------------------ vm_to_clx_function_ - side door for convert.cpp
LValue vm_to_clx_function_(LState* clx_L, lua_State* L, int stack_idx)
{
    return VMFunction::wrap(clx_L, L, stack_idx);
}

//------------------ vm_pcall_function_ - generic clx-to-VM dispatch
bool vm_pcall_function_(LState* clx_L, int lua_ref, const LValue* args, size_t nargs, MultiValue& out)
{
    DynamicVM* vm = DynamicVM::acquire(clx_L);
    lua_State* L = vm->lua_L();

    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_ref);
    int base = lua_gettop(L);

    for (size_t i = 0; i < nargs; ++i)
        clx_to_vm_value_(clx_L, L, args[i]);

    int rc = lua_pcallk(L, static_cast<int>(nargs), LUA_MULTRET, 0, 0, nullptr);
    if (rc != LUA_OK) {

        LValue err = clx_vm_capture_error_(clx_L, L);
        out = MultiValue(err);
        return false;
    }

    int top = lua_gettop(L);
    int results = top - (base - 1);
    if (results == 0) {
        out = MultiValue();
        return true;
    }
    if (results == 1) {
        out = MultiValue(vm_to_clx_value_(clx_L, L, base));
        return true;
    }
    std::vector<LValue> tmp(static_cast<size_t>(results));
    for (int i = 0; i < results; ++i)
        tmp[i] = vm_to_clx_value_(clx_L, L, base + i);
    out = MultiValue(tmp.data(), tmp.size(), clx_L);
    return true;
}

}
