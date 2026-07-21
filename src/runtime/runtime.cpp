// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  runtime.cpp · Runtime core (GC, state,...) │
// └─────────────────────────────────────────────┘

#include "clx_runtime.h"
#include "clx.h"
#include "../codegen/codegen.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <vector>

namespace clx {
    void register_static_preload(LState* L, const char* name, LValue (open_func)(LState*));
}

namespace clx {

//------------------ LThread::LThread — thread constructor
LThread::LThread() : state(nullptr), status(THREAD_SUSPENDED), caller(nullptr), is_main(false), has_error(false), close_requested(false) {
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
    if (fiber && !is_main) DeleteFiber(fiber);
#else
    if (stack_memory) delete[] stack_memory;
#endif
}


static void fiber_entry_impl(LThread* t) {
    LState* L = t->state;
    try {
        clx::LValue args[8];
        size_t argc = t->resume_args.count < 8 ? t->resume_args.count : 8;
        for(size_t i = 0; i < argc; ++i) args[i] = t->resume_args[i];
        t->resume_args = MultiValue();

        size_t prev_top = L->shadow_top;
        for(size_t i = 0; i < argc; ++i) L->shadow_stack[L->shadow_top++]= TypedSlot(&args[i].val, &args[i].type);
        t->yield_args = call_function(L, t->function, args, argc, "coroutine", 0);
        L->shadow_top = prev_top;
        t->status = THREAD_DEAD;
    } catch (const LRuntimeException& e) {
        t->yield_args = MultiValue(e.error_obj);
        t->status = THREAD_DEAD;
        t->has_error = true;
    } catch (...) {
        t->yield_args = MultiValue(clx::LValue("unknown error"));
        t->status = THREAD_DEAD;
        t->has_error = true;
    }

    LThread* caller = t->caller;
    L->running_thread = caller;
    caller->status = THREAD_RUNNING;

#if defined(_WIN32)
    SwitchToFiber(caller->fiber);
#elif defined(__APPLE__) && defined(__aarch64__)
    clx_coro_switch(&t->ctx, &caller->ctx);
#else
    swapcontext(&t->ctx, &caller->ctx);
#endif
}

#if defined(_WIN32)

static void WINAPI fiber_trampoline(LPVOID param) {
    fiber_entry_impl(static_cast<LThread*>(param));
}
#else
static thread_local LThread* g_starting_thread = nullptr;

static void fiber_trampoline() {
    fiber_entry_impl(g_starting_thread);
}
#endif


//------------------ create_thread: creates a new coroutine thread (public API)
LValue create_thread(LState* L, const LValue& func, double stack_size) {
    LThread* t = new LThread();
    t->state = L;
    t->function = func;

#if defined(_WIN32)
    t->fiber = CreateFiber(static_cast<SIZE_T>(stack_size), fiber_trampoline, t);
#elif defined(__APPLE__) && defined(__aarch64__)
    t->stack_memory = new char[static_cast<size_t>(stack_size)];
    clx_coro_init(&t->ctx, t->stack_memory + static_cast<size_t>(stack_size), (void*)fiber_trampoline);
    g_starting_thread = t;
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
    return LValue(Thread, t);
}


//------------------ resume: resumes a suspended coroutine (public API)
MultiValue resume(LState* L, const LValue& thread, const LValue* args, size_t count) {
    LThread* t = static_cast<LThread*>(thread.as_pointer());
    t->resume_args = MultiValue(args, count);
    t->caller = L->running_thread;
    t->caller->status = THREAD_NORMAL;
    t->status = THREAD_RUNNING;
    L->running_thread = t;

#if defined(_WIN32)
    SwitchToFiber(t->fiber);
#elif defined(__APPLE__) && defined(__aarch64__)
    clx_coro_switch(&t->caller->ctx, &t->ctx);
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


//------------------ yield: yields from a coroutine (public API)
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
#elif defined(__APPLE__) && defined(__aarch64__)
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
MultiValue close_thread(LState* L, const LValue& thread) {
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


//------------------ LRuntimeException::LRuntimeException — error exception constructor
LRuntimeException::LRuntimeException(clx::LValue err) : error_obj(err) {}

//------------------ LRuntimeException::~LRuntimeException — exception destructor
LRuntimeException::~LRuntimeException() noexcept {}

//------------------ LRuntimeException::what — get error message
const char* LRuntimeException::what() const noexcept {
    if (cached_msg.empty()) {
        cached_msg = error_obj.to_string(nullptr);
    }
    return cached_msg.c_str();
}

//------------------ LValue::to_string — convert value to string
std::string LValue::to_string(LState* L) const {
    if (L && type == Table) {
        LTable* t = static_cast<LTable*>(as_pointer());
        if (t->metatable) {
            LValue meta_key = L->str_tostring;
            LValue meta_func = t->metatable->gettable(meta_key);

            if (meta_func.type != Nil && meta_func.type == Function) {
                LValue args[1];
                args[0] = *this;

                L->shadow_stack[L->shadow_top++]= TypedSlot(&args[0].val, &args[0].type);
                MultiValue ret = call_function(L, meta_func, args, 1, __FILE__, __LINE__);
                L->shadow_top--;

                if (ret.count > 0 && ret[0].type == String) {
                    return std::string(ret[0].as_string());
                }
            }
        }
    }

    switch (type) {
        case Nil:      return "nil";
        case Boolean:     return as_bool() ? "true" : "false";
        case Double: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", as_number());
            return std::string(buf);
        }
        case Int64:  return std::to_string(as_integer());
        case String:   return std::string(as_string(), string_len());
        case Table: {
            const char* prefix = "table";
            if (L && static_cast<LTable*>(as_pointer())->metatable) {
                LValue n = static_cast<LTable*>(as_pointer())->metatable->gettable(LValue(L->intern_string("__name")));
                if (n.type != Nil && n.type == String) prefix = n.as_string();
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
        default:              return "userdata";
    }
}

//------------------ LValue::slow_eq — equality comparison
LValue LValue::slow_eq(const LValue& other) const {
    if (type != other.type) return LValue(false);
    if (type == String) {
        return LValue(std::string_view(as_string()) == std::string_view(other.as_string()));
    }
    return LValue(false);
}

//------------------ LValue::slow_lt — less-than comparison
LValue LValue::slow_lt(const LValue& other) const {
    if ((type == Double || type == Int64) &&
        (other.type == Double || other.type == Int64)) {
        double l = (type == Int64) ? (double)as_integer() : as_number();
        double r = (other.type == Int64) ? (double)other.as_integer() : other.as_number();
        return LValue(l < r);
    }
    if (type == String && other.type == String) {
        return LValue(std::string_view(as_string()) < std::string_view(other.as_string()));
    }
    return LValue(false);
}

//------------------ LValue::slow_le — less-or-equal comparison
LValue LValue::slow_le(const LValue& other) const {
    if ((type == Double || type == Int64) &&
        (other.type == Double || other.type == Int64)) {
        double l = (type == Int64) ? (double)as_integer() : as_number();
        double r = (other.type == Int64) ? (double)other.as_integer() : other.as_number();
        return LValue(l <= r);
    }
    if (type == String && other.type == String) {
        return LValue(std::string_view(as_string()) <= std::string_view(other.as_string()));
    }
    return LValue(false);
}

//------------------ LCFunction::LCFunction — C function wrapper constructor
LCFunction::LCFunction(CFunctionType f) : func(std::move(f)) {
    type = static_cast<uint8_t>(Function);
    marked = 0;
    next = nullptr;
}







static CLX_INLINE_COLD size_t next_pow2(size_t n) {
    if (n < 8) return 8;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4;
    n |= n >> 8; n |= n >> 16; n |= n >> 32;
    return n + 1;
}

















//------------------ LTable::LTable — table constructor
LTable::LTable()
    : array(nullptr), array_types(nullptr), array_size(0), array_cap(0),
      entries(nullptr), hash_size(0), hash_count(0), hash_tombs(0),
      hash_bitmap(nullptr),
      hash_version(0), array_version(0), metatable(nullptr), is_arena(false)
{
    type = static_cast<uint8_t>(Table);
    marked = 0;
    next = nullptr;
}

//------------------ LTable::~LTable — table destructor
LTable::~LTable() {
    if (!is_arena) {
        if (array)   delete[] array;
        if (array_types) delete[] array_types;
        if (entries) delete[] entries;
        if (hash_bitmap) delete[] hash_bitmap;
    }
}


//------------------ LTable::resize_hash — allocate/rehash to new_size (power of 2)
void LTable::resize_hash(size_t new_size) {
    new_size = next_pow2(new_size);

    HashEntry* new_entries = new HashEntry[new_size];
    for (size_t i = 0; i < new_size; ++i) { new_entries[i].key.payload.u64 = HASH_EMPTY; new_entries[i].ktype = Nil; }

    size_t bm_words = (new_size + 63) / 64;
    uint64_t* new_bitmap = new uint64_t[bm_words]();

    uint32_t mask = static_cast<uint32_t>(new_size - 1);
    if (entries) {
        for (size_t i = 0; i < hash_size; ++i) {
            if (entries[i].ktype == Nil) continue;
            uint64_t h = lvalue_hash(LValue(entries[i].key, entries[i].ktype)) & mask;
            while (new_entries[h].ktype != Nil)
                h = (h + 1) & mask;
            new_entries[h].key = entries[i].key;
            new_entries[h].ktype = entries[i].ktype;
            new_entries[h].val = entries[i].val;
            new_entries[h].vtype = entries[i].vtype;
            new_bitmap[h / 64] |= (1ULL << (h % 64));
        }
        if (!is_arena) {
            delete[] entries;
        }
    }

    is_arena = false;
    entries    = new_entries;
    hash_size  = new_size;
    hash_tombs = 0;
    if (hash_bitmap) delete[] hash_bitmap;
    hash_bitmap = new_bitmap;
    hash_version++;
}


//------------------ LTable::gettable — get value by key
LValue LTable::gettable(const LValue& key) {
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
    if (hash_size == 0) return LValue();

    // Inline cache: fast path for repeated access to same key on same table
    // Uses hash_version (structural changes only) not per-entry version (every write)
    uint32_t ic_idx = static_cast<uint32_t>(key.val.payload.u64 ^ (key.val.payload.u64 >> 17) ^ (key.val.payload.u64 >> 33)) % IC_SIZE;
    auto& _ic = ic[ic_idx];
    if (_ic.key_payload == key.val.payload.u64 && _ic.table_ver == hash_version && _ic.entry_idx < hash_size) {
        HashEntry& _e = entries[_ic.entry_idx];
        if (_e.ktype != Nil)
            return LValue(_e.val, _e.vtype);
    }

    uint32_t mask = static_cast<uint32_t>(hash_size - 1);
    uint64_t h    = lvalue_hash(key) & mask;
    for (;;) {
        HashEntry& e = entries[h];
        if (e.ktype == Nil) {
            if (e.key.payload.u64 == HASH_EMPTY) return LValue();
        } else if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) {
            // Update inline cache
            _ic.key_payload = key.val.payload.u64;
            _ic.entry_idx = static_cast<uint32_t>(h);
            _ic.table_ver = hash_version;
            return LValue(e.val, e.vtype);
        }
        h = (h + 1) & mask;
    }
}


//------------------ LTable::settable — set value by key
void LTable::settable(const LValue& key, const LValue& val) {

    if (key.type == Int64) {
        int64_t idx = key.as_integer();
        if (static_cast<uint64_t>(idx - 1) < array_cap) {
            array[idx - 1] = val.val; array_types[idx - 1] = val.type;
            if (static_cast<size_t>(idx) > array_size) array_size = static_cast<size_t>(idx);
            array_version++;
            return;
        }
        if (idx == static_cast<int64_t>(array_size + 1)) {
            size_t new_cap = (array_cap == 0) ? 8 : array_cap * 2;
            TValue* new_arr = new TValue[new_cap]; ValueType* new_types = new ValueType[new_cap]();
            if (array_cap) { std::memcpy(new_arr, array, array_cap * sizeof(TValue)); std::memcpy(new_types, array_types, array_cap * sizeof(ValueType)); }
            if (!is_arena) { delete[] array; delete[] array_types; }
            is_arena = false;
            array = new_arr;
            array_types = new_types;
            array[array_size] = val.val; array_types[array_size] = val.type;
            array_size++;
            array_cap = new_cap;
            array_version++;
            if (hash_size > 0) {
                for (size_t i = 0; i < hash_size; ++i) {
                    if (entries[i].ktype == Nil) continue;
                    LValue kv(entries[i].key, entries[i].ktype);
                    int64_t hidx = -1;
                    if (kv.type == Int64) hidx = kv.as_integer();
                    else if (kv.type == Double) {
                        double d = kv.as_number(); int64_t t = static_cast<int64_t>(d);
                        if (d == static_cast<double>(t)) hidx = t;
                    }
                    if (hidx > 0 && hidx != idx && static_cast<uint64_t>(hidx - 1) < new_cap) {
                        array[hidx - 1] = entries[i].val; array_types[hidx - 1] = entries[i].vtype;
                        entries[i].key.payload.u64 = HASH_TOMBSTONE;
                        entries[i].ktype = Nil;
                        entries[i].val = TValue(); entries[i].vtype = Nil;
                        hash_count--;
                        hash_tombs++;
                        hash_version++;
                        if (hash_bitmap) hash_bitmap[i / 64] &= ~(1ULL << (i % 64));
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
                array[idx - 1] = val.val; array_types[idx - 1] = val.type;
                if (static_cast<size_t>(idx) > array_size) array_size = static_cast<size_t>(idx);
                array_version++;
                return;
            }
            if (idx == static_cast<int64_t>(array_size + 1)) {
                size_t new_cap = (array_cap == 0) ? 8 : array_cap * 2;
                TValue* new_arr = new TValue[new_cap]; ValueType* new_types = new ValueType[new_cap]();
                if (array_cap) { std::memcpy(new_arr, array, array_cap * sizeof(TValue)); std::memcpy(new_types, array_types, array_cap * sizeof(ValueType)); }
                if (!is_arena) { delete[] array; delete[] array_types; }
                is_arena = false;
                array = new_arr;
                array_types = new_types;
            array[array_size] = val.val; array_types[array_size] = val.type;
                array_size++;
                array_cap = new_cap;
                array_version++;
                if (hash_size > 0) {
                    for (size_t i = 0; i < hash_size; ++i) {
                        if (entries[i].ktype == Nil) continue;
                        LValue kv(entries[i].key, entries[i].ktype);
                        int64_t hidx = -1;
                        if (kv.type == Int64) hidx = kv.as_integer();
                        else if (kv.type == Double) {
                            double dd = kv.as_number(); int64_t t = static_cast<int64_t>(dd);
                            if (dd == static_cast<double>(t)) hidx = t;
                        }
                        if (hidx > 0 && hidx != idx && static_cast<uint64_t>(hidx - 1) < new_cap) {
                            array[hidx - 1] = entries[i].val; array_types[hidx - 1] = entries[i].vtype;
                            entries[i].key.payload.u64 = HASH_TOMBSTONE;
                            entries[i].ktype = Nil;
                            entries[i].val = TValue(); entries[i].vtype = Nil;
                            hash_count--;
                            hash_tombs++;
                            hash_version++;
                            if (hash_bitmap) hash_bitmap[i / 64] &= ~(1ULL << (i % 64));
                        }
                    }
                }
                return;
            }
        }
    }


    if (val.type == Nil) {
        if (hash_size == 0) return;
        uint32_t mask = static_cast<uint32_t>(hash_size - 1);
        uint64_t h = lvalue_hash(key) & mask;
        for (;;) {
            HashEntry& e = entries[h];
            if (e.ktype == Nil) {
                if (e.key.payload.u64 == HASH_EMPTY) return;
            } else if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) {
                e.key.payload.u64 = HASH_TOMBSTONE;
                e.ktype = Nil;
                e.val = TValue(); e.vtype = Nil;
                hash_count--;
                hash_tombs++;
                hash_version++;
                if (hash_bitmap) hash_bitmap[h / 64] &= ~(1ULL << (h % 64));
                return;
            }
            h = (h + 1) & mask;
        }
    }

    if (hash_size == 0) {
        resize_hash(8);
    } else if ((hash_count + hash_tombs + 1) * 4 >= hash_size * 3) {
        resize_hash(hash_count >= hash_size / 2 ? hash_size * 2 : hash_size);
    }

    uint32_t mask = static_cast<uint32_t>(hash_size - 1);
    uint64_t h    = lvalue_hash(key) & mask;
    int32_t  tomb = -1;
    for (;;) {
        HashEntry& e = entries[h];
        if (e.ktype == Nil) {
            HashEntry& slot = (tomb != -1) ? entries[tomb] : e;
            slot.key = key.val; slot.ktype = key.type;
            slot.val = val.val; slot.vtype = val.type;
            if (tomb != -1) hash_tombs--;
            hash_count++;
            hash_version++;
            if (hash_bitmap) {
                size_t bit_idx = (tomb != -1) ? static_cast<size_t>(tomb) : h;
                hash_bitmap[bit_idx / 64] |= (1ULL << (bit_idx % 64));
            }
            return;
        }
        if (e.key.payload.u64 == HASH_TOMBSTONE && e.ktype == Nil) {
            if (tomb == -1) tomb = static_cast<int32_t>(h);
        } else if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) {
            e.val = val.val; e.vtype = val.type;
            return;
        }
        h = (h + 1) & mask;
    }
}


//------------------ LTable::get_value — get with metamethod fallback
LValue LTable::get_value(LState* L, const LValue& key) {
    LValue ptr = gettable(key);
    if (ptr.type != Nil)
        return ptr;

    if (metatable) {
        LValue index_key = L->str_index;
        LValue index_ptr = metatable->gettable(index_key);

        if (index_ptr.type != Nil) {
            if (index_ptr.type == Table) {
                LTable* parent = static_cast<LTable*>(index_ptr.as_pointer());
                return parent->get_value(L, key);
            }
            else if (index_ptr.type == Function) {
                LValue args[2];
                args[0] = LValue(this);
                args[1] = key;

                L->shadow_stack[L->shadow_top++]= TypedSlot(&args[0].val, &args[0].type);
                L->shadow_stack[L->shadow_top++]= TypedSlot(&args[1].val, &args[1].type);
                MultiValue ret = call_function(L, index_ptr, args, 2, __FILE__, __LINE__);
                L->shadow_top -= 2;

                return ret.count > 0 ? ret[0] : LValue();
            }
        }
    }
    return LValue();
}


//------------------ LTable::set_value — set with metamethod fallback
void LTable::set_value(LState* L, const LValue& key, const LValue& val) {
    LValue ptr = gettable(key);
    if (ptr.type != Nil) {
        settable(key, val);
        return;
    }

    if (metatable) {
        LValue newindex_key = L->str_newindex;
        LValue newindex_ptr = metatable->gettable(newindex_key);

        if (newindex_ptr.type != Nil) {
            if (newindex_ptr.type == Table) {
                LTable* parent = static_cast<LTable*>(newindex_ptr.as_pointer());
                parent->set_value(L, key, val);
                return;
            }
            else if (newindex_ptr.type == Function) {
                LValue args[3];
                args[0] = LValue(this);
                args[1] = key;
                args[2] = val;

                L->shadow_stack[L->shadow_top++]= TypedSlot(&args[0].val, &args[0].type);
                L->shadow_stack[L->shadow_top++]= TypedSlot(&args[1].val, &args[1].type);
                L->shadow_stack[L->shadow_top++]= TypedSlot(&args[2].val, &args[2].type);
                call_function(L, newindex_ptr, args, 3, __FILE__, __LINE__);
                L->shadow_top -= 3;
                return;
            }
        }
    }
    settable(key, val);
}


//------------------ LTable::bind — bind constant value
void LTable::bind(const char* name, const LValue& val) {
    settable(LValue(name), val);
}


//------------------ LTable::bind — bind C function
void LTable::bind(LState* L, const char* name, CFunctionType func) {
    LCFunction* f = new LCFunction(func);
    f->next = L->allocated_objects;
    L->allocated_objects = f;
    settable(LValue(L->intern_string(name)), LValue(Function, f));
}


//------------------ LTable::bind_all — bind multiple C functions
void LTable::bind_all(LState* L, std::initializer_list<LReg> funcs) {
    for (const auto& reg : funcs)
        bind(L, reg.name, reg.func);
}


//------------------ LState::LState — state constructor
LState::LState()
    : allocated_objects(nullptr), free_tables(nullptr), free_functions(nullptr), finalizable_ud(nullptr),
      shadow_top(0), current_file(""), current_line(0),
      object_count(0), gc_bytes_threshold(2 * 1024 * 1024), string_metatable(nullptr)
{
    int dummy = 0;
    stack_bottom = &dummy;
    _G = new LTable();
    allocated_bytes += sizeof(LTable);
    _G->next = allocated_objects;
    allocated_objects = _G;
    object_count++;
    _G->settable(LValue(intern_string("_G")), LValue(Table, _G));


    str_index    = LValue(intern_string("__index"));
    str_newindex = LValue(intern_string("__newindex"));
    str_gc       = LValue(intern_string("__gc"));
    str_call     = LValue(intern_string("__call"));
    str_close    = LValue(intern_string("__close"));
    str_pairs    = LValue(intern_string("__pairs"));
    str_tostring = LValue(intern_string("__tostring"));
}


static void dtor_free_table(LTable* t) {
    if (t->array) { delete[] t->array; t->array = nullptr; } if (t->array_types) { delete[] t->array_types; t->array_types = nullptr; }
    if (t->entries) { delete[] t->entries; t->entries = nullptr; }
    if (t->hash_bitmap) { delete[] t->hash_bitmap; t->hash_bitmap = nullptr; }
    t->array_size = t->array_cap = t->hash_size = t->hash_count = 0;
    t->hash_count = t->hash_tombs = 0;
    delete t;
}

//------------------ LState::~LState — state destructor
LState::~LState() {
    if (gc_phase == GCPhase::Sweeping) {
        while (gc_phase == GCPhase::Sweeping)
            gc_step();
    }

    for (LHeader* h = allocated_objects; h; h = h->next) {
        if (h->type == static_cast<uint8_t>(UserData)) {
            LUserdata* ud = static_cast<LUserdata*>(h);
            if (ud->metatable) {
                LValue gc_func = ud->metatable->gettable(this->str_gc);
                if (gc_func.type != Nil && gc_func.type == Function) {
                    LValue args[1] = { LValue(UserData, ud) };
                    size_t prev_shadow = this->shadow_top;
                    this->shadow_stack[this->shadow_top++]= TypedSlot(&args[0].val, &args[0].type);
                    try {
                        call_function(this, gc_func, args, 1, "StateClose_Finalizer", 0);
                    } catch (...) {}
                    this->shadow_top = prev_shadow;
                }
            }
        }
    }

    LHeader* curr = allocated_objects;
    while (curr) {
        LHeader* next = curr->next;
        if (curr->type == static_cast<uint8_t>(Table))
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
            if (ud->metatable) {
                LValue gc_func = ud->metatable->gettable(this->str_gc);
                if (gc_func.type != Nil && gc_func.type == Function) {
                    LValue args[1] = { LValue(UserData, ud) };
                    size_t prev_shadow = this->shadow_top;
                    this->shadow_stack[this->shadow_top++]= TypedSlot(&args[0].val, &args[0].type);
                    try {
                        call_function(this, gc_func, args, 1, "GC_Finalizer", 0);
                    } catch (...) {}
                    this->shadow_top = prev_shadow;
                }
            }
            delete[] reinterpret_cast<char*>(ud);
            ud = nxt;
        }
    }
    gc_finalizable_ud = nullptr;

    LTable* ft = free_tables;
    while (ft) {
        LTable* next = static_cast<LTable*>(ft->next);
        delete ft;
        ft = next;
    }
    free_tables = nullptr;

    LCFunction* ff = free_functions;
    while (ff) {
        LCFunction* next = static_cast<LCFunction*>(ff->next);
        delete ff;
        ff = next;
    }
    free_functions = nullptr;
}












static void clx_trigger_gc(LState* L, LTable* t) {
    if (!t->metatable) return;
    LValue gc_func = t->metatable->gettable(L->str_gc);
    if (gc_func.type == Nil) return;

    LValue args[1] = { LValue(Table, t) };
    size_t prev_shadow = L->shadow_top;
    L->shadow_stack[L->shadow_top++]= TypedSlot(&args[0].val, &args[0].type);
    try {
        call_function(L, gc_func, args, 1, "GC_Finalizer", 0);
    } catch (const LRuntimeException& e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (std::exception& e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "error in __gc metamethod\n";
    }
    L->shadow_top = prev_shadow;
}

//------------------ CloseGuard::~CloseGuard — close guard destructor
CloseGuard::~CloseGuard() {
    if (val.type != Table) return;
    LTable* t = static_cast<LTable*>(val.as_pointer());
    if (!t->metatable) return;
    LValue close_func = t->metatable->gettable(L->str_close);
    if (close_func.type == Nil) return;

    LValue args[2] = { val, LValue() };
    size_t prev_shadow = L->shadow_top;
    L->shadow_stack[L->shadow_top++]= TypedSlot(&args[0].val, &args[0].type);
    L->shadow_stack[L->shadow_top++]= TypedSlot(&args[1].val, &args[1].type);
    try {
        call_function(L, close_func, args, 2, "CloseGuard", 0);
    } catch (const LRuntimeException& e) {
        std::cerr << "error in __close metamethod: " << e.what() << "\n";
    } catch (std::exception& e) {
        std::cerr << "error in __close metamethod: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "error in __close metamethod\n";
    }
    L->shadow_top = prev_shadow;
}


//------------------ LState::gc_step — incremental GC sweep step
bool LState::gc_step() {
    if (gc_phase != GCPhase::Sweeping) return true;

    size_t budget = GC_STEP_BUDGET;
    LHeader* curr = gc_sweep_cursor;
    LHeader* prev = gc_prev;

    while (curr && budget--) {
        LHeader* next_obj = curr->next;
        if (curr->marked == 0) {
            if (prev) {
                prev->next = next_obj;
            } else {
                if (curr != allocated_objects) {
                    LHeader* h = allocated_objects;
                    while (h && h->next != curr)
                        h = h->next;
                    if (h) h->next = next_obj;
                } else {
                    allocated_objects = next_obj;
                }
            }
            if (curr->type == static_cast<uint8_t>(Table)) {
                LTable* t = static_cast<LTable*>(curr);
                if (t->metatable) {
                    t->next = gc_finalizable;
                    gc_finalizable = t;
                } else {
                    if (t->array_cap > 0) allocated_bytes -= sizeof(TValue) * t->array_cap + sizeof(ValueType) * t->array_cap;
                    if (t->array) { delete[] t->array; t->array = nullptr; } if (t->array_types) { delete[] t->array_types; t->array_types = nullptr; }
                    if (t->entries) { delete[] t->entries; t->entries = nullptr; }
                    if (t->hash_bitmap) { delete[] t->hash_bitmap; t->hash_bitmap = nullptr; }
                                    t->array_size = t->array_cap = t->hash_size = t->hash_count = 0;
                    t->hash_count = t->hash_tombs = 0;
                    t->next = free_tables;
                    free_tables = t;
                }
            } else if (curr->type == static_cast<uint8_t>(Function)) {
                LCFunction* f = static_cast<LCFunction*>(curr);
                f->func = nullptr;
                f->next = free_functions;
                free_functions = f;
            } else if (curr->type == static_cast<uint8_t>(Thread)) {
                allocated_bytes -= sizeof(LThread);
                delete static_cast<LThread*>(curr);
            } else if (curr->type == static_cast<uint8_t>(UserData)) {
                LUserdata* ud = static_cast<LUserdata*>(curr);
                if (ud->metatable) {
                    ud->next = gc_finalizable_ud;
                    gc_finalizable_ud = ud;
                } else {
                    allocated_bytes -= sizeof(LUserdata) + ud->size;
                    delete[] reinterpret_cast<char*>(ud);
                }
            }
            curr = next_obj;
        } else {
            if (curr->marked == 2) curr->marked = 0;
            else curr->marked = 0;
            object_count++;
            prev = curr;
            curr = next_obj;
        }
    }

    gc_sweep_cursor = curr;
    gc_prev = prev;

    if (!gc_sweep_cursor) {
        for (LTable* t = static_cast<LTable*>(gc_finalizable); t; ) {
            LTable* nx = static_cast<LTable*>(t->next);
            clx_trigger_gc(this, t);
            if (t->array_cap > 0) allocated_bytes -= sizeof(TValue) * t->array_cap + sizeof(ValueType) * t->array_cap;
            if (t->array) { delete[] t->array; t->array = nullptr; } if (t->array_types) { delete[] t->array_types; t->array_types = nullptr; }
            if (t->entries) { delete[] t->entries; t->entries = nullptr; }
                    t->array_size = t->array_cap = t->hash_size = t->hash_count = 0;
            t->hash_count = t->hash_tombs = 0;
            t->next = free_tables;
            free_tables = t;
            t = nx;
        }
        gc_finalizable = nullptr;
        for (LUserdata* ud = static_cast<LUserdata*>(gc_finalizable_ud); ud; ) {
            LUserdata* nx = static_cast<LUserdata*>(ud->next);
            if (ud->metatable) {
                LValue gc_func = ud->metatable->gettable(this->str_gc);
                if (gc_func.type != Nil && gc_func.type == Function) {
                    LValue args[1] = { LValue(UserData, ud) };
                    size_t prev_shadow = this->shadow_top;
                    this->shadow_stack[this->shadow_top++]= TypedSlot(&args[0].val, &args[0].type);
                    try {
                        call_function(this, gc_func, args, 1, "GC_Finalizer", 0);
                    } catch (const LRuntimeException& e) {
                        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
                    } catch (std::exception& e) {
                        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
                    } catch (...) {
                        std::cerr << "error in __gc metamethod\n";
                    }
                    this->shadow_top = prev_shadow;
                }
            }
            allocated_bytes -= sizeof(LUserdata) + ud->size;
            delete[] reinterpret_cast<char*>(ud);
            ud = nx;
        }
        gc_finalizable_ud = nullptr;
        gc_prev = nullptr;
        gc_phase = GCPhase::Idle;
        size_t live = allocated_bytes;
        size_t headroom = std::clamp(live / 2, size_t(512 * 1024), size_t(8 * 1024 * 1024));
        gc_bytes_threshold = live + headroom;
        return true;
    }
    return false;
}


//------------------ LState::collect_garbage — mark-sweep collection
void LState::collect_garbage() {
    auto& wl = gc_worklist;
    wl.clear();

    auto push_if_needed = [&](LValue v) {
        if (!v.is_gc_obj()) return;
        LHeader* h = v.as_pointer();
        if (h && h->marked == 0) {
        h->marked = 1;
        ValueType t = v.type;
        if (t == Table || t == Thread || t == Function) wl.push_back(h);
        }
    };

    if (_G)          push_if_needed(LValue(Table,  _G));
    if (main_thread) push_if_needed(LValue(Thread, main_thread));
    if (running_thread && running_thread != main_thread)
        push_if_needed(LValue(Thread, running_thread));
    for (size_t i = 0; i < shadow_top; ++i)
        if (shadow_stack[i].val) push_if_needed(LValue(*shadow_stack[i].val, *shadow_stack[i].type));

    while (!wl.empty()) {
        LHeader* curr = wl.back(); wl.pop_back();

        if (curr->type == static_cast<uint8_t>(Table)) {
            LTable* t = static_cast<LTable*>(curr);
            // SIMD: scan type array in 32/16-byte chunks, only process non-nil entries
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
                // Remainder as 16-byte SSE2
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
                        if (lane_vals[k]) push_if_needed(LValue(t->array[i + k], t->array_types[i + k]));
                    }
                }
#endif
                for (; i < t->array_size; ++i) push_if_needed(LValue(t->array[i], t->array_types[i]));
            }
            if (t->hash_bitmap) {
                size_t bm_words = (t->hash_size + 63) / 64;
                for (size_t word = 0; word < bm_words; ++word) {
                    uint64_t bits = t->hash_bitmap[word];
                    while (bits) {
                        size_t idx = word * 64 + clx_ctzll(bits);
                        if (idx >= t->hash_size) break;
                        LValue kv(t->entries[idx].key, t->entries[idx].ktype);
                        push_if_needed(kv);
                        push_if_needed(LValue(t->entries[idx].val, t->entries[idx].vtype));
                        bits &= bits - 1;
                    }
                }
            } else {
                for (size_t _i = 0; _i < t->hash_size; ++_i) {
                    if (t->entries[_i].ktype == Nil) continue;
                    LValue kv(t->entries[_i].key, t->entries[_i].ktype);
                    push_if_needed(kv);
                    push_if_needed(LValue(t->entries[_i].val, t->entries[_i].vtype));
                }
            }
            if (t->metatable && t->metatable->marked == 0) {
                t->metatable->marked = 1;
                wl.push_back(t->metatable);
            }
        } else if (curr->type == static_cast<uint8_t>(Thread)) {
            LThread* th = static_cast<LThread*>(curr);
            push_if_needed(th->function);
            for (size_t i = 0; i < th->yield_args.count;  ++i) push_if_needed(th->yield_args[i]);
            for (size_t i = 0; i < th->resume_args.count; ++i) push_if_needed(th->resume_args[i]);
        } else if (curr->type == static_cast<uint8_t>(Function)) {
            LCFunction* f = static_cast<LCFunction*>(curr);
            if (f->env) push_if_needed(LValue(Table, f->env));
        }
    }

