// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  base.cpp · Base module (print, type, etc)  │
// └─────────────────────────────────────────────┘

#include "../include/clx_simd.h"
#include "clx.h"
#include "internal.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace clx {

//------------------ clx_error: raises an error (global error function)
clx::MultiValue lua_error(clx::LState *L, const clx::LValue *args, size_t count) {
    clx::LValue err_val = (count > 0) ? args[0] : clx::LValue();

    int level = 1;
    if (count > 1) {
        level = static_cast<int>(clx::to_number(args[1], 1.0));
    }

    //------------------ stock adds a position only for level 1 (the frame that called 'error'); deeper
    //------------------ levels point at C frames, which carry no line information
    if (err_val.type == clx::String && level == 1) {
        std::string prefix = clx::file_line_prefix(L);
        std::string err_str(err_val.as_string(), err_val.string_len());
        std::string full_msg = prefix + err_str;
        throw clx::LRuntimeException(clx::string(L, full_msg.data(), full_msg.size()));
    }
    throw clx::LRuntimeException(err_val);
}

//------------------ clx_pcall: protected call (global pcall function)
clx::MultiValue lua_pcall(clx::LState *L, const clx::LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error(bad_arg_msg(L, 1, "pcall", "value expected"));

    return clx::pcall_function(L, args[0], args + 1, count - 1);
}

//------------------ run_message_handler: stock feeds an error raised *by* the handler back into the same
//------------------ handler, so a chain that keeps erroring ends at LUAI_MAXCCALLS with 'C stack overflow'
static constexpr int kMaxMessageHandlerDepth = 200;

//------------------ stack allowance for the handler: stock resets its C-call counter before running a
//------------------ message handler, so the handler always has room. The pointer guard can be met with the
//------------------ stack pointer already at its lowest frame (MSVC's unwinder leaves it at the throw
//------------------ depth), which would refuse the handler's very first call. The top-level handler call
//------------------ runs with the guard shifted down by a fixed allowance; a handler that overflows inside
//------------------ that allowance reports LUA_ERRERR the same way stock's exhausted C stack does.
static constexpr size_t kHandlerStackAllowance = 24 * 1024;

//------------------ HandlerStackAllowance: shifts the guard down for the duration of one handler call
struct HandlerStackAllowance {
    clx::LState *L;
    char *saved_low;

    HandlerStackAllowance(clx::LState *state, int depth)
        : L(state)
        , saved_low(state->stack_low) {
        if (depth == 0 && saved_low && clx::stack_probe() < saved_low)
            L->stack_low = clx::stack_probe() - kHandlerStackAllowance;
    }

    ~HandlerStackAllowance() { L->stack_low = saved_low; }
};

static bool run_message_handler(
    clx::LState *L, clx::LValue handler, clx::LValue err_obj, clx::MultiValue &out, int depth = 0) {
    //------------------ stock re-enters the handler until its C-call limit, then reports LUA_ERRERR;
    //------------------ whether the limit's message reads 'C stack overflow' or 'error in error handling'
    //------------------ depends on where in stock's C stack the limit lands, so clx reports the
    //------------------ documented LUA_ERRERR message
    if (depth >= kMaxMessageHandlerDepth) {
        out = clx::MultiValue(clx::string(L, "error in error handling"));
        return false;
    }
    HandlerStackAllowance allowance(L, depth);
    try {
        out = clx::call_function_rooted(
            L, handler, &err_obj, 1, L->current_site ? L->current_site : clx::default_call_site());
        return true;
    } catch (const clx::LRuntimeException &e) {
        clx::LValue next = e.error_obj;
        if (L->has_close_err) {
            next = L->close_err;
            L->has_close_err = false;
        }
        //------------------ stock re-enters the handler only while it can still call; a handler that fails
        //------------------ with its own stack overflow is already at the C limit, so the nested error is
        //------------------ reported as 'error in error handling' (LUA_ERRERR) instead of recursing again
        if (next.type == clx::ValueType::String) {
            std::string msg(next.as_string(), next.string_len());
            if (msg.find("stack overflow") != std::string::npos) {
                out = clx::MultiValue(clx::string(L, "error in error handling"));
                return false;
            }
        }
        return run_message_handler(L, handler, next, out, depth + 1);
    } catch (...) {
        out = clx::MultiValue(clx::string(L, "error in error handling"));
        return false;
    }
}

