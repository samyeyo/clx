// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  coroutine.cpp · Coroutine Library          │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include <vector>

namespace clx {


//------------------ fiber_entry_impl: entry point for a new coroutine fiber
static void fiber_entry_impl(LThread* t) {
    LState* L = t->state;
    try {
        clx::LValue args[8];
        size_t argc = t->resume_args.count < 8 ? t->resume_args.count : 8;
        for(size_t i = 0; i < argc; ++i) args[i] = t->resume_args[i];
        t->resume_args = MultiValue();

        size_t prev_top = L->shadow_top;
        for(size_t i = 0; i < argc; ++i) L->shadow_stack[L->shadow_top++] = &args[i];

        t->yield_args = call_function(L, t->function, args, argc, "coroutine", 0);

        L->shadow_top = prev_top;
        t->status = THREAD_DEAD;
    } catch (const LRuntimeException& e) {
        t->yield_args = MultiValue(e.error_obj);
        t->status = THREAD_DEAD;
        t->has_error = true;
    } catch (...) {
        t->yield_args = MultiValue(clx::string(L, "unknown error"));
        t->status = THREAD_DEAD;
        t->has_error = true;
    }

    LThread* caller = t->caller;
    L->running_thread = caller;
    caller->status = THREAD_RUNNING;

#if defined(_WIN32)
    SwitchToFiber(caller->fiber);
#else
    swapcontext(&t->ctx, &caller->ctx);
#endif
}

//------------------ fiber_trampoline: Win32 fiber entry trampoline
#if defined(_WIN32)

static void WINAPI fiber_trampoline(LPVOID param) {
    fiber_entry_impl(static_cast<LThread*>(param));
}
#else
//------------------ g_starting_thread: thread-local pointer used to pass initial thread to makecontext
thread_local LThread* g_starting_thread = nullptr;

//------------------ fiber_trampoline: POSIX ucontext entry trampoline
static void fiber_trampoline() {
    fiber_entry_impl(g_starting_thread);
}
#endif



//------------------ clx_coroutine_create: creates a new coroutine (thread)
clx::MultiValue clx_coroutine_create(clx::LState* L, const clx::LValue* args, size_t count) {
    clx::LValue func = count > 0 ? args[0] : clx::LValue();
    double stack_size = count > 1 ? clx::to_number(args[1], 1048576.0) : 1048576.0;

    LThread* t = new LThread();
    t->state = L;
    t->function = func;

#if defined(_WIN32)
    t->fiber = CreateFiber(static_cast<SIZE_T>(stack_size), fiber_trampoline, t);
#else
    getcontext(&t->ctx);
    t->stack_memory = new char[static_cast<size_t>(stack_size)];
    t->ctx.uc_stack.ss_sp = t->stack_memory;
    t->ctx.uc_stack.ss_size = static_cast<size_t>(stack_size);
    t->ctx.uc_link = nullptr;
    g_starting_thread = t;
    makecontext(&t->ctx, fiber_trampoline, 0);
#endif

    t->next = L->allocated_objects;
    L->allocated_objects = t;
    L->object_count++;
    return clx::MultiValue(clx::LValue(LType::Thread, t));
}


//------------------ clx_coroutine_resume: resumes a suspended coroutine
clx::MultiValue clx_coroutine_resume(clx::LState* L, const clx::LValue* args, size_t count) {
    if (count == 0 || !clx::is_thread(args[0])) {
        clx::LValue r[2] = { clx::boolean(false), clx::string(L, "bad argument #1 to 'resume' (thread expected)") };
        return clx::MultiValue(r, 2);
    }
    LThread* t = static_cast<LThread*>(args[0].as_pointer());
    if (t->status == THREAD_DEAD) {
        clx::LValue r[2] = { clx::boolean(false), clx::string(L, "cannot resume dead coroutine") };
        return clx::MultiValue(r, 2);
    }

    t->resume_args = clx::MultiValue(count > 1 ? args + 1 : nullptr, count > 1 ? count - 1 : 0);
    t->caller = L->running_thread;
    t->caller->status = THREAD_NORMAL;
    t->status = THREAD_RUNNING;
    L->running_thread = t;

#if defined(_WIN32)
    SwitchToFiber(t->fiber);
#else
    swapcontext(&t->caller->ctx, &t->ctx);
#endif

    std::vector<clx::LValue> ret;
    ret.push_back(clx::boolean(!t->has_error));
    for(size_t i = 0; i < t->yield_args.count; ++i) ret.push_back(t->yield_args[i]);
    t->has_error = false;
    return clx::MultiValue(ret);
}


//------------------ clx_coroutine_yield: yields execution from a coroutine
clx::MultiValue clx_coroutine_yield(clx::LState* L, const clx::LValue* args, size_t count) {
    LThread* t = L->running_thread;
    if (t->is_main) {
        clx::error(L, "attempt to yield from outside a coroutine");
    }
    t->yield_args = clx::MultiValue(args, count);
    t->status = THREAD_SUSPENDED;

    LThread* caller = t->caller;
    L->running_thread = caller;
    caller->status = THREAD_RUNNING;

#if defined(_WIN32)
    SwitchToFiber(caller->fiber);
#else
    swapcontext(&t->ctx, &caller->ctx);
#endif

    return t->resume_args;
}


//------------------ clx_coroutine_status: returns the status of a coroutine
clx::MultiValue clx_coroutine_status(clx::LState* L, const clx::LValue* args, size_t count) {
    if (count == 0 || !clx::is_thread(args[0])) {
        return clx::MultiValue(clx::string(L, ""));
    }
    LThread* t = static_cast<LThread*>(args[0].as_pointer());
    if (t->status == THREAD_SUSPENDED) return clx::MultiValue(clx::string(L, "suspended"));
    if (t->status == THREAD_RUNNING)   return clx::MultiValue(clx::string(L, "running"));
    if (t->status == THREAD_DEAD)      return clx::MultiValue(clx::string(L, "dead"));
    return clx::MultiValue(clx::string(L, "normal"));
}


//------------------ clx_coroutine_wrap: wraps a coroutine in a callable table
static clx::MultiValue clx_coroutine_wrap(clx::LState* L, const clx::LValue* args, size_t count) {
    if (count == 0 || !clx::is_function(args[0])) {
        return clx::MultiValue();
    }

    clx::MultiValue create_result = clx_coroutine_create(L, args, 1);
    clx::LValue thread = create_result.count > 0 ? create_result[0] : clx::LValue();

    clx::LValue wrapper = clx::table(L);
    clx::set_field(L, wrapper, "_co", thread);

    clx::LValue meta = clx::table(L);

    clx::LValue call_closure = clx::cfunction(L, [](LState* L, const LValue* args, size_t arg_count) -> clx::MultiValue {
        if (arg_count == 0) return clx::MultiValue();

        const clx::LValue& self = args[0];
        if (!clx::is_table(self)) return clx::MultiValue();

        clx::LValue co_val = clx::get_field(L, self, "_co");
        if (!clx::is_thread(co_val)) return clx::MultiValue();

        LThread* t = static_cast<LThread*>(co_val.as_pointer());

        if (t->status == THREAD_DEAD) {
            return clx::MultiValue();
        }

        size_t resume_arg_count = (arg_count > 1) ? (arg_count - 1) : 0;
        t->resume_args.count = resume_arg_count;
        for (size_t i = 0; i < resume_arg_count; ++i) {
            t->resume_args[i] = args[i + 1];
        }

        t->caller = L->running_thread;
        t->status = THREAD_RUNNING;
        LThread* prev = L->running_thread;
        L->running_thread = t;

#if !defined(_WIN32)
        LThread* prev_starting = g_starting_thread;
        g_starting_thread = t;
#endif

#if defined(_WIN32)
        SwitchToFiber(t->fiber);
#else
        swapcontext(&t->caller->ctx, &t->ctx);
#endif

#if !defined(_WIN32)
        g_starting_thread = prev_starting;
#endif
        L->running_thread = prev;

        if (t->has_error) {
            throw clx::LRuntimeException(t->yield_args.count > 0 ? t->yield_args[0] : clx::string(L, "error"));
        }

        return t->yield_args;
    });

    clx::set_field(L, meta, "__call", call_closure);
    static_cast<clx::LTable*>(wrapper.as_pointer())->metatable = static_cast<clx::LTable*>(meta.as_pointer());

    return clx::MultiValue(wrapper);
}



//------------------ luastd_coroutine: registers the coroutine library into the global state
void luastd_coroutine(LState* L) {
    clx::LValue co = clx::table(L);
    clx::set_function(L, co, "create", clx_coroutine_create);
    clx::set_function(L, co, "resume", clx_coroutine_resume);
    clx::set_function(L, co, "yield", clx_coroutine_yield);
    clx::set_function(L, co, "status", clx_coroutine_status);
    clx::set_function(L, co, "wrap", clx_coroutine_wrap);
    clx::set_global(L, "coroutine", co);
}



//------------------ create_thread: creates and starts a new thread/coroutine (public API)
LValue create_thread(LState* L, const LValue& func, double stack_size) {
    LThread* t = new LThread();
    t->state = L;
    t->function = func;

#if defined(_WIN32)
    t->fiber = CreateFiber(static_cast<SIZE_T>(stack_size), fiber_trampoline, t);
#else
    getcontext(&t->ctx);
    t->stack_memory = new char[static_cast<size_t>(stack_size)];
    t->ctx.uc_stack.ss_sp = t->stack_memory;
    t->ctx.uc_stack.ss_size = static_cast<size_t>(stack_size);
    t->ctx.uc_link = nullptr;
    g_starting_thread = t;
    makecontext(&t->ctx, fiber_trampoline, 0);
#endif

    t->next = L->allocated_objects;
    L->allocated_objects = t;
    L->object_count++;
    return LValue(LType::Thread, t);
}


//------------------ resume: resumes a thread/coroutine (public API)
MultiValue resume(LState* L, const LValue& thread, const LValue* args, size_t count) {
    LThread* t = static_cast<LThread*>(thread.as_pointer());
    t->resume_args = MultiValue(args, count);
    t->caller = L->running_thread;
    t->caller->status = THREAD_NORMAL;
    t->status = THREAD_RUNNING;
    L->running_thread = t;

#if defined(_WIN32)
    SwitchToFiber(t->fiber);
#else
    swapcontext(&t->caller->ctx, &t->ctx);
#endif

    std::vector<LValue> ret;
    ret.push_back(boolean(!t->has_error));
    for (size_t i = 0; i < t->yield_args.count; ++i)
        ret.push_back(t->yield_args[i]);
    t->has_error = false;
    return MultiValue(ret);
}


//------------------ yield: yields from a thread/coroutine (public API)
MultiValue yield(LState* L, const LValue* args, size_t count) {
    LThread* t = L->running_thread;
    if (t->is_main)
        clx::error(L, "attempt to yield from outside a coroutine");
    t->yield_args = MultiValue(args, count);
    t->status = THREAD_SUSPENDED;

    LThread* caller = t->caller;
    L->running_thread = caller;
    caller->status = THREAD_RUNNING;

#if defined(_WIN32)
    SwitchToFiber(caller->fiber);
#else
    swapcontext(&t->ctx, &caller->ctx);
#endif

    return t->resume_args;
}

}
