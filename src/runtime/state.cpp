// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  state.cpp · LState, allocators & modules   │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include "clx_runtime.h"
#include "internal.h"
#include "vm/vm_convert.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <sys/wait.h>
#endif

namespace clx {

//------------------ main_stack_low: lowest safe address of the main thread's C stack (defined below)
char *main_stack_low();

//------------------ LState::LState — state constructor
LState::LState()
    : allocated_objects(nullptr)
    , free_tables(nullptr)
    , free_tables_big(nullptr)
    , free_functions(nullptr)
    , metatabled_ud(nullptr)
    , object_count(0)
    , gc_bytes_threshold(2 * 1024 * 1024)
    , string_metatable(nullptr) {
    _G = slab_alloc_table();
    allocated_bytes += sizeof(LTable);
    _G->next = allocated_objects;
    allocated_objects = _G;
    object_count++;
    _G->settable(LValue(intern_string("_G")), LValue(Table, _G));

    str_index = LValue(intern_string("__index"));
    str_newindex = LValue(intern_string("__newindex"));
    str_gc = LValue(intern_string("__gc"));
    str_mode = LValue(intern_string("__mode"));
    str_call = LValue(intern_string("__call"));
    str_close = LValue(intern_string("__close"));
    str_pairs = LValue(intern_string("__pairs"));
    str_tostring = LValue(intern_string("__tostring"));
    str_len = LValue(intern_string("__len"));
    str_eq = LValue(intern_string("__eq"));
    str_lt = LValue(intern_string("__lt"));
    str_le = LValue(intern_string("__le"));
    str_concat = LValue(intern_string("__concat"));
    str_add = LValue(intern_string("__add"));
    str_sub = LValue(intern_string("__sub"));
    str_mul = LValue(intern_string("__mul"));
    str_div = LValue(intern_string("__div"));
    str_mod = LValue(intern_string("__mod"));
    str_idiv = LValue(intern_string("__idiv"));
    str_pow = LValue(intern_string("__pow"));
    str_unm = LValue(intern_string("__unm"));
    str_band = LValue(intern_string("__band"));
    str_bor = LValue(intern_string("__bor"));
    str_bxor = LValue(intern_string("__bxor"));
    str_shl = LValue(intern_string("__shl"));
    str_shr = LValue(intern_string("__shr"));
    str_bnot = LValue(intern_string("__bnot"));
    str_metatable = LValue(intern_string("__metatable"));
    str_name = LValue(intern_string("__name"));
    str_ipairs = LValue(intern_string("__ipairs"));

    //------------------ GC work-vector pre-sizing (callgrind: vector growth is

    gc_worklist.reserve(1 << 18);
    gc_recent.reserve(1 << 18);
    gc_remembered.reserve(1 << 16);
    gc_pinned.reserve(1 << 16);

    //------------------ GC mode / tuning knobs (environment override)

    if (const char *e = getenv("CLX_GC_MODE")) {
        if (strcmp(e, "incremental") == 0 || strcmp(e, "incr") == 0) {
            gc_mode = GCMode::Incremental;
        }
    }
    if (const char *e = getenv("CLX_GC_MINOR_KB")) {
        long long v = atoll(e);
        if (v > 0) {
            gc_minor_threshold = size_t(v) * 1024;
            gc_minor_threshold_env = true;
        }
    }
    if (const char *e = getenv("CLX_GC_MAJOR_KB")) {
        long long v = atoll(e);
        if (v > 0)
            gc_major_threshold = size_t(v) * 1024;
    }

    stack_low = main_stack_low();
}

//------------------ main_stack_low: lowest address the process main stack may reach, plus a reserve for
//------------------ the deepest frame's own locals and for unwinding
char *main_stack_low() {
#if defined(_WIN32)
    return windows_stack_low();
#elif defined(__APPLE__)
    size_t size = pthread_get_stacksize_np(pthread_self());
    char *high = static_cast<char *>(pthread_get_stackaddr_np(pthread_self()));
    if (!high || size <= (128u * 1024u))
        return nullptr;
    return high - size + (64 * 1024);
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0)
        return nullptr;
    void *addr = nullptr;
    size_t size = 0;
    int rc = pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    if (rc != 0 || !addr || size <= (128u * 1024u))
        return nullptr;
    return static_cast<char *>(addr) + (64 * 1024);
#else
    return nullptr;
#endif
}

