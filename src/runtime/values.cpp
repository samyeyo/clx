// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  values.cpp · LValue, errors, metamethods   │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include "clx_runtime.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace clx {

//------------------ intern_error_message — interns an error message raised without an LState
const char *intern_error_message(const char *msg, size_t len) {
    static std::mutex pool_mutex;
    static StringPool pool;
    uint64_t h = len <= 8 ? swar_hash_8(msg, len) : wyhash_str(msg, len);
    std::lock_guard<std::mutex> guard(pool_mutex);
    return pool.intern(msg, len, h);
}

//------------------ LRuntimeException::LRuntimeException — error exception constructor
LRuntimeException::LRuntimeException(clx::LValue err)
    : error_obj(err) { }

//------------------ LRuntimeException::~LRuntimeException — exception destructor
LRuntimeException::~LRuntimeException() noexcept { }

//------------------ LRuntimeException::what — get error message
const char *LRuntimeException::what() const noexcept {
    if (cached_msg.empty()) {
        cached_msg = error_obj.to_string(nullptr);
    }
    return cached_msg.c_str();
}

//------------------ LValue::to_string — convert value to string
std::string LValue::to_string(LState *L) const {
    if (L && (type == Table || type == UserData)) {
        LTable *mt = (type == Table) ? tbl_metatable(static_cast<LTable *>(as_pointer()))
                                     : static_cast<LUserdata *>(as_pointer())->metatable;
        if (mt) {
            LValue meta_key = L->str_tostring;
            LValue meta_func = mt->gettable(meta_key);

            if (meta_func.type != Nil && meta_func.type == Function) {
                LValue args[1];
                args[0] = *this;

                MultiValue ret = call_function_rooted(L, meta_func, args, 1, __FILE__, __LINE__);

                if (ret.count > 0 && ret[0].type == String) {
                    return std::string(ret[0].as_string(), ret[0].string_len());
                }
            }
        }
    }

    switch (type) {
    case Nil:
        return "nil";
    case Boolean:
        return as_bool() ? "true" : "false";
    case Double: {
        char buf[64];
        int n = clx_format_double(buf, sizeof(buf), as_number());
        return std::string(buf, (size_t)n);
    }
    case Int64:
        return std::to_string(as_integer());
    case String:
        return std::string(as_string(), string_len());
    case Table: {
        const char *prefix = "table";
        if (L) {
            LTable *mt = tbl_metatable(static_cast<LTable *>(as_pointer()));
            if (mt) {
                LValue n = mt->gettable(LValue(L->intern_string("__name")));
                if (n.type != Nil && n.type == String)
                    prefix = n.as_string();
            }
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s: %p", prefix, as_pointer());
        return std::string(buf);
    }
    case Function: {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "function: %p", as_pointer());
        return std::string(buf);
    }
    default:
        return "userdata";
    }
}

//------------------ LValue::slow_eq — equality comparison
LValue LValue::slow_eq(const LValue &other) const {
    if (type != other.type)
        return LValue(false);
    if (type == String) {
        return LValue(
            string_len() == other.string_len() && clx_memcmp(as_string(), other.as_string(), string_len()) == 0);
    }
    return LValue(false);
}

//------------------ LValue::slow_lt — less-than comparison
LValue LValue::slow_lt(const LValue &other) const {
    if ((type == Double || type == Int64) && (other.type == Double || other.type == Int64)) {
        double l = (type == Int64) ? (double)as_integer() : as_number();
        double r = (other.type == Int64) ? (double)other.as_integer() : other.as_number();
        return LValue(l < r);
    }
    if (type == String && other.type == String) {
        size_t al = string_len(), bl = other.string_len();
        int cmp = clx_memcmp(as_string(), other.as_string(), std::min(al, bl));
        if (cmp != 0)
            return LValue(cmp < 0);
        return LValue(al < bl);
    }
    return LValue(false);
}

//------------------ LValue::slow_le — less-or-equal comparison
LValue LValue::slow_le(const LValue &other) const {
    if ((type == Double || type == Int64) && (other.type == Double || other.type == Int64)) {
        double l = (type == Int64) ? (double)as_integer() : as_number();
        double r = (other.type == Int64) ? (double)other.as_integer() : other.as_number();
        return LValue(l <= r);
    }
    if (type == String && other.type == String) {
        size_t al = string_len(), bl = other.string_len();
        int cmp = clx_memcmp(as_string(), other.as_string(), std::min(al, bl));
        if (cmp != 0)
            return LValue(cmp <= 0 ? cmp < 0 : false);
        return LValue(al <= bl);
    }
    return LValue(false);
}

//------------------ LCFunction::LCFunction — C function wrapper constructor
LCFunction::LCFunction(CFunctionType f)
    : func(std::move(f)) {
    type = static_cast<uint8_t>(Function);
    marked = 0;
    next = nullptr;
}

//------------------ call_bin_metamethod — call binary op metamethod
LValue call_bin_metamethod(LState *L, const LValue &a, const LValue &b, const char *event) {
    LTable *mt = nullptr;
    if (a.type == Table)
        mt = tbl_metatable(static_cast<LTable *>(a.as_pointer()));
    else if (a.type == UserData)
        mt = static_cast<LUserdata *>(a.as_pointer())->metatable;
    if (!mt && b.type == Table)
        mt = tbl_metatable(static_cast<LTable *>(b.as_pointer()));
    else if (!mt && b.type == UserData)
        mt = static_cast<LUserdata *>(b.as_pointer())->metatable;

    if (mt) {
        LValue method = mt->gettable(LValue(L->intern_string(event)));
        if (method.type != Nil && method.type == Function) {
            LValue args[2] = { a, b };
            MultiValue res = call_function_rooted(L, method, args, 2, __FILE__, __LINE__);
            return res[0];
        }
    }

    std::string_view ev(event);

    if (ev == "__eq")
        return clx::LValue(false);

    std::string prefix = file_line_prefix(L);

    if (ev == "__lt" || ev == "__le") {
        auto type_name = [](const LValue &v) {
            size_t idx
                = (v.type == ValueType::Int64) ? static_cast<size_t>(ValueType::Double) : static_cast<size_t>(v.type);
            return VALUE_TYPE_NAMES[idx];
        };
        throw LRuntimeException(
            LValue(L->intern_string(prefix + "attempt to compare " + type_name(a) + " with " + type_name(b))));
    }

    std::string op_type = "perform arithmetic on";
    if (ev == "__concat")
        op_type = "concatenate";
    else if (ev == "__len")
        op_type = "get length of";
    else if (ev == "__band" || ev == "__bor" || ev == "__bxor" || ev == "__bnot" || ev == "__shl" || ev == "__shr")
        op_type = "perform bitwise operation on";

    bool a_is_number = (a.type == ValueType::Int64 || a.type == ValueType::Double);
    bool a_is_concatable = (a_is_number || a.type == ValueType::String);
    const LValue &bad = (ev == "__concat") ? (a_is_concatable ? b : a) : (a_is_number ? b : a);

    std::string err_msg
        = prefix + "attempt to " + op_type + " a " + VALUE_TYPE_NAMES[static_cast<size_t>(bad.type)] + " value";
    throw LRuntimeException(LValue(L->intern_string(err_msg)));
}

}
