// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  internal.h · Shared internal runtime state │
// └─────────────────────────────────────────────┘

#pragma once

#include "clx_runtime.h"

namespace clx {

//------------------ register_static_preload: registers a static preload loader for a module
void register_static_preload(LState *L, const char *name, LValue(open_func)(LState *));

//------------------ luaapi_free_state: releases the Lua 5.5 C API bundle owned by this state (provided by libclx_capi; reached from ~LState through LState::luaapi_cleanup)
void luaapi_free_state(LState *L);

//------------------ luaapi_reset_thread_bundle: drops a recycled coroutine's C API frames (provided by libclx_capi; reached from create_thread through LState::luaapi_thread_reset)
void luaapi_reset_thread_bundle(LState *L, void *bundle);

//------------------ kMaxPooledFibers: cap on recycled fiber stacks kept by the thread pool
inline constexpr size_t kMaxPooledFibers = 2048;

}
