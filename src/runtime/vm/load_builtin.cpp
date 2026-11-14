// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  load_builtin.cpp · load/loadfile/dofile    │
// └─────────────────────────────────────────────┘

#include "dynamic_vm.h"
#include "vm_convert.h"
#include "vm_function_object.h"
#include "dump_builtin.h"
#include "../internal.h"
#include <clx.h>
#include <clx_runtime.h>
#include <cstdio>
#include <fstream>

namespace clx {

//------------------ pkg_searcher_luafile_vm - package.searchers[2] for --dynamic builds: locate and VM-load module files
static MultiValue pkg_searcher_luafile_vm(LState *L, const LValue *args, size_t count) {
    DynamicVM *vm = DynamicVM::acquire(L);
    if (!vm)
        return MultiValue(LValue(), LValue(L->intern_string("\n\tdynamic loading disabled")));
    if (count < 1 || args[0].type != String)
        return MultiValue(LValue(), LValue());

    if (!L->package_table)
        return MultiValue(LValue(), LValue(L->intern_string("\n\t'package' library is not open")));
    LValue path = L->package_table->gettable(LValue(L->intern_string("path", 4)));
    if (path.type != String)
        return MultiValue(LValue(), LValue(L->intern_string("\n\tpackage.path is not a string")));
#if defined(_WIN32)
    const char dirsep = '\\';
#else
    const char dirsep = '/';
#endif

    std::string tried;
    const char sep_str[2] = { '.', '\0' };
    const char dirsep_str[2] = { dirsep, '\0' };
    std::string filename = package_searchpath(L, args[0].as_string(), args[0].string_len(), path.as_string(),
        path.string_len(), sep_str, 1, dirsep_str, 1, &tried);
    if (filename.empty())
        return MultiValue(LValue(), LValue(L->intern_string(tried.data(), tried.size())));

    int ref = LUA_NOREF;
    std::string err;
    if (!vm->load_file(filename.c_str(), "bt", vm->default_env_registry_ref(), &ref, &err)) {
        //------------------ a file that exists but does not compile raises the stock "error loading
        //------------------ module" error instead of being reported as a missing module
        std::string msg = "error loading module '";
        msg.append(args[0].as_string(), args[0].string_len());
        msg += "' from file '" + filename + "':\n\t" + err;
        throw LRuntimeException(LValue(L->intern_string(msg.data(), msg.size())));
    }
    //------------------ stock's loader searcher returns the resolved file name as the loader's extra
    //------------------ value, which require hands to the chunk as its second vararg
    return MultiValue(VMFunction::from_ref(L, ref), LValue(L->intern_string(filename.data(), filename.size())));
}

//------------------ pkg_searcher_c - package.searchers[3]: lists the tried cpath files, then reports that C
//------------------ modules must be linked statically at compile time (see --modules)
static MultiValue pkg_searcher_c(LState *L, const LValue *args, size_t count) {
    if (count < 1 || args[0].type != String)
        return MultiValue(LValue(), LValue());
    if (!L->package_table)
        return MultiValue(LValue(), LValue(L->intern_string("\n\t'package' library is not open")));
    LValue cpath = L->package_table->gettable(LValue(L->intern_string("cpath", 5)));
    if (cpath.type != String)
        return MultiValue(LValue(), LValue(L->intern_string("\n\tpackage.cpath is not a string")));
#if defined(_WIN32)
    const char dirsep_str[2] = { '\\', '\0' };
#else
    const char dirsep_str[2] = { '/', '\0' };
#endif
    std::string tried;
    package_searchpath(L, args[0].as_string(), args[0].string_len(), cpath.as_string(), cpath.string_len(), ".", 1,
        dirsep_str, 1, &tried);
    tried += "\n\tno C loader (C modules must be linked statically at compile time; see --modules)";
    return MultiValue(LValue(), LValue(L->intern_string(tried.data(), tried.size())));
}

//------------------ check_load_mode: stock's luaL_argcheck(strspn(mode, "bt") == strlen(mode), "invalid mode")
static void check_load_mode(LState *L, const char *mode, int argn, const char *fname) {
    for (const char *p = mode; *p; ++p) {
        if (*p != 'b' && *p != 't')
            throw_runtime_error(bad_arg_msg(L, argn, fname, "invalid mode"));
    }
}

//------------------ clx_load - load([chunk [, chunkname [, mode [, env]]]])
static MultiValue clx_load(LState *L, const LValue *args, size_t count) {
    DynamicVM *vm = DynamicVM::acquire(L);
    if (!vm) {
        return MultiValue(LValue(false), LValue(L->intern_string("dynamic loading disabled")));
    }
    LValue sv;
    std::string reader_src;
    const char *src = nullptr;
    size_t len = 0;
    bool from_reader = false;
    if (count > 0 && args[0].type == Function) {
        //------------------ stock load() also takes a reader: call it until it returns nil.
        //------------------ A reader error is a load error (nil, message), not a raised error:
        //------------------ in stock it happens inside the protected parser.
        static constexpr CallSite reader_site = { __FILE__, __LINE__, false, false };
        try {
            for (;;) {
                MultiValue part = call_function_rooted(L, args[0], nullptr, 0, &reader_site);
                if (part.count == 0 || part[0].type == Nil)
                    break;
                if (part[0].type != String)
                    throw_runtime_error("reader function must return a string");
                if (part[0].string_len() == 0)
                    break;
                reader_src.append(part[0].as_string(), part[0].string_len());
            }
        } catch (const LRuntimeException &e) {
            return MultiValue(LValue(), e.error_obj.type != Nil ? e.error_obj : LValue(L->intern_string(e.what())));
        }
        src = reader_src.data();
        len = reader_src.size();
        from_reader = true;
    } else if (count > 0 && args[0].type != Nil) {
        sv = args[0];
        if (sv.type != String)
            throw_runtime_error("bad argument #1 to 'load' (string expected)");
        src = sv.as_string();
        len = sv.string_len();
    } else {

        return MultiValue(LValue(false), LValue(L->intern_string("load(): no chunk given")));
    }

    //------------------ stock load() defaults chunkname to the chunk string itself, "=(load)" for readers
    const char *chunkname = from_reader ? "=(load)" : (src ? src : "=(load)");
    if (count > 1 && args[1].type != Nil) {
        if (args[1].type != String)
            throw_runtime_error("bad argument #2 to 'load' (string expected)");
        chunkname = args[1].as_string();
    }
    const char *mode = "bt";
    if (count > 2 && args[2].type != Nil) {
        if (args[2].type != String)
            throw_runtime_error("bad argument #3 to 'load' (string expected)");
        mode = args[2].as_string();
        check_load_mode(L, mode, 3, "load");
    }

    int env_ref = vm->default_env_registry_ref();
    if (count > 3) {
        //------------------ stock load() accepts any value as _ENV, not just a table
        if (args[3].type == Table)
            env_ref = vm->build_env_table(args[3]);
        else {
            clx_to_vm_value_(L, vm->lua_L(), args[3]);
            env_ref = luaL_ref(vm->lua_L(), LUA_REGISTRYINDEX);
        }
    }

    int ref = LUA_NOREF;
    std::string err;
    if (!vm->load_buffer(src, len, chunkname, mode, env_ref, &ref, &err)) {
        return MultiValue(LValue(), LValue(L->intern_string(err.data(), err.size())));
    }
    return MultiValue(VMFunction::from_ref(L, ref));
}

//------------------ clx_loadfile - loadfile([path [, mode [, env]]])
static MultiValue clx_loadfile(LState *L, const LValue *args, size_t count) {
    DynamicVM *vm = DynamicVM::acquire(L);
    if (!vm) {
        return MultiValue(LValue(false), LValue(L->intern_string("dynamic loading disabled")));
    }
    const char *path = nullptr;
    const char *mode = "bt";
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
        check_load_mode(L, mode, 2, "loadfile");
    }
    if (count > 2) {
        if (args[2].type == Table)
            env_ref = vm->build_env_table(args[2]);
        else {
            clx_to_vm_value_(L, vm->lua_L(), args[2]);
            env_ref = luaL_ref(vm->lua_L(), LUA_REGISTRYINDEX);
        }
    }

