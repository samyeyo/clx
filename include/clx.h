// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  clx.h · Public C++ API Header              │
// └─────────────────────────────────────────────┘

#ifndef CLX_H
#define CLX_H

#include <clx_runtime.h>

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
    if (len <= 5) return LValue::istr(s, len);
    return LValue(L->intern_string(s, len));
}

//------------------ Creates a string LValue
CLX_INLINE LValue string(LState* L, const char* s) {
    size_t len = __builtin_strlen(s);
    if (len <= 5) return LValue::istr(s, len);
    return LValue(L->intern_string(s, len));
}

//------------------ Creates a table LValue
CLX_INLINE LValue table(LState* L, size_t as = 0, size_t hs = 0) {
    return L->create_table(as, hs);
}

//------------------ Creates a C function LValue
CLX_INLINE LValue cfunction(LState* L, CFunctionType func) {
    return L->create_closure(std::move(func));
}

//------------------ Creates a thread LValue
CLX_INLINE_HOT LValue thread(LThread* t)              { return LValue(LType::Thread, t); }
//------------------ Creates a light userdata LValue
CLX_INLINE_HOT LValue lightuserdata(void* p)          { return LValue(LType::Userdata, reinterpret_cast<LHeader*>(p)); }

//------------------ Returns type enum of a value
CLX_INLINE_HOT LType  type_of(const LValue& v)        { return v.type(); }
//------------------ Returns type name string from enum
CLX_INLINE_HOT const char* type_name(LType t)          { return TYPE_NAMES[static_cast<size_t>(t)]; }
//------------------ Checks if value is nil
CLX_INLINE_HOT bool   is_nil(const LValue& v)          { return v.type() == LType::Nil; }
//------------------ Checks if value is boolean
CLX_INLINE_HOT bool   is_bool(const LValue& v)         { return v.type() == LType::Bool; }
//------------------ Checks if value is number or integer
CLX_INLINE_HOT bool   is_number(const LValue& v)       { return v.type() == LType::Number || v.type() == LType::Integer; }
//------------------ Checks if value is integer
CLX_INLINE_HOT bool   is_integer(const LValue& v)      { return v.type() == LType::Integer; }
//------------------ Checks if value is string
CLX_INLINE_HOT bool   is_string(const LValue& v)       { return v.type() == LType::String; }
//------------------ Checks if value is table
CLX_INLINE_HOT bool   is_table(const LValue& v)        { return v.type() == LType::Table; }
//------------------ Checks if value is function
CLX_INLINE_HOT bool   is_function(const LValue& v)     { return v.type() == LType::Function; }
//------------------ Checks if value is thread
CLX_INLINE_HOT bool   is_thread(const LValue& v)       { return v.type() == LType::Thread; }
//------------------ Checks if value is userdata
CLX_INLINE_HOT bool   is_userdata(const LValue& v)     { return v.type() == LType::Userdata; }
//------------------ Always false (API compat)
CLX_INLINE_HOT bool   is_none(const LValue& v)         { return false; }  
//------------------ Checks if value is nil
CLX_INLINE_HOT bool   is_noneornil(const LValue& v)    { return v.type() == LType::Nil; }

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

//------------------ Converts value to int64_t (with default)
CLX_INLINE_HOT int64_t to_integer(const LValue& v, int64_t def = 0) {
    int64_t out;
    return clx_to_integer(v, out) ? out : def;
}

