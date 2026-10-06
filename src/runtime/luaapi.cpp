// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  luaapi.cpp · Lua 5.5 C API bridge          │
// └─────────────────────────────────────────────┘

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <clx.h>
#include <clx_runtime.h>

#include "internal.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

//---------- One API frame: a deque of shadow-rooted values plus the upvalues of the C function that owns it.
struct ApiFrame {
    std::deque<clx::LValue> stack;
    std::shared_ptr<std::vector<clx::LUpValue>> ups;
    size_t shadow_base = 0;
};

//---------- CFrameReturn: abandons a C function once its continuation ran; n becomes the function's result count
struct CFrameReturn {
    int n;
};

//---------- C closure payload: a lua_CFunction plus its shared upvalue cells.
struct CFuncAdapter {
    lua_CFunction fn = nullptr;
    std::shared_ptr<std::vector<clx::LUpValue>> ups;

    clx::MultiValue operator()(clx::LState *L, const clx::LValue *args, size_t count) const;
};

//---------- Opaque lua_State: one per clx::LState, holding the API stack frames and registry.
struct lua_State {
    clx::LState *L;
    //---------- thread: the clx::LThread this bundle represents; null means the root bundle (main thread)
    clx::LThread *thread;
    //---------- pcall_depth: nested lua_pcallk frames, reproducing stock's non-yieldable C boundary
    int pcall_depth;
    //---------- catching_frames: live CFuncAdapter/luaopen frames able to catch a CFrameReturn
    int catching_frames;
    std::deque<ApiFrame> frames;
    clx::LValue registry;
    clx::LValue uservalues;
    luaL_Stream file_scratch;
    lua_CFunction panicf;
    lua_WarnFunction warnf;
    void *warnf_ud;
};

//---------- Bundle keeps LUA_EXTRASPACE bytes immediately in front of the lua_State for lua_getextraspace.
struct ApiBundle {
    alignas(lua_State) char extra[LUA_EXTRASPACE];
    lua_State st;
};

static_assert(
    offsetof(ApiBundle, st) == LUA_EXTRASPACE, "lua_getextraspace requires extraspace bytes before lua_State");

//------------------ luaapi_type_names: lua_typename table indexed by (type + 1)
static const char *const luaapi_type_names[] = { "no value", "nil", "boolean", "userdata", "number", "string", "table",
    "function", "userdata", "thread", "upvalue", "proto" };

//---------- luaapi_hexval: hex digit value for integer string parsing
static int luaapi_hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 0;
}

//---------- luaapi_str2int: stock l_str2int (decimal/hex, spaces allowed, wraps two's complement)
static bool luaapi_str2int(const char *s, int64_t &out) {
    while (std::isspace(static_cast<unsigned char>(*s)))
        ++s;
    bool neg = false;
    if (*s == '-') {
        neg = true;
        ++s;
    } else if (*s == '+') {
        ++s;
    }
    uint64_t a = 0;
    bool empty = true;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (; std::isxdigit(static_cast<unsigned char>(*s)); ++s) {
            a = a * 16 + static_cast<uint64_t>(luaapi_hexval(*s));
            empty = false;
        }
    } else {
        for (; std::isdigit(static_cast<unsigned char>(*s)); ++s) {
            uint64_t d = static_cast<uint64_t>(*s - '0');
            if (a > (UINT64_MAX - d) / 10)
                return false;
            a = a * 10 + d;
            empty = false;
        }
    }
    while (std::isspace(static_cast<unsigned char>(*s)))
        ++s;
    if (empty || *s != '\0')
        return false;
    if (!neg && a > static_cast<uint64_t>(INT64_MAX))
        return false;
    out = static_cast<int64_t>(neg ? (~a + 1) : a);
    return true;
}

//---------- luaapi_str2num: stock luaO_str2num — integer first, then float, whole string consumed
static bool luaapi_str2num(const char *s, clx::LValue &out) {
    int64_t iv;
    if (luaapi_str2int(s, iv)) {
        out = clx::LValue(iv);
        return true;
    }
    const char *pm = std::strpbrk(s, ".xXnN");
    if (pm && (*pm == 'n' || *pm == 'N'))
        return false;
    char *end = nullptr;
    errno = 0;
    double d = std::strtod(s, &end);
    if (end == s)
        return false;
    while (std::isspace(static_cast<unsigned char>(*end)))
        ++end;
    if (*end != '\0')
        return false;
    out = clx::LValue(d);
    return true;
}

//---------- luaapi_full_ud: pointer identity first, then header type, so foreign pointers are never dereferenced
static bool luaapi_full_ud(clx::LState *S, const clx::LValue &v) {
    clx::LHeader *p = v.as_pointer();
    if (!p)
        return false;
    if (!S->is_allocated_userdata(p))
        return false;
    return p->type == static_cast<uint8_t>(clx::ValueType::UserData);
}

//---------- luaapi_utf8esc: stock luaO_utf8esc — encode code point backwards into an 8-byte buffer
static int luaapi_utf8esc(char *buff, uint32_t x) {
    static constexpr int kUTF8BuffSize = 8;
    int n = 1;
    if (x < 0x80) {
        buff[kUTF8BuffSize - 1] = static_cast<char>(x);
    } else {
        unsigned int mfb = 0x3f;
        do {
            buff[kUTF8BuffSize - (n++)] = static_cast<char>(0x80 | (x & 0x3f));
            x >>= 6;
            mfb >>= 1;
        } while (x > mfb);
        buff[kUTF8BuffSize - n] = static_cast<char>((~mfb << 1) | x);
    }
    return n;
}

//---------- luaapi_seed_registry: fill the per-state registry and reuse existing package tables
static void luaapi_seed_registry(lua_State *ls) {
    clx::LState *L = ls->L;
    ls->registry = L->create_table(16, 16);
    L->root_value(ls->registry);
    ls->uservalues = L->create_table(0, 8);
    L->root_value(ls->uservalues);

    clx::LValue glob(clx::ValueType::Table, L->_G);
    clx::LValue pkg = clx::table_get(L, glob, clx::LValue(L->intern_string("package")));
    clx::LValue owned_pkg;
    if (pkg.type != clx::ValueType::Table) {
        pkg = L->create_table(8, 0);
        owned_pkg = pkg;
        L->root_value(owned_pkg);
        clx::raw_set(L, glob, clx::LValue(L->intern_string("package")), pkg);
    }

    clx::LValue loaded = clx::raw_get(L, pkg, clx::LValue(L->intern_string("loaded")));
    if (loaded.type != clx::ValueType::Table) {
        loaded = L->create_table(8, 0);
        L->root_value(loaded);
        clx::raw_set(L, pkg, clx::LValue(L->intern_string("loaded")), loaded);
    }
    clx::LValue preload = clx::raw_get(L, pkg, clx::LValue(L->intern_string("preload")));
    if (preload.type != clx::ValueType::Table) {
        preload = L->create_table(8, 0);
        L->root_value(preload);
        clx::raw_set(L, pkg, clx::LValue(L->intern_string("preload")), preload);
    }

    clx::raw_set(L, ls->registry, clx::LValue(static_cast<int64_t>(LUA_RIDX_GLOBALS)), glob);
    if (L->main_thread)
        clx::raw_set(L, ls->registry, clx::LValue(static_cast<int64_t>(LUA_RIDX_MAINTHREAD)),
            clx::LValue(clx::ValueType::Thread, L->main_thread));
    clx::raw_set(L, ls->registry, clx::LValue(L->intern_string(LUA_LOADED_TABLE)), loaded);
    clx::raw_set(L, ls->registry, clx::LValue(L->intern_string(LUA_PRELOAD_TABLE)), preload);

    L->unroot_value(loaded);
    L->unroot_value(preload);
    L->unroot_value(owned_pkg);
}

//---------- g_capi_thread_bundles: every coroutine bundle, freed alongside its owning LState
static std::vector<ApiBundle *> g_capi_thread_bundles;

//---------- luaapi_fetch: lazily create the ApiBundle backing a clx::LState
static lua_State *luaapi_fetch(clx::LState *L) {
    if (!L->luaapi) {
        ApiBundle *b = new ApiBundle();
        b->st.L = L;
        b->st.thread = nullptr;
        b->st.pcall_depth = 0;
        b->st.catching_frames = 0;
        b->st.registry = clx::LValue();
        b->st.uservalues = clx::LValue();
        b->st.file_scratch.f = nullptr;
        b->st.file_scratch.closef = nullptr;
        b->st.panicf = nullptr;
        b->st.warnf = nullptr;
        b->st.warnf_ud = nullptr;
        L->luaapi = b;
        L->luaapi_cleanup = &clx::luaapi_free_state;
        L->luaapi_thread_reset = &clx::luaapi_reset_thread_bundle;
        luaapi_seed_registry(&b->st);
    }
    return &static_cast<ApiBundle *>(L->luaapi)->st;
}

//---------- luaapi_owner: the thread whose GC-root region backs one bundle's API frames
static clx::LThread *luaapi_owner(lua_State *L) {
    if (L->thread)
        return L->thread;
    clx::LState *S = L->L;
    if (S->main_thread)
        return S->main_thread;
    return S->running_thread;
}

//---------- ShadowRegion: the shadow slot array plus its live height for one owner thread
struct ShadowRegion {
    clx::ShadowStack *stack;
    size_t *top;
};

//---------- luaapi_region: only the running thread keeps its roots in LState; every other thread owns its own copy
static ShadowRegion luaapi_region(lua_State *L) {
    clx::LState *S = L->L;
    clx::LThread *o = luaapi_owner(L);
    if (o && o != S->running_thread)
        return ShadowRegion { &o->shadow, &o->shadow_top };
    return ShadowRegion { &S->shadow_stack, &S->shadow_top };
}

//---------- luaapi_fetch_thread: lazily create the ApiBundle backing one coroutine
static lua_State *luaapi_fetch_thread(clx::LThread *t) {
    if (!t->luaapi) {
        lua_State *root = luaapi_fetch(t->state);
        ApiBundle *b = new ApiBundle();
        b->st.L = t->state;
        b->st.thread = t;
        b->st.pcall_depth = 0;
        b->st.catching_frames = 0;
        b->st.registry = root->registry;
        b->st.uservalues = root->uservalues;
        b->st.file_scratch.f = nullptr;
        b->st.file_scratch.closef = nullptr;
        b->st.panicf = nullptr;
        b->st.warnf = nullptr;
        b->st.warnf_ud = nullptr;
        t->luaapi = b;
        g_capi_thread_bundles.push_back(b);
    }
    return &static_cast<ApiBundle *>(t->luaapi)->st;
}

//---------- luaapi_for_thread: the bundle representing a thread; the main thread resolves to the root bundle
static lua_State *luaapi_for_thread(clx::LThread *t) {
    clx::LState *S = t->state;
    if (!S->main_thread || t == S->main_thread)
        return luaapi_fetch(S);
    return luaapi_fetch_thread(t);
}

