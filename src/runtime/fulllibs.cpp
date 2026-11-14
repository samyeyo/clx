// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  fulllibs.cpp · Full standard library opener│
// └─────────────────────────────────────────────┘

//------------------ Separate TU from openlibs.cpp on purpose: archive-member granularity guarantees
//------------------ minimal links never pull openlibs() (and its references to every luastd_* module)
//------------------ even on toolchains without function-sections/LTO dead-stripping (MinGW, MSVC w/o /Gy,
//------------------ --debug builds). Only full-build mains call openlibs(); the driver links all module
//------------------ archives for those.

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
    luastd_debug(L);
    openlibs_minimal(L);
}
}
