// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  builtins.h · Std module builtin registry   │
// └─────────────────────────────────────────────┘

#ifndef CLX_BUILTINS_H
#define CLX_BUILTINS_H

#include <cstdint>
#include <string_view>

namespace clx {

//------------------ BuiltinEntry: statically known std module function and how every lowering path reaches it
struct BuiltinEntry {
    std::string_view module;
    std::string_view name;
    const char *intrinsic;
    const char *cfunc;
    uint8_t min_args;
    bool alias_ok;
    bool numeric;
};

//------------------ BuiltinConst: foldable std module constant (boxed LValue expr, native double expr)
struct BuiltinConst {
    std::string_view module;
    std::string_view name;
    const char *boxed;
    const char *native;
    bool is_int;
};

//------------------ builtin_lookup: registry entry for module.name, else nullptr
inline const BuiltinEntry *builtin_lookup(std::string_view module, std::string_view name) {
    static constexpr BuiltinEntry entries[] = {
        //------------------ math (intrinsics, 2-arg list, extension-only entries)
        { "math", "sin", "std::sin", "math_sin", 1, true, true },
        { "math", "cos", "std::cos", "math_cos", 1, true, true },
        { "math", "sqrt", "std::sqrt", "math_sqrt", 1, true, true },
        { "math", "exp", "std::exp", "math_exp", 1, true, true },
        { "math", "tan", "std::tan", "math_tan", 1, true, true },
        { "math", "atan", "std::atan", "math_atan", 1, true, true },
        { "math", "asin", "std::asin", "math_asin", 1, true, true },
        { "math", "acos", "std::acos", "math_acos", 1, true, true },
        { "math", "sinh", "std::sinh", nullptr, 1, false, true },
        { "math", "cosh", "std::cosh", nullptr, 1, false, true },
        { "math", "tanh", "std::tanh", nullptr, 1, false, true },
        { "math", "atan2", "std::atan2", nullptr, 2, false, true },
        { "math", "pow", "std::pow", nullptr, 2, false, true },
        { "math", "fmod", nullptr, "math_fmod", 2, true, true },
        { "math", "deg", "__clx_deg", "math_deg", 1, true, true },
        { "math", "rad", "__clx_rad", "math_rad", 1, true, true },
        //------------------ math (cfunc only)
        { "math", "floor", nullptr, "math_floor", 1, true, true },
        { "math", "ceil", nullptr, "math_ceil", 1, true, true },
        { "math", "abs", nullptr, "math_abs", 1, true, true },
        { "math", "log", nullptr, "math_log", 1, true, true },
        { "math", "max", nullptr, "math_max", 1, true, true },
        { "math", "min", nullptr, "math_min", 1, true, true },
        { "math", "ldexp", nullptr, "math_ldexp", 1, true, false },
        { "math", "tointeger", nullptr, "math_tointeger", 1, true, false },
        { "math", "frexp", nullptr, "math_frexp", 1, true, false },
        { "math", "modf", nullptr, "math_modf", 1, true, false },
        { "math", "random", nullptr, "math_random", 1, true, false },
        { "math", "randomseed", nullptr, "math_randomseed", 1, true, false },
        { "math", "type", nullptr, "math_type", 1, true, false },
        { "math", "ult", nullptr, "math_ult", 2, true, false },
        //------------------ string (cfunc only)
        { "string", "byte", nullptr, "str_byte", 1, true, false },
        { "string", "sub", nullptr, "str_sub", 1, true, false },
        { "string", "match", nullptr, "str_match", 1, true, false },
        { "string", "find", nullptr, "str_find", 1, true, false },
        { "string", "gsub", nullptr, "str_gsub", 1, true, false },
        { "string", "gmatch", nullptr, "str_gmatch", 1, true, false },
        { "string", "len", nullptr, "str_len", 1, true, false },
        { "string", "format", nullptr, "str_format", 1, true, false },
        { "string", "char", nullptr, "str_char", 1, true, false },
        { "string", "rep", nullptr, "str_rep", 2, true, false },
        { "string", "reverse", nullptr, "str_reverse", 1, true, false },
        { "string", "lower", nullptr, "str_lower", 1, true, false },
        { "string", "upper", nullptr, "str_upper", 1, true, false },
        { "string", "dump", nullptr, "str_dump", 1, true, false },
        { "string", "pack", nullptr, "str_pack", 1, true, false },
        { "string", "packsize", nullptr, "str_packsize", 1, true, false },
        { "string", "unpack", nullptr, "str_unpack", 1, true, false },
        //------------------ os (cfunc only)
        { "os", "clock", nullptr, "os_clock", 0, true, true },
        { "os", "date", nullptr, "os_date", 0, true, false },
        { "os", "difftime", nullptr, "os_difftime", 2, true, true },
        { "os", "execute", nullptr, "os_execute", 0, true, false },
        { "os", "exit", nullptr, "os_exit", 0, true, false },
        { "os", "getenv", nullptr, "os_getenv", 1, true, false },
        { "os", "remove", nullptr, "os_remove", 1, true, false },
        { "os", "rename", nullptr, "os_rename", 2, true, false },
        { "os", "setlocale", nullptr, "os_setlocale", 0, true, false },
        { "os", "time", nullptr, "os_time", 0, true, true },
        { "os", "tmpname", nullptr, "os_tmpname", 0, true, false },
        //------------------ table (cfunc only)
        { "table", "concat", nullptr, "table_concat", 1, true, false },
        { "table", "insert", nullptr, "table_insert", 2, true, false },
        { "table", "remove", nullptr, "table_remove", 1, true, false },
        { "table", "sort", nullptr, "table_sort", 1, true, false },
        { "table", "pack", nullptr, "table_pack", 1, true, false },
        { "table", "unpack", nullptr, "table_unpack", 1, true, false },
        { "table", "move", nullptr, "table_move", 5, true, false },
        { "table", "create", nullptr, "table_create", 0, true, false },
        //------------------ _G (intrinsic only — no clx:: CFunc symbols exist)
        { "_G", "type", "__clx_type", nullptr, 1, true, false },
        { "_G", "tostring", "__clx_tostring", nullptr, 1, true, false },
    };
    for (const auto &e : entries) {
        if (e.module == module && e.name == name)
            return &e;
    }
    return nullptr;
}

//------------------ builtin_const_lookup: registry constant for module.name, else nullptr
inline const BuiltinConst *builtin_const_lookup(std::string_view module, std::string_view name) {
    static constexpr BuiltinConst consts[] = {
        { "math", "huge", "clx::LValue(static_cast<double>(INFINITY))", "INFINITY", false },
        { "math", "pi", "clx::LValue(static_cast<double>(3.141592653589793))", "3.141592653589793", false },
        { "math", "maxinteger", "clx::integer(static_cast<int64_t>(9223372036854775807))",
            "static_cast<int64_t>(9223372036854775807)", true },
        { "math", "mininteger", "clx::integer(static_cast<int64_t>(-9223372036854775807LL - 1))",
            "-9223372036854775807LL - 1", true },
    };
    for (const auto &c : consts) {
        if (c.module == module && c.name == name)
            return &c;
    }
    return nullptr;
}

//------------------ kStdModules: std module globals known to the registry (write-scan coverage)
inline constexpr std::string_view kStdModules[] = { "math", "string", "table", "os", "io", "utf8", "coroutine",
    "debug", "package", "base", "_G" };

//------------------ is_std_module_name: name is a std module known to the registry
inline bool is_std_module_name(std::string_view name) {
    for (const auto &m : kStdModules) {
        if (m == name)
            return true;
    }
    return false;
}

}

#endif