//---------- luaapi_fetch_current: the bundle of whichever thread is executing right now
static lua_State *luaapi_fetch_current(clx::LState *S) {
    clx::LThread *t = S->running_thread;
    if (t && S->main_thread && t != S->main_thread)
        return luaapi_fetch_thread(t);
    return luaapi_fetch(S);
}

//---------- luaapi_self_thread: the thread a bundle stands for (root bundle == main thread)
static clx::LThread *luaapi_self_thread(lua_State *L) {
    if (L->thread)
        return L->thread;
    clx::LState *S = L->L;
    if (S->main_thread)
        return S->main_thread;
    return S->running_thread;
}

//---------- luaapi_frame: current API frame, orphan-created when a C entry point is entered unprotected
static ApiFrame &luaapi_frame(lua_State *L) {
    if (L->frames.empty()) {
        L->frames.emplace_back();
        L->frames.back().shadow_base = *luaapi_region(L).top;
    }
    return L->frames.back();
}

//---------- luaapi_locate: resolve a pseudo/relative index to a live LValue slot
static clx::LValue *luaapi_locate(lua_State *L, int idx) {
    ApiFrame *f = L->frames.empty() ? nullptr : &L->frames.back();
    if (idx == LUA_REGISTRYINDEX)
        return &L->registry;
    if (idx < LUA_REGISTRYINDEX) {
        long long i = static_cast<long long>(LUA_REGISTRYINDEX) - static_cast<long long>(idx);
        if (!f || !f->ups || i < 1 || static_cast<size_t>(i) > f->ups->size())
            return nullptr;
        return (*f->ups)[static_cast<size_t>(i) - 1].get();
    }
    if (idx == 0 || !f)
        return nullptr;
    long long n = static_cast<long long>(f->stack.size());
    long long i = (idx > 0) ? (static_cast<long long>(idx) - 1) : (n + idx);
    if (i < 0 || i >= n)
        return nullptr;
    return &f->stack[static_cast<size_t>(i)];
}

//---------- luaapi_push: append a value and root it on the owning thread's shadow stack
static void luaapi_push(lua_State *L, const clx::LValue &v) {
    ApiFrame &f = luaapi_frame(L);
    f.stack.push_back(v);
    ShadowRegion r = luaapi_region(L);
    (*r.stack)[(*r.top)++] = clx::TypedSlot(&f.stack.back().val, &f.stack.back().type);
}

//---------- luaapi_truncate: shrink the frame to n slots; growth must go through luaapi_push
static void luaapi_truncate(lua_State *L, size_t n) {
    ApiFrame &f = luaapi_frame(L);
    if (n >= f.stack.size())
        return;
    *luaapi_region(L).top = f.shadow_base + n;
    f.stack.resize(n);
}

//---------- luaapi_pop: drop the top n slots
static void luaapi_pop(lua_State *L, int n) {
    ApiFrame &f = luaapi_frame(L);
    if (n <= 0)
        return;
    size_t want = (static_cast<size_t>(n) >= f.stack.size()) ? 0 : f.stack.size() - static_cast<size_t>(n);
    luaapi_truncate(L, want);
}

//---------- ApiFrameGuard: pushes a frame and restores the owner's shadow height + frame on scope exit
class ApiFrameGuard {
public:
    ApiFrameGuard(clx::LState *L, lua_State *ls)
        : m_ls(ls) {
        (void)L;
        m_base = *luaapi_region(ls).top;
        m_ls->frames.emplace_back();
        m_ls->frames.back().shadow_base = m_base;
        m_ls->catching_frames++;
    }

    ~ApiFrameGuard() {
        *luaapi_region(m_ls).top = m_base;
        m_ls->frames.pop_back();
        m_ls->catching_frames--;
    }

    ApiFrameGuard(const ApiFrameGuard &) = delete;
    ApiFrameGuard &operator=(const ApiFrameGuard &) = delete;

private:
    lua_State *m_ls;
    size_t m_base;
};

//---------- CFuncAdapter::operator — clx entry point that frames the lua_CFunction call
clx::MultiValue CFuncAdapter::operator()(clx::LState *L, const clx::LValue *args, size_t count) const {
    lua_State *ls = luaapi_fetch_current(L);
    {
        ApiFrameGuard guard(L, ls);
        ls->frames.back().ups = ups;
        for (size_t i = 0; i < count; ++i)
            luaapi_push(ls, args[i]);
        int n;
        try {
            n = fn(ls);
        } catch (const CFrameReturn &ret) {
            n = ret.n;
        }
        ApiFrame &f = ls->frames.back();
        size_t sz = f.stack.size();
        size_t keep = (n <= 0) ? 0 : (static_cast<size_t>(n) > sz ? sz : static_cast<size_t>(n));
        std::vector<clx::LValue> tmp(keep);
        for (size_t i = 0; i < keep; ++i)
            tmp[i] = f.stack[sz - keep + i];
        return clx::MultiValue(tmp, L);
    }
}

//---------- luaapi_tag: map an LValue to a LUA_T* code, splitting userdata from light userdata
static int luaapi_tag(clx::LState *S, const clx::LValue &v) {
    using clx::ValueType;
    switch (v.type) {
    case ValueType::Nil:
        return LUA_TNIL;
    case ValueType::Boolean:
        return LUA_TBOOLEAN;
    case ValueType::Int64:
    case ValueType::Double:
        return LUA_TNUMBER;
    case ValueType::String:
        return LUA_TSTRING;
    case ValueType::Table:
        return LUA_TTABLE;
    case ValueType::Function:
        return LUA_TFUNCTION;
    case ValueType::UserData:
        return luaapi_full_ud(S, v) ? LUA_TUSERDATA : LUA_TLIGHTUSERDATA;
    case ValueType::Thread:
        return LUA_TTHREAD;
    default:
        return LUA_TNONE;
    }
}

//---------- luaapi_upvalue_cell: upvalue cell n of a closure, shared by the adapter view and gc_cells
static clx::LUpValue *luaapi_upvalue_cell(clx::LCFunction *fn, int n) {
    if (n < 1 || static_cast<size_t>(n) > fn->gc_cells.size())
        return nullptr;
    return &fn->gc_cells[static_cast<size_t>(n) - 1];
}

//---------- luaapi_close_stub: refuse to close a borrowed FILE* capture
static int luaapi_close_stub(lua_State *L) {
    return luaL_error(L, "attempt to close a borrowed file handle");
}

//---------- luaapi_push_lib: push a global library table, creating an empty one when absent
static int luaapi_push_lib(lua_State *L, const char *name) {
    clx::LState *S = L->L;
    clx::LValue v = clx::table_get(S, clx::LValue(clx::ValueType::Table, S->_G), clx::LValue(S->intern_string(name)));
    if (v.type != clx::ValueType::Table)
        v = S->create_table(8, 0);
    luaapi_push(L, v);
    return 1;
}

//---------- luaapi_iscfn: a closure counts as a C function when it was built by lua_pushcclosure
static bool luaapi_iscfn(clx::LCFunction *f) {
    return f->func.target<CFuncAdapter>() != nullptr;
}

namespace clx {

//---------- luaapi_c_module_open: run a C module's luaopen_*, register it in package.loaded
LValue luaapi_c_module_open(LState *L, int (*openf)(struct ::lua_State *), const char *modname) {
    lua_State *ls = luaapi_fetch_current(L);
    LValue mod;
    {
        ApiFrameGuard guard(L, ls);
        int n;
        try {
            n = openf(ls);
        } catch (const CFrameReturn &ret) {
            n = ret.n;
        }
        ApiFrame &f = ls->frames.back();
        size_t sz = f.stack.size();
        size_t want = (n <= 0) ? 0 : (static_cast<size_t>(n) > sz ? sz : static_cast<size_t>(n));
        if (want > 0)
            mod = f.stack[sz - want];
        L->root_value(mod);
    }
    if (modname && mod.type != ValueType::Nil)
        L->register_loaded_module(modname, mod);
    L->unroot_value(mod);
    return mod;
}

//---------- luaapi_free_state: drop the ApiBundle and its rooted registry values before LState teardown
void luaapi_free_state(LState *L) {
    if (!L || !L->luaapi)
        return;
    for (size_t i = g_capi_thread_bundles.size(); i-- > 0;) {
        ApiBundle *tb = g_capi_thread_bundles[i];
        if (tb->st.L != L)
            continue;
        tb->st.frames.clear();
        tb->st.registry = LValue();
        tb->st.uservalues = LValue();
        if (tb->st.thread)
            tb->st.thread->luaapi = nullptr;
        delete tb;
        g_capi_thread_bundles.erase(g_capi_thread_bundles.begin() + static_cast<long long>(i));
    }
    ApiBundle *b = static_cast<ApiBundle *>(L->luaapi);
    L->unroot_value(b->st.registry);
    L->unroot_value(b->st.uservalues);
    b->st.registry = LValue();
    b->st.uservalues = LValue();
    b->st.frames.clear();
    L->luaapi = nullptr;
    L->luaapi_cleanup = nullptr;
    L->luaapi_thread_reset = nullptr;
    delete b;
}

//---------- luaapi_reset_thread_bundle: clear a recycled coroutine's API frames so it starts from a clean stack
void luaapi_reset_thread_bundle(LState *L, void *bundle) {
    (void)L;
    ApiBundle *b = static_cast<ApiBundle *>(bundle);
    b->st.frames.clear();
    b->st.pcall_depth = 0;
    b->st.catching_frames = 0;
}

}

//---------- lua_ident: RCS ident string matching the stock Lua release banner
extern "C" const char lua_ident[] = "$LuaVersion: " LUA_COPYRIGHT " $"
                                    "$LuaAuthors: " LUA_AUTHORS " $";

//=========================== state manipulation ===========================

lua_State *lua_newstate(lua_Alloc f, void *ud, unsigned seed) {
    (void)f;
    (void)ud;
    (void)seed;
    return nullptr;
}

void lua_close(lua_State *L) {
    (void)L;
}

//---------- luaapi_resume_error: stock resume_error — drop narg args, then report the failure on the coroutine's own stack
static int luaapi_resume_error(lua_State *L, const char *msg, int narg, int *nres) {
    ApiFrame &f = luaapi_frame(L);
    size_t sz = f.stack.size();
    size_t drop = (narg <= 0) ? 0 : ((static_cast<size_t>(narg) > sz) ? sz : static_cast<size_t>(narg));
    luaapi_truncate(L, sz - drop);
    luaapi_push(L, clx::LValue(L->L->intern_string(msg)));
    if (nres)
        *nres = 1;
    return LUA_ERRRUN;
}

//---------- luaapi_set_error_obj: collapse the coroutine's stack onto its single pending error object
static void luaapi_set_error_obj(lua_State *L) {
    clx::LThread *t = L->thread;
    clx::LValue err;
    if (t && t->yield_args.count > 0)
        err = t->yield_args[0];
    else
        err = clx::LValue(L->L->intern_string("coroutine error"));
    luaapi_truncate(L, 0);
    luaapi_push(L, err);
}

lua_State *lua_newthread(lua_State *L) {
    clx::LState *S = L->L;
    clx::LValue th = clx::create_thread(S, clx::LValue(), 262144.0);
    S->root_value(th);
    luaapi_push(L, th);
    S->unroot_value(th);
    return luaapi_for_thread(static_cast<clx::LThread *>(th.as_pointer()));
}

