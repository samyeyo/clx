// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  gc.cpp · Generational garbage collector    │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include "clx_runtime.h"
#include "internal.h"
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

void (*clx_mark_vm_proxies_ptr)(LState *clx_L, LState::GCStack &wl) = nullptr;
void (*clx_free_vm_proxy_ptr)(LState *clx_L, LHeader *proxy) = nullptr;

static void clx_trigger_gc(LState *L, LTable *t) {
    LTable *mt = tbl_metatable(t);
    if (!mt)
        return;
    LValue gc_func = mt->gettable(L->str_gc);
    if (gc_func.type == Nil)
        return;

    LValue args[1] = { LValue(Table, t) };
    try {
        static constexpr CallSite finalizer_site = { "GC_Finalizer", 0, false, false };
        call_function_rooted(L, gc_func, args, 1, &finalizer_site);
    } catch (const LRuntimeException &e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (std::exception &e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "error in __gc metamethod\n";
    }
}

#define GC_SUB(TAG, PTR, amt)                                                                                          \
    do {                                                                                                               \
        if (static_cast<size_t>(amt) > allocated_bytes)                                                                \
            allocated_bytes = 0;                                                                                       \
        else                                                                                                           \
            allocated_bytes -= (amt);                                                                                  \
    } while (0)

//------------------ shared sweep helpers — used by both the incremental major sweep (gc_step)

static void gc_dispose_swept(LState *L, LHeader *curr) {
    auto &GC_SUB = L->allocated_bytes;
    (void)GC_SUB;
    curr->flags &= ~(LFLAG_REMEMBERED | LFLAG_GC_PIN);
    //------------------ Old-byte accounting only changes when an OLD object dies (to is always YOUNG
    //------------------ here); skip the type-switch + size computation for young/survivor garbage.
    if (curr->age == AGE_OLD)
        L->gc_update_old_bytes(curr, AGE_OLD, AGE_YOUNG);
    if (curr->flags & LFLAG_VM_PROXY) {
        if (clx_free_vm_proxy_ptr)
            clx_free_vm_proxy_ptr(L, curr);
    } else if (curr->type == static_cast<uint8_t>(Table)) {
        LTable *t = static_cast<LTable *>(curr);
        if (tbl_metatable(t)) {
            t->flags |= LFLAG_FINALIZABLE;
            t->next = L->gc_finalizable;
            L->gc_finalizable = t;
        } else {
            const size_t tbl_bytes = sizeof(LTable) + table_heap_bytes(t);
            if (tbl_bytes > L->allocated_bytes)
                L->allocated_bytes = 0;
            else
                L->allocated_bytes -= tbl_bytes;
            if (t->ext) {
                t->ext->hash_count = 0;
                t->ext->hash_tombs = 0;
            }
            t->array_size = 0;
            t->array_lim = 0;
            if (t->array && t->array != t->small_array) {
                t->next = L->free_tables_big;
                L->free_tables_big = t;
            } else {
                t->next = L->free_tables;
                L->free_tables = t;
            }
        }
    } else if (curr->type == static_cast<uint8_t>(Function)) {
        LCFunction *f = static_cast<LCFunction *>(curr);
        f->func = nullptr;
        f->direct = nullptr;
        f->env = nullptr;
        f->self_ref = LValue();
        f->gc_cells.clear();
        f->next = L->free_functions;
        L->free_functions = f;
    } else if (curr->type == static_cast<uint8_t>(Thread)) {
        LThread *th = static_cast<LThread *>(curr);
        size_t th_bytes = sizeof(LThread) + th->stack_bytes;
        if (th_bytes > L->allocated_bytes)
            L->allocated_bytes = 0;
        else
            L->allocated_bytes -= th_bytes;
#if defined(_WIN32)
        if (th->fiber && L->free_fiber_threads < kMaxPooledFibers) {
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
        LUserdata *ud = static_cast<LUserdata *>(curr);
        if (ud->metatable) {
            ud->next = L->gc_finalizable_ud;
            L->gc_finalizable_ud = ud;
        } else {
            size_t ud_bytes = sizeof(LUserdata) + ud->size;
            if (ud_bytes > L->allocated_bytes)
                L->allocated_bytes = 0;
            else
                L->allocated_bytes -= ud_bytes;
            delete[] reinterpret_cast<char *>(ud);
        }
    }
}

//------------------ gc_prune_meta_list: unlink dead metatabled tables in one O(N) pass; per-table removal would be O(M*N).

static void gc_prune_meta_list(LState *L) {
    LTable **pp = &L->metatabled_tables;
    while (*pp) {
        LTable *n = *pp;
        if (n->flags & LFLAG_FINALIZABLE) {
            *pp = n->ext->meta_next;
            n->ext->meta_next = nullptr;
            n->flags &= ~(LFLAG_META_LIST | LFLAG_FINALIZABLE);
        } else {
            pp = &n->ext->meta_next;
        }
    }
}

static void gc_drain_finalizables(LState *L) {
    gc_prune_meta_list(L);
    for (LTable *t = static_cast<LTable *>(L->gc_finalizable); t;) {
        LTable *nx = static_cast<LTable *>(t->next);
        meta_list_remove(L, t);
        clx_trigger_gc(L, t);
        size_t t_bytes = sizeof(LTable) + table_heap_bytes(t);
        if (t_bytes > L->allocated_bytes)
            L->allocated_bytes = 0;
        else
            L->allocated_bytes -= t_bytes;
        table_free_array(t->array, t->array_types, t->small_array);
        if (t->ext) {
            table_free_hash(t->ext->entries, t->ext->hash_bitmap);
            t->ext->hash_count = 0;
            t->ext->hash_tombs = 0;
            t->ext->hash_size = 0;
            t->ext->metatable = nullptr;
            t->ext->meta_next = nullptr;
        }
        t->array_size = t->array_cap = 0;
        t->array_lim = 0;
        t->flags &= ~(LFLAG_META_LIST | LFLAG_FINALIZABLE);
        if (t->array && t->array != t->small_array) {
            t->next = L->free_tables_big;
            L->free_tables_big = t;
        } else {
            t->next = L->free_tables;
            L->free_tables = t;
        }
        t = nx;
    }
    L->gc_finalizable = nullptr;
    for (LUserdata *ud = static_cast<LUserdata *>(L->gc_finalizable_ud); ud;) {
        LUserdata *nx = static_cast<LUserdata *>(ud->next);
        meta_ud_list_remove(L, ud);
        L->invoke_gc_finalizer(ud, "GC_Finalizer");
        size_t ud_bytes = sizeof(LUserdata) + ud->size;
        if (ud_bytes > L->allocated_bytes)
            L->allocated_bytes = 0;
        else
            L->allocated_bytes -= ud_bytes;
        delete[] reinterpret_cast<char *>(ud);
        ud = nx;
    }
    L->gc_finalizable_ud = nullptr;
}

//------------------ LState::gc_remember — record an old-generation owner for the next minor
void LState::gc_remember(LHeader *owner) {
    if (owner->flags & LFLAG_REMEMBERED)
        return;
    owner->flags |= LFLAG_REMEMBERED;
    gc_remembered.push_back(owner);
}

//------------------ GCStack growth (cold) + reserve/free
void LState::gc_stack_grow(LState::GCStack *s) {
    size_t ncap = s->cap ? s->cap * 2 : 1024;
    LHeader **nd = static_cast<LHeader **>(std::realloc(s->data, ncap * sizeof(LHeader *)));
    if (!nd)
        throw std::bad_alloc();
    s->data = nd;
    s->cap = ncap;
}

void LState::GCStack::reserve(size_t n) {
    if (n <= cap)
        return;
    LHeader **nd = static_cast<LHeader **>(std::realloc(data, n * sizeof(LHeader *)));
    if (!nd)
        throw std::bad_alloc();
    data = nd;
    cap = n;
}

void LState::GCStack::free() {
    std::free(data);
    data = nullptr;
    top = cap = 0;
}

//------------------ LState::gc_remember_cell — record an upvalue cell for the next minor
void LState::gc_remember_cell(const LUpValue &cell) {
    if (!cell)
        return;
    if (!gc_remembered_cell_set.insert(cell.get()).second)
        return;
    gc_remembered_cells.push_back(cell);
}

//------------------ gc_barrier_header — slow path of the write barrier
void gc_barrier_header(LState *L, LHeader *owner, const LValue &newval) {
    if (!newval.is_gc_obj())
        return;
    LHeader *nv = static_cast<LHeader *>(newval.as_pointer());
    if (!nv || nv->age == AGE_OLD)
        return;
    if (owner->age == AGE_OLD || owner->age == AGE_SURVIVOR)
        L->gc_remember(owner);
}

//------------------ gc_barrier_cell — upvalue cell write barrier
void gc_barrier_cell(LState *L, const LUpValue &cell, const LValue &newval) {
    (void)L;
    if (!newval.is_gc_obj())
        return;
    LHeader *nv = static_cast<LHeader *>(newval.as_pointer());
    if (!nv || nv->age == AGE_OLD)
        return;
    L->gc_remember_cell(cell);
}

//------------------ mark helpers shared by major and minor marking

//------------------ chain_contains — membership check on one GC chain
static bool chain_contains(LHeader *head, const LHeader *h) {
    for (LHeader *o = head; o; o = o->next)
        if (o == h)
            return true;
    return false;
}

static CLX_INLINE_HOT LHeader *gc_mark_value(LState *L, const LValue &v, uint8_t markval) {
    if (!v.is_gc_obj())
        return nullptr;
    LHeader *h = v.as_pointer();
    if (!h)
        return nullptr;
    if (h->type != static_cast<uint8_t>(v.type))
        return nullptr;
    if (v.type == ValueType::UserData) {

        bool real = chain_contains(L->allocated_objects, h) || chain_contains(L->old_objects, h);
        if (!real)
            return nullptr;
    }
    if (h->marked != 0)
        return nullptr;
    h->marked = markval;
    return h;
}

static CLX_INLINE_HOT void gc_push_for_trace(LState::GCStack &wl, LHeader *h, LValue v) {
    ValueType t = v.type;
    if (t == Table || t == Thread || t == Function || t == UserData)
        wl.push(h);
}

//------------------ weak_mode_of — cached __mode bits of a table's metatable (1 = weak keys, 2 = weak values)
static uint8_t weak_mode_of(LState *L, LTable *t);
//------------------ weak_clearable — stock clears weak references only to these four kinds (never strings)
static bool weak_clearable(const LValue &v);

static bool gc_trace_table_young(LState *L, LTable *t, LState::GCStack &wl) {
    bool has_nonold = false;
    //------------------ weak entries are not traced through: the weak pass of the same cycle decides them
    //------------------ from what the strong graph reached (weak-keyed values are ephemerons)
    uint8_t wm = weak_mode_of(L, t);
    for (size_t i = 0; i < t->array_size; ++i) {
        if (t->array_types[i] == Nil || (wm & 2))
            continue;
        LValue v(t->array[i], t->array_types[i]);
        if (!v.is_gc_obj())
            continue;
        LHeader *h = v.as_pointer();
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
    LTableExt *ex = t->ext;
    if (!ex)
        return has_nonold;
    auto trace_entry = [&](const HashEntry &e) {
        if (e.ktype == Nil)
            return;
        for (int which = 0; which < 2; ++which) {
            if (which == 0 ? (wm & 1) : (wm & 3))
                continue;
            LValue v(which == 0 ? e.key : e.val, which == 0 ? e.ktype : e.vtype);
            if (!v.is_gc_obj())
                continue;
            LHeader *h = v.as_pointer();
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
    };
    if (ex->hash_bitmap) {
        size_t words = (ex->hash_size + 63) / 64;
        for (size_t w = 0; w < words; ++w) {
            uint64_t bits = ex->hash_bitmap[w];
            while (bits) {
                size_t b = static_cast<size_t>(clx_ctzll(bits));
                bits &= bits - 1;
                size_t _i = (w << 6) + b;
                if (_i >= ex->hash_size)
                    break;
                trace_entry(ex->entries[_i]);
            }
        }
    } else {
        for (size_t _i = 0; _i < ex->hash_size; ++_i)
            trace_entry(ex->entries[_i]);
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

static CLX_INLINE_HOT bool gc_mark_young(LState *L, const LValue &v, LState::GCStack &wl) {
    if (!v.is_gc_obj())
        return false;
    LHeader *h = v.as_pointer();
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

static bool gc_trace_thread_young(LState *L, LThread *th, LState::GCStack &wl) {
    bool has_nonold = false;
    auto one = [&](const LValue &v) {
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
    gc_walk_roots(th->top_root, one);
    return has_nonold;
}

static bool gc_trace_function_young(LState *L, LCFunction *f, LState::GCStack &wl) {
    bool has_nonold = false;
    if (f->env) {
        LValue envv(Table, f->env);
        if (gc_mark_young(L, envv, wl))
            has_nonold = true;
    }
    for (const LUpValue &c : f->gc_cells)
        if (c && gc_mark_young(L, *c, wl))
            has_nonold = true;
    return has_nonold;
}

//------------------ LState::is_allocated_userdata — membership check on both GC chains
bool LState::is_allocated_userdata(const LHeader *h) const {
    return chain_contains(allocated_objects, h) || chain_contains(old_objects, h);
}

//------------------ gc_trace_remember_nonold_children — write-barrier catch-up for

static void gc_trace_remember_nonold_children(LTable *t) {
    LState *L = clx_current_L;
    if (!L)
        return;
    for (size_t i = 0; i < t->array_size; ++i) {
        if (t->array_types[i] == Nil)
            continue;
        LValue v(t->array[i], t->array_types[i]);
        if (!v.is_gc_obj())
            continue;
        LHeader *h = v.as_pointer();
        if (h && h->type == static_cast<uint8_t>(v.type) && h->age != AGE_OLD)
            L->gc_remember(t);
    }
    if (t->ext) {
        for (size_t i = 0; i < t->ext->hash_size; ++i) {
            HashEntry &e = t->ext->entries[i];
            if (e.ktype == Nil)
                continue;
            for (int which = 0; which < 2; ++which) {
                LValue v(which == 0 ? e.key : e.val, which == 0 ? e.ktype : e.vtype);
                if (!v.is_gc_obj())
                    continue;
                LHeader *h = v.as_pointer();
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

void LState::gc_minor() {
    //------------------ re-entrancy guard: draining finalizables below runs Lua, which may allocate
    //------------------ and re-enter through gc_maybe_collect().
    if (gc_minor_active)
        return;
    ++gc_stats_minors;
    gc_minor_active = true;
    if (gc_stats_remembered_high < gc_remembered.size())
        gc_stats_remembered_high = gc_remembered.size();
    auto &wl = gc_worklist;
    wl.clear();

    //------------------ arena tables: reset stale marks (never cleared by sweep)
    for (FuncArena *ar = active_arenas; ar; ar = ar->next_active)
        for (LTable *at = ar->tables; at; at = static_cast<LTable *>(at->next))
            at->marked = 0;

    auto push_if_needed = [&](const LValue &v) {
        if (!v.is_gc_obj())
            return;
        LHeader *h = v.as_pointer();
        if (!h || h->type != static_cast<uint8_t>(v.type) || h->age == AGE_OLD || h->marked != 0)
            return;
        if (v.type == ValueType::UserData && !is_allocated_userdata(h))
            return;
        h->marked = 1;
        gc_push_for_trace(wl, h, v);
    };

    //------------------ roots: root-frame chain, state threads, permanent roots
    if (_G)
        push_if_needed(LValue(Table, _G));
    if (package_table)
        push_if_needed(LValue(Table, package_table));
    if (main_thread)
        push_if_needed(LValue(Thread, main_thread));
    if (running_thread && running_thread != main_thread)
        push_if_needed(LValue(Thread, running_thread));
    gc_walk_roots(top_root, push_if_needed);
    for (const LValue &r : permanent_roots)
        push_if_needed(r);

    //------------------ roots: metatables set through debug.setmetatable plus the debug registry
    if (string_metatable)
        push_if_needed(LValue(Table, string_metatable));
    for (LTable *mt : type_metatables)
        if (mt)
            push_if_needed(LValue(Table, mt));
    if (registry)
        push_if_needed(LValue(Table, registry));

    //------------------ roots: VM proxy wrappers (VMFunction, VMTableProxy) and bridged clx callables.
    //------------------ These live outside the slab and are only reachable through the VM's own object
    //------------------ graph, so without this walk a minor collection frees every live proxy.
    if (clx_mark_vm_proxies_ptr)
        clx_mark_vm_proxies_ptr(this, wl);

    //------------------ roots: gc_recent. Objects here were allocated since the previous collection and
    //------------------ are now ordinary objects: normal reachability decides them. Rooting every one of
    //------------------ them unconditionally promoted each dead object to AGE_SURVIVOR, so it could only be
    //------------------ freed one cycle later — that lag is what kept dead tables off free_tables and forced
    //------------------ fresh array growth. Everything reachable from a live object is still marked through
    //------------------ that object's own trace, so nothing in flight is lost here.

    gc_recent.clear();

    //------------------ roots: remembered old-generation owners and upvalue cells.

    for (LHeader *owner : gc_remembered) {
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

            if (!gc_trace_table_young(this, static_cast<LTable *>(owner), wl))
                owner->flags &= ~LFLAG_REMEMBERED;
        } else if (owner->type == static_cast<uint8_t>(UserData)) {
            LUserdata *ud = static_cast<LUserdata *>(owner);
            bool keeps_young = false;
            if (ud->metatable && ud->metatable->age != AGE_OLD) {
                keeps_young = true;
                if (ud->metatable->marked == 0) {
                    ud->metatable->marked = 1;
                    wl.push(ud->metatable);
                }
            }
            if (ud->uservalue.is_gc_obj()) {
                LHeader *vh = ud->uservalue.as_pointer();
                if (vh && vh->age != AGE_OLD) {
                    keeps_young = true;
                    if (vh->marked == 0) {
                        vh->marked = 1;
                        wl.push(vh);
                    }
                }
            }
            if (!keeps_young)
                owner->flags &= ~LFLAG_REMEMBERED;
        } else if (owner->type == static_cast<uint8_t>(Thread)) {

            if (!gc_trace_thread_young(this, static_cast<LThread *>(owner), wl))
                owner->flags &= ~LFLAG_REMEMBERED;
        } else if (owner->type == static_cast<uint8_t>(Function)) {

            if (!gc_trace_function_young(this, static_cast<LCFunction *>(owner), wl))
                owner->flags &= ~LFLAG_REMEMBERED;
        } else {
            owner->flags &= ~LFLAG_REMEMBERED;
        }
    }
    for (const LUpValue &cell : gc_remembered_cells)
        if (cell)
            push_if_needed(*cell);

    //------------------ trace: descend only into YOUNG children

    bool meta_protected = false;
    while (!wl.empty() || !meta_protected) {
        if (wl.empty()) {
            //------------------ protect metatables of dead finalizable tables. gc_dispose_swept() will push
            //------------------ those tables onto gc_finalizable, but their metatable (and the __gc closure
            //------------------ it holds) is only reachable through the dead object, so the sweep below would
            //------------------ null the closure's func and the drain would throw std::bad_function_call.
            //------------------ Mirrors the protect_wl pass in collect_garbage_full().
            meta_protected = true;
            for (LTable *obj = metatabled_tables; obj; obj = obj->ext->meta_next) {
                if (obj->marked != 0 || (obj->flags & LFLAG_VM_PROXY))
                    continue;
                LTable *mt = obj->ext ? obj->ext->metatable : nullptr;
                if (!mt || mt->marked != 0 || mt->age == AGE_OLD)
                    continue;
                mt->marked = 1;
                wl.push(mt);
            }
            //------------------ same for dead finalizable userdata (files): their metatable (+ __gc
            //------------------ closure) is otherwise swept in-cycle and the drain throws
            //------------------ std::bad_function_call on the nulled closure.
            for (LUserdata *ud = metatabled_ud; ud; ud = ud->meta_next) {
                if (ud->marked != 0)
                    continue;
                LTable *mt = ud->metatable;
                if (!mt || mt->marked != 0 || mt->age == AGE_OLD)
                    continue;
                mt->marked = 1;
                wl.push(mt);
            }
            continue;
        }
        LHeader *curr = wl.back();
        wl.pop_back();
        if (curr->flags & LFLAG_VM_PROXY)
            continue;
        if (curr->type == static_cast<uint8_t>(Table)) {
            gc_trace_table_young(this, static_cast<LTable *>(curr), wl);
        } else if (curr->type == static_cast<uint8_t>(Thread)) {
            gc_trace_thread_young(this, static_cast<LThread *>(curr), wl);
        } else if (curr->type == static_cast<uint8_t>(Function)) {
            gc_trace_function_young(this, static_cast<LCFunction *>(curr), wl);
        }

        else if (curr->type == static_cast<uint8_t>(UserData)) {
            LUserdata *ud = static_cast<LUserdata *>(curr);
            if (ud->metatable && ud->metatable->age != AGE_OLD && ud->metatable->marked == 0) {
                ud->metatable->marked = 1;
                wl.push(ud->metatable);
            }
            if (ud->uservalue.is_gc_obj()) {
                LHeader *vh = ud->uservalue.as_pointer();
                if (vh && vh->age != AGE_OLD && vh->marked == 0) {
                    vh->marked = 1;
                    wl.push(vh);
                }
            }
        }
    }

    //------------------ weak tables: a young key/value this cycle did not reach is dead. Weak-keyed
    //------------------ tables are ephemerons, so their values are marked from live keys first.
    if (metatabled_tables) {
        auto weak_dead_young = [&](const LValue &v) {
            if (!weak_clearable(v))
                return false;
            LHeader *h = v.as_pointer();
            if (!h || h->type != static_cast<uint8_t>(v.type))
                return false;
            return h->age != AGE_OLD && h->marked == 0;
        };
        auto trace_young = [&](LHeader *start) {
            if (start->marked == 0) {
                start->marked = 1;
                wl.push(start);
            }
            while (!wl.empty()) {
                LHeader *curr = wl.back();
                wl.pop_back();
                if (curr->flags & LFLAG_VM_PROXY)
                    continue;
                if (curr->type == static_cast<uint8_t>(Table))
                    gc_trace_table_young(this, static_cast<LTable *>(curr), wl);
                else if (curr->type == static_cast<uint8_t>(Thread))
                    gc_trace_thread_young(this, static_cast<LThread *>(curr), wl);
                else if (curr->type == static_cast<uint8_t>(Function))
                    gc_trace_function_young(this, static_cast<LCFunction *>(curr), wl);
            }
        };
        for (;;) {
            bool progress = false;
            for (LTable *obj = metatabled_tables; obj; obj = obj->ext->meta_next) {
                LTableExt *ex = obj->ext;
                if (!ex || weak_mode_of(this, obj) != 1)
                    continue;
                for (size_t _i = 0; _i < obj->array_size; ++_i) {
                    if (obj->array_types[_i] == Nil)
                        continue;
                    LValue v(obj->array[_i], obj->array_types[_i]);
                    if (!weak_clearable(v) || v.as_pointer()->marked != 0)
                        continue;
                    if (v.as_pointer()->age == AGE_OLD)
                        continue;
                    trace_young(v.as_pointer());
                    progress = true;
                }
                for (size_t _i = 0; _i < ex->hash_size; ++_i) {
                    if (ex->entries[_i].ktype == Nil)
                        continue;
                    if (weak_dead_young(LValue(ex->entries[_i].key, ex->entries[_i].ktype)))
                        continue;
                    LValue v(ex->entries[_i].val, ex->entries[_i].vtype);
                    if (!weak_clearable(v) || v.as_pointer()->marked != 0)
                        continue;
                    if (v.as_pointer()->age == AGE_OLD)
                        continue;
                    trace_young(v.as_pointer());
                    progress = true;
                }
            }
            if (!progress)
                break;
        }
        for (LTable *obj = metatabled_tables; obj; obj = obj->ext->meta_next) {
            LTableExt *ex = obj->ext;
            if (!ex)
                continue;
            uint8_t wm = weak_mode_of(this, obj);
            if (!wm)
                continue;
            if (wm & 2) {
                for (size_t _i = 0; _i < obj->array_size; ++_i) {
                    if (obj->array_types[_i] == Nil)
                        continue;
                    if (!weak_dead_young(LValue(obj->array[_i], obj->array_types[_i])))
                        continue;
                    obj->array[_i] = TValue();
                    obj->array_types[_i] = Nil;
                    if (obj->array_lim > _i)
                        obj->array_lim = _i;
                }
            }
            if (ex->hash_size == 0)
                continue;
            bool removed = false;
            for (size_t _i = 0; _i < ex->hash_size; ++_i) {
                HashEntry &e = ex->entries[_i];
                if (e.ktype == Nil)
                    continue;
                bool dead = false;
                if (wm & 1)
                    dead = weak_dead_young(LValue(e.key, e.ktype));
                if (!dead && (wm & 2))
                    dead = weak_dead_young(LValue(e.val, e.vtype));
                if (!dead)
                    continue;
                e.key.payload.u64 = HASH_TOMBSTONE;
                e.ktype = Nil;
                e.val = TValue();
                e.vtype = Nil;
                ex->hash_count--;
                ex->hash_tombs++;
                if (ex->hash_bitmap)
                    ex->hash_bitmap[_i / 64] &= ~(1ULL << (_i % 64));
                removed = true;
            }
            if (removed)
                ex->hash_version++;
        }
    }

    //------------------ single walk over the allocation chain: age/promote, then free.

    auto age_and_promote = [&](LHeader *h) {
        uint8_t from = h->age;
        uint8_t to = (from == AGE_YOUNG) ? AGE_SURVIVOR : AGE_OLD;
        //------------------ Young->survivor promotion never moves old-bytes (both non-old); only
        //------------------ pay for the size computation on the survivor->old transition.
        if (to == AGE_OLD)
            gc_update_old_bytes(h, from, to);
        h->age = to;

        if (to == AGE_OLD && h->marked == 1 && !(h->flags & LFLAG_VM_PROXY)) {
            if (h->type == static_cast<uint8_t>(Table)) {
                gc_trace_remember_nonold_children(static_cast<LTable *>(h));
            } else if (h->type == static_cast<uint8_t>(UserData)) {
                LUserdata *ud = static_cast<LUserdata *>(h);
                bool young_child = ud->metatable && ud->metatable->age != AGE_OLD;
                if (ud->uservalue.is_gc_obj()) {
                    LHeader *vh = ud->uservalue.as_pointer();
                    if (vh && vh->age != AGE_OLD)
                        young_child = true;
                }
                if (young_child)
                    gc_remember(ud);
            } else if (h->type == static_cast<uint8_t>(Thread)) {
                LThread *th = static_cast<LThread *>(h);
                auto needs_root = [&](const LValue &v) {
                    if (!v.is_gc_obj())
                        return false;
                    LHeader *ch = v.as_pointer();
                    return ch && ch->type == static_cast<uint8_t>(v.type) && ch->age != AGE_OLD;
                };
                bool young_ref = needs_root(th->function);
                if (th->caller && needs_root(LValue(Thread, th->caller)))
                    young_ref = true;
                for (size_t i = 0; !young_ref && i < th->yield_args.count; ++i)
                    if (needs_root(th->yield_args[i]))
                        young_ref = true;
                for (size_t i = 0; !young_ref && i < th->resume_args.count; ++i)
                    if (needs_root(th->resume_args[i]))
                        young_ref = true;
                gc_walk_roots(th->top_root, [&](const LValue &v) {
                    if (!young_ref && needs_root(v))
                        young_ref = true;
                });
                if (young_ref)
                    gc_remember(th);
            } else if (h->type == static_cast<uint8_t>(Function)) {
                LCFunction *fn = static_cast<LCFunction *>(h);
                if (fn->env && fn->env->age != AGE_OLD)
                    gc_remember(fn);
                for (const LUpValue &c : fn->gc_cells) {
                    if (c && c->is_gc_obj()) {
                        LHeader *ch = static_cast<LHeader *>(c->as_pointer());
                        if (ch && ch->type == static_cast<uint8_t>(c->type) && ch->age != AGE_OLD) {
                            gc_remember(fn);
                            break;
                        }
                    }
                }
            }
        }
    };

    //------------------ single walk over the allocation chain: age/promote,

    size_t before_sweep = allocated_bytes;
    LHeader **link = &allocated_objects;
    while (*link) {
        LHeader *o = *link;
        if (o->age == AGE_OLD) {
            //------------------ Should not happen (promotions move to old_objects below, majors
            //------------------ re-partition); kept as a safety net, not a fast path.
            *link = o->next;
            o->next = old_objects;
            old_objects = o;
            continue;
        }
        if (o->marked == 1) {
            age_and_promote(o);
            o->marked = 0;
            if (o->age == AGE_OLD) {
                //------------------ Segregate: minors never walk old_objects, killing the per-minor
                //------------------ re-walk of the long-lived set (2.9M wasted visits in binarytrees).
                *link = o->next;
                o->next = old_objects;
                old_objects = o;
            } else {
                link = &o->next;
            }
        } else if (o->age == AGE_SURVIVOR) {
            LHeader *next_obj = o->next;
            gc_dispose_swept(this, o);
            *link = next_obj;
            object_count--;
        } else if (!(o->flags & (LFLAG_GC_PIN | LFLAG_REMEMBERED))) {
            //------------------ dead young: unreachable at this collection. gc_recent was cleared above and no Lua
            //------------------ runs between that clear and this walk, so an unmarked young object here is garbage,
            //------------------ not something in flight. Returning it to free_tables now is what lets the next
            //------------------ table reuse its array buffer instead of re-growing 8->16->32->64. Pinned objects
            //------------------ are spared: one freshly taken off the free list may not be rooted yet.
            LHeader *next_obj = o->next;
            gc_dispose_swept(this, o);
            *link = next_obj;
            object_count--;
        } else {
            link = &o->next;
        }
    }
    gc_stats_minor_freed += (before_sweep > allocated_bytes) ? (before_sweep - allocated_bytes) : 0;

    //------------------ drop stale remembered entries and pins (mark bits were

    {
        size_t w = 0;
        for (size_t i = 0; i < gc_remembered.size(); ++i) {
            LHeader *h = gc_remembered[i];
            if (h->flags & LFLAG_REMEMBERED) {
                gc_remembered[w++] = h;
            } else {
                h->flags &= ~LFLAG_REMEMBERED;
            }
        }
        gc_remembered.resize(w);
        w = 0;
        for (size_t i = 0; i < gc_remembered_cells.size(); ++i) {
            const LUpValue &cell = gc_remembered_cells[i];
            if (cell && cell->is_gc_obj()) {
                LHeader *ch = static_cast<LHeader *>(cell->as_pointer());
                if (ch && ch->type == static_cast<uint8_t>(cell->type) && ch->age != AGE_OLD
                    && (cell->type != ValueType::UserData || is_allocated_userdata(ch))) {
                    gc_remembered_cells[w++] = cell;
                    continue;
                }
            }
        }
        gc_remembered_cells.resize(w);
        gc_remembered_cell_set.clear();
        for (const LUpValue &cell : gc_remembered_cells)
            gc_remembered_cell_set.insert(cell.get());
        for (size_t _pi = 0; _pi < gc_pinned.top; ++_pi)
            gc_pinned.data[_pi]->flags &= ~LFLAG_GC_PIN;
        gc_pinned.clear();
    }

    //------------------ rebuild pacing state
    gc_recent.clear();

    //------------------ drain finalizables. gc_dispose_swept() routes every swept metatabled table onto
    //------------------ gc_finalizable, but only the incremental major sweep used to drain that list, so
    //------------------ minors disposed objects whose __gc could then never run (finalizers stalled at 1).
    //------------------ Runs before gc_minor_active drops so a finalizer that allocates cannot nest a minor
    //------------------ inside this drain.

    if (gc_finalizable || gc_finalizable_ud)
        gc_drain_finalizables(this);

    gc_bytes_at_minor = allocated_bytes;
    gc_minor_active = false;

    //------------------ adaptive minor pacing: next trigger ≈ one current heap-full, clamped.
    //------------------ Hysteresis on the way down (at most halves per minor): without it the threshold
    //------------------ collapses to the post-minor live set after every good minor, then oscillates — churn
    //------------------ workloads like binarytrees re-walk their whole live chain in mark+sweep on each of
    //------------------ those too-frequent minors. Growth stays immediate via allocated_bytes.

    if (!gc_minor_threshold_env) {
        size_t next = allocated_bytes;
        size_t floor = gc_minor_threshold / 2;
        if (next < floor)
            next = floor;
        if (next < kMinorThresholdMin)
            next = kMinorThresholdMin;
        if (next > kMinorThresholdMax)
            next = kMinorThresholdMax;
        gc_minor_threshold = next;
    }

    //------------------ major scheduling: if the old set outgrew its budget, the next

    gc_major_pending = (gc_old_bytes >= gc_major_threshold);

    wl.clear();
}

//------------------ LState::gc_step — incremental GC step; 'size' is collectgarbage("step", size)
//------------------ and scales the work done, zero meaning "finish a whole cycle"
bool LState::gc_step(int64_t size) {
    if (size == 0) {
        if (gc_phase == GCPhase::Sweeping)
            while (!gc_step(-1))
                ;
        collect_garbage();
        return true;
    }

    if (gc_phase != GCPhase::Sweeping) {
        if (gc_draining || gc_minor_active)
            return true;
        gc_collect_mark();
        if (gc_phase != GCPhase::Sweeping)
            return true;
    }

    size_t budget = GC_STEP_BUDGET;
    if (size > 0) {
        size_t scaled = static_cast<size_t>((size * static_cast<int64_t>(gc_stepmul)) / 8);
        budget = scaled ? scaled : 1;
    }
    LHeader *curr = gc_sweep_cursor;
    LHeader *prev = gc_prev;

    while (curr && budget--) {
        LHeader *next_obj = curr->next;

        if (curr->marked == 0 && !(curr->flags & (LFLAG_REMEMBERED | LFLAG_GC_PIN))) {
            if (prev) {
                prev->next = next_obj;
            } else {
                if (curr != allocated_objects) {
                    LHeader *h = allocated_objects;
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
            if (gc_mode == GCMode::Generational && curr->age != AGE_OLD && !(curr->flags & LFLAG_REMEMBERED)
                && !(curr->flags & LFLAG_GC_PIN)) {
                bool promoting_to_old = curr->age == AGE_SURVIVOR;
                gc_update_old_bytes(curr, curr->age, AGE_OLD);
                curr->age = AGE_OLD;
                if (promoting_to_old && curr->marked == 1) {
                    if (curr->type == static_cast<uint8_t>(Table))
                        gc_trace_remember_nonold_children(static_cast<LTable *>(curr));
                    else if (curr->type == static_cast<uint8_t>(Function)) {
                        LCFunction *fn = static_cast<LCFunction *>(curr);
                        if (fn->env && fn->env->age != AGE_OLD)
                            gc_remember(fn);
                        for (const LUpValue &c : fn->gc_cells) {
                            if (c && c->is_gc_obj()) {
                                LHeader *ch = static_cast<LHeader *>(c->as_pointer());
                                if (ch && ch->type == static_cast<uint8_t>(c->type) && ch->age != AGE_OLD) {
                                    gc_remember(fn);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
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

        //------------------ Re-partition: minors walk only allocated_objects, so move every AGE_OLD
        //------------------ survivor back to old_objects. Single O(heap) pass per completed cycle.
        {
            LHeader **link = &allocated_objects;
            while (*link) {
                LHeader *o = *link;
                if (o->age == AGE_OLD) {
                    *link = o->next;
                    o->next = old_objects;
                    old_objects = o;
                } else {
                    link = &o->next;
                }
            }
        }

        size_t live = allocated_bytes;
        //------------------ Pacing: one collection is a synchronous mark+sweep of O(heap), so

        size_t headroom = std::clamp(live / 2, size_t(1 * 1024 * 1024), size_t(256 * 1024 * 1024));
        gc_bytes_threshold = live + headroom;
        gc_draining = false;
        return true;
    }
    return false;
}

//------------------ LState::collect_garbage — full mark-sweep collection (entry point)

void LState::collect_garbage() {
    //------------------ re-entrancy guard: gc_minor() drains finalizables before dropping gc_minor_active,
    //------------------ so a __gc that calls collectgarbage() must not start a full collection mid-drain.
    if (gc_minor_active)
        return;
    ++gc_stats_collects;
    if (gc_mode == GCMode::Generational && !gc_draining && gc_phase == GCPhase::Idle)
        gc_minor();
    size_t before = allocated_bytes;
    collect_garbage_full();
    gc_stats_major_freed += (before > allocated_bytes) ? (before - allocated_bytes) : 0;
    if (gc_mode == GCMode::Generational)
        gc_major_pending = false;
}

static uint8_t weak_mode_of(LState *L, LTable *t) {
    LTableExt *ex = t->ext;
    if (!ex || !ex->metatable)
        return 0;
    LTable *mt = ex->metatable;
    uint32_t stamp = mt->ext ? mt->ext->hash_version : 0;
    if (ex->weak_meta == mt && ex->weak_stamp == stamp)
        return ex->weak_mode;
    uint8_t mode = 0;
    LValue mv = mt->gettable(L->str_mode);
    if (mv.type == ValueType::String) {
        const char *s = mv.as_string();
        uint32_t len = mv.string_len();
        for (uint32_t i = 0; i < len; ++i) {
            if (s[i] == 'k')
                mode |= 1;
            else if (s[i] == 'v')
                mode |= 2;
        }
    }
    ex->weak_mode = mode;
    ex->weak_meta = mt;
    ex->weak_stamp = stamp;
    return mode;
}

//------------------ weak_clearable — stock clears weak references only to these four kinds (never strings)
static bool weak_clearable(const LValue &v) {
    ValueType t = v.type;
    return t == Table || t == Thread || t == Function || t == UserData;
}

//------------------ weak_dead — a clearable value that this mark did not reach
static bool weak_dead(const LValue &v) {
    if (!weak_clearable(v))
        return false;
    LHeader *h = v.as_pointer();
    return h && h->type == static_cast<uint8_t>(v.type) && h->marked == 0;
}

//------------------ LState::gc_collect_mark — marks from every root and arms the incremental sweep
void LState::gc_collect_mark() {
    if (gc_draining)
        return;
    //------------------ Reunite generations: gc_step sweeps a single chain, so splice old_objects
    //------------------ back onto the young chain before marking (O(old) on a cold path). Partitioned
    //------------------ again after the sweep below.
    while (old_objects) {
        LHeader *o = old_objects;
        old_objects = o->next;
        o->next = allocated_objects;
        allocated_objects = o;
    }
    auto &wl = gc_worklist;
    wl.clear();

    //------------------ clear every leftover mark first: a minor interrupted mid-sweep, or a collection
    //------------------ that returned before its sweep, leaves marks set. gc_mark_value treats a marked
    //------------------ object as already traced, so a stale mark on _G would skip the whole global table
    //------------------ and let its reachable values be swept as garbage.
    for (LHeader *o = allocated_objects; o; o = o->next)
        o->marked = 0;
    for (FuncArena *ar = active_arenas; ar; ar = ar->next_active)
        for (LTable *at = ar->tables; at; at = static_cast<LTable *>(at->next))
            at->marked = 0;

    auto mark_gc = [&](LValue v, uint8_t mark) -> LHeader * { return gc_mark_value(this, v, mark); };
    auto push_if_needed = [&](LValue v) {
        LHeader *h = mark_gc(v, 1);
        if (!h)
            return;
        ValueType t = v.type;
        if (t == Table || t == Thread || t == Function || t == UserData)
            wl.push(h);
    };

    if (_G)
        push_if_needed(LValue(Table, _G));
    if (package_table)
        push_if_needed(LValue(Table, package_table));
    if (main_thread)
        push_if_needed(LValue(Thread, main_thread));
    if (running_thread && running_thread != main_thread)
        push_if_needed(LValue(Thread, running_thread));
    gc_walk_roots(top_root, push_if_needed);

    for (const LValue &r : permanent_roots)
        push_if_needed(r);

    //------------------ roots: metatables set through debug.setmetatable plus the debug registry
    if (string_metatable)
        push_if_needed(LValue(Table, string_metatable));
    for (LTable *mt : type_metatables)
        if (mt)
            push_if_needed(LValue(Table, mt));
    if (registry)
        push_if_needed(LValue(Table, registry));

    if (!gc_recent.empty()) {
        for (size_t _ri = 0; _ri < gc_recent.top; ++_ri) {
            LHeader *h = gc_recent.data[_ri];
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

    std::vector<LTable *> weak_tables;

    auto propagate = [&]() {
    while (!wl.empty()) {
        LHeader *curr = wl.back();
        wl.pop_back();

        if (curr->flags & LFLAG_VM_PROXY) {
            continue;
        }

        if (curr->type == static_cast<uint8_t>(Table)) {
            LTable *t = static_cast<LTable *>(curr);
            uint8_t wm = weak_mode_of(this, t);
            if (wm)
                weak_tables.push_back(t);
            //------------------ weak values are not traced from the array part; integer keys are always
            //------------------ alive, so a weak-keys-only table keeps those values strong
            if (!(wm & 2)) {
                const uint8_t *types_raw = reinterpret_cast<const uint8_t *>(t->array_types);
                size_t i = 0;
#if defined(CLX_HAS_AVX2)
                const __m256i zero256 = _mm256_setzero_si256();
                for (; i + 32 <= t->array_size; i += 32) {
                    __m256i types = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(types_raw + i));
                    __m256i cmp = _mm256_cmpeq_epi8(types, zero256);
                    uint32_t mask = ~_mm256_movemask_epi8(cmp);
                    while (mask) {
                        int bit = clx_ctz(mask);
                        push_if_needed(LValue(t->array[i + bit], t->array_types[i + bit]));
                        mask &= mask - 1;
                    }
                }
                for (; i + 16 <= t->array_size; i += 16) {
                    __m128i types = _mm_loadu_si128(reinterpret_cast<const __m128i *>(types_raw + i));
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
                    __m128i types = _mm_loadu_si128(reinterpret_cast<const __m128i *>(types_raw + i));
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
            LTableExt *ex = t->ext;
            bool weak_k = (wm & 1) != 0;
            bool weak_v = (wm & 2) != 0;
            if (ex)                    {
                if (ex->hash_bitmap) {
                    size_t bm_words = (ex->hash_size + 63) / 64;
                    size_t seen = 0;
                    for (size_t word = 0; word < bm_words; ++word) {
                        uint64_t bits = ex->hash_bitmap[word];
                        while (bits) {
                            size_t idx = word * 64 + clx_ctzll(bits);
                            if (idx >= ex->hash_size)
                                break;
                            ++seen;
                            if (!weak_k)
                                push_if_needed(LValue(ex->entries[idx].key, ex->entries[idx].ktype));
                            //------------------ weak-keyed values wait for the ephemeron pass below
                            if (!weak_k && !weak_v)
                                push_if_needed(LValue(ex->entries[idx].val, ex->entries[idx].vtype));
                            bits &= bits - 1;
                        }
                    }
                    (void)seen;
                } else {
                    for (size_t _i = 0; _i < ex->hash_size; ++_i) {
                        if (ex->entries[_i].ktype == Nil)
                            continue;
                        if (!weak_k)
                            push_if_needed(LValue(ex->entries[_i].key, ex->entries[_i].ktype));
                        if (!weak_k && !weak_v)
                            push_if_needed(LValue(ex->entries[_i].val, ex->entries[_i].vtype));
                    }
                }
                if (ex->metatable && ex->metatable->marked == 0) {
                    ex->metatable->marked = 1;
                    wl.push(ex->metatable);
                }
            }
        } else if (curr->type == static_cast<uint8_t>(Thread)) {
            LThread *th = static_cast<LThread *>(curr);
            push_if_needed(th->function);
            if (th->caller)
                push_if_needed(LValue(Thread, th->caller));
            for (size_t i = 0; i < th->yield_args.count; ++i)
                push_if_needed(th->yield_args[i]);
            for (size_t i = 0; i < th->resume_args.count; ++i)
                push_if_needed(th->resume_args[i]);
            gc_walk_roots(th->top_root, push_if_needed);
        } else if (curr->type == static_cast<uint8_t>(Function)) {
            LCFunction *f = static_cast<LCFunction *>(curr);
            if (f->env)
                push_if_needed(LValue(Table, f->env));
            for (const LUpValue &cell : f->gc_cells)
                if (cell)
                    push_if_needed(*cell);
        } else if (curr->type == static_cast<uint8_t>(UserData)) {
            LUserdata *ud = static_cast<LUserdata *>(curr);
            if (ud->metatable && ud->metatable->marked == 0) {
                ud->metatable->marked = 1;
                wl.push(ud->metatable);
            }
            if (ud->uservalue.is_gc_obj()) {
                LHeader *vh = ud->uservalue.as_pointer();
                if (vh && vh->marked == 0) {
                    vh->marked = 1;
                    wl.push(vh);
                }
            }
        }
    }
    };

    propagate();

    //------------------ weak-keyed tables are ephemerons: a value is reachable only through its key, so
    //------------------ values are marked in rounds until nothing new appears (a marked value can expose
    //------------------ more live keys) and only then are the dead entries dropped
    if (!weak_tables.empty()) {
        for (;;) {
            size_t before_size = weak_tables.size();
            bool progress = false;
            for (size_t ti = 0; ti < weak_tables.size(); ++ti) {
                LTable *t = weak_tables[ti];
                LTableExt *ex = t->ext;
                if (!ex || ex->weak_mode != 1)
                    continue;
                size_t before = wl.size();
                for (size_t _i = 0; _i < t->array_size; ++_i)
                    if (t->array_types[_i] != Nil)
                        push_if_needed(LValue(t->array[_i], t->array_types[_i]));
                for (size_t _i = 0; _i < ex->hash_size; ++_i) {
                    if (ex->entries[_i].ktype == Nil)
                        continue;
                    LValue k(ex->entries[_i].key, ex->entries[_i].ktype);
                    if (weak_dead(k))
                        continue;
                    push_if_needed(LValue(ex->entries[_i].val, ex->entries[_i].vtype));
                }
                if (wl.size() > before)
                    progress = true;
            }
            propagate();
            if (!progress && weak_tables.size() == before_size)
                break;
        }
        for (size_t ti = 0; ti < weak_tables.size(); ++ti) {
            LTable *t = weak_tables[ti];
            LTableExt *ex = t->ext;
            if (!ex || !ex->weak_mode)
                continue;
            bool weak_k = (ex->weak_mode & 1) != 0;
            bool weak_v = (ex->weak_mode & 2) != 0;
            if (weak_v) {
                for (size_t _i = 0; _i < t->array_size; ++_i) {
                    if (t->array_types[_i] == Nil)
                        continue;
                    if (!weak_dead(LValue(t->array[_i], t->array_types[_i])))
                        continue;
                    t->array[_i] = TValue();
                    t->array_types[_i] = Nil;
                    if (t->array_lim > _i)
                        t->array_lim = _i;
                }
            }
            if (ex->hash_size == 0)
                continue;
            bool removed = false;
            for (size_t _i = 0; _i < ex->hash_size; ++_i) {
                HashEntry &e = ex->entries[_i];
                if (e.ktype == Nil)
                    continue;
                bool dead = false;
                if (weak_k)
                    dead = weak_dead(LValue(e.key, e.ktype));
                if (!dead && weak_v)
                    dead = weak_dead(LValue(e.val, e.vtype));
                if (!dead)
                    continue;
                e.key.payload.u64 = HASH_TOMBSTONE;
                e.ktype = Nil;
                e.val = TValue();
                e.vtype = Nil;
                ex->hash_count--;
                ex->hash_tombs++;
                if (ex->hash_bitmap)
                    ex->hash_bitmap[_i / 64] &= ~(1ULL << (_i % 64));
                removed = true;
            }
            if (removed)
                ex->hash_version++;
        }
    }

    std::vector<LHeader *> protect_wl;
    for (LTable *obj = metatabled_tables; obj; obj = obj->ext->meta_next) {
        if (obj->marked == 0 && !(obj->flags & LFLAG_VM_PROXY)) {
            LTable *mt = obj->ext ? obj->ext->metatable : nullptr;
            if (mt && mt->marked == 0) {
                mt->marked = 2;
                protect_wl.push_back(mt);
            }
        }
    }
    for (LUserdata *ud = metatabled_ud; ud; ud = ud->meta_next) {
        if (ud->marked == 0) {
            LTable *mt = ud->metatable;
            if (mt && mt->marked == 0) {
                mt->marked = 2;
                protect_wl.push_back(mt);
            }
        }
    }
    while (!protect_wl.empty()) {
        LHeader *curr = protect_wl.back();
        protect_wl.pop_back();
        if (curr->type == static_cast<uint8_t>(Table)) {
            LTable *tt = static_cast<LTable *>(curr);
            {
                const uint8_t *types_raw = reinterpret_cast<const uint8_t *>(tt->array_types);
                size_t i = 0;
#if defined(CLX_HAS_AVX2)
                const __m256i zero256 = _mm256_setzero_si256();
                for (; i + 32 <= tt->array_size; i += 32) {
                    __m256i types = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(types_raw + i));
                    __m256i cmp = _mm256_cmpeq_epi8(types, zero256);
                    uint32_t mask = ~_mm256_movemask_epi8(cmp);
                    while (mask) {
                        int bit = clx_ctz(mask);
                        LValue v = LValue(tt->array[i + bit], tt->array_types[i + bit]);
                        if (LHeader *h = mark_gc(v, 2))
                            protect_wl.push_back(h);
                        mask &= mask - 1;
                    }
                }
                for (; i + 16 <= tt->array_size; i += 16) {
                    __m128i types = _mm_loadu_si128(reinterpret_cast<const __m128i *>(types_raw + i));
                    __m128i cmp = _mm_cmpeq_epi8(types, _mm_setzero_si128());
                    uint32_t mask = static_cast<uint32_t>(~_mm_movemask_epi8(cmp)) & 0xFFFF;
                    while (mask) {
                        int bit = clx_ctz(mask);
                        LValue v = LValue(tt->array[i + bit], tt->array_types[i + bit]);
                        if (LHeader *h = mark_gc(v, 2))
                            protect_wl.push_back(h);
                        mask &= mask - 1;
                    }
                }
#elif defined(CLX_HAS_SSE2)
                const __m128i zero = _mm_setzero_si128();
                for (; i + 16 <= tt->array_size; i += 16) {
                    __m128i types = _mm_loadu_si128(reinterpret_cast<const __m128i *>(types_raw + i));
                    __m128i cmp = _mm_cmpeq_epi8(types, zero);
                    uint32_t mask = static_cast<uint32_t>(~_mm_movemask_epi8(cmp)) & 0xFFFF;
                    while (mask) {
                        int bit = clx_ctz(mask);
                        LValue v = LValue(tt->array[i + bit], tt->array_types[i + bit]);
                        if (LHeader *h = mark_gc(v, 2))
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
                            if (LHeader *h = mark_gc(v, 2))
                                protect_wl.push_back(h);
                        }
                    }
                }
#endif
                for (; i < tt->array_size; ++i) {
                    LValue v = LValue(tt->array[i], tt->array_types[i]);
                    if (LHeader *h = mark_gc(v, 2))
                        protect_wl.push_back(h);
                }
            }
            LTableExt *tt_ex = tt->ext;
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
                                if (LHeader *h = mark_gc(v, 2))
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
                            if (LHeader *h = mark_gc(v, 2))
                                protect_wl.push_back(h);
                        }
                    }
                }
            }
        }
    }
    //------------------ A major marks from roots, so LFLAG_REMEMBERED no longer says anything about
    //------------------ reachability. gc_step() never disposes a remembered object and minors skip AGE_OLD
    //------------------ entirely, so a dead old table mutated by a write barrier would survive every
    //------------------ collection. Drop the entries this mark proved dead *before* sweeping, so the sweep
    //------------------ below can free them and gc_remembered holds no pointer to a soon-freed object.

    {
        size_t w = 0;
        for (size_t i = 0; i < gc_remembered.size(); ++i) {
            LHeader *h = gc_remembered[i];
            if (!h)
                continue;
            if (h->marked != 0)
                gc_remembered[w++] = h;
            else
                h->flags &= ~LFLAG_REMEMBERED;
        }
        gc_remembered.resize(w);

        size_t wc = 0;
        for (size_t i = 0; i < gc_remembered_cells.size(); ++i) {
            const LUpValue &cell = gc_remembered_cells[i];
            bool live = false;
            if (cell && cell->is_gc_obj()) {
                LHeader *ch = static_cast<LHeader *>(cell->as_pointer());
                live = ch && ch->type == static_cast<uint8_t>(cell->type) && ch->marked != 0;
            }
            if (live)
                gc_remembered_cells[wc++] = cell;
        }
        gc_remembered_cells.resize(wc);
        gc_remembered_cell_set.clear();
        for (size_t i = 0; i < gc_remembered_cells.size(); ++i)
            gc_remembered_cell_set.insert(gc_remembered_cells[i].get());
    }

    gc_phase = GCPhase::Sweeping;
    gc_sweep_cursor = allocated_objects;
    gc_prev = nullptr;
    gc_finalizable = nullptr;
    gc_finalizable_ud = nullptr;
    object_count = 0;
}

//------------------ LState::collect_garbage_full — mark, then drain the sweep to completion
void LState::collect_garbage_full() {
    if (gc_draining)
        return;
    gc_collect_mark();
    while (!gc_step())
        ;
}

}
