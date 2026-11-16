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

//------------------ Optional-io hooks: assigned by luastd_io when the io module is linked in, null
//------------------ under --minimal (base + package + string only). Same pattern as
//------------------ clx_free_vm_proxy_ptr: the pointers live in always-linked state.cpp so referencing
//------------------ them never pulls io.o back in (which --gc-sections would otherwise drop).
//------------------ clx_io_write_ptr shares print/io.write ordering on stdout; every read/seek/flush/
//------------------ close path inside io.cpp flushes first, so ordering matches stock.
extern void (*clx_io_flush_all_ptr)();
extern void (*clx_io_write_ptr)(FILE *fp, const char *p, size_t n);

//------------------ string.dump hook: assigned by the embedded-VM bridge (libclx_lua) when linked
//------------------ with --dynamic, null otherwise. Same pattern as the io hooks: the pointer lives
//------------------ in always-linked state.cpp so referencing it never pulls vm code into plain builds.
//------------------ Takes over string.dump entirely when set (VM bytecode strings for VM functions,
//------------------ VM-side boxes for native closures); when null, string.dump throws unsupported.
extern MultiValue (*clx_string_dump_hook)(LState *L, const LValue *args, size_t count);

//------------------ debug.getupvalue bridge: assigned by the embedded-VM layer when linked with
//------------------ --dynamic so upvalue lookups on VM closures go to the Lua debug API instead of
//------------------ the clx closure cell array. Null (returns false) in plain builds.
extern bool (*clx_vm_debug_upvalue_hook)(LState *L, LHeader *fn, int64_t n, LValue &name, LValue &value);

//------------------ debug.getinfo bridge: assigned by the embedded-VM layer when linked with --dynamic, so
//------------------ a VM closure (a chunk loaded at run time) is described by the VM's own debug data. AOT
//------------------ closures have no line table, so the hook returns false for them and the caller errors.
extern bool (*clx_vm_debug_getinfo_hook)(LState *L, const LValue &fn, const char *what, LValue &out);

//------------------ clx_table_release_ptr: assigned by the embedded-VM bridge (libclx_lua) when linked with
//------------------ --dynamic, null otherwise. Called before a table is recycled, so the VM registry
//------------------ reference that a converted table holds on its source table is dropped with it. Same
//------------------ always-linked-pointer pattern as clx_free_vm_proxy_ptr.
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
