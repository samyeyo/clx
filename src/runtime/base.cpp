// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  base.cpp · Base module (print, type, etc)  │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include "../include/clx_simd.h"
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <cstdlib>

namespace clx {

//------------------ clx_error: raises an error (global error function)
clx::MultiValue lua_error(clx::LState *L, const clx::LValue *args, size_t count) {
    clx::LValue err_val = (count > 0) ? args[0] : clx::LValue();

    int level = 1;
    if (count > 1) {
        level = static_cast<int>(clx::to_number(args[1], 1.0));
    }

    if (err_val.type == clx::String && level > 0) {
        std::string prefix = clx::file_line_prefix(L);
        std::string full_msg = prefix + err_val.as_string();
        throw clx::LRuntimeException(clx::string(L, full_msg.c_str()));
    }
    throw clx::LRuntimeException(err_val);
}

//------------------ clx_pcall: protected call (global pcall function)
clx::MultiValue lua_pcall(clx::LState *L, const clx::LValue *args, size_t count) {
    if (count == 0) {
        throw clx::LRuntimeException(clx::string(L, "bad argument #1 to 'pcall' (value expected)"));
    }

    return clx::pcall_function(L, args[0], args + 1, count - 1);
}

//------------------ clx_xpcall: protected call with error handler (global xpcall function)
clx::MultiValue lua_xpcall(clx::LState *L, const clx::LValue *args, size_t count) {
    if (count < 2) {
        throw clx::LRuntimeException(clx::string(L, "bad argument to 'xpcall' (2 arguments expected)"));
    }

    clx::LValue func = args[0];
    clx::LValue msgh = args[1];

    size_t arg_count = count - 2;
    const clx::LValue *func_args = (arg_count > 0) ? (args + 2) : nullptr;

    try {
        clx::MultiValue ret = clx::call_function(L, func, func_args, arg_count, L->current_file, L->current_line);

        if (ret.count == 0)
            return clx::MultiValue({ clx::boolean(true) });
        if (ret.count == 1)
            return clx::MultiValue({ clx::boolean(true), ret[0] });

        std::vector<clx::LValue> results;
        results.reserve(ret.count + 1);
        results.push_back(clx::boolean(true));
        for (size_t i = 0; i < ret.count; ++i)
            results.push_back(ret[i]);
        return clx::MultiValue(results);

    } catch (const clx::LRuntimeException &e) {
        clx::LValue err_obj = e.error_obj;
        clx::MultiValue msgh_ret;

        try {
            L->shadow_stack[L->shadow_top++] = TypedSlot(&err_obj.val, &err_obj.type);
            msgh_ret = clx::call_function(L, msgh, &err_obj, 1, L->current_file, L->current_line);
            L->shadow_top--;
        } catch (...) {
            return clx::MultiValue({ clx::boolean(false), clx::string(L, "error in error handling") });
        }

        return clx::MultiValue({ clx::boolean(false), msgh_ret.count > 0 ? msgh_ret[0] : clx::LValue() });

    } catch (const std::exception &e) {
        clx::LValue err_obj = clx::string(L, e.what());
        clx::MultiValue msgh_ret;

        try {
            L->shadow_stack[L->shadow_top++] = TypedSlot(&err_obj.val, &err_obj.type);
            msgh_ret = clx::call_function(L, msgh, &err_obj, 1, L->current_file, L->current_line);
            L->shadow_top--;
        } catch (...) {
            return clx::MultiValue({ clx::boolean(false), clx::string(L, "error in error handling") });
        }

        return clx::MultiValue({ clx::boolean(false), msgh_ret.count > 0 ? msgh_ret[0] : clx::LValue() });
    }
}

//------------------ clx_getmetatable: returns the metatable of a value
static MultiValue lua_getmetatable(LState *L, const LValue *args, size_t count) {
    if (count < 1)
        return MultiValue();
    return MultiValue(clx::getmetatable(L, args[0]));
}

//------------------ clx_setmetatable: sets the metatable of a table
static MultiValue lua_setmetatable(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        clx::error(L, "bad argument to 'setmetatable' (table expected)");
    clx::check_table(L, args[0]);
    clx::setmetatable(L, args[0], args[1]);
    return MultiValue(args[0]);
}

//------------------ clx_print: prints values to stdout (global print function)
static MultiValue print(LState *L, const LValue *args, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        std::cout << args[i].to_string(L);
        if (i < count - 1)
            std::cout << "\t";
    }
    std::cout << "\n";
    return MultiValue();
}