int lua_closethread(lua_State *L, lua_State *from) {
    (void)from;
    clx::LThread *t = L->thread;
    if (!t)
        return LUA_OK;
    clx::LState *S = L->L;
    if (t->status == clx::THREAD_RUNNING || t->status == clx::THREAD_NORMAL) {
        luaapi_push(L,
            clx::LValue(S->intern_string(t->status == clx::THREAD_RUNNING ? "cannot close running coroutine"
                                                                          : "cannot close normal coroutine")));
        return LUA_ERRRUN;
    }
    if (t->status == clx::THREAD_DEAD) {
        if (!t->has_error) {
            luaapi_truncate(L, 0);
            return LUA_OK;
        }
        luaapi_set_error_obj(L);
        return LUA_ERRRUN;
    }
    if (!t->started) {
        luaapi_truncate(L, 0);
        t->status = clx::THREAD_DEAD;
        t->has_error = false;
        return LUA_OK;
    }
    clx::close_thread(S, clx::LValue(clx::ValueType::Thread, t));
    if (t->status == clx::THREAD_DEAD) {
        if (t->has_error) {
            luaapi_set_error_obj(L);
            return LUA_ERRRUN;
        }
        luaapi_truncate(L, 0);
        return LUA_OK;
    }
    luaapi_truncate(L, 0);
    t->status = clx::THREAD_DEAD;
    t->has_error = false;
    return LUA_OK;
}

lua_CFunction lua_atpanic(lua_State *L, lua_CFunction panicf) {
    lua_CFunction old = L->panicf;
    L->panicf = panicf;
    return old;
}

lua_Number lua_version(lua_State *L) {
    (void)L;
    return static_cast<lua_Number>(LUA_VERSION_NUM);
}

//=========================== stack manipulation ===========================

int lua_absindex(lua_State *L, int idx) {
    if (idx > 0 || idx <= LUA_REGISTRYINDEX)
        return idx;
    ApiFrame &f = luaapi_frame(L);
    return static_cast<int>(f.stack.size()) + idx + 1;
}

int lua_gettop(lua_State *L) {
    ApiFrame &f = luaapi_frame(L);
    return static_cast<int>(f.stack.size());
}

void lua_settop(lua_State *L, int idx) {
    ApiFrame &f = luaapi_frame(L);
    if (idx >= 0) {
        size_t want = static_cast<size_t>(idx);
        while (f.stack.size() < want)
            luaapi_push(L, clx::LValue());
        luaapi_truncate(L, want > f.stack.size() ? f.stack.size() : want);
    } else {
        luaapi_pop(L, -idx - 1);
    }
}

void lua_pushvalue(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    luaapi_push(L, v ? *v : clx::LValue());
}

void lua_rotate(lua_State *L, int idx, int n) {
    ApiFrame &f = luaapi_frame(L);
    long long base = lua_absindex(L, idx) - 1;
    if (base < 0 || static_cast<size_t>(base) >= f.stack.size())
        return;
    size_t off = static_cast<size_t>(base);
    size_t k = f.stack.size() - off;
    if (k <= 1)
        return;
    long long shift = (n >= 0) ? (static_cast<long long>(k) - n) : -static_cast<long long>(n);
    if (shift < 0)
        shift = 0;
    if (static_cast<size_t>(shift) > k)
        shift = static_cast<long long>(k);
    std::rotate(f.stack.begin() + static_cast<long long>(off), f.stack.begin() + static_cast<long long>(off) + shift,
        f.stack.end());
}

void lua_copy(lua_State *L, int fromidx, int toidx) {
    clx::LValue *from = luaapi_locate(L, fromidx);
    clx::LValue *to = luaapi_locate(L, toidx);
    if (from && to)
        *to = *from;
}

int lua_checkstack(lua_State *L, int n) {
    (void)L;
    (void)n;
    return 1;
}

void lua_xmove(lua_State *from, lua_State *to, int n) {
    if (from == to)
        return;
    if (n <= 0)
        return;
    if (from->L != to->L)
        clx::error(from->L, "moving among independent states is not supported");
    if (from->frames.empty())
        return;
    ApiFrame &ff = from->frames.back();
    size_t sz = ff.stack.size();
    size_t cnt = (static_cast<size_t>(n) > sz) ? sz : static_cast<size_t>(n);
    if (cnt == 0)
        return;
    size_t first = sz - cnt;
    for (size_t i = 0; i < cnt; ++i)
        luaapi_push(to, ff.stack[first + i]);
    luaapi_truncate(from, first);
}

//=========================== access functions ===========================

int lua_isnumber(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v)
        return 0;
    if (v->type == clx::ValueType::Int64 || v->type == clx::ValueType::Double)
        return 1;
    if (v->type == clx::ValueType::String) {
        clx::LValue tmp;
        return luaapi_str2num(v->as_string(), tmp) ? 1 : 0;
    }
    return 0;
}

int lua_isstring(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v)
        return 0;
    if (v->type == clx::ValueType::String)
        return 1;
    return lua_isnumber(L, idx);
}

int lua_iscfunction(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v || v->type != clx::ValueType::Function)
        return 0;
    return luaapi_iscfn(static_cast<clx::LCFunction *>(v->as_pointer())) ? 1 : 0;
}

int lua_isinteger(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    return (v && v->type == clx::ValueType::Int64) ? 1 : 0;
}

int lua_isuserdata(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    return (v && v->type == clx::ValueType::UserData) ? 1 : 0;
}

int lua_type(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v)
        return LUA_TNONE;
    return luaapi_tag(L->L, *v);
}

const char *lua_typename(lua_State *L, int tp) {
    (void)L;
    if (tp < LUA_TNONE || tp >= LUA_NUMTYPES)
        return "no value";
    return luaapi_type_names[tp + 1];
}

lua_Number lua_tonumberx(lua_State *L, int idx, int *isnum) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v) {
        if (isnum)
            *isnum = 0;
        return 0;
    }
    if (v->type == clx::ValueType::Double) {
        if (isnum)
            *isnum = 1;
        return v->val.payload.f64;
    }
    if (v->type == clx::ValueType::Int64) {
        if (isnum)
            *isnum = 1;
        return static_cast<lua_Number>(v->val.payload.i64);
    }
    if (v->type == clx::ValueType::String) {
        clx::LValue tmp;
        if (luaapi_str2num(v->as_string(), tmp)) {
            if (isnum)
                *isnum = 1;
            return tmp.as_number();
        }
    }
    if (isnum)
        *isnum = 0;
    return 0;
}

lua_Integer lua_tointegerx(lua_State *L, int idx, int *isnum) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v) {
        if (isnum)
            *isnum = 0;
        return 0;
    }
    if (v->type == clx::ValueType::Int64) {
        if (isnum)
            *isnum = 1;
        return v->val.payload.i64;
    }
    if (v->type == clx::ValueType::Double) {
        int64_t i;
        if (clx::flt_to_integer(v->val.payload.f64, i)) {
            if (isnum)
                *isnum = 1;
            return i;
        }
    } else if (v->type == clx::ValueType::String) {
        int64_t i;
        if (luaapi_str2int(v->as_string(), i)) {
            if (isnum)
                *isnum = 1;
            return i;
        }
    }
    if (isnum)
        *isnum = 0;
    return 0;
}

int lua_toboolean(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v)
        return 0;
    return v->as_bool() ? 1 : 0;
}

const char *lua_tolstring(lua_State *L, int idx, size_t *len) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v) {
        if (len)
            *len = 0;
        return nullptr;
    }
    clx::LState *S = L->L;
    if (v->type == clx::ValueType::Int64 || v->type == clx::ValueType::Double) {
        std::string s = v->to_string(S);
        *v = clx::LValue(S->intern_string(s));
    } else if (v->type != clx::ValueType::String) {
        if (len)
            *len = 0;
        return nullptr;
    }
    if ((v->val.payload.u64 >> 56) != 0)
        *v = clx::LValue(S->intern_string(reinterpret_cast<const char *>(&v->val.payload.u64), v->string_len()));
    const char *s = v->as_string();
    if (len)
        *len = v->string_len();
    return s;
}

lua_Unsigned lua_rawlen(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v)
        return 0;
    if (v->type == clx::ValueType::String || v->type == clx::ValueType::Table)
        return static_cast<lua_Unsigned>(clx::rawlen(*v));
    if (v->type == clx::ValueType::UserData && luaapi_full_ud(L->L, *v))
        return static_cast<lua_Unsigned>(static_cast<clx::LUserdata *>(v->as_pointer())->size);
    return 0;
}

lua_CFunction lua_tocfunction(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v || v->type != clx::ValueType::Function)
        return nullptr;
    clx::LCFunction *f = static_cast<clx::LCFunction *>(v->as_pointer());
    const CFuncAdapter *ad = f->func.target<CFuncAdapter>();
    return ad ? ad->fn : nullptr;
}

void *lua_touserdata(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v || v->type != clx::ValueType::UserData)
        return nullptr;
    clx::LHeader *p = v->as_pointer();
    if (!p)
        return nullptr;
    if (L->L->is_allocated_userdata(p))
        return static_cast<clx::LUserdata *>(p)->data();
    return p;
}

lua_State *lua_tothread(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v || v->type != clx::ValueType::Thread)
        return nullptr;
    clx::LThread *th = static_cast<clx::LThread *>(v->as_pointer());
    if (!th || th->state != L->L)
        return nullptr;
    return luaapi_for_thread(th);
}

const void *lua_topointer(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v)
        return nullptr;
    using clx::ValueType;
    switch (v->type) {
    case ValueType::String:
        return v->as_string();
    case ValueType::Table:
    case ValueType::Function:
    case ValueType::Thread:
        return v->as_pointer();
    case ValueType::UserData:
        return luaapi_full_ud(L->L, *v) ? static_cast<clx::LUserdata *>(v->as_pointer())->data()
                                        : static_cast<const void *>(v->as_pointer());
    default:
        return nullptr;
    }
}

//=========================== comparison and arithmetic ===========================

void lua_arith(lua_State *L, int op) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    bool unary = (op == LUA_OPUNM || op == LUA_OPBNOT);
    if (unary) {
        if (f.stack.empty())
            clx::error(S, "not enough values for arithmetic");
        luaapi_push(L, f.stack.back());
    } else if (f.stack.size() < 2) {
        clx::error(S, "not enough values for arithmetic");
    }
    size_t n = f.stack.size();
    const clx::LValue &a = f.stack[n - 2];
    const clx::LValue &b = f.stack[n - 1];
    clx::LValue r;
    switch (op) {
    case LUA_OPADD:
        r = clx::add(S, a, b);
        break;
    case LUA_OPSUB:
        r = clx::sub(S, a, b);
        break;
    case LUA_OPMUL:
        r = clx::mul(S, a, b);
        break;
    case LUA_OPMOD:
        r = clx::mod(S, a, b);
        break;
    case LUA_OPPOW:
        r = clx::pow(S, a, b);
        break;
    case LUA_OPDIV:
        r = clx::div(S, a, b);
        break;
    case LUA_OPIDIV:
        r = clx::idiv(S, a, b);
        break;
    case LUA_OPBAND:
        r = clx::band(S, a, b);
        break;
    case LUA_OPBOR:
        r = clx::bor(S, a, b);
        break;
    case LUA_OPBXOR:
        r = clx::bxor(S, a, b);
        break;
    case LUA_OPSHL:
        r = clx::shl(S, a, b);
        break;
    case LUA_OPSHR:
        r = clx::shr(S, a, b);
        break;
    case LUA_OPUNM:
        r = clx::unm(S, a);
        break;
    case LUA_OPBNOT:
        r = clx::bnot(S, a);
        break;
    default:
        clx::error(S, "invalid arithmetic option");
    }
    luaapi_truncate(L, n - 2);
    luaapi_push(L, r);
}

