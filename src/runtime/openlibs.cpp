// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  openlibs.cpp · Standard Library Opener     │
// └─────────────────────────────────────────────┘

#include <clx_runtime.h>

namespace clx {

//------------------ openlibs: opens all standard Lua libraries
void openlibs(LState *L) {
    luastd_string(L);
    luastd_table(L);
    luastd_math(L);
    luastd_io(L);
    luastd_os(L);
    luastd_utf8(L);
    luastd_coroutine(L);
}
}
