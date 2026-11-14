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
    , close_requested(false)
    , self_close(false)
    , closing(false)
    , close_error(false) {
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

//---------- root_stash: move LState's live root-frame chain into a thread that is about to stop running
static inline void root_stash(LState *L, LThread *t) {
    t->top_root = L->top_root;
    L->top_root = nullptr;
}

//---------- root_load: move a thread's root-frame chain back into LState, leaving the thread's copy empty
static inline void root_load(LState *L, LThread *t) {
    L->top_root = t->top_root;
    t->top_root = nullptr;
}

//---------- coro_switch_to: the single site for the platform context switch (fiber end, resume
//---------- and yield each had their own copy). Conditions preserved exactly: the two middle
//---------- branches had identical bodies, so they are OR-ed, nothing else changes.
static inline void coro_switch_to(LThread *from, LThread *to) {
    //------------------ the stack-limit check follows the stack being entered: a recycled fiber keeps its
    //------------------ own "stack overflow" threshold, and the caller's is restored on the way back
    LState *Ls = to->state;
    char *saved_low = Ls->stack_low;
#if !defined(_WIN32)
    Ls->stack_low = to->stack_memory ? to->stack_memory + (64 * 1024) : nullptr;
#endif
#if defined(_WIN32)
    SwitchToFiber(to->fiber);
#elif ((defined(__APPLE__) || defined(__linux__)) && defined(__aarch64__)) || (defined(__linux__) && defined(__x86_64__))
    clx_coro_switch(&from->ctx, &to->ctx);
#else
    swapcontext(&from->ctx, &to->ctx);
#endif
    Ls->stack_low = saved_low;
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

            static constexpr CallSite coroutine_site = { "coroutine", 0, false, false };
            t->yield_args = call_function_rooted(L, t->function, args, argc, &coroutine_site);
            t->status = THREAD_DEAD;
        } catch (const CoroutineClose &) {
            //------------------ a __close that raised while closing keeps its error (already stored in yield_args)
            if (t->close_error) {
                t->has_error = true;
            } else {
                t->yield_args = MultiValue();
                t->has_error = false;
            }
            t->closing = false;
            t->status = THREAD_DEAD;
        } catch (const LRuntimeException &e) {
            //------------------ a __close that raised while unwinding replaced the error (stock reports the
            //------------------ last one to the resumer)
            LValue err = e.error_obj;
            if (L->has_close_err) {
                err = L->close_err;
                L->has_close_err = false;
            }
            t->yield_args = MultiValue(err);
            t->status = THREAD_DEAD;
            t->has_error = true;
        } catch (...) {
            t->yield_args = MultiValue(clx::LValue(L->intern_string("unknown error")));
            t->status = THREAD_DEAD;
            t->has_error = true;
        }

        if (t->self_close) {
            t->self_close = false;
            t->closing = false;
            if (t->close_error) {
                t->has_error = true;
            } else {
                t->has_error = false;
                t->yield_args = MultiValue();
            }
        }

        if (t->pre_unwind) {
            t->pre_unwind = false;
            t->has_error = false;
            continue;
        }
        break;
    }

    LThread *caller = t->caller;
    //------------------ the fiber is finished, so every frame on its stack is gone: any chain left over
    //------------------ belongs to already-unwound frames and must never be walked again (the stack memory
    //------------------ is recycled). A suspended fiber keeps its live chain via yield's root_stash.
    t->top_root = nullptr;
    root_load(L, caller);
    L->running_thread = caller;
    caller->status = THREAD_RUNNING;

    coro_switch_to(t, caller);
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
    t->self_close = false;
    t->closing = false;
    t->close_error = false;
    t->caller = nullptr;
    t->status = THREAD_SUSPENDED;
    t->top_root = nullptr;
    t->started = false;
    t->marked = 0;
    t->age = AGE_YOUNG;
    if (t->luaapi && L->luaapi_thread_reset)
        L->luaapi_thread_reset(L, t->luaapi);
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
#elif defined(__linux__) && defined(__x86_64__)
    if (!t->stack_memory)
        t->stack_memory = new char[t->stack_bytes];
    clx_coro_init(&t->ctx, t->stack_memory + t->stack_bytes, (void *)fiber_trampoline);
#else
    getcontext(&t->ctx);
    if (!t->stack_memory)
        t->stack_memory = new char[static_cast<size_t>(stack_size)];
    t->ctx.uc_stack.ss_sp = t->stack_memory;
    t->ctx.uc_stack.ss_size = static_cast<size_t>(stack_size);
    t->ctx.uc_link = nullptr;
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
    //------------------ every nested resume is a real C stack frame: refuse past stock's LUAI_MAXCCALLS
    //------------------ depth instead of overflowing the process stack
    if (L->resume_depth >= 200)
        throw LRuntimeException(LValue(L->intern_string(file_line_prefix(L) + "C stack overflow")));
    //------------------ assign() reuses the thread's already-constructed inline slots: no temp
    //------------------ MultiValue + move-assign copy chain on every resume.
    t->resume_args.assign(args, count, L);
    t->has_error = false;
    t->close_error = false;