//------------------ clx_collectgarbage: controls the garbage collector
MultiValue collectgarbage(LState *L, const LValue *args, size_t arg_count) {
    if (arg_count == 0 || !clx::is_string(args[0]))
        return MultiValue(clx::number(0.0));
    std::string_view opt = args[0].as_string();
    if (opt == "collect") {
        L->collect_garbage();
        return MultiValue(clx::number(0.0));
    }
    if (opt == "count") {
        double mem_kb = static_cast<double>(L->get_memory_usage()) / 1024.0;
        return MultiValue(clx::number(mem_kb));
    }
    if (opt == "stop") {
        L->gc_running = false;
        return MultiValue(clx::number(0.0));
    }
    if (opt == "restart") {
        L->gc_running = true;
        return MultiValue(clx::number(0.0));
    }
    if (opt == "isrunning") {
        return MultiValue(clx::boolean(L->gc_running));
    }
    if (opt == "step") {
        int64_t step_size = (arg_count >= 2) ? clx::check_integer(L, args[1]) : 0;
        if (step_size > 0) {
            L->allocated_bytes += static_cast<size_t>(step_size);
            if (L->allocated_bytes >= L->gc_bytes_threshold)
                L->collect_garbage();
        }
        bool finished = L->gc_step();
        return MultiValue(clx::boolean(finished));
    }
    if (opt == "incremental") {
        return MultiValue(clx::string(L, "incremental"));
    }
    if (opt == "generational") {

        return MultiValue(clx::string(L, "incremental"));
    }
    if (opt == "param") {
        if (arg_count < 2)
            return MultiValue(clx::number(0.0));
        std::string_view param = clx::check_string(L, args[1]);
        int old_val = 0;
        if (param == "pause")
            old_val = L->gc_pause;
        else if (param == "stepmul")
            old_val = L->gc_stepmul;
        else if (param == "stepsize")
            old_val = L->gc_stepsize;
        else
            return MultiValue(clx::number(0.0));
        if (arg_count >= 3) {
            int new_val = static_cast<int>(clx::check_integer(L, args[2]));
            new_val = std::clamp(new_val, 0, 100000);
            if (param == "pause")
                L->gc_pause = new_val;
            else if (param == "stepmul")
                L->gc_stepmul = new_val;
            else if (param == "stepsize")
                L->gc_stepsize = new_val;
        }
        return MultiValue(clx::integer(old_val));
    }
    return MultiValue(clx::number(0.0));
}

//------------------ clx_type: returns the type name of a value (global type function)
static MultiValue type(LState *L, const LValue *args, size_t count) {
    if (count == 0) {
        clx::error(L, "bad argument #1 to 'type' (value expected)");
    }
    const char *name = VALUE_TYPE_NAMES[static_cast<uint8_t>(args[0].type)];
    if (args[0].type == ValueType::Int64)
        name = "number";
    return MultiValue(clx::string(L, name));
}

//------------------ clx_assert: asserts a condition (global assert function)
static MultiValue assert(LState *L, const LValue *args, size_t count) {
    if (count == 0 || !clx::to_boolean(args[0])) {
        if (count > 1 && clx::is_string(args[1]))
            throw LRuntimeException(args[1]);
        clx::error(L, "assertion failed!");
    }
    return MultiValue(args, count);
}

//------------------ clx_tostring: converts value to string (global tostring function)
static MultiValue tostring(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        return MultiValue(LValue::istr("", 0));
    const LValue &v = args[0];
    switch (v.type) {
    case String:
        return MultiValue(v);
    case Nil:
        return MultiValue(LValue::istr("nil", 3));
    case Boolean:
        return MultiValue(LValue::istr(v.as_bool() ? "true" : "false", v.as_bool() ? 4 : 5));
    case Double: {
        char buf[64];
        int n = std::snprintf(buf, sizeof(buf), "%.17g", v.as_number());
        return MultiValue(clx::string(L, buf, (size_t)n));
    }
    case Int64: {
        char buf[32];
        int n = std::snprintf(buf, sizeof(buf), "%lld", (long long)v.as_integer());
        return MultiValue(clx::string(L, buf, (size_t)n));
    }
    default:
        return MultiValue(clx::string(L, v.to_string(L).c_str()));
    }
}

