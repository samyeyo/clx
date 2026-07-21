// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  clx.h · Public C++ API Header              │
// └─────────────────────────────────────────────┘

#ifndef CLX_H
#define CLX_H

#include <clx_runtime.h>
#include "clx_simd.h"

namespace clx {

//------------------ Creates a nil LValue
CLX_INLINE_HOT LValue nil()                           { return LValue(); }
//------------------ Creates a boolean LValue
CLX_INLINE_HOT LValue boolean(bool v)                 { return LValue(v); }
//------------------ Creates a number LValue
CLX_INLINE_HOT LValue number(double v)                { return LValue(v); }
//------------------ Creates an integer LValue
CLX_INLINE_HOT LValue integer(int64_t v)              { return LValue(v); }
//------------------ Creates a string LValue
CLX_INLINE LValue string(LState* L, const char* s, size_t len) {
    return make_string(L, s, len);
}

//------------------ Creates a string LValue
CLX_INLINE LValue string(LState* L, const char* s) {
    return make_string(L, s);
}

//------------------ Creates a table LValue
CLX_INLINE LValue table(LState* L, size_t as = 0, size_t hs = 0) {
    return L->create_table(as, hs);
}

//------------------ Creates a C function LValue
CLX_INLINE LValue cfunction(LState* L, CFunctionType func) {
    return L->create_closure(std::move(func));
}

//------------------ Sets the environment table of a function
CLX_INLINE void set_fenv(LState* L, const LValue& func, const LValue& env) {
    (void)L;
    if (func.type != ValueType::Function) return;
    LCFunction* f = static_cast<LCFunction*>(func.as_pointer());
    f->env = (env.type == ValueType::Table) ? static_cast<LTable*>(env.as_pointer()) : nullptr;
}

//------------------ Gets the environment table of a function
CLX_INLINE LValue get_fenv(LState* L, const LValue& func) {
    (void)L;
    if (func.type != ValueType::Function) return LValue();
    LCFunction* f = static_cast<LCFunction*>(func.as_pointer());
    return f->env ? LValue(ValueType::Table, f->env) : LValue();
}

//------------------ Creates a thread LValue
CLX_INLINE_HOT LValue thread(LThread* t)              { return LValue(ValueType::Thread, t); }
//------------------ Creates a light userdata LValue
CLX_INLINE_HOT LValue lightuserdata(void* p)          { return LValue(ValueType::UserData, reinterpret_cast<LHeader*>(p)); }

//------------------ Returns type enum of a value
CLX_INLINE_HOT ValueType type_of(const LValue& v)        { return v.type; }
//------------------ Returns type name string from enum
CLX_INLINE_HOT const char* type_name(ValueType t)        { return VALUE_TYPE_NAMES[static_cast<size_t>(t)]; }
//------------------ Checks if value is nil
CLX_INLINE_HOT bool   is_nil(const LValue& v)          { return v.type == ValueType::Nil; }
//------------------ Checks if value is boolean
CLX_INLINE_HOT bool   is_bool(const LValue& v)         { return v.type == ValueType::Boolean; }
//------------------ Checks if value is number or integer
CLX_INLINE_HOT bool   is_number(const LValue& v)       { return v.type == ValueType::Double || v.type == ValueType::Int64; }
//------------------ Checks if value is integer
CLX_INLINE_HOT bool   is_integer(const LValue& v)      { return v.type == ValueType::Int64; }
//------------------ Checks if value is string
CLX_INLINE_HOT bool   is_string(const LValue& v)       { return v.type == ValueType::String; }
//------------------ Checks if value is table
CLX_INLINE_HOT bool   is_table(const LValue& v)        { return v.type == ValueType::Table; }
//------------------ Checks if value is function
CLX_INLINE_HOT bool   is_function(const LValue& v)     { return v.type == ValueType::Function; }
//------------------ Checks if value is thread
CLX_INLINE_HOT bool   is_thread(const LValue& v)       { return v.type == ValueType::Thread; }
//------------------ Checks if value is userdata
CLX_INLINE_HOT bool   is_userdata(const LValue& v)     { return v.type == ValueType::UserData; }
//------------------ Always false (API compat)
CLX_INLINE_HOT bool   is_none(const LValue& v)         { return false; }
//------------------ Checks if value is nil
CLX_INLINE_HOT bool   is_noneornil(const LValue& v)    { return v.type == ValueType::Nil; }

//------------------ Checks if current thread can yield
CLX_INLINE_HOT bool isyieldable(LState* L) {
    return !L->running_thread->is_main;
}

//------------------ Gets coroutine thread status
CLX_INLINE_HOT int status(LThread* t) {
    return static_cast<int>(t->status);
}

//------------------ Converts value to double (with default)
CLX_INLINE_HOT double to_number(const LValue& v, double def = 0.0) {
    double out;
    return v.to_number(out) ? out : def;
}

//------------------ Converts to int64_t, reports success
CLX_INLINE int64_t to_integerx(const LValue& v, bool* isnum = nullptr) {
    int64_t out;
    bool ok = to_integer(v, out);
    if (isnum) *isnum = ok;
    return ok ? out : 0;
}

//------------------ Converts to double, reports success
CLX_INLINE double to_numberx(const LValue& v, bool* isnum = nullptr) {
    double out;
    bool ok = v.to_number(out);
    if (isnum) *isnum = ok;
    return ok ? out : 0.0;
}

//------------------ Truthy check / converts to bool
CLX_INLINE_HOT bool to_boolean(const LValue& v) {
    return v.as_bool();
}

//------------------ Converts value to const char* (via intern)
CLX_INLINE const char* to_string(LState* L, const LValue& v, const char* def = "") {
    if (v.type == ValueType::String) return v.as_string();
    if (v.type == ValueType::Double || v.type == ValueType::Int64)
        return L->intern_string(v.to_string(L));
    return def;
}

//------------------ Gets userdata pointer
CLX_INLINE void* touserdata(const LValue& v) {
    return v.as_pointer();
}

//------------------ Gets LThread pointer
CLX_INLINE LThread* tothread(const LValue& v) {
    return static_cast<LThread*>(v.as_pointer());
}

//------------------ Gets raw pointer
CLX_INLINE const void* topointer(const LValue& v) {
    return v.as_pointer();
}

//------------------ Converts string to number
CLX_INLINE_HOT LValue stringtonumber(LState* L, const char* s) {
    LValue v = string(L, s);
    double d;
    if (v.to_number(d)) return number(d);
    return nil();
}

//------------------ Checks and returns userdata (throws)
CLX_INLINE_COLD LUserdata* check_userdata(LState* L, const LValue& v) {
    if (v.type == ValueType::UserData) return static_cast<LUserdata*>(v.as_pointer());
    std::string msg = std::string("userdata expected, got ") + VALUE_TYPE_NAMES[static_cast<size_t>(v.type)];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Checks and returns double (throws)
CLX_INLINE_COLD double check_number(LState* L, const LValue& v) {
    double out;
    if (v.to_number(out)) return out;
    std::string msg = std::string("number expected, got ") + VALUE_TYPE_NAMES[static_cast<size_t>(v.type)];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Checks and returns int64_t (throws)
CLX_INLINE_COLD int64_t check_integer(LState* L, const LValue& v) {
    int64_t out;
    if (to_integer(v, out)) return out;
    std::string msg = std::string("integer expected, got ") + VALUE_TYPE_NAMES[static_cast<size_t>(v.type)];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Checks and returns string (throws)
CLX_INLINE_COLD const char* check_string(LState* L, const LValue& v) {
    if (v.type == ValueType::String) return v.as_string();
    std::string msg = std::string("string expected, got ") + VALUE_TYPE_NAMES[static_cast<size_t>(v.type)];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Checks and returns LTable* (throws)
CLX_INLINE_COLD LTable* check_table(LState* L, const LValue& v) {
    if (v.type == ValueType::Table) return static_cast<LTable*>(v.as_pointer());
    std::string msg = std::string("table expected, got ") + VALUE_TYPE_NAMES[static_cast<size_t>(v.type)];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Checks and returns LCFunction* (throws)
CLX_INLINE_COLD LCFunction* check_function(LState* L, const LValue& v) {
    if (v.type == ValueType::Function) return static_cast<LCFunction*>(v.as_pointer());
    std::string msg = std::string("function expected, got ") + VALUE_TYPE_NAMES[static_cast<size_t>(v.type)];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Optional number with default
CLX_INLINE_COLD double opt_number(LState* L, const LValue& v, double def) {
    if (v.type == ValueType::Nil) return def;
    return check_number(L, v);
}

//------------------ Optional integer with default
CLX_INLINE_COLD int64_t opt_integer(LState* L, const LValue& v, int64_t def) {
    if (v.type == ValueType::Nil) return def;
    return check_integer(L, v);
}

//------------------ Optional string with default
CLX_INLINE_COLD const char* opt_string(LState* L, const LValue& v, const char* def) {
    if (v.type == ValueType::Nil) return def;
    return check_string(L, v);
}

//------------------ Reads a string-keyed field from a table
CLX_INLINE LValue get_field(LState* L, const LValue& table, const char* key) {
    return table_get(L, table, LValue(L->intern_string(key)));
}

//------------------ Writes a string-keyed field to a table
CLX_INLINE void set_field(LState* L, const LValue& table, const char* key, const LValue& val) {
    table_set(L, table, LValue(L->intern_string(key)), val);
}

//------------------ Gets table value bypassing metamethods
CLX_INLINE LValue raw_get(LState* L, const LValue& table, const LValue& key) {
    LTable* t = check_table(L, table);
    return t->gettable(key);
}

//------------------ Sets table value bypassing metamethods
CLX_INLINE void raw_set(LState* L, const LValue& table, const LValue& key, const LValue& val) {
    LTable* t = check_table(L, table);
    t->settable(key, val);
}

//------------------ Gets integer-keyed table value (fast)
CLX_INLINE LValue raw_get_i(LState* L, const LValue& table, int64_t idx) {
    return table_get_int(L, table, static_cast<size_t>(idx));
}

//------------------ Sets integer-keyed table value (fast)
CLX_INLINE void raw_set_i(LState* L, const LValue& table, int64_t idx, const LValue& val) {
    LTable* t = check_table(L, table);
    if (static_cast<uint64_t>(idx - 1) < t->array_cap) {
        t->array[idx - 1] = val.val;
        t->array_types[idx - 1] = val.type;
        if (static_cast<size_t>(idx) > t->array_size) t->array_size = static_cast<size_t>(idx);
    } else {
        t->settable(LValue(static_cast<int64_t>(idx)), val);
    }
}

//------------------ Binds a C function to a table
CLX_INLINE void set_function(LState* L, const LValue& table, const char* name, CFunctionType func) {
    if (table.type != ValueType::Table) return;
    static_cast<LTable*>(table.as_pointer())->bind(L, name, std::move(func));
}

//------------------ Sets a named value on a table
CLX_INLINE void set_value(LState* L, const LValue& table, const char* name, const LValue& val) {
    if (table.type != ValueType::Table) return;
    static_cast<LTable*>(table.as_pointer())->settable(LValue(L->intern_string(name)), val);
}

//------------------ Binds multiple C functions (via LReg*)
CLX_INLINE void set_functions(LState* L, const LValue& table, std::initializer_list<LReg> funcs) {
    if (table.type != ValueType::Table) return;
    static_cast<LTable*>(table.as_pointer())->bind_all(L, funcs);
}

//------------------ Creates a library table from LReg array
CLX_INLINE LValue new_lib(LState* L, const LReg* regs) {
    LValue t = L->create_table();
    LTable* tbl = static_cast<LTable*>(t.as_pointer());
    for (const LReg* r = regs; r->name; ++r)
        tbl->bind(L, r->name, r->func);
    return t;
}

//------------------ Binds multiple C functions (via LReg*)
CLX_INLINE void set_functions(LState* L, const LValue& table, const LReg* regs) {
    LTable* tbl = check_table(L, table);
    for (const LReg* r = regs; r->name; ++r)
        tbl->bind(L, r->name, r->func);
}

//------------------ Gets metatable of table/userdata
CLX_INLINE LValue getmetatable(LState* L, const LValue& obj) {
    LTable* mt = nullptr;
    if (obj.type == ValueType::Table) {
        mt = static_cast<LTable*>(obj.as_pointer())->metatable;
    } else if (obj.type == ValueType::UserData) {
        mt = static_cast<LUserdata*>(obj.as_pointer())->metatable;
    }
    if (mt) {
        LValue protected_mt = mt->gettable(LValue(L->intern_string("__metatable")));
        if (protected_mt.type != ValueType::Nil) {
            return protected_mt;
        }
        return LValue(ValueType::Table, mt);
    }
    return LValue();
}

//------------------ Sets metatable of table/userdata
CLX_INLINE void setmetatable(LState* L, const LValue& obj, const LValue& mt) {
    if (obj.type != ValueType::Table && obj.type != ValueType::UserData) {
        throw_runtime_error("bad argument to 'setmetatable' (table or userdata expected)");
    }
    if (mt.type != ValueType::Table && mt.type != ValueType::Nil) {
        throw_runtime_error("bad argument #2 to 'setmetatable' (nil or table expected)");
    }
    LTable* new_mt = (mt.type == ValueType::Table) ? static_cast<LTable*>(mt.as_pointer()) : nullptr;
    if (obj.type == ValueType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        if (t->metatable) {
            LValue protected_mt = t->metatable->gettable(LValue(L->intern_string("__metatable")));
            if (protected_mt.type != ValueType::Nil) {
                throw_runtime_error("cannot change a protected metatable");
            }
        }
        t->metatable = new_mt;
        t->hash_version++;
    } else if (obj.type == ValueType::UserData) {
        static_cast<LUserdata*>(obj.as_pointer())->metatable = new_mt;
    }
}

//------------------ Raw equality without metamethods
CLX_INLINE_HOT bool rawequal(const LValue& a, const LValue& b) {
    if (a.type == ValueType::Int64 && b.type == ValueType::Int64)
        return a.as_integer() == b.as_integer();
    if (a.type == ValueType::Double && b.type == ValueType::Double)
        return a.as_number() == b.as_number();
    if (a.type == ValueType::Int64 && b.type == ValueType::Double)
        return static_cast<double>(a.as_integer()) == b.as_number();
    if (a.type == ValueType::Double && b.type == ValueType::Int64)
        return a.as_number() == static_cast<double>(b.as_integer());
    if (a.type != b.type) return false;
    if (a.type == ValueType::String)  return a.val.payload.u64 == b.val.payload.u64;
    if (a.type == ValueType::Boolean) return a.as_bool() == b.as_bool();
    if (a.type == ValueType::Nil)     return true;
    return a.as_pointer() == b.as_pointer();
}

//------------------ Table iteration primitive
CLX_INLINE MultiValue next(LState* L, const LValue& table, const LValue& key) {
    if (table.type != ValueType::Table) {
        throw_runtime_error("bad argument #1 to 'next' (table expected)");
    }
    LTable* t = static_cast<LTable*>(table.as_pointer());

    if (key.type == ValueType::Nil) {
        size_t idx = clx_find_first_nonnil(reinterpret_cast<const uint8_t*>(t->array_types), t->array_size, 0);
        if (idx < t->array_size) {
            return MultiValue({LValue(static_cast<int64_t>(idx + 1)), LValue(t->array[idx], t->array_types[idx])});
        }
    } else if (key.type == ValueType::Double || key.type == ValueType::Int64) {
        int64_t idx = key.type == ValueType::Int64 ? key.as_integer() : static_cast<int64_t>(key.as_number());
        if (idx >= 1 && static_cast<size_t>(idx) <= t->array_size) {
            size_t found = clx_find_first_nonnil(reinterpret_cast<const uint8_t*>(t->array_types), t->array_size, static_cast<size_t>(idx));
            if (found < t->array_size) {
                return MultiValue({LValue(static_cast<int64_t>(found + 1)), LValue(t->array[found], t->array_types[found])});
            }
        }
    }

    if (t->hash_size == 0) return MultiValue();

    bool found_key = (key.type == ValueType::Nil);
    if (t->hash_bitmap) {
        size_t bm_words = (t->hash_size + 63) / 64;
        for (size_t word = 0; word < bm_words; ++word) {
            uint64_t bits = t->hash_bitmap[word];
            while (bits) {
                size_t idx = word * 64 + clx_ctzll(bits);
                if (idx >= t->hash_size) break;
                HashEntry& e = t->entries[idx];
                if (!found_key) {
                    if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) found_key = true;
                } else if (e.vtype != ValueType::Nil) {
                    return MultiValue({LValue(e.key, e.ktype), LValue(e.val, e.vtype)});
                }
                bits &= bits - 1;
            }
        }
    } else {
        for (size_t i = 0; i < t->hash_size; ++i) {
            HashEntry& e = t->entries[i];
            if (e.ktype == ValueType::Nil) continue;
            if (!found_key) {
                if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) found_key = true;
            } else if (e.vtype != ValueType::Nil) {
                return MultiValue({LValue(e.key, e.ktype), LValue(e.val, e.vtype)});
            }
        }
    }
    return MultiValue();
}

//------------------ Raw length (no metamethods)
CLX_INLINE_HOT int64_t rawlen(const LValue& v) {
    if (v.type == ValueType::String) {
        return static_cast<int64_t>(v.string_len());
    }
    if (v.type == ValueType::Table) {
        LTable* t = static_cast<LTable*>(v.as_pointer());
        size_t n = clx_find_first_nil(reinterpret_cast<const uint8_t*>(t->array_types), t->array_size);
        if (n < t->array_size) return static_cast<int64_t>(n);
        int64_t len = static_cast<int64_t>(t->array_size);
        while (true) {
            LValue p = t->gettable(LValue(static_cast<int64_t>(len + 1)));
            if (p.type == ValueType::Nil) break;
            len++;
        }
        return len;
    }
    return 0;
}

//------------------ String concatenation wrapper
CLX_INLINE LValue concat(LState* L, const LValue& a, const LValue& b) {
    LValue args[2] = {a, b};
    return concat_multi(L, args, 2);
}

//------------------ Calls a function
CLX_INLINE MultiValue call(LState* L, const LValue& func, const LValue* args, size_t count) {
    return call_function(L, func, args, count, "C API", 0);
}

template <typename... Args>

//------------------ Calls a function (variadic)
CLX_INLINE MultiValue call(LState* L, const LValue& func, Args&&... args) {
    constexpr size_t N = sizeof...(Args);
    LValue arr[N ? N : 1] = { LValue(std::forward<Args>(args))... };
    return call_function(L, func, arr, N, "C API", 0);
}

//------------------ Protected function call
CLX_INLINE MultiValue pcall(LState* L, const LValue& func, const LValue* args, size_t count) {
    return pcall_function(L, func, args, count);
}

template <typename... Args>

//------------------ Protected function call (variadic)
CLX_INLINE MultiValue pcall(LState* L, const LValue& func, Args&&... args) {
    constexpr size_t N = sizeof...(Args);
    LValue arr[N ? N : 1] = { LValue(std::forward<Args>(args))... };
    return pcall_function(L, func, arr, N);
}

//------------------ Type assertion (throws on mismatch)
CLX_INLINE_COLD void checktype(LState* L, int argnum, const LValue& v, ValueType t) {
    if (v.type != t) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bad argument #%d (%s expected, got %s)",
            argnum, VALUE_TYPE_NAMES[static_cast<size_t>(t)],
            VALUE_TYPE_NAMES[static_cast<size_t>(v.type)]);
        throw LRuntimeException(LValue(L->intern_string(buf)));
    }
}

//------------------ No-op type check
CLX_INLINE_COLD void checkany(LState*, const LValue&) {}

//------------------ Boolean argument assertion
CLX_INLINE_COLD void argcheck(LState* L, bool cond, int argnum, const char* msg) {
    if (!cond) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bad argument #%d (%s)", argnum, msg);
        throw LRuntimeException(LValue(L->intern_string(buf)));
    }
}

//------------------ Type assertion with custom type name
CLX_INLINE_COLD void argexpected(LState* L, bool cond, int argnum, const LValue& v, const char* wanted_type, const char* extramsg = nullptr) {
    if (!cond) {
        char buf[128];
        const char* actual = VALUE_TYPE_NAMES[static_cast<size_t>(v.type)];
        int n = std::snprintf(buf, sizeof(buf), "bad argument #%d (%s expected, got %s)",
            argnum, wanted_type, actual);
        if (extramsg && extramsg[0])
            std::snprintf(buf + n, sizeof(buf) - n, " (%s)", extramsg);
        throw LRuntimeException(LValue(L->intern_string(buf)));
    }
}

//------------------ Returns type name of a value
CLX_INLINE const char* type_name(const LValue& v) {
    return VALUE_TYPE_NAMES[static_cast<size_t>(v.type)];
}

//------------------ Converts value to string LValue
CLX_INLINE LValue tolstring(LState* L, const LValue& v) {
    if (v.type == ValueType::String) return v;
    std::string s = v.to_string(L);
    return L->intern_lvalue(s);
}

//------------------ Sets a global variable (overloads)
void set_global(LState* L, const char* name, const LValue& val);
CLX_INLINE void set_global(LState* L, const char* name, double val) {
    set_global(L, name, LValue(val));
}
CLX_INLINE void set_global(LState* L, const char* name, int64_t val) {
    set_global(L, name, LValue(val));
}
CLX_INLINE void set_global(LState* L, const char* name, const char* val) {
    set_global(L, name, LValue(L->intern_string(val)));
}

//------------------ Reads and validates an integer field
CLX_INLINE_COLD int64_t check_field_integer(LState* L, const LValue& v, const char* field) {
    if (v.type == ValueType::Int64) return v.as_integer();
    if (v.type == ValueType::Double)  return static_cast<int64_t>(v.as_number());
    char buf[192];
    std::snprintf(buf, sizeof(buf), "field '%s' (integer expected, got %s)",
        field, VALUE_TYPE_NAMES[static_cast<size_t>(v.type)]);
    throw LRuntimeException(LValue(L->intern_string(buf)));
}

//------------------ Reads and validates a number field
CLX_INLINE_COLD double check_field_number(LState* L, const LValue& v, const char* field) {
    if (v.type == ValueType::Double)  return v.as_number();
    if (v.type == ValueType::Int64) return static_cast<double>(v.as_integer());
    char buf[192];
    std::snprintf(buf, sizeof(buf), "field '%s' (number expected, got %s)",
        field, VALUE_TYPE_NAMES[static_cast<size_t>(v.type)]);
    throw LRuntimeException(LValue(L->intern_string(buf)));
}

//------------------ Reads and validates a string field
CLX_INLINE_COLD const char* check_field_string(LState* L, const LValue& v, const char* field) {
    if (v.type == ValueType::String) return v.as_string();
    char buf[192];
    std::snprintf(buf, sizeof(buf), "field '%s' (string expected, got %s)",
        field, VALUE_TYPE_NAMES[static_cast<size_t>(v.type)]);
    throw LRuntimeException(LValue(L->intern_string(buf)));
}

//------------------ Gets table value bypassing metamethods
CLX_INLINE LValue raw_get(LState* L, const LValue& table, const char* key) {
    return raw_get(L, table, LValue(L->intern_string(key)));
}

//------------------ Gets table value bypassing metamethods
CLX_INLINE LValue raw_get(LState* L, const LValue& table, double key) {
    return raw_get(L, table, LValue(key));
}

//------------------ Gets table value bypassing metamethods
CLX_INLINE LValue raw_get(LState* L, const LValue& table, int64_t key) {
    return raw_get(L, table, LValue(key));
}

//------------------ Sets table value bypassing metamethods
CLX_INLINE void raw_set(LState* L, const LValue& table, const char* key, const LValue& val) {
    raw_set(L, table, LValue(L->intern_string(key)), val);
}

//------------------ Sets table value bypassing metamethods
CLX_INLINE void raw_set(LState* L, const LValue& table, double key, const LValue& val) {
    raw_set(L, table, LValue(key), val);
}

//------------------ Sets table value bypassing metamethods
CLX_INLINE void raw_set(LState* L, const LValue& table, int64_t key, const LValue& val) {
    raw_set(L, table, LValue(key), val);
}

//------------------ C++ iterator over table entries
class table_iterator {
public:
    struct value_type {
        LValue key;
        LValue value;
    };

    table_iterator(LState* L, const LValue& table)
        : L_(L), table_(table), key_(LValue()), has_more_(advance()) {}

    value_type& operator*()  { return current_; }
    value_type* operator->() { return &current_; }

    table_iterator& operator++() {
        if (has_more_) key_ = current_.key;
        has_more_ = advance();
        return *this;
    }

    explicit operator bool() const { return has_more_; }

private:
    bool advance() {
        MultiValue mv = next(L_, table_, key_);
        if (mv.count == 0) { has_more_ = false; return false; }
        current_.key   = mv[0];
        current_.value = mv[1];
        return true;
    }

    LState* L_;
    LValue table_;
    LValue key_;
    value_type current_;
    bool has_more_ = false;
};

//------------------ Creates a table_iterator
CLX_INLINE table_iterator iterate(LState* L, const LValue& table) {
    return table_iterator(L, table);
}

//------------------ Throws a Lua-style error
[[noreturn]] CLX_INLINE_COLD void error(LState* L, const char* msg) {
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Throws a bad-argument error
[[noreturn]] CLX_INLINE_COLD void arg_error(LState* L, int argnum, const char* expected) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "bad argument #%d (%s expected)", argnum, expected);
    throw LRuntimeException(LValue(L->intern_string(buf)));
}

//------------------ Throws a type error
[[noreturn]] CLX_INLINE_COLD void type_error(LState* L, int argnum, const char* expected, const LValue* args, size_t count) {
    char buf[128];
    const char* got = (argnum >= 1 && static_cast<size_t>(argnum - 1) < count)
        ? VALUE_TYPE_NAMES[static_cast<size_t>(args[argnum - 1].type)]
        : "no value";
    std::snprintf(buf, sizeof(buf), "bad argument #%d (%s expected, got %s)",
        argnum, expected, got);
    throw LRuntimeException(LValue(L->intern_string(buf)));
}

}

#endif