#if !defined(_WIN32)
    // fiber_trampoline() reads g_starting_thread on the thread's first entry. create_thread() also sets it,
    // but with several coroutines created before the first resume it would point at the last one created.
    if (!t->started) g_starting_thread = t;
#endif
    t->started = true;
    if (L->gc_mode == LState::GCMode::Generational && t->age == AGE_OLD) {
        for (size_t i = 0; i < t->resume_args.count; ++i)
            gc_barrier_header(L, t, t->resume_args[i]);
    }
    t->caller = L->running_thread;
    t->caller->status = THREAD_NORMAL;
    t->status = THREAD_RUNNING;
    root_stash(L, L->running_thread);
    root_load(L, t);
    L->running_thread = t;

    struct DepthGuard {
        size_t &d;
        DepthGuard(size_t &depth)
            : d(depth) { ++d; }
        ~DepthGuard() { --d; }
    } depth_guard(L->resume_depth);

    coro_switch_to(t->caller, t);

    //------------------ Build (ok, yield_args...) directly in the return slot: one placement-new per
    //------------------ value, no temp buffer + re-copy (was: fill buf[total], then MultiValue(buf)
    //------------------ copied every slot a second time, plus dead default-ctors for the inline buf).
    size_t total = 1 + t->yield_args.count;
    MultiValue ret;
    ret.count = total;
    ret.alloc_L = L;
    LValue *dst = ret.inline_ptr();
    size_t n_inline = (total < MultiValue::INLINE_CAP) ? total : MultiValue::INLINE_CAP;
    new (dst) LValue(!t->has_error);
    for (size_t i = 1; i < n_inline; ++i)
        new (dst + i) LValue(t->yield_args[i - 1]);
    if (total <= MultiValue::INLINE_CAP) {
        ret.overflow = nullptr;
    } else {
        ret.overflow = L->alloc_overflow(total - MultiValue::INLINE_CAP);
        for (size_t i = MultiValue::INLINE_CAP; i < total; ++i)
            ret.overflow[i - MultiValue::INLINE_CAP] = t->yield_args[i - 1];
    }
    return ret;
}

//------------------ yield: yields from a coroutine (public API)
MultiValue yield(LState *L, const LValue *args, size_t count) {
    LThread *t = L->running_thread;
    if (t->is_main)
        clx::error(L, "attempt to yield from outside a coroutine");
    //------------------ stock cannot resume a coroutine across a C frame that has no continuation
    //------------------ (a sort comparator, a gsub replacement): the C stack below would be unwound
    if (L->c_yield_blocked > 0)
        clx::error(L, "attempt to yield across a C-call boundary");
    //------------------ a __close that runs while the coroutine is being closed sits under the close's own
    //------------------ C frame: stock refuses the yield there
    if (t->closing || t->self_close)
        clx::error(L, "attempt to yield across a C-call boundary");
    t->yield_args.assign(args, count, L);
    if (L->gc_mode == LState::GCMode::Generational && t->age == AGE_OLD) {
        for (size_t i = 0; i < t->yield_args.count; ++i)
            gc_barrier_header(L, t, t->yield_args[i]);
    }
    t->status = THREAD_SUSPENDED;
    // The coroutine may have stored young objects in its frame slots since the last minor GC scanned it.
    // An old suspended thread is only rescanned if remembered, so remember it whenever it suspends.
    if (L->gc_mode == LState::GCMode::Generational && t->age == AGE_OLD)
        L->gc_remember(t);

    LThread *caller = t->caller;
    root_stash(L, t);
    root_load(L, caller);
    L->running_thread = caller;
    caller->status = THREAD_RUNNING;

    coro_switch_to(t, caller);

    if (t->close_requested) {
        t->close_requested = false;
        t->closing = true;
        throw CoroutineClose();
    }

    ++t->yield_count;

    return t->resume_args;
}

//------------------ close_thread: closes a suspended coroutine (public API)
MultiValue close_thread(LState *L, const LValue &thread) {
    LThread *t = static_cast<LThread *>(thread.as_pointer());

    //------------------ a dead coroutine reports its pending error once, then close succeeds like stock
    if (t->status == THREAD_DEAD) {
        if (t->has_error) {
            LValue err = t->yield_args.count > 0 ? t->yield_args[0] : LValue();
            t->has_error = false;
            t->yield_args = MultiValue();
            return MultiValue(false, err);
        }
        return MultiValue(true);
    }

    if (t->status == THREAD_RUNNING)
        clx::error(L, "cannot close a running coroutine");
    if (t->status == THREAD_NORMAL)
        clx::error(L, "cannot close a normal coroutine");

    t->close_requested = true;

    MultiValue result = resume(L, thread, nullptr, 0);

    if (t->status == THREAD_DEAD) {
        //------------------ the error is reported once: a later close of the same dead coroutine succeeds
        bool failed = (result.count > 1 && !result[0].as_bool());
        LValue err = failed ? result[1] : LValue();
        t->has_error = false;
        t->yield_args = MultiValue();
        return failed ? MultiValue(false, err) : MultiValue(true);
    }

    return MultiValue(false);
}

}