//------------------ clx_tonumber: converts value to number (global tonumber function)
static MultiValue tonumber(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        return MultiValue();
    const LValue &v = args[0];
    if (v.type == Double)
        return MultiValue(v);
    if (clx::is_string(v)) {
        double d;
        if (v.to_number(d))
            return MultiValue(clx::number(d));
    }
    return MultiValue();
}

//------------------ clx_rawequal: compares two values without metamethods
static MultiValue lua_rawequal(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        return MultiValue(clx::boolean(false));
    return MultiValue(clx::boolean(clx::rawequal(args[0], args[1])));
}

//------------------ clx_rawget: gets table value without metamethods
static MultiValue rawget(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        clx::error(L, "bad argument #1 to 'rawget' (table expected)");
    clx::check_table(L, args[0]);
    return MultiValue(clx::raw_get(L, args[0], args[1]));
}

//------------------ clx_rawset: sets table value without metamethods
static MultiValue rawset(LState *L, const LValue *args, size_t count) {
    if (count < 3)
        clx::error(L, "bad argument #1 to 'rawset' (table expected)");
    clx::check_table(L, args[0]);
    clx::raw_set(L, args[0], args[1], args[2]);
    return MultiValue(args[0]);
}

//------------------ clx_rawlen: returns raw length of string or table without __len
static MultiValue lua_rawlen(LState *L, const LValue *args, size_t count) {
    if (count < 1) {
        clx::error(L, "bad argument #1 to 'rawlen' (value expected)");
    }
    return MultiValue(clx::integer(clx::rawlen(args[0])));
}

//------------------ warnings_enabled: global flag tracking whether warnings are on/off
static bool warnings_enabled = true;

//------------------ clx_warn: issues a warning to stderr (global warn function)
static MultiValue warn(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        return MultiValue();

    if (clx::is_string(args[0])) {
        std::string_view s = args[0].as_string();
        if (s == "@off") {
            warnings_enabled = false;
            return MultiValue();
        }
        if (s == "@on") {
            warnings_enabled = true;
            return MultiValue();
        }
    }

    if (!warnings_enabled)
        return MultiValue();

    for (size_t i = 0; i < count; ++i) {
        if (i > 0)
            std::cerr << " ";
        std::cerr << args[i].to_string(L);
    }
    std::cerr << "\n";
    return MultiValue();
}

//------------------ clx_require: stub require function (global require)
static MultiValue require(LState *L, const LValue *args, size_t count) {
    return MultiValue();
}

//------------------ clx_next: returns next key-value pair from a table (global next function)
static MultiValue lua_next(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'next' (table expected)");
    clx::check_table(L, args[0]);
    LValue key = (count > 1) ? args[1] : LValue();
    return clx::next(L, args[0], key);
}

//------------------ clx_pairs: returns iterator for key-value traversal (global pairs function)
static MultiValue pairs(LState *L, const LValue *args, size_t count) {
    if (count == 0) {
        clx::error(L, "bad argument #1 to 'pairs' (value expected)");
    }
    LValue t = args[0];

    if (clx::is_table(t)) {
        LTable *tbl = static_cast<LTable *>(t.as_pointer());
        if (tbl->metatable) {
            LValue mt_pairs = tbl->metatable->gettable(L->str_pairs);
            if (mt_pairs.type == Function) {
                L->shadow_stack[L->shadow_top++] = TypedSlot(&t.val, &t.type);
                MultiValue ret = call_function(L, mt_pairs, &t, 1, __FILE__, __LINE__);
                L->shadow_top--;
                return ret;
            }
        }
    }

    return MultiValue({ get_global(L, "next"), t, LValue() });
}

