// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  internal.h · Shared internal runtime state │
// └─────────────────────────────────────────────┘

#pragma once

#include "clx_runtime.h"
#include <cstdio>

namespace clx {

//------------------ io hooks: assigned by luastd_io when linked, null under --minimal (pointers live in state.cpp)
extern void (*clx_io_flush_all_ptr)();
extern void (*clx_io_write_ptr)(FILE *fp, const char *p, size_t n);

//------------------ string.dump hook: assigned by the embedded-VM bridge under --dynamic, null in plain builds
extern MultiValue (*clx_string_dump_hook)(LState *L, const LValue *args, size_t count);

//------------------ debug.getupvalue bridge: assigned by the embedded-VM layer under --dynamic, null in plain builds
extern bool (*clx_vm_debug_upvalue_hook)(LState *L, LHeader *fn, int64_t n, LValue &name, LValue &value);

//------------------ debug.getinfo bridge: assigned by the embedded-VM layer under --dynamic (AOT closures report false)
extern bool (*clx_vm_debug_getinfo_hook)(LState *L, const LValue &fn, const char *what, LValue &out);

//------------------ clx_table_release_ptr: drops a converted table's VM registry reference before it is recycled
extern void (*clx_table_release_ptr)(LState *L, LTable *t);

//------------------ register_static_preload: registers a static preload loader for a module
void register_static_preload(LState *L, const char *name, LValue(open_func)(LState *));

//------------------ luaapi_free_state: releases the Lua 5.5 C API bundle owned by this state (provided by libclx_capi; reached from ~LState through LState::luaapi_cleanup)
void luaapi_free_state(LState *L);

//------------------ luaapi_reset_thread_bundle: drops a recycled coroutine's C API frames (provided by libclx_capi; reached from create_thread through LState::luaapi_thread_reset)
void luaapi_reset_thread_bundle(LState *L, void *bundle);

//------------------ kMaxPooledFibers: cap on recycled fiber stacks kept by the thread pool
inline constexpr size_t kMaxPooledFibers = 2048;

}
