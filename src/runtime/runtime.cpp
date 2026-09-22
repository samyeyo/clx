// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  runtime.cpp · Runtime core (GC, state,...) │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include "clx_runtime.h"
#include "vm/vm_convert.h"
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
void register_static_preload(LState* L, const char* name, LValue(open_func)(LState*));
}

namespace clx {

void (*clx_mark_vm_proxies_ptr)(LState* clx_L, LState::GCStack& wl) = nullptr;
void (*clx_free_vm_proxy_ptr)(LState* clx_L, LHeader* proxy) = nullptr;

//------------------ LThread::LThread — thread constructor
LThread::LThread()
    : state(nullptr)
    , status(THREAD_SUSPENDED)
    , caller(nullptr)
    , is_main(false)
    , has_error(false)
    , close_requested(false)
{
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
LThread::~LThread()
{
#if defined(_WIN32)
    if (fiber && !is_main)
        DeleteFiber(fiber);
#else
    if (stack_memory)
        delete[] stack_memory;
#endif
}

static void fiber_entry_impl(LThread* t)
{
    LState* L = t->state;
    for (;;) {
        try {
            clx::LValue args[8];
            size_t argc = t->resume_args.count < 8 ? t->resume_args.count : 8;
            for (size_t i = 0; i < argc; ++i)
                args[i] = t->resume_args[i];
            t->resume_args = MultiValue();

            t->yield_args = call_function_rooted(L, t->function, args, argc, "coroutine", 0);
            t->status = THREAD_DEAD;
        } catch (const LRuntimeException& e) {

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

    LThread* caller = t->caller;
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

static constexpr size_t kMaxPooledFibers = 2048;

static void WINAPI fiber_trampoline(LPVOID param)
{
    LThread* t = static_cast<LThread*>(param);
    t->fiber_started = true;
    for (;;) {
        fiber_entry_impl(t);
    }
}
#else
static thread_local LThread* g_starting_thread = nullptr;

static void fiber_trampoline()
{
    fiber_entry_impl(g_starting_thread);
}
#endif

//------------------ create_thread: creates a new coroutine thread (public API)
LValue create_thread(LState* L, const LValue& func, double stack_size)
{
    L->gc_maybe_collect();

    LThread* t = L->free_threads;
    bool recycled = (t != nullptr);
    uint8_t prev_age_th = recycled ? t->age : AGE_YOUNG;
#if defined(_WIN32)
    bool pooled_suspended = recycled && t->status == THREAD_SUSPENDED && t->fiber_started;
#endif
    if (t) {
        L->free_threads = static_cast<LThread*>(t->next);
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
    if (recycled && prev_age_th != AGE_YOUNG
        && (L->gc_phase == LState::GCPhase::Sweeping || L->gc_minor_active)) {

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
    clx_coro_init(&t->ctx, t->stack_memory + t->stack_bytes, (void*)fiber_trampoline);
    g_starting_thread = t;
#elif defined(__linux__) && defined(__x86_64__)
    if (!t->stack_memory)
        t->stack_memory = new char[t->stack_bytes];
    clx_coro_init(&t->ctx, t->stack_memory + t->stack_bytes, (void*)fiber_trampoline);
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
MultiValue resume(LState* L, const LValue& thread, const LValue* args, size_t count)
{
    LThread* t = static_cast<LThread*>(thread.as_pointer());
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
    LValue* buf;
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
MultiValue yield(LState* L, const LValue* args, size_t count)
{
    LThread* t = L->running_thread;
    if (t->is_main)
        clx::error(L, "attempt to yield from outside a coroutine");
    t->yield_args = MultiValue(args, count, L);
    if (L->gc_mode == LState::GCMode::Generational && t->age == AGE_OLD) {
        for (size_t i = 0; i < t->yield_args.count; ++i)
            gc_barrier_header(L, t, t->yield_args[i]);
    }
    t->status = THREAD_SUSPENDED;

    LThread* caller = t->caller;
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
MultiValue close_thread(LState* L, const LValue& thread)
{
    LThread* t = static_cast<LThread*>(thread.as_pointer());

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

//------------------ intern_error_message — interns an error message raised without an LState
const char* intern_error_message(const char* msg, size_t len)
{
    static std::mutex pool_mutex;
    static StringPool pool;
    uint64_t h = len <= 8 ? swar_hash_8(msg, len) : wyhash_str(msg, len);
    std::lock_guard<std::mutex> guard(pool_mutex);
    return pool.intern(msg, len, h);
}

//------------------ LRuntimeException::LRuntimeException — error exception constructor
LRuntimeException::LRuntimeException(clx::LValue err)
    : error_obj(err)
{
}

//------------------ LRuntimeException::~LRuntimeException — exception destructor
LRuntimeException::~LRuntimeException() noexcept { }

//------------------ LRuntimeException::what — get error message
const char* LRuntimeException::what() const noexcept
{
    if (cached_msg.empty()) {
        cached_msg = error_obj.to_string(nullptr);
    }
    return cached_msg.c_str();
}

//------------------ LValue::to_string — convert value to string
std::string LValue::to_string(LState* L) const
{
    if (L && (type == Table || type == UserData)) {
        LTable* mt = (type == Table) ? tbl_metatable(static_cast<LTable*>(as_pointer()))
                                     : static_cast<LUserdata*>(as_pointer())->metatable;
        if (mt) {
            LValue meta_key = L->str_tostring;
            LValue meta_func = mt->gettable(meta_key);

            if (meta_func.type != Nil && meta_func.type == Function) {
                LValue args[1];
                args[0] = *this;

                MultiValue ret = call_function_rooted(L, meta_func, args, 1, __FILE__, __LINE__);

                if (ret.count > 0 && ret[0].type == String) {
                    return std::string(ret[0].as_string(), ret[0].string_len());
                }
            }
        }
    }

    switch (type) {
    case Nil:
        return "nil";
    case Boolean:
        return as_bool() ? "true" : "false";
    case Double: {
        char buf[64];
        int n = clx_format_double(buf, sizeof(buf), as_number());
        return std::string(buf, (size_t)n);
    }
    case Int64:
        return std::to_string(as_integer());
    case String:
        return std::string(as_string(), string_len());
    case Table: {
        const char* prefix = "table";
        if (L) {
            LTable* mt = tbl_metatable(static_cast<LTable*>(as_pointer()));
            if (mt) {
                LValue n = mt->gettable(LValue(L->intern_string("__name")));
                if (n.type != Nil && n.type == String)
                    prefix = n.as_string();
            }
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s: %p", prefix, as_pointer());
        return std::string(buf);
    }
    case Function: {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "function: %p", as_pointer());
        return std::string(buf);
    }
    default:
        return "userdata";
    }
}

//------------------ LValue::slow_eq — equality comparison
LValue LValue::slow_eq(const LValue& other) const
{
    if (type != other.type)
        return LValue(false);
    if (type == String) {
        return LValue(
            string_len() == other.string_len() && clx_memcmp(as_string(), other.as_string(), string_len()) == 0);
    }
    return LValue(false);
}

//------------------ LValue::slow_lt — less-than comparison
LValue LValue::slow_lt(const LValue& other) const
{
    if ((type == Double || type == Int64) && (other.type == Double || other.type == Int64)) {
        double l = (type == Int64) ? (double)as_integer() : as_number();
        double r = (other.type == Int64) ? (double)other.as_integer() : other.as_number();
        return LValue(l < r);
    }
    if (type == String && other.type == String) {
        size_t al = string_len(), bl = other.string_len();
        int cmp = clx_memcmp(as_string(), other.as_string(), std::min(al, bl));
        if (cmp != 0)
            return LValue(cmp < 0);
        return LValue(al < bl);
    }
    return LValue(false);
}

//------------------ LValue::slow_le — less-or-equal comparison
LValue LValue::slow_le(const LValue& other) const
{
    if ((type == Double || type == Int64) && (other.type == Double || other.type == Int64)) {
        double l = (type == Int64) ? (double)as_integer() : as_number();
        double r = (other.type == Int64) ? (double)other.as_integer() : other.as_number();
        return LValue(l <= r);
    }
    if (type == String && other.type == String) {
        size_t al = string_len(), bl = other.string_len();
        int cmp = clx_memcmp(as_string(), other.as_string(), std::min(al, bl));
        if (cmp != 0)
            return LValue(cmp <= 0 ? cmp < 0 : false);
        return LValue(al <= bl);
    }
    return LValue(false);
}

//------------------ LCFunction::LCFunction — C function wrapper constructor
LCFunction::LCFunction(CFunctionType f)
    : func(std::move(f))
{
    type = static_cast<uint8_t>(Function);
    marked = 0;
    next = nullptr;
}

static CLX_INLINE_COLD size_t next_pow2(size_t n)
{
    if (n < 8)
        return 8;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

//--------- Slow Path (hash table lookup and metamethods)
LValue table_get_slow(LState* L, const LValue& obj, const LValue& key)
{
    LTable* mt = nullptr;
    LValue direct;

    if (obj.type == ValueType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        if (key.type == ValueType::Int64) {
            int64_t idx = key.val.payload.i64;
            if (static_cast<uint64_t>(idx - 1) < t->array_cap) {
                direct = LValue(t->array[idx - 1], t->array_types[idx - 1]);
            }
        } else if (key.type == ValueType::Double) {
            double d = key.val.payload.f64;
            int64_t idx = static_cast<int64_t>(d);
            if (d == static_cast<double>(idx) && static_cast<uint64_t>(idx - 1) < t->array_cap) {
                direct = LValue(t->array[idx - 1], t->array_types[idx - 1]);
            }
        }
        if (direct.type == ValueType::Nil) {
            LTableExt* ex = t->ext;
            if (ex && ex->ic) {
                uint32_t ic_idx
                    = static_cast<uint32_t>(key.val.payload.u64 ^ (key.val.payload.u64 >> 17)
                          ^ (key.val.payload.u64 >> 33) ^ (key.val.payload.u64 >> 5) ^ (key.val.payload.u64 >> 11))
                    % LTABLE_IC_SIZE;
                LTableInlineCache& _ic = ex->ic[ic_idx];
                if (_ic.key_payload == key.val.payload.u64 && _ic.table_ver == ex->hash_version
                    && _ic.entry_idx < ex->hash_size) {
                    HashEntry& _e = ex->entries[_ic.entry_idx];
                    if (_e.ktype != ValueType::Nil)
                        return LValue(_e.val, _e.vtype);
                }
            }
            direct = t->get_value(L, key);
        }
        if (direct.type != ValueType::Nil)
            return direct;
        mt = tbl_metatable(t);
    } else if (obj.type == ValueType::UserData) {
        LUserdata* ud = static_cast<LUserdata*>(obj.as_pointer());
        mt = ud->metatable;
    } else if (obj.type == ValueType::String) {
        mt = L->string_metatable;
        if (!mt)
            return LValue();
        LValue index = mt->gettable(LValue(L->intern_string("__index")));
        if (index.type == ValueType::Nil)
            return LValue();
        if (index.type == ValueType::Table)
            return table_get(L, index, key);
        if (index.type == ValueType::Function) {
            LValue args[2] = { obj, key };
            MultiValue mv = call_function_rooted(L, index, args, 2, "__index", 0);
            return mv.count > 0 ? mv[0] : LValue();
        }
        return LValue();
    } else {
        throw_index_error(L, obj);
    }

    if (!mt)
        return LValue();
    LValue index = mt->gettable(LValue(L->intern_string("__index")));
    if (index.type == ValueType::Nil)
        return LValue();
    if (index.type == ValueType::Function) {
        LValue args[2] = { obj, key };
        MultiValue mv = call_function_rooted(L, index, args, 2, "__index", 0);
        return mv.count > 0 ? mv[0] : LValue();
    }
    if (index.type == ValueType::Table)
        return table_get(L, index, key);
    return LValue();
}

//------------------ LTable::LTable — table constructor
LTable::LTable()
    : array(nullptr)
    , array_types(nullptr)
    , array_size(0)
    , array_cap(0)
    , ext(nullptr)
{
    type = static_cast<uint8_t>(Table);
    marked = 0;
    next = nullptr;
}

//------------------ LTable::~LTable — table destructor
LTable::~LTable()
{
    if (ext) {
        if (ext->ic)
            delete[] ext->ic;
        if (!(flags & LFLAG_ARENA)) {
            if (ext->entries)
                delete[] ext->entries;
            if (ext->hash_bitmap)
                delete[] ext->hash_bitmap;
        }
        delete ext;
        ext = nullptr;
    }
    if (!(flags & LFLAG_ARENA)) {
        if (array && array != small_array)
            delete[] array;
        if (array_types && array_types != small_array_types)
            delete[] array_types;
    }
}

//------------------ LTable::resize_hash — allocate/rehash to new_size (power of 2)
void LTable::resize_hash(size_t new_size)
{
    new_size = next_pow2(new_size);

    LTableExt* ex = tbl_ensure_ext(this);

    HashEntry* new_entries = new HashEntry[new_size];
    for (size_t i = 0; i < new_size; ++i) {
        new_entries[i].key.payload.u64 = HASH_EMPTY;
        new_entries[i].ktype = Nil;
    }

    size_t bm_words = (new_size + 63) / 64;
    uint64_t* new_bitmap = new uint64_t[bm_words]();

    uint32_t mask = static_cast<uint32_t>(new_size - 1);
    if (ex->entries) {
        for (size_t i = 0; i < ex->hash_size; ++i) {
            if (ex->entries[i].ktype == Nil || ex->entries[i].vtype == Nil)
                continue;
            uint64_t h = lvalue_hash(LValue(ex->entries[i].key, ex->entries[i].ktype)) & mask;
            while (new_entries[h].ktype != Nil)
                h = (h + 1) & mask;
            new_entries[h].key = ex->entries[i].key;
            new_entries[h].ktype = ex->entries[i].ktype;
            new_entries[h].val = ex->entries[i].val;
            new_entries[h].vtype = ex->entries[i].vtype;
            new_bitmap[h / 64] |= (1ULL << (h % 64));
        }
        if (!(flags & LFLAG_ARENA))
            delete[] ex->entries;
    }

    flags &= ~LFLAG_ARENA;
    ex->entries = new_entries;
    ex->hash_size = new_size;
    ex->hash_tombs = 0;
    if (ex->hash_bitmap)
        delete[] ex->hash_bitmap;
    ex->hash_bitmap = new_bitmap;
    ex->hash_version++;
}

//------------------ LTable::gettable — get value by key
LValue LTable::gettable(const LValue& key)
{
    if (key.type == Int64) {
        int64_t idx = key.as_integer();
        if (static_cast<uint64_t>(idx - 1) < array_cap)
            return LValue(array[idx - 1], array_types[idx - 1]);
    } else if (key.type == Double) {
        double d = key.as_number();
        int64_t idx = static_cast<int64_t>(d);
        if (d == static_cast<double>(idx) && static_cast<uint64_t>(idx - 1) < array_cap)
            return LValue(array[idx - 1], array_types[idx - 1]);
    }
    LTableExt* ex = ext;
    if (!ex || ex->hash_size == 0)
        return LValue();

    if (!ex->ic)
        ex->ic = new LTableInlineCache[LTABLE_IC_SIZE]();

    uint32_t ic_idx = static_cast<uint32_t>(key.val.payload.u64 ^ (key.val.payload.u64 >> 17)
                          ^ (key.val.payload.u64 >> 33) ^ (key.val.payload.u64 >> 5) ^ (key.val.payload.u64 >> 11))
        % LTABLE_IC_SIZE;
    auto& _ic = ex->ic[ic_idx];
    if (_ic.key_payload == key.val.payload.u64 && _ic.table_ver == ex->hash_version && _ic.entry_idx < ex->hash_size) {
        HashEntry& _e = ex->entries[_ic.entry_idx];
        if (_e.ktype != Nil)
            return LValue(_e.val, _e.vtype);
    }

    uint32_t mask = static_cast<uint32_t>(ex->hash_size - 1);
    uint64_t h = lvalue_hash(key) & mask;
    for (;;) {
        HashEntry& e = ex->entries[h];
        if (e.ktype == Nil) {
            if (e.key.payload.u64 == HASH_EMPTY)
                return LValue();
        } else if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) {
            _ic.key_payload = key.val.payload.u64;
            _ic.entry_idx = static_cast<uint32_t>(h);
            _ic.table_ver = ex->hash_version;
            return LValue(e.val, e.vtype);
        }
        h = (h + 1) & mask;
    }
}

//------------------ LTable::settable — set value by key
void LTable::settable(const LValue& key, const LValue& val)
{
    // Generational barrier for direct users (generated _G stores, runtime
    // init): table_set/table_set_direct/table_set_int barrier themselves,
    // but this member cannot rely on callers, so it barriers via the
    // thread-local state. Null during LState construction (all young).
    if (clx_current_L)
        gc_barrier_table(clx_current_L, this, val);

    if (key.type == Int64) {
        int64_t idx = key.as_integer();
        if (static_cast<uint64_t>(idx - 1) < array_cap) {
            array[idx - 1] = val.val;
            array_types[idx - 1] = val.type;
            if (static_cast<size_t>(idx) > array_size)
                array_size = static_cast<size_t>(idx);
            return;
        }
        if (idx == static_cast<int64_t>(array_size + 1)) {
            size_t new_cap = (array_cap == 0) ? 8 : array_cap * 2;
            TValue* new_arr = new TValue[new_cap];
            ValueType* new_types = new ValueType[new_cap]();
            if (array_cap) {
                std::memcpy(new_arr, array, array_cap * sizeof(TValue));
                std::memcpy(new_types, array_types, array_cap * sizeof(ValueType));
            }
            if (!(flags & LFLAG_ARENA)) {
                if (array != small_array)
                    delete[] array;
                if (array_types != small_array_types)
                    delete[] array_types;
            }
            flags &= ~LFLAG_ARENA;
            array = new_arr;
            array_types = new_types;
            array[array_size] = val.val;
            array_types[array_size] = val.type;
            array_size++;
            array_cap = new_cap;
            if (ext && ext->hash_size > 0) {
                for (size_t i = 0; i < ext->hash_size; ++i) {
                    if (ext->entries[i].ktype == Nil || ext->entries[i].vtype == Nil)
                        continue;
                    LValue kv(ext->entries[i].key, ext->entries[i].ktype);
                    int64_t hidx = -1;
                    if (kv.type == Int64)
                        hidx = kv.as_integer();
                    else if (kv.type == Double) {
                        double d = kv.as_number();
                        int64_t t = static_cast<int64_t>(d);
                        if (d == static_cast<double>(t))
                            hidx = t;
                    }
                    if (hidx > 0 && hidx != idx && static_cast<uint64_t>(hidx - 1) < new_cap) {
                        array[hidx - 1] = ext->entries[i].val;
                        array_types[hidx - 1] = ext->entries[i].vtype;
                        ext->entries[i].key.payload.u64 = HASH_TOMBSTONE;
                        ext->entries[i].ktype = Nil;
                        ext->entries[i].val = TValue();
                        ext->entries[i].vtype = Nil;
                        ext->hash_count--;
                        ext->hash_tombs++;
                        ext->hash_version++;
                        if (ext->hash_bitmap)
                            ext->hash_bitmap[i / 64] &= ~(1ULL << (i % 64));
                    }
                }
            }
            return;
        }
    } else if (key.type == Double) {
        double d = key.as_number();
        int64_t idx = static_cast<int64_t>(d);
        if (d == static_cast<double>(idx)) {
            if (static_cast<uint64_t>(idx - 1) < array_cap) {
                array[idx - 1] = val.val;
                array_types[idx - 1] = val.type;
                if (static_cast<size_t>(idx) > array_size)
                    array_size = static_cast<size_t>(idx);
                return;
            }
            if (idx == static_cast<int64_t>(array_size + 1)) {
                size_t new_cap = (array_cap == 0) ? 8 : array_cap * 2;
                TValue* new_arr = new TValue[new_cap];
                ValueType* new_types = new ValueType[new_cap]();
                if (array_cap) {
                    std::memcpy(new_arr, array, array_cap * sizeof(TValue));
                    std::memcpy(new_types, array_types, array_cap * sizeof(ValueType));
                }
                if (!(flags & LFLAG_ARENA)) {
                    if (array != small_array)
                        delete[] array;
                    if (array_types != small_array_types)
                        delete[] array_types;
                }
                flags &= ~LFLAG_ARENA;
                array = new_arr;
                array_types = new_types;
                array[array_size] = val.val;
                array_types[array_size] = val.type;
                array_size++;
                array_cap = new_cap;
                if (ext && ext->hash_size > 0) {
                    for (size_t i = 0; i < ext->hash_size; ++i) {
                        if (ext->entries[i].ktype == Nil || ext->entries[i].vtype == Nil)
                            continue;
                        LValue kv(ext->entries[i].key, ext->entries[i].ktype);
                        int64_t hidx = -1;
                        if (kv.type == Int64)
                            hidx = kv.as_integer();
                        else if (kv.type == Double) {
                            double dd = kv.as_number();
                            int64_t t = static_cast<int64_t>(dd);
                            if (dd == static_cast<double>(t))
                                hidx = t;
                        }
                        if (hidx > 0 && hidx != idx && static_cast<uint64_t>(hidx - 1) < new_cap) {
                            array[hidx - 1] = ext->entries[i].val;
                            array_types[hidx - 1] = ext->entries[i].vtype;
                            ext->entries[i].key.payload.u64 = HASH_TOMBSTONE;
                            ext->entries[i].ktype = Nil;
                            ext->entries[i].val = TValue();
                            ext->entries[i].vtype = Nil;
                            ext->hash_count--;
                            ext->hash_tombs++;
                            ext->hash_version++;
                            if (ext->hash_bitmap)
                                ext->hash_bitmap[i / 64] &= ~(1ULL << (i % 64));
                        }
                    }
                }
                return;
            }
        }
    }

    if (val.type == Nil) {
        LTableExt* ex = ext;
        if (!ex || ex->hash_size == 0)
            return;
        uint32_t mask = static_cast<uint32_t>(ex->hash_size - 1);
        uint64_t h = lvalue_hash(key) & mask;
        for (;;) {
            HashEntry& e = ex->entries[h];
            if (e.ktype == Nil) {
                if (e.key.payload.u64 == HASH_EMPTY)
                    return;
            } else if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) {
                if (e.vtype == Nil)
                    return;
                e.val = TValue();
                e.vtype = Nil;
                ex->hash_count--;
                ex->hash_tombs++;
                ex->hash_version++;
                return;
            }
            h = (h + 1) & mask;
        }
    }

    LTableExt* ex = ext;
    if (!ex || ex->hash_size == 0) {
        resize_hash(8);
        ex = ext;
    } else if ((ex->hash_count + ex->hash_tombs + 1) * 4 >= ex->hash_size * 3) {
        resize_hash(ex->hash_count >= ex->hash_size / 2 ? ex->hash_size * 2 : ex->hash_size);
        ex = ext;
    }

    if (ex->ic) {
        uint32_t ic_idx = static_cast<uint32_t>(key.val.payload.u64 ^ (key.val.payload.u64 >> 17)
                              ^ (key.val.payload.u64 >> 33) ^ (key.val.payload.u64 >> 5) ^ (key.val.payload.u64 >> 11))
            % LTABLE_IC_SIZE;
        LTableInlineCache& _ic = ex->ic[ic_idx];
        if (_ic.key_payload == key.val.payload.u64 && _ic.table_ver == ex->hash_version
            && _ic.entry_idx < ex->hash_size) {
            HashEntry& _e = ex->entries[_ic.entry_idx];
            if (_e.ktype != Nil && _e.vtype != Nil) {
                _e.val = val.val;
                _e.vtype = val.type;
                return;
            }
        }
    }

    uint32_t mask = static_cast<uint32_t>(ex->hash_size - 1);
    uint64_t h = lvalue_hash(key) & mask;
    int32_t tomb = -1;
    for (;;) {
        HashEntry& e = ex->entries[h];
        if (e.ktype == Nil) {
            HashEntry& slot = (tomb != -1) ? ex->entries[tomb] : e;
            slot.key = key.val;
            slot.ktype = key.type;
            slot.val = val.val;
            slot.vtype = val.type;
            if (tomb != -1)
                ex->hash_tombs--;
            ex->hash_count++;
            ex->hash_version++;
            if (ex->hash_bitmap) {
                size_t bit_idx = (tomb != -1) ? static_cast<size_t>(tomb) : h;
                ex->hash_bitmap[bit_idx / 64] |= (1ULL << (bit_idx % 64));
            }
            return;
        }
        if (e.key.payload.u64 == HASH_TOMBSTONE && e.ktype == Nil) {
            if (tomb == -1)
                tomb = static_cast<int32_t>(h);
        } else if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) {
            if (e.vtype == Nil) {
                ex->hash_count++;
                ex->hash_tombs--;
                if (ex->hash_bitmap)
                    ex->hash_bitmap[h / 64] |= (1ULL << (h % 64));
            }
            e.val = val.val;
            e.vtype = val.type;
            return;
        }
        h = (h + 1) & mask;
    }
}

//------------------ LTable::get_value — get with metamethod fallback
LValue LTable::get_value(LState* L, const LValue& key)
{
    LValue ptr = gettable(key);
    if (ptr.type != Nil)
        return ptr;

    if (ext && ext->metatable) {
        LValue index_key = L->str_index;
        LValue index_ptr = ext->metatable->gettable(index_key);

        if (index_ptr.type != Nil) {
            if (index_ptr.type == Table) {
                LTable* parent = static_cast<LTable*>(index_ptr.as_pointer());
                return parent->get_value(L, key);
            } else if (index_ptr.type == Function) {
                LValue args[2];
                args[0] = LValue(this);
                args[1] = key;

                MultiValue ret = call_function_rooted(L, index_ptr, args, 2, __FILE__, __LINE__);
                return ret.count > 0 ? ret[0] : LValue();
            }
        }
    }
    return LValue();
}

//------------------ LTable::set_value — set with metamethod fallback
void LTable::set_value(LState* L, const LValue& key, const LValue& val)
{
    LValue ptr = gettable(key);
    if (ptr.type != Nil) {
        settable(key, val);
        return;
    }

    if (ext && ext->metatable) {
        LValue newindex_key = L->str_newindex;
        LValue newindex_ptr = ext->metatable->gettable(newindex_key);

        if (newindex_ptr.type != Nil) {
            if (newindex_ptr.type == Table) {
                LTable* parent = static_cast<LTable*>(newindex_ptr.as_pointer());
                parent->set_value(L, key, val);
                return;
            } else if (newindex_ptr.type == Function) {
                LValue args[3];
                args[0] = LValue(this);
                args[1] = key;
                args[2] = val;

                call_function_rooted(L, newindex_ptr, args, 3, __FILE__, __LINE__);
                return;
            }
        }
    }
    settable(key, val);
}

//------------------ LTable::bind — bind constant value
void LTable::bind(const char* name, const LValue& val)
{
    settable(LValue(name), val);
}

//------------------ LTable::bind — bind C function
void LTable::bind(LState* L, const char* name, CFunctionType func)
{
    LCFunction* f = new LCFunction(func);
    f->next = L->allocated_objects;
    L->allocated_objects = f;
    L->gc_recent.push(f);
    settable(LValue(L->intern_string(name)), LValue(Function, f));
}

//------------------ LTable::bind_all — bind multiple C functions
void LTable::bind_all(LState* L, std::initializer_list<LReg> funcs)
{
    for (const auto& reg : funcs)
        bind(L, reg.name, reg.func);
}

//------------------ LState::LState — state constructor
LState::LState()
    : allocated_objects(nullptr)
    , free_tables(nullptr)
    , free_functions(nullptr)
    , shadow_top(0)
    , current_file("")
    , current_line(0)
    , object_count(0)
    , gc_bytes_threshold(2 * 1024 * 1024)
    , string_metatable(nullptr)
{
    _G = slab_alloc_table();
    allocated_bytes += sizeof(LTable);
    _G->next = allocated_objects;
    allocated_objects = _G;
    object_count++;
    _G->settable(LValue(intern_string("_G")), LValue(Table, _G));

    str_index = LValue(intern_string("__index"));
    str_newindex = LValue(intern_string("__newindex"));
    str_gc = LValue(intern_string("__gc"));
    str_call = LValue(intern_string("__call"));
    str_close = LValue(intern_string("__close"));
    str_pairs = LValue(intern_string("__pairs"));
    str_tostring = LValue(intern_string("__tostring"));

    //------------------ GC work-vector pre-sizing (callgrind: vector growth is

    gc_worklist.reserve(1 << 18);
    gc_recent.reserve(1 << 18);
    gc_remembered.reserve(1 << 16);
    gc_pinned.reserve(1 << 16);

    //------------------ GC mode / tuning knobs (environment override)

    if (const char* e = getenv("CLX_GC_MODE")) {
        if (strcmp(e, "incremental") == 0 || strcmp(e, "incr") == 0) {
            gc_mode = GCMode::Incremental;
        }
    }
    if (const char* e = getenv("CLX_GC_MINOR_KB")) {
        long long v = atoll(e);
        if (v > 0)
            gc_minor_threshold = size_t(v) * 1024;
    }
    if (const char* e = getenv("CLX_GC_MAJOR_KB")) {
        long long v = atoll(e);
        if (v > 0)
            gc_major_threshold = size_t(v) * 1024;
    }
    if (const char* e = getenv("CLX_GC_HEADROOM")) {
        long long v = atoll(e);
        if (v > 0)
            gc_headroom_override = size_t(v);
    }
}

static void dtor_free_table(LTable* t)
{
    if (t->array && t->array != t->small_array) {
        delete[] t->array;
        t->array = nullptr;
    }
    if (t->array_types && t->array_types != t->small_array_types) {
        delete[] t->array_types;
        t->array_types = nullptr;
    }
    if (t->ext) {
        if (t->ext->entries) {
            delete[] t->ext->entries;
            t->ext->entries = nullptr;
        }
        if (t->ext->hash_bitmap) {
            delete[] t->ext->hash_bitmap;
            t->ext->hash_bitmap = nullptr;
        }
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
LTable* LState::slab_alloc_table()
{
    static_assert(alignof(LTable) <= 16, "slab bump assumes 16-byte-aligned table slots");
    if (slab_current + sizeof(LTable) > slab_end) {
        constexpr size_t kAlign = 16;

        size_t slab_bytes = sizeof(TableSlab) + kAlign + SLAB_TABLE_COUNT * sizeof(LTable);
        char* mem = static_cast<char*>(std::malloc(slab_bytes));
        if (!mem)
            throw std::bad_alloc();
        TableSlab* slab = reinterpret_cast<TableSlab*>(mem);
        slab->next = slab_blocks;
        slab_blocks = slab;
        uintptr_t data = reinterpret_cast<uintptr_t>(mem) + sizeof(TableSlab);
        data = (data + (kAlign - 1)) & ~static_cast<uintptr_t>(kAlign - 1);
        slab_current = reinterpret_cast<char*>(data);
        slab_end = mem + slab_bytes;
    }
    LTable* t = new (slab_current) LTable();
    slab_current += sizeof(LTable);
    t->flags |= LFLAG_SLAB;
    return t;
}

//------------------ LState::slab_release — free all table slabs (state close only)
void LState::slab_release()
{
    TableSlab* s = slab_blocks;
    while (s) {
        TableSlab* n = s->next;
        std::free(s);
        s = n;
    }
    slab_blocks = nullptr;
    slab_current = slab_end = nullptr;
}

//------------------ LState::invoke_gc_finalizer — call __gc metamethod on userdata
void LState::invoke_gc_finalizer(LUserdata* ud, const char* tag)
{
    if (!ud->metatable)
        return;
    LValue gc_func = ud->metatable->gettable(this->str_gc);
    if (gc_func.type != Function)
        return;
    LValue args[1] = { LValue(UserData, ud) };
    size_t prev_shadow = this->shadow_top;
    this->shadow_stack[this->shadow_top++] = TypedSlot(&args[0].val, &args[0].type);
    try {
        call_function(this, gc_func, args, 1, tag, 0);
    } catch (const LRuntimeException& e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (std::exception& e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "error in __gc metamethod\n";
    }
    this->shadow_top = prev_shadow;
}

//------------------ LState::~LState — state destructor
LState::~LState()
{
    shadow_stack.reset();
    if (gc_phase == GCPhase::Sweeping) {
        while (gc_phase == GCPhase::Sweeping)
            gc_step();
    }

    for (LHeader* h = allocated_objects; h; h = h->next) {
        if (h->type == static_cast<uint8_t>(UserData)) {
            LUserdata* ud = static_cast<LUserdata*>(h);
            invoke_gc_finalizer(ud, "StateClose_Finalizer");
        }
    }

    LHeader* curr = allocated_objects;
    while (curr) {
        LHeader* next = curr->next;
        if (curr->flags & LFLAG_VM_PROXY) {
            if (clx_free_vm_proxy_ptr)
                clx_free_vm_proxy_ptr(this, curr);
        } else if (curr->type == static_cast<uint8_t>(Table))
            dtor_free_table(static_cast<LTable*>(curr));
        else if (curr->type == static_cast<uint8_t>(Function))
            delete static_cast<LCFunction*>(curr);
        else if (curr->type == static_cast<uint8_t>(UserData))
            delete[] reinterpret_cast<char*>(curr);
        else if (curr->type == static_cast<uint8_t>(Thread))
            delete static_cast<LThread*>(curr);
        curr = next;
    }
    allocated_objects = nullptr;

    {
        LTable* t = static_cast<LTable*>(gc_finalizable);
        while (t) {
            LTable* nxt = static_cast<LTable*>(t->next);
            dtor_free_table(t);
            t = nxt;
        }
    }
    gc_finalizable = nullptr;

    {
        LUserdata* ud = static_cast<LUserdata*>(gc_finalizable_ud);
        while (ud) {
            LUserdata* nxt = static_cast<LUserdata*>(ud->next);
            invoke_gc_finalizer(ud, "GC_Finalizer");
            delete[] reinterpret_cast<char*>(ud);
            ud = nxt;
        }
    }
    gc_finalizable_ud = nullptr;

    LTable* ft = free_tables;
    while (ft) {
        LTable* next = static_cast<LTable*>(ft->next);
        dtor_free_table(ft);
        ft = next;
    }
    free_tables = nullptr;

    slab_release();

    LCFunction* ff = free_functions;
    while (ff) {
        LCFunction* next = static_cast<LCFunction*>(ff->next);
        delete ff;
        ff = next;
    }
    free_functions = nullptr;

    LThread* fth = free_threads;
    while (fth) {
        LThread* nxt0 = static_cast<LThread*>(fth->next);
        delete fth;
        fth = nxt0;
    }
    free_threads = nullptr;
    std::free(overflow_heap);
    gc_worklist.free();
    gc_recent.free();
    gc_pinned.free();
}

static void clx_trigger_gc(LState* L, LTable* t)
{
    LTable* mt = tbl_metatable(t);
    if (!mt)
        return;
    LValue gc_func = mt->gettable(L->str_gc);
    if (gc_func.type == Nil)
        return;

    LValue args[1] = { LValue(Table, t) };
    try {
        call_function_rooted(L, gc_func, args, 1, "GC_Finalizer", 0);
    } catch (const LRuntimeException& e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (std::exception& e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "error in __gc metamethod\n";
    }
}

//------------------ CloseGuard::~CloseGuard — close guard destructor
CloseGuard::~CloseGuard()
{
    if (val.type != Table)
        return;
    LTable* t = static_cast<LTable*>(val.as_pointer());
    LTable* mt = tbl_metatable(t);
    if (!mt)
        return;
    LValue close_func = mt->gettable(L->str_close);
    if (close_func.type == Nil)
        return;

    LValue args[2] = { val, LValue() };
    try {
        call_function_rooted(L, close_func, args, 2, "CloseGuard", 0);
    } catch (const LRuntimeException& e) {
        std::cerr << "error in __close metamethod: " << e.what() << "\n";
    } catch (std::exception& e) {
        std::cerr << "error in __close metamethod: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "error in __close metamethod\n";
    }
}

#define GC_SUB(TAG, PTR, amt)                           \
    do {                                                \
        if (static_cast<size_t>(amt) > allocated_bytes) \
            allocated_bytes = 0;                        \
        else                                            \
            allocated_bytes -= (amt);                   \
    } while (0)

//------------------ shared sweep helpers — used by both the incremental major sweep (gc_step)

static void gc_dispose_swept(LState* L, LHeader* curr)
{
    auto& GC_SUB = L->allocated_bytes;
    (void)GC_SUB;
    curr->flags &= ~(LFLAG_REMEMBERED | LFLAG_GC_PIN);
    // Leaving the OLD generation (freed or parked): drop its old-set
    // accounting, otherwise gc_old_bytes only grows and every trigger runs
    // a full collection once it crosses the major threshold.
    L->gc_update_old_bytes(curr, curr->age, AGE_YOUNG);
    if (curr->flags & LFLAG_VM_PROXY) {
        if (clx_free_vm_proxy_ptr)
            clx_free_vm_proxy_ptr(L, curr);
    } else if (curr->type == static_cast<uint8_t>(Table)) {
        LTable* t = static_cast<LTable*>(curr);
        if (tbl_metatable(t)) {
            t->next = L->gc_finalizable;
            L->gc_finalizable = t;
        } else {
            if (static_cast<size_t>(sizeof(LTable)) > L->allocated_bytes)
                L->allocated_bytes = 0;
            else
                L->allocated_bytes -= sizeof(LTable);
            if (t->ext) {
                t->ext->hash_count = 0;
                t->ext->hash_tombs = 0;
            }
            t->array_size = 0;
            t->next = L->free_tables;
            L->free_tables = t;
        }
    } else if (curr->type == static_cast<uint8_t>(Function)) {
        LCFunction* f = static_cast<LCFunction*>(curr);
        f->func = nullptr;
        f->gc_cells.clear();
        f->next = L->free_functions;
        L->free_functions = f;
    } else if (curr->type == static_cast<uint8_t>(Thread)) {
        LThread* th = static_cast<LThread*>(curr);
        size_t th_bytes = sizeof(LThread) + th->stack_bytes;
        if (th_bytes > L->allocated_bytes)
            L->allocated_bytes = 0;
        else
            L->allocated_bytes -= th_bytes;
#if defined(_WIN32)
        if (th->fiber && L->free_fiber_threads < LState::kMaxPooledFibers) {
            L->free_fiber_threads++;
        } else {
            if (th->fiber)
                DeleteFiber(th->fiber);
            th->fiber = nullptr;
        }
#endif
        th->next = L->free_threads;
        L->free_threads = th;
    } else if (curr->type == static_cast<uint8_t>(UserData)) {
        LUserdata* ud = static_cast<LUserdata*>(curr);
        if (ud->metatable) {
            ud->next = L->gc_finalizable_ud;
            L->gc_finalizable_ud = ud;
        } else {
            size_t ud_bytes = sizeof(LUserdata) + ud->size;
            if (ud_bytes > L->allocated_bytes)
                L->allocated_bytes = 0;
            else
                L->allocated_bytes -= ud_bytes;
            delete[] reinterpret_cast<char*>(ud);
        }
    }
}

static void gc_drain_finalizables(LState* L)
{
    for (LTable* t = static_cast<LTable*>(L->gc_finalizable); t;) {
        LTable* nx = static_cast<LTable*>(t->next);
        meta_list_remove(L, t);
        clx_trigger_gc(L, t);
        size_t t_bytes = sizeof(LTable);
        if (t_bytes > L->allocated_bytes)
            L->allocated_bytes = 0;
        else
            L->allocated_bytes -= t_bytes;
        if (t->array && t->array != t->small_array) {
            delete[] t->array;
            t->array = nullptr;
        }
        if (t->array_types && t->array_types != t->small_array_types) {
            delete[] t->array_types;
            t->array_types = nullptr;
        }
        if (t->ext) {
            if (t->ext->entries) {
                delete[] t->ext->entries;
                t->ext->entries = nullptr;
            }
            t->ext->hash_count = 0;
            t->ext->hash_tombs = 0;
            t->ext->hash_size = 0;
            t->ext->metatable = nullptr;
            t->ext->meta_next = nullptr;
        }
        t->array_size = t->array_cap = 0;
        t->next = L->free_tables;
        L->free_tables = t;
        t = nx;
    }
    L->gc_finalizable = nullptr;
    for (LUserdata* ud = static_cast<LUserdata*>(L->gc_finalizable_ud); ud;) {
        LUserdata* nx = static_cast<LUserdata*>(ud->next);
        L->invoke_gc_finalizer(ud, "GC_Finalizer");
        size_t ud_bytes = sizeof(LUserdata) + ud->size;
        if (ud_bytes > L->allocated_bytes)
            L->allocated_bytes = 0;
        else
            L->allocated_bytes -= ud_bytes;
        delete[] reinterpret_cast<char*>(ud);
        ud = nx;
    }
    L->gc_finalizable_ud = nullptr;
}

//------------------ LState::gc_maybe_collect — shared allocation-site trigger
void LState::gc_maybe_collect()
{
    if (!gc_running)
        return;
    if (gc_phase == GCPhase::Sweeping) {
        gc_step();
        return;
    }
    if (gc_mode == GCMode::Generational) {
        if (allocated_bytes - gc_bytes_at_minor >= gc_minor_threshold)
            gc_minor();
        if (gc_mode == GCMode::Generational && gc_old_bytes >= gc_major_threshold
            && allocated_bytes >= gc_bytes_threshold)
            collect_garbage();
    } else if (allocated_bytes >= gc_bytes_threshold) {
        collect_garbage();
    }
}

//------------------ LState::gc_remember — record an old-generation owner for the next minor
void LState::gc_remember(LHeader* owner)
{
    if (owner->flags & LFLAG_REMEMBERED)
        return;
    owner->flags |= LFLAG_REMEMBERED;
    gc_remembered.push_back(owner);
}

//------------------ GCStack growth (cold) + reserve/free
void LState::gc_stack_grow(LState::GCStack* s)
{
    size_t ncap = s->cap ? s->cap * 2 : 1024;
    LHeader** nd = static_cast<LHeader**>(std::realloc(s->data, ncap * sizeof(LHeader*)));
    if (!nd)
        throw std::bad_alloc();
    s->data = nd;
    s->cap = ncap;
}

void LState::GCStack::reserve(size_t n)
{
    if (n <= cap)
        return;
    LHeader** nd = static_cast<LHeader**>(std::realloc(data, n * sizeof(LHeader*)));
    if (!nd)
        throw std::bad_alloc();
    data = nd;
    cap = n;
}

void LState::GCStack::free()
{
    std::free(data);
    data = nullptr;
    top = cap = 0;
}

//------------------ LState::gc_remember_cell — record an upvalue cell for the next minor
void LState::gc_remember_cell(const LUpValue& cell)
{
    if (!cell || cell->is_gc_obj())
        return;
    if (!gc_remembered_cell_set.insert(cell.get()).second)
        return;
    gc_remembered_cells.push_back(cell);
}

//------------------ gc_barrier_header — slow path of the write barrier
void gc_barrier_header(LState* L, LHeader* owner, const LValue& newval)
{
    if (!newval.is_gc_obj())
        return;
    LHeader* nv = static_cast<LHeader*>(newval.as_pointer());
    if (!nv || nv->age != AGE_YOUNG)
        return;
    if (owner->age == AGE_OLD)
        L->gc_remember(owner);
}

//------------------ gc_barrier_cell — upvalue cell write barrier
void gc_barrier_cell(LState* L, const LUpValue& cell, const LValue& newval)
{
    (void)L;
    if (!newval.is_gc_obj())
        return;
    LHeader* nv = static_cast<LHeader*>(newval.as_pointer());
    if (!nv || nv->age != AGE_YOUNG)
        return;
    L->gc_remember_cell(cell);
}

//------------------ mark helpers shared by major and minor marking

static CLX_INLINE_HOT LHeader* gc_mark_value(LState* L, const LValue& v, uint8_t markval)
{
    if (!v.is_gc_obj())
        return nullptr;
    LHeader* h = v.as_pointer();
    if (!h)
        return nullptr;
    if (h->type != static_cast<uint8_t>(v.type))
        return nullptr;
    if (v.type == ValueType::UserData) {

        bool real = false;
        for (LHeader* o = L->allocated_objects; o; o = o->next)
            if (o == h) {
                real = true;
                break;
            }
        if (!real)
            return nullptr;
    }
    if (h->marked != 0)
        return nullptr;
    h->marked = markval;
    return h;
}

static CLX_INLINE_HOT void gc_push_for_trace(LState::GCStack& wl, LHeader* h, LValue v)
{
    ValueType t = v.type;
    if (t == Table || t == Thread || t == Function || t == UserData)
        wl.push(h);
}

static bool gc_trace_table_young(LState* L, LTable* t, LState::GCStack& wl)
{
    bool has_nonold = false;
    for (size_t i = 0; i < t->array_size; ++i) {
        if (t->array_types[i] == Nil)
            continue;
        LValue v(t->array[i], t->array_types[i]);
        if (!v.is_gc_obj())
            continue;
        LHeader* h = v.as_pointer();
        if (!h || h->type != static_cast<uint8_t>(v.type))
            continue;
        if (h->age == AGE_OLD)
            continue;
        has_nonold = true;
        if (h->marked != 0)
            continue;
        if (v.type == ValueType::UserData && !L->is_allocated_userdata(h))
            continue;
        h->marked = 1;
        gc_push_for_trace(wl, h, v);
    }
    LTableExt* ex = t->ext;
    if (!ex)
        return has_nonold;
    for (size_t _i = 0; _i < ex->hash_size; ++_i) {
        HashEntry& e = ex->entries[_i];
        if (e.ktype == Nil)
            continue;
        for (int which = 0; which < 2; ++which) {
            LValue v(which == 0 ? e.key : e.val, which == 0 ? e.ktype : e.vtype);
            if (!v.is_gc_obj())
                continue;
            LHeader* h = v.as_pointer();
            if (!h || h->type != static_cast<uint8_t>(v.type))
                continue;
            if (h->age == AGE_OLD)
                continue;
            has_nonold = true;
            if (h->marked != 0)
                continue;
            if (v.type == ValueType::UserData && !L->is_allocated_userdata(h))
                continue;
            h->marked = 1;
            gc_push_for_trace(wl, h, v);
        }
    }
    if (ex->metatable && ex->metatable->age != AGE_OLD && ex->metatable->marked == 0) {
        has_nonold = true;
        ex->metatable->marked = 1;
        wl.push(ex->metatable);
    } else if (ex->metatable && ex->metatable->age != AGE_OLD) {
        has_nonold = true;
    }
    return has_nonold;
}

static CLX_INLINE_HOT bool gc_mark_young(LState* L, const LValue& v, LState::GCStack& wl)
{
    if (!v.is_gc_obj())
        return false;
    LHeader* h = v.as_pointer();
    if (!h || h->type != static_cast<uint8_t>(v.type) || h->age == AGE_OLD)
        return false;
    if (h->marked != 0)
        return true;
    if (v.type == ValueType::UserData && !L->is_allocated_userdata(h))
        return true;
    h->marked = 1;
    gc_push_for_trace(wl, h, v);
    return true;
}

static bool gc_trace_thread_young(LState* L, LThread* th, LState::GCStack& wl)
{
    bool has_nonold = false;
    auto one = [&](const LValue& v) {
        if (gc_mark_young(L, v, wl))
            has_nonold = true;
    };
    one(th->function);
    if (th->caller)
        one(LValue(Thread, th->caller));
    for (size_t i = 0; i < th->yield_args.count; ++i)
        one(th->yield_args[i]);
    for (size_t i = 0; i < th->resume_args.count; ++i)
        one(th->resume_args[i]);
    return has_nonold;
}

static bool gc_trace_function_young(LState* L, LCFunction* f, LState::GCStack& wl)
{
    bool has_nonold = false;
    for (const LUpValue& c : f->gc_cells)
        if (c && gc_mark_young(L, *c, wl))
            has_nonold = true;
    return has_nonold;
}

//------------------ LState::is_allocated_userdata — membership check on allocated_objects
bool LState::is_allocated_userdata(const LHeader* h) const
{
    for (LHeader* o = allocated_objects; o; o = o->next)
        if (o == h)
            return true;
    return false;
}

//------------------ gc_trace_remember_nonold_children — write-barrier catch-up for

static void gc_trace_remember_nonold_children(LTable* t)
{
    LState* L = clx_current_L;
    if (!L)
        return;
    for (size_t i = 0; i < t->array_size; ++i) {
        if (t->array_types[i] == Nil)
            continue;
        LValue v(t->array[i], t->array_types[i]);
        if (!v.is_gc_obj())
            continue;
        LHeader* h = v.as_pointer();
        if (h && h->type == static_cast<uint8_t>(v.type) && h->age != AGE_OLD)
            L->gc_remember(t);
    }
    if (t->ext) {
        for (size_t i = 0; i < t->ext->hash_size; ++i) {
            HashEntry& e = t->ext->entries[i];
            if (e.ktype == Nil)
                continue;
            for (int which = 0; which < 2; ++which) {
                LValue v(which == 0 ? e.key : e.val, which == 0 ? e.ktype : e.vtype);
                if (!v.is_gc_obj())
                    continue;
                LHeader* h = v.as_pointer();
                if (h && h->type == static_cast<uint8_t>(v.type) && h->age != AGE_OLD) {
                    L->gc_remember(t);
                    return;
                }
            }
        }
        if (t->ext->metatable && t->ext->metatable->age != AGE_OLD)
            L->gc_remember(t);
    }
}

//------------------ LState::gc_minor — young-generation collection (generational mode)

void LState::gc_minor()
{
    ++gc_stats_minors;
    gc_minor_active = true;
    if (gc_stats_remembered_high < gc_remembered.size())
        gc_stats_remembered_high = gc_remembered.size();
    auto& wl = gc_worklist;
    wl.clear();

    auto push_if_needed = [&](const LValue& v) {
        if (!v.is_gc_obj())
            return;
        LHeader* h = v.as_pointer();
        if (!h || h->type != static_cast<uint8_t>(v.type) || h->age == AGE_OLD || h->marked != 0)
            return;
        if (v.type == ValueType::UserData && !is_allocated_userdata(h))
            return;
        h->marked = 1;
        gc_push_for_trace(wl, h, v);
    };

    //------------------ roots: shadow stack, state threads, permanent roots
    if (_G)
        push_if_needed(LValue(Table, _G));
    if (main_thread)
        push_if_needed(LValue(Thread, main_thread));
    if (running_thread && running_thread != main_thread)
        push_if_needed(LValue(Thread, running_thread));
    for (size_t i = 0; i < shadow_top; ++i)
        if (shadow_stack[i].val)
            push_if_needed(LValue(*shadow_stack[i].val, *shadow_stack[i].type));
    for (const LValue& r : permanent_roots)
        push_if_needed(r);

    //------------------ roots: gc_recent (every object allocated since the last collection)

    for (size_t _ri = 0; _ri < gc_recent.top; ++_ri) {
        LHeader* h = gc_recent.data[_ri];
        if (h->marked != 0 || h->age != AGE_YOUNG)
            continue;
        if (h->type == static_cast<uint8_t>(UserData) && !is_allocated_userdata(h))
            continue;
        h->marked = 1;
        if (h->type == static_cast<uint8_t>(Table)) {
            wl.push(h);
        } else if (h->type == static_cast<uint8_t>(Thread) || h->type == static_cast<uint8_t>(Function)
            || h->type == static_cast<uint8_t>(UserData)) {
            wl.push(h);
        }
    }

    //------------------ roots: remembered old-generation owners and upvalue cells.

    for (LHeader* owner : gc_remembered) {
        if (!(owner->flags & LFLAG_REMEMBERED))
            continue;
        if (owner->flags & LFLAG_VM_PROXY)
            continue;
        if (owner->age != AGE_OLD) {
            if (owner->marked == 0) {
                owner->marked = 1;
                if (owner->type == static_cast<uint8_t>(Table) || owner->type == static_cast<uint8_t>(Thread)
                    || owner->type == static_cast<uint8_t>(Function) || owner->type == static_cast<uint8_t>(UserData))
                    wl.push(owner);
            }
        } else if (owner->type == static_cast<uint8_t>(Table)) {

            if (!gc_trace_table_young(this, static_cast<LTable*>(owner), wl))
                owner->flags &= ~LFLAG_REMEMBERED;
        } else if (owner->type == static_cast<uint8_t>(UserData)) {
            LUserdata* ud = static_cast<LUserdata*>(owner);
            if (ud->metatable && ud->metatable->age != AGE_OLD) {
                if (ud->metatable->marked == 0) {
                    ud->metatable->marked = 1;
                    wl.push(ud->metatable);
                }
            } else {
                owner->flags &= ~LFLAG_REMEMBERED;
            }
        } else if (owner->type == static_cast<uint8_t>(Thread)) {

            if (!gc_trace_thread_young(this, static_cast<LThread*>(owner), wl))
                owner->flags &= ~LFLAG_REMEMBERED;
        } else if (owner->type == static_cast<uint8_t>(Function)) {

            if (!gc_trace_function_young(this, static_cast<LCFunction*>(owner), wl))
                owner->flags &= ~LFLAG_REMEMBERED;
        } else {
            owner->flags &= ~LFLAG_REMEMBERED;
        }
    }
    for (const LUpValue& cell : gc_remembered_cells)
        if (cell)
            push_if_needed(*cell);

    //------------------ trace: descend only into YOUNG children
    while (!wl.empty()) {
        LHeader* curr = wl.back();
        wl.pop_back();
        if (curr->flags & LFLAG_VM_PROXY)
            continue;
        if (curr->type == static_cast<uint8_t>(Table)) {
            gc_trace_table_young(this, static_cast<LTable*>(curr), wl);
        } else if (curr->type == static_cast<uint8_t>(Thread)) {
            gc_trace_thread_young(this, static_cast<LThread*>(curr), wl);
        } else if (curr->type == static_cast<uint8_t>(Function)) {
            gc_trace_function_young(this, static_cast<LCFunction*>(curr), wl);
        }

        else if (curr->type == static_cast<uint8_t>(UserData)) {
            LUserdata* ud = static_cast<LUserdata*>(curr);
            if (ud->metatable && ud->metatable->age == AGE_YOUNG && ud->metatable->marked == 0) {
                ud->metatable->marked = 1;
                wl.push(ud->metatable);
            }
        }
    }

    //------------------ single walk over the allocation chain: age/promote, then free.

    auto age_and_promote = [&](LHeader* h) {
        uint8_t from = h->age;
        uint8_t to = (from == AGE_YOUNG) ? AGE_SURVIVOR : AGE_OLD;
        gc_update_old_bytes(h, from, to);
        h->age = to;

        if (to == AGE_OLD && h->marked == 1 && !(h->flags & LFLAG_VM_PROXY)) {
            if (h->type == static_cast<uint8_t>(Table)) {
                gc_trace_remember_nonold_children(static_cast<LTable*>(h));
            } else if (h->type == static_cast<uint8_t>(UserData)) {
                LUserdata* ud = static_cast<LUserdata*>(h);
                if (ud->metatable && ud->metatable->age != AGE_OLD)
                    gc_remember(ud);
            }
        }
    };

    //------------------ single walk over the allocation chain: age/promote,

    size_t before_sweep = allocated_bytes;
    LHeader** link = &allocated_objects;
    while (*link) {
        LHeader* o = *link;
        if (o->age == AGE_OLD) {
            link = &o->next;
            continue;
        }
        if (o->marked == 1) {
            age_and_promote(o);

            o->marked = (o->age == AGE_SURVIVOR) ? 2 : 0;
            link = &o->next;
        } else if (o->age == AGE_SURVIVOR) {
            if (o->marked == 2) {
                o->marked = 0;
                link = &o->next;
            } else {
                LHeader* next_obj = o->next;
                gc_dispose_swept(this, o);
                *link = next_obj;
                object_count--;
            }
        } else {
            link = &o->next;
        }
    }
    gc_stats_minor_freed += (before_sweep > allocated_bytes) ? (before_sweep - allocated_bytes) : 0;

    //------------------ drop stale remembered entries and pins (mark bits were

    {
        size_t w = 0;
        for (size_t i = 0; i < gc_remembered.size(); ++i) {
            LHeader* h = gc_remembered[i];
            if (h->flags & LFLAG_REMEMBERED) {
                gc_remembered[w++] = h;
            } else {
                h->flags &= ~LFLAG_REMEMBERED;
            }
        }
        gc_remembered.resize(w);
        w = 0;
        for (size_t i = 0; i < gc_remembered_cells.size(); ++i) {
            const LUpValue& cell = gc_remembered_cells[i];
            if (cell && !cell->is_gc_obj()) {
                gc_remembered_cells[w++] = cell;
            }
        }
        gc_remembered_cells.resize(w);
        gc_remembered_cell_set.clear();
        for (const LUpValue& cell : gc_remembered_cells)
            gc_remembered_cell_set.insert(cell.get());
        for (size_t _pi = 0; _pi < gc_pinned.top; ++_pi)
            gc_pinned.data[_pi]->flags &= ~LFLAG_GC_PIN;
        gc_pinned.clear();
    }

    //------------------ rebuild pacing state
    gc_recent.clear();
    gc_bytes_at_minor = allocated_bytes;
    gc_minor_active = false;

    //------------------ major scheduling: if the old set outgrew its budget, the next

    gc_major_pending = (gc_old_bytes >= gc_major_threshold);

    wl.clear();
}

//------------------ LState::gc_step — incremental GC sweep step
bool LState::gc_step()
{
    if (gc_phase != GCPhase::Sweeping)
        return true;

    size_t budget = GC_STEP_BUDGET;
    LHeader* curr = gc_sweep_cursor;
    LHeader* prev = gc_prev;

    while (curr && budget--) {
        LHeader* next_obj = curr->next;

        if (curr->marked == 0 && !(curr->flags & (LFLAG_REMEMBERED | LFLAG_GC_PIN))) {
            if (prev) {
                prev->next = next_obj;
            } else {
                if (curr != allocated_objects) {
                    LHeader* h = allocated_objects;
                    while (h && h->next != curr)
                        h = h->next;
                    if (h) {
                        h->next = next_obj;
                        prev = h;
                    }
                } else {
                    allocated_objects = next_obj;
                }
            }
            gc_dispose_swept(this, curr);
            curr = next_obj;
        } else {
            if (gc_mode == GCMode::Generational && curr->age != AGE_OLD
                && !(curr->flags & LFLAG_REMEMBERED) && !(curr->flags & LFLAG_GC_PIN))
                gc_update_old_bytes(curr, curr->age, AGE_OLD), curr->age = AGE_OLD;
            curr->marked = 0;
            object_count++;
            prev = curr;
            curr = next_obj;
        }
    }

    gc_sweep_cursor = curr;
    gc_prev = prev;

    if (!gc_sweep_cursor) {
        if (gc_draining)
            return true;
        gc_draining = true;
        gc_drain_finalizables(this);
        gc_prev = nullptr;
        gc_phase = GCPhase::Idle;
        overflow_heap_used = 0;

        if (gc_mode == GCMode::Generational) {

            gc_bytes_at_minor = allocated_bytes;
        }

        size_t live = allocated_bytes;
        //------------------ Pacing: one collection is a synchronous mark+sweep of O(heap), so

        size_t headroom = std::clamp(live / 2, size_t(1 * 1024 * 1024), size_t(256 * 1024 * 1024));
        if (gc_headroom_override > 0)
            headroom = gc_headroom_override;
        gc_bytes_threshold = live + headroom;
        gc_draining = false;
        return true;
    }
    return false;
}

//------------------ LState::collect_garbage — full mark-sweep collection (entry point)

void LState::collect_garbage()
{
    ++gc_stats_collects;
    if (gc_mode == GCMode::Generational && !gc_draining && gc_phase == GCPhase::Idle)
        gc_minor();
    size_t before = allocated_bytes;
    collect_garbage_full();
    gc_stats_major_freed += (before > allocated_bytes) ? (before - allocated_bytes) : 0;
    if (gc_mode == GCMode::Generational)
        gc_major_pending = false;
}

//------------------ LState::collect_garbage_full — mark-sweep collection
void LState::collect_garbage_full()
{
    if (gc_draining)
        return;
    auto& wl = gc_worklist;
    wl.clear();

    auto mark_gc = [&](LValue v, uint8_t mark) -> LHeader* { return gc_mark_value(this, v, mark); };
    auto push_if_needed = [&](LValue v) {
        LHeader* h = mark_gc(v, 1);
        if (!h)
            return;
        ValueType t = v.type;
        if (t == Table || t == Thread || t == Function || t == UserData)
            wl.push(h);
    };

    if (_G)
        push_if_needed(LValue(Table, _G));
    if (main_thread)
        push_if_needed(LValue(Thread, main_thread));
    if (running_thread && running_thread != main_thread)
        push_if_needed(LValue(Thread, running_thread));
    for (size_t i = 0; i < shadow_top; ++i)
        if (shadow_stack[i].val)
            push_if_needed(LValue(*shadow_stack[i].val, *shadow_stack[i].type));

    for (const LValue& r : permanent_roots)
        push_if_needed(r);

    if (!gc_recent.empty()) {
        for (size_t _ri = 0; _ri < gc_recent.top; ++_ri) {
            LHeader* h = gc_recent.data[_ri];
            if (h->marked != 0)
                continue;
            h->marked = 1;
            uint8_t ty = h->type;
            if (ty == static_cast<uint8_t>(Table) || ty == static_cast<uint8_t>(Thread)
                || ty == static_cast<uint8_t>(Function) || ty == static_cast<uint8_t>(UserData))
                wl.push(h);
        }
        gc_recent.clear();
    }

    if (clx_mark_vm_proxies_ptr)
        clx_mark_vm_proxies_ptr(this, wl);

    while (!wl.empty()) {
        LHeader* curr = wl.back();
        wl.pop_back();

        if (curr->flags & LFLAG_VM_PROXY) {
            continue;
        }

        if (curr->type == static_cast<uint8_t>(Table)) {
            LTable* t = static_cast<LTable*>(curr);
            {
                const uint8_t* types_raw = reinterpret_cast<const uint8_t*>(t->array_types);
                size_t i = 0;
#if defined(CLX_HAS_AVX2)
                const __m256i zero256 = _mm256_setzero_si256();
                for (; i + 32 <= t->array_size; i += 32) {
                    __m256i types = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(types_raw + i));
                    __m256i cmp = _mm256_cmpeq_epi8(types, zero256);
                    uint32_t mask = ~_mm256_movemask_epi8(cmp);
                    while (mask) {
                        int bit = clx_ctz(mask);
                        push_if_needed(LValue(t->array[i + bit], t->array_types[i + bit]));
                        mask &= mask - 1;
                    }
                }
                for (; i + 16 <= t->array_size; i += 16) {
                    __m128i types = _mm_loadu_si128(reinterpret_cast<const __m128i*>(types_raw + i));
                    __m128i cmp = _mm_cmpeq_epi8(types, _mm_setzero_si128());
                    uint32_t mask = static_cast<uint32_t>(~_mm_movemask_epi8(cmp)) & 0xFFFF;
                    while (mask) {
                        int bit = clx_ctz(mask);
                        push_if_needed(LValue(t->array[i + bit], t->array_types[i + bit]));
                        mask &= mask - 1;
                    }
                }
#elif defined(CLX_HAS_SSE2)
                const __m128i zero = _mm_setzero_si128();
                for (; i + 16 <= t->array_size; i += 16) {
                    __m128i types = _mm_loadu_si128(reinterpret_cast<const __m128i*>(types_raw + i));
                    __m128i cmp = _mm_cmpeq_epi8(types, zero);
                    uint32_t mask = static_cast<uint32_t>(~_mm_movemask_epi8(cmp)) & 0xFFFF;
                    while (mask) {
                        int bit = clx_ctz(mask);
                        push_if_needed(LValue(t->array[i + bit], t->array_types[i + bit]));
                        mask &= mask - 1;
                    }
                }
#elif defined(CLX_HAS_NEON)
                const uint8x16_t zero = vdupq_n_u8(0);
                for (; i + 16 <= t->array_size; i += 16) {
                    uint8x16_t types = vld1q_u8(types_raw + i);
                    uint8x16_t cmp = vceqq_u8(types, zero);
                    uint8_t lane_vals[16];
                    vst1q_u8(lane_vals, vmvnq_u8(cmp));
                    for (int k = 0; k < 16; ++k) {
                        if (lane_vals[k])
                            push_if_needed(LValue(t->array[i + k], t->array_types[i + k]));
                    }
                }
#endif
                for (; i < t->array_size; ++i)
                    push_if_needed(LValue(t->array[i], t->array_types[i]));
            }
            LTableExt* ex = t->ext;
            if (ex) {
                if (ex->hash_bitmap) {
                    size_t bm_words = (ex->hash_size + 63) / 64;
                    for (size_t word = 0; word < bm_words; ++word) {
                        uint64_t bits = ex->hash_bitmap[word];
                        while (bits) {
                            size_t idx = word * 64 + clx_ctzll(bits);
                            if (idx >= ex->hash_size)
                                break;
                            LValue kv(ex->entries[idx].key, ex->entries[idx].ktype);
                            push_if_needed(kv);
                            push_if_needed(LValue(ex->entries[idx].val, ex->entries[idx].vtype));
                            bits &= bits - 1;
                        }
                    }
                } else {
                    for (size_t _i = 0; _i < ex->hash_size; ++_i) {
                        if (ex->entries[_i].ktype == Nil)
                            continue;
                        LValue kv(ex->entries[_i].key, ex->entries[_i].ktype);
                        push_if_needed(kv);
                        push_if_needed(LValue(ex->entries[_i].val, ex->entries[_i].vtype));
                    }
                }
                if (ex->metatable && ex->metatable->marked == 0) {
                    ex->metatable->marked = 1;
                    wl.push(ex->metatable);
                }
            }
        } else if (curr->type == static_cast<uint8_t>(Thread)) {
            LThread* th = static_cast<LThread*>(curr);
            push_if_needed(th->function);
            if (th->caller)
                push_if_needed(LValue(Thread, th->caller));
            for (size_t i = 0; i < th->yield_args.count; ++i)
                push_if_needed(th->yield_args[i]);
            for (size_t i = 0; i < th->resume_args.count; ++i)
                push_if_needed(th->resume_args[i]);
        } else if (curr->type == static_cast<uint8_t>(Function)) {
            LCFunction* f = static_cast<LCFunction*>(curr);
            if (f->env)
                push_if_needed(LValue(Table, f->env));
            for (const LUpValue& cell : f->gc_cells)
                if (cell)
                    push_if_needed(*cell);
        } else if (curr->type == static_cast<uint8_t>(UserData)) {
            LUserdata* ud = static_cast<LUserdata*>(curr);
            if (ud->metatable && ud->metatable->marked == 0) {
                ud->metatable->marked = 1;
                wl.push(ud->metatable);
            }
        }
    }

    std::vector<LHeader*> protect_wl;
    for (LTable* obj = metatabled_tables; obj; obj = obj->ext->meta_next) {
        if (obj->marked == 0 && !(obj->flags & LFLAG_VM_PROXY)) {
            LTable* mt = obj->ext ? obj->ext->metatable : nullptr;
            if (mt && mt->marked == 0) {
                mt->marked = 2;
                protect_wl.push_back(mt);
            }
        }
    }
    while (!protect_wl.empty()) {
        LHeader* curr = protect_wl.back();
        protect_wl.pop_back();
        if (curr->type == static_cast<uint8_t>(Table)) {
            LTable* tt = static_cast<LTable*>(curr);
            {
                const uint8_t* types_raw = reinterpret_cast<const uint8_t*>(tt->array_types);
                size_t i = 0;
#if defined(CLX_HAS_AVX2)
                const __m256i zero256 = _mm256_setzero_si256();
                for (; i + 32 <= tt->array_size; i += 32) {
                    __m256i types = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(types_raw + i));
                    __m256i cmp = _mm256_cmpeq_epi8(types, zero256);
                    uint32_t mask = ~_mm256_movemask_epi8(cmp);
                    while (mask) {
                        int bit = clx_ctz(mask);
                        LValue v = LValue(tt->array[i + bit], tt->array_types[i + bit]);
                        if (LHeader* h = mark_gc(v, 2))
                            protect_wl.push_back(h);
                        mask &= mask - 1;
                    }
                }
                for (; i + 16 <= tt->array_size; i += 16) {
                    __m128i types = _mm_loadu_si128(reinterpret_cast<const __m128i*>(types_raw + i));
                    __m128i cmp = _mm_cmpeq_epi8(types, _mm_setzero_si128());
                    uint32_t mask = static_cast<uint32_t>(~_mm_movemask_epi8(cmp)) & 0xFFFF;
                    while (mask) {
                        int bit = clx_ctz(mask);
                        LValue v = LValue(tt->array[i + bit], tt->array_types[i + bit]);
                        if (LHeader* h = mark_gc(v, 2))
                            protect_wl.push_back(h);
                        mask &= mask - 1;
                    }
                }
#elif defined(CLX_HAS_SSE2)
                const __m128i zero = _mm_setzero_si128();
                for (; i + 16 <= tt->array_size; i += 16) {
                    __m128i types = _mm_loadu_si128(reinterpret_cast<const __m128i*>(types_raw + i));
                    __m128i cmp = _mm_cmpeq_epi8(types, zero);
                    uint32_t mask = static_cast<uint32_t>(~_mm_movemask_epi8(cmp)) & 0xFFFF;
                    while (mask) {
                        int bit = clx_ctz(mask);
                        LValue v = LValue(tt->array[i + bit], tt->array_types[i + bit]);
                        if (LHeader* h = mark_gc(v, 2))
                            protect_wl.push_back(h);
                        mask &= mask - 1;
                    }
                }
#elif defined(CLX_HAS_NEON)
                const uint8x16_t zero = vdupq_n_u8(0);
                for (; i + 16 <= tt->array_size; i += 16) {
                    uint8x16_t types = vld1q_u8(types_raw + i);
                    uint8x16_t cmp = vceqq_u8(types, zero);
                    uint8_t lane_vals[16];
                    vst1q_u8(lane_vals, vmvnq_u8(cmp));
                    for (int k = 0; k < 16; ++k) {
                        if (lane_vals[k]) {
                            LValue v = LValue(tt->array[i + k], tt->array_types[i + k]);
                            if (LHeader* h = mark_gc(v, 2))
                                protect_wl.push_back(h);
                        }
                    }
                }
#endif
                for (; i < tt->array_size; ++i) {
                    LValue v = LValue(tt->array[i], tt->array_types[i]);
                    if (LHeader* h = mark_gc(v, 2))
                        protect_wl.push_back(h);
                }
            }
            LTableExt* tt_ex = tt->ext;
            if (tt_ex) {
                if (tt_ex->hash_bitmap) {
                    size_t bm_words = (tt_ex->hash_size + 63) / 64;
                    for (size_t word = 0; word < bm_words; ++word) {
                        uint64_t bits = tt_ex->hash_bitmap[word];
                        while (bits) {
                            size_t idx = word * 64 + clx_ctzll(bits);
                            if (idx >= tt_ex->hash_size)
                                break;
                            LValue kv(tt_ex->entries[idx].key, tt_ex->entries[idx].ktype);
                            for (LValue v : { kv, LValue(tt_ex->entries[idx].val, tt_ex->entries[idx].vtype) }) {
                                if (LHeader* h = mark_gc(v, 2))
                                    protect_wl.push_back(h);
                            }
                            bits &= bits - 1;
                        }
                    }
                } else {
                    for (size_t _pi = 0; _pi < tt_ex->hash_size; ++_pi) {
                        if (tt_ex->entries[_pi].ktype == Nil)
                            continue;
                        LValue kv(tt_ex->entries[_pi].key, tt_ex->entries[_pi].ktype);
                        for (LValue v : { kv, LValue(tt_ex->entries[_pi].val, tt_ex->entries[_pi].vtype) }) {
                            if (LHeader* h = mark_gc(v, 2))
                                protect_wl.push_back(h);
                        }
                    }
                }
            }
        }
    }
    gc_phase = GCPhase::Sweeping;
    gc_sweep_cursor = allocated_objects;
    gc_prev = nullptr;
    gc_finalizable = nullptr;
    gc_finalizable_ud = nullptr;
    object_count = 0;
    while (!gc_step())
        ;

    //------------------ generational bookkeeping: the remembered set is NOT cleared here —

    if (gc_mode == GCMode::Generational) {
        gc_bytes_at_minor = allocated_bytes;
    }
}

//------------------ LState::register_module — register module loader
void LState::register_module(const std::string& name, LValue (*func)(LState*))
{
    register_static_preload(this, name.c_str(), func);
}

//------------------ LState::register_loaded_module — seed package.loaded[name] with an already-built module value
void LState::register_loaded_module(const std::string& name, const LValue& module)
{
    LValue pack_val = get_global(this, "package");
    if (pack_val.type != ValueType::Table)
        return;
    LValue loaded = static_cast<LTable*>(pack_val.as_pointer())->gettable(LValue(intern_string("loaded")));
    if (loaded.type != ValueType::Table)
        return;
    static_cast<LTable*>(loaded.as_pointer())->settable(LValue(intern_string(name)), module);
}

//------------------ call_function — call a value as function
MultiValue call_function(LState* L, const LValue& func, const LValue* args, size_t count, const char* file, int line)
{
    L->current_file = file;
    L->current_line = line;

    size_t prev_shadow = L->shadow_top;
    L->shadow_stack[L->shadow_top++] = TypedSlot(const_cast<TValue*>(&func.val), const_cast<ValueType*>(&func.type));

    if (func.type == Function) {
        LCFunction* f = static_cast<LCFunction*>(func.as_pointer());
        LCFunction* saved_func = L->current_func;
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
        LTable* mt = tbl_metatable(static_cast<LTable*>(func.as_pointer()));
        if (mt) {
            LValue m = mt->gettable(L->str_call);
            if (m.type != Nil) {
                size_t nargs = count + 1;
                LValue* new_args;
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
MultiValue pcall_function(LState* L, const LValue& func, const LValue* args, size_t count)
{
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
    } catch (const LRuntimeException& e) {
        L->shadow_top = shadow_base;

        LValue err_val = e.error_obj;
        if (err_val.type != String) {
            err_val = LValue(L->intern_string(e.what()));
        }
        return MultiValue({ LValue(false), err_val });
    } catch (const std::exception& e) {
        L->shadow_top = shadow_base;
        return MultiValue({ LValue(false), LValue(L->intern_string(e.what())) });
    }
}

//------------------ call_direct — fast path for LCFunction direct calls
MultiValue call_direct(LState* L, const LValue& func, const LValue* args, size_t count, const char* file, int line)
{
    if (func.type == ValueType::Function) {
        LCFunction* f = static_cast<LCFunction*>(func.as_pointer());
        if (f->direct) {
            LCFunction* saved = L->current_func;
            L->current_func = f;
            MultiValue ret = f->direct(L, args, count);
            L->current_func = saved;
            return ret;
        }
    }
    return call_function(L, func, args, count, file, line);
}

//------------------ callmeta — call metamethod
static MultiValue lazy_funcs_index(LState* L, const LValue* args, size_t n)
{
    if (n < 2 || args[1].type != String)
        return MultiValue();

    const char* name = args[1].as_string();
    LTable* t = static_cast<LTable*>(args[0].as_pointer());
    LTable* mt = tbl_metatable(t);
    if (!mt)
        return MultiValue();

    LValue regs_key(L->intern_string("__lazy_regs"));
    LValue count_key(L->intern_string("__lazy_count"));

    LValue regs_val = mt->gettable(regs_key);
    LValue count_val = mt->gettable(count_key);
    if (regs_val.type != UserData || regs_val.type == Nil || count_val.type == Nil || count_val.type != Int64)
        return MultiValue();

    const LazyReg* regs = reinterpret_cast<const LazyReg*>(regs_val.as_pointer());
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
void set_lazy_funcs(LState* L, const LValue& table, const LazyReg* regs, size_t count)
{
    if (table.type != Table)
        return;
    LTable* t = static_cast<LTable*>(table.as_pointer());

    LTable* mt = tbl_metatable(t);
    if (!mt) {
        mt = static_cast<LTable*>(L->create_table().as_pointer());
        tbl_set_metatable(t, mt);
        if (t->age == AGE_OLD)
            gc_barrier_header(L, t, LValue(Table, mt));
    }
    meta_list_add(L, t);
    mt->settable(LValue(L->intern_string("__lazy_regs")),
        LValue(UserData, reinterpret_cast<LHeader*>(const_cast<LazyReg*>(regs))));
    mt->settable(LValue(L->intern_string("__lazy_count")), LValue(static_cast<int64_t>(count)));

    LValue existing = mt->gettable(L->str_index);
    if (existing.type == Nil) {
        LValue handler = L->create_closure(CFunctionType(lazy_funcs_index));
        mt->settable(L->str_index, handler);
    }
}

//------------------ LState::create_table — allocate a table
LValue LState::create_table(size_t asize, size_t hsize)
{
    gc_maybe_collect();

    LTable* t;
    bool recycled_tbl = (free_tables != nullptr);
    uint8_t prev_age_tbl = recycled_tbl ? free_tables->age : AGE_YOUNG;
    if (free_tables) {
        t = free_tables;
        free_tables = static_cast<LTable*>(free_tables->next);
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

            if (t->array && t->array != t->small_array) {
                delete[] t->array;
                t->array = nullptr;
            }
            if (t->array_types && t->array_types != t->small_array_types) {
                delete[] t->array_types;
                t->array_types = nullptr;
            }
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
            if (t->array && t->array != t->small_array)
                delete[] t->array;
            if (t->array_types && t->array_types != t->small_array_types)
                delete[] t->array_types;
            t->array = new TValue[asize];
            t->array_types = new ValueType[asize]();
            t->array_size = asize;
            t->array_cap = asize;
        }
    } else {
        t->array_size = 0;
    }

    t->type = static_cast<uint8_t>(Table);
    t->marked = 0;
    t->age = AGE_YOUNG;
    if (recycled_tbl && prev_age_tbl != AGE_YOUNG
        && (gc_phase == GCPhase::Sweeping || gc_minor_active)) {

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
clx::LValue clx::LState::create_closure(CFunctionType func, LTable* env, std::vector<LUpValue> gc_cells)
{
    gc_maybe_collect();

    LCFunction* f;
    bool recycled_fn = (free_functions != nullptr);
    uint8_t prev_age_fn = recycled_fn ? free_functions->age : AGE_YOUNG;
    if (free_functions) {
        f = free_functions;
        free_functions = static_cast<LCFunction*>(free_functions->next);
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

    auto* fnptr = f->func.target<MultiValue (*)(LState*, const LValue*, size_t)>();
    if (fnptr) {
        f->direct = *fnptr;
    }
    f->type = static_cast<uint8_t>(Function);
    f->marked = 0;
    f->age = AGE_YOUNG;
    if (recycled_fn && prev_age_fn != AGE_YOUNG
        && (gc_phase == GCPhase::Sweeping || gc_minor_active)) {

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
LValue newuserdata(LState* L, size_t size)
{
    L->gc_maybe_collect();

    char* mem = new char[sizeof(LUserdata) + size];
    LUserdata* ud = reinterpret_cast<LUserdata*>(mem);
    ud->type = static_cast<uint8_t>(UserData);
    ud->marked = 0;
    ud->flags = 0;
    ud->age = AGE_YOUNG;
    ud->metatable = nullptr;
    ud->size = size;

    ud->next = L->allocated_objects;
    L->allocated_objects = ud;
    L->gc_recent.push(ud);
    L->object_count++;
    L->allocated_bytes += sizeof(LUserdata) + size;
    return LValue(UserData, ud);
}

//------------------ call_bin_metamethod — call binary op metamethod
LValue call_bin_metamethod(LState* L, const LValue& a, const LValue& b, const char* event)
{
    LTable* mt = nullptr;
    if (a.type == Table)
        mt = tbl_metatable(static_cast<LTable*>(a.as_pointer()));
    else if (a.type == UserData)
        mt = static_cast<LUserdata*>(a.as_pointer())->metatable;
    if (!mt && b.type == Table)
        mt = tbl_metatable(static_cast<LTable*>(b.as_pointer()));
    else if (!mt && b.type == UserData)
        mt = static_cast<LUserdata*>(b.as_pointer())->metatable;

    if (mt) {
        LValue method = mt->gettable(LValue(L->intern_string(event)));
        if (method.type != Nil && method.type == Function) {
            LValue args[2] = { a, b };
            MultiValue res = call_function_rooted(L, method, args, 2, __FILE__, __LINE__);
            return res[0];
        }
    }

    std::string prefix = file_line_prefix(L);

    std::string_view ev(event);

    if (ev == "__eq")
        return clx::LValue(false);

    std::string op_type = "perform arithmetic on";
    if (ev == "__lt" || ev == "__le")
        op_type = "compare";
    else if (ev == "__concat")
        op_type = "concatenate";

    std::string err_msg
        = prefix + "attempt to " + op_type + " a " + VALUE_TYPE_NAMES[static_cast<size_t>(a.type)] + " value";
    throw LRuntimeException(LValue(L->intern_string(err_msg)));
}

//------------------ get_global — get global variable
LValue get_global(LState* L, const char* name)
{
    LValue val = L->_G->gettable(LValue(L->intern_string(name)));
    return val.type != Nil ? val : LValue();
}

//------------------ set_global — set global variable
void set_global(LState* L, const char* name, const LValue& val)
{
    L->_G->settable(LValue(L->intern_string(name)), val);
}

//------------------ open — create Lua state
extern thread_local LState* clx_current_L;
thread_local LState* clx_current_L = nullptr;

LState* open(int argc, char* argv[])
{
    LState* L = new LState();
    clx_current_L = L;

    LThread* main_th = new LThread();
    main_th->state = L;
    main_th->is_main = true;
    main_th->status = THREAD_RUNNING;
#if defined(_WIN32)
    main_th->fiber = ConvertThreadToFiber(nullptr);
#elif (defined(__APPLE__) || defined(__linux__)) && (defined(__aarch64__) || defined(__x86_64__))
    clx_coro_save(&main_th->ctx);
#else
    getcontext(&main_th->ctx);
#endif
    L->main_thread = main_th;
    L->running_thread = main_th;

    if (argc > 0 && argv) {
        LValue arg_table = L->create_table(argc);
        LTable* t = static_cast<LTable*>(arg_table.as_pointer());
        for (int i = 0; i < argc; ++i) {
            t->settable(LValue(static_cast<int64_t>(i)), LValue(L->intern_string(argv[i])));
        }
        L->_G->settable(LValue(L->intern_string("arg")), arg_table);
    }

    luastd_base(L);
    luastd_package(L);

    return L;
}

//------------------ close — close Lua state
void close(LState* L)
{
    if (clx_current_L == L)
        clx_current_L = nullptr;
#if defined(_WIN32)
    ConvertFiberToThread();
#endif
    delete L->main_thread;
    delete L;
}

}