//------------------ clx_select: selects a range of arguments (global select function)
static clx::MultiValue select(clx::LState *L, const clx::LValue *args, size_t arg_count) {
    if (arg_count < 1) {
        clx::error(L, "bad argument #1 to 'select' (number expected, got no value)");
    }

    const clx::LValue &arg1 = args[0];

    if (clx::is_string(arg1)) {
        if (std::string_view(arg1.as_string()) == "#") {
            return clx::MultiValue(clx::number(static_cast<double>(arg_count - 1)));
        }
    }

    double d_idx;
    if (!arg1.to_number(d_idx)) {
        clx::error(L, "bad argument #1 to 'select' (number expected)");
    }

    int64_t idx = static_cast<int64_t>(d_idx);
    int64_t n = static_cast<int64_t>(arg_count) - 1;

    if (idx < 0)
        idx = n + idx + 1;

    if (idx < 1) {
        clx::error(L, "bad argument #1 to 'select' (index out of range)");
    }

    if (idx > n) {
        return clx::MultiValue();
    }

    std::vector<clx::LValue> ret_vals;
    ret_vals.reserve(n - idx + 1);
    for (int64_t i = idx; i <= n; ++i) {
        ret_vals.push_back(args[i]);
    }

    return clx::MultiValue(ret_vals);
}

//------------------ clx_ipairs_iter: iterator function for ipairs traversal
static MultiValue ipairs_iter(LState *L, const LValue *args, size_t count) {
    if (count < 2 || !clx::is_table(args[0]))
        return MultiValue();

    LTable *t = static_cast<LTable *>(args[0].as_pointer());
    int64_t idx = args[1].type == Int64 ? args[1].as_integer() : static_cast<int64_t>(args[1].as_number());
    idx++;

    LValue val;
    if (idx >= 1 && static_cast<size_t>(idx) <= t->array_size) {
        val = LValue(t->array[idx - 1], t->array_types[idx - 1]);
    } else {
        LValue hash_val = t->gettable(LValue(static_cast<double>(idx)));
        if (hash_val.type != Nil)
            val = hash_val;
    }

    if (val.type == Nil)
        return MultiValue();
    return MultiValue({ clx::number(static_cast<double>(idx)), val });
}

//------------------ clx_ipairs: returns iterator for array traversal (global ipairs function)
static MultiValue ipairs(LState *L, const LValue *args, size_t count) {
    if (count == 0) {
        clx::error(L, "bad argument #1 to 'ipairs' (value expected)");
    }
    LValue t = args[0];

    if (clx::is_table(t)) {
        LTable *tbl = static_cast<LTable *>(t.as_pointer());
        if (tbl->metatable) {
            LValue mt_ipairs = tbl->metatable->gettable(LValue(L->intern_string("__ipairs")));
            if (mt_ipairs.type == Function) {
                L->shadow_stack[L->shadow_top++] = TypedSlot(&t.val, &t.type);
                MultiValue ret = call_function(L, mt_ipairs, &t, 1, __FILE__, __LINE__);
                L->shadow_top--;
                return ret;
            }
        }
    }

    LValue ipairs_iter_val = get_global(L, "_ipairs_iter");
    if (clx::is_nil(ipairs_iter_val)) {
        LValue g = LValue(Table, L->_G);
        clx::set_function(L, g, "_ipairs_iter", ipairs_iter);
        ipairs_iter_val = get_global(L, "_ipairs_iter");
    }

    return MultiValue({ ipairs_iter_val, t, clx::number(0.0) });
}

//------------------ luastd_base: registers all base library functions into the global table
void luastd_base(LState *L) {
    LValue g = LValue(Table, L->_G);
    clx::set_functions(L, g,
        { { "print", print }, { "require", require }, { "error", lua_error }, { "assert", assert },
            { "tostring", tostring }, { "tonumber", tonumber }, { "rawequal", lua_rawequal }, { "rawget", rawget },
            { "rawset", rawset }, { "rawlen", lua_rawlen }, { "setmetatable", lua_setmetatable },
            { "getmetatable", lua_getmetatable }, { "collectgarbage", collectgarbage }, { "type", type },
            { "pcall", lua_pcall }, { "xpcall", lua_xpcall }, { "warn", warn }, { "next", lua_next },
            { "pairs", pairs }, { "ipairs", ipairs }, { "select", select } });
    clx::set_field(L, g, "_VERSION", clx::string(L, "lua 5.5"));
}

}
