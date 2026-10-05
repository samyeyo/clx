// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  openlibs.cpp · Standard Library Opener     │
// └─────────────────────────────────────────────┘

#include <clx_runtime.h>

namespace clx {

//------------------ register_loaded_builtins: put stdlib tables into package.loaded (require() resolution)
static void register_loaded_builtins(LState *L) {
    LValue pkg = get_global(L, "package");
    if (pkg.type != ValueType::Table)
        return;
    LValue loaded = static_cast<LTable *>(pkg.as_pointer())->gettable(LValue(L->intern_string("loaded")));
    if (loaded.type != ValueType::Table)
        return;
    LTable *lt = static_cast<LTable *>(loaded.as_pointer());

    static constexpr const char *kModules[] = { "string", "table", "math", "io", "os", "utf8", "coroutine" };
    for (const char *name : kModules) {
        LValue lib = get_global(L, name);
        if (lib.type == ValueType::Table)
            lt->settable(LValue(L->intern_string(name)), lib);
    }
    lt->settable(LValue(L->intern_string("_G")), LValue(ValueType::Table, L->_G));
}

//------------------ openlibs: opens all standard Lua libraries
void openlibs(LState *L) {
    luastd_string(L);
    luastd_table(L);
    luastd_math(L);
    luastd_io(L);
    luastd_os(L);
    luastd_utf8(L);
    luastd_coroutine(L);
    register_loaded_builtins(L);
}

//------------------ openlibs_minimal: opens only the string library (--minimal keeps base + package + string)
void openlibs_minimal(LState *L) {
    luastd_string(L);
    register_loaded_builtins(L);
}
}
