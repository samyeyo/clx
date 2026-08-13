// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  load_builtin.cpp · load/loadfile/dofile    │
// └─────────────────────────────────────────────┘

#include "dynamic_vm.h"
#include "vm_convert.h"
#include "vm_function_object.h"
#include <clx.h>
#include <cstdio>
#include <fstream>

namespace clx {

//------------------ sync_vm_package_path - copy clx's package.path to Lua VM
static void sync_vm_package_path(LState* L, DynamicVM* vm)
{
    lua_State* luaL = vm->lua_L();
    if (!luaL)
        return;
    LValue pkg = get_global(L, "package");
    if (pkg.type != Table)
        return;
    LTable* pkg_tbl = static_cast<LTable*>(pkg.as_pointer());
    LValue path = pkg_tbl->gettable(L->intern_lvalue("path", 4));
    if (path.type != String)
        return;
    lua_getglobal(luaL, "package");
    if (!lua_istable(luaL, -1)) {
        lua_pop(luaL, 1);
        return;
    }
    lua_pushlstring(luaL, path.as_string(), path.string_len());
    lua_setfield(luaL, -2, "path");
    lua_pop(luaL, 1);
}

//------------------ clx_load - load([chunk [, chunkname [, mode [, env]]]])
static MultiValue clx_load(LState* L, const LValue* args, size_t count)
{
    DynamicVM* vm = DynamicVM::acquire(L);
    if (!vm) {
        return MultiValue(LValue(false), LValue(L->intern_string("dynamic loading disabled")));
    }
    sync_vm_package_path(L, vm);

    const char* src = nullptr;
    size_t len = 0;
    if (count > 0 && args[0].type != Nil) {
        LValue sv = args[0];
        if (sv.type != String)
            throw_runtime_error("bad argument #1 to 'load' (string expected)");
        src = sv.as_string();
        len = sv.string_len();
    } else {

        return MultiValue(LValue(false), LValue(L->intern_string("load(): no chunk given")));
    }

    const char* chunkname = nullptr;
    if (count > 1 && args[1].type != Nil) {
        if (args[1].type != String)
            throw_runtime_error("bad argument #2 to 'load' (string expected)");
        chunkname = args[1].as_string();
    }
    const char* mode = "bt";
    if (count > 2 && args[2].type != Nil) {
        if (args[2].type != String)
            throw_runtime_error("bad argument #3 to 'load' (string expected)");
        mode = args[2].as_string();
    }

    int env_ref = vm->default_env_registry_ref();
    if (count > 3 && args[3].type == Table) {
        env_ref = vm->build_env_table(args[3]);
    }

    int ref = LUA_NOREF;
    std::string err;
    if (!vm->load_buffer(src, len, chunkname, mode, env_ref, &ref, &err)) {
        return MultiValue(LValue(), LValue(L->intern_string(err.data(), err.size())));
    }
    return MultiValue(VMFunction::from_ref(L, ref));
}

//------------------ clx_loadfile - loadfile([path [, mode [, env]]])
static MultiValue clx_loadfile(LState* L, const LValue* args, size_t count)
{
    DynamicVM* vm = DynamicVM::acquire(L);
    if (!vm) {
        return MultiValue(LValue(false), LValue(L->intern_string("dynamic loading disabled")));
    }
    const char* path = nullptr;
    const char* mode = "bt";
    int env_ref = vm->default_env_registry_ref();

    if (count > 0 && args[0].type != Nil) {
        if (args[0].type != String)
            throw_runtime_error("bad argument #1 to 'loadfile' (string expected)");
        path = args[0].as_string();
    }
    if (count > 1 && args[1].type != Nil) {
        if (args[1].type != String)
            throw_runtime_error("bad argument #2 to 'loadfile' (string expected)");
        mode = args[1].as_string();
    }
    if (count > 2 && args[2].type == Table) {
        env_ref = vm->build_env_table(args[2]);
    }

    int ref = LUA_NOREF;
    std::string err;
    if (!vm->load_file(path, mode, env_ref, &ref, &err)) {
        return MultiValue(LValue(), LValue(L->intern_string(err.data(), err.size())));
    }
    return MultiValue(VMFunction::from_ref(L, ref));
}

//------------------ clx_dofile - loadfile(path)() with error propagation
static MultiValue clx_dofile(LState* L, const LValue* args, size_t count)
{
    MultiValue loaded = clx_loadfile(L, args, count);
    if (loaded.count < 1 || loaded[0].type != Function)
        return MultiValue(LValue(false), loaded.count > 1 ? loaded[1] : LValue());

    LValue func = loaded[0];
    return call_function(L, func, nullptr, 0, __FILE__, __LINE__);
}

}

//------------------ register_load_builtins - called from openlibs.cpp
extern "C" void clx_register_load_builtins(clx::LState* L)
{
    using namespace clx;
    LValue pkg = get_global(L, "package");
    LTable* pkg_tbl = (pkg.type == Table) ? static_cast<LTable*>(pkg.as_pointer()) : nullptr;

    LValue load_fn = L->create_closure(clx_load);
    LValue loadfile_fn = L->create_closure(clx_loadfile);
    LValue dofile_fn = L->create_closure(clx_dofile);

    set_global(L, "load", load_fn);
    set_global(L, "loadfile", loadfile_fn);
    set_global(L, "dofile", dofile_fn);

    (void)pkg_tbl;
}