    std::vector<LHeader*> protect_wl;
    for (LHeader* obj = allocated_objects; obj; obj = obj->next) {
        if (obj->marked == 0 && obj->type == static_cast<uint8_t>(Table)) {
            LTable* t = static_cast<LTable*>(obj);
            if (t->metatable && t->metatable->marked == 0) {
                t->metatable->marked = 2;
                protect_wl.push_back(t->metatable);
            }
        }
    }
    while (!protect_wl.empty()) {
        LHeader* curr = protect_wl.back(); protect_wl.pop_back();
        if (curr->type == static_cast<uint8_t>(Table)) {
            LTable* tt = static_cast<LTable*>(curr);
            // SIMD: scan array type tags, skip nil entries
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
                        if (v.is_gc_obj()) {
                            LHeader* h = v.as_pointer();
                            if (h && h->marked == 0) { h->marked = 2; protect_wl.push_back(h); }
                        }
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
                        if (v.is_gc_obj()) {
                            LHeader* h = v.as_pointer();
                            if (h && h->marked == 0) { h->marked = 2; protect_wl.push_back(h); }
                        }
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
                        if (v.is_gc_obj()) {
                            LHeader* h = v.as_pointer();
                            if (h && h->marked == 0) { h->marked = 2; protect_wl.push_back(h); }
                        }
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
                            if (v.is_gc_obj()) {
                                LHeader* h = v.as_pointer();
                                if (h && h->marked == 0) { h->marked = 2; protect_wl.push_back(h); }
                            }
                        }
                    }
                }