//------------------ clx_xpcall: protected call with error handler (global xpcall function)
clx::MultiValue lua_xpcall(clx::LState *L, const clx::LValue *args, size_t count) {
    if (count < 2)
        throw_runtime_error(bad_arg_msg(L, 2, "xpcall", "function expected, got no value"));

    clx::LValue func = args[0];
    clx::LValue msgh = args[1];

    if (args[1].type != clx::ValueType::Function)
        clx::error(L, bad_arg_msg(L, 2, "xpcall", "function expected, got no value").c_str());

    size_t arg_count = count - 2;
    const clx::LValue *func_args = (arg_count > 0) ? (args + 2) : nullptr;

    try {
        clx::MultiValue ret = clx::call_function_rooted(
            L, func, func_args, arg_count, L->current_site ? L->current_site : clx::default_call_site());

        if (ret.count == 0)
            return clx::MultiValue(true);
        if (ret.count == 1)
            return clx::MultiValue(true, ret[0]);

        //------------------ single allocation (see pcall_function): push_back is quadratic in the result count
        std::vector<clx::LValue> vals;
        vals.reserve(ret.count + 1);
        vals.push_back(clx::LValue(true));
        for (size_t i = 0; i < ret.count; ++i)
            vals.push_back(ret[i]);
        return clx::MultiValue(vals, L);

    } catch (const clx::LRuntimeException &e) {
        clx::LValue err_obj = e.error_obj;
        if (L->has_close_err) {
            err_obj = L->close_err;
            L->has_close_err = false;
        }
        clx::MultiValue msgh_ret;

        if (!run_message_handler(L, msgh, err_obj, msgh_ret))
            return clx::MultiValue(false, msgh_ret.count > 0 ? msgh_ret[0] : clx::LValue());

        return clx::MultiValue(false, msgh_ret.count > 0 ? msgh_ret[0] : clx::LValue());

    } catch (const std::exception &e) {
        clx::LValue err_obj = clx::string(L, e.what());
        clx::MultiValue msgh_ret;

        if (!run_message_handler(L, msgh, err_obj, msgh_ret))
            return clx::MultiValue(false, msgh_ret.count > 0 ? msgh_ret[0] : clx::LValue());

        return clx::MultiValue(false, msgh_ret.count > 0 ? msgh_ret[0] : clx::LValue());
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
        throw_runtime_error(bad_arg_msg(L, 1, "setmetatable", "table expected, got no value"));
    if (args[0].type != clx::ValueType::Table)
        throw_runtime_error(
            bad_arg_msg(L, 1, "setmetatable", std::string("table expected, got ") + stock_type_name(args[0])));
    clx::setmetatable(L, args[0], args[1]);
    return MultiValue(args[0]);
}

//------------------ clx_print: prints values to stdout (global print function)
//------------------ print_write — stdout via the io batch when linked (shared ordering with
//------------------ io.write), direct cout under --minimal where io.o is dropped.
static void print_write(const char *p, size_t n) {
    if (clx_io_write_ptr)
        clx_io_write_ptr(stdout, p, n);
    else
        std::cout.write(p, static_cast<std::streamsize>(n));
}

static MultiValue print(LState *L, const LValue *args, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        std::string s = args[i].to_string(L);
        print_write(s.data(), s.size());
        if (i < count - 1)
            print_write("\t", 1);
    }
    print_write("\n", 1);
    return MultiValue();
}