int lua_rawequal(lua_State *L, int idx1, int idx2) {
    clx::LValue *a = luaapi_locate(L, idx1);
    clx::LValue *b = luaapi_locate(L, idx2);
    if (!a || !b)
        return 0;
    return clx::rawequal(*a, *b) ? 1 : 0;
}

int lua_compare(lua_State *L, int idx1, int idx2, int op) {
    clx::LValue *a = luaapi_locate(L, idx1);
    clx::LValue *b = luaapi_locate(L, idx2);
    if (!a || !b)
        return 0;
    clx::LState *S = L->L;
    switch (op) {
    case LUA_OPEQ:
        return clx::eq(S, *a, *b).as_bool() ? 1 : 0;
    case LUA_OPLT:
        return clx::lt(S, *a, *b).as_bool() ? 1 : 0;
    case LUA_OPLE:
        return clx::le(S, *a, *b).as_bool() ? 1 : 0;
    default:
        return 0;
    }
}

//=========================== push functions ===========================

void lua_pushnil(lua_State *L) {
    luaapi_push(L, clx::LValue());
}

void lua_pushnumber(lua_State *L, lua_Number n) {
    luaapi_push(L, clx::LValue(static_cast<double>(n)));
}

void lua_pushinteger(lua_State *L, lua_Integer n) {
    luaapi_push(L, clx::LValue(static_cast<int64_t>(n)));
}

const char *lua_pushlstring(lua_State *L, const char *s, size_t len) {
    if (!s) {
        luaapi_push(L, clx::LValue());
        return nullptr;
    }
    const char *p = L->L->intern_string(s, len);
    luaapi_push(L, clx::LValue(p));
    return p;
}

const char *lua_pushexternalstring(lua_State *L, const char *s, size_t len, lua_Alloc falloc, void *ud) {
    if (!s) {
        luaapi_push(L, clx::LValue());
        return nullptr;
    }
    const char *p = L->L->intern_string(s, len);
    luaapi_push(L, clx::LValue(p));
    if (falloc)
        falloc(ud, const_cast<char *>(s), len + 1, 0);
    return p;
}

const char *lua_pushstring(lua_State *L, const char *s) {
    if (!s) {
        luaapi_push(L, clx::LValue());
        return nullptr;
    }
    return lua_pushlstring(L, s, std::strlen(s));
}

const char *lua_pushvfstring(lua_State *L, const char *fmt, va_list argp) {
    std::string out;
    const char *fmt0 = fmt;
    const char *e;
    while ((e = std::strchr(fmt0, '%')) != nullptr) {
        out.append(fmt0, static_cast<size_t>(e - fmt0));
        if (e[1] == '\0') {
            out.push_back('%');
            fmt0 = e + 1;
            break;
        }
        switch (e[1]) {
        case 's': {
            const char *s = va_arg(argp, const char *);
            out += s ? s : "(null)";
            break;
        }
        case 'c': {
            out += static_cast<char>(va_arg(argp, int));
            break;
        }
        case 'd': {
            char buf[32];
            long long i = static_cast<long long>(va_arg(argp, int));
            std::snprintf(buf, sizeof(buf), "%lld", i);
            out += buf;
            break;
        }
        case 'I': {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(va_arg(argp, lua_Integer)));
            out += buf;
            break;
        }
        case 'f': {
            char buf[LUA_N2SBUFFSZ];
            int n = clx::clx_format_double(buf, sizeof(buf), va_arg(argp, lua_Number));
            out.append(buf, static_cast<size_t>(n));
            break;
        }
        case 'p': {
            char buf[LUA_N2SBUFFSZ];
            std::snprintf(buf, sizeof(buf), "%p", va_arg(argp, void *));
            out += buf;
            break;
        }
        case 'U': {
            char buf[8];
            unsigned long code = va_arg(argp, unsigned long);
            int n = luaapi_utf8esc(buf, static_cast<uint32_t>(code));
            out.append(buf + 8 - n, static_cast<size_t>(n));
            break;
        }
        case '%': {
            out += '%';
            break;
        }
        default: {
            out.push_back('%');
            if (e[1])
                out.push_back(e[1]);
            break;
        }
        }
        fmt0 = e + 2;
    }
    out += fmt0;
    const char *p = L->L->intern_string(out);
    luaapi_push(L, clx::LValue(p));
    return p;
}

const char *lua_pushfstring(lua_State *L, const char *fmt, ...) {
    va_list argp;
    va_start(argp, fmt);
    const char *r = lua_pushvfstring(L, fmt, argp);
    va_end(argp);
    return r;
}

void lua_pushcclosure(lua_State *L, lua_CFunction fn, int n) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (n < 0)
        n = 0;
    if (static_cast<size_t>(n) > f.stack.size())
        n = static_cast<int>(f.stack.size());
    auto cells = std::make_shared<std::vector<clx::LUpValue>>();
    cells->reserve(static_cast<size_t>(n));
    size_t start = f.stack.size() - static_cast<size_t>(n);
    for (size_t i = 0; i < static_cast<size_t>(n); ++i)
        cells->push_back(std::make_shared<clx::LValue>(f.stack[start + i]));
    CFuncAdapter ad;
    ad.fn = fn;
    ad.ups = cells;
    clx::LValue clo = S->create_closure(ad, nullptr, *cells);
    luaapi_truncate(L, start);
    luaapi_push(L, clo);
}

void lua_pushboolean(lua_State *L, int b) {
    luaapi_push(L, clx::LValue(b != 0));
}

void lua_pushlightuserdata(lua_State *L, void *p) {
    luaapi_push(L, clx::LValue(clx::ValueType::UserData, static_cast<clx::LHeader *>(p)));
}

int lua_pushthread(lua_State *L) {
    clx::LState *S = L->L;
    clx::LThread *th = luaapi_self_thread(L);
    if (!th) {
        luaapi_push(L, clx::LValue());
        return 0;
    }
    luaapi_push(L, clx::LValue(clx::ValueType::Thread, th));
    return (S->main_thread && th == S->main_thread) ? 1 : 0;
}

//=========================== get functions ===========================

int lua_getglobal(lua_State *L, const char *name) {
    clx::LState *S = L->L;
    clx::LValue key(S->intern_string(name));
    clx::LValue r = clx::table_get(S, clx::LValue(clx::ValueType::Table, S->_G), key);
    luaapi_push(L, r);
    return luaapi_tag(S, r);
}

int lua_gettable(lua_State *L, int idx) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    clx::LValue *t = luaapi_locate(L, idx);
    if (f.stack.empty())
        clx::error(S, "not enough values on the stack");
    clx::LValue key = f.stack.back();
    clx::LValue r;
    if (t && t->type == clx::ValueType::UserData && !luaapi_full_ud(S, *t))
        r = clx::LValue();
    else if (t)
        r = clx::table_get(S, *t, key);
    luaapi_truncate(L, f.stack.size() - 1);
    luaapi_push(L, r);
    return luaapi_tag(S, r);
}

int lua_getfield(lua_State *L, int idx, const char *k) {
    clx::LState *S = L->L;
    clx::LValue *t = luaapi_locate(L, idx);
    clx::LValue key(S->intern_string(k ? k : ""));
    clx::LValue r;
    if (t && t->type == clx::ValueType::UserData && !luaapi_full_ud(S, *t))
        r = clx::LValue();
    else if (t)
        r = clx::table_get(S, *t, key);
    luaapi_push(L, r);
    return luaapi_tag(S, r);
}

int lua_geti(lua_State *L, int idx, lua_Integer n) {
    clx::LState *S = L->L;
    clx::LValue *t = luaapi_locate(L, idx);
    clx::LValue key(static_cast<int64_t>(n));
    clx::LValue r;
    if (t && t->type == clx::ValueType::UserData && !luaapi_full_ud(S, *t))
        r = clx::LValue();
    else if (t)
        r = clx::table_get(S, *t, key);
    luaapi_push(L, r);
    return luaapi_tag(S, r);
}

int lua_rawget(lua_State *L, int idx) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    clx::LValue *t = luaapi_locate(L, idx);
    if (!t)
        return luaL_error(L, "table expected");
    if (f.stack.empty())
        clx::error(S, "not enough values on the stack");
    clx::LValue key = f.stack.back();
    clx::LValue r = clx::raw_get(S, *t, key);
    luaapi_truncate(L, f.stack.size() - 1);
    luaapi_push(L, r);
    return luaapi_tag(S, r);
}

int lua_rawgeti(lua_State *L, int idx, lua_Integer n) {
    clx::LState *S = L->L;
    clx::LValue *t = luaapi_locate(L, idx);
    if (!t)
        return luaL_error(L, "table expected");
    clx::LValue r = clx::raw_get(S, *t, clx::LValue(static_cast<int64_t>(n)));
    luaapi_push(L, r);
    return luaapi_tag(S, r);
}

int lua_rawgetp(lua_State *L, int idx, const void *p) {
    clx::LState *S = L->L;
    clx::LValue *t = luaapi_locate(L, idx);
    if (!t)
        return luaL_error(L, "table expected");
    clx::LValue key(clx::ValueType::UserData, static_cast<clx::LHeader *>(const_cast<void *>(p)));
    clx::LValue r = clx::raw_get(S, *t, key);
    luaapi_push(L, r);
    return luaapi_tag(S, r);
}

void lua_createtable(lua_State *L, int narr, int nrec) {
    clx::LState *S = L->L;
    luaapi_push(L, S->create_table(narr > 0 ? static_cast<size_t>(narr) : 0, nrec > 0 ? static_cast<size_t>(nrec) : 0));
}

void *lua_newuserdatauv(lua_State *L, size_t sz, int nuvalue) {
    clx::LValue v = clx::newuserdata(L->L, sz);
    luaapi_push(L, v);
    (void)nuvalue;
    return static_cast<clx::LUserdata *>(v.as_pointer())->data();
}