    int ref = LUA_NOREF;
    std::string err;
    if (!vm->load_file(path, mode, env_ref, &ref, &err)) {
        return MultiValue(LValue(), LValue(L->intern_string(err.data(), err.size())));
    }
    return MultiValue(VMFunction::from_ref(L, ref));
}

//------------------ clx_dofile - loadfile(path)() with error propagation
static MultiValue clx_dofile(LState *L, const LValue *args, size_t count) {
    MultiValue loaded = clx_loadfile(L, args, count);
    if (loaded.count < 1 || loaded[0].type != Function)
        return MultiValue(LValue(false), loaded.count > 1 ? loaded[1] : LValue());

    LValue func = loaded[0];
    static constexpr CallSite builtin_site = { __FILE__, __LINE__, false, false };
    return call_function_rooted(L, func, nullptr, 0, &builtin_site);
}

}

//------------------ vm_debug_upvalue - debug.getupvalue for a VM closure, answered by the Lua debug API
static bool vm_debug_upvalue(clx::LState *L, clx::LHeader *fn, int64_t n, clx::LValue &name, clx::LValue &value) {
    using namespace clx;
    if (!(fn->flags & LFLAG_VM_PROXY))
        return false;
    VMFunction *vf = reinterpret_cast<VMFunction *>(fn);
    DynamicVM *vm = DynamicVM::acquire(L);
    if (!vm || n < 1)
        return false;
    lua_State *vL = vm->lua_L();
    lua_rawgeti(vL, LUA_REGISTRYINDEX, vf->registry_ref);
    const char *nm = lua_getupvalue(vL, -1, static_cast<int>(n));
    if (!nm) {
        lua_pop(vL, 1);
        return false;
    }
    value = vm_to_clx_value_(L, vL, -1);
    name = LValue(L->intern_string(nm));
    lua_pop(vL, 2);
    return true;
}