//------------------ throw_stack_overflow: raises stock's "stack overflow" error
void throw_stack_overflow(LState *L) {
    throw_runtime_error(file_line_prefix(L) + "stack overflow");
}

//------------------ raise_stack_overflow: raises the error a fast body recorded, at the site that overflowed
void raise_stack_overflow(LState *L) {
    L->current_site = L->stack_overflow_site;
    L->stack_overflow_site = nullptr;
    throw_stack_overflow(L);
}

static void dtor_free_table(LState *L, LTable *t) {
    if (clx_table_release_ptr && t->ext && t->ext->vm_origin >= 0)
        clx_table_release_ptr(L, t);
    table_free_array(t->array, t->array_types, t->small_array);
    if (t->ext) {
        table_free_hash(t->ext->entries, t->ext->hash_bitmap);
        if (t->ext->ic) {
            delete[] t->ext->ic;
            t->ext->ic = nullptr;
        }
        delete t->ext;
        t->ext = nullptr;
    }
    t->array_size = t->array_cap = 0;
    if (t->flags & LFLAG_SLAB) {
        //------------------ slab object: storage lives in a slab, released whole by slab_release()
        t->flags &= ~LFLAG_SLAB;
        return;
    }
    delete t;
}

//------------------ LState::slab_alloc_table — bump-allocate an LTable from the current slab
LTable *LState::slab_alloc_table() {
    static_assert(alignof(LTable) <= 16, "slab bump assumes 16-byte-aligned table slots");
    if (slab_current + sizeof(LTable) > slab_end) {
        constexpr size_t kAlign = 16;

        size_t slab_bytes = sizeof(TableSlab) + kAlign + SLAB_TABLE_COUNT * sizeof(LTable);
        char *mem = static_cast<char *>(std::malloc(slab_bytes));
        if (!mem)
            throw std::bad_alloc();
        TableSlab *slab = reinterpret_cast<TableSlab *>(mem);
        slab->next = slab_blocks;
        slab_blocks = slab;
        uintptr_t data = reinterpret_cast<uintptr_t>(mem) + sizeof(TableSlab);
        data = (data + (kAlign - 1)) & ~static_cast<uintptr_t>(kAlign - 1);
        slab_current = reinterpret_cast<char *>(data);
        slab_end = mem + slab_bytes;
    }
    LTable *t = new (slab_current) LTable();
    slab_current += sizeof(LTable);
    t->flags |= LFLAG_SLAB;
    return t;
}

//------------------ LState::slab_release — free all table slabs (state close only)
void LState::slab_release() {
    TableSlab *s = slab_blocks;
    while (s) {
        TableSlab *n = s->next;
        std::free(s);
        s = n;
    }
    slab_blocks = nullptr;
    slab_current = slab_end = nullptr;
}

//------------------ FinalizingUdGuard — keep the dying userdata recognizable as a full userdata for __gc
struct FinalizingUdGuard {
    LState *L;
    FinalizingUdGuard(LState *S, LUserdata *ud)
        : L(S) {
        L->finalizing_ud.push_back(ud);
    }
    ~FinalizingUdGuard() { L->finalizing_ud.pop_back(); }
};