int lua_getmetatable(lua_State *L, int objindex) {
    clx::LState *S = L->L;
    clx::LValue *v = luaapi_locate(L, objindex);
    if (!v)
        return 0;
    clx::LTable *mt = nullptr;
    using clx::ValueType;
    switch (v->type) {
    case ValueType::Table:
        mt = clx::tbl_metatable(static_cast<clx::LTable *>(v->as_pointer()));
        break;
    case ValueType::UserData: {
        clx::LHeader *p = v->as_pointer();
        if (p && S->is_allocated_userdata(p) && p->type == static_cast<uint8_t>(ValueType::UserData))
            mt = static_cast<clx::LUserdata *>(p)->metatable;
        break;
    }
    case ValueType::String:
        mt = S->string_metatable;
        break;
    default:
        break;
    }
    if (!mt)
        return 0;
    luaapi_push(L, clx::LValue(clx::ValueType::Table, mt));
    return 1;
}

int lua_getiuservalue(lua_State *L, int idx, int n) {
    clx::LState *S = L->L;
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v || v->type != clx::ValueType::UserData || n <= 0) {
        luaapi_push(L, clx::LValue());
        return LUA_TNONE;
    }
    clx::LValue per = clx::raw_get(S, luaapi_fetch(S)->uservalues, *v);
    clx::LValue r;
    if (per.type == clx::ValueType::Table)
        r = clx::raw_get(S, per, clx::LValue(static_cast<int64_t>(n)));
    luaapi_push(L, r);
    return luaapi_tag(S, r);
}

//=========================== set functions ===========================

void lua_setglobal(lua_State *L, const char *name) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (f.stack.empty())
        clx::error(S, "not enough values on the stack");
    clx::LValue key(S->intern_string(name));
    clx::table_set(S, clx::LValue(clx::ValueType::Table, S->_G), key, f.stack.back());
    luaapi_truncate(L, f.stack.size() - 1);
}

void lua_settable(lua_State *L, int idx) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (f.stack.size() < 2)
        clx::error(S, "not enough values on the stack");
    clx::LValue *t = luaapi_locate(L, idx);
    if (t) {
        if (t->type == clx::ValueType::UserData && !luaapi_full_ud(S, *t)) {
            luaapi_truncate(L, f.stack.size() - 2);
            return;
        }
        clx::table_set(S, *t, f.stack[f.stack.size() - 2], f.stack.back());
    }
    luaapi_truncate(L, f.stack.size() - 2);
}

void lua_setfield(lua_State *L, int idx, const char *k) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (f.stack.empty())
        clx::error(S, "not enough values on the stack");
    clx::LValue *t = luaapi_locate(L, idx);
    clx::LValue key(S->intern_string(k ? k : ""));
    if (t) {
        if (t->type == clx::ValueType::UserData && !luaapi_full_ud(S, *t)) {
            luaapi_truncate(L, f.stack.size() - 1);
            return;
        }
        clx::table_set(S, *t, key, f.stack.back());
    }
    luaapi_truncate(L, f.stack.size() - 1);
}

void lua_seti(lua_State *L, int idx, lua_Integer n) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (f.stack.empty())
        clx::error(S, "not enough values on the stack");
    clx::LValue *t = luaapi_locate(L, idx);
    clx::LValue key(static_cast<int64_t>(n));
    if (t) {
        if (t->type == clx::ValueType::UserData && !luaapi_full_ud(S, *t)) {
            luaapi_truncate(L, f.stack.size() - 1);
            return;
        }
        clx::table_set(S, *t, key, f.stack.back());
    }
    luaapi_truncate(L, f.stack.size() - 1);
}

void lua_rawset(lua_State *L, int idx) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (f.stack.size() < 2)
        clx::error(S, "not enough values on the stack");
    clx::LValue *t = luaapi_locate(L, idx);
    if (!t) {
        luaL_error(L, "table expected");
        return;
    }
    clx::raw_set(S, *t, f.stack[f.stack.size() - 2], f.stack.back());
    luaapi_truncate(L, f.stack.size() - 2);
}

void lua_rawseti(lua_State *L, int idx, lua_Integer n) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (f.stack.empty())
        clx::error(S, "not enough values on the stack");
    clx::LValue *t = luaapi_locate(L, idx);
    if (!t) {
        luaL_error(L, "table expected");
        return;
    }
    clx::raw_set(S, *t, clx::LValue(static_cast<int64_t>(n)), f.stack.back());
    luaapi_truncate(L, f.stack.size() - 1);
}

void lua_rawsetp(lua_State *L, int idx, const void *p) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (f.stack.empty())
        clx::error(S, "not enough values on the stack");
    clx::LValue *t = luaapi_locate(L, idx);
    if (!t) {
        luaL_error(L, "table expected");
        return;
    }
    clx::LValue key(clx::ValueType::UserData, static_cast<clx::LHeader *>(const_cast<void *>(p)));
    clx::raw_set(S, *t, key, f.stack.back());
    luaapi_truncate(L, f.stack.size() - 1);
}

int lua_setmetatable(lua_State *L, int objindex) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (f.stack.empty())
        return 1;
    clx::LValue *v = luaapi_locate(L, objindex);
    clx::LValue mtv = f.stack.back();
    if (v) {
        using clx::ValueType;
        if (v->type == ValueType::Table || (v->type == ValueType::UserData && luaapi_full_ud(S, *v))) {
            clx::setmetatable(S, *v, mtv);
        } else if (v->type == ValueType::String) {
            clx::LTable *nw = (mtv.type == ValueType::Table) ? static_cast<clx::LTable *>(mtv.as_pointer()) : nullptr;
            if (nw && nw != S->string_metatable) {
                S->string_metatable = nw;
                S->root_value(mtv);
            } else if (!nw) {
                S->string_metatable = nullptr;
            }
        }
    }
    luaapi_truncate(L, f.stack.size() - 1);
    return 1;
}

int lua_setiuservalue(lua_State *L, int idx, int n) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (f.stack.empty())
        return 0;
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v || v->type != clx::ValueType::UserData || n <= 0) {
        luaapi_truncate(L, f.stack.size() - 1);
        return 0;
    }
    clx::LValue per = clx::raw_get(S, luaapi_fetch(S)->uservalues, *v);
    if (per.type != clx::ValueType::Table) {
        per = S->create_table(0, 4);
        clx::raw_set(S, luaapi_fetch(S)->uservalues, *v, per);
    }
    clx::raw_set(S, per, clx::LValue(static_cast<int64_t>(n)), f.stack.back());
    luaapi_truncate(L, f.stack.size() - 1);
    return 1;
}

//=========================== load and call ===========================

//---------- luaapi_call: shared body of lua_callk / lua_pcallk
static int luaapi_call(lua_State *L, int nargs, int nresults, int errfunc, bool protect, bool yieldable) {
    //---------- PcallDepthGuard: stock marks protected frames without a continuation non-yieldable, which lua_yieldk must reproduce
    struct PcallDepthGuard {
        lua_State *ls;
        bool armed;

        PcallDepthGuard(lua_State *s, bool on)
            : ls(s)
            , armed(on) {
            if (armed)
                ls->pcall_depth++;
        }

        ~PcallDepthGuard() {
            if (armed)
                ls->pcall_depth--;
        }
    } guard(L, protect && !yieldable);

    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (nargs < 0)
        nargs = 0;
    size_t sz = f.stack.size();
    if (static_cast<size_t>(nargs) > sz)
        nargs = static_cast<int>(sz);
    size_t func_i = sz - static_cast<size_t>(nargs);

    clx::LValue fn;
    std::vector<clx::LValue> args;
    if (func_i == 0)
        clx::error(S, "call stack underflow");
    fn = f.stack[func_i - 1];
    args.assign(f.stack.begin() + static_cast<long long>(func_i), f.stack.end());

    clx::LValue errf;
    if (errfunc != 0) {
        int ei = lua_absindex(L, errfunc);
        clx::LValue *ep = luaapi_locate(L, ei);
        if (ep)
            errf = *ep;
    }
    S->root_value(errf);
    luaapi_truncate(L, func_i - 1);
    func_i -= 1;

    auto push_results = [&](const clx::MultiValue &ret) {
        size_t got = ret.count;
        if (nresults == LUA_MULTRET) {
            for (size_t i = 0; i < got; ++i)
                luaapi_push(L, ret[i]);
        } else {
            for (int i = 0; i < nresults; ++i)
                luaapi_push(L, i < static_cast<int>(got) ? ret[i] : clx::LValue());
        }
    };

    auto push_error = [&](const clx::LValue &errv) -> int {
        if (errf.type == clx::ValueType::Function) {
            try {
                clx::LValue call_args[2] = { errf, errv };
                clx::MultiValue r2 = clx::call_function_rooted(S, errf, call_args, 2, "C API", 0);
                luaapi_truncate(L, func_i);
                if (r2.count > 0)
                    luaapi_push(L, r2[0]);
                else
                    luaapi_push(L, clx::LValue());
                return LUA_ERRRUN;
            } catch (...) {
                luaapi_truncate(L, func_i);
                luaapi_push(L, clx::LValue(S->intern_string("error in error handling")));
                return LUA_ERRERR;
            }
        }
        luaapi_truncate(L, func_i);
        luaapi_push(L, errv);
        return LUA_ERRRUN;
    };

    try {
        clx::MultiValue ret = clx::call_function_rooted(S, fn, args.data(), args.size(), "C API", 0);
        S->unroot_value(errf);
        luaapi_truncate(L, func_i);
        push_results(ret);
        return LUA_OK;
    } catch (clx::LRuntimeException &e) {
        clx::LValue errv = e.error_obj;
        S->root_value(errv);
        S->unroot_value(errf);
        int st = push_error(errv);
        S->unroot_value(errv);
        if (!protect)
            throw;
        return st;
    } catch (std::exception &e) {
        clx::LValue errv(S->intern_string(e.what() ? e.what() : "unknown error"));
        S->root_value(errv);
        S->unroot_value(errf);
        int st = push_error(errv);
        S->unroot_value(errv);
        if (!protect)
            throw;
        return st;
    } catch (...) {
        clx::LValue errv(S->intern_string("unknown error"));
        S->root_value(errv);
        S->unroot_value(errf);
        int st = push_error(errv);
        S->unroot_value(errv);
        if (!protect)
            throw;
        return st;
    }
}

void lua_callk(lua_State *L, int nargs, int nresults, lua_KContext ctx, lua_KFunction k) {
    clx::LState *S = L->L;
    clx::LThread *self = S->running_thread;
    int y0 = self ? self->yield_count : 0;
    luaapi_call(L, nargs, nresults, 0, false, true);
    if (k && self && self->yield_count != y0 && L->catching_frames > 0 && L == luaapi_fetch_current(S)) {
        int n = k(L, LUA_YIELD, ctx);
        throw CFrameReturn { n };
    }
}

int lua_pcallk(lua_State *L, int nargs, int nresults, int errfunc, lua_KContext ctx, lua_KFunction k) {
    clx::LState *S = L->L;
    clx::LThread *self = S->running_thread;
    int y0 = self ? self->yield_count : 0;
    int st = luaapi_call(L, nargs, nresults, errfunc, true, k != nullptr);
    if (k && self && self->yield_count != y0 && L->catching_frames > 0 && L == luaapi_fetch_current(S)) {
        st = k(L, st == LUA_OK ? LUA_YIELD : st, ctx);
        throw CFrameReturn { st };
    }
    return st;
}