#endif
                for (; i < tt->array_size; ++i) {
                    LValue v = LValue(tt->array[i], tt->array_types[i]);
                    if (v.is_gc_obj()) {
                        LHeader* h = v.as_pointer();
                        if (h && h->marked == 0) { h->marked = 2; protect_wl.push_back(h); }
                    }
                }
            }
            if (tt->hash_bitmap) {
                size_t bm_words = (tt->hash_size + 63) / 64;
                for (size_t word = 0; word < bm_words; ++word) {
                    uint64_t bits = tt->hash_bitmap[word];
                    while (bits) {
                        size_t idx = word * 64 + clx_ctzll(bits);
                        if (idx >= tt->hash_size) break;
                        LValue kv(tt->entries[idx].key, tt->entries[idx].ktype);
                        for (LValue v : { kv, LValue(tt->entries[idx].val, tt->entries[idx].vtype) }) {
                            if (v.is_gc_obj()) {
                                LHeader* h = v.as_pointer();
                                if (h && h->marked == 0) { h->marked = 2; protect_wl.push_back(h); }
                            }
                        }
                        bits &= bits - 1;
                    }
                }
            } else {
                for (size_t _pi = 0; _pi < tt->hash_size; ++_pi) {
                    if (tt->entries[_pi].ktype == Nil) continue;
                    LValue kv(tt->entries[_pi].key, tt->entries[_pi].ktype);
                    for (LValue v : { kv, LValue(tt->entries[_pi].val, tt->entries[_pi].vtype) }) {
                        if (v.is_gc_obj()) {
                            LHeader* h = v.as_pointer();
                            if (h && h->marked == 0) { h->marked = 2; protect_wl.push_back(h); }
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
    gc_step();
}

//------------------ LState::register_module — register module loader
void LState::register_module(const std::string& name, LValue (*func)(LState*)) {
    register_static_preload(this, name.c_str(), func);
}


//------------------ call_function — call a value as function
MultiValue call_function(LState* L, const LValue& func, const LValue* args, size_t count, const char* file, int line) {
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
        LTable* mt = static_cast<LTable*>(func.as_pointer())->metatable;
        if (mt) {
            LValue m = mt->gettable(L->str_call);
            if (m.type != Nil) {
                size_t nargs = count + 1;
                LValue* new_args;
                LValue stack_buf[16];
                bool heap = nargs > 16;
                if (heap) new_args = new LValue[nargs];
                else new_args = stack_buf;
                new_args[0] = func;
                for (size_t i = 0; i < count; ++i) new_args[i + 1] = args[i];

                size_t prev_inner = L->shadow_top;
                for (size_t i = 0; i < nargs; ++i) L->shadow_stack[L->shadow_top++] = TypedSlot(&new_args[i].val, &new_args[i].type);
                MultiValue ret = call_function(L, m, new_args, nargs, file, line);
                L->shadow_top = prev_inner;
                if (heap) delete[] new_args;
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
MultiValue pcall_function(LState* L, const LValue& func, const LValue* args, size_t count) {
    try {
        MultiValue ret = call_function(L, func, args, count, L->current_file, L->current_line);
        if (ret.count == 0) return MultiValue({ LValue(true) });
        if (ret.count == 1) return MultiValue({ LValue(true), ret[0] });
        std::vector<LValue> results;
        results.reserve(ret.count + 1);
        results.push_back(LValue(true));
        for (size_t i = 0; i < ret.count; ++i)
            results.push_back(ret[i]);
        return MultiValue(results);
    } catch (const LRuntimeException& e) {
        return MultiValue({ LValue(false), e.error_obj });
    } catch (const std::exception& e) {
        return MultiValue({ LValue(false), LValue(L->intern_string(e.what())) });
    }
}






//------------------ call_direct — fast path for LCFunction direct calls
MultiValue call_direct(LState* L, const LValue& func, const LValue* args, size_t count, const char* file, int line) {
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

//------------------ getmetafield — get metatable field
LValue getmetafield(LState* L, const LValue& obj, const char* field) {
    LTable* mt = nullptr;
    if (obj.type == Table) {
        mt = static_cast<LTable*>(obj.as_pointer())->metatable;
    } else if (obj.type == UserData) {
        mt = static_cast<LUserdata*>(obj.as_pointer())->metatable;
    }
    if (!mt) return LValue();
    LValue p = mt->gettable(LValue(L->intern_string(field)));
    return p.type != Nil ? p : LValue();
}

//------------------ callmeta — call metamethod
bool callmeta(LState* L, const LValue& obj, const char* event) {
    LTable* mt = nullptr;
    if (obj.type == Table) {
        mt = static_cast<LTable*>(obj.as_pointer())->metatable;
    } else if (obj.type == UserData) {
        mt = static_cast<LUserdata*>(obj.as_pointer())->metatable;
    }
    if (!mt) return false;
    LValue p = mt->gettable(LValue(L->intern_string(event)));
    if (p.type == Nil) return false;
    LValue args[1] = {obj};
    call_function(L,  p, args, 1, "C API", 0);
    return true;
}






static MultiValue lazy_funcs_index(LState* L, const LValue* args, size_t n) {
    if (n < 2 || args[1].type != String)
        return MultiValue();

    const char* name = args[1].as_string();
    LTable* t = static_cast<LTable*>(args[0].as_pointer());
    if (!t->metatable) return MultiValue();

    LValue regs_key(L->intern_string("__lazy_regs"));
    LValue count_key(L->intern_string("__lazy_count"));

    LValue regs_val = t->metatable->gettable(regs_key);
    LValue count_val = t->metatable->gettable(count_key);
    if (regs_val.type != UserData || regs_val.type == Nil || count_val.type == Nil || count_val.type != Int64)
        return MultiValue();

    const LazyReg* regs = reinterpret_cast<const LazyReg*>(regs_val.as_pointer());
    int64_t count = count_val.as_integer();

    for (int64_t i = 0; i < count; i++) {
        if (std::strcmp(regs[i].name, name) == 0) {
            LValue func = L->create_closure(CFunctionType(regs[i].func));
            t->settable(args[1], func);
            return MultiValue(func);
        }
    }

    return MultiValue();
}


//------------------ set_lazy_funcs — set lazy function registrations
void set_lazy_funcs(LState* L, const LValue& table, const LazyReg* regs, size_t count) {
    if (table.type != Table) return;
    LTable* t = static_cast<LTable*>(table.as_pointer());

    LTable* mt = t->metatable;
    if (!mt) {
        mt = static_cast<LTable*>(L->create_table().as_pointer());
        t->metatable = mt;
        t->hash_version++;
    }

    mt->settable(LValue(L->intern_string("__lazy_regs")),
                 LValue(UserData, reinterpret_cast<LHeader*>(const_cast<LazyReg*>(regs))));
    mt->settable(LValue(L->intern_string("__lazy_count")),
                 LValue(static_cast<int64_t>(count)));

    LValue existing = mt->gettable(L->str_index);
    if (existing.type == Nil) {
        LValue handler = L->create_closure(CFunctionType(lazy_funcs_index));
        mt->settable(L->str_index, handler);
    }
}



//------------------ LState::create_table — allocate a table
LValue LState::create_table(size_t asize, size_t hsize) {
    if (gc_running) {
        if (gc_phase == GCPhase::Sweeping) {
            gc_step();
        } else if (allocated_bytes >= gc_bytes_threshold) {
            collect_garbage();
        }
    }

    LTable* t;
    if (free_tables) {
        t = free_tables;
        free_tables = static_cast<LTable*>(free_tables->next);
        t->array_size  = 0;
        t->array_cap   = 0;
        t->hash_size   = 0;
        t->hash_count  = 0;
        t->hash_count    = 0;
        t->hash_tombs    = 0;
        t->hash_version  = 0;
        t->array_version = 0;
        t->metatable   = nullptr;
        t->marked      = 0;
    } else {
        t = new LTable();
        allocated_bytes += sizeof(LTable);
    }

    if (asize > 0) {
        t->array = new TValue[asize]; t->array_types = new ValueType[asize]();
        t->array_size = asize;
        t->array_cap = asize;
        allocated_bytes += sizeof(TValue) * asize + sizeof(ValueType) * asize;
    }

    t->type = static_cast<uint8_t>(Table);

    t->next = allocated_objects;
    allocated_objects = t;

    object_count++;
    return LValue(Table, t);
}

//------------------ LState::create_closure — allocate a closure
clx::LValue clx::LState::create_closure(CFunctionType func, LTable* env) {
    if (gc_running) {
        if (gc_phase == GCPhase::Sweeping) {
            gc_step();
        } else if (allocated_bytes >= gc_bytes_threshold) {
            collect_garbage();
        }
    }

    LCFunction* f;
    if (free_functions) {
        f = free_functions;
        free_functions = static_cast<LCFunction*>(free_functions->next);
        f->func = std::move(func);
        f->env = env ? env : _G;
        f->marked = 0;
    } else {
        f = new LCFunction(std::move(func));
        f->env = env ? env : _G;
        allocated_bytes += sizeof(LCFunction);
    }

    auto* fnptr = f->func.target<MultiValue(*)(LState*, const LValue*, size_t)>();
    if (fnptr) { f->direct = *fnptr; }
    f->type = static_cast<uint8_t>(Function);
    f->next = allocated_objects;
    allocated_objects = f;
    object_count++;
    return clx::LValue(Function, f);
}

//------------------ newuserdata — allocate userdata
LValue newuserdata(LState* L, size_t size) {
    if (L->gc_running) {
        if (L->gc_phase == LState::GCPhase::Sweeping) {
            L->gc_step();
        } else if (L->allocated_bytes >= L->gc_bytes_threshold) {
            L->collect_garbage();
        }
    }

    char* mem = new char[sizeof(LUserdata) + size];
    LUserdata* ud = reinterpret_cast<LUserdata*>(mem);
    ud->type = static_cast<uint8_t>(UserData);
    ud->marked = 0;
    ud->metatable = nullptr;
    ud->size = size;

    ud->next = L->allocated_objects;
    L->allocated_objects = ud;
    L->object_count++;
    L->allocated_bytes += sizeof(LUserdata) + size;
    return LValue(UserData, ud);
}



//------------------ call_bin_metamethod — call binary op metamethod
LValue call_bin_metamethod(LState* L, const LValue& a, const LValue& b, const char* event) {
    LTable* mt = nullptr;
    if (a.type == Table) mt = static_cast<LTable*>(a.as_pointer())->metatable;
    else if (a.type == UserData) mt = static_cast<LUserdata*>(a.as_pointer())->metatable;
    if (!mt && b.type == Table) mt = static_cast<LTable*>(b.as_pointer())->metatable;
    else if (!mt && b.type == UserData) mt = static_cast<LUserdata*>(b.as_pointer())->metatable;

    if (mt) {
        LValue method = mt->gettable(LValue(L->intern_string(event)));
        if (method.type != Nil && method.type == Function) {
            LValue args[2] = { a, b };
            L->shadow_stack[L->shadow_top++]= TypedSlot(&args[0].val, &args[0].type);
            L->shadow_stack[L->shadow_top++]= TypedSlot(&args[1].val, &args[1].type);
            MultiValue res = call_function(L, method, args, 2, __FILE__, __LINE__);
            L->shadow_top -= 2;
            return res[0];
        }
    }

    std::string prefix = "";
    if (L->current_file && L->current_file[0] != '\0') {
        prefix = std::string(L->current_file) + ":" + std::to_string(L->current_line) + ": ";
    }

    std::string_view ev(event);

    if (ev == "__eq") return clx::LValue(false);

    std::string op_type = "perform arithmetic on";
    if (ev == "__lt" || ev == "__le") op_type = "compare";
    else if (ev == "__concat") op_type = "concatenate";

    std::string err_msg = prefix + "attempt to " + op_type + " a " + VALUE_TYPE_NAMES[static_cast<size_t>(a.type)] + " value";
    throw LRuntimeException(LValue(L->intern_string(err_msg)));
}






//------------------ get_global — get global variable
LValue get_global(LState* L, const char* name) {
    LValue val = L->_G->gettable(LValue(L->intern_string(name)));
    return val.type != Nil ? val : LValue();
}


//------------------ set_global — set global variable
void set_global(LState* L, const char* name, const LValue& val) {
    L->_G->settable(LValue(L->intern_string(name)), val);
}


//------------------ open — create Lua state
LState* open(int argc, char* argv[]) {
    LState* L = new LState();

    LThread* main_th = new LThread();
    main_th->state = L;
    main_th->is_main = true;
    main_th->status = THREAD_RUNNING;
#if defined(_WIN32)
    main_th->fiber = ConvertThreadToFiber(nullptr);
#elif defined(__APPLE__) && defined(__aarch64__)
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
void close(LState* L) {
#if defined(_WIN32)
    ConvertFiberToThread();
#endif
    delete L->main_thread;
    delete L;
}

}