//------------------ LState::invoke_gc_finalizer — call __gc metamethod on userdata
void LState::invoke_gc_finalizer(LUserdata *ud, const char *tag) {
    if (!ud->metatable)
        return;
    //------------------ any non-nil __gc is called, as in stock: a table or userdata with __call finalizes the
    //------------------ object, and a value that cannot be called raises the usual error. Like stock, that error
    //------------------ is swallowed: luaE_warnerror only reaches a host warning function, and the default host
    //------------------ has none, so a failing __gc never stops the collection or prints anything
    LValue gc_func = ud->metatable->gettable(this->str_gc);
    if (gc_func.type == Nil)
        return;
    LValue args[1] = { LValue(UserData, ud) };
    RootRange rr(this, args, 1);
    FinalizingUdGuard finalizing(this, ud);
    try {
        CallSite gc_site = { tag, 0, false, false };
        call_function(this, gc_func, args, 1, &gc_site);
    } catch (...) {
    }
}

//------------------ LState::invoke_gc_finalizer — call __gc metamethod on a table. The table is rooted for the
//------------------ duration of the call: a finalizer that allocates may trigger a collection, and an unrooted
//------------------ object would then be swept out from under its own finalizer.
void LState::invoke_gc_finalizer(LTable *t, const char *tag) {
    if (!t)
        return;
    LTable *mt = tbl_metatable(t);
    if (!mt)
        return;
    LValue gc_func = mt->gettable(this->str_gc);
    if (gc_func.type == Nil)
        return;
    LValue args[1] = { LValue(Table, t) };
    RootRange rr(this, args, 1);
    try {
        CallSite gc_site = { tag, 0, false, false };
        call_function(this, gc_func, args, 1, &gc_site);
    } catch (...) {
    }
}

//------------------ LState::run_exit_finalizers — run the __gc of every marked table and userdata still alive,
//------------------ the way stock's lua_close does: an object is marked when a metatable carrying __gc is
//------------------ installed, so a later __gc assignment cannot mark it retroactively. The candidates are pinned
//------------------ first so a finalizer that allocates cannot sweep (and recycle) one before its own __gc runs.
void LState::run_exit_finalizers() {
    std::vector<LHeader *> dying;
    for (int pass = 0; pass < 2; ++pass) {
        for (LHeader *h = (pass == 0) ? allocated_objects : old_objects; h; h = h->next) {
            if ((h->type == static_cast<uint8_t>(UserData) || h->type == static_cast<uint8_t>(Table)) &&
                (h->flags & LFLAG_GC_MARKED))
                dying.push_back(h);
        }
    }
    for (LHeader *h : dying)
        h->flags |= LFLAG_GC_PIN;
    for (LHeader *h : dying) {
        if (h->type == static_cast<uint8_t>(UserData))
            invoke_gc_finalizer(static_cast<LUserdata *>(h), "StateClose_Finalizer");
        else
            invoke_gc_finalizer(static_cast<LTable *>(h), "StateClose_Finalizer");
    }
}

void (*clx_io_flush_all_ptr)() = nullptr;
void (*clx_io_write_ptr)(FILE *fp, const char *p, size_t n) = nullptr;
MultiValue (*clx_string_dump_hook)(LState *L, const LValue *args, size_t count) = nullptr;
bool (*clx_vm_debug_upvalue_hook)(LState *L, LHeader *fn, int64_t n, LValue &name, LValue &value) = nullptr;
bool (*clx_vm_debug_getinfo_hook)(LState *L, const LValue &fn, const char *what, LValue &out) = nullptr;

//------------------ exec_result: stock luaL_execresult — a wait status decoded into (ok, "exit"|"signal",
//------------------ code). Core-resident so a popen handle's close reports the same triple as os.execute
//------------------ without the io archive depending on the os archive (--minimal --modules io links alone)
MultiValue exec_result(LState *L, int status) {
    if (status == -1) {
        int en = errno;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", std::strerror(en));
        return MultiValue({ LValue(), LValue(L->intern_string(buf)), clx::integer(en) });
    }
#if defined(_WIN32) || defined(_WIN64)
    int code = status;
    const char *what = "exit";
#else
    int code = WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status);
    const char *what = WIFSIGNALED(status) ? "signal" : "exit";