//------------------ Converts to int64_t, reports success
CLX_INLINE int64_t to_integerx(const LValue& v, bool* isnum = nullptr) {
    int64_t out;
    bool ok = clx_to_integer(v, out);
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
    if (v.type() == LType::String) return v.as_string();
    if (v.type() == LType::Number || v.type() == LType::Integer)
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
    if (v.type() == LType::Userdata) return static_cast<LUserdata*>(v.as_pointer());
    std::string msg = std::string("userdata expected, got ") + TYPE_NAMES[static_cast<size_t>(v.type())];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Checks and returns double (throws)
CLX_INLINE_COLD double check_number(LState* L, const LValue& v) {
    double out;
    if (v.to_number(out)) return out;
    std::string msg = std::string("number expected, got ") + TYPE_NAMES[static_cast<size_t>(v.type())];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Checks and returns int64_t (throws)
CLX_INLINE_COLD int64_t check_integer(LState* L, const LValue& v) {
    int64_t out;
    if (clx_to_integer(v, out)) return out;
    std::string msg = std::string("integer expected, got ") + TYPE_NAMES[static_cast<size_t>(v.type())];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Checks and returns string (throws)
CLX_INLINE_COLD const char* check_string(LState* L, const LValue& v) {
    if (v.type() == LType::String) return v.as_string();
    std::string msg = std::string("string expected, got ") + TYPE_NAMES[static_cast<size_t>(v.type())];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Checks and returns LTable* (throws)
CLX_INLINE_COLD LTable* check_table(LState* L, const LValue& v) {
    if (v.type() == LType::Table) return static_cast<LTable*>(v.as_pointer());
    std::string msg = std::string("table expected, got ") + TYPE_NAMES[static_cast<size_t>(v.type())];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Checks and returns LCFunction* (throws)
CLX_INLINE_COLD LCFunction* check_function(LState* L, const LValue& v) {
    if (v.type() == LType::Function) return static_cast<LCFunction*>(v.as_pointer());
    std::string msg = std::string("function expected, got ") + TYPE_NAMES[static_cast<size_t>(v.type())];
    throw LRuntimeException(LValue(L->intern_string(msg)));
}

//------------------ Optional number with default
CLX_INLINE_COLD double opt_number(LState* L, const LValue& v, double def) {
    if (v.type() == LType::Nil) return def;
    return check_number(L, v);
}

//------------------ Optional integer with default
CLX_INLINE_COLD int64_t opt_integer(LState* L, const LValue& v, int64_t def) {
    if (v.type() == LType::Nil) return def;
    return check_integer(L, v);
}

//------------------ Optional string with default
CLX_INLINE_COLD const char* opt_string(LState* L, const LValue& v, const char* def) {
    if (v.type() == LType::Nil) return def;
    return check_string(L, v);
}

//------------------ Reads a string-keyed field from a table
CLX_INLINE LValue get_field(LState* L, const LValue& table, const char* key) {
    return clx_table_get(L, table, LValue(L->intern_string(key)));
}

//------------------ Writes a string-keyed field to a table
CLX_INLINE void set_field(LState* L, const LValue& table, const char* key, const LValue& val) {
    clx_table_set(L, table, LValue(L->intern_string(key)), val);
}

//------------------ Gets table value bypassing metamethods
CLX_INLINE LValue raw_get(LState* L, const LValue& table, const LValue& key) {
    LTable* t = check_table(L, table);
    LValue* p = t->gettable(key);
    return p ? *p : LValue();
}

//------------------ Sets table value bypassing metamethods
CLX_INLINE void raw_set(LState* L, const LValue& table, const LValue& key, const LValue& val) {
    LTable* t = check_table(L, table);
    t->settable(key, val);
}

//------------------ Gets integer-keyed table value (fast)
CLX_INLINE LValue raw_get_i(LState* L, const LValue& table, int64_t idx) {
    return clx_table_get_int(L, table, static_cast<size_t>(idx));
}

//------------------ Sets integer-keyed table value (fast)
CLX_INLINE void raw_set_i(LState* L, const LValue& table, int64_t idx, const LValue& val) {
    LTable* t = check_table(L, table);
    if (static_cast<uint64_t>(idx - 1) < t->array_cap) {
        t->array[idx - 1] = val;
        if (static_cast<size_t>(idx) > t->array_size) t->array_size = static_cast<size_t>(idx);
    } else {
        t->settable(LValue(idx), val);
    }
}

//------------------ Binds a C function to a table
CLX_INLINE void set_function(LState* L, const LValue& table, const char* name, CFunctionType func) {
    if (table.type() != LType::Table) return;
    static_cast<LTable*>(table.as_pointer())->bind(L, name, std::move(func));
}

//------------------ Sets a named value on a table
CLX_INLINE void set_value(LState* L, const LValue& table, const char* name, const LValue& val) {
    if (table.type() != LType::Table) return;
    static_cast<LTable*>(table.as_pointer())->settable(LValue(L->intern_string(name)), val);
}

//------------------ Binds multiple C functions (via LReg*)
CLX_INLINE void set_functions(LState* L, const LValue& table, std::initializer_list<LReg> funcs) {
    if (table.type() != LType::Table) return;
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
    if (obj.type() == LType::Table) {
        mt = static_cast<LTable*>(obj.as_pointer())->metatable;
    } else if (obj.type() == LType::Userdata) {
        mt = static_cast<LUserdata*>(obj.as_pointer())->metatable;
    }
    if (mt) {
        LValue* protected_mt = mt->gettable(LValue(L->intern_string("__metatable")));
        if (protected_mt && protected_mt->type() != LType::Nil) {
            return *protected_mt;
        }
        return LValue(LType::Table, mt);
    }
    return LValue();
}

//------------------ Sets metatable of table/userdata
CLX_INLINE void setmetatable(LState* L, const LValue& obj, const LValue& mt) {
    if (obj.type() != LType::Table && obj.type() != LType::Userdata) {
        throw_runtime_error("bad argument to 'setmetatable' (table or userdata expected)");
    }
    if (mt.type() != LType::Table && mt.type() != LType::Nil) {
        throw_runtime_error("bad argument #2 to 'setmetatable' (nil or table expected)");
    }
    LTable* new_mt = (mt.type() == LType::Table) ? static_cast<LTable*>(mt.as_pointer()) : nullptr;
    if (obj.type() == LType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        if (t->metatable) {
            LValue* protected_mt = t->metatable->gettable(LValue(L->intern_string("__metatable")));
            if (protected_mt && protected_mt->type() != LType::Nil) {
                throw_runtime_error("cannot change a protected metatable");
            }
        }
        t->metatable = new_mt;
        t->shape_version++;
    } else if (obj.type() == LType::Userdata) {
        static_cast<LUserdata*>(obj.as_pointer())->metatable = new_mt;
    }
}

//------------------ Raw equality without metamethods
CLX_INLINE_HOT bool rawequal(const LValue& a, const LValue& b) {
    if (a.type() == LType::Integer && b.type() == LType::Integer)
        return a.as_integer() == b.as_integer();
    if (a.type() == LType::Number && b.type() == LType::Number)
        return a.as_number() == b.as_number();
    if (a.type() == LType::Integer && b.type() == LType::Number)
        return static_cast<double>(a.as_integer()) == b.as_number();
    if (a.type() == LType::Number && b.type() == LType::Integer)
        return a.as_number() == static_cast<double>(b.as_integer());
    if (a.type() != b.type()) return false;
    if (a.type() == LType::String)  return a.val == b.val;
    if (a.type() == LType::Bool)    return a.as_bool() == b.as_bool();
    if (a.type() == LType::Nil)     return true;
    return a.as_pointer() == b.as_pointer();
}

//------------------ Table iteration primitive
CLX_INLINE MultiValue next(LState* L, const LValue& table, const LValue& key) {
    if (table.type() != LType::Table) {
        throw_runtime_error("bad argument #1 to 'next' (table expected)");
    }
    LTable* t = static_cast<LTable*>(table.as_pointer());

    if (key.type() == LType::Nil) {
        for (size_t i = 0; i < t->array_size; ++i) {
            if (t->array[i].type() != LType::Nil) {
                return MultiValue({LValue(static_cast<double>(i + 1)), t->array[i]});
            }
        }
    } else if (key.type() == LType::Number || key.type() == LType::Integer) {
        int64_t idx = key.type() == LType::Integer ? key.as_integer() : static_cast<int64_t>(key.as_number());
        if (idx >= 1 && static_cast<size_t>(idx) <= t->array_size) {
            for (size_t i = idx; i < t->array_size; ++i) {
                if (t->array[i].type() != LType::Nil) {
                    return MultiValue({LValue(static_cast<double>(i + 1)), t->array[i]});
                }
            }
        }
    }

    bool found_key = (key.type() == LType::Nil);
    for (size_t i = 0; i < t->hash_size; ++i) {
        LValue k; k.val = t->keys[i];
        if (!found_key) {
            if (k.val == key.val) found_key = true;
        } else if (k.type() != LType::Nil && t->vals[i].type() != LType::Nil) {
            return MultiValue({k, t->vals[i]});
        }
    }

    return MultiValue();
}

//------------------ Length operator wrapper
CLX_INLINE int64_t len(LState* L, const LValue& v) {
    return clx_len(L, v).as_integer();
}

//------------------ Raw length (no metamethods)
CLX_INLINE_HOT int64_t rawlen(const LValue& v) {
    if (v.type() == LType::String) {
        return static_cast<int64_t>(v.string_len());
    }
    if (v.type() == LType::Table) {
        LTable* t = static_cast<LTable*>(v.as_pointer());
        size_t n = 0;
        while (n < t->array_size && t->array[n].type() != LType::Nil) n++;
        if (n < t->array_size) return static_cast<int64_t>(n);
        int64_t len = static_cast<int64_t>(t->array_size);
        while (true) {
            LValue* p = t->gettable(LValue(static_cast<double>(len + 1)));
            if (!p || p->type() == LType::Nil) break;
            len++;
        }
        return len;
    }
    return 0;
}

//------------------ String concatenation wrapper
CLX_INLINE LValue concat(LState* L, const LValue& a, const LValue& b) {
    LValue args[2] = {a, b};
    return clx_concat_multi(L, args, 2);
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
CLX_INLINE_COLD void checktype(LState* L, int argnum, const LValue& v, LType t) {
    if (v.type() != t) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bad argument #%d (%s expected, got %s)",
            argnum, TYPE_NAMES[static_cast<size_t>(t)],
            TYPE_NAMES[static_cast<size_t>(v.type())]);
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
        const char* actual = TYPE_NAMES[static_cast<size_t>(v.type())];
        int n = std::snprintf(buf, sizeof(buf), "bad argument #%d (%s expected, got %s)",
            argnum, wanted_type, actual);
        if (extramsg && extramsg[0])
            std::snprintf(buf + n, sizeof(buf) - n, " (%s)", extramsg);
        throw LRuntimeException(LValue(L->intern_string(buf)));
    }
}

//------------------ Returns type name of a value
CLX_INLINE const char* type_name(const LValue& v) {
    return TYPE_NAMES[static_cast<size_t>(v.type())];
}

//------------------ Converts value to string LValue
CLX_INLINE LValue tolstring(LState* L, const LValue& v) {
    if (v.type() == LType::String) return v;
    std::string s = v.to_string(L);
    return LValue(L->intern_string(s.data(), s.size()));
}

//------------------ Sets a global variable (overloads)
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
    if (v.type() == LType::Integer) return v.as_integer();
    if (v.type() == LType::Number)  return static_cast<int64_t>(v.as_number());
    char buf[192];
    std::snprintf(buf, sizeof(buf), "field '%s' (integer expected, got %s)",
        field, TYPE_NAMES[static_cast<size_t>(v.type())]);
    throw LRuntimeException(LValue(L->intern_string(buf)));
}

//------------------ Reads and validates a number field
CLX_INLINE_COLD double check_field_number(LState* L, const LValue& v, const char* field) {
    if (v.type() == LType::Number)  return v.as_number();
    if (v.type() == LType::Integer) return static_cast<double>(v.as_integer());
    char buf[192];
    std::snprintf(buf, sizeof(buf), "field '%s' (number expected, got %s)",
        field, TYPE_NAMES[static_cast<size_t>(v.type())]);
    throw LRuntimeException(LValue(L->intern_string(buf)));
}

//------------------ Reads and validates a string field
CLX_INLINE_COLD const char* check_field_string(LState* L, const LValue& v, const char* field) {
    if (v.type() == LType::String) return v.as_string();
    char buf[192];
    std::snprintf(buf, sizeof(buf), "field '%s' (string expected, got %s)",
        field, TYPE_NAMES[static_cast<size_t>(v.type())]);
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
[[noreturn]] CLX_INLINE_COLD void type_error(LState* L, int argnum, const char* expected) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "bad argument #%d (%s expected, got %s)",
        argnum, expected, TYPE_NAMES[static_cast<size_t>(LType::Nil)]);
    throw LRuntimeException(LValue(L->intern_string(buf)));
}

}

#endif