//------------------ vm_debug_getinfo - debug.getinfo for a VM closure, described by the Lua debug API and
//------------------ handed back as a stock-shaped table (the '>' option pops the function it inspects)
static bool vm_debug_getinfo(clx::LState *L, const clx::LValue &fn, const char *what, clx::LValue &out) {
    using namespace clx;
    if (fn.type != Function || fn.as_pointer() == nullptr)
        return false;
    LHeader *header = static_cast<LHeader *>(fn.as_pointer());
    if (!(header->flags & LFLAG_VM_PROXY))
        return false;
    VMFunction *vf = reinterpret_cast<VMFunction *>(header);
    DynamicVM *vm = DynamicVM::acquire(L);
    if (!vm)
        return false;

    lua_State *vL = vm->lua_L();
    lua_rawgeti(vL, LUA_REGISTRYINDEX, vf->registry_ref);
    if (!lua_isfunction(vL, -1)) {
        lua_pop(vL, 1);
        return false;
    }
    std::string opts = std::string(">") + (what ? what : "flnSu");
    lua_Debug ar;
    if (!lua_getinfo(vL, opts.c_str(), &ar))
        return false;

    LValue tbl = L->create_table(0, 12);
    LTable *t = static_cast<LTable *>(tbl.as_pointer());
    auto set_str = [&](const char *key, const char *value) {
        if (value)
            t->settable(LValue(L->intern_string(key)), LValue(L->intern_string(value)));
    };
    auto set_int = [&](const char *key, int64_t value) { t->settable(LValue(L->intern_string(key)), LValue(value)); };
    set_str("source", ar.source);
    set_str("short_src", ar.short_src);
    set_str("what", ar.what);
    set_str("name", ar.name);
    set_str("namewhat", ar.namewhat);
    set_int("linedefined", ar.linedefined);
    set_int("lastlinedefined", ar.lastlinedefined);
    set_int("currentline", ar.currentline);
    set_int("nups", ar.nups);
    set_int("nparams", ar.nparams);
    set_int("ftransfer", ar.ftransfer);
    set_int("ntransfer", ar.ntransfer);
    set_int("isvararg", ar.isvararg != 0);
    set_int("istailcall", ar.istailcall != 0);
    out = tbl;
    return true;
}

//------------------ register_load_builtins - called from openlibs.cpp
extern "C" void clx_register_load_builtins(clx::LState *L) {
    using namespace clx;
    LTable *pkg_tbl = L->package_table;

    LValue load_fn = L->create_closure(clx_load);
    LValue loadfile_fn = L->create_closure(clx_loadfile);
    LValue dofile_fn = L->create_closure(clx_dofile);

    //------------------ --dynamic builds give string.dump a VM backing: bytecode strings
    //------------------ for VM functions, callable VM boxes for native closures (see dump_builtin.cpp)
    clx_string_dump_hook = clx_string_dump_dynamic;
    clx_vm_debug_upvalue_hook = vm_debug_upvalue;
    clx_vm_debug_getinfo_hook = vm_debug_getinfo;

    set_global(L, "load", load_fn);
    set_global(L, "loadfile", loadfile_fn);
    set_global(L, "dofile", dofile_fn);

    //------------------ --dynamic builds upgrade package.searchers[2] to load Lua files through the embedded VM
    if (pkg_tbl) {
        LValue searchers = pkg_tbl->gettable(LValue(L->intern_string("searchers")));
        if (searchers.type == Table) {
            LTable *searchers_tbl = static_cast<LTable *>(searchers.as_pointer());
            searchers_tbl->settable(LValue(static_cast<int64_t>(2)), L->create_closure(pkg_searcher_luafile_vm));
            searchers_tbl->settable(LValue(static_cast<int64_t>(3)), L->create_closure(pkg_searcher_c));
        }
    }
}