#endif
    return MultiValue({ code == 0 ? clx::boolean(true) : clx::LValue(), clx::string(L, what),
        clx::integer(static_cast<int64_t>(code)) });
}

//------------------ LState::~LState — state destructor
LState::~LState() {
    if (clx_io_flush_all_ptr)
        clx_io_flush_all_ptr();
    if (luaapi_cleanup)
        luaapi_cleanup(this);
    if (gc_phase == GCPhase::Sweeping) {
        while (gc_phase == GCPhase::Sweeping)
            gc_step();
    }

    run_exit_finalizers();

    for (int pass = 0; pass < 2; ++pass) {
        LHeader *curr = (pass == 0) ? allocated_objects : old_objects;
        while (curr) {
            LHeader *next = curr->next;
            if (curr->flags & LFLAG_VM_PROXY) {
                if (clx_free_vm_proxy_ptr)
                    clx_free_vm_proxy_ptr(this, curr);
            } else if (curr->type == static_cast<uint8_t>(Table))
                dtor_free_table(this, static_cast<LTable *>(curr));
            else if (curr->type == static_cast<uint8_t>(Function))
                delete static_cast<LCFunction *>(curr);
            else if (curr->type == static_cast<uint8_t>(UserData))
                delete[] reinterpret_cast<char *>(curr);
            else if (curr->type == static_cast<uint8_t>(Thread))
                delete static_cast<LThread *>(curr);
            curr = next;
        }
    }
    allocated_objects = nullptr;
    old_objects = nullptr;

    {
        LTable *t = static_cast<LTable *>(gc_finalizable);
        while (t) {
            LTable *nxt = static_cast<LTable *>(t->next);
            if (t->flags & LFLAG_GC_MARKED)
                invoke_gc_finalizer(t, "GC_Finalizer");
            dtor_free_table(this, t);
            t = nxt;
        }
    }
    gc_finalizable = nullptr;

    {
        LUserdata *ud = static_cast<LUserdata *>(gc_finalizable_ud);
        while (ud) {
            LUserdata *nxt = static_cast<LUserdata *>(ud->next);
            if (ud->flags & LFLAG_GC_MARKED)
                invoke_gc_finalizer(ud, "GC_Finalizer");
            delete[] reinterpret_cast<char *>(ud);
            ud = nxt;
        }
    }
    gc_finalizable_ud = nullptr;

    LTable *ft = free_tables;
    while (ft) {
        LTable *next = static_cast<LTable *>(ft->next);
        dtor_free_table(this, ft);
        ft = next;
    }
    free_tables = nullptr;
    ft = free_tables_big;
    while (ft) {
        LTable *next = static_cast<LTable *>(ft->next);
        dtor_free_table(this, ft);
        ft = next;
    }
    free_tables_big = nullptr;

    slab_release();

    LCFunction *ff = free_functions;
    while (ff) {
        LCFunction *next = static_cast<LCFunction *>(ff->next);
        delete ff;
        ff = next;
    }
    free_functions = nullptr;

    LThread *fth = free_threads;
    while (fth) {
        LThread *nxt0 = static_cast<LThread *>(fth->next);
        delete fth;
        fth = nxt0;
    }
    free_threads = nullptr;
    //------------------ overflow blocks are a chain of malloc'd raw allocations (see alloc_overflow)
    while (overflow_blocks) {
        void *next = *static_cast<void **>(overflow_blocks);
        std::free(overflow_blocks);
        overflow_blocks = next;
    }
    overflow_heap = nullptr;
    overflow_heap_cap = 0;
    overflow_heap_used = 0;
    gc_worklist.free();
    gc_recent.free();
    gc_pinned.free();

    //------------------ the main thread is on neither GC chain, so it is released here: last, because
    //------------------ run_exit_finalizers can run a collection (and the finalizer's own calls) that still
    //------------------ consult it
    delete main_thread;
    main_thread = nullptr;
}

