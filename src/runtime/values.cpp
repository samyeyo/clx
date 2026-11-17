// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  values.cpp · LValue, errors, metamethods   │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include "clx_runtime.h"
#include <algorithm>
#include <clocale>
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

//------------------ clx_unwinding_error: error object of the exception being unwound right now
thread_local const clx::LValue *clx_unwinding_error = nullptr;

//------------------ LRuntimeException::LRuntimeException — error exception constructor
LRuntimeException::LRuntimeException(clx::LValue err)
    : error_obj(err)
    , prev_unwinding(clx_unwinding_error) {
    clx_unwinding_error = &error_obj;
}

//------------------ LRuntimeException::~LRuntimeException — exception destructor
LRuntimeException::~LRuntimeException() noexcept {
    clx_unwinding_error = prev_unwinding;
}

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

                static constexpr CallSite meta_site = { __FILE__, __LINE__, false, false };
                MultiValue ret = call_function_rooted(L, meta_func, args, 1, &meta_site);

                //------------------ stock's luaL_tolstring refuses a __tostring result that is not a
                //------------------ string instead of falling back to the default representation
                if (ret.count < 1 || ret[0].type != String)
                    throw LRuntimeException(LValue(L->intern_string(
                        file_line_prefix(L) + "'__tostring' must return a string")));
                return std::string(ret[0].as_string(), ret[0].string_len());
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
                LValue n = mt->gettable(L->str_name);
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
    case Thread: {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "thread: %p", as_pointer());
        return std::string(buf);
    }
    case UserData: {
        const char *prefix = "userdata";
        if (L) {
            LUserdata *ud = static_cast<LUserdata *>(as_pointer());
            if (ud->metatable) {
                LValue n = ud->metatable->gettable(L->str_name);
                if (n.type != Nil && n.type == String)
                    prefix = n.as_string();
            }
        }
        char ptr[32];
        std::snprintf(ptr, sizeof(ptr), "%p", as_pointer());
        return std::string(prefix) + ": " + ptr;
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

//------------------ g_collate_is_c, g_ctype_is_c: true while LC_COLLATE/LC_CTYPE are the C locale, where string
//------------------ ordering is a byte comparison and case folding only covers ASCII. os.setlocale refreshes
//------------------ them; outside the C locale stock defers to strcoll and to the host's character tables
static bool g_collate_is_c = true;
static bool g_ctype_is_c = true;
static uint64_t g_locale_generation = 0;

//------------------ collate_is_c: reports whether string ordering may skip the host's collation rules
bool collate_is_c() { return g_collate_is_c; }

//------------------ ctype_is_c: reports whether character classes and case folding cover ASCII only
bool ctype_is_c() { return g_ctype_is_c; }

//------------------ locale_generation: the number of locale changes so far
uint64_t locale_generation() { return g_locale_generation; }

//------------------ refresh_locale_flags: re-reads LC_COLLATE/LC_CTYPE after os.setlocale changed them
void refresh_locale_flags() {
    const char *collate = std::setlocale(LC_COLLATE, nullptr);
    const char *ctype = std::setlocale(LC_CTYPE, nullptr);
    g_collate_is_c = !collate || std::strcmp(collate, "C") == 0 || std::strcmp(collate, "POSIX") == 0;
    g_ctype_is_c = !ctype || std::strcmp(ctype, "C") == 0 || std::strcmp(ctype, "POSIX") == 0;
    g_locale_generation++;
}

//------------------ collate_strcmp: stock's l_strcmp - strcoll segment by segment, so strings that share a
//------------------ prefix up to an embedded zero are still ordered the way stock orders them
static int collate_strcmp(std::string_view l, std::string_view r) {
    for (;;) {
        std::string lz(l);
        std::string rz(r);
        int temp = std::strcoll(lz.c_str(), rz.c_str());
        if (temp != 0)
            return temp;
        size_t len = std::strlen(lz.c_str());
        if (len == std::strlen(rz.c_str())) {
            if (len == l.size())
                return 0;
            l.remove_prefix(len + 1);
            r.remove_prefix(len + 1);
        } else {
            return (len == l.size()) ? -1 : 1;
        }
    }
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
        if (!g_collate_is_c)
            return LValue(collate_strcmp(std::string_view(as_string(), al), std::string_view(other.as_string(), bl))
                < 0);
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
        if (!g_collate_is_c)
            return LValue(collate_strcmp(std::string_view(as_string(), al), std::string_view(other.as_string(), bl))
                <= 0);
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

//------------------ metatable_of — the metatable of a value for every type, like luaT_gettmbyobj
LTable *metatable_of(LState *L, const LValue &v) {
    switch (v.type) {
    case Table:
        return tbl_metatable(static_cast<LTable *>(v.as_pointer()));
    case UserData:
        return static_cast<LUserdata *>(v.as_pointer())->metatable;
    case String:
        return L->string_metatable;
    default:
        return L->type_metatables[type_meta_index(v.type)];
    }
}

//------------------ find_bin_metamethod — returns the binary-op metamethod from either operand's metatable, or Nil
LValue find_bin_metamethod(LState *L, const LValue &a, const LValue &b, const LValue &key) {
    const LValue *operands[2] = { &a, &b };
    for (const LValue *op : operands) {
        LTable *mt = metatable_of(L, *op);
        if (mt) {
            LValue method = mt->gettable(key);
            if (method.type != Nil && method.type == Function)
                return method;
        }
    }
    return LValue();
}

//------------------ find_bin_metamethod (event-name overload: interns and forwards)
LValue find_bin_metamethod(LState *L, const LValue &a, const LValue &b, const char *event) {
    return find_bin_metamethod(L, a, b, LValue(L->intern_string(event)));
}

//------------------ call_bin_metamethod — call binary op metamethod
LValue call_bin_metamethod(LState *L, const LValue &a, const LValue &b, const LValue &key, const char *event) {
    LValue method = find_bin_metamethod(L, a, b, key);
    if (method.type == Function) {
        LValue args[2] = { a, b };
        static constexpr CallSite method_site = { __FILE__, __LINE__, false, false };
        MultiValue res = call_function_rooted(L, method, args, 2, &method_site);
        return res[0];
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

    auto bad_type_name = [](const LValue &v) {
        size_t idx
            = (v.type == ValueType::Int64) ? static_cast<size_t>(ValueType::Double) : static_cast<size_t>(v.type);
        return VALUE_TYPE_NAMES[idx];
    };

    if (ev == "__band" || ev == "__bor" || ev == "__bxor" || ev == "__bnot" || ev == "__shl" || ev == "__shr") {
        bool a_is_number = (a.type == ValueType::Int64 || a.type == ValueType::Double);
        bool b_is_number = (b.type == ValueType::Int64 || b.type == ValueType::Double);
        if (a_is_number && b_is_number)
            throw LRuntimeException(LValue(L->intern_string(prefix + "number has no integer representation")));
        const LValue &bad = a_is_number ? b : a;
        throw LRuntimeException(LValue(
            L->intern_string(prefix + "attempt to perform bitwise operation on a " + bad_type_name(bad) + " value")));
    }

    std::string op_type = "perform arithmetic on";
    if (ev == "__concat")
        op_type = "concatenate";
    else if (ev == "__len")
        op_type = "get length of";

    bool a_is_number = (a.type == ValueType::Int64 || a.type == ValueType::Double);
    bool a_is_concatable = (a_is_number || a.type == ValueType::String);
    const LValue &bad = (ev == "__concat") ? (a_is_concatable ? b : a) : (a_is_number ? b : a);

    std::string err_msg = prefix + "attempt to " + op_type + " a " + bad_type_name(bad) + " value";
    throw LRuntimeException(LValue(L->intern_string(err_msg)));
}

//------------------ call_bin_metamethod (event-name overload: interns and forwards)
LValue call_bin_metamethod(LState *L, const LValue &a, const LValue &b, const char *event) {
    return call_bin_metamethod(L, a, b, LValue(L->intern_string(event)), event);
}

}