//------------------ clx_collectgarbage: controls the garbage collector
MultiValue collectgarbage(LState *L, const LValue *args, size_t arg_count) {
    //------------------ no option means "collect" (stock default), not a no-op
    std::string_view opt = "collect";
    if (arg_count > 0) {
        if (!clx::is_string(args[0]))
            clx::error(L,
                bad_arg_msg(L, 1, "collectgarbage", std::string("string expected, got ") + stock_type_name(args[0]))
                    .c_str());
        opt = std::string_view(args[0].as_string(), args[0].string_len());
    }
    if (opt == "collect") {
        //------------------ stock answers nil for a re-entrant collection, which is what a __gc that calls
        //------------------ collectgarbage() gets; it never runs a second collection from inside one
        if (L->gc_minor_active || L->gc_draining)
            return MultiValue(clx::LValue());
        bool had_recent = !L->gc_recent.empty();
        L->collect_garbage();
        if (had_recent)
            L->collect_garbage();
        return MultiValue(clx::integer(0));
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
        bool finished = L->gc_step(step_size);
        return MultiValue(clx::boolean(finished));
    }
    if (opt == "incremental" || opt == "generational") {
        const char *previous = (L->gc_mode == LState::GCMode::Generational) ? "generational" : "incremental";
        L->gc_mode = (opt == "incremental") ? LState::GCMode::Incremental : LState::GCMode::Generational;
        return MultiValue(clx::string(L, previous));
    }
    if (opt == "mode") {
        return MultiValue(clx::string(L, L->gc_mode == LState::GCMode::Generational ? "generational" : "incremental"));
    }
    if (opt == "stats") {
        LTable *st = static_cast<LTable *>(L->create_table(0, 8).as_pointer());
        st->settable(LValue(L->intern_string("mode")),
            clx::string(L, L->gc_mode == LState::GCMode::Generational ? "generational" : "incremental"));
        st->settable(LValue(L->intern_string("collects")), LValue(static_cast<int64_t>(L->gc_stats_collects)));
        st->settable(LValue(L->intern_string("minors")), LValue(static_cast<int64_t>(L->gc_stats_minors)));
        st->settable(
            LValue(L->intern_string("minor_freed_kb")), LValue(static_cast<double>(L->gc_stats_minor_freed) / 1024.0));
        st->settable(
            LValue(L->intern_string("major_freed_kb")), LValue(static_cast<double>(L->gc_stats_major_freed) / 1024.0));
        st->settable(LValue(L->intern_string("old_kb")), LValue(static_cast<double>(L->gc_old_bytes) / 1024.0));
        st->settable(LValue(L->intern_string("minor_kb")), LValue(static_cast<double>(L->gc_minor_threshold) / 1024.0));
        st->settable(
            LValue(L->intern_string("remembered_high")), LValue(static_cast<int64_t>(L->gc_stats_remembered_high)));
        return MultiValue(LValue(Table, st));
    }
    if (opt == "param") {
        if (arg_count < 2)
            clx::error(L, bad_arg_msg(L, 2, "collectgarbage", "option expected").c_str());
        std::string_view param = clx::check_string(L, args[1]);
        int *target = nullptr;
        int old_val;
        if (param == "pause")
            target = &L->gc_pause;
        else if (param == "stepmul")
            target = &L->gc_stepmul;
        else if (param == "stepsize")
            target = &L->gc_stepsize;
        else if (param == "minormul")
            target = &L->gc_minormul;
        else if (param == "majorminor")
            target = &L->gc_majorminor;
        else if (param == "minormajor")
            target = &L->gc_minormajor;
        else
            clx::error(L, "bad option parameter to 'collectgarbage'");
        old_val = *target;
        if (arg_count >= 3) {
            int new_val = static_cast<int>(clx::check_integer(L, args[2]));
            new_val = std::clamp(new_val, 0, 100000);
            *target = new_val;
        }
        return MultiValue(clx::integer(old_val));
    }
    return MultiValue(clx::number(0.0));
}

//------------------ clx_type: returns the type name of a value (global type function)
static MultiValue type(LState *L, const LValue *args, size_t count) {
    if (count == 0) {
        clx::error(L, bad_arg_msg(L, 1, "type", "value expected").c_str());
    }
    const char *name = VALUE_TYPE_NAMES[static_cast<uint8_t>(args[0].type)];
    if (args[0].type == ValueType::Int64)
        name = "number";
    return MultiValue(clx::string(L, name));
}

