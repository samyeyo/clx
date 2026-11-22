// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  debug.cpp · Debug Library (AOT subset)     │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include "internal.h"

namespace clx {

//------------------ type_meta_slot: registry key holding the metatable of one non-table type
static LValue type_meta_slot(LState *L, ValueType t) {
    std::string key("\0type_meta:", 11);
    key.push_back(static_cast<char>('0' + static_cast<int>(t)));
    return LValue(L->intern_string(key));
}

//------------------ registry_table: the debug.getregistry() table, created on first use and rooted
static LTable *registry_table(LState *L) {
    if (!L->registry) {
        LValue r = L->create_table(0, 4);
        L->registry = static_cast<LTable *>(r.as_pointer());
        L->permanent_roots.push_back(r);
    }
    return L->registry;
}

//------------------ lookup_metatable: the metatable debug.getmetatable reports for one value
static LTable *lookup_metatable(LState *L, const LValue &v) {
    switch (v.type) {
    case ValueType::Table:
        return tbl_metatable(static_cast<LTable *>(v.as_pointer()));
    case ValueType::UserData:
        return static_cast<LUserdata *>(v.as_pointer())->metatable;
    case ValueType::String:
        return L->string_metatable;
    default:
        return L->type_metatables[type_meta_index(v.type)];
    }
}

//------------------ debug_getmetatable: debug.getmetatable (ignores the __metatable guard)
static MultiValue debug_getmetatable(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        error(L, bad_arg_msg(L, 1, "debug.getmetatable", "value expected").c_str());
    LTable *mt = lookup_metatable(L, args[0]);
    return MultiValue(mt ? LValue(ValueType::Table, mt) : LValue());
}

//------------------ debug_setmetatable: debug.setmetatable, for every type including numbers and nil
static MultiValue debug_setmetatable(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        error(L, bad_arg_msg(L, 1, "debug.setmetatable", "table expected").c_str());
    const LValue v = args[0];
    LValue mt_val = (count > 1) ? args[1] : LValue();
    if (mt_val.type != ValueType::Nil && mt_val.type != ValueType::Table)
        error(L, bad_arg_msg(L, 2, "debug.setmetatable", "nil or table expected").c_str());
    LTable *mt = (mt_val.type == ValueType::Table) ? static_cast<LTable *>(mt_val.as_pointer()) : nullptr;

    //------------------ a light userdata has no metatable at all, so there is no slot to install one into
    if (v.type == ValueType::LightUserData)
        error(L, bad_arg_msg(L, 1, "debug.setmetatable", "cannot set a metatable for a light userdata").c_str());

    switch (v.type) {
    case ValueType::Table: {
        LTable *t = static_cast<LTable *>(v.as_pointer());
        if (mt)
            meta_list_add(L, t);
        else
            meta_list_remove(L, t);
        tbl_set_metatable(t, mt);
        mark_gc_object(L, t, mt);
        if (mt)
            gc_barrier_table(L, t, mt_val);
        break;
    }
    case ValueType::UserData: {
        LUserdata *ud = static_cast<LUserdata *>(v.as_pointer());
        if (mt)
            meta_ud_list_add(L, ud);
        else
            meta_ud_list_remove(L, ud);
        ud->metatable = mt;
        mark_gc_object(L, ud, mt);
        if (mt)
            gc_barrier_header(L, ud, mt_val);
        break;
    }
    case ValueType::String:
        L->string_metatable = mt;
        if (mt_val.type == ValueType::Table)
            registry_table(L)->settable(type_meta_slot(L, v.type), mt_val);
        break;
    default:
        L->type_metatables[type_meta_index(v.type)] = mt;
        if (mt_val.type == ValueType::Table)
            registry_table(L)->settable(type_meta_slot(L, v.type), mt_val);
        else
            registry_table(L)->settable(type_meta_slot(L, v.type), LValue());
        break;
    }
    return MultiValue(v);
}

//------------------ uservalue_index: validates the optional 1-based uservalue index argument
static size_t uservalue_index(LState *L, const LValue *args, size_t count, const char *fname) {
    if (count < 2 || args[1].type == ValueType::Nil)
        return 1;
    int64_t n = 0;
    if (!to_integer(args[1], n))
        error(L, bad_arg_msg(L, 2, fname, "number expected").c_str());
    if (n != 1)
        error(L, bad_arg_msg(L, 2, fname, "index out of range").c_str());
    return 1;
}

//------------------ debug_getuservalue: value of one uservalue slot
static MultiValue debug_getuservalue(LState *L, const LValue *args, size_t count) {
    if (count == 0 || args[0].type != ValueType::UserData)
        return MultiValue();
    uservalue_index(L, args, count, "debug.getuservalue");
    LUserdata *ud = static_cast<LUserdata *>(args[0].as_pointer());
    return MultiValue(ud->uservalue);
}

//------------------ debug_setuservalue: sets one uservalue slot, returns the userdata
static MultiValue debug_setuservalue(LState *L, const LValue *args, size_t count) {
    if (count == 0 || args[0].type != ValueType::UserData)
        error(L, bad_arg_msg(L, 1, "debug.setuservalue", "userdata expected").c_str());
    size_t idx = uservalue_index(L, args, count, "debug.setuservalue");
    (void)idx;
    LUserdata *ud = static_cast<LUserdata *>(args[0].as_pointer());
    LValue val = (count > 1) ? args[1] : LValue();
    ud->uservalue = val;
    if (val.type == ValueType::Table)
        gc_barrier_header(L, ud, val);
    else if (val.type == ValueType::Function || val.type == ValueType::UserData || val.type == ValueType::Thread)
        gc_barrier_header(L, ud, val);
    return MultiValue(args[0]);
}

//------------------ upvalue_slot: the n-th upvalue cell of a function, or nullptr when out of range.
//------------------ One index model for get/set/id/join: the runtime's cell order, numbered from 1,
//------------------ exactly as the Lua C API adapter numbers them (luaapi_upvalue_cell). A compiled
//------------------ closure keeps its captures inside the emitted C++ lambda, so the cells here are the
//------------------ in-scope capture cells the closure was created with, and their names are not recorded.
static LUpValue *upvalue_slot(LValue &func, int64_t n) {
    if (func.type != ValueType::Function || n < 1)
        return nullptr;
    LCFunction *f = static_cast<LCFunction *>(func.as_pointer());
    if (static_cast<size_t>(n) > f->gc_cells.size())
        return nullptr;
    return &f->gc_cells[static_cast<size_t>(n) - 1];
}

//------------------ debug_getupvalue: (name, value) of one upvalue; the name is empty for compiled code
static MultiValue debug_getupvalue(LState *L, const LValue *args, size_t count) {
    if (count < 2 || args[0].type != ValueType::Function)
        error(L, bad_arg_msg(L, 1, "debug.getupvalue", "function expected").c_str());
    int64_t n = 0;
    if (!to_integer(args[1], n))
        error(L, bad_arg_msg(L, 2, "debug.getupvalue", "number expected").c_str());
    LValue func = args[0];
    LHeader *header = static_cast<LHeader *>(func.as_pointer());
    if (clx_vm_debug_upvalue_hook) {
        LValue name, value;
        if (clx_vm_debug_upvalue_hook(L, header, n, name, value)) {
            LValue vals[2] = { name, value };
            return MultiValue(vals, 2, L);
        }
    }
    LUpValue *cell = upvalue_slot(func, n);
    if (!cell || !*cell)
        return MultiValue();
    LValue vals[2] = { LValue(L->intern_string(std::string())), **cell };
    return MultiValue(vals, 2, L);
}

//------------------ debug_setupvalue: assigns one upvalue, returns its (unknown) name
static MultiValue debug_setupvalue(LState *L, const LValue *args, size_t count) {
    if (count < 2 || args[0].type != ValueType::Function)
        error(L, bad_arg_msg(L, 1, "debug.setupvalue", "function expected").c_str());
    int64_t n = 0;
    if (!to_integer(args[1], n))
        error(L, bad_arg_msg(L, 2, "debug.setupvalue", "number expected").c_str());
    LValue func = args[0];
    LUpValue *cell = upvalue_slot(func, n);
    if (!cell || !*cell)
        return MultiValue();
    LValue val = (count > 2) ? args[2] : LValue();
    **cell = val;
    gc_barrier_cell(L, *cell, val);
    return MultiValue(LValue(L->intern_string(std::string())));
}

//------------------ debug_upvalueid: stable identity for one upvalue cell, shaped like stock's
//------------------ light userdata; a heap userdata would be collected and reused, and the runtime has
//------------------ no light-userdata value type, so the address is interned as text.
static MultiValue debug_upvalueid(LState *L, const LValue *args, size_t count) {
    if (count < 2 || args[0].type != ValueType::Function)
        error(L, bad_arg_msg(L, 1, "debug.upvalueid", "function expected").c_str());
    int64_t n = 0;
    if (!to_integer(args[1], n))
        error(L, bad_arg_msg(L, 2, "debug.upvalueid", "number expected").c_str());
    LValue func = args[0];
    LUpValue *cell = upvalue_slot(func, n);
    if (!cell || !*cell)
        return MultiValue(LValue());
    char buf[48];
    std::snprintf(buf, sizeof(buf), "userdata: %p", static_cast<void *>(cell->get()));
    return MultiValue(LValue(L->intern_string(buf)));
}

//------------------ stub_error: raises a "not available in compiled code" message behind the call site's
//------------------ file:line prefix, so an uncaught failure points at the statement that made the call
[[noreturn]] static void stub_error(LState *L, const char *msg) {
    error(L, (file_line_prefix(L) + msg).c_str());
}

//------------------ debug_upvaluejoin: refused for compiled code. A compiled closure reads its captures
//------------------ through the C++ lambda it was emitted as, so replacing the recorded cell would change
//------------------ what debug.getupvalue reports without changing what the function reads.
static MultiValue debug_upvaluejoin(LState *L, const LValue *args, size_t count) {
    (void)args;
    (void)count;
    stub_error(L, "debug.upvaluejoin is not available in compiled code");
    return MultiValue();
}

//------------------ debug_getregistry: the registry table
static MultiValue debug_getregistry(LState *L, const LValue *args, size_t count) {
    (void)args;
    (void)count;
    return MultiValue(LValue(ValueType::Table, registry_table(L)));
}

//------------------ debug_traceback: message plus a placeholder trace, since AOT frames carry no chain
static MultiValue debug_traceback(LState *L, const LValue *args, size_t count) {
    size_t first = 0;
    if (count > 0 && args[0].type == ValueType::Thread)
        first = 1;
    LValue msg = (count > first) ? args[first] : LValue();
    if (msg.type != ValueType::Nil && msg.type != ValueType::String)
        return MultiValue(msg);
    std::string text = (msg.type == ValueType::String) ? msg.to_string(L) + "\n" : "";
    text += "stack traceback:";
    return MultiValue(LValue(L->intern_string(text)));
}

//------------------ debug_getinfo: a run-time loaded chunk is described by the embedded VM; an AOT closure
//------------------ has no line/frame table, so only that case reports the entry as unavailable
static MultiValue debug_getinfo(LState *L, const LValue *args, size_t count) {
    std::string_view what = "flnSu";
    if (count >= 2 && args[1].type == ValueType::String)
        what = std::string_view(args[1].as_string(), args[1].string_len());
    if (count >= 1 && args[0].type == ValueType::Function && clx_vm_debug_getinfo_hook) {
        std::string opts(what);
        LValue info;
        if (clx_vm_debug_getinfo_hook(L, args[0], opts.c_str(), info))
            return MultiValue(info);
    }
    stub_error(L, "debug.getinfo/getlocal/setlocal/sethook/gethook are not available in compiled code");
    return MultiValue();
}

//------------------ unsupported: debug entries that need an interpreter's frame/line tables
static MultiValue unsupported(LState *L, const LValue *args, size_t count) {
    (void)args;
    (void)count;
    stub_error(L, "debug.getinfo/getlocal/setlocal/sethook/gethook are not available in compiled code");
    return MultiValue();
}

//------------------ luastd_debug: registers the debug library into the global state
void luastd_debug(LState *L) {
    LValue dbg = clx::table(L);
    static constexpr clx::LazyReg debug_funcs[] = {
        { "getmetatable", debug_getmetatable },
        { "setmetatable", debug_setmetatable },
        { "getuservalue", debug_getuservalue },
        { "setuservalue", debug_setuservalue },
        { "getupvalue", debug_getupvalue },
        { "setupvalue", debug_setupvalue },
        { "upvalueid", debug_upvalueid },
        { "upvaluejoin", debug_upvaluejoin },
        { "getregistry", debug_getregistry },
        { "traceback", debug_traceback },
        { "getinfo", debug_getinfo },
        { "getlocal", unsupported },
        { "setlocal", unsupported },
        { "sethook", unsupported },
        { "gethook", unsupported },
    };
    set_lazy_funcs(L, dbg, debug_funcs, std::size(debug_funcs));
    clx::set_global(L, "debug", dbg);
}

}