int lua_load(lua_State *L, lua_Reader reader, void *dt, const char *chunkname, const char *mode) {
    (void)reader;
    (void)dt;
    (void)mode;
    std::string msg = std::string("clx: lua_load is not supported (chunk '") + (chunkname ? chunkname : "?") + "')";
    luaapi_push(L, clx::LValue(L->L->intern_string(msg)));
    return LUA_ERRSYNTAX;
}

int lua_dump(lua_State *L, lua_Writer writer, void *data, int strip) {
    (void)L;
    (void)writer;
    (void)data;
    (void)strip;
    return 1;
}

//=========================== coroutine functions ===========================

//---------- lua_yieldk: hand the top nresults values to the resumer and switch back to it
int lua_yieldk(lua_State *L, int nresults, lua_KContext ctx, lua_KFunction k) {
    clx::LState *S = L->L;
    clx::LThread *self = luaapi_self_thread(L);
    if (!self || self == S->main_thread || self->status != clx::THREAD_RUNNING)
        clx::error(S, "attempt to yield from outside a coroutine");
    if (L->pcall_depth > 0)
        clx::error(S, "attempt to yield across a C-call boundary");

    ApiFrame &f = luaapi_frame(L);
    size_t sz = f.stack.size();
    size_t n = (nresults <= 0) ? 0 : ((static_cast<size_t>(nresults) > sz) ? sz : static_cast<size_t>(nresults));
    std::vector<clx::LValue> vals(f.stack.end() - static_cast<long long>(n), f.stack.end());
    luaapi_truncate(L, sz - n);

    clx::MultiValue ra = clx::yield(S, vals.data(), vals.size());
    for (size_t i = 0; i < ra.count; ++i)
        luaapi_push(L, ra[i]);
    if (k)
        return k(L, LUA_YIELD, ctx);
    return static_cast<int>(ra.count);
}

int lua_resume(lua_State *L, lua_State *from, int narg, int *nres) {
    clx::LState *S = L->L;
    if (nres)
        *nres = 0;
    if (from && from->L != S)
        clx::error(S, "cannot resume a coroutine that belongs to a different state");
    clx::LThread *t = L->thread;
    if (!t)
        return luaapi_resume_error(L, "cannot resume non-suspended coroutine", narg, nres);
    if (t->status == clx::THREAD_RUNNING || t->status == clx::THREAD_NORMAL)
        return luaapi_resume_error(L, "cannot resume non-suspended coroutine", narg, nres);
    if (t->status == clx::THREAD_DEAD)
        return luaapi_resume_error(L, "cannot resume dead coroutine", narg, nres);

    ApiFrame &f = luaapi_frame(L);
    size_t sz = f.stack.size();
    size_t na = (narg <= 0) ? 0 : ((static_cast<size_t>(narg) > sz) ? sz : static_cast<size_t>(narg));
    clx::LValue fn;
    size_t arg_start;
    size_t keep;
    if (!t->started) {
        if (sz > na) {
            fn = f.stack[sz - na - 1];
            arg_start = sz - na;
            keep = arg_start - 1;
        } else if (t->function.type != clx::ValueType::Nil) {
            fn = t->function;
            arg_start = sz - na;
            keep = arg_start;
        } else {
            return luaapi_resume_error(L, "cannot resume dead coroutine", narg, nres);
        }
        t->function = fn;
    } else {
        arg_start = sz - na;
        keep = arg_start;
    }
    std::vector<clx::LValue> args(
        f.stack.begin() + static_cast<long long>(arg_start), f.stack.begin() + static_cast<long long>(sz));
    luaapi_truncate(L, keep);

    clx::MultiValue r = clx::resume(S, clx::LValue(clx::ValueType::Thread, t), args.data(), args.size());
    bool ok = (r.count > 0) && r[0].as_bool();
    size_t nout = (r.count > 0) ? r.count - 1 : 0;
    if (ok) {
        for (size_t i = 0; i < nout; ++i)
            luaapi_push(L, r[i + 1]);
        if (nres)
            *nres = static_cast<int>(nout);
        return (t->status == clx::THREAD_DEAD) ? LUA_OK : LUA_YIELD;
    }
    luaapi_push(L, (nout > 0) ? r[1] : clx::LValue(S->intern_string("coroutine error")));
    if (nres)
        *nres = 1;
    return LUA_ERRRUN;
}

int lua_status(lua_State *L) {
    clx::LThread *t = L->thread;
    if (!t)
        return LUA_OK;
    switch (t->status) {
    case clx::THREAD_DEAD:
        return t->has_error ? LUA_ERRRUN : LUA_OK;
    case clx::THREAD_SUSPENDED:
        return t->started ? LUA_YIELD : LUA_OK;
    default:
        return LUA_OK;
    }
}

int lua_isyieldable(lua_State *L) {
    clx::LThread *t = luaapi_self_thread(L);
    if (!t || t == L->L->main_thread)
        return 0;
    return (L->pcall_depth == 0) ? 1 : 0;
}

//=========================== warnings ===========================

void lua_setwarnf(lua_State *L, lua_WarnFunction f, void *ud) {
    L->warnf = f;
    L->warnf_ud = ud;
}

void lua_warning(lua_State *L, const char *msg, int tocont) {
    if (L->warnf)
        L->warnf(L->warnf_ud, msg, tocont);
}

//=========================== garbage collection ===========================

int lua_gc(lua_State *L, int what, ...) {
    clx::LState *S = L->L;
    va_list ap;
    va_start(ap, what);
    int res = 0;
    switch (what) {
    case LUA_GCSTOP:
        S->gc_running = false;
        break;
    case LUA_GCRESTART:
        S->gc_running = true;
        break;
    case LUA_GCCOLLECT:
        S->collect_garbage();
        break;
    case LUA_GCCOUNT:
        res = static_cast<int>(S->allocated_bytes >> 10);
        break;
    case LUA_GCCOUNTB:
        res = static_cast<int>(S->allocated_bytes & 0x3FF);
        break;
    case LUA_GCSTEP:
        res = S->gc_step() ? 1 : 0;
        break;
    case LUA_GCISRUNNING:
        res = S->gc_running ? 1 : 0;
        break;
    case LUA_GCGEN:
    case LUA_GCINC: {
        bool want_gen = (what == LUA_GCGEN);
        res = (S->gc_mode == clx::LState::GCMode::Generational) ? 1 : 0;
        S->gc_mode = want_gen ? clx::LState::GCMode::Generational : clx::LState::GCMode::Incremental;
        break;
    }
    case LUA_GCPARAM: {
        int id = va_arg(ap, int);
        int val = va_arg(ap, int);
        switch (id) {
        case LUA_GCPPAUSE: {
            res = S->gc_pause;
            S->gc_pause = val;
            break;
        }
        case LUA_GCPSTEPMUL: {
            res = S->gc_stepmul;
            S->gc_stepmul = val;
            break;
        }
        case LUA_GCPSTEPSIZE: {
            res = S->gc_stepsize;
            S->gc_stepsize = val;
            break;
        }
        case LUA_GCPMINORMUL: {
            res = S->gc_minormul;
            S->gc_minormul = val;
            break;
        }
        case LUA_GCPMAJORMINOR: {
            res = S->gc_majorminor;
            S->gc_majorminor = val;
            break;
        }
        case LUA_GCPMINORMAJOR: {
            res = S->gc_minormajor;
            S->gc_minormajor = val;
            break;
        }
        default:
            res = -1;
            break;
        }
        break;
    }
    default:
        res = -1;
        break;
    }
    va_end(ap);
    return res;
}

//=========================== miscellaneous ===========================

int lua_error(lua_State *L) {
    ApiFrame &f = luaapi_frame(L);
    clx::LValue v = f.stack.empty() ? clx::LValue() : f.stack.back();
    throw clx::LRuntimeException(v);
}

int lua_next(lua_State *L, int idx) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    clx::LValue *t = luaapi_locate(L, idx);
    if (!t || f.stack.empty())
        clx::error(L->L, "table expected");
    clx::LValue key = f.stack.back();
    clx::MultiValue mv = clx::next(S, *t, key);
    if (mv.count >= 2) {
        luaapi_truncate(L, f.stack.size() - 1);
        luaapi_push(L, mv[0]);
        luaapi_push(L, mv[1]);
        return 1;
    }
    luaapi_truncate(L, f.stack.size() - 1);
    return 0;
}

void lua_concat(lua_State *L, int n) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    if (n <= 0) {
        luaapi_push(L, clx::LValue(S->intern_string("", 0)));
        return;
    }
    size_t sz = f.stack.size();
    if (static_cast<size_t>(n) > sz)
        n = static_cast<int>(sz);
    if (n <= 0)
        return;
    size_t base = sz - static_cast<size_t>(n);
    for (size_t i = base + 1; i < sz; ++i)
        f.stack[base] = clx::concat(S, f.stack[base], f.stack[i]);
    luaapi_truncate(L, base + 1);
}

void lua_len(lua_State *L, int idx) {
    clx::LValue *v = luaapi_locate(L, idx);
    clx::LValue r = v ? clx::len(L->L, *v) : clx::LValue(static_cast<int64_t>(0));
    luaapi_push(L, r);
}

unsigned lua_numbertocstring(lua_State *L, int idx, char *buff) {
    clx::LValue *v = luaapi_locate(L, idx);
    if (!v)
        return 0;
    if (v->type == clx::ValueType::Int64) {
        int n = std::snprintf(buff, LUA_N2SBUFFSZ, "%lld", static_cast<long long>(v->val.payload.i64));
        if (n < 0)
            return 0;
        if (n >= LUA_N2SBUFFSZ)
            n = LUA_N2SBUFFSZ - 1;
        buff[n] = '\0';
        return static_cast<unsigned>(n + 1);
    }
    if (v->type == clx::ValueType::Double) {
        int n = clx::clx_format_double(buff, LUA_N2SBUFFSZ, v->val.payload.f64);
        if (n < 0)
            return 0;
        if (n >= LUA_N2SBUFFSZ)
            n = LUA_N2SBUFFSZ - 1;
        buff[n] = '\0';
        return static_cast<unsigned>(n + 1);
    }
    return 0;
}

size_t lua_stringtonumber(lua_State *L, const char *s) {
    if (!s)
        return 0;
    clx::LValue v;
    if (!luaapi_str2num(s, v))
        return 0;
    luaapi_push(L, v);
    return std::strlen(s) + 1;
}

lua_Alloc lua_getallocf(lua_State *L, void **ud) {
    (void)L;
    if (ud)
        *ud = nullptr;
    return luaL_alloc;
}

void lua_setallocf(lua_State *L, lua_Alloc f, void *ud) {
    (void)L;
    (void)f;
    (void)ud;
}

void lua_toclose(lua_State *L, int idx) {
    (void)L;
    (void)idx;
}

void lua_closeslot(lua_State *L, int idx) {
    (void)L;
    (void)idx;
}

//=========================== debug interface ===========================

