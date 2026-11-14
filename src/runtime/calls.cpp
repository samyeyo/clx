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

//------------------ kMaxCallChain: stock resolves a '__call' chain iteratively and refuses more than 15
//------------------ metamethod levels, which clx mirrors because its chain is a native recursion
static constexpr int kMaxCallChain = 15;

//------------------ CallChainScope: one '__call' dispatch, popped again when the nested call returns
struct CallChainScope {
    int &depth;

    explicit CallChainScope(int &d)
        : depth(d) {
        ++depth;
    }

    ~CallChainScope() { --depth; }
};

//------------------ ChainReset: a real function ends the chain, so its body starts dispatching at zero
//------------------ (stock resolves the whole chain before the function runs)
struct ChainReset {
    int &depth;
    int saved;

    explicit ChainReset(int &d)
        : depth(d)
        , saved(d) {
        depth = 0;
    }

    ~ChainReset() { depth = saved; }
};

//------------------ call_function — call a value as function
MultiValue call_function(LState *L, const LValue &func, const LValue *args, size_t count, const CallSite *cs) {
    stack_check(L);
    L->current_site = cs;

    LValue *func_ind[1] = { const_cast<LValue *>(&func) };

    RootRange rr(L, const_cast<LValue *>(args), args ? count : 0, func_ind, 1);

    if (func.type == Function) {
        ChainReset chain_reset(L->call_chain_depth);
        LCFunction *f = static_cast<LCFunction *>(func.as_pointer());
        LCFunction *saved_func = L->current_func;
        L->current_func = f;
        if (f->direct) {
            MultiValue ret = f->direct(L, args, count);
            L->current_func = saved_func;
            return ret;
        }
        MultiValue ret = f->func(L, args, count);
        L->current_func = saved_func;
        return ret;
    }

    if (func.type == Table) {
        LTable *mt = tbl_metatable(static_cast<LTable *>(func.as_pointer()));
        if (mt) {
            LValue m = mt->gettable(L->str_call);
            if (m.type != Nil) {
                if (L->call_chain_depth >= kMaxCallChain)
                    throw LRuntimeException(LValue(L->intern_string("'__call' chain too long")));
                CallChainScope chain_scope(L->call_chain_depth);
                size_t nargs = count + 1;
                LValue *new_args;
                alignas(LValue) unsigned char stack_buf[16 * sizeof(LValue)];
                bool heap = nargs > 16;
                if (heap)
                    new_args = new LValue[nargs];
                else
                    new_args = reinterpret_cast<LValue *>(stack_buf);
                //------------------ Placement-new only the slots used (was: 16 default ctors + assigns).
                //------------------ Heap path keeps default-new (cold; delete[] needs live objects).
                if (heap) {
                    new_args[0] = func;
                    for (size_t i = 0; i < count; ++i)
                        new_args[i + 1] = args[i];
                } else {
                    new (new_args) LValue(func);
                    for (size_t i = 0; i < count; ++i)
                        new (new_args + i + 1) LValue(args[i]);
                }

                MultiValue ret = call_function_rooted(L, m, new_args, nargs, cs);
                if (heap)
                    delete[] new_args;
                return ret;
            }
        }
    }

    std::string prefix = "";
    if (cs && cs->file && cs->file[0] != '\0') {
        prefix = std::string(cs->file) + ":" + std::to_string(cs->line) + ": ";
    }

    std::string err_msg = prefix + "attempt to call a " + VALUE_TYPE_NAMES[static_cast<size_t>(func.type)] + " value";
    throw LRuntimeException(LValue(L->intern_string(err_msg)));
}

//------------------ pcall_function — protected call
MultiValue pcall_function(LState *L, const LValue &func, const LValue *args, size_t count) {
    try {
        //------------------ stock luaL_where sees the C pcall frame for its callers: no line, so errors
        //------------------ raised directly under pcall carry no file:line prefix (keep the source name).
        //------------------ The callee is passed as a value (not a field/method access at this call site), so
        //------------------ clear the name/method flags: a forwarded call falls back to the full function name.
        L->pcall_site.file = L->current_site ? L->current_site->file : "";
        L->pcall_site.line = 0;
        L->pcall_site.is_method = false;
        L->pcall_site.name_short = false;
        MultiValue ret = call_function(L, func, args, count, &L->pcall_site);
        if (ret.count == 0)
            return MultiValue(true);
        if (ret.count == 1)
            return MultiValue(true, ret[0]);
        //------------------ one allocation for the whole result: push_back grows the shared overflow heap
        //------------------ quadratically (each step copies every earlier slot), which is unusable when a
        //------------------ protected call returns hundreds of thousands of values
        std::vector<LValue> vals;
        vals.reserve(ret.count + 1);
        vals.push_back(LValue(true));
        for (size_t i = 0; i < ret.count; ++i)
            vals.push_back(ret[i]);
        return MultiValue(vals, L);
    } catch (const LRuntimeException &e) {
        //------------------ a __close that raised while this error unwound replaced it (stock rethrows the
        //------------------ last close error), so report that one and drop the pending slot
        LValue err_obj = e.error_obj;
        if (L->has_close_err) {
            err_obj = L->close_err;
            L->has_close_err = false;
        }
        //------------------ stock's pcall returns whatever error object was raised (a table, a number, ...),
        //------------------ not a rendered message; a nil object becomes the 'no error object' placeholder
        if (err_obj.type == ValueType::Nil)
            return MultiValue(false, LValue(L->intern_string("<no error object>")));
        return MultiValue(false, err_obj);
    } catch (const std::exception &e) {
        return MultiValue(false, LValue(L->intern_string(e.what())));
    }
}

//------------------ call_direct — fast path for LCFunction direct calls
MultiValue call_direct(LState *L, const LValue &func, const LValue *args, size_t count, const CallSite *cs) {
    stack_check(L);
    if (func.type == ValueType::Function) {
        LCFunction *f = static_cast<LCFunction *>(func.as_pointer());
        if (f->direct) {
            LValue *func_ind[1] = { const_cast<LValue *>(&func) };
            RootRange rr(L, const_cast<LValue *>(args), args ? count : 0, func_ind, 1);
            LCFunction *saved = L->current_func;
            L->current_func = f;
            if (cs && cs->line > 0)
                L->current_site = cs;
            MultiValue ret = f->direct(L, args, count);
            L->current_func = saved;
            return ret;
        }
    }
    return call_function(L, func, args, count, cs);
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