//------------------ CloseGuard::~CloseGuard — close guard destructor
CloseGuard::~CloseGuard() noexcept(false) {
    //------------------ <close> accepts any value carrying a __close metamethod: tables and userdata
    //------------------ (a file handle is userdata, so restricting this to tables left files open)
    LTable *mt = nullptr;
    if (val.type == Table)
        mt = tbl_metatable(static_cast<LTable *>(val.as_pointer()));
    else if (val.type == UserData)
        mt = static_cast<LUserdata *>(val.as_pointer())->metatable;
    if (!mt)
        return;
    LValue close_func = mt->gettable(L->str_close);
    if (close_func.type == Nil)
        return;

    LValue args[2] = { val, LValue() };
    size_t nargs = 1;
    LThread *th = L->running_thread;
    bool unwinding = std::uncaught_exceptions() > 0;
    bool self_closing = th && (th->closing || th->self_close);
    //------------------ stock hands __close the error being unwound, or the one an earlier __close of the
    //------------------ same unwind raised; normal exits and clean closes get the object alone, with no
    //------------------ second argument at all (not a nil one)
    if (th && th->closing) {
        if (th->close_error && th->yield_args.count > 0) {
            args[1] = th->yield_args[0];
            nargs = 2;
        }
    } else if (unwinding) {
        if (L->has_close_err) {
            args[1] = L->close_err;
            nargs = 2;
        } else if (clx_unwinding_error) {
            args[1] = *clx_unwinding_error;
            nargs = 2;
        }
    }
    try {
        static constexpr CallSite close_site = { "CloseGuard", 0, false, false };
        call_function_rooted(L, close_func, args, nargs, &close_site);
    } catch (const CoroutineClose &) {
        //------------------ a __close that closes its own coroutine must abort the whole thread, not be reported
        throw;
    } catch (const LRuntimeException &e) {
        LThread *t = L->running_thread;
        if (self_closing) {
            //------------------ while closing a coroutine the error becomes the close result (false, err)
            t->yield_args = MultiValue(e.error_obj);
            t->close_error = true;
        } else if (unwinding) {
            //------------------ stock replaces the pending error: the remaining closers and the catch site
            //------------------ that reports it see this one instead
            L->close_err = e.error_obj;
            L->has_close_err = true;
        } else {
            throw;
        }
    } catch (std::exception &e) {
        if (unwinding) {
            L->close_err = LValue(L->intern_string(e.what()));
            L->has_close_err = true;
        } else {
            throw;
        }
    } catch (...) {
        if (unwinding) {
            L->close_err = LValue(L->intern_string("<no error object>"));
            L->has_close_err = true;
        } else {
            throw;
        }
    }
}

//------------------ LState::register_module — register module loader
void LState::register_module(const std::string &name, LValue (*func)(LState *)) {
    register_static_preload(this, name.c_str(), func);
}

//------------------ LState::register_loaded_module — seed package.loaded[name] with an already-built module value
void LState::register_loaded_module(const std::string &name, const LValue &module) {
    LValue pack_val = get_global(this, "package");
    if (pack_val.type != ValueType::Table)
        return;
    LValue loaded = static_cast<LTable *>(pack_val.as_pointer())->gettable(LValue(intern_string("loaded")));
    if (loaded.type != ValueType::Table)
        return;
    static_cast<LTable *>(loaded.as_pointer())->settable(LValue(intern_string(name)), module);
}