int lua_getstack(lua_State *L, int level, lua_Debug *ar) {
    if (level < 0 || !ar)
        return 0;
    clx::LThread *t = luaapi_self_thread(L);
    if (level == 0 && t && (t->status == clx::THREAD_RUNNING || t->status == clx::THREAD_NORMAL)) {
        std::memset(ar, 0, sizeof(lua_Debug));
        return 1;
    }
    return 0;
}

int lua_getinfo(lua_State *L, const char *what, lua_Debug *ar) {
    (void)L;
    (void)what;
    (void)ar;
    return 0;
}

const char *lua_getlocal(lua_State *L, const lua_Debug *ar, int n) {
    (void)L;
    (void)ar;
    (void)n;
    return nullptr;
}

const char *lua_setlocal(lua_State *L, const lua_Debug *ar, int n) {
    (void)L;
    (void)ar;
    (void)n;
    return nullptr;
}

const char *lua_getupvalue(lua_State *L, int funcindex, int n) {
    clx::LValue *v = luaapi_locate(L, funcindex);
    if (!v || v->type != clx::ValueType::Function)
        return nullptr;
    clx::LUpValue *cell = luaapi_upvalue_cell(static_cast<clx::LCFunction *>(v->as_pointer()), n);
    if (!cell || !*cell)
        return nullptr;
    luaapi_push(L, **cell);
    return "";
}

const char *lua_setupvalue(lua_State *L, int funcindex, int n) {
    clx::LState *S = L->L;
    ApiFrame &f = luaapi_frame(L);
    clx::LValue *v = luaapi_locate(L, funcindex);
    if (!v || v->type != clx::ValueType::Function || f.stack.empty())
        return nullptr;
    clx::LUpValue *cell = luaapi_upvalue_cell(static_cast<clx::LCFunction *>(v->as_pointer()), n);
    if (!cell || !*cell)
        return nullptr;
    clx::LValue val = f.stack.back();
    clx::upval_store(*cell, val);
    luaapi_truncate(L, f.stack.size() - 1);
    return "";
}

void *lua_upvalueid(lua_State *L, int fidx, int n) {
    clx::LValue *v = luaapi_locate(L, fidx);
    if (!v || v->type != clx::ValueType::Function)
        return nullptr;
    clx::LUpValue *cell = luaapi_upvalue_cell(static_cast<clx::LCFunction *>(v->as_pointer()), n);
    return cell ? static_cast<void *>(cell->get()) : nullptr;
}

void lua_upvaluejoin(lua_State *L, int fidx1, int n1, int fidx2, int n2) {
    clx::LValue *a = luaapi_locate(L, fidx1);
    clx::LValue *b = luaapi_locate(L, fidx2);
    if (!a || !b || a->type != clx::ValueType::Function || b->type != clx::ValueType::Function)
        return;
    clx::LCFunction *fn1 = static_cast<clx::LCFunction *>(a->as_pointer());
    clx::LCFunction *fn2 = static_cast<clx::LCFunction *>(b->as_pointer());
    clx::LUpValue *cell1 = luaapi_upvalue_cell(fn1, n1);
    clx::LUpValue *cell2 = luaapi_upvalue_cell(fn2, n2);
    if (!cell1 || !cell2)
        return;
    CFuncAdapter *ad1 = fn1->func.target<CFuncAdapter>();
    if (ad1 && ad1->ups && static_cast<size_t>(n1) <= ad1->ups->size())
        (*ad1->ups)[static_cast<size_t>(n1) - 1] = *cell2;
    *cell1 = *cell2;
}

void lua_sethook(lua_State *L, lua_Hook func, int mask, int count) {
    (void)L;
    (void)func;
    (void)mask;
    (void)count;
}

lua_Hook lua_gethook(lua_State *L) {
    (void)L;
    return nullptr;
}

int lua_gethookmask(lua_State *L) {
    (void)L;
    return 0;
}

int lua_gethookcount(lua_State *L) {
    (void)L;
    return 0;
}

//=========================== auxiliary library ===========================

void luaL_checkversion_(lua_State *L, lua_Number ver, size_t sz) {
    lua_Number v = lua_version(L);
    if (sz != LUAL_NUMSIZES)
        luaL_error(L, "core and library have incompatible numeric types");
    else if (v != ver)
        luaL_error(L, "version mismatch: app. needs %f, Lua core provides %f", ver, v);
}

int luaL_getmetafield(lua_State *L, int obj, const char *event) {
    if (!lua_getmetatable(L, obj))
        return LUA_TNIL;
    int tt;
    lua_pushstring(L, event);
    tt = lua_rawget(L, -2);
    if (tt == LUA_TNIL)
        lua_pop(L, 2);
    else
        lua_remove(L, -2);
    return tt;
}

int luaL_callmeta(lua_State *L, int obj, const char *event) {
    obj = lua_absindex(L, obj);
    if (luaL_getmetafield(L, obj, event) == LUA_TNIL)
        return 0;
    lua_pushvalue(L, obj);
    lua_call(L, 1, 1);
    return 1;
}

const char *luaL_tolstring(lua_State *L, int idx, size_t *len) {
    idx = lua_absindex(L, idx);
    if (luaL_callmeta(L, idx, "__tostring")) {
        if (!lua_isstring(L, -1))
            luaL_error(L, "'__tostring' must return a string");
    } else {
        switch (lua_type(L, idx)) {
        case LUA_TNUMBER: {
            char buff[LUA_N2SBUFFSZ];
            lua_numbertocstring(L, idx, buff);
            lua_pushstring(L, buff);
            break;
        }
        case LUA_TSTRING:
            lua_pushvalue(L, idx);
            break;
        case LUA_TBOOLEAN:
            lua_pushstring(L, lua_toboolean(L, idx) ? "true" : "false");
            break;
        case LUA_TNIL:
            lua_pushliteral(L, "nil");
            break;
        default: {
            int tt = luaL_getmetafield(L, idx, "__name");
            const char *kind = (tt == LUA_TSTRING) ? lua_tostring(L, -1) : luaL_typename(L, idx);
            lua_pushfstring(L, "%s: %p", kind, lua_topointer(L, idx));
            if (tt != LUA_TNIL)
                lua_remove(L, -2);
            break;
        }
        }
    }
    return lua_tolstring(L, -1, len);
}

int luaL_argerror(lua_State *L, int arg, const char *extramsg) {
    return luaL_error(L, "bad argument #%d (%s)", arg, extramsg);
}

int luaL_typeerror(lua_State *L, int arg, const char *tname) {
    const char *typearg;
    if (luaL_getmetafield(L, arg, "__name") == LUA_TSTRING)
        typearg = lua_tostring(L, -1);
    else if (lua_type(L, arg) == LUA_TLIGHTUSERDATA)
        typearg = "light userdata";
    else
        typearg = luaL_typename(L, arg);
    const char *msg = lua_pushfstring(L, "%s expected, got %s", tname, typearg);
    return luaL_argerror(L, arg, msg);
}

const char *luaL_checklstring(lua_State *L, int arg, size_t *len) {
    const char *s = lua_tolstring(L, arg, len);
    if (!s)
        luaL_typeerror(L, arg, "string");
    return s;
}

const char *luaL_optlstring(lua_State *L, int arg, const char *def, size_t *len) {
    if (lua_isnoneornil(L, arg)) {
        if (len)
            *len = def ? std::strlen(def) : 0;
        return def;
    }
    return luaL_checklstring(L, arg, len);
}

lua_Number luaL_checknumber(lua_State *L, int arg) {
    int isnum;
    lua_Number d = lua_tonumberx(L, arg, &isnum);
    if (!isnum)
        luaL_typeerror(L, arg, "number");
    return d;
}

lua_Number luaL_optnumber(lua_State *L, int arg, lua_Number def) {
    return luaL_opt(L, luaL_checknumber, arg, def);
}

lua_Integer luaL_checkinteger(lua_State *L, int arg) {
    int isnum;
    lua_Integer d = lua_tointegerx(L, arg, &isnum);
    if (!isnum) {
        if (lua_isnumber(L, arg))
            luaL_argerror(L, arg, "number has no integer representation");
        luaL_typeerror(L, arg, "number");
    }
    return d;
}

lua_Integer luaL_optinteger(lua_State *L, int arg, lua_Integer def) {
    return luaL_opt(L, luaL_checkinteger, arg, def);
}

void luaL_checkstack(lua_State *L, int space, const char *msg) {
    if (!lua_checkstack(L, space)) {
        if (msg)
            luaL_error(L, "stack overflow (%s)", msg);
        luaL_error(L, "stack overflow");
    }
}

void luaL_checktype(lua_State *L, int arg, int t) {
    if (lua_type(L, arg) != t)
        luaL_typeerror(L, arg, lua_typename(L, t));
}

void luaL_checkany(lua_State *L, int arg) {
    if (lua_type(L, arg) == LUA_TNONE)
        luaL_argerror(L, arg, "value expected");
}

int luaL_newmetatable(lua_State *L, const char *tname) {
    if (luaL_getmetatable(L, tname) != LUA_TNIL)
        return 0;
    lua_pop(L, 1);
    lua_createtable(L, 0, 2);
    lua_pushstring(L, tname);
    lua_setfield(L, -2, "__name");
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, tname);
    return 1;
}

void luaL_setmetatable(lua_State *L, const char *tname) {
    luaL_getmetatable(L, tname);
    lua_setmetatable(L, -2);
}

void *luaL_testudata(lua_State *L, int ud, const char *tname) {
    void *p = lua_touserdata(L, ud);
    if (p == nullptr)
        return nullptr;
    if (!lua_getmetatable(L, ud))
        return nullptr;
    if (tname)
        luaL_getmetatable(L, tname);
    else
        lua_pushnil(L);
    if (lua_rawequal(L, -1, -2)) {
        lua_pop(L, 2);
        return p;
    }
    if (tname && std::strcmp(tname, LUA_FILEHANDLE) == 0) {
        lua_pop(L, 2);
        if (lua_type(L, ud) == LUA_TUSERDATA && lua_getfield(L, ud, "seek") == LUA_TFUNCTION) {
            lua_pop(L, 1);
            lua_State *ls = luaapi_fetch(L->L);
            ls->file_scratch.f = *static_cast<FILE **>(p);
            ls->file_scratch.closef = luaapi_close_stub;
            return &ls->file_scratch;
        }
        lua_pop(L, 1);
        return nullptr;
    }
    lua_pop(L, 2);
    return nullptr;
}

void *luaL_checkudata(lua_State *L, int ud, const char *tname) {
    void *p = luaL_testudata(L, ud, tname);
    luaL_argexpected(L, p != nullptr, ud, tname);
    return p;
}

void luaL_where(lua_State *L, int level) {
    clx::LState *S = L->L;
    if (level > 0 && S->current_file && S->current_file[0] != '\0' && S->current_line > 0) {
        char buf[LUA_IDSIZE + 32];
        std::snprintf(buf, sizeof(buf), "%s:%d: ", S->current_file, S->current_line);
        lua_pushstring(L, buf);
        return;
    }
    lua_pushliteral(L, "");
}

