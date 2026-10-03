// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  threads.cpp · Coroutine thread management  │
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

//------------------ LThread::LThread — thread constructor
LThread::LThread()
    : state(nullptr)
    , status(THREAD_SUSPENDED)
    , caller(nullptr)
    , is_main(false)
    , has_error(false)
    , close_requested(false) {
    type = static_cast<uint8_t>(Thread);
    marked = 0;
    next = nullptr;
#if defined(_WIN32)
    fiber = nullptr;
#else
    stack_memory = nullptr;
#endif
}

//------------------ LThread::~LThread — thread destructor
LThread::~LThread() {
#if defined(_WIN32)
    if (fiber && !is_main)
        DeleteFiber(fiber);
#else
    if (stack_memory)
        delete[] stack_memory;
#endif
}

static void fiber_entry_impl(LThread *t) {
    LState *L = t->state;
    for (;;) {
        try {
            clx::LValue args[8];
            size_t argc = t->resume_args.count < 8 ? t->resume_args.count : 8;
            for (size_t i = 0; i < argc; ++i)
                args[i] = t->resume_args[i];
            t->resume_args = MultiValue();

            t->yield_args = call_function_rooted(L, t->function, args, argc, "coroutine", 0);
            t->status = THREAD_DEAD;
        } catch (const LRuntimeException &e) {

            t->yield_args = MultiValue(e.error_obj);
            t->status = THREAD_DEAD;
            t->has_error = true;
        } catch (...) {
            t->yield_args = MultiValue(clx::LValue(L->intern_string("unknown error")));
            t->status = THREAD_DEAD;
            t->has_error = true;
        }

        if (t->pre_unwind) {
            t->pre_unwind = false;
            t->has_error = false;
            continue;
        }
        break;
    }

    LThread *caller = t->caller;
    L->running_thread = caller;
    caller->status = THREAD_RUNNING;

#if defined(_WIN32)
    SwitchToFiber(caller->fiber);
#elif (defined(__APPLE__) || defined(__linux__)) && defined(__aarch64__)
    clx_coro_switch(&t->ctx, &caller->ctx);
#elif defined(__linux__) && defined(__x86_64__)
    clx_coro_switch(&t->ctx, &caller->ctx);
#else
    swapcontext(&t->ctx, &caller->ctx);
#endif
}

#if defined(_WIN32)

static void WINAPI fiber_trampoline(LPVOID param) {
    LThread *t = static_cast<LThread *>(param);
    t->fiber_started = true;
    for (;;) {
        fiber_entry_impl(t);
    }
}
#else
static thread_local LThread *g_starting_thread = nullptr;

static void fiber_trampoline() {
    fiber_entry_impl(g_starting_thread);
}
#endif

//------------------ create_thread: creates a new coroutine thread (public API)
LValue create_thread(LState *L, const LValue &func, double stack_size) {
    L->gc_maybe_collect();

    LThread *t = L->free_threads;
    bool recycled = (t != nullptr);
    uint8_t prev_age_th = recycled ? t->age : AGE_YOUNG;
#if defined(_WIN32)
    bool pooled_suspended = recycled && t->status == THREAD_SUSPENDED && t->fiber_started;
#endif
    if (t) {
        L->free_threads = static_cast<LThread *>(t->next);
    } else {
        t = new LThread();
    }
    t->state = L;
    t->function = func;
    t->stack_bytes = static_cast<size_t>(stack_size);
    t->resume_args = MultiValue();
    t->yield_args = MultiValue();
    t->has_error = false;
    t->close_requested = false;
    t->caller = nullptr;
    t->status = THREAD_SUSPENDED;
    t->marked = 0;
    t->age = AGE_YOUNG;
    if (recycled && prev_age_th != AGE_YOUNG && (L->gc_phase == LState::GCPhase::Sweeping || L->gc_minor_active)) {

        t->flags |= LFLAG_GC_PIN;
        L->gc_pinned.push(t);
    }

#if defined(_WIN32)
    if (recycled) {
        if (t->fiber) {
            L->free_fiber_threads--;
            t->pre_unwind = pooled_suspended;
        } else {
            t->fiber = CreateFiber(static_cast<SIZE_T>(stack_size), fiber_trampoline, t);
        }
    } else {
        t->fiber = CreateFiber(static_cast<SIZE_T>(stack_size), fiber_trampoline, t);
    }
#elif (defined(__APPLE__) || defined(__linux__)) && defined(__aarch64__)
    if (!t->stack_memory)
        t->stack_memory = new char[t->stack_bytes];
    clx_coro_init(&t->ctx, t->stack_memory + t->stack_bytes, (void *)fiber_trampoline);
    g_starting_thread = t;
#elif defined(__linux__) && defined(__x86_64__)
    if (!t->stack_memory)
        t->stack_memory = new char[t->stack_bytes];
    clx_coro_init(&t->ctx, t->stack_memory + t->stack_bytes, (void *)fiber_trampoline);
    g_starting_thread = t;
#else
    getcontext(&t->ctx);
    if (!t->stack_memory)
        t->stack_memory = new char[static_cast<size_t>(stack_size)];
    t->ctx.uc_stack.ss_sp = t->stack_memory;
    t->ctx.uc_stack.ss_size = static_cast<size_t>(stack_size);
    t->ctx.uc_link = nullptr;
    g_starting_thread = t;
    makecontext(&t->ctx, fiber_trampoline, 0);
#endif

    t->next = L->allocated_objects;
    L->allocated_objects = t;
    L->gc_recent.push(t);
    if (!recycled)
        L->object_count++;
    L->allocated_bytes += sizeof(LThread) + t->stack_bytes;
    return LValue(Thread, t);
}