//------------------ LState::create_table — allocate a table
LValue LState::create_table(size_t asize, size_t hsize) {
    gc_maybe_collect();

    LTable *t;
    //------------------ Size-segregated pool pop: a sized creation reuses a big-buffer table (same-shape
    //------------------ waves land head-first), while a size-unknown creation ({}) takes a small one — a
    //------------------ 90KB array must never park, counted, on a table that will never use it while its
    //------------------ rightful user allocates another (linear pool growth wave-over-wave).
    bool want_big = (asize > 0);
    LTable **pool = want_big ? &free_tables_big : &free_tables;
    if (!*pool)
        pool = want_big ? &free_tables : &free_tables_big;
    bool recycled_tbl = (*pool != nullptr);
    uint8_t prev_age_tbl = recycled_tbl ? (*pool)->age : AGE_YOUNG;
    if (*pool) {
        t = *pool;
        *pool = static_cast<LTable *>((*pool)->next);
        //------------------ a recycled table must not still pin a VM table it was copied from
        if (clx_table_release_ptr && t->ext && t->ext->vm_origin >= 0)
            clx_table_release_ptr(this, t);
        if (!want_big && t->array && t->array != t->small_array) {
            //------------------ small creation drew big (small list drained): free the array it will never
            //------------------ use instead of parking it. No accounting: dispose already subtracted it.
            table_free_array(t->array, t->array_types, t->small_array);
            t->array_cap = 0;
        }
        if (t->ext) {
            if (!t->ext->entries)
                t->ext->hash_size = 0;
            t->ext->hash_count = 0;
            t->ext->hash_tombs = 0;
            t->ext->hash_version++;
            t->ext->metatable = nullptr;
            if (t->ext->entries) {
                for (size_t i = 0; i < t->ext->hash_size; ++i) {
                    t->ext->entries[i].key.payload.u64 = HASH_EMPTY;
                    t->ext->entries[i].ktype = Nil;
                }
                if (t->ext->hash_bitmap)
                    std::memset(t->ext->hash_bitmap, 0, ((t->ext->hash_size + 63) / 64) * sizeof(uint64_t));
            }
        }
        allocated_bytes += sizeof(LTable);
    } else {
        t = slab_alloc_table();
        allocated_bytes += sizeof(LTable);
    }

    if (asize > 0) {
        if (asize <= 2) {

            table_free_array(t->array, t->array_types, t->small_array);
            t->array = t->small_array;
            t->array_types = t->small_array_types;
            t->array_size = asize;
            t->array_cap = asize;
            t->array_types[0] = ValueType::Nil;
            t->array_types[1] = ValueType::Nil;
        } else if (t->array_cap >= asize) {
            t->array_size = asize;
            std::fill(t->array_types, t->array_types + asize, ValueType::Nil);
            t->array_cap = asize;
        } else {
            table_free_array(t->array, t->array_types, t->small_array);
            table_alloc_array(t->array, t->array_types, asize);
            t->array_size = asize;
            t->array_cap = asize;
        }
    } else {
        t->array_size = 0;
    }
    t->array_lim = 0;

    allocated_bytes += table_heap_bytes(t);

    if (hsize > 0)
        t->presize_hash(hsize);

    t->type = static_cast<uint8_t>(Table);
    t->marked = 0;
    t->age = AGE_YOUNG;
    if (recycled_tbl && prev_age_tbl != AGE_YOUNG && (gc_phase == GCPhase::Sweeping || gc_minor_active)) {

        t->flags |= LFLAG_GC_PIN;
        gc_pinned.push(t);
    }

    t->next = allocated_objects;
    allocated_objects = t;
    gc_recent.push(t);

    object_count++;
    return LValue(Table, t);
}

//------------------ LState::create_closure — allocate a closure
clx::LValue clx::LState::create_closure(CFunctionType func, LTable *env, std::vector<LUpValue> gc_cells) {
    gc_maybe_collect();

    LCFunction *f;
    bool recycled_fn = (free_functions != nullptr);
    uint8_t prev_age_fn = recycled_fn ? free_functions->age : AGE_YOUNG;
    if (free_functions) {
        f = free_functions;
        free_functions = static_cast<LCFunction *>(free_functions->next);
        f->func = std::move(func);
        f->env = env ? env : _G;
        f->marked = 0;
        f->gc_cells = std::move(gc_cells);
    } else {
        f = new LCFunction(std::move(func));
        f->env = env ? env : _G;
        f->gc_cells = std::move(gc_cells);
        allocated_bytes += sizeof(LCFunction);
    }

    auto *fnptr = f->func.target<MultiValue (*)(LState *, const LValue *, size_t)>();
    f->direct = fnptr ? *fnptr : nullptr;
    f->self_ref = LValue();
    f->type = static_cast<uint8_t>(Function);
    f->marked = 0;
    f->age = AGE_YOUNG;
    if (recycled_fn && prev_age_fn != AGE_YOUNG && (gc_phase == GCPhase::Sweeping || gc_minor_active)) {

        f->flags |= LFLAG_GC_PIN;
        gc_pinned.push(f);
    }
    f->next = allocated_objects;
    allocated_objects = f;
    gc_recent.push(f);
    object_count++;
    return clx::LValue(Function, f);
}