//------------------ clx_assert: asserts a condition (global assert function)
static MultiValue assert(LState *L, const LValue *args, size_t count) {
    //------------------ stock checks the argument itself first, so assert() with no argument is a bad argument
    if (count == 0)
        throw_runtime_error(bad_arg_msg(L, 1, "assert", "value expected"));

    if (!clx::to_boolean(args[0])) {
        //------------------ stock assert routes through error(), which prefixes a string message with
        //------------------ the caller's position (a non-string message is raised untouched)
        if (count > 1) {
            const LValue &msg = args[1];
            if (msg.type != ValueType::String)
                throw LRuntimeException(msg);
            std::string full = clx::file_line_prefix(L) + std::string(msg.as_string(), msg.string_len());
            throw LRuntimeException(clx::string(L, full.data(), full.size()));
        }
        std::string full = clx::file_line_prefix(L) + "assertion failed!";
        throw LRuntimeException(clx::string(L, full.data(), full.size()));
    }
    return MultiValue(args, count, L);
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
        int n = clx_format_double(buf, sizeof(buf), v.as_number());
        return MultiValue(clx::string(L, buf, (size_t)n));
    }
    case Int64: {
        char buf[32];
        int n = std::snprintf(buf, sizeof(buf), "%lld", (long long)v.as_integer());
        return MultiValue(clx::string(L, buf, (size_t)n));
    }
    default: {
        std::string sv = v.to_string(L);
        return MultiValue(clx::string(L, sv.data(), sv.size()));
    }
    }
}

//------------------ clx_tonumber: converts value to number (global tonumber function)
static MultiValue tonumber(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error(bad_arg_msg(L, 1, "tonumber", "value expected"));
    const LValue &v = args[0];
    if (count >= 2 && args[1].type != Nil) {
        int64_t base = clx::check_integer(L, args[1]);
        if (base < 2 || base > 36)
            clx::error(L, bad_arg_msg(L, 2, "tonumber", "base out of range").c_str());
        //------------------ stock b_str2int: walk the length-delimited numeral, rejecting any byte
        //------------------ that is not a digit of the base (so embedded NULs never truncate)
        const char *s = clx::check_string(L, v);
        const size_t len = v.string_len();
        size_t i = 0;
        while (i < len && std::isspace(static_cast<unsigned char>(s[i])))
            i++;
        bool neg = false;
        if (i < len && (s[i] == '-' || s[i] == '+')) {
            neg = (s[i] == '-');
            i++;
        }
        if (i >= len || !std::isalnum(static_cast<unsigned char>(s[i])))
            return MultiValue(LValue());
        uint64_t n = 0;
        for (; i < len && std::isalnum(static_cast<unsigned char>(s[i])); i++) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const int digit = std::isdigit(c) ? (c - '0') : (std::tolower(c) - 'a' + 10);
            if (digit < 0 || digit >= static_cast<int>(base))
                return MultiValue(LValue());
            n = n * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
        }
        while (i < len && std::isspace(static_cast<unsigned char>(s[i])))
            i++;
        if (i != len)
            return MultiValue(LValue());
        return MultiValue(clx::integer(static_cast<int64_t>(neg ? 0ull - n : n)));
    }
    if (v.type == Double || v.type == Int64)
        return MultiValue(v);
    if (clx::is_string(v)) {
        //------------------ stock rejects numerals carrying an embedded zero (luaO_str2num sees a prefix)
        if (std::strlen(v.as_string()) != v.string_len())
            return MultiValue(LValue());
        int64_t iv;
        if (clx::to_integer_coerced(v, iv))
            return MultiValue(clx::integer(iv));
        double d;
        if (v.to_number(d))
            return MultiValue(clx::number(d));
    }
    return MultiValue(LValue());
}

//------------------ clx_rawequal: compares two values without metamethods
static MultiValue lua_rawequal(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        throw_runtime_error(bad_arg_msg(L, 1, "rawequal", "value expected"));
    return MultiValue(clx::boolean(clx::rawequal(args[0], args[1])));
}