//------------------ resume: resumes a suspended coroutine (public API)
MultiValue resume(LState *L, const LValue &thread, const LValue *args, size_t count) {
    LThread *t = static_cast<LThread *>(thread.as_pointer());
    t->resume_args = MultiValue(args, count, L);
    if (L->gc_mode == LState::GCMode::Generational && t->age == AGE_OLD) {
        for (size_t i = 0; i < t->resume_args.count; ++i)
            gc_barrier_header(L, t, t->resume_args[i]);
    }
    t->caller = L->running_thread;
    t->caller->status = THREAD_NORMAL;
    t->status = THREAD_RUNNING;
    L->running_thread = t;

#if defined(_WIN32)
    SwitchToFiber(t->fiber);
#elif (defined(__APPLE__) || defined(__linux__)) && defined(__aarch64__)
    clx_coro_switch(&t->caller->ctx, &t->ctx);
#elif defined(__linux__) && defined(__x86_64__)
    clx_coro_switch(&t->caller->ctx, &t->ctx);
#else
    swapcontext(&t->caller->ctx, &t->ctx);
#endif

    size_t total = 1 + t->yield_args.count;
    LValue *buf;
    LValue inline_buf[3];
    if (total <= 3) {
        buf = inline_buf;
    } else {
        buf = L->alloc_overflow(total);
    }
    buf[0] = boolean(!t->has_error);
    for (size_t i = 0; i < t->yield_args.count; ++i)
        buf[1 + i] = t->yield_args[i];
    t->has_error = false;
    return MultiValue(buf, total, L);
}

//------------------ yield: yields from a coroutine (public API)
MultiValue yield(LState *L, const LValue *args, size_t count) {
    LThread *t = L->running_thread;
    if (t->is_main)
        clx::error(L, "attempt to yield from outside a coroutine");
    t->yield_args = MultiValue(args, count, L);
    if (L->gc_mode == LState::GCMode::Generational && t->age == AGE_OLD) {
        for (size_t i = 0; i < t->yield_args.count; ++i)
            gc_barrier_header(L, t, t->yield_args[i]);
    }
    t->status = THREAD_SUSPENDED;

    LThread *caller = t->caller;
    L->running_thread = caller;
    caller->status = THREAD_RUNNING;

#if defined(_WIN32)
    SwitchToFiber(caller->fiber);
#elif (defined(__APPLE__) || defined(__linux__)) && defined(__aarch64__)
    clx_coro_switch(&t->ctx, &caller->ctx);
#elif defined(__linux__) && defined(__x86_64__)
    clx_coro_switch(&t->ctx, &caller->ctx);
#else
    swapcontext(&t->ctx, &caller->ctx);
#endif

    if (t->close_requested) {
        t->close_requested = false;
        t->has_error = true;
        throw LRuntimeException(clx::string(L, "thread is being closed"));
    }

    return t->resume_args;
}

//------------------ close_thread: closes a suspended coroutine (public API)
MultiValue close_thread(LState *L, const LValue &thread) {
    LThread *t = static_cast<LThread *>(thread.as_pointer());

    if (t->status == THREAD_DEAD)
        return MultiValue(clx::boolean(true));

    if (t->status == THREAD_RUNNING)
        clx::error(L, "cannot close running coroutine");
    if (t->status == THREAD_NORMAL)
        clx::error(L, "cannot close normal coroutine");

    t->close_requested = true;

    MultiValue result = resume(L, thread, nullptr, 0);

    if (t->status == THREAD_DEAD)
        return MultiValue(clx::boolean(true));

    return MultiValue(clx::boolean(false));
}

}