//------------------ newuserdata — allocate userdata
LValue newuserdata(LState *L, size_t size) {
    L->gc_maybe_collect();

    char *mem = new char[sizeof(LUserdata) + size];
    LUserdata *ud = reinterpret_cast<LUserdata *>(mem);
    ud->type = static_cast<uint8_t>(UserData);
    ud->marked = 0;
    ud->flags = 0;
    ud->age = AGE_YOUNG;
    ud->metatable = nullptr;
    ud->meta_next = nullptr;
    ud->uservalue = LValue();
    ud->size = size;

    ud->next = L->allocated_objects;
    L->allocated_objects = ud;
    L->gc_recent.push(ud);
    L->object_count++;
    L->allocated_bytes += sizeof(LUserdata) + size;
    return LValue(UserData, ud);
}

//------------------ get_global — get global variable
LValue get_global(LState *L, const char *name) {
    LValue val = L->_G->gettable(LValue(L->intern_string(name)));
    return val.type != Nil ? val : LValue();
}

//------------------ set_global — set global variable
void set_global(LState *L, const char *name, const LValue &val) {
    LValue key = LValue(L->intern_string(name));
    gc_barrier_table(L, L->_G, val);
    if (key.is_gc_obj())
        gc_barrier_table(L, L->_G, key);
    L->_G->settable(key, val);
}

//------------------ open — create Lua state
extern thread_local LState *clx_current_L;
thread_local LState *clx_current_L = nullptr;

//------------------ open_bare — create Lua state without registering any std library. Generated
//------------------ executables use this plus a filtered registration sequence so the linker strips
//------------------ unreferenced base functions; open() below preserves the legacy all-registered behavior.
LState *open_bare(int argc, char *argv[]) {
    LState *L = new LState();
    clx_current_L = L;

    LThread *main_th = new LThread();
    main_th->state = L;
    main_th->is_main = true;
    main_th->status = THREAD_RUNNING;
#if defined(_WIN32)
    main_th->fiber = ConvertThreadToFiber(nullptr);
    main_th->stack_low = L->stack_low;
#elif (defined(__APPLE__) || defined(__linux__)) && (defined(__aarch64__) || defined(__x86_64__))
    clx_coro_save(&main_th->ctx);
#else
    getcontext(&main_th->ctx);
#endif
    L->main_thread = main_th;
    L->running_thread = main_th;

    if (argc > 0 && argv) {
        LValue arg_table = L->create_table(argc);
        LTable *t = static_cast<LTable *>(arg_table.as_pointer());
        for (int i = 0; i < argc; ++i) {
            t->settable(LValue(static_cast<int64_t>(i)), LValue(L->intern_string(argv[i])));
        }
        L->_G->settable(LValue(L->intern_string("arg")), arg_table);
    }

    return L;
}

//------------------ open — create Lua state with the full minimal std set (legacy behavior)
LState *open(int argc, char *argv[]) {
    LState *L = open_bare(argc, argv);
    luastd_base(L);
    luastd_package(L);
    return L;
}

//------------------ close — close Lua state
void close(LState *L) {
    if (clx_current_L == L)
        clx_current_L = nullptr;
#if defined(_WIN32)
    ConvertFiberToThread();
#endif
    delete L;
}

}