//------------------ clx_rawget: gets table value without metamethods
static MultiValue rawget(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        clx::error(L, bad_arg_msg(L, 1, "rawget", "table expected").c_str());
    clx::check_table(L, args[0]);
    return MultiValue(clx::raw_get(L, args[0], args[1]));
}

//------------------ clx_rawset: sets table value without metamethods
static MultiValue rawset(LState *L, const LValue *args, size_t count) {
    if (count < 3)
        clx::error(L, bad_arg_msg(L, 1, "rawset", "table expected").c_str());
    clx::check_table(L, args[0]);
    clx::raw_set(L, args[0], args[1], args[2]);
    return MultiValue(args[0]);
}

//------------------ clx_rawlen: returns raw length of string or table without __len
static MultiValue lua_rawlen(LState *L, const LValue *args, size_t count) {
    if (count < 1) {
        clx::error(L, bad_arg_msg(L, 1, "rawlen", "value expected").c_str());
    }
    if (args[0].type != ValueType::Table && args[0].type != ValueType::String)
        clx::error(L, bad_arg_msg(L, 1, "rawlen", "table or string expected").c_str());
    return MultiValue(clx::integer(clx::rawlen(args[0])));
}

//------------------ warnings_enabled: global flag tracking whether warnings are on/off
static bool warnings_enabled = true;

//------------------ clx_warn: issues a warning to stderr (global warn function)
static MultiValue warn(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        return MultiValue();

    if (clx::is_string(args[0])) {
        std::string_view s(args[0].as_string(), args[0].string_len());
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
        std::string s = args[i].to_string(L);
        if (i > 0)
            s.insert(0, " ");
        if (clx_io_write_ptr)
            clx_io_write_ptr(stderr, s.data(), s.size());
        else
            std::cerr << s;
    }
    if (clx_io_write_ptr)
        clx_io_write_ptr(stderr, "\n", 1);
    else
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
        clx::error(L, bad_arg_msg(L, 1, "next", "table expected").c_str());
    clx::check_table(L, args[0]);
    LValue key = (count > 1) ? args[1] : LValue();
    return clx::next(L, args[0], key);
}

//------------------ clx_pairs: returns iterator for key-value traversal (global pairs function)
static MultiValue pairs(LState *L, const LValue *args, size_t count) {
    if (count == 0) {
        clx::error(L, bad_arg_msg(L, 1, "pairs", "value expected").c_str());
    }
    LValue t = args[0];

    if (clx::is_table(t)) {
        LTable *tbl = static_cast<LTable *>(t.as_pointer());
        if (LTable *mt = tbl_metatable(tbl)) {
            LValue mt_pairs = mt->gettable(L->str_pairs);
            if (mt_pairs.type == Function) {
                static constexpr CallSite pairs_site = { __FILE__, __LINE__, false, false };
                MultiValue ret = call_function_rooted(L, mt_pairs, &t, 1, &pairs_site);
                return ret;
            }
        }
    }

    return MultiValue({ get_global(L, "next"), t, LValue(), LValue() }, L);
}

//------------------ clx_select: selects a range of arguments (global select function)
static clx::MultiValue select(clx::LState *L, const clx::LValue *args, size_t arg_count) {
    if (arg_count < 1) {
        clx::error(L, bad_arg_msg(L, 1, "select", "number expected, got no value").c_str());
    }

    const clx::LValue &arg1 = args[0];

    if (clx::is_string(arg1)) {
        if (arg1.string_len() == 1 && arg1.as_string()[0] == '#') {
            return clx::MultiValue(clx::integer(static_cast<int64_t>(arg_count) - 1));
        }
    }

    double d_idx;
    if (!arg1.to_number(d_idx)) {
        clx::error(L, bad_arg_msg(L, 1, "select", "number expected").c_str());
    }

    int64_t idx = static_cast<int64_t>(d_idx);
    int64_t n = static_cast<int64_t>(arg_count) - 1;

    if (idx < 0)
        idx = n + idx + 1;

    if (idx < 1) {
        clx::error(L, bad_arg_msg(L, 1, "select", "index out of range").c_str());
    }

    if (idx > n) {
        return clx::MultiValue();
    }

    std::vector<clx::LValue> ret_vals;
    ret_vals.reserve(n - idx + 1);
    for (int64_t i = idx; i <= n; ++i) {
        ret_vals.push_back(args[i]);
    }

    return clx::MultiValue(ret_vals, L);
}

