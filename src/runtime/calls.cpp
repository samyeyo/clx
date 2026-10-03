// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  calls.cpp · Function call dispatch         │
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

//------------------ call_function — call a value as function
MultiValue call_function(LState *L, const LValue &func, const LValue *args, size_t count, const char *file, int line) {
    L->current_file = file;
    L->current_line = line;

    size_t prev_shadow = L->shadow_top;
    L->shadow_stack[L->shadow_top++] = TypedSlot(const_cast<TValue *>(&func.val), const_cast<ValueType *>(&func.type));
    for (size_t ai = 0; ai < count; ++ai)
        L->shadow_stack[L->shadow_top++]
            = TypedSlot(const_cast<TValue *>(&args[ai].val), const_cast<ValueType *>(&args[ai].type));

    if (func.type == Function) {
        LCFunction *f = static_cast<LCFunction *>(func.as_pointer());
        LCFunction *saved_func = L->current_func;
        L->current_func = f;
        if (f->direct) {
            MultiValue ret = f->direct(L, args, count);
            L->current_func = saved_func;
            L->shadow_top = prev_shadow;
            return ret;
        }
        MultiValue ret = f->func(L, args, count);
        L->current_func = saved_func;
        L->shadow_top = prev_shadow;
        return ret;
    }

    if (func.type == Table) {
        LTable *mt = tbl_metatable(static_cast<LTable *>(func.as_pointer()));
        if (mt) {
            LValue m = mt->gettable(L->str_call);
            if (m.type != Nil) {
                size_t nargs = count + 1;
                LValue *new_args;
                LValue stack_buf[16];
                bool heap = nargs > 16;
                if (heap)
                    new_args = new LValue[nargs];
                else
                    new_args = stack_buf;
                new_args[0] = func;
                for (size_t i = 0; i < count; ++i)
                    new_args[i + 1] = args[i];

                MultiValue ret = call_function_rooted(L, m, new_args, nargs, file, line);
                if (heap)
                    delete[] new_args;
                L->shadow_top = prev_shadow;
                return ret;
            }
        }
    }

    L->shadow_top = prev_shadow;

    std::string prefix = "";
    if (file && file[0] != '\0') {
        prefix = std::string(file) + ":" + std::to_string(line) + ": ";
    }

    std::string err_msg = prefix + "attempt to call a " + VALUE_TYPE_NAMES[static_cast<size_t>(func.type)] + " value";
    throw LRuntimeException(LValue(L->intern_string(err_msg)));
}

//------------------ pcall_function — protected call
MultiValue pcall_function(LState *L, const LValue &func, const LValue *args, size_t count) {
    size_t shadow_base = L->shadow_top;
    try {
        MultiValue ret = call_function(L, func, args, count, L->current_file, L->current_line);
        if (ret.count == 0)
            return MultiValue({ LValue(true) });
        if (ret.count == 1)
            return MultiValue({ LValue(true), ret[0] });
        std::vector<LValue> results;
        results.reserve(ret.count + 1);
        results.push_back(LValue(true));
        for (size_t i = 0; i < ret.count; ++i)
            results.push_back(ret[i]);
        return MultiValue(results, L);
    } catch (const LRuntimeException &e) {
        L->shadow_top = shadow_base;

        LValue err_val = e.error_obj;
        if (err_val.type != String) {
            err_val = LValue(L->intern_string(e.what()));
        }
        return MultiValue({ LValue(false), err_val });
    } catch (const std::exception &e) {
        L->shadow_top = shadow_base;
        return MultiValue({ LValue(false), LValue(L->intern_string(e.what())) });
    }
}

//------------------ call_direct — fast path for LCFunction direct calls
MultiValue call_direct(LState *L, const LValue &func, const LValue *args, size_t count, const char *file, int line) {
    if (func.type == ValueType::Function) {
        LCFunction *f = static_cast<LCFunction *>(func.as_pointer());
        if (f->direct) {
            size_t prev_shadow = L->shadow_top;
            L->shadow_stack[L->shadow_top++]
                = TypedSlot(const_cast<TValue *>(&func.val), const_cast<ValueType *>(&func.type));
            for (size_t ai = 0; ai < count; ++ai)
                L->shadow_stack[L->shadow_top++]
                    = TypedSlot(const_cast<TValue *>(&args[ai].val), const_cast<ValueType *>(&args[ai].type));
            LCFunction *saved = L->current_func;
            L->current_func = f;
            MultiValue ret = f->direct(L, args, count);
            L->current_func = saved;
            L->shadow_top = prev_shadow;
            return ret;
        }
    }
    return call_function(L, func, args, count, file, line);
}

//------------------ callmeta — call metamethod
static MultiValue lazy_funcs_index(LState *L, const LValue *args, size_t n) {
    if (n < 2 || args[1].type != String)
        return MultiValue();

    const char *name = args[1].as_string();
    LTable *t = static_cast<LTable *>(args[0].as_pointer());
    LTable *mt = tbl_metatable(t);
    if (!mt)
        return MultiValue();

    LValue regs_key(L->intern_string("__lazy_regs"));
    LValue count_key(L->intern_string("__lazy_count"));

    LValue regs_val = mt->gettable(regs_key);
    LValue count_val = mt->gettable(count_key);
    if (regs_val.type != UserData || regs_val.type == Nil || count_val.type == Nil || count_val.type != Int64)
        return MultiValue();

    const LazyReg *regs = reinterpret_cast<const LazyReg *>(regs_val.as_pointer());
    int64_t count = count_val.as_integer();

    for (int64_t i = 0; i < count; i++) {
        if (std::strcmp(regs[i].name, name) == 0) {
            LValue func = L->create_closure(CFunctionType(regs[i].func));
            t->settable(args[1], func);
            if (t->age == AGE_OLD && func.is_gc_obj())
                gc_barrier_header(L, t, func);
            return MultiValue(func);
        }
    }

    return MultiValue();
}

//------------------ set_lazy_funcs — set lazy function registrations
void set_lazy_funcs(LState *L, const LValue &table, const LazyReg *regs, size_t count) {
    if (table.type != Table)
        return;
    LTable *t = static_cast<LTable *>(table.as_pointer());

    LTable *mt = tbl_metatable(t);
    if (!mt) {
        mt = static_cast<LTable *>(L->create_table().as_pointer());
        tbl_set_metatable(t, mt);
        if (t->age == AGE_OLD)
            gc_barrier_header(L, t, LValue(Table, mt));
    }
    meta_list_add(L, t);
    mt->settable(LValue(L->intern_string("__lazy_regs")),
        LValue(UserData, reinterpret_cast<LHeader *>(const_cast<LazyReg *>(regs))));
    mt->settable(LValue(L->intern_string("__lazy_count")), LValue(static_cast<int64_t>(count)));

    LValue existing = mt->gettable(L->str_index);
    if (existing.type == Nil) {
        LValue handler = L->create_closure(CFunctionType(lazy_funcs_index));
        mt->settable(L->str_index, handler);
    }
}

}
