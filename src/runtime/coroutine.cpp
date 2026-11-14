// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  coroutine.cpp · Coroutine Library          │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include <exception>

namespace clx {

//------------------ coroutine_create: creates a new coroutine (thread)
clx::MultiValue coroutine_create(clx::LState *L, const clx::LValue *args, size_t count) {
    clx::LValue func = count > 0 ? args[0] : clx::LValue();
    double stack_size = count > 1 ? clx::to_number(args[1], 262144.0) : 262144.0;
    return clx::MultiValue(clx::create_thread(L, func, stack_size));
}

//------------------ coroutine_resume: resumes a suspended coroutine
clx::MultiValue coroutine_resume(clx::LState *L, const clx::LValue *args, size_t count) {
    //------------------ stock raises on a non-thread argument (luaL_argexpected), it does not return a failure
    if (count == 0 || !clx::is_thread(args[0]))
        clx::error(L, bad_arg_msg(L, 1, "coroutine.resume", "thread expected").c_str());
    LThread *t = static_cast<LThread *>(args[0].as_pointer());
    if (t->status == THREAD_DEAD) {
        clx::LValue r[2] = { clx::boolean(false), clx::string(L, "cannot resume dead coroutine") };
        return clx::MultiValue(r, 2, L);
    }
    if (t == L->running_thread || t->status == THREAD_RUNNING || t->status == THREAD_NORMAL) {
        clx::LValue r[2] = { clx::boolean(false), clx::string(L, "cannot resume non-suspended coroutine") };
        return clx::MultiValue(r, 2, L);
    }

    clx::LValue thread_val = args[0];
    size_t arg_count = count > 1 ? count - 1 : 0;
    return clx::resume(L, thread_val, count > 1 ? args + 1 : nullptr, arg_count);
}

//------------------ coroutine_yield: yields execution from a coroutine
clx::MultiValue coroutine_yield(clx::LState *L, const clx::LValue *args, size_t count) {
    return clx::yield(L, args, count);
}

//------------------ coroutine_status: returns the status of a coroutine
clx::MultiValue coroutine_status(clx::LState *L, const clx::LValue *args, size_t count) {
    if (count == 0 || !clx::is_thread(args[0]))
        clx::error(L, bad_arg_msg(L, 1, "coroutine.status", "thread expected").c_str());
    LThread *t = static_cast<LThread *>(args[0].as_pointer());
    if (t->status == THREAD_SUSPENDED)
        return clx::MultiValue(clx::string(L, "suspended"));
    if (t->status == THREAD_RUNNING)
        return clx::MultiValue(clx::string(L, "running"));
    if (t->status == THREAD_DEAD)
        return clx::MultiValue(clx::string(L, "dead"));
    return clx::MultiValue(clx::string(L, "normal"));
}

static clx::MultiValue coroutine_isyieldable(LState *L, const LValue *args, size_t count) {
    if (count >= 1 && is_thread(args[0])) {
        LThread *t = static_cast<LThread *>(args[0].as_pointer());
        return MultiValue(boolean(!t->is_main));
    }
    //------------------ stock's isyieldable is false across a C call with no continuation (a sort
    //------------------ comparator, a gsub replacement), not just outside a coroutine
    return MultiValue(boolean(!L->running_thread->is_main && L->c_yield_blocked == 0));
}

//------------------ coroutine_wrap: wraps a coroutine in a callable table
static clx::MultiValue coroutine_wrap(clx::LState *L, const clx::LValue *args, size_t count) {
    if (count == 0 || !clx::is_function(args[0])) {
        clx::error(L, "bad argument #1 to 'wrap' (function expected)");
    }
    clx::LValue thread = clx::create_thread(L, args[0]);
    // A C++ lambda capture is invisible to the GC, so the thread also lives in a traced gc_cells entry
    // (as generated closures do). Root it meanwhile: create_closure() allocates.
    clx::RootRange thread_root(L, &thread, 1);
    auto cell = std::make_shared<clx::LValue>(thread);
    return clx::MultiValue(
        L->create_closure([cell](LState *L2, const LValue *call_args, size_t call_arg_count) -> clx::MultiValue {
            const clx::LValue thread = *cell;
            LThread *t = static_cast<LThread *>(thread.as_pointer());
            if (t->status == THREAD_DEAD)
                clx::error(L2, "cannot resume dead coroutine");
            //------------------ stock refuses to resume a coroutine that is running (possibly itself) or that
            //------------------ resumed another one: the caller must not switch onto a live stack
            if (t == L2->running_thread || t->status == THREAD_RUNNING || t->status == THREAD_NORMAL)
                clx::error(L2, "cannot resume non-suspended coroutine");
            clx::MultiValue result = clx::resume(L2, thread, call_args, call_arg_count);
            if (result.count == 0)
                clx::error(L2, "cannot resume dead coroutine");
            if (!result[0].as_bool()) {
                throw LRuntimeException(result.count > 1 ? result[1] : LValue());
            }
            if (result.count <= 1)
                return MultiValue();
            //------------------ single allocation: push_back grows the shared overflow heap quadratically
            std::vector<LValue> vals;
            vals.reserve(result.count - 1);
            for (size_t i = 1; i < result.count; ++i)
                vals.push_back(result[i]);
            return MultiValue(vals, L2);
        }, nullptr, std::vector<clx::LUpValue>{ cell }));
}

//------------------ coroutine_running: returns the running thread
clx::MultiValue coroutine_running(clx::LState *L, const clx::LValue *args, size_t count) {
    clx::LThread *t = L->running_thread;
    clx::LValue r[2] = { clx::thread(t), clx::boolean(t->is_main) };
    return clx::MultiValue(r, 2, L);
}

//------------------ coroutine_close: closes a coroutine; with no argument it closes the running one
clx::MultiValue coroutine_close(clx::LState *L, const clx::LValue *args, size_t count) {
    clx::LThread *t = L->running_thread;
    if (count > 0) {
        if (!clx::is_thread(args[0]))
            clx::error(L, bad_arg_msg(L, 1, "coroutine.close", "thread expected").c_str());
        t = static_cast<clx::LThread *>(args[0].as_pointer());
    }
    if (t == L->running_thread) {
        if (t->is_main)
            clx::error(L, "cannot close main thread");
        //------------------ closing the running coroutine never returns; while another error is already unwinding a throw would terminate, so the flag alone supersedes it
        t->self_close = true;
        t->closing = true;
        if (std::uncaught_exceptions() == 0)
            throw clx::CoroutineClose();
        return clx::MultiValue();
    }
    return clx::close_thread(L, args[0]);
}

//------------------ luastd_coroutine: registers the coroutine library into the global state
void luastd_coroutine(LState *L) {
    clx::LValue co = clx::table(L);
    static constexpr clx::LazyReg coro_funcs[] = {
        { "create", coroutine_create },
        { "resume", coroutine_resume },
        { "yield", coroutine_yield },
        { "running", coroutine_running },
        { "status", coroutine_status },
        { "wrap", coroutine_wrap },
        { "close", coroutine_close },
        { "isyieldable", coroutine_isyieldable },
    };
    set_lazy_funcs(L, co, coro_funcs, std::size(coro_funcs));
    clx::set_global(L, "coroutine", co);
}

}