int luaL_error(lua_State *L, const char *fmt, ...) {
    va_list argp;
    va_start(argp, fmt);
    luaL_where(L, 1);
    lua_pushvfstring(L, fmt, argp);
    va_end(argp);
    lua_concat(L, 2);
    return lua_error(L);
}

int luaL_checkoption(lua_State *L, int arg, const char *def, const char *const lst[]) {
    const char *name = def ? luaL_optstring(L, arg, def) : luaL_checkstring(L, arg);
    for (int i = 0; lst[i]; i++) {
        if (std::strcmp(lst[i], name) == 0)
            return i;
    }
    return luaL_argerror(L, arg, lua_pushfstring(L, "invalid option '%s'", name));
}

int luaL_fileresult(lua_State *L, int stat, const char *fname) {
    int en = errno;
    if (stat) {
        lua_pushboolean(L, 1);
        return 1;
    }
    const char *msg = (en != 0) ? std::strerror(en) : "(no extra info)";
    luaL_pushfail(L);
    if (fname)
        lua_pushfstring(L, "%s: %s", fname, msg);
    else
        lua_pushstring(L, msg);
    lua_pushinteger(L, en);
    return 3;
}

int luaL_execresult(lua_State *L, int stat) {
    if (stat != 0 && errno != 0)
        return luaL_fileresult(L, 0, NULL);
    const char *what = "exit";
#if !defined(_WIN32)
    if (WIFEXITED(stat)) {
        stat = WEXITSTATUS(stat);
    } else if (WIFSIGNALED(stat)) {
        stat = WTERMSIG(stat);
        what = "signal";
    }
#endif
    if (what[0] == 'e' && stat == 0)
        lua_pushboolean(L, 1);
    else
        luaL_pushfail(L);
    lua_pushstring(L, what);
    lua_pushinteger(L, stat);
    return 3;
}

void *luaL_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        std::free(ptr);
        return nullptr;
    }
    return std::realloc(ptr, nsize);
}

int luaL_ref(lua_State *L, int t) {
    int ref;
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return LUA_REFNIL;
    }
    t = lua_absindex(L, t);
    if (lua_rawgeti(L, t, 1) == LUA_TNUMBER)
        ref = static_cast<int>(lua_tointeger(L, -1));
    else {
        ref = 0;
        lua_pushinteger(L, 0);
        lua_rawseti(L, t, 1);
    }
    lua_pop(L, 1);
    if (ref != 0) {
        lua_rawgeti(L, t, ref);
        lua_rawseti(L, t, 1);
    } else {
        ref = static_cast<int>(lua_rawlen(L, t)) + 1;
    }
    lua_rawseti(L, t, ref);
    return ref;
}

void luaL_unref(lua_State *L, int t, int ref) {
    if (ref >= 0) {
        t = lua_absindex(L, t);
        lua_rawgeti(L, t, 1);
        lua_rawseti(L, t, ref);
        lua_pushinteger(L, ref);
        lua_rawseti(L, t, 1);
    }
}

int luaL_loadfilex(lua_State *L, const char *filename, const char *mode) {
    (void)mode;
    std::string msg;
    FILE *f = filename ? std::fopen(filename, "rb") : nullptr;
    if (!f) {
        msg = std::string("cannot open ") + (filename ? filename : "stdin");
        luaapi_push(L, clx::LValue(L->L->intern_string(msg)));
        return LUA_ERRFILE;
    }
    std::fclose(f);
    msg = std::string("clx: cannot load '") + (filename ? filename : "stdin") + "' at runtime";
    luaapi_push(L, clx::LValue(L->L->intern_string(msg)));
    return LUA_ERRSYNTAX;
}

int luaL_loadbufferx(lua_State *L, const char *buff, size_t size, const char *name, const char *mode) {
    (void)buff;
    (void)size;
    (void)mode;
    std::string msg = std::string("clx: cannot load '") + (name ? name : "?") + "' at runtime";
    luaapi_push(L, clx::LValue(L->L->intern_string(msg)));
    return LUA_ERRSYNTAX;
}

int luaL_loadstring(lua_State *L, const char *s) {
    return luaL_loadbufferx(L, s, s ? std::strlen(s) : 0, s, nullptr);
}

lua_State *luaL_newstate(void) {
    return nullptr;
}

unsigned luaL_makeseed(lua_State *L) {
    std::time_t t = std::time(nullptr);
    uintptr_t p = reinterpret_cast<uintptr_t>(static_cast<void *>(L));
    return static_cast<unsigned>((p >> 4) ^ static_cast<uintptr_t>(t) ^ 0x9e3779b9u);
}

lua_Integer luaL_len(lua_State *L, int idx) {
    lua_Integer l;
    int isnum;
    lua_len(L, idx);
    l = lua_tointegerx(L, -1, &isnum);
    if (!isnum)
        luaL_error(L, "object length is not an integer");
    lua_pop(L, 1);
    return l;
}

void luaL_addgsub(luaL_Buffer *b, const char *s, const char *p, const char *r) {
    const char *wild;
    size_t l = std::strlen(p);
    while ((wild = std::strstr(s, p)) != nullptr) {
        luaL_addlstring(b, s, static_cast<size_t>(wild - s));
        luaL_addstring(b, r);
        s = wild + l;
    }
    luaL_addstring(b, s);
}

const char *luaL_gsub(lua_State *L, const char *s, const char *p, const char *r) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addgsub(&b, s, p, r);
    luaL_pushresult(&b);
    return lua_tostring(L, -1);
}

void luaL_setfuncs(lua_State *L, const luaL_Reg *l, int nup) {
    luaL_checkstack(L, nup, "too many upvalues");
    for (; l->name != NULL; l++) {
        if (l->func == NULL)
            lua_pushboolean(L, 0);
        else {
            int i;
            for (i = 0; i < nup; i++)
                lua_pushvalue(L, -nup);
            lua_pushcclosure(L, l->func, nup);
        }
        lua_setfield(L, -(nup + 2), l->name);
    }
    lua_pop(L, nup);
}

int luaL_getsubtable(lua_State *L, int idx, const char *fname) {
    if (lua_getfield(L, idx, fname) == LUA_TTABLE)
        return 1;
    lua_pop(L, 1);
    idx = lua_absindex(L, idx);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, idx, fname);
    return 0;
}

void luaL_traceback(lua_State *L, lua_State *L1, const char *msg, int level) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    (void)L1;
    (void)level;
    if (msg) {
        luaL_addstring(&b, msg);
        luaL_addchar(&b, '\n');
    }
    luaL_addstring(&b, "stack traceback:");
    luaL_pushresult(&b);
}

void luaL_requiref(lua_State *L, const char *modname, lua_CFunction openf, int glb) {
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, modname);
    if (!lua_toboolean(L, -1)) {
        lua_pop(L, 1);
        lua_pushcfunction(L, openf);
        lua_pushstring(L, modname);
        lua_call(L, 1, 1);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, modname);
    }
    lua_remove(L, -2);
    if (glb) {
        lua_pushvalue(L, -1);
        lua_setglobal(L, modname);
    }
}

void luaL_openselectedlibs(lua_State *L, int load, int preload) {
    (void)L;
    (void)load;
    (void)preload;
}

//=========================== generic buffers ===========================

//---------- luaapi_prepbuff: grow the buffer heap block when the static area is full
static char *luaapi_prepbuff(luaL_Buffer *B, size_t sz) {
    if (B->size - B->n >= sz)
        return B->b + B->n;
    if (sz >= SIZE_MAX - B->n) {
        luaL_error(B->L, "resulting string too large");
        return nullptr;
    }
    size_t newsize = B->size;
    if (newsize <= SIZE_MAX / 3 * 2)
        newsize += (newsize >> 1);
    if (newsize < B->n + sz + 1)
        newsize = B->n + sz + 1;
    char *nb = new char[newsize];
    if (B->n)
        std::memcpy(nb, B->b, B->n);
    delete[] static_cast<char *>(B->ext);
    B->ext = nb;
    B->b = nb;
    B->size = newsize;
    return nb + B->n;
}

void luaL_buffinit(lua_State *L, luaL_Buffer *B) {
    B->L = L;
    B->b = B->init.b;
    B->n = 0;
    B->size = LUAL_BUFFERSIZE;
    B->ext = nullptr;
    lua_pushlightuserdata(L, static_cast<void *>(B));
}

char *luaL_prepbuffsize(luaL_Buffer *B, size_t sz) {
    return luaapi_prepbuff(B, sz);
}

char *luaL_buffinitsize(lua_State *L, luaL_Buffer *B, size_t sz) {
    luaL_buffinit(L, B);
    return luaapi_prepbuff(B, sz);
}

void luaL_addlstring(luaL_Buffer *B, const char *s, size_t l) {
    if (l > 0) {
        char *b = luaapi_prepbuff(B, l);
        std::memcpy(b, s, l);
        B->n += l;
    }
}

void luaL_addstring(luaL_Buffer *B, const char *s) {
    luaL_addlstring(B, s, std::strlen(s));
}

void luaL_addvalue(luaL_Buffer *B) {
    size_t len;
    const char *s = lua_tolstring(B->L, -1, &len);
    if (!s)
        return;
    char *b = luaapi_prepbuff(B, len);
    std::memcpy(b, s, len);
    B->n += len;
    lua_pop(B->L, 1);
}

void luaL_pushresult(luaL_Buffer *B) {
    lua_State *L = B->L;
    clx::LValue v(L->L->intern_string(B->b, B->n));
    delete[] static_cast<char *>(B->ext);
    B->ext = nullptr;
    B->b = B->init.b;
    B->size = LUAL_BUFFERSIZE;
    B->n = 0;
    luaapi_push(L, v);
    lua_remove(L, -2);
}

void luaL_pushresultsize(luaL_Buffer *B, size_t sz) {
    B->n += sz;
    luaL_pushresult(B);
}

//=========================== standard library openers ===========================

int luaopen_base(lua_State *L) {
    luaapi_push(L, clx::LValue(clx::ValueType::Table, L->L->_G));
    return 1;
}

int luaopen_package(lua_State *L) {
    return luaapi_push_lib(L, LUA_LOADLIBNAME);
}

int luaopen_coroutine(lua_State *L) {
    return luaapi_push_lib(L, LUA_COLIBNAME);
}

int luaopen_debug(lua_State *L) {
    return luaapi_push_lib(L, LUA_DBLIBNAME);
}

int luaopen_io(lua_State *L) {
    return luaapi_push_lib(L, LUA_IOLIBNAME);
}

int luaopen_math(lua_State *L) {
    return luaapi_push_lib(L, LUA_MATHLIBNAME);
}

int luaopen_os(lua_State *L) {
    return luaapi_push_lib(L, LUA_OSLIBNAME);
}

int luaopen_string(lua_State *L) {
    return luaapi_push_lib(L, LUA_STRLIBNAME);
}

int luaopen_table(lua_State *L) {
    return luaapi_push_lib(L, LUA_TABLIBNAME);
}

int luaopen_utf8(lua_State *L) {
    return luaapi_push_lib(L, LUA_UTF8LIBNAME);
}
