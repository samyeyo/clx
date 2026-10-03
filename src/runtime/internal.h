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

//------------------ kMaxPooledFibers: cap on recycled fiber stacks kept by the thread pool
inline constexpr size_t kMaxPooledFibers = 2048;

}