//------------------ clx_ipairs_iter: iterator function for ipairs traversal
static MultiValue ipairs_iter(LState *L, const LValue *args, size_t count) {
    if (count < 2 || !clx::is_table(args[0]))
        return MultiValue();

    LTable *t = static_cast<LTable *>(args[0].as_pointer());
    int64_t idx = args[1].type == Int64 ? args[1].as_integer() : static_cast<int64_t>(args[1].as_number());
    //------------------ stock wraps the index on overflow (i.e. maxinteger -> mininteger)
    idx = static_cast<int64_t>(static_cast<uint64_t>(idx) + 1u);

    //------------------ stock uses lua_geti, so an __index metamethod supplies missing keys:
    //------------------ the raw LTable::gettable would stop the traversal at the first hole
    LValue val;
    if (idx >= 1 && static_cast<size_t>(idx) <= t->array_size && t->array_types[idx - 1] != clx::ValueType::Nil) {
        val = LValue(t->array[idx - 1], t->array_types[idx - 1]);
    } else {
        val = clx::table_get(L, LValue(clx::ValueType::Table, t), LValue(idx));
    }

    if (val.type == Nil)
        return MultiValue(clx::LValue());
    return MultiValue({ clx::integer(idx), val });
}

//------------------ clx_ipairs: returns iterator for array traversal (global ipairs function)
static MultiValue ipairs(LState *L, const LValue *args, size_t count) {
    if (count == 0) {
        clx::error(L, bad_arg_msg(L, 1, "ipairs", "value expected").c_str());
    }
    LValue t = args[0];

    if (clx::is_table(t)) {
        LTable *tbl = static_cast<LTable *>(t.as_pointer());
        if (LTable *mt = tbl_metatable(tbl)) {
            LValue mt_ipairs = mt->gettable(L->str_ipairs);
            if (mt_ipairs.type == Function) {
                static constexpr CallSite ipairs_site = { __FILE__, __LINE__, false, false };
                MultiValue ret = call_function_rooted(L, mt_ipairs, &t, 1, &ipairs_site);
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
    clx::set_field(L, g, "_VERSION", clx::string(L, "Lua 5.5"));
}

//------------------ Per-function registrars for filtered std startup (see luastd_base_filtered expose
//------------------ via the driver-emitted open sequence). Each references exactly one base function, so
//------------------ the linker strips every registrar — and its function — the program never uses.
#define CLX_BASE_REG(name, fn)                                                                                         \
    void luastd_base_##name(LState *L) {                                                                               \
        clx::set_function(L, LValue(Table, L->_G), #name, fn);                                                         \
    }
CLX_BASE_REG(print, print)
CLX_BASE_REG(require, require)
CLX_BASE_REG(error, lua_error)
CLX_BASE_REG(assert, assert)
CLX_BASE_REG(tostring, tostring)
CLX_BASE_REG(tonumber, tonumber)
CLX_BASE_REG(rawequal, lua_rawequal)
CLX_BASE_REG(rawget, rawget)
CLX_BASE_REG(rawset, rawset)
CLX_BASE_REG(rawlen, lua_rawlen)
CLX_BASE_REG(setmetatable, lua_setmetatable)
CLX_BASE_REG(getmetatable, lua_getmetatable)
CLX_BASE_REG(collectgarbage, collectgarbage)
CLX_BASE_REG(type, type)
CLX_BASE_REG(pcall, lua_pcall)
CLX_BASE_REG(xpcall, lua_xpcall)
CLX_BASE_REG(warn, warn)
CLX_BASE_REG(next, lua_next)
CLX_BASE_REG(pairs, pairs)
CLX_BASE_REG(ipairs, ipairs)
CLX_BASE_REG(select, select)
#undef CLX_BASE_REG

}
