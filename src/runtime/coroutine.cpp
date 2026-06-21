// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  coroutine.cpp · Coroutine Library          │
// └─────────────────────────────────────────────┘

#include "clx.h"

namespace clx {


//------------------ coroutine_create: creates a new coroutine (thread)
clx::MultiValue coroutine_create(clx::LState* L, const clx::LValue* args, size_t count) {
    clx::LValue func = count > 0 ? args[0] : clx::LValue();
    double stack_size = count > 1 ? clx::to_number(args[1], 1048576.0) : 1048576.0;
    return clx::MultiValue(clx::create_thread(L, func, stack_size));
}


//------------------ coroutine_resume: resumes a suspended coroutine
clx::MultiValue coroutine_resume(clx::LState* L, const clx::LValue* args, size_t count) {
    if (count == 0 || !clx::is_thread(args[0])) {
        clx::LValue r[2] = { clx::boolean(false), clx::string(L, "bad argument #1 to 'resume' (thread expected)") };
        return clx::MultiValue(r, 2);
    }
    LThread* t = static_cast<LThread*>(args[0].as_pointer());
    if (t->status == THREAD_DEAD) {
        clx::LValue r[2] = { clx::boolean(false), clx::string(L, "cannot resume dead coroutine") };
        return clx::MultiValue(r, 2);
    }

    clx::LValue thread_val = args[0];
    size_t arg_count = count > 1 ? count - 1 : 0;
    return clx::resume(L, thread_val, count > 1 ? args + 1 : nullptr, arg_count);
}


//------------------ coroutine_yield: yields execution from a coroutine
clx::MultiValue coroutine_yield(clx::LState* L, const clx::LValue* args, size_t count) {
    return clx::yield(L, args, count);
}


//------------------ coroutine_status: returns the status of a coroutine
clx::MultiValue coroutine_status(clx::LState* L, const clx::LValue* args, size_t count) {
    if (count == 0 || !clx::is_thread(args[0])) {
        return clx::MultiValue(clx::string(L, ""));
    }
    LThread* t = static_cast<LThread*>(args[0].as_pointer());
    if (t->status == THREAD_SUSPENDED) return clx::MultiValue(clx::string(L, "suspended"));
    if (t->status == THREAD_RUNNING)   return clx::MultiValue(clx::string(L, "running"));
    if (t->status == THREAD_DEAD)      return clx::MultiValue(clx::string(L, "dead"));
    return clx::MultiValue(clx::string(L, "normal"));
}


//------------------ coroutine_wrap: wraps a coroutine in a callable table
static clx::MultiValue coroutine_wrap(clx::LState* L, const clx::LValue* args, size_t count) {
    if (count == 0 || !clx::is_function(args[0])) {
        return clx::MultiValue();
    }

    clx::LValue thread = clx::create_thread(L, args[0]);

    clx::LValue wrapper = clx::table(L);
    clx::set_field(L, wrapper, "_co", thread);

    clx::LValue meta = clx::table(L);

    clx::LValue call_closure = clx::cfunction(L, [thread](LState* L2, const LValue* call_args, size_t call_arg_count) -> clx::MultiValue {
        if (call_arg_count == 0) return clx::MultiValue();
        const clx::LValue& self = call_args[0];
        if (!clx::is_table(self)) return clx::MultiValue();

        LThread* t = static_cast<LThread*>(thread.as_pointer());
        if (t->status == THREAD_DEAD) {
            return clx::MultiValue();
        }

        size_t resume_arg_count = (call_arg_count > 1) ? (call_arg_count - 1) : 0;
        clx::MultiValue result = clx::resume(L2, thread, call_arg_count > 1 ? call_args + 1 : nullptr, resume_arg_count);

        if (result.count <= 1) return clx::MultiValue();
        std::vector<clx::LValue> ret;
        for (size_t i = 1; i < result.count; ++i)
            ret.push_back(result[i]);
        return clx::MultiValue(ret);
    });

    clx::set_field(L, meta, "__call", call_closure);
    static_cast<clx::LTable*>(wrapper.as_pointer())->metatable = static_cast<clx::LTable*>(meta.as_pointer());

    return clx::MultiValue(wrapper);
}


//------------------ coroutine_running: returns the running thread
clx::MultiValue coroutine_running(clx::LState* L, const clx::LValue* args, size_t count) {
    clx::LThread* t = L->running_thread;
    clx::LValue r[2] = {clx::thread(t), clx::boolean(t->is_main)};
    return clx::MultiValue(r, 2);
}


//------------------ coroutine_close: closes a coroutine
clx::MultiValue coroutine_close(clx::LState* L, const clx::LValue* args, size_t count) {
    if (count == 0 || !clx::is_thread(args[0])) {
        clx::LValue r[2] = { clx::boolean(false), clx::string(L, "bad argument #1 to 'close' (thread expected)") };
        return clx::MultiValue(r, 2);
    }
    return clx::close_thread(L, args[0]);
}


//------------------ luastd_coroutine: registers the coroutine library into the global state
void luastd_coroutine(LState* L) {
    clx::LValue co = clx::table(L);
    clx::set_function(L, co, "create", coroutine_create);
    clx::set_function(L, co, "resume", coroutine_resume);
    clx::set_function(L, co, "yield", coroutine_yield);
    clx::set_function(L, co, "running", coroutine_running);
    clx::set_function(L, co, "status", coroutine_status);
    clx::set_function(L, co, "wrap", coroutine_wrap);
    clx::set_function(L, co, "close", coroutine_close);
    clx::set_global(L, "coroutine", co);
}


}
