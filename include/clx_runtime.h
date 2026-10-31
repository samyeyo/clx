// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  clx_runtime.h · Internal Runtime Header    │
// └─────────────────────────────────────────────┘

#ifndef CLX_RUNTIME_H
#define CLX_RUNTIME_H

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__) && defined(__aarch64__) || defined(__linux__) && defined(__aarch64__)
struct CoroutineContext {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp;
    uint64_t lr;
    uint64_t sp;
    uint64_t d8, d9, d10, d11, d12, d13, d14, d15;
};

extern "C" {
void clx_coro_save(CoroutineContext *ctx);
void clx_coro_switch(CoroutineContext *from, CoroutineContext *to);
void clx_coro_init(CoroutineContext *ctx, void *stack_top, void *entry);
}
#elif defined(__linux__) && defined(__x86_64__)
struct CoroutineContext {
    uint64_t rbx, rbp, r12, r13, r14, r15, rsp;
};

extern "C" {
void clx_coro_save(CoroutineContext *ctx);
void clx_coro_switch(CoroutineContext *from, CoroutineContext *to);
void clx_coro_init(CoroutineContext *ctx, void *stack_top, void *entry);
}
#else
#if defined(__APPLE__)
#define _XOPEN_SOURCE
#endif
#include <ucontext.h>
#endif

//---------- CLX: CLX_MUSTTAIL - GCC only; clang bans closure->free musttail, MSVC has none.
//          -O0 (-Og) does not perform tail calls, so musttail is only enabled when optimization is on.
//          GNU --size uses -O1 (not -Os, Windows and Linux) because GCC 15 rejects [[gnu::musttail]] at -Os.
//          At -O1 the heuristic tail-call pass is off by default, so size mode also passes
//          -foptimize-sibling-calls (covers GCC builds without the attribute, e.g. GCC 13).
#if defined(__clang__)
#define CLX_MUSTTAIL
#elif defined(__GNUC__) && defined(__has_cpp_attribute) && __has_cpp_attribute(gnu::musttail) && !defined(__aarch64__) \
    && defined(__OPTIMIZE__)
#define CLX_MUSTTAIL [[gnu::musttail]]
#else
#define CLX_MUSTTAIL
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define clx_strlen strlen
#define clx_memcpy memcpy
#define clx_memcmp memcmp
#else
#define clx_strlen __builtin_strlen
#define clx_memcpy __builtin_memcpy
#define clx_memcmp __builtin_memcmp
#endif

#if defined(_WIN32) && defined(CLX_BUILD_AS_DLL)
#define CLX_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define CLX_API __attribute__((visibility("default")))
#else
#define CLX_API
#endif

#if defined(_MSC_VER)
#ifndef CLX_INLINE_HOT
#define CLX_INLINE_HOT __forceinline
#endif
#ifndef CLX_INLINE_COLD
#define CLX_INLINE_COLD inline __declspec(noinline)
#endif
#ifndef CLX_INLINE
#define CLX_INLINE inline
#endif
#elif defined(__GNUC__) || defined(__clang__)
#ifndef CLX_INLINE_HOT
#define CLX_INLINE_HOT inline __attribute__((always_inline))
#endif
#ifndef CLX_INLINE_COLD
#define CLX_INLINE_COLD inline __attribute__((noinline))
#endif
#ifndef CLX_INLINE
#define CLX_INLINE inline
#endif
#else
#ifndef CLX_INLINE_HOT
#define CLX_INLINE_HOT inline
#endif
#ifndef CLX_INLINE_COLD
#define CLX_INLINE_COLD inline
#endif
#ifndef CLX_INLINE
#define CLX_INLINE inline
#endif
#endif

#include "clx_simd.h"

namespace clx {

#if defined(_MSC_VER)

//------------------ 128-bit multiply helper
static CLX_INLINE_HOT uint64_t clx_umul128(uint64_t a, uint64_t b, uint64_t *hi) {
    return _umul128(a, b, hi);
}
#else

//------------------ 128-bit multiply helper
static CLX_INLINE_HOT uint64_t clx_umul128(uint64_t a, uint64_t b, uint64_t *hi) {
    __uint128_t r = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
    *hi = static_cast<uint64_t>(r >> 64);
    return static_cast<uint64_t>(r);
}
#endif

//------------------ Value type enum (shadow type discriminator)
enum class ValueType : uint8_t {
    Nil = 0,
    Boolean = 1,
    Int64 = 2,
    Double = 3,
    String = 4,
    Table = 5,
    Function = 6,
    UserData = 7,
    Thread = 8
};

//------------------ ValueType short aliases (clx::Nil, clx::String, etc.)
constexpr auto Nil = ValueType::Nil;
constexpr auto Boolean = ValueType::Boolean;
constexpr auto Int64 = ValueType::Int64;
constexpr auto Double = ValueType::Double;
constexpr auto String = ValueType::String;
constexpr auto Table = ValueType::Table;
constexpr auto Function = ValueType::Function;
constexpr auto UserData = ValueType::UserData;
constexpr auto Thread = ValueType::Thread;

//------------------ Type name strings by ValueType index
static constexpr const char *VALUE_TYPE_NAMES[]
    = { "nil", "boolean", "integer", "number", "string", "table", "function", "userdata", "thread" };

//------------------ clx_format_double: Lua-compatible number-to-string
CLX_INLINE_HOT int clx_format_double(char *buf, size_t cap, double v) {
    int n = std::snprintf(buf, cap, "%.15g", v);
    if (std::strtod(buf, nullptr) != v)
        n = std::snprintf(buf, cap, "%.17g", v);
    if (buf[strspn(buf, "-0123456789")] == '\0' && n + 2 < static_cast<int>(cap)) {
        buf[n++] = '.';
        buf[n++] = '0';
    }
    return n;
}

//------------------ GC object header
struct LHeader {
    uint8_t type;
    uint8_t marked;
    uint8_t flags = 0;
    uint8_t age = 0;
    LHeader *next;
};

static constexpr uint8_t AGE_YOUNG = 0;
static constexpr uint8_t AGE_SURVIVOR = 1;
static constexpr uint8_t AGE_OLD = 2;

static constexpr uint8_t LFLAG_VM_PROXY = 0x01;
static constexpr uint8_t LFLAG_ARENA = 0x02;
static constexpr uint8_t LFLAG_META_LIST = 0x04;
static constexpr uint8_t LFLAG_SLAB = 0x08;
static constexpr uint8_t LFLAG_REMEMBERED = 0x10;
static constexpr uint8_t LFLAG_GC_PIN = 0x20;
static constexpr uint8_t LFLAG_FINALIZABLE = 0x40;
static constexpr uint8_t LFLAG_META_UD = 0x80;

struct LState;
struct FuncArena;
struct TValue;
struct LValue;

//------------------ Binary metamethod dispatcher
LValue call_bin_metamethod(LState *L, const LValue &a, const LValue &b, const char *event);
LValue call_bin_metamethod(LState *L, const LValue &a, const LValue &b, const LValue &key, const char *event);

//------------------ find_bin_metamethod: returns the binary-op metamethod from either operand's metatable, or Nil
LValue find_bin_metamethod(LState *L, const LValue &a, const LValue &b, const char *event);
LValue find_bin_metamethod(LState *L, const LValue &a, const LValue &b, const LValue &key);

//------------------ Float/int string coercions (defined later in this header)
bool flt_to_integer(double d, int64_t &out);
bool parse_num_string(const char *s, double &out);

//------------------ 8-byte value payload --- pure data, no type bits
union alignas(8) TValuePayload {
    uint64_t u64;
    int64_t i64;
    double f64;
    void *ptr;
};

struct TValue {
    TValuePayload payload;

    CLX_INLINE_HOT TValue() { payload.u64 = 0; }

    CLX_INLINE_HOT TValue(int64_t i) { payload.i64 = i; }

    CLX_INLINE_HOT TValue(double d) { payload.f64 = d; }

    CLX_INLINE_HOT TValue(void *p) { payload.ptr = p; }

    CLX_INLINE_HOT TValue(uint64_t u) { payload.u64 = u; }
};

//------------------ is_gc_obj_tag: tag-only GC-object test so hot paths (e.g. write barriers fed by raw
//------------------ TValue/ValueType pairs) need not box a throwaway LValue just to ask the question.
CLX_INLINE_HOT bool is_gc_obj_tag(ValueType t) {
    uint8_t tt = static_cast<uint8_t>(t);
    return tt >= static_cast<uint8_t>(ValueType::Table) && tt <= static_cast<uint8_t>(ValueType::Thread);
}

//------------------ 16-byte convenience wrapper (runtime C++ API)
struct LValue {
    TValue val;
    ValueType type;

    CLX_INLINE_HOT LValue()
        : val()
        , type(ValueType::Nil) { }

    CLX_INLINE_HOT explicit LValue(bool b)
        : val(static_cast<uint64_t>(b ? 1ULL : 0ULL))
        , type(ValueType::Boolean) { }

    CLX_INLINE_HOT explicit LValue(double n)
        : val(n)
        , type(ValueType::Double) { }

    CLX_INLINE_HOT explicit LValue(int64_t i)
        : val(i)
        , type(ValueType::Int64) { }

    CLX_INLINE_HOT explicit LValue(const char *s)
        : type(ValueType::String) {
        val.payload.ptr = const_cast<char *>(s);
    }

    CLX_INLINE_HOT LValue(const TValue &v, ValueType t)
        : val(v)
        , type(t) { }

    CLX_INLINE_HOT explicit LValue(ValueType t, LHeader *p)
        : val(static_cast<void *>(p))
        , type(t) { }

    CLX_INLINE_HOT explicit LValue(LHeader *p)
        : val(static_cast<void *>(p))
        , type(static_cast<ValueType>(p->type)) { }

    CLX_INLINE_HOT explicit LValue(std::nullptr_t)
        : val()
        , type(ValueType::Nil) { }

    CLX_INLINE_HOT bool is_gc_obj() const { return is_gc_obj_tag(type); }

    CLX_INLINE_HOT double as_number() const {
        if (type == ValueType::Int64)
            return static_cast<double>(val.payload.i64);
        if (type == ValueType::Double)
            return val.payload.f64;
        return 0.0;
    }

    CLX_INLINE_HOT int64_t as_integer() const {
        if (type == ValueType::Int64)
            return val.payload.i64;
        if (type == ValueType::Double)
            return static_cast<int64_t>(val.payload.f64);
        return 0;
    }

    CLX_INLINE_HOT bool as_bool() const {
        if (type == ValueType::Nil)
            return false;
        if (type == ValueType::Boolean)
            return val.payload.u64 != 0;
        return true;
    }

    CLX_INLINE_HOT const char *as_string() const {
        if (type == ValueType::String && (val.payload.u64 >> 56))
            return reinterpret_cast<const char *>(&val.payload.u64);
        if (type == ValueType::String && val.payload.u64 == 0)
            return "";
        return static_cast<const char *>(val.payload.ptr);
    }

    CLX_INLINE_HOT LHeader *as_pointer() const { return static_cast<LHeader *>(val.payload.ptr); }

    CLX_INLINE_HOT uint32_t string_len() const {
        if (type == ValueType::String) {
            uint64_t top = val.payload.u64 >> 56;
            if (top)
                return static_cast<uint32_t>(top);
            if (val.payload.u64 == 0)
                return 0;
            uint32_t len;
            clx_memcpy(&len, static_cast<const char *>(val.payload.ptr) - 8, 4);
            return len;
        }
        return 0;
    }

    static CLX_INLINE_HOT LValue istr(const char *s, size_t len) {
        LValue v;
        v.type = ValueType::String;
        v.val.payload.u64 = 0;
        for (size_t i = 0; i < len && i < 6; i++)
            v.val.payload.u64 |= static_cast<uint64_t>(static_cast<uint8_t>(s[i])) << (i * 8);
        v.val.payload.u64 |= static_cast<uint64_t>(len) << 56;
        return v;
    }

    CLX_INLINE_HOT bool to_number(double &out) const {
        if (type == ValueType::Double) {
            out = val.payload.f64;
            return true;
        }
        if (type == ValueType::Int64) {
            out = static_cast<double>(val.payload.i64);
            return true;
        }
        if (type == ValueType::String)
            return parse_num_string(as_string(), out);
        return false;
    }

    CLX_INLINE_HOT LValue operator==(const LValue &other) const {
        if (val.payload.u64 == other.val.payload.u64 && type == other.type)
            return LValue(true);
        if (type == ValueType::Double && other.type == ValueType::Double)
            return LValue(val.payload.f64 == other.val.payload.f64);
        if (type == ValueType::Int64 && other.type == ValueType::Int64)
            return LValue(val.payload.i64 == other.val.payload.i64);
        if (type == ValueType::Int64 && other.type == ValueType::Double)
            return LValue(static_cast<double>(val.payload.i64) == other.val.payload.f64);
        if (type == ValueType::Double && other.type == ValueType::Int64)
            return LValue(val.payload.f64 == static_cast<double>(other.val.payload.i64));
        return slow_eq(other);
    }

    CLX_INLINE_HOT LValue operator!=(const LValue &other) const { return LValue(!(operator==(other)).as_bool()); }

    CLX_INLINE_HOT LValue operator<(const LValue &other) const {
        if (type == ValueType::Int64 && other.type == ValueType::Int64)
            return LValue(val.payload.i64 < other.val.payload.i64);
        if (type == ValueType::Double && other.type == ValueType::Double)
            return LValue(val.payload.f64 < other.val.payload.f64);
        if ((type == ValueType::Int64 || type == ValueType::Double)
            && (other.type == ValueType::Int64 || other.type == ValueType::Double)) {
            double l = (type == ValueType::Int64) ? static_cast<double>(val.payload.i64) : val.payload.f64;
            double r
                = (other.type == ValueType::Int64) ? static_cast<double>(other.val.payload.i64) : other.val.payload.f64;
            return LValue(l < r);
        }
        return slow_lt(other);
    }

    CLX_INLINE_HOT LValue operator>(const LValue &other) const { return other.operator<(*this); }

    CLX_INLINE_HOT LValue operator<=(const LValue &other) const {
        if (type == ValueType::Int64 && other.type == ValueType::Int64)
            return LValue(val.payload.i64 <= other.val.payload.i64);
        if (type == ValueType::Double && other.type == ValueType::Double)
            return LValue(val.payload.f64 <= other.val.payload.f64);
        if ((type == ValueType::Int64 || type == ValueType::Double)
            && (other.type == ValueType::Int64 || other.type == ValueType::Double)) {
            double l = (type == ValueType::Int64) ? static_cast<double>(val.payload.i64) : val.payload.f64;
            double r
                = (other.type == ValueType::Int64) ? static_cast<double>(other.val.payload.i64) : other.val.payload.f64;
            return LValue(l <= r);
        }
        return slow_le(other);
    }

    CLX_INLINE_HOT LValue operator>=(const LValue &other) const { return other.operator<=(*this); }

    std::string to_string(LState *L = nullptr) const;

private:
    LValue slow_eq(const LValue &other) const;
    LValue slow_lt(const LValue &other) const;
    LValue slow_le(const LValue &other) const;
};

static_assert(sizeof(LValue) == 16, "LValue must be 16 bytes");

//------------------ Integer: bare integer operand for hot comparison fast paths (no boxing)
struct Integer {
    int64_t v;

    CLX_INLINE_HOT Integer(int64_t value)
        : v(value) { }
};

//------------------ Multi-value return container
struct MultiValue {
    static constexpr size_t INLINE_CAP = 4;
    size_t count = 0;
    clx::LValue *overflow = nullptr;
    LState *alloc_L = nullptr;
    //------------------ Raw inline storage: only the first min(count, INLINE_CAP) slots are ever
    //------------------ constructed (placement-new). LValue is trivially destructible, so unused slots cost
    //------------------ nothing and no destruction is needed. This removes 3-4 dead LValue()/TValue()
    //------------------ constructions per MultiValue temporary (the default ctor used to build all four).
    alignas(clx::LValue) unsigned char inline_buf[INLINE_CAP * sizeof(clx::LValue)];

    CLX_INLINE_HOT clx::LValue *inline_ptr() { return reinterpret_cast<clx::LValue *>(inline_buf); }
    CLX_INLINE_HOT const clx::LValue *inline_ptr() const {
        return reinterpret_cast<const clx::LValue *>(inline_buf);
    }

    MultiValue() = default;

    MultiValue(const clx::LValue &single)
        : count(1) {
        new (inline_buf) clx::LValue(single);
    }

    MultiValue(const clx::LValue &a, const clx::LValue &b)
        : count(2) {
        new (inline_buf) clx::LValue(a);
        new (inline_buf + sizeof(clx::LValue)) clx::LValue(b);
    }

    MultiValue(const clx::LValue &a, const clx::LValue &b, const clx::LValue &c)
        : count(3) {
        new (inline_buf) clx::LValue(a);
        new (inline_buf + sizeof(clx::LValue)) clx::LValue(b);
        new (inline_buf + 2 * sizeof(clx::LValue)) clx::LValue(c);
    }

    //------------------ Primitive ctors: construct the LValue in place, skipping the LValue temporary
    //------------------ + copy that `MultiValue(LValue(int64))` paid at every single-value return.
    MultiValue(int64_t i)
        : count(1) {
        new (inline_buf) clx::LValue(i);
    }

    MultiValue(bool b)
        : count(1) {
        new (inline_buf) clx::LValue(b);
    }

    MultiValue(bool a, const clx::LValue &b)
        : count(2) {
        new (inline_buf) clx::LValue(a);
        new (inline_buf + sizeof(clx::LValue)) clx::LValue(b);
    }

    MultiValue(int64_t a, int64_t b, int64_t c)
        : count(3) {
        new (inline_buf) clx::LValue(a);
        new (inline_buf + sizeof(clx::LValue)) clx::LValue(b);
        new (inline_buf + 2 * sizeof(clx::LValue)) clx::LValue(c);
    }

    //------------------ push_back: appends one value, constructing it in place in the inline slots
    //------------------ (no LValue temporary + vector hop). Overflow beyond INLINE_CAP is bump-allocated;
    //------------------ the growth path is cold (only 4+ returns take it) and keeps alloc_L-backed data
    //------------------ safe across the realloc inside alloc_overflow via a snapshot copy.
    CLX_INLINE_HOT void push_back(const clx::LValue &v) { new (push_slot()) clx::LValue(v); }

    CLX_INLINE_HOT void push_back(bool b) { new (push_slot()) clx::LValue(b); }

    CLX_INLINE_HOT clx::LValue *push_slot() {
        if (count < INLINE_CAP)
            return inline_ptr() + count++;
        return push_grow();
    }

    //------------------ Defined out of line (below LState) like overflow_init: needs LState complete.
    clx::LValue *push_grow();

    MultiValue(const clx::LValue *arr, size_t c, LState *L = nullptr)
        : count(c)
        , alloc_L(L) {
        if (c <= INLINE_CAP) {
            for (size_t i = 0; i < c; ++i)
                new (inline_buf + i * sizeof(clx::LValue)) clx::LValue(arr[i]);
        } else {
            for (size_t i = 0; i < INLINE_CAP; ++i)
                new (inline_buf + i * sizeof(clx::LValue)) clx::LValue(arr[i]);
            overflow_init(arr, c);
        }
    }

    MultiValue(std::initializer_list<clx::LValue> init, LState *L = nullptr)
        : MultiValue(init.begin(), init.size(), L) { }

    MultiValue(const std::vector<clx::LValue> &vec, LState *L = nullptr)
        : MultiValue(vec.data(), vec.size(), L) { }

    MultiValue(const MultiValue &other)
        : count(other.count)
        , alloc_L(other.alloc_L) {
        size_t inline_c = (count < INLINE_CAP) ? count : INLINE_CAP;
        const clx::LValue *src = other.inline_ptr();
        for (size_t i = 0; i < inline_c; ++i)
            new (inline_buf + i * sizeof(clx::LValue)) clx::LValue(src[i]);
        overflow = other.overflow;
    }

    MultiValue(MultiValue &&other) noexcept
        : count(other.count)
        , overflow(other.overflow)
        , alloc_L(other.alloc_L) {
        size_t inline_c = (count < INLINE_CAP) ? count : INLINE_CAP;
        const clx::LValue *src = other.inline_ptr();
        for (size_t i = 0; i < inline_c; ++i)
            new (inline_buf + i * sizeof(clx::LValue)) clx::LValue(src[i]);
        other.overflow = nullptr;
        other.count = 0;
        other.alloc_L = nullptr;
    }

    MultiValue &operator=(MultiValue &&other) noexcept {
        if (this != &other) {
            //------------------ Slots below old_live are already constructed: assign. Slots at or above
            //------------------ it are raw: placement-new. (LValue is trivially destructible, so shrinking
            //------------------ needs no destruction; growing must not assign into raw storage.)
            size_t old_live = (count < INLINE_CAP) ? count : INLINE_CAP;
            count = other.count;
            overflow = other.overflow;
            alloc_L = other.alloc_L;
            size_t inline_c = (count < INLINE_CAP) ? count : INLINE_CAP;
            const clx::LValue *src = other.inline_ptr();
            clx::LValue *dst = inline_ptr();
            size_t i = 0;
            for (; i < inline_c && i < old_live; ++i)
                dst[i] = src[i];
            for (; i < inline_c; ++i)
                new (dst + i) clx::LValue(src[i]);
            other.overflow = nullptr;
            other.count = 0;
            other.alloc_L = nullptr;
        }
        return *this;
    }

    MultiValue &operator=(const MultiValue &other) {
        if (this != &other) {
            size_t old_live = (count < INLINE_CAP) ? count : INLINE_CAP;
            count = other.count;
            alloc_L = other.alloc_L;
            size_t inline_c = (count < INLINE_CAP) ? count : INLINE_CAP;
            const clx::LValue *src = other.inline_ptr();
            clx::LValue *dst = inline_ptr();
            size_t i = 0;
            for (; i < inline_c && i < old_live; ++i)
                dst[i] = src[i];
            for (; i < inline_c; ++i)
                new (dst + i) clx::LValue(src[i]);
            overflow = other.overflow;
        }
        return *this;
    }

    //------------------ In-place assign from an array: reuses already-constructed inline slots (assign)
    //------------------ and placement-news only the growth. Saves one full set of constructions versus
    //------------------ `m = MultiValue(arr, c, L)` (temp constructs c, then move-assigns c again). Overflow
    //------------------ beyond INLINE_CAP is freshly bump-allocated; stale bump pointers die at GC reset.
    CLX_INLINE_HOT void assign(const clx::LValue *arr, size_t c, LState *L = nullptr) {
        size_t old_live = (count < INLINE_CAP) ? count : INLINE_CAP;
        count = c;
        alloc_L = L;
        clx::LValue *dst = inline_ptr();
        size_t inline_c = (c < INLINE_CAP) ? c : INLINE_CAP;
        size_t i = 0;
        for (; i < inline_c && i < old_live; ++i)
            dst[i] = arr[i];
        for (; i < inline_c; ++i)
            new (dst + i) clx::LValue(arr[i]);
        if (c <= INLINE_CAP) {
            overflow = nullptr;
        } else {
            overflow_init(arr, c);
        }
    }

    ~MultiValue() = default;

    clx::LValue &operator[](size_t i) { return (i < INLINE_CAP) ? inline_ptr()[i] : overflow[i - INLINE_CAP]; }

    const clx::LValue &operator[](size_t i) const {
        return (i < INLINE_CAP) ? inline_ptr()[i] : overflow[i - INLINE_CAP];
    }

    void overflow_init(const clx::LValue *arr, size_t c);
};

struct LState;
struct LTable;
struct RootRange;
struct CallSite;

//------------------ default_call_site: no-file/no-line descriptor used when a call has no source context
const CallSite *default_call_site();

//------------------ Shared upvalue type
using LUpValue = std::shared_ptr<LValue>;

//------------------ Function call dispatcher (with error context)
MultiValue call_function(
    LState *L, const LValue &func, const LValue *args, size_t count, const CallSite *cs = default_call_site());
//------------------ Protected function call
MultiValue pcall_function(LState *L, const LValue &func, const LValue *args, size_t count);

//------------------ Creates a shared upvalue
CLX_INLINE_HOT LUpValue make_upvalue(const LValue &val) {
    return std::make_shared<LValue>(val);
}

//------------------ C function type: single-indirection type erasure over a
//------------------ shared box. Replaces std::function (which costs ~5 libstdc++
//------------------ erasure frames per call and per frame unwound per throw) with one
//------------------ static invoker frame. Copies share the box (a shared_ptr bump),
//------------------ so captures are never re-copied on closure creation. target<T>()
//------------------ keeps the two identity queries the runtime needs (CFuncAdapter
//------------------ for the C API, the raw signature for the direct fast path).
struct CFunctionType {
    using Sig = MultiValue (*)(LState *, const LValue *, size_t);
    using InvokeFn = MultiValue (*)(void *, LState *, const LValue *, size_t);
    std::shared_ptr<void> box;
    InvokeFn invoke = nullptr;
    const std::type_info *type = nullptr;

    CFunctionType() = default;
    CFunctionType(const CFunctionType &) = default;
    CFunctionType(CFunctionType &&) = default;
    CFunctionType &operator=(const CFunctionType &) = default;
    CFunctionType &operator=(CFunctionType &&) = default;
    CFunctionType &operator=(std::nullptr_t) {
        box.reset();
        invoke = nullptr;
        type = nullptr;
        return *this;
    }
    template<typename F,
        typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, CFunctionType>>>
    CFunctionType(F &&f) {
        assign(std::forward<F>(f));
    }
    template<typename F,
        typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, CFunctionType>>>
    CFunctionType &operator=(F &&f) {
        assign(std::forward<F>(f));
        return *this;
    }
    CLX_INLINE_HOT MultiValue operator()(LState *L, const LValue *args, size_t count) const {
        if (!invoke)
            throw std::bad_function_call();
        return invoke(box.get(), L, args, count);
    }
    explicit operator bool() const {
        return invoke != nullptr;
    }
    template<typename T> const T *target() const {
        if (type && *type == typeid(T))
            return static_cast<const T *>(box.get());
        return nullptr;
    }

private:
    template<typename F> void assign(F &&f) {
        using D = std::decay_t<F>;
        box = std::shared_ptr<void>(new D(std::forward<F>(f)));
        type = &typeid(D);
        invoke = [](void *p, LState *L, const LValue *a, size_t n) -> MultiValue {
            return (*static_cast<D *>(p))(L, a, n);
        };
    }
};

struct LCFunction : public LHeader {
    CFunctionType func;
    MultiValue (*direct)(LState *, const LValue *, size_t) = nullptr;
    LTable *env = nullptr;
    LValue self_ref;
    std::vector<LUpValue> gc_cells;
    LCFunction(CFunctionType f);
};

//------------------ Fast path for LCFunction direct calls
MultiValue call_direct(
    LState *L, const LValue &func, const LValue *args, size_t count, const CallSite *cs = default_call_site());

//------------------ Rooted call wrappers (defined after LState; alias func + args into a RootRange around a call)
MultiValue call_function_rooted(
    LState *L, const LValue &func, const LValue *args, size_t count, const CallSite *cs = default_call_site());
MultiValue call_direct_rooted(
    LState *L, const LValue &func, const LValue *args, size_t count, const CallSite *cs = default_call_site());
MultiValue call_cfunc_rooted(
    LState *L, CFunctionType f, const LValue *args, size_t count, const CallSite *cs = default_call_site());

//------------------ Userdata block
struct LUserdata : public LHeader {
    LTable *metatable;
    size_t size;
    LUserdata *meta_next = nullptr;

    void *data() { return reinterpret_cast<char *>(this) + sizeof(LUserdata); }
};

//------------------ C function registration entry
struct LReg {
    const char *name;
    CFunctionType func;
};

//------------------ Raw C function pointer type
using RawCFunction = MultiValue (*)(LState *, const LValue *, size_t);

MultiValue call_cfunc_rooted(
    LState *L, RawCFunction f, const LValue *args, size_t count, const CallSite *cs = default_call_site());

//------------------ C function registration (lazy init)
struct LazyReg {
    const char *name;
    RawCFunction func;
};

//------------------ Hash/array table: EMPTY=0 makes calloc'd arrays valid; TOMBSTONE keeps evacuated slots probing.
static constexpr uint64_t HASH_EMPTY = 0;
static constexpr uint64_t HASH_TOMBSTONE = 0xFFFFFFFFFFFFFFFEULL;

struct alignas(8) HashEntry {
    TValue key;
    TValue val;
    ValueType ktype;
    ValueType vtype;
};

static_assert(sizeof(HashEntry) == 24, "HashEntry must be 24 bytes");

struct LTable;
struct LTableExt;

static constexpr int LTABLE_IC_SIZE = 8;

struct LTableInlineCache {
    uint64_t key_payload = 0;
    uint32_t entry_idx = 0;
    uint32_t table_ver = 0;
};

//------------------ Lazily-allocated hash/metatable state (keeps LTable compact)
struct LTableExt {
    HashEntry *entries = nullptr;
    size_t hash_size = 0;
    size_t hash_count = 0;
    size_t hash_tombs = 0;
    uint64_t *hash_bitmap = nullptr;
    uint32_t hash_version = 0;
    LTableInlineCache *ic = nullptr;
    LTable *metatable = nullptr;
    LTable *meta_next = nullptr;
};

struct LTable : public LHeader {
    TValue *array;
    ValueType *array_types;
    size_t array_size;
    size_t array_cap;
    //------------------ Length hint: array[0..array_lim-1] are all non-nil (array_lim <= array_size).
    //------------------ A stale-low hint only costs a rescan; writers keep it from ever going stale-high.
    size_t array_lim;
    TValue small_array[2];
    ValueType small_array_types[2];

    LTableExt *ext;

    LTable();
    ~LTable();

    LValue gettable(const LValue &key);
    void settable(const LValue &key, const LValue &val);
    //------------------ op_direct: fused single-walk t[k]=t[k]<op>amount; misses take the full generic path (arith-on-nil, metatables).
    void op_direct(LState *L, const LValue &obj, const LValue &key, double amount, int op_kind, bool nil_ok);

    LValue get_value(LState *L, const LValue &key);
    void set_value(LState *L, const LValue &key, const LValue &val);

    void bind(const char *name, const LValue &val);
    void bind(LState *L, const char *name, CFunctionType func);
    void bind_all(LState *L, std::initializer_list<LReg> funcs);

    //------------------ presize_hash: pre-allocate storage for `expected` entries at 1/2 load; no-op if already large enough.
    void presize_hash(size_t expected);

private:
    void resize_hash(size_t new_size);
};

static_assert(sizeof(LTable) <= 96, "LTable must be compact (~96 bytes)");

//------------------ Length-hint maintenance for a direct array-part write at 0-based idx0 (call only for
//------------------ slots inside the array part; gap fills above array_size need no update). A nil stored
//------------------ below the hint pulls it down; a value stored exactly at the hint extends it by one.
//------------------ Both are O(1); forward progress is recovered by the next length query.
CLX_INLINE_HOT void table_hint_on_array_write(LTable *t, size_t idx0, ValueType vt) {
    size_t h = t->array_lim;
    if (vt == Nil) {
        if (idx0 < h)
            t->array_lim = idx0;
    } else if (idx0 == h) {
        t->array_lim = h + 1;
    }
}

//------------------ Scan the array part for the first nil starting at the hint; heals array_lim to the
//------------------ boundary found (clamped to array_size). Callers probe integer keys above array_size after.
CLX_INLINE_HOT size_t table_array_prefix_len(LTable *t) {
    size_t h = t->array_lim;
    size_t n = t->array_size;
    if (h < n)
        h += clx_find_first_nil(reinterpret_cast<const uint8_t *>(t->array_types) + h, n - h);
    t->array_lim = h;
    return h;
}
//------------------ Layout note (#4): hot array fields (array, array_types, array_size, array_cap, array_lim)
//------------------ directly follow the 16-byte LHeader, so header + hot fields occupy the first 64-byte cache
//------------------ line on the t[i] fast path. (No offsetof assert: LTable is non-standard-layout via LHeader.)

//------------------ Generational write barrier

void gc_barrier_header(LState *L, LHeader *owner, const LValue &newval);

CLX_INLINE_HOT void gc_barrier(LState *L, LHeader *owner, const LValue &newval) {
    if (owner->age == AGE_YOUNG || !newval.is_gc_obj())
        return;
    gc_barrier_header(L, owner, newval);
}

CLX_INLINE_HOT void gc_barrier_table(LState *L, const LTable *owner, const LValue &newval) {
    if (owner->age == AGE_YOUNG || !newval.is_gc_obj())
        return;
    gc_barrier_header(L, const_cast<LTable *>(owner), newval);
}

//------------------ Tag-based table barrier for sites that already hold a raw TValue/ValueType pair
//------------------ (in-place array swap/store fast paths). Skips boxing the pair into a temporary
//------------------ LValue; the box happens lazily inside the cold slow path only when it fires.
CLX_INLINE_HOT void gc_barrier_table(LState *L, const LTable *owner, const TValue &v, ValueType t) {
    if (owner->age == AGE_YOUNG || !is_gc_obj_tag(t))
        return;
    gc_barrier_header(L, const_cast<LTable *>(owner), LValue(v, t));
}

void gc_barrier_cell(LState *L, const LUpValue &cell, const LValue &newval);

//------------------ LTable accessor helpers (compact-layout indirection)
CLX_INLINE LTable *tbl_metatable(const LTable *t) {
    return t->ext ? t->ext->metatable : nullptr;
}

CLX_INLINE LTableExt *tbl_ensure_ext(LTable *t) {
    if (t->ext == nullptr) {
        t->ext = new LTableExt();
    }
    return t->ext;
}

CLX_INLINE void tbl_set_metatable(LTable *t, LTable *m) {
    if (t->ext) {
        t->ext->metatable = m;
        t->ext->hash_version++;
    } else if (m) {
        tbl_ensure_ext(t)->metatable = m;
        t->ext->hash_version++;
    }
}

//------------------ Co-allocated array block: one malloc holding TValue[cap] followed by ValueType[cap].
//------------------ Zeroed memory == initialized (TValue u64=0, Nil=0), so calloc/malloc+memset both work.
//------------------ The types pointer always derives from the base: types = base + cap*sizeof(TValue).
//------------------ Freeing is a single free(base); array_types points inside the block and is never freed.
CLX_INLINE size_t table_array_block_bytes(size_t cap) {
    return cap * sizeof(TValue) + cap * sizeof(ValueType);
}

CLX_INLINE_HOT void table_alloc_array(TValue *&arr, ValueType *&types, size_t cap) {
    if (cap == 0) {
        arr = nullptr;
        types = nullptr;
        return;
    }
    char *mem = static_cast<char *>(std::calloc(1, table_array_block_bytes(cap)));
    if (!mem)
        throw std::bad_alloc();
    arr = reinterpret_cast<TValue *>(mem);
    types = reinterpret_cast<ValueType *>(mem + cap * sizeof(TValue));
}

CLX_INLINE_HOT void table_free_array(TValue *&arr, ValueType *&types, const TValue *small_block) {
    if (arr && arr != small_block)
        std::free(arr);
    arr = nullptr;
    types = nullptr;
}

//------------------ Fused hash block: one calloc holding entries[cap] followed by bitmap words.
//------------------ entries sits at offset 0, so freeing entries frees the whole block; hash_bitmap
//------------------ points inside and is never freed separately. cap*24 is always 8-byte aligned
//------------------ (sizeof(HashEntry)==24), so the bitmap needs no padding. Arena-owned tables never
//------------------ use this: their entries live in the arena with a null bitmap until first resize.
CLX_INLINE size_t table_hash_block_bytes(size_t cap) {
    return cap * sizeof(HashEntry) + ((cap + 63) / 64) * sizeof(uint64_t);
}

CLX_INLINE_HOT void table_alloc_hash(HashEntry *&entries, uint64_t *&bitmap, size_t cap) {
    if (cap == 0) {
        entries = nullptr;
        bitmap = nullptr;
        return;
    }
    char *mem = static_cast<char *>(std::calloc(1, table_hash_block_bytes(cap)));
    if (!mem)
        throw std::bad_alloc();
    entries = reinterpret_cast<HashEntry *>(mem);
    bitmap = reinterpret_cast<uint64_t *>(mem + cap * sizeof(HashEntry));
}

CLX_INLINE_HOT void table_free_hash(HashEntry *&entries, uint64_t *&bitmap) {
    if (entries)
        std::free(entries);
    entries = nullptr;
    bitmap = nullptr;
}

//------------------ table_heap_bytes: heap bytes owned by a table's side buffers (mirrors the free paths); arena tables own none
CLX_INLINE size_t table_heap_bytes(const LTable *t) {
    if (t->flags & LFLAG_ARENA)
        return 0;
    size_t b = 0;
    if (t->array && t->array != t->small_array)
        b += t->array_cap * (sizeof(TValue) + sizeof(ValueType));
    if (t->ext) {
        b += sizeof(LTableExt);
        if (t->ext->entries) {
            b += t->ext->hash_size * sizeof(HashEntry);
            b += ((t->ext->hash_size + 63) / 64) * sizeof(uint64_t);
        }
        if (t->ext->ic)
            b += static_cast<size_t>(LTABLE_IC_SIZE) * sizeof(LTableInlineCache);
    }
    return b;
}

//------------------ Wyhash secret constants
static constexpr uint64_t WY_SECRET0 = 0xa0761d6478bd642fULL;
static constexpr uint64_t WY_SECRET1 = 0xe7037ed1a0b428dbULL;

//------------------ 64-bit wyhash mix
static CLX_INLINE_HOT uint64_t wyhash64(uint64_t v) {
    v ^= WY_SECRET0;
    uint64_t lo, hi;
    lo = clx_umul128(v, v ^ WY_SECRET1, &hi);
    return lo ^ hi;
}

//------------------ SWAR hash for <=8 byte strings
static CLX_INLINE uint64_t swar_hash_8(const char *p, size_t len) {
    uint64_t data = 0;
    if (len > 0 && len <= 8)
        clx_memcpy(&data, p, len);
    return wyhash64(data ^ static_cast<uint64_t>(len));
}

//------------------ Wyhash for arbitrary-length strings
static CLX_INLINE uint64_t wyhash_str(const char *p, size_t len) {
    uint64_t seed = WY_SECRET0 ^ static_cast<uint64_t>(len);
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t chunk;
        clx_memcpy(&chunk, p + i, 8);
        chunk ^= WY_SECRET1;
        uint64_t lo, hi;
        lo = clx_umul128(seed, chunk, &hi);
        seed = lo ^ hi;
    }
    uint64_t tail = 0;
    size_t rem = len - i;
    if (rem > 0) {
        if (rem >= 4) {
            uint32_t lo, hi;
            clx_memcpy(&lo, p + i, 4);
            clx_memcpy(&hi, p + i + rem - 4, 4);
            tail = (static_cast<uint64_t>(hi) << 32) | lo;
        } else if (rem >= 2) {
            uint16_t lo;
            clx_memcpy(&lo, p + i, 2);
            tail = (static_cast<uint64_t>(p[i + rem - 1]) << 16) | lo;
        } else {
            tail = static_cast<uint8_t>(p[i]);
        }
        tail ^= WY_SECRET1;
        uint64_t lo, hi;
        lo = clx_umul128(seed, tail, &hi);
        seed = lo ^ hi;
    }
    return seed ^ (seed >> 32);
}

//------------------ Hash an LValue by type
static CLX_INLINE_HOT uint64_t lvalue_hash(const LValue &key) {
    if (key.type == ValueType::String) {
        if (key.val.payload.u64 >> 56) {
            uint32_t len = static_cast<uint32_t>(key.val.payload.u64 >> 56);
            const char *data = reinterpret_cast<const char *>(&key.val.payload.u64);
            return swar_hash_8(data, len);
        }
        if (key.val.payload.u64 == 0)
            return swar_hash_8("", 0);
        const char *ptr = static_cast<const char *>(key.val.payload.ptr);
        uint32_t h_low;
        clx_memcpy(&h_low, ptr - 16, 4);
        return h_low;
    }
    if (key.type == ValueType::Int64) {
        double vd = static_cast<double>(key.val.payload.i64);
        uint64_t v;
        clx_memcpy(&v, &vd, sizeof(uint64_t));
        v ^= WY_SECRET0;
        uint64_t lo, hi;
        lo = clx_umul128(v, v ^ WY_SECRET1, &hi);
        return lo ^ hi;
    }
    uint64_t v = key.val.payload.u64 ^ (static_cast<uint64_t>(static_cast<uint8_t>(key.type)) << 56);
    v ^= WY_SECRET0;
    uint64_t lo, hi;
    lo = clx_umul128(v, v ^ WY_SECRET1, &hi);
    return lo ^ hi;
}

//------------------ Fast cross-type equality (string/int/number/double)
static CLX_INLINE_HOT bool lvalue_eq_fast(const LValue &a, const LValue &b) {
    if (a.val.payload.u64 == b.val.payload.u64 && a.type == b.type)
        return true;
    bool a_str = a.type == ValueType::String;
    bool b_str = b.type == ValueType::String;
    if (a_str && b_str) {
        uint32_t al = a.string_len();
        uint32_t bl = b.string_len();
        return al == bl && clx_memcmp(a.as_string(), b.as_string(), al) == 0;
    }
    if ((a.type == ValueType::Int64 && b.type == ValueType::Double)
        || (a.type == ValueType::Double && b.type == ValueType::Int64)) {
        double da = a.as_number(), db = b.as_number();
        return da == db;
    }
    return false;
}

//------------------ Bump arena for interned strings
struct StringArena {
    struct Block {
        char *base;
        size_t used;
        size_t capacity;
        Block *next;

        Block(size_t cap)
            : base(new char[cap])
            , used(0)
            , capacity(cap)
            , next(nullptr) { }

        ~Block() { delete[] base; }
    };

    Block *head = nullptr;
    Block *current = nullptr;
    size_t block_size;

    static constexpr size_t DEFAULT_BLOCK_SIZE = 65536;

    StringArena(size_t bs = DEFAULT_BLOCK_SIZE)
        : block_size(bs) {
        head = current = new Block(block_size);
    }

    ~StringArena() {
        Block *b = head;
        while (b) {
            Block *n = b->next;
            delete b;
            b = n;
        }
    }

    StringArena(const StringArena &) = delete;
    StringArena &operator=(const StringArena &) = delete;

    CLX_INLINE char *allocate(size_t size) {
        size = (size + 15) & ~size_t(15);
        if (!current || current->used + size > current->capacity) {
            Block *b = new Block(std::max(block_size, size));
            current->next = b;
            current = b;
        }
        char *result = current->base + current->used;
        current->used += size;
        return result;
    }
};

//------------------ String pool: slots hold only the baked pointer; hash-low/length live in the string header (8-byte slots).
struct StringPool {
    struct Slot {
        char *baked;

        bool empty() const { return baked == nullptr; }
    };

    Slot *slots = nullptr;
    size_t capacity = 0;
    size_t count = 0;
    StringArena arena;

    static constexpr size_t INIT_CAP = 64;

    StringPool()
        : arena(65536) {
        rehash(INIT_CAP);
    }

    ~StringPool() { delete[] slots; }

    StringPool(const StringPool &) = delete;
    StringPool &operator=(const StringPool &) = delete;

    const char *intern(const char *str, size_t len, uint64_t h) {
        if (count * 2 >= capacity)
            rehash(capacity * 2);
        size_t mask = capacity - 1;
        size_t idx = h & mask;
        for (;;) {
            Slot &s = slots[idx];
            if (s.empty()) {
                s = make_slot(str, len, h);
                count++;
                return s.baked;
            }
            if (slot_matches(s.baked, str, len, h))
                return s.baked;
            idx = (idx + 1) & mask;
        }
    }

    const char *intern_preallocated(char *prealloc, uint64_t h, size_t len) {
        if (count * 2 >= capacity)
            rehash(capacity * 2);
        size_t mask = capacity - 1;
        size_t idx = h & mask;
        for (;;) {
            Slot &s = slots[idx];
            if (s.empty()) {
                s = make_slot(prealloc, len, h);
                delete[] (prealloc - 16);
                count++;
                return s.baked;
            }
            if (slot_matches(s.baked, prealloc, len, h)) {
                delete[] (prealloc - 16);
                return s.baked;
            }
            idx = (idx + 1) & mask;
        }
    }

    const char *lookup(const char *str, size_t len, uint64_t h) const {
        if (count == 0)
            return nullptr;
        size_t mask = capacity - 1;
        size_t idx = h & mask;
        for (;;) {
            const Slot &s = slots[idx];
            if (s.empty())
                return nullptr;
            if (slot_matches(s.baked, str, len, h))
                return s.baked;
            idx = (idx + 1) & mask;
        }
    }

    void reserve(size_t n) {
        if (n > capacity)
            rehash(n);
    }

    struct PrecomputedEntry {
        const char *s;
        uint32_t len;
        uint64_t hash;
        uint32_t slot;
    };

    void bulk_fill_precomputed(const PrecomputedEntry *entries, size_t n) {
        size_t total_arena = 0;
        for (size_t i = 0; i < n; ++i) {
            size_t entry_size = 16 + entries[i].len + 1;
            total_arena += (entry_size + 15) & ~size_t(15);
        }

        char *arena_base = arena.allocate(total_arena);
        char *arena_ptr = arena_base;

        for (size_t i = 0; i < n; ++i) {
            uint32_t len32 = entries[i].len;
            uint32_t h_low = static_cast<uint32_t>(entries[i].hash);
            clx_memcpy(arena_ptr, &h_low, 4);
            clx_memcpy(arena_ptr + 8, &len32, 4);
            clx_memcpy(arena_ptr + 16, entries[i].s, entries[i].len);
            arena_ptr[16 + entries[i].len] = '\0';

            Slot &s = slots[entries[i].slot];
            s.baked = arena_ptr + 16;

            size_t entry_size = 16 + entries[i].len + 1;
            arena_ptr += (entry_size + 15) & ~size_t(15);
        }
        count += n;
    }

private:
    //------------------ slot_matches: probe compare via the baked header (h_low at -16, len at -8)
    static bool slot_matches(const char *baked, const char *str, size_t len, uint64_t h) {
        uint32_t s_low;
        uint32_t s_len;
        clx_memcpy(&s_low, baked - 16, 4);
        clx_memcpy(&s_len, baked - 8, 4);
        return s_low == static_cast<uint32_t>(h) && s_len == static_cast<uint32_t>(len)
            && clx_memcmp(baked, str, len) == 0;
    }

    //------------------ slot_hash: cached header hash low 32 bits; index bits match the full hash below capacity 2^31.
    static uint64_t slot_hash(const char *baked) {
        uint32_t s_low;
        clx_memcpy(&s_low, baked - 16, 4);
        return s_low;
    }

    Slot make_slot(const char *str, size_t len, uint64_t h) {
        size_t total = 16 + len + 1;
        total = (total + 15) & ~size_t(15);
        char *mem = arena.allocate(total);
        uint32_t len32 = static_cast<uint32_t>(len);
        uint32_t h_low = static_cast<uint32_t>(h);
        clx_memcpy(mem, &h_low, 4);
        clx_memcpy(mem + 8, &len32, 4);
        clx_memcpy(mem + 16, str, len);
        mem[16 + len] = '\0';
        return Slot { mem + 16 };
    }

    void rehash(size_t new_cap) {
        Slot *old = slots;
        size_t old_cap = capacity;
        slots = new Slot[new_cap]();
        capacity = new_cap;
        size_t mask = new_cap - 1;
        for (size_t i = 0; i < old_cap; ++i) {
            if (!old[i].empty()) {
                size_t idx = slot_hash(old[i].baked) & mask;
                while (!slots[idx].empty())
                    idx = (idx + 1) & mask;
                slots[idx] = old[i];
            }
        }
        delete[] old;
    }
};

//------------------ Coroutine thread status enum
enum ThreadStatus { THREAD_SUSPENDED = 0, THREAD_RUNNING = 1, THREAD_DEAD = 2, THREAD_NORMAL = 3 };

//------------------ Coroutine thread
struct LThread : public LHeader {
    LState *state;
    LValue function;
    int status;
    LThread *caller;
    MultiValue yield_args;
    MultiValue resume_args;
    bool is_main;
    bool has_error;
    bool close_requested;
    size_t stack_bytes = 0;
    bool fiber_started = false;
    bool pre_unwind = false;
    //---------- started: true once the thread has been resumed at least once in its current incarnation
    bool started = false;
    //---------- yield_count: how often this thread continued after a yield; lua_callk/lua_pcallk diff it across a call to detect one
    int yield_count = 0;
    //---------- luaapi: backing ApiBundle for the Lua 5.5 C API (owned by libclx_capi)
    void *luaapi = nullptr;
    //---------- top_root: this thread's root-frame chain head; empty while the thread is running (the head lives in LState)
    RootRange *top_root = nullptr;
#if defined(_WIN32)
    LPVOID fiber;
#elif (defined(__APPLE__) || defined(__linux__)) && (defined(__aarch64__) || defined(__x86_64__))
    CoroutineContext ctx;
    char *stack_memory;
#else
    ucontext_t ctx;
    char *stack_memory;
#endif

    LThread();
    ~LThread();
};

//------------------ VM state
struct TableSlab {
    TableSlab *next;
};

//------------------ CallSite: immutable source location + name/method flags for one call site
struct CallSite {
    const char *file;
    int line;
    bool is_method;
    bool name_short;
};

inline const CallSite *default_call_site() {
    static constexpr CallSite site = { "", 0, false, false };
    return &site;
}

struct LState {
    LTable *_G;
    LCFunction *current_func = nullptr;
    LHeader *allocated_objects;
    //------------------ Old-generation chain: minors promote here and never walk it (see gc_minor sweep),
    //------------------ so stop-the-world minors skip long-lived objects entirely. Majors reunite both
    //------------------ chains before sweeping and re-partition after; close/membership walks cover both.
    LHeader *old_objects = nullptr;
    LTable *free_tables;
    //------------------ Big-buffer free list: tables retaining a heap array park here so size-unknown
    //------------------ creations ({}) never inherit a 90KB array they will not use while the table that
    //------------------ needs it allocates another (linear pool growth wave-over-wave; see create_table).
    LTable *free_tables_big = nullptr;
    LCFunction *free_functions;
    LThread *free_threads = nullptr;
    size_t free_fiber_threads = 0;
    LTable *metatabled_tables = nullptr;
    //------------------ Metatabled-userdata list: files register here so the protect passes can shield
    //------------------ their metatable (+ __gc closure) when the userdata itself dies in the same cycle.
    LUserdata *metatabled_ud = nullptr;

    //------------------ LTable slab arena: bump-allocated object slabs (freed whole at close)

    static constexpr size_t SLAB_TABLE_COUNT = 4096;
    char *slab_current = nullptr;
    char *slab_end = nullptr;
    TableSlab *slab_blocks = nullptr;
    LTable *slab_alloc_table();
    void slab_release();

    LThread *main_thread = nullptr;
    LThread *running_thread = nullptr;

    //---------- top_root: head of the running thread's RootRange chain (one frame per generated function)
    RootRange *top_root = nullptr;

    //------------------ Active call site: the descriptor captured at the most recent call/statement, read by
    //------------------ file_line_prefix and bad_arg_msg for Lua-compatible error context.
    const CallSite *current_site = nullptr;
    //------------------ pcall_site: stable descriptor for errors raised directly under pcall (file, no line)
    CallSite pcall_site = { "", 0, false, false };

    //---------- luaapi: backing ApiBundle for the Lua 5.5 C API (src/runtime/luaapi.cpp)
    void *luaapi = nullptr;

    //---------- luaapi_cleanup: teardown hook installed by the optional clx_capi bridge archive
    void (*luaapi_cleanup)(LState *) = nullptr;
    //---------- luaapi_thread_reset: recycle hook installed by the optional clx_capi bridge archive
    void (*luaapi_thread_reset)(LState *, void *) = nullptr;

    size_t object_count;
    size_t gc_bytes_threshold;
    size_t allocated_bytes = 0;

    size_t get_memory_usage() const { return allocated_bytes; }

    //------------------ account_delta: track side-buffer growth/shrink so GC pacing sees real bytes, not just headers
    CLX_INLINE void account_delta(size_t before, size_t after) {
        if (after > before)
            allocated_bytes += after - before;
        else if (before > after) {
            size_t d = before - after;
            allocated_bytes = (d > allocated_bytes) ? 0 : allocated_bytes - d;
        }
    }

    LValue str_index;
    LValue str_newindex;
    LValue str_gc;
    LValue str_call;
    LValue str_close;
    LValue str_pairs;
    LValue str_tostring;
    LValue str_len;
    LValue str_eq;
    LValue str_lt;
    LValue str_le;
    LValue str_concat;
    LValue str_add;
    LValue str_sub;
    LValue str_mul;
    LValue str_div;
    LValue str_mod;
    LValue str_idiv;
    LValue str_pow;
    LValue str_unm;
    LValue str_band;
    LValue str_bor;
    LValue str_bxor;
    LValue str_shl;
    LValue str_shr;
    LValue str_bnot;
    LValue str_metatable;
    LValue str_name;
    LValue str_ipairs;
    LTable *string_metatable;

    StringPool string_pool;

    CLX_INLINE LValue intern_lvalue(const char *str, size_t len) {
        if (len <= 6)
            return LValue::istr(str, len);
        return LValue(intern_string(str, len));
    }

    CLX_INLINE LValue intern_lvalue(const std::string &s) { return intern_lvalue(s.data(), s.size()); }

    CLX_INLINE const char *intern_string(const char *str, size_t len) {
        uint64_t h = len <= 8 ? swar_hash_8(str, len) : wyhash_str(str, len);
        return string_pool.intern(str, len, h);
    }

    CLX_INLINE const char *intern_string(const std::string &s) { return intern_string(s.data(), s.size()); }

    CLX_INLINE const char *intern_string(const char *str) { return intern_string(str, clx_strlen(str)); }

    LState();
    ~LState();

    //------------------ GCStack: raw pointer stack for GC worklists.

    struct GCStack {
        LHeader **data = nullptr;
        size_t top = 0;
        size_t cap = 0;

        CLX_INLINE_HOT void push(LHeader *h) {
            if (top >= cap)
                gc_stack_grow(this);
            data[top++] = h;
        }

        CLX_INLINE_HOT LHeader *back() const { return data[top - 1]; }

        CLX_INLINE_HOT void pop_back() { --top; }

        CLX_INLINE_HOT bool empty() const { return top == 0; }

        CLX_INLINE_HOT void clear() { top = 0; }

        CLX_INLINE_HOT size_t size() const { return top; }

        void reserve(size_t n);
        void free();
    };

    static void gc_stack_grow(GCStack *s);

    LValue create_table(size_t asize = 0, size_t hsize = 0);
    LValue create_closure(CFunctionType func, LTable *env = nullptr, std::vector<LUpValue> gc_cells = { });

    GCStack gc_worklist;
    std::vector<LValue> permanent_roots;
    GCStack gc_recent;

    void root_value(const LValue &v) {
        if (v.is_gc_obj())
            permanent_roots.push_back(v);
    }

    void unroot_value(const LValue &v) {
        if (!v.is_gc_obj())
            return;
        for (size_t i = permanent_roots.size(); i-- > 0;) {
            if (permanent_roots[i].is_gc_obj() && permanent_roots[i].as_pointer() == v.as_pointer()) {
                permanent_roots.erase(permanent_roots.begin() + i);
                return;
            }
        }
    }

    enum class GCPhase : uint8_t { Idle, Sweeping };
    enum class GCMode : uint8_t { Incremental, Generational };
    GCPhase gc_phase = GCPhase::Idle;
    bool gc_minor_active = false;
    GCMode gc_mode = GCMode::Generational;
    LHeader *gc_sweep_cursor = nullptr;
    LHeader *gc_prev = nullptr;
    LHeader *gc_finalizable = nullptr;
    LHeader *gc_finalizable_ud = nullptr;
    bool gc_draining = false;
    static constexpr size_t GC_STEP_BUDGET = 512;
    bool gc_running = true;
    int gc_pause = 200;
    int gc_stepmul = 200;
    int gc_stepsize = 13;
    int gc_minormul = 20;
    int gc_majorminor = 0;
    int gc_minormajor = 0;

    //------------------ Generational GC state

    //------------------ Adaptive minor pacing: next trigger = one current heap-full, clamped (CLX_GC_MINOR_KB pins it)
    //------------------ Initial value must be the MIN, not the MAX: the first minor reclaims ~nothing (gc_recent
    //------------------ roots everything allocated since the previous cycle), so starting at 8MB feeds the adaptive
    //------------------ clamp a heap still holding garbage and pins it right back at 8MB — leaving no collections at all.
    size_t gc_minor_threshold = kMinorThresholdMin;
    bool gc_minor_threshold_env = false;
    static constexpr size_t kMinorThresholdMin = 256 * 1024;
    static constexpr size_t kMinorThresholdMax = 8 * 1024 * 1024;
    size_t gc_major_threshold = 32 * 1024 * 1024;
    size_t gc_old_bytes = 0;
    size_t gc_bytes_at_minor = 0;
    bool gc_major_pending = false;
    std::vector<LHeader *> gc_remembered;
    GCStack gc_pinned;
    std::vector<LUpValue> gc_remembered_cells;
    std::unordered_set<const LValue *> gc_remembered_cell_set;

    //------------------ Per-function arenas: their tables live outside allocated_objects; each mark phase clears their marks.

    FuncArena *active_arenas = nullptr;

    //------------------ GC instrumentation (generational.md step 1)
    size_t gc_stats_collects = 0;
    size_t gc_stats_minors = 0;
    size_t gc_stats_minor_freed = 0;
    size_t gc_stats_major_freed = 0;
    size_t gc_stats_remembered_high = 0;

    CLX_INLINE size_t gc_heap_object_size(const LHeader *h) const {
        switch (h->type) {
        case static_cast<uint8_t>(Table):
            return sizeof(LTable);
        case static_cast<uint8_t>(Function):
            return sizeof(LCFunction);
        case static_cast<uint8_t>(Thread): {
            const LThread *th = static_cast<const LThread *>(h);
            return sizeof(LThread) + th->stack_bytes;
        }
        case static_cast<uint8_t>(UserData): {
            const LUserdata *ud = static_cast<const LUserdata *>(h);
            return sizeof(LUserdata) + ud->size;
        }
        default:
            return 0;
        }
    }

    CLX_INLINE void gc_update_old_bytes(LHeader *h, uint8_t from, uint8_t to) {
        if (h->flags & LFLAG_VM_PROXY)
            return;
        size_t sz = gc_heap_object_size(h);
        if (sz == 0)
            return;
        if (to == AGE_OLD && from != AGE_OLD)
            gc_old_bytes += sz;
        else if (from == AGE_OLD && to != AGE_OLD)
            gc_old_bytes -= (sz <= gc_old_bytes ? sz : gc_old_bytes);
    }

    //------------------ Allocation-site GC trigger (shared by the allocator paths)
    CLX_INLINE_HOT void gc_maybe_collect() {
        if (!gc_running)
            return;
        //------------------ a __gc finalizer draining at the end of gc_minor() must not start another
        //------------------ collection while that minor's sweep state is still live.
        if (gc_minor_active)
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

    //------------------ Generational write barrier helpers
    void gc_remember(LHeader *owner);
    void gc_remember_cell(const LUpValue &cell);

    void gc_minor();
    void collect_garbage();
    void collect_garbage_full();
    bool gc_step();
    void invoke_gc_finalizer(LUserdata *ud, const char *tag);
    bool is_allocated_userdata(const LHeader *h) const;

    LValue *overflow_heap = nullptr;
    size_t overflow_heap_cap = 0;
    size_t overflow_heap_used = 0;

    CLX_INLINE_HOT LValue *alloc_overflow(size_t n) {
        if (overflow_heap_used + n > overflow_heap_cap) {
            size_t new_cap = overflow_heap_cap ? overflow_heap_cap * 2 : 64;
            while (new_cap < overflow_heap_used + n)
                new_cap *= 2;
            overflow_heap = static_cast<LValue *>(std::realloc(overflow_heap, new_cap * sizeof(LValue)));
            overflow_heap_cap = new_cap;
        }
        LValue *result = overflow_heap + overflow_heap_used;
        overflow_heap_used += n;
        return result;
    }

    void register_module(const std::string &name, LValue (*func)(LState *));
    void register_loaded_module(const std::string &name, const LValue &module);
    //------------------ _layout_reserve: holds sizeof(LState) at its pre-call-site-refactor footprint.
    //------------------ Shrinking the struct shifts every later heap allocation; that changed table
    //------------------ inline-cache cache-set aliasing and slowed nbody ~1.7x. The reserve keeps the
    //------------------ established allocation layout. Re-validate hot table benchmarks before resizing.
    char _layout_reserve[256] = {};
};

//------------------ RootRange — one GC-root frame. The constructor links it into the owning thread's chain, the
//------------------ destructor unlinks it (RAII, so unwinding unroots without any explicit restore). `base[0..n)` is
//------------------ marked by value; every `ind[i]` is an alias to a live LValue and marked by dereference.
struct RootRange {
    RootRange *prev;
    LState *owner;
    LValue *base;
    size_t n;
    LValue **ind;
    size_t ind_n;

    CLX_INLINE_HOT RootRange(LState *L, LValue *b, size_t count, LValue **iv = nullptr, size_t iv_count = 0)
        : prev(L->top_root)
        , owner(L)
        , base(b)
        , n(count)
        , ind(iv)
        , ind_n(iv_count) {
        L->top_root = this;
    }

    CLX_INLINE_HOT ~RootRange() {
        if (owner->top_root == this)
            owner->top_root = prev;
    }

    RootRange(const RootRange &) = delete;
    RootRange &operator=(const RootRange &) = delete;
};

//------------------ FrameBase — watermark logic shared by every Frame instantiation. `used` counts live
//------------------ aliases; blocks save it and restore it, which reuses slots across disjoint blocks.
//------------------ `compact(base, mid)` folds callee-pushed tail entries down over a discarded base region
//------------------ (the old multi-assign compaction), handling the empty-tail case the same way.
struct FrameBase : RootRange {
    size_t used = 0;
    LValue **_heap = nullptr;
    size_t _cap = 0;

    CLX_INLINE_HOT explicit FrameBase(LState *L)
        : RootRange(L, nullptr, 0, nullptr, 0) { }

    CLX_INLINE_HOT ~FrameBase() { delete[] _heap; }

    CLX_INLINE_COLD void grow() {
        size_t ncap = _cap ? _cap * 2 : 64;
        LValue **nh = new LValue *[ncap];
        for (size_t i = 0; i < used; ++i)
            nh[i] = ind[i];
        delete[] _heap;
        _heap = nh;
        ind = nh;
        _cap = ncap;
    }

    //------------------ push lives on the base (non-virtual, still inlined at callers) so that fast
    //------------------ lambdas — which only ever see a FrameBase& — can root their locals in the
    //------------------ caller's frame. Slots accumulate until the owning function returns, exactly like
    //------------------ the old shadow stack; a private sub-frame per fast call would defeat C++ tail
    //------------------ calls (its destructor runs after the recursive call) and overflow the stack.
    CLX_INLINE_HOT void push(const LValue &v) {
        if (used >= _cap)
            grow();
        ind[used++] = const_cast<LValue *>(&v);
        ind_n = used;
    }

    CLX_INLINE_HOT size_t mark() const { return used; }

    CLX_INLINE_HOT void restore(size_t m) {
        used = m;
        ind_n = m;
    }

    CLX_INLINE_HOT void pop() {
        --used;
        ind_n = used;
    }

    CLX_INLINE_HOT void pop_n(size_t k) {
        used -= k;
        ind_n = used;
    }

    CLX_INLINE_HOT void compact(size_t base, size_t mid) {
        if (used > mid) {
            size_t tail = used - mid;
            for (size_t i = 0; i < tail; ++i)
                ind[base + i] = ind[mid + i];
            used = base + tail;
        } else {
            used = base;
        }
        ind_n = used;
    }
};

//------------------ Frame<N> — the single root frame emitted at the top of every generated function and of the
//------------------ main chunk. Slots are indirect (each aliases an LValue with a stable address: a local, an
//------------------ argument array element, an upvalue cell, or a runtime temporary), so a reassigned local is
//------------------ always seen by the collector. N inline slots cover every realistic function; a heap spill
//------------------ (transparent to the collector, which only ever reads `ind[0..ind_n)`) covers the rest.
template<size_t N> struct Frame : FrameBase {
    LValue *_inline_buf[N];

    CLX_INLINE_HOT explicit Frame(LState *L)
        : FrameBase(L) {
        ind = _inline_buf;
        _cap = N;
    }

    Frame(const Frame &) = delete;
    Frame &operator=(const Frame &) = delete;
};

//------------------ FrameGuard — RAII watermark save/restore for one block scope (unwind-safe by construction)
struct FrameGuard {
    FrameBase *f;
    size_t m;

    CLX_INLINE_HOT explicit FrameGuard(FrameBase &fr)
        : f(&fr)
        , m(fr.mark()) { }

    CLX_INLINE_HOT ~FrameGuard() { f->restore(m); }

    FrameGuard(const FrameGuard &) = delete;
    FrameGuard &operator=(const FrameGuard &) = delete;
};

//------------------ gc_walk_roots — mark every root in one thread's frame chain
template<typename F> static void gc_walk_roots(RootRange *head, F &&mark) {
    for (RootRange *r = head; r; r = r->prev) {
        for (size_t i = 0; i < r->n; ++i)
            mark(r->base[i]);
        for (size_t i = 0; i < r->ind_n; ++i)
            mark(*r->ind[i]);
    }
}

//------------------ Rooted call wrappers: alias func + args into a RootRange around a call; RAII unlinks it
//------------------ call_function_rooted/call_direct_rooted forward without their own RootRange:
//------------------ call_function/call_direct root func+args themselves on entry, so a wrapper
//------------------ range would double-root the same values on every call.
CLX_INLINE_HOT MultiValue call_function_rooted(
    LState *L, const LValue &func, const LValue *args, size_t count, const CallSite *cs) {
    return call_function(L, func, args, count, cs);
}

CLX_INLINE_HOT MultiValue call_direct_rooted(
    LState *L, const LValue &func, const LValue *args, size_t count, const CallSite *cs) {
    return call_direct(L, func, args, count, cs);
}

//------------------ Rooted call wrapper for direct C function pointers (hoisted builtins, direct impls)
//------------------ call_cfunc_rooted_impl: shared body for the overloads below. Templated so the
//------------------ raw-pointer overload still skips CFunctionType construction on hot builtin calls.
template<typename CFunc>
CLX_INLINE_HOT MultiValue call_cfunc_rooted_impl(
    LState *L, CFunc f, const LValue *args, size_t count, const CallSite *cs) {
    RootRange rr(L, const_cast<LValue *>(args), args ? count : 0);
    if (cs && cs->line > 0)
        L->current_site = cs;
    return f(L, args, count);
}

CLX_INLINE_HOT MultiValue call_cfunc_rooted(
    LState *L, CFunctionType f, const LValue *args, size_t count, const CallSite *cs) {
    return call_cfunc_rooted_impl(L, f, args, count, cs);
}

//------------------ Raw pointer overload: skips CFunctionType construction on hot builtin calls
CLX_INLINE_HOT MultiValue call_cfunc_rooted(
    LState *L, RawCFunction f, const LValue *args, size_t count, const CallSite *cs) {
    return call_cfunc_rooted_impl(L, f, args, count, cs);
}

//------------------ Roots args[] across the direct-call fast path; else collections inside the callee miss survivor-aged args.
CLX_INLINE_HOT MultiValue call_cfunc_direct(
    LState *L, const CFunctionType &f, const LValue *args, size_t count, const CallSite *cs = default_call_site()) {
    RootRange rr(L, const_cast<LValue *>(args), args ? count : 0);
    if (cs && cs->line > 0)
        L->current_site = cs;
    return f(L, args, count);
}

//------------------ Unboxed overload: invokes a concrete callable (a codegen lambda)
//------------------ with no type erasure at all — no box, no indirection frame, and the
//------------------ call is visible to the C++ compiler for inlining. Exact-match F wins
//------------------ overload resolution over the CFunctionType conversion, while
//------------------ CFunctionType arguments still prefer the non-template above.
template<typename F>
CLX_INLINE_HOT MultiValue call_cfunc_direct(
    LState *L, const F &f, const LValue *args, size_t count, const CallSite *cs = default_call_site()) {
    RootRange rr(L, const_cast<LValue *>(args), args ? count : 0);
    if (cs && cs->line > 0)
        L->current_site = cs;
    return f(L, args, count);
}

//------------------ Metatabled-tables list (protect-pass fast path)
CLX_INLINE void meta_list_add(LState *L, LTable *t) {
    if (t->flags & LFLAG_META_LIST)
        return;
    LTableExt *ex = tbl_ensure_ext(t);
    ex->meta_next = L->metatabled_tables;
    L->metatabled_tables = t;
    t->flags |= LFLAG_META_LIST;
}

CLX_INLINE void meta_list_remove(LState *L, LTable *t) {
    if (!(t->flags & LFLAG_META_LIST))
        return;
    if (t->ext) {
        LTable **pp = &L->metatabled_tables;
        while (*pp && *pp != t)
            pp = &(*pp)->ext->meta_next;
        if (*pp == t)
            *pp = t->ext->meta_next;
        t->ext->meta_next = nullptr;
    }
    t->flags &= ~LFLAG_META_LIST;
}

//------------------ Metatabled-userdata list (dead-__gc protection fast path)
CLX_INLINE void meta_ud_list_add(LState *L, LUserdata *ud) {
    if (ud->flags & LFLAG_META_UD)
        return;
    ud->meta_next = L->metatabled_ud;
    L->metatabled_ud = ud;
    ud->flags |= LFLAG_META_UD;
}

CLX_INLINE void meta_ud_list_remove(LState *L, LUserdata *ud) {
    if (!(ud->flags & LFLAG_META_UD))
        return;
    LUserdata **pp = &L->metatabled_ud;
    while (*pp && *pp != ud)
        pp = &(*pp)->meta_next;
    if (*pp == ud)
        *pp = ud->meta_next;
    ud->meta_next = nullptr;
    ud->flags &= ~LFLAG_META_UD;
}

inline std::string file_line_prefix(LState *L) {
    const CallSite *cs = L->current_site;
    if (cs && cs->file && cs->file[0] != '\0' && cs->line > 0)
        return std::string(cs->file) + ":" + std::to_string(cs->line) + ": ";
    return "";
}

//------------------ stock_type_name: luaL_typename mapping (Int64 and Double are both "number")
inline const char *stock_type_name(const LValue &v) {
    if (v.type == ValueType::Int64 || v.type == ValueType::Double)
        return "number";
    return VALUE_TYPE_NAMES[static_cast<size_t>(v.type)];
}

//------------------ err_func_name: stock luaL_argerror uses the short name when the call site is
//------------------ resolvable (direct field call or method); otherwise the pushglobalfuncname fallback.
inline const char *err_func_name(LState *L, const char *fname) {
    if (L->current_site && (L->current_site->is_method || L->current_site->name_short)) {
        const char *dot = std::strrchr(fname, '.');
        if (dot)
            return dot + 1;
    }
    return fname;
}

//------------------ bad_arg_msg: stock luaL_argerror message shape, incl. method-call index shift and
//------------------ "calling 'X' on bad self" for the self argument
inline std::string bad_arg_msg(LState *L, int idx, const char *fname, const std::string &expected_got) {
    const char *nm = err_func_name(L, fname);
    int eidx = idx;
    if (L->current_site && L->current_site->is_method) {
        if (idx == 1)
            return file_line_prefix(L) + "calling '" + nm + "' on bad self (" + expected_got + ")";
        eidx = idx - 1;
    }
    return file_line_prefix(L) + "bad argument #" + std::to_string(eidx) + " to '" + nm + "' (" + expected_got + ")";
}

inline void MultiValue::overflow_init(const clx::LValue *arr, size_t c) {
    if (!alloc_L)
        throw std::runtime_error(
            "clx: MultiValue overflow requires an LState; pass L when returning more than 4 values");
    overflow = alloc_L->alloc_overflow(c - INLINE_CAP);
    for (size_t i = INLINE_CAP; i < c; ++i)
        overflow[i - INLINE_CAP] = arr[i];
}

CLX_INLINE_COLD clx::LValue *MultiValue::push_grow() {
    if (!alloc_L)
        throw std::runtime_error(
            "clx: MultiValue overflow requires an LState; pass L when returning more than 4 values");
    size_t have = count - INLINE_CAP;
    std::vector<clx::LValue> tmp;
    tmp.reserve(have);
    for (size_t i = 0; i < have; ++i)
        tmp.push_back(overflow[i]);
    LValue *nb = alloc_L->alloc_overflow(have + 1);
    for (size_t i = 0; i < have; ++i)
        nb[i] = tmp[i];
    overflow = nb;
    return nb + (count++ - INLINE_CAP);
}

//------------------ String builder (part list)
class StringBuilder {
    static constexpr size_t INLINE_CAP = 8;
    static constexpr size_t ARENA_BLOCK = 4096;

    const char *parts_[INLINE_CAP];
    uint32_t lens_[INLINE_CAP];
    const char **parts;
    uint32_t *lens;
    size_t count;
    size_t cap;
    size_t total_len;
    mutable const char *cached;

    //------------------ OwnedBlock: arena chunk with its capacity (clear() rewinds to blocks[0], so sizes stay known)
    struct OwnedBlock {
        std::unique_ptr<char[]> mem;
        size_t cap = 0;
    };

    std::vector<OwnedBlock> arena_blocks;
    char *arena_cur = nullptr;
    char *arena_end = nullptr;
    //------------------ ARENA_FIRST: small first block so number/short-string appends never cost a 4KB chunk
    static constexpr size_t ARENA_FIRST = 256;

    void grow() {
        size_t new_cap = cap * 2;
        auto *np = new const char *[new_cap];
        auto *nl = new uint32_t[new_cap];
        for (size_t i = 0; i < count; ++i) {
            np[i] = parts[i];
            nl[i] = lens[i];
        }
        if (parts != parts_) {
            delete[] parts;
            delete[] lens;
        }
        parts = np;
        lens = nl;
        cap = new_cap;
    }

    char *arena_alloc(size_t n) {
        if (static_cast<size_t>(arena_end - arena_cur) < n) {
            size_t blk;
            if (arena_blocks.empty())
                blk = n > ARENA_FIRST ? n : ARENA_FIRST;
            else
                blk = n > ARENA_BLOCK ? n : ARENA_BLOCK;
            OwnedBlock ob;
            ob.mem = std::make_unique<char[]>(blk);
            ob.cap = blk;
            arena_cur = ob.mem.get();
            arena_end = arena_cur + blk;
            arena_blocks.push_back(std::move(ob));
        }
        char *p = arena_cur;
        arena_cur += n;
        return p;
    }

public:
    StringBuilder()
        : parts(parts_)
        , lens(lens_)
        , count(0)
        , cap(INLINE_CAP)
        , total_len(0)
        , cached(nullptr) { }

    ~StringBuilder() {
        if (parts != parts_) {
            delete[] parts;
            delete[] lens;
        }
    }

    StringBuilder(const StringBuilder &o)
        : parts(parts_)
        , lens(lens_)
        , count(0)
        , cap(INLINE_CAP)
        , total_len(0)
        , cached(nullptr) {
        for (size_t i = 0; i < o.count; ++i)
            append_owned(o.parts[i], o.lens[i]);
    }

    StringBuilder &operator=(const StringBuilder &o) {
        if (this != &o) {
            if (parts != parts_) {
                delete[] parts;
                delete[] lens;
            }
            parts = parts_;
            lens = lens_;
            count = 0;
            cap = INLINE_CAP;
            total_len = 0;
            cached = nullptr;
            arena_blocks.clear();
            arena_cur = nullptr;
            arena_end = nullptr;
            for (size_t i = 0; i < o.count; ++i)
                append_owned(o.parts[i], o.lens[i]);
        }
        return *this;
    }

    StringBuilder(StringBuilder &&o) noexcept
        : parts(o.parts == o.parts_ ? parts_ : o.parts)
        , lens(o.lens == o.lens_ ? lens_ : o.lens)
        , count(o.count)
        , cap(o.cap)
        , total_len(o.total_len)
        , cached(o.cached)
        , arena_blocks(std::move(o.arena_blocks))
        , arena_cur(o.arena_cur)
        , arena_end(o.arena_end) {
        if (o.parts == o.parts_) {
            for (size_t i = 0; i < count; ++i) {
                parts_[i] = o.parts_[i];
                lens_[i] = o.lens_[i];
            }
        }
        o.parts = o.parts_;
        o.lens = o.lens_;
        o.count = 0;
        o.cap = INLINE_CAP;
        o.total_len = 0;
        o.cached = nullptr;
        o.arena_cur = nullptr;
        o.arena_end = nullptr;
    }

    StringBuilder &operator=(StringBuilder &&o) noexcept {
        if (parts != parts_) {
            delete[] parts;
            delete[] lens;
        }
        parts = o.parts == o.parts_ ? parts_ : o.parts;
        lens = o.lens == o.lens_ ? lens_ : o.lens;
        count = o.count;
        cap = o.cap;
        total_len = o.total_len;
        cached = o.cached;
        arena_blocks = std::move(o.arena_blocks);
        arena_cur = o.arena_cur;
        arena_end = o.arena_end;
        if (o.parts == o.parts_) {
            for (size_t i = 0; i < count; ++i) {
                parts_[i] = o.parts_[i];
                lens_[i] = o.lens_[i];
            }
        }
        o.parts = o.parts_;
        o.lens = o.lens_;
        o.count = 0;
        o.cap = INLINE_CAP;
        o.total_len = 0;
        o.cached = nullptr;
        o.arena_cur = nullptr;
        o.arena_end = nullptr;
        return *this;
    }

    CLX_INLINE_HOT void clear() {
        cached = nullptr;
        count = 0;
        total_len = 0;
        if (!arena_blocks.empty()) {
            arena_cur = arena_blocks[0].mem.get();
            arena_end = arena_cur + arena_blocks[0].cap;
        } else {
            arena_cur = nullptr;
            arena_end = nullptr;
        }
    }

    void reserve(size_t n) {
        if (n <= cap)
            return;
        auto *np = new const char *[n];
        auto *nl = new uint32_t[n];
        for (size_t i = 0; i < count; ++i) {
            np[i] = parts[i];
            nl[i] = lens[i];
        }
        if (parts != parts_) {
            delete[] parts;
            delete[] lens;
        }
        parts = np;
        lens = nl;
        cap = n;
    }

    CLX_INLINE StringBuilder &append(const char *s, uint32_t len) {
        cached = nullptr;
        if (count >= cap)
            grow();
        parts[count] = s;
        lens[count] = len;
        count++;
        total_len += len;
        return *this;
    }

    CLX_INLINE StringBuilder &append_owned(const char *s, uint32_t len) {
        char *p = arena_alloc(len);
        clx_memcpy(p, s, len);
        return append(p, len);
    }

    CLX_INLINE StringBuilder &append(StringBuilder &other) {
        cached = nullptr;
        for (size_t i = 0; i < other.count; ++i)
            append(other.parts[i], other.lens[i]);
        return *this;
    }

    CLX_INLINE StringBuilder &append(LState *L, const LValue &v) {
        (void)L;
        if (v.type == ValueType::String) {
            if (v.val.payload.u64 >> 56)
                return append_owned(v.as_string(), v.string_len());
            return append(v.as_string(), v.string_len());
        }
        if (v.type == ValueType::Int64) {
            char buf[32];
            size_t n = static_cast<size_t>(
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.val.payload.i64)));
            return append_owned(buf, static_cast<uint32_t>(n));
        }
        if (v.type == ValueType::Double) {
            char buf[32];
            size_t n = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "%.14g", v.val.payload.f64));
            return append_owned(buf, static_cast<uint32_t>(n));
        }
        if (v.type == ValueType::Nil)
            return append("nil", 3);
        if (v.type == ValueType::Boolean)
            return append(v.as_bool() ? "true" : "false", v.as_bool() ? 4 : 5);
        return append("(unknown)", 9);
    }

    const char *to_string(LState *L) const {
        if (cached)
            return cached;
        if (count == 0) {
            cached = L->intern_string("", 0);
            return cached;
        }
        if (total_len <= 256) {
            char tmp[256];
            char *p = tmp;
            for (size_t i = 0; i < count; ++i) {
                clx_memcpy(p, parts[i], lens[i]);
                p += lens[i];
            }
            cached = L->intern_string(tmp, total_len);
            return cached;
        }
        char *mem = new char[total_len + 1];
        char *p = mem;
        for (size_t i = 0; i < count; ++i) {
            clx_memcpy(p, parts[i], lens[i]);
            p += lens[i];
        }
        *p = '\0';
        cached = L->intern_string(mem, total_len);
        delete[] mem;
        return cached;
    }

    CLX_INLINE_HOT size_t size() const { return total_len; }

    CLX_INLINE_HOT bool empty() const { return count == 0; }

    CLX_INLINE LValue to_lvalue(LState *L) const {
        if (count == 0)
            return LValue::istr("", 0);
        if (total_len <= 6) {
            char buf[8];
            char *p = buf;
            for (size_t i = 0; i < count; ++i) {
                clx_memcpy(p, parts[i], lens[i]);
                p += lens[i];
            }
            return LValue::istr(buf, static_cast<uint32_t>(total_len));
        }
        return LValue(to_string(L));
    }
};

//------------------ Runtime error exception
class LRuntimeException : public std::exception {
public:
    clx::LValue error_obj;
    mutable std::string cached_msg;

    LRuntimeException(clx::LValue err);
    virtual ~LRuntimeException() noexcept;
    virtual const char *what() const noexcept override;
};

//------------------ Interns an error message raised without an LState
const char *intern_error_message(const char *msg, size_t len);

//------------------ Throws an LRuntimeException
[[noreturn]] CLX_INLINE_COLD void throw_runtime_error(const char *msg) {
    throw LRuntimeException(LValue(intern_error_message(msg, clx_strlen(msg))));
}

[[noreturn]] CLX_INLINE_COLD void throw_runtime_error(const std::string &msg) {
    throw LRuntimeException(LValue(intern_error_message(msg.data(), msg.size())));
}

//------------------ Raises "attempt to index a nil value" (or the actual type name) like Lua does.
//------------------ Carries file:line like stock (missing-module indexing under --minimal hits this).
[[noreturn]] CLX_INLINE_COLD void throw_index_error(LState *L, const LValue &obj) {
    std::string msg = file_line_prefix(L) + std::string("attempt to index a ")
        + VALUE_TYPE_NAMES[static_cast<size_t>(obj.type)] + " value";
    throw LRuntimeException(LValue(intern_error_message(msg.data(), msg.size())));
}

//------------------ Per-function bump-pointer arena
#ifndef CLX_ARENA_DEFAULT_FIELDS
#define CLX_ARENA_DEFAULT_FIELDS 8
#endif

//------------------ Per-function bump arena for short-lived tables.
struct FuncArenaBlock {
    FuncArenaBlock *next;
    size_t cap;
};

struct FuncArena {
    FuncArenaBlock *blocks = nullptr;
    char *ptr = nullptr;
    char *end = nullptr;
    size_t block_size = 0;
    LState *owner = nullptr;
    FuncArena *next_active = nullptr;
    LTable *tables = nullptr;

    FuncArena() = default;
    FuncArena(const FuncArena &) = delete;
    FuncArena &operator=(const FuncArena &) = delete;
    ~FuncArena();
};

//------------------ Unlink an arena from the state's active-arena registry.
CLX_INLINE void arena_unregister(FuncArena *a) {
    LState *L = a->owner;
    if (!L)
        return;
    FuncArena **pp = &L->active_arenas;
    while (*pp && *pp != a)
        pp = &(*pp)->next_active;
    if (*pp == a)
        *pp = a->next_active;
    a->owner = nullptr;
    a->next_active = nullptr;
    a->tables = nullptr;
}

CLX_INLINE_HOT void arena_init(FuncArena *a, size_t size) {
    if (size < 64)
        size = 64;
    arena_unregister(a);
    a->blocks = nullptr;
    a->ptr = nullptr;
    a->end = nullptr;
    a->block_size = size;
}

CLX_INLINE_HOT void *arena_alloc(FuncArena *a, size_t bytes, size_t align = 8) {
    uintptr_t aligned = (reinterpret_cast<uintptr_t>(a->ptr) + align - 1) & ~(align - 1);
    if (aligned + bytes > reinterpret_cast<uintptr_t>(a->end)) {
        size_t need = bytes + align;
        size_t bcap = need > a->block_size ? need : a->block_size;
        FuncArenaBlock *b = static_cast<FuncArenaBlock *>(std::malloc(sizeof(FuncArenaBlock) + bcap));
        b->next = a->blocks;
        a->blocks = b;
        char *data = reinterpret_cast<char *>(b + 1);
        a->ptr = data;
        a->end = data + bcap;
        aligned = (reinterpret_cast<uintptr_t>(data) + align - 1) & ~(align - 1);
    }
    a->ptr = reinterpret_cast<char *>(aligned + bytes);
    return reinterpret_cast<char *>(aligned);
}

CLX_INLINE_HOT void arena_reset(FuncArena *a) {
    arena_unregister(a);
    FuncArenaBlock *b = a->blocks;
    while (b) {
        FuncArenaBlock *n = b->next;
        std::free(b);
        b = n;
    }
    a->blocks = nullptr;
    a->ptr = nullptr;
    a->end = nullptr;
}

//------------------ Runs on unwind too, so a Lua error unregisters the arena (no dangling entry).
CLX_INLINE FuncArena::~FuncArena() {
    arena_reset(this);
}

CLX_INLINE_HOT LValue arena_create_table(LState *L, FuncArena *a, size_t asize, size_t hsize) {
#if CLX_ARENA_DEFAULT_FIELDS > 0
    if (asize < CLX_ARENA_DEFAULT_FIELDS)
        asize = CLX_ARENA_DEFAULT_FIELDS;
    if (hsize < CLX_ARENA_DEFAULT_FIELDS)
        hsize = CLX_ARENA_DEFAULT_FIELDS;
#endif
    size_t header_sz = ((sizeof(LTable) + 7) & ~static_cast<size_t>(7));
    size_t array_sz = asize > 0 ? ((sizeof(TValue) * asize + 7) & ~static_cast<size_t>(7)) : 0;
    size_t types_sz = asize > 0 ? ((sizeof(ValueType) * asize + 7) & ~static_cast<size_t>(7)) : 0;
    size_t hash_sz = hsize > 0 ? ((sizeof(HashEntry) * hsize + 7) & ~static_cast<size_t>(7)) : 0;
    size_t total = header_sz + array_sz + types_sz + hash_sz;
    char *mem = static_cast<char *>(arena_alloc(a, total));
    LTable *t = new (mem) LTable();
    t->flags |= LFLAG_ARENA;
    //------------------ Per-arena table list (via LHeader::next) + registry for mark-phase clearing.
    t->next = a->tables;
    a->tables = t;
    if (!a->owner) {
        a->owner = L;
        a->next_active = L->active_arenas;
        L->active_arenas = a;
    }
    if (asize > 0) {
        t->array = reinterpret_cast<TValue *>(mem + header_sz);
        t->array_types = reinterpret_cast<ValueType *>(mem + header_sz + array_sz);
        t->array_cap = asize;
        for (size_t i = 0; i < asize; ++i) {
            t->array[i] = TValue();
            t->array_types[i] = Nil;
        }
    }
    if (hsize > 0) {
        LTableExt *ex = static_cast<LTableExt *>(arena_alloc(a, sizeof(LTableExt)));
        t->ext = ex;
        ex->entries = reinterpret_cast<HashEntry *>(mem + header_sz + array_sz + types_sz);
        ex->hash_size = hsize;
        ex->hash_count = 0;
        ex->hash_tombs = 0;
        ex->hash_bitmap = nullptr;
        ex->hash_version = 0;
        ex->ic = nullptr;
        ex->metatable = nullptr;
        ex->meta_next = nullptr;
        for (size_t i = 0; i < hsize; ++i) {
            ex->entries[i].key.payload.u64 = HASH_EMPTY;
            ex->entries[i].ktype = Nil;
            ex->entries[i].val = TValue();
            ex->entries[i].vtype = Nil;
        }
    }
    return LValue(Table, t);
}

//------------------ Close guard (for <close> vars)
struct CloseGuard {
    LState *L;
    LValue val;

    CLX_INLINE_HOT CloseGuard(LState *state, const LValue &v)
        : L(state)
        , val(v) { }

    ~CloseGuard();
};

//------------------ Reads a variable from environment table (direct LValue)
CLX_INLINE_HOT LValue get_env_var(LState *L, const LValue &env, const char *name) {
    LTable *t = static_cast<LTable *>(env.val.payload.ptr);
    LValue key = LValue(L->intern_string(name));
    return t->gettable(key);
}

//------------------ Writes a variable to environment table (direct LValue)
CLX_INLINE_HOT void set_env_var(LState *L, const LValue &env, const char *name, const LValue &val) {
    LTable *t = static_cast<LTable *>(env.val.payload.ptr);
    LValue key = LValue(L->intern_string(name));
    t->settable(key, val);
}

//------------------ Keyed environment access: same raw gettable/settable as get/set_env_var but with a
//------------------ pre-interned (codegen-pooled cstr_) key, so hot global reads/writes skip per-execution interning.
CLX_INLINE_HOT LValue get_env_var_key(LState *L, const LValue &env, const LValue &key) {
    LTable *t = static_cast<LTable *>(env.val.payload.ptr);
    (void)L;
    return t->gettable(key);
}

CLX_INLINE_HOT void set_env_var_key(LState *L, const LValue &env, const LValue &key, const LValue &val) {
    LTable *t = static_cast<LTable *>(env.val.payload.ptr);
    (void)L;
    t->settable(key, val);
}

//------------------ Int64 fast-path arithmetic. These mirror clx::add/sub/mul/mod using Lua 5.5 semantics.
CLX_INLINE_HOT int64_t int_add(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}

CLX_INLINE_HOT int64_t int_sub(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
}

CLX_INLINE_HOT int64_t int_mul(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}

//------------------ int_neg: unary minus over exact int64 (wraps, like Lua: -mininteger == mininteger).
CLX_INLINE_HOT int64_t int_neg(int64_t a) {
    return static_cast<int64_t>(0 - static_cast<uint64_t>(a));
}

//------------------ Lua floor modulo (%): result takes the sign of the divisor. b == 0 errors like stock luaV_mod.
CLX_INLINE_HOT int64_t int_floor_mod(LState *L, int64_t a, int64_t b) {
    if (b == 0)
        throw_runtime_error(file_line_prefix(L) + "attempt to perform 'n%0'");
    if (b == -1)
        return 0;
    int64_t r = a % b;
    if (r && (r < 0) != (b < 0))
        r += b;
    return r;
}

//------------------ Lua integer floor division (//): truncation toward negative infinity, integer result.
//------------------ b == 0 errors like stock luaV_idiv; b == -1 wraps (mininteger // -1 == mininteger).
CLX_INLINE_HOT int64_t int_floor_div(LState *L, int64_t a, int64_t b) {
    if (b == 0)
        throw_runtime_error(file_line_prefix(L) + "attempt to divide by zero");
    if (b == -1)
        return (a == INT64_MIN) ? INT64_MIN : -a;
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q -= 1;
    return q;
}

//------------------ Lua integer shift (stock luaV_shiftl): unsigned shifts, out-of-range counts give 0.
//------------------ shr computes shiftl_int(x, -y) with a wrapping negation (mininteger count -> 0).
CLX_INLINE_HOT int64_t shiftl_int(int64_t x, int64_t y) {
    if (y < 0) {
        if (y <= -64)
            return 0;
        return static_cast<int64_t>(static_cast<uint64_t>(x) >> static_cast<unsigned>(-y));
    }
    if (y >= 64)
        return 0;
    return static_cast<int64_t>(static_cast<uint64_t>(x) << static_cast<unsigned>(y));
}

CLX_INLINE_HOT int64_t shr_int(int64_t x, int64_t y) {
    return shiftl_int(x, static_cast<int64_t>(0 - static_cast<uint64_t>(y)));
}

//------------------ Lua floor modulo for the floating fast path (%): result takes the sign of the divisor.
CLX_INLINE_HOT double fmod_floor(double a, double b) {
    return a - std::floor(a / b) * b;
}

//------------------ LValue-returning int arithmetic: Lua integer semantics — wrap on overflow, stay Int64
CLX_INLINE_HOT LValue int_add_lv(int64_t a, int64_t b) {
    return LValue(static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b)));
}

CLX_INLINE_HOT LValue int_sub_lv(int64_t a, int64_t b) {
    return LValue(static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b)));
}

CLX_INLINE_HOT LValue int_mul_lv(int64_t a, int64_t b) {
    return LValue(static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b)));
}

//------------------ add_exact/sub_exact/mul_exact: exact Lua int arithmetic
CLX_INLINE_HOT LValue add_exact(bool lf, double da, int64_t sa, bool rf, double db, int64_t sb) {
    if (lf && rf)
        return int_add_lv(sa, sb);
    return LValue(da + db);
}

CLX_INLINE_HOT LValue sub_exact(bool lf, double da, int64_t sa, bool rf, double db, int64_t sb) {
    if (lf && rf)
        return int_sub_lv(sa, sb);
    return LValue(da - db);
}

CLX_INLINE_HOT LValue mul_exact(bool lf, double da, int64_t sa, bool rf, double db, int64_t sb) {
    if (lf && rf)
        return int_mul_lv(sa, sb);
    return LValue(da * db);
}

//------------------ box_int_flag: box a native local/param for calls/returns/concat
CLX_INLINE_HOT LValue box_int_flag(double d, int64_t shadow, bool is_int) {
    return is_int ? LValue(shadow) : LValue(d);
}

//------------------ num_box: box a raw double as an LValue, restoring the Int64 subtype if possible
CLX_INLINE_HOT LValue num_box(double v, bool is_int) {
    if (!is_int)
        return LValue(v);
    return (v >= -9223372036854775808.0 && v < 9223372036854775808.0
               && v == static_cast<double>(static_cast<int64_t>(v)))
        ? LValue(static_cast<int64_t>(v))
        : LValue(v);
}

//------------------ clx_format_num: number-to-string driven by the runtime int flag
CLX_INLINE_HOT int clx_format_num(char *buf, size_t cap, double v, bool is_int) {
    if (is_int && v > -9223372036854775808.0 && v < 9223372036854775808.0
        && v == static_cast<double>(static_cast<int64_t>(v)))
        return std::snprintf(buf, cap, "%lld", static_cast<long long>(static_cast<int64_t>(v)));
    return clx_format_double(buf, cap, v);
}

//------------------ to_integer_coerced: exact integer coercion (stock l_str2int). Int64 passes
//------------------ through; strings parse decimal/hex with wrap-on-hex-overflow; Doubles never coerce
CLX_INLINE bool to_integer_coerced(const LValue &v, int64_t &out) {
    if (v.type == ValueType::Int64) {
        out = v.val.payload.i64;
        return true;
    }
    if (v.type != ValueType::String)
        return false;
    const char *s = v.as_string();
    while (std::isspace(static_cast<unsigned char>(*s)))
        s++;
    bool neg = false;
    if (*s == '-') {
        neg = true;
        s++;
    } else if (*s == '+')
        s++;
    uint64_t a = 0;
    bool empty = true;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (; std::isxdigit(static_cast<unsigned char>(*s)); s++) {
            char c = static_cast<char>(*s | 0x20);
            int d = (c >= '0' && c <= '9') ? c - '0' : c - 'a' + 10;
            a = a * 16 + static_cast<uint64_t>(d);
            empty = false;
        }
    } else {
        const uint64_t max_by10 = static_cast<uint64_t>(INT64_MAX) / 10;
        const int max_lastd = static_cast<int>(INT64_MAX % 10);
        for (; std::isdigit(static_cast<unsigned char>(*s)); s++) {
            int d = *s - '0';
            if (a >= max_by10 && (a > max_by10 || d > max_lastd + (neg ? 1 : 0)))
                return false;
            a = a * 10 + static_cast<uint64_t>(d);
            empty = false;
        }
    }
    while (std::isspace(static_cast<unsigned char>(*s)))
        s++;
    if (empty || *s != '\0')
        return false;
    out = neg ? static_cast<int64_t>(0ull - a) : static_cast<int64_t>(a);
    return true;
}

//------------------ Addition with metamethod fallback
CLX_INLINE_HOT LValue add(LState *L, const LValue &a, const LValue &b) {
    if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]]
        return int_add_lv(a.val.payload.i64, b.val.payload.i64);
    if ((a.type == ValueType::Double || a.type == ValueType::Int64)
        && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(l + r);
    }
    int64_t li, ri;
    if (to_integer_coerced(a, li) && to_integer_coerced(b, ri))
        return int_add_lv(li, ri);
    double l, r;
    if (a.to_number(l) && b.to_number(r))
        return LValue(l + r);
    return call_bin_metamethod(L, a, b, L->str_add, "__add");
}

CLX_INLINE_HOT LValue sub(LState *L, const LValue &a, const LValue &b) {
    if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]]
        return int_sub_lv(a.val.payload.i64, b.val.payload.i64);
    if ((a.type == ValueType::Double || a.type == ValueType::Int64)
        && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(l - r);
    }
    int64_t li, ri;
    if (to_integer_coerced(a, li) && to_integer_coerced(b, ri))
        return int_sub_lv(li, ri);
    double l, r;
    if (a.to_number(l) && b.to_number(r))
        return LValue(l - r);
    return call_bin_metamethod(L, a, b, L->str_sub, "__sub");
}

CLX_INLINE_HOT LValue mul(LState *L, const LValue &a, const LValue &b) {
    if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]]
        return int_mul_lv(a.val.payload.i64, b.val.payload.i64);
    if ((a.type == ValueType::Double || a.type == ValueType::Int64)
        && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(l * r);
    }
    int64_t li, ri;
    if (to_integer_coerced(a, li) && to_integer_coerced(b, ri))
        return int_mul_lv(li, ri);
    double l, r;
    if (a.to_number(l) && b.to_number(r))
        return LValue(l * r);
    return call_bin_metamethod(L, a, b, L->str_mul, "__mul");
}

CLX_INLINE_HOT LValue div(LState *L, const LValue &a, const LValue &b) {
    if ((a.type == ValueType::Double || a.type == ValueType::Int64)
        && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(l / r);
    }
    double l, r;
    if (a.to_number(l) && b.to_number(r))
        return LValue(l / r);
    return call_bin_metamethod(L, a, b, L->str_div, "__div");
}

//------------------ coerce_number: checked conversion to double for native-impl entries and reads.
//------------------ Numbers pass through (Int64 exactly); strings parse like arithmetic coercion;
//------------------ anything else throws the same arithmetic error the generic path would raise.
//------------------ Unlike as_number() (which maps non-numbers to 0.0), this never silences a
//------------------ type error. Metamethods are not consulted, matching the native_direct bodies.
CLX_INLINE_HOT double coerce_number(LState *L, const LValue &v) {
    if (v.type == ValueType::Double) [[likely]]
        return v.val.payload.f64;
    if (v.type == ValueType::Int64) [[likely]]
        return static_cast<double>(v.val.payload.i64);
    double out;
    if (v.to_number(out))
        return out;
    size_t idx = (v.type == ValueType::Int64) ? static_cast<size_t>(ValueType::Double) : static_cast<size_t>(v.type);
    throw LRuntimeException(LValue(L->intern_string(
        file_line_prefix(L) + "attempt to perform arithmetic on a " + VALUE_TYPE_NAMES[idx] + " value")));
}

//------------------ Equality with metamethod fallback
CLX_INLINE_HOT LValue eq(LState *L, const LValue &a, const LValue &b) {
    if (a.type != ValueType::Double && a.type == b.type && a.val.payload.u64 == b.val.payload.u64)
        return LValue(true);
    if (a.type == ValueType::Int64 && b.type == ValueType::Double)
        return LValue(static_cast<double>(a.val.payload.i64) == b.val.payload.f64);
    if (a.type == ValueType::Double && b.type == ValueType::Int64)
        return LValue(a.val.payload.f64 == static_cast<double>(b.val.payload.i64));
    if (a.type == b.type) {
        if (a.type == ValueType::Int64)
            return LValue(a.val.payload.i64 == b.val.payload.i64);
        if (a.type == ValueType::Double)
            return LValue(a.val.payload.f64 == b.val.payload.f64);
        if (a.type == ValueType::String) {
            uint32_t al = a.string_len(), bl = b.string_len();
            return LValue(al == bl && clx_memcmp(a.as_string(), b.as_string(), al) == 0);
        }
        if (a.type == ValueType::Boolean)
            return LValue(a.val.payload.u64 == b.val.payload.u64);
        if (a.type == ValueType::Nil)
            return LValue(true);
        if (a.type == ValueType::Table || a.type == ValueType::UserData) {
            LTable *mt = (a.type == ValueType::Table) ? tbl_metatable(static_cast<LTable *>(a.as_pointer()))
                                                      : static_cast<LUserdata *>(a.as_pointer())->metatable;
            LTable *mt_b = (b.type == ValueType::Table) ? tbl_metatable(static_cast<LTable *>(b.as_pointer()))
                                                        : static_cast<LUserdata *>(b.as_pointer())->metatable;
            if (mt && mt == mt_b) {
                LValue mm = mt->gettable(L->str_eq);
                if (mm.type != ValueType::Nil)
                    return LValue(call_bin_metamethod(L, a, b, L->str_eq, "__eq").as_bool());
            }
        }
        return LValue(a.val.payload.ptr == b.val.payload.ptr);
    }
    return LValue(false);
}

//------------------ Integer fast equality: compares without boxing a constant operand
CLX_INLINE_HOT LValue eq(LState *L, const LValue &a, const Integer &b) {
    if (a.type == ValueType::Int64) [[likely]]
        return LValue(a.val.payload.i64 == b.v);
    return eq(L, a, LValue(b.v));
}

CLX_INLINE_HOT LValue eq(LState *L, const Integer &a, const LValue &b) {
    if (b.type == ValueType::Int64) [[likely]]
        return LValue(a.v == b.val.payload.i64);
    return eq(L, LValue(a.v), b);
}

CLX_INLINE_HOT LValue eq(LState *L, const Integer &a, const Integer &b) {
    return LValue(a.v == b.v);
}

//------------------ Float to integer conversion (integral + in int64 range)
CLX_INLINE bool flt_to_integer(double d, int64_t &out) {
    if (std::floor(d) == d && d >= -9.223372036854775808e18 && d < 9.223372036854775808e18) {
        out = static_cast<int64_t>(d);
        return true;
    }
    return false;
}

//------------------ Full-parse a numeric string (Lua tonumber/toint semantics: l_str2d).
CLX_INLINE bool parse_num_string(const char *s, double &out) {
    const char *pmode = std::strpbrk(s, ".xXnN");
    char mode = pmode ? static_cast<char>(std::tolower(static_cast<unsigned char>(*pmode))) : 0;
    if (mode == 'n')
        return false;
    if (mode == 'x') {
        const char *p = s;
        while (std::isspace(static_cast<unsigned char>(*p)))
            p++;
        if (*p == '+' || *p == '-')
            p++;
        if (!(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')))
            return false;
    }
    char *end;
    errno = 0;
    double d = std::strtod(s, &end);
    if (end == s)
        return false;
    while (std::isspace(static_cast<unsigned char>(*end)))
        end++;
    if (*end != '\0')
        return false;
    out = d;
    return true;
}

//------------------ Safe integer conversion (no metamethods)
CLX_INLINE bool to_integer(const LValue &v, int64_t &out) {
    if (v.type == ValueType::Int64) {
        out = v.val.payload.i64;
        return true;
    }
    if (v.type == ValueType::Double)
        return flt_to_integer(v.val.payload.f64, out);
    if (v.type == ValueType::String) {
        int64_t iv;
        if (to_integer_coerced(v, iv)) {
            out = iv;
            return true;
        }
        double d;
        if (parse_num_string(v.as_string(), d))
            return flt_to_integer(d, out);
    }
    return false;
}

//------------------ forerror: stock luaG_forerror message shape
CLX_INLINE_COLD void forerror(LState *L, const LValue &v, const char *what) {
    throw_runtime_error(file_line_prefix(L) + "bad 'for' " + what + " (number expected, got "
        + stock_type_name(v) + ")");
}

//------------------ forlimit_tointeger: stock luaV_tointeger with F2Ifloor/F2Iceil for 'for' limits
CLX_INLINE bool forlimit_tointeger(const LValue &v, int step_sign, int64_t &out) {
    if (v.type == ValueType::Int64) {
        out = v.val.payload.i64;
        return true;
    }
    double d;
    if (v.type == ValueType::Double) {
        d = v.val.payload.f64;
    } else if (v.type == ValueType::String) {
        int64_t iv;
        if (to_integer_coerced(v, iv)) {
            out = iv;
            return true;
        }
        if (!parse_num_string(v.as_string(), d))
            return false;
    } else {
        return false;
    }
    double f = (step_sign < 0) ? std::ceil(d) : std::floor(d);
    if (!(f >= -9223372036854775808.0) || !(f < 9223372036854775808.0))
        return false;
    out = static_cast<int64_t>(f);
    return true;
}

//------------------ forlimit: stock forlimit — integer limit with clipping; returns true to skip loop
CLX_INLINE bool forlimit(LState *L, int64_t init, const LValue &lim, int64_t step, int64_t &p) {
    if (forlimit_tointeger(lim, step < 0 ? -1 : 1, p))
        return (step > 0 ? init > p : init < p);
    double flim;
    if (!lim.to_number(flim))
        forerror(L, lim, "limit");
    if (flim > 0) {
        if (step < 0)
            return true;
        p = INT64_MAX;
    } else {
        if (step > 0)
            return true;
        p = INT64_MIN;
    }
    return (step > 0 ? init > p : init < p);
}

//------------------ to_integer_bitwise: stock trygetinteger — Int64 and exact doubles only (F2Ieq).
//------------------ Strings NEVER coerce for bitwise ops; inexact/out-of-range doubles fail (the
//------------------ both-numbers miss in call_bin_metamethod reports "number has no integer representation").
CLX_INLINE bool to_integer_bitwise(const LValue &v, int64_t &out) {
    if (v.type == ValueType::Int64) {
        out = v.val.payload.i64;
        return true;
    }
    if (v.type == ValueType::Double)
        return flt_to_integer(v.val.payload.f64, out);
    return false;
}

//------------------ Bitwise AND with metamethod fallback (stock error wording via call_bin_metamethod)
CLX_INLINE LValue band(LState *L, const LValue &a, const LValue &b) {
    int64_t l, r;
    if (to_integer_bitwise(a, l) && to_integer_bitwise(b, r)) [[likely]]
        return LValue(l & r);
    return call_bin_metamethod(L, a, b, L->str_band, "__band");
}

//------------------ Bitwise OR with metamethod fallback
CLX_INLINE LValue bor(LState *L, const LValue &a, const LValue &b) {
    int64_t l, r;
    if (to_integer_bitwise(a, l) && to_integer_bitwise(b, r)) [[likely]]
        return LValue(l | r);
    return call_bin_metamethod(L, a, b, L->str_bor, "__bor");
}

//------------------ Bitwise XOR with metamethod fallback
CLX_INLINE LValue bxor(LState *L, const LValue &a, const LValue &b) {
    int64_t l, r;
    if (to_integer_bitwise(a, l) && to_integer_bitwise(b, r)) [[likely]]
        return LValue(l ^ r);
    return call_bin_metamethod(L, a, b, L->str_bxor, "__bxor");
}

//------------------ Bitwise SHL with metamethod fallback (stock luaV_shiftl semantics)
CLX_INLINE LValue shl(LState *L, const LValue &a, const LValue &b) {
    int64_t l, r;
    if (to_integer_bitwise(a, l) && to_integer_bitwise(b, r)) [[likely]]
        return LValue(shiftl_int(l, r));
    return call_bin_metamethod(L, a, b, L->str_shl, "__shl");
}

//------------------ Bitwise SHR with metamethod fallback (shiftl_int(x, -y))
CLX_INLINE LValue shr(LState *L, const LValue &a, const LValue &b) {
    int64_t l, r;
    if (to_integer_bitwise(a, l) && to_integer_bitwise(b, r)) [[likely]]
        return LValue(shr_int(l, r));
    return call_bin_metamethod(L, a, b, L->str_shr, "__shr");
}

//------------------ Bitwise NOT with metamethod fallback
CLX_INLINE LValue bnot(LState *L, const LValue &a) {
    int64_t v;
    if (to_integer_bitwise(a, v)) [[likely]]
        return LValue(~v);
    return call_bin_metamethod(L, a, a, L->str_bnot, "__bnot");
}

CLX_INLINE LValue lt(LState *L, const LValue &a, const LValue &b) {
    if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]]
        return LValue(a.val.payload.i64 < b.val.payload.i64);
    if ((a.type == ValueType::Double || a.type == ValueType::Int64)
        && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(l < r);
    }
    if (a.type == ValueType::String && b.type == ValueType::String)
        return a < b;
    return LValue(call_bin_metamethod(L, a, b, L->str_lt, "__lt").as_bool());
}

CLX_INLINE LValue le(LState *L, const LValue &a, const LValue &b) {
    if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]]
        return LValue(a.val.payload.i64 <= b.val.payload.i64);
    if ((a.type == ValueType::Double || a.type == ValueType::Int64)
        && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(l <= r);
    }
    if (a.type == ValueType::String && b.type == ValueType::String)
        return a <= b;
    return LValue(call_bin_metamethod(L, a, b, L->str_le, "__le").as_bool());
}

//------------------ Integer fast ordering: compares without boxing a constant operand
CLX_INLINE LValue lt(LState *L, const LValue &a, const Integer &b) {
    if (a.type == ValueType::Int64) [[likely]]
        return LValue(a.val.payload.i64 < b.v);
    return lt(L, a, LValue(b.v));
}

CLX_INLINE LValue lt(LState *L, const Integer &a, const LValue &b) {
    if (b.type == ValueType::Int64) [[likely]]
        return LValue(a.v < b.val.payload.i64);
    return lt(L, LValue(a.v), b);
}

CLX_INLINE LValue lt(LState *L, const Integer &a, const Integer &b) {
    return LValue(a.v < b.v);
}

CLX_INLINE LValue le(LState *L, const LValue &a, const Integer &b) {
    if (a.type == ValueType::Int64) [[likely]]
        return LValue(a.val.payload.i64 <= b.v);
    return le(L, a, LValue(b.v));
}

CLX_INLINE LValue le(LState *L, const Integer &a, const LValue &b) {
    if (b.type == ValueType::Int64) [[likely]]
        return LValue(a.v <= b.val.payload.i64);
    return le(L, LValue(a.v), b);
}

CLX_INLINE LValue le(LState *L, const Integer &a, const Integer &b) {
    return LValue(a.v <= b.v);
}

//------------------ Length operator with metamethod fallback
CLX_INLINE_COLD LValue len(LState *L, const LValue &a) {
    if (a.type == ValueType::String) {
        return LValue(static_cast<int64_t>(a.string_len()));
    }

    if (a.type != ValueType::Table)
        return call_bin_metamethod(L, a, a, L->str_len, "__len");
    LTable *t = static_cast<LTable *>(a.as_pointer());

    if (LTable *mt = tbl_metatable(t)) {
        LValue mm = mt->gettable(L->str_len);
        if (mm.type != ValueType::Nil)
            return call_bin_metamethod(L, a, a, L->str_len, "__len");
    }

    if (t->array_size > 0) {
        size_t n = table_array_prefix_len(t);
        size_t hc = (t->ext != nullptr) ? t->ext->hash_count : 0;
        if (n == t->array_size) {
            if (hc == 0)
                return LValue(static_cast<int64_t>(t->array_size));
            LValue next = t->gettable(LValue(static_cast<int64_t>(t->array_size + 1)));
            if (next.type == ValueType::Nil)
                return LValue(static_cast<int64_t>(t->array_size));
        } else if (hc == 0) {
            //------------------ hole inside a hash-free array part: the prefix scan found the border
            return LValue(static_cast<int64_t>(n));
        }
        //------------------ holes with integer keys above the array part: fall through to the probe below
    }

    int64_t lo = 0;
    int64_t hi = static_cast<int64_t>(t->array_size) + 1;
    while (true) {
        LValue p = t->gettable(LValue(static_cast<int64_t>(hi)));
        if (p.type == ValueType::Nil)
            break;
        lo = hi;
        hi = (hi < 8) ? 8 : hi * 2;
    }
    while (hi - lo > 1) {
        int64_t mid = lo + (hi - lo) / 2;
        LValue p = t->gettable(LValue(static_cast<int64_t>(mid)));
        if (p.type != ValueType::Nil)
            lo = mid;
        else
            hi = mid;
    }
    return LValue(lo);
}

//------------------ Modulo with metamethod fallback (stock errors for integer b == 0)
CLX_INLINE LValue mod(LState *L, const LValue &a, const LValue &b) {
    if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]]
        return LValue(int_floor_mod(L, a.val.payload.i64, b.val.payload.i64));
    if ((a.type == ValueType::Double || a.type == ValueType::Int64)
        && (b.type == ValueType::Double || b.type == ValueType::Int64)) {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(l - std::floor(l / r) * r);
    }
    int64_t li, ri;
    if (to_integer_coerced(a, li) && to_integer_coerced(b, ri))
        return LValue(int_floor_mod(L, li, ri));
    double l, r;
    if (a.to_number(l) && b.to_number(r))
        return LValue(l - std::floor(l / r) * r);
    return call_bin_metamethod(L, a, b, L->str_mod, "__mod");
}

//------------------ Floor-division with metamethod fallback (stock errors for integer b == 0)
CLX_INLINE LValue idiv(LState *L, const LValue &a, const LValue &b) {
    if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]]
        return LValue(int_floor_div(L, a.val.payload.i64, b.val.payload.i64));
    if ((a.type == ValueType::Double || a.type == ValueType::Int64)
        && (b.type == ValueType::Double || b.type == ValueType::Int64)) {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(std::floor(l / r));
    }
    int64_t li, ri;
    if (to_integer_coerced(a, li) && to_integer_coerced(b, ri))
        return LValue(int_floor_div(L, li, ri));
    double l, r;
    if (a.to_number(l) && b.to_number(r))
        return LValue(std::floor(l / r));
    return call_bin_metamethod(L, a, b, L->str_idiv, "__idiv");
}

//------------------ Power with metamethod fallback
CLX_INLINE LValue pow(LState *L, const LValue &a, const LValue &b) {
    if ((a.type == ValueType::Double || a.type == ValueType::Int64)
        && (b.type == ValueType::Double || b.type == ValueType::Int64)) {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(std::pow(l, r));
    }
    double l, r;
    if (a.to_number(l) && b.to_number(r))
        return LValue(std::pow(l, r));
    return call_bin_metamethod(L, a, b, L->str_pow, "__pow");
}

//------------------ Unary minus with metamethod fallback
CLX_INLINE LValue unm(LState *L, const LValue &a) {
    if (a.type == ValueType::Int64) [[likely]] {
        int64_t v = a.val.payload.i64;
        if (v == INT64_MIN)
            return LValue(v);
        return LValue(-v);
    }
    if (a.type == ValueType::Double)
        return LValue(-a.val.payload.f64);
    int64_t iv;
    if (to_integer_coerced(a, iv)) {
        if (iv == INT64_MIN)
            return LValue(iv);
        return LValue(-iv);
    }
    double d;
    if (a.to_number(d))
        return LValue(-d);
    return call_bin_metamethod(L, a, a, L->str_unm, "__unm");
}

//------------------ Logical NOT
CLX_INLINE LValue logical_not(const LValue &a) {
    return LValue(!a.as_bool());
}

//------------------ make_string_pooled: forward declaration for concat_multi below
CLX_INLINE LValue make_string_pooled(LState *L, const char *s, size_t len);

//------------------ Multi-value concat (string builder fast path)
CLX_INLINE_COLD LValue concat_multi(LState *L, const LValue *args, size_t count) {
    if (count <= 8) [[likely]] {
        const char *ptrs[8];
        size_t lens[8];
        char nbuf[8][32];
        size_t total_len = 0;
        for (size_t i = 0; i < count; ++i) {
            if (args[i].type != ValueType::String && args[i].type != ValueType::Int64
                && args[i].type != ValueType::Double) {
                return call_bin_metamethod(L, args[i], args[i == count - 1 ? i - 1 : i + 1], L->str_concat, "__concat");
            }
            if (args[i].type == ValueType::String) {
                ptrs[i] = args[i].as_string();
                lens[i] = args[i].string_len();
            } else if (args[i].type == ValueType::Int64) {
                lens[i] = static_cast<size_t>(
                    std::snprintf(nbuf[i], sizeof(nbuf[i]), "%lld", static_cast<long long>(args[i].val.payload.i64)));
                ptrs[i] = nbuf[i];
            } else {
                lens[i] = static_cast<size_t>(clx_format_double(nbuf[i], sizeof(nbuf[i]), args[i].val.payload.f64));
                ptrs[i] = nbuf[i];
            }
            total_len += lens[i];
        }
        if (total_len <= 6) {
            char buf[8];
            char *bp = buf;
            for (size_t i = 0; i < count; ++i) {
                clx_memcpy(bp, ptrs[i], lens[i]);
                bp += lens[i];
            }
            return LValue::istr(buf, static_cast<uint32_t>(total_len));
        }
        if (total_len <= 256) {
            char tmp[256];
            char *p = tmp;
            for (size_t i = 0; i < count; ++i) {
                clx_memcpy(p, ptrs[i], lens[i]);
                p += lens[i];
            }
            return make_string_pooled(L, tmp, total_len);
        }
        char *mem = new char[total_len + 1];
        char *p = mem;
        for (size_t i = 0; i < count; ++i) {
            clx_memcpy(p, ptrs[i], lens[i]);
            p += lens[i];
        }
        LValue r = make_string_pooled(L, mem, total_len);
        delete[] mem;
        return r;
    }

    size_t total_len = 0;
    std::vector<std::string> parts;
    parts.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (args[i].type != ValueType::String && args[i].type != ValueType::Int64
            && args[i].type != ValueType::Double) {
            return call_bin_metamethod(L, args[i], args[i == count - 1 ? i - 1 : i + 1], "__concat");
        }
        parts.push_back(args[i].to_string(L));
        total_len += parts.back().length();
    }
    std::string res;
    res.reserve(total_len);
    for (const auto &p : parts)
        res += p;
    return LValue(L->intern_string(res));
}

//------------------ Creates a string LValue with inline encoding for short strings
CLX_INLINE LValue make_string(LState *L, const char *s, size_t len) {
    if (len <= 6)
        return LValue::istr(s, len);
    return LValue(L->intern_string(s, len));
}

CLX_INLINE LValue make_string(LState *L, const char *s) {
    size_t len = clx_strlen(s);
    if (len <= 6)
        return LValue::istr(s, len);
    return LValue(L->intern_string(s, len));
}

CLX_INLINE_HOT LValue make_string(LState *L, const std::string &s) {
    return make_string(L, s.data(), s.size());
}

//------------------ Creates a string LValue, interning in a single hash/probe pass
CLX_INLINE LValue make_string_pooled(LState *L, const char *s, size_t len) {
    if (len <= 6)
        return LValue::istr(s, len);
    uint64_t h = len <= 8 ? swar_hash_8(s, len) : wyhash_str(s, len);
    return LValue(L->string_pool.intern(s, len, h));
}

//------------------ Table read with __index fallback

//--------- Slow Path (hash table lookup and metamethods)
LValue table_get_slow(LState *L, const LValue &obj, const LValue &key);

CLX_INLINE_HOT LValue table_get(LState *L, const LValue &obj, const LValue &key) {
    if (obj.type == ValueType::Table) {
        LTable *t = static_cast<LTable *>(obj.as_pointer());

        if (key.type == ValueType::Int64) {
            int64_t idx = key.val.payload.i64;
            if (static_cast<uint64_t>(idx - 1) < t->array_size) {
                LValue direct(t->array[idx - 1], t->array_types[idx - 1]);
                if (direct.type != ValueType::Nil)
                    return direct;
            }
        } else if (key.type == ValueType::Double) {
            double d = key.val.payload.f64;
            int64_t idx = static_cast<int64_t>(d);
            if (d == static_cast<double>(idx) && static_cast<uint64_t>(idx - 1) < t->array_size) {
                LValue direct(t->array[idx - 1], t->array_types[idx - 1]);
                if (direct.type != ValueType::Nil)
                    return direct;
            }
        }

        LTableExt *ex = t->ext;
        if (ex && ex->ic) {
            uint32_t ic_idx
                = static_cast<uint32_t>(key.val.payload.u64 ^ (key.val.payload.u64 >> 17) ^ (key.val.payload.u64 >> 33)
                      ^ (key.val.payload.u64 >> 5) ^ (key.val.payload.u64 >> 11))
                % LTABLE_IC_SIZE;
            LTableInlineCache &_ic = ex->ic[ic_idx];
            if (_ic.key_payload == key.val.payload.u64 && _ic.table_ver == ex->hash_version
                && _ic.entry_idx < ex->hash_size) {
                HashEntry &_e = ex->entries[_ic.entry_idx];
                if (_e.ktype != ValueType::Nil)
                    return LValue(_e.val, _e.vtype);
            }
        }
    }
    return table_get_slow(L, obj, key);
}

//------------------ Currently-running state, for write barriers in hot paths that

extern thread_local LState *clx_current_L;

//------------------ Fill array gaps with nil so recycled buffers never expose stale slots or GC references.
CLX_INLINE void table_fill_gap(LTable *t, size_t from, size_t to) {
    for (size_t j = from; j < to; ++j) {
        t->array[j] = TValue();
        t->array_types[j] = Nil;
    }
}

//------------------ Generational write barrier for stores into an LTable without an

CLX_INLINE_HOT void gc_barrier_table(LTable *owner, const LValue &newval) {
    if (owner->age == AGE_YOUNG || !newval.is_gc_obj())
        return;
    LHeader *nv = static_cast<LHeader *>(newval.as_pointer());
    if (!nv || nv->age == AGE_OLD)
        return;
    if (clx_current_L)
        clx_current_L->gc_remember(static_cast<LHeader *>(owner));
}

//------------------ Upvalue cell write barrier for LState-less hot paths.

CLX_INLINE_HOT void gc_barrier_cell(const LUpValue &cell, const LValue &newval) {
    if (!clx_current_L || !newval.is_gc_obj() || !cell)
        return;
    LHeader *nv = static_cast<LHeader *>(newval.as_pointer());
    if (nv && nv->age != AGE_OLD)
        clx_current_L->gc_remember_cell(cell);
}

//------------------ Store into an upvalue cell + generational write barrier.

CLX_INLINE_HOT void upval_store(const LUpValue &cell, const LValue &v) {
    *cell = v;
    gc_barrier_cell(cell, v);
}

//------------------ Table write with __newindex fallback
CLX_INLINE void table_set(LState *L, const LValue &obj, const LValue &key, const LValue &val) {
    LTable *mt = nullptr;

    if (obj.type == ValueType::Table) {
        LTable *t = static_cast<LTable *>(obj.as_pointer());
        if (key.type == ValueType::Int64) {
            int64_t k = key.val.payload.i64;
            if (static_cast<uint64_t>(k - 1) < t->array_cap) {
                gc_barrier_table(L, t, val);
                if (static_cast<uint64_t>(k - 1) > t->array_size)
                    table_fill_gap(t, t->array_size, static_cast<size_t>(k) - 1);
                t->array[k - 1] = val.val;
                t->array_types[k - 1] = val.type;
                if (static_cast<size_t>(k) > t->array_size)
                    t->array_size = static_cast<size_t>(k);
                table_hint_on_array_write(t, static_cast<size_t>(k) - 1, val.type);
                return;
            }
        } else if (key.type == ValueType::Double) {
            double d = key.val.payload.f64;
            int64_t idx = static_cast<int64_t>(d);
            if (d == static_cast<double>(idx) && static_cast<uint64_t>(idx - 1) < t->array_cap) {
                gc_barrier_table(L, t, val);
                if (static_cast<uint64_t>(idx - 1) > t->array_size)
                    table_fill_gap(t, t->array_size, static_cast<size_t>(idx) - 1);
                t->array[idx - 1] = val.val;
                t->array_types[idx - 1] = val.type;
                if (static_cast<size_t>(idx) > t->array_size)
                    t->array_size = static_cast<size_t>(idx);
                table_hint_on_array_write(t, static_cast<size_t>(idx) - 1, val.type);
                return;
            }
        }
        gc_barrier_table(L, t, val);
        if (key.is_gc_obj())
            gc_barrier_table(L, t, key);
        t->set_value(L, key, val);
        return;
    } else if (obj.type == ValueType::UserData) {
        LUserdata *ud = static_cast<LUserdata *>(obj.as_pointer());
        mt = ud->metatable;
        if (!mt)
            return;
        LValue newindex = mt->gettable(L->str_newindex);
        if (newindex.type == ValueType::Nil)
            return;
        if (newindex.type == ValueType::Function) {
            static constexpr CallSite newindex_site = { "__newindex", 0, false, false };
            LValue args[3] = { obj, key, val };
            call_function_rooted(L, newindex, args, 3, &newindex_site);
        } else if (newindex.type == ValueType::Table) {
            table_set(L, newindex, key, val);
        }
        return;
    } else {
        throw_index_error(L, obj);
    }
}

//------------------ Integer-key table read (fast path)
CLX_INLINE LValue table_get_int(LState *L, const LValue &obj, size_t idx) {
    LTable *mt = nullptr;
    LValue key_val = LValue(static_cast<int64_t>(idx));

    if (obj.type == ValueType::Table) {
        LTable *t = static_cast<LTable *>(obj.as_pointer());
        if (idx - 1 < t->array_size)
            return LValue(t->array[idx - 1], t->array_types[idx - 1]);
        LValue result = t->get_value(L, key_val);
        if (result.type != ValueType::Nil)
            return result;
        mt = tbl_metatable(t);
    } else if (obj.type == ValueType::UserData) {
        LUserdata *ud = static_cast<LUserdata *>(obj.as_pointer());
        mt = ud->metatable;
    } else {
        throw_index_error(L, obj);
    }

    if (!mt)
        return LValue();
    LValue index = mt->gettable(L->str_index);
    if (index.type == ValueType::Nil)
        return LValue();
    if (index.type == ValueType::Function) {
        LValue args[2] = { obj, key_val };
        static constexpr CallSite index_site = { "__index", 0, false, false };
        MultiValue mv = call_function_rooted(L, index, args, 2, &index_site);
        return mv.count > 0 ? mv[0] : LValue();
    }
    if (index.type == ValueType::Table)
        return table_get(L, index, key_val);
    return LValue();
}

//------------------ Integer-key table write (fast path)
CLX_INLINE void table_set_int(LState *L, const LValue &obj, size_t idx, const LValue &val) {
    LValue key_val = LValue(static_cast<int64_t>(idx));

    if (obj.type == ValueType::Table) {
        LTable *t = static_cast<LTable *>(obj.as_pointer());
        if (idx - 1 < t->array_cap) {
            gc_barrier_table(L, t, val);
            if (idx - 1 > t->array_size)
                table_fill_gap(t, t->array_size, idx - 1);
            t->array[idx - 1] = val.val;
            t->array_types[idx - 1] = val.type;
            if (static_cast<size_t>(idx) > t->array_size)
                t->array_size = idx;
            table_hint_on_array_write(t, idx - 1, val.type);
            return;
        }
        gc_barrier_table(L, t, val);
        t->set_value(L, key_val, val);
        return;
    }

    if (obj.type == ValueType::UserData) {
        LUserdata *ud = static_cast<LUserdata *>(obj.as_pointer());
        if (!ud->metatable)
            return;
        LValue newindex = ud->metatable->gettable(L->str_newindex);
        if (newindex.type == ValueType::Nil)
            return;
        if (newindex.type == ValueType::Function) {
            static constexpr CallSite newindex_site = { "__newindex", 0, false, false };
            LValue args[3] = { obj, key_val, val };
            call_function_rooted(L, newindex, args, 3, &newindex_site);
        } else if (newindex.type == ValueType::Table) {
            table_set(L, newindex, key_val, val);
        }
    }
}

//------------------ Direct table write (skips existence check / metatable)
CLX_INLINE_HOT void table_set_direct(LState *L, const LValue &obj, const LValue &key, const LValue &val) {
    if (obj.type == ValueType::Table) {
        LTable *t = static_cast<LTable *>(obj.as_pointer());
        LTableExt *ex = t->ext;
        if (val.type != ValueType::Nil && ex && ex->hash_size > 0 && ex->ic) {
            uint32_t ic_idx
                = static_cast<uint32_t>(key.val.payload.u64 ^ (key.val.payload.u64 >> 17) ^ (key.val.payload.u64 >> 33)
                      ^ (key.val.payload.u64 >> 5) ^ (key.val.payload.u64 >> 11))
                % LTABLE_IC_SIZE;
            LTableInlineCache &_ic = ex->ic[ic_idx];
            if (_ic.key_payload == key.val.payload.u64 && _ic.table_ver == ex->hash_version
                && _ic.entry_idx < ex->hash_size) {
                HashEntry &_e = ex->entries[_ic.entry_idx];
                if (_e.ktype != ValueType::Nil) {
                    gc_barrier_table(L, t, val);
                    _e.val = val.val;
                    _e.vtype = val.type;
                    return;
                }
            }
        }
        gc_barrier_table(L, t, val);
        t->settable(key, val);
        return;
    }
    table_set(L, obj, key, val);
}

//------------------ table_op_amount: fused-op constant as LValue with int fidelity for integral doubles.
CLX_INLINE_HOT LValue table_op_amount(double amount) {
    if (amount >= -9007199254740992.0 && amount <= 9007199254740992.0
        && static_cast<double>(static_cast<int64_t>(amount)) == amount)
        return LValue(static_cast<int64_t>(amount));
    return LValue(amount);
}

//------------------ table_op_apply: fused-op arithmetic identical to clx add/sub/mul/div
CLX_INLINE_HOT LValue table_op_apply(LState *L, const LValue &a, const LValue &b, int op_kind) {
    if (op_kind == 0)
        return add(L, a, b);
    if (op_kind == 1)
        return sub(L, a, b);
    if (op_kind == 2)
        return mul(L, a, b);
    return div(L, a, b);
}

//------------------ Direct table arithmetic: t[k] = t[k] op amount
CLX_INLINE_HOT void table_op(LState *L, const LValue &obj, const LValue &key, double amount, int op_kind, bool nil_ok) {
    if (obj.type == ValueType::Table) {
        static_cast<LTable *>(obj.val.payload.ptr)->op_direct(L, obj, key, amount, op_kind, nil_ok);
        return;
    }
    LValue amt = table_op_amount(amount);
    table_set(L, obj, key, table_op_apply(L, table_get(L, obj, key), amt, op_kind));
}

CLX_INLINE_HOT void table_increment(
    LState *L, const LValue &obj, const LValue &key, double amount, bool nil_ok = false) {
    table_op(L, obj, key, amount, 0, nil_ok);
}

CLX_INLINE_HOT void table_decrement(
    LState *L, const LValue &obj, const LValue &key, double amount, bool nil_ok = false) {
    table_op(L, obj, key, amount, 1, nil_ok);
}

CLX_INLINE_HOT void table_multiply(
    LState *L, const LValue &obj, const LValue &key, double amount, bool nil_ok = false) {
    table_op(L, obj, key, amount, 2, nil_ok);
}

CLX_INLINE_HOT void table_divide(LState *L, const LValue &obj, const LValue &key, double amount, bool nil_ok = false) {
    table_op(L, obj, key, amount, 3, nil_ok);
}

MultiValue str_len(LState *, const LValue *, size_t);
MultiValue str_sub(LState *, const LValue *, size_t);
MultiValue str_reverse(LState *, const LValue *, size_t);
MultiValue str_lower(LState *, const LValue *, size_t);
MultiValue str_upper(LState *, const LValue *, size_t);
MultiValue str_rep(LState *, const LValue *, size_t);
MultiValue str_byte(LState *, const LValue *, size_t);
MultiValue str_char(LState *, const LValue *, size_t);
MultiValue str_dump(LState *, const LValue *, size_t);
MultiValue str_format(LState *, const LValue *, size_t);
MultiValue str_find(LState *, const LValue *, size_t);
MultiValue str_match(LState *, const LValue *, size_t);
MultiValue str_gmatch(LState *, const LValue *, size_t);
MultiValue str_gsub(LState *, const LValue *, size_t);
MultiValue str_pack(LState *, const LValue *, size_t);
MultiValue str_packsize(LState *, const LValue *, size_t);
MultiValue str_unpack(LState *, const LValue *, size_t);

//------------------ package module (package.cpp); package_searchpath resolves module names against path templates
std::string package_searchpath(LState *L, const char *name, size_t name_len, const char *path, size_t path_len,
    char sep, char rep, std::string *tried);

MultiValue table_concat(LState *, const LValue *, size_t);
MultiValue table_insert(LState *, const LValue *, size_t);
MultiValue table_remove(LState *, const LValue *, size_t);
MultiValue table_sort(LState *, const LValue *, size_t);
MultiValue table_pack(LState *, const LValue *, size_t);
MultiValue table_unpack(LState *, const LValue *, size_t);
MultiValue table_move(LState *, const LValue *, size_t);
MultiValue table_create(LState *, const LValue *, size_t);

//------------------ math module (math.cpp); extern for direct-call lowering via the builtin registry
MultiValue math_abs(LState *, const LValue *, size_t);
MultiValue math_acos(LState *, const LValue *, size_t);
MultiValue math_asin(LState *, const LValue *, size_t);
MultiValue math_atan(LState *, const LValue *, size_t);
MultiValue math_ceil(LState *, const LValue *, size_t);
MultiValue math_cos(LState *, const LValue *, size_t);
MultiValue math_deg(LState *, const LValue *, size_t);
MultiValue math_exp(LState *, const LValue *, size_t);
MultiValue math_floor(LState *, const LValue *, size_t);
MultiValue math_fmod(LState *, const LValue *, size_t);
MultiValue math_frexp(LState *, const LValue *, size_t);
MultiValue math_ldexp(LState *, const LValue *, size_t);
MultiValue math_log(LState *, const LValue *, size_t);
MultiValue math_max(LState *, const LValue *, size_t);
MultiValue math_min(LState *, const LValue *, size_t);
MultiValue math_modf(LState *, const LValue *, size_t);
MultiValue math_rad(LState *, const LValue *, size_t);
MultiValue math_random(LState *, const LValue *, size_t);
MultiValue math_randomseed(LState *, const LValue *, size_t);
MultiValue math_sin(LState *, const LValue *, size_t);
MultiValue math_sqrt(LState *, const LValue *, size_t);
MultiValue math_tan(LState *, const LValue *, size_t);
MultiValue math_tointeger(LState *, const LValue *, size_t);
MultiValue math_type(LState *, const LValue *, size_t);
MultiValue math_ult(LState *, const LValue *, size_t);

//------------------ os module (os.cpp); extern for direct-call lowering via the builtin registry
MultiValue os_clock(LState *, const LValue *, size_t);
MultiValue os_date(LState *, const LValue *, size_t);
MultiValue os_difftime(LState *, const LValue *, size_t);
MultiValue os_execute(LState *, const LValue *, size_t);
MultiValue os_exit(LState *, const LValue *, size_t);
MultiValue os_getenv(LState *, const LValue *, size_t);
MultiValue os_remove(LState *, const LValue *, size_t);
MultiValue os_rename(LState *, const LValue *, size_t);
MultiValue os_setlocale(LState *, const LValue *, size_t);
MultiValue os_time(LState *, const LValue *, size_t);
MultiValue os_tmpname(LState *, const LValue *, size_t);

void luastd_base(LState *L);
//------------------ Per-function base registrars (base.cpp): each registers exactly one global so the
//------------------ linker strips unused base functions. Declared here for driver-emitted open sequences.
void luastd_base_print(LState *L);
void luastd_base_require(LState *L);
void luastd_base_error(LState *L);
void luastd_base_assert(LState *L);
void luastd_base_tostring(LState *L);
void luastd_base_tonumber(LState *L);
void luastd_base_rawequal(LState *L);
void luastd_base_rawget(LState *L);
void luastd_base_rawset(LState *L);
void luastd_base_rawlen(LState *L);
void luastd_base_setmetatable(LState *L);
void luastd_base_getmetatable(LState *L);
void luastd_base_collectgarbage(LState *L);
void luastd_base_type(LState *L);
void luastd_base_pcall(LState *L);
void luastd_base_xpcall(LState *L);
void luastd_base_warn(LState *L);
void luastd_base_next(LState *L);
void luastd_base_pairs(LState *L);
void luastd_base_ipairs(LState *L);
void luastd_base_select(LState *L);
void luastd_package(LState *L);
void luastd_math(LState *L);
void luastd_coroutine(LState *L);
void luastd_string(LState *L);
void luastd_table(LState *L);
void luastd_os(LState *L);
void luastd_utf8(LState *L);
void luastd_io(LState *L);

//------------------ Gets a global variable
LValue get_global(LState *L, const char *name);
//------------------ Sets a global variable
void set_global(LState *L, const char *name, const LValue &val);

//------------------ Creates a new coroutine thread
LValue create_thread(LState *L, const LValue &func, double stack_size = 262144.0);
//------------------ Allocates a new userdata
LValue newuserdata(LState *L, size_t size);
//------------------ Resumes a coroutine
MultiValue resume(LState *L, const LValue &thread, const LValue *args, size_t count);
//------------------ Yields from a coroutine
MultiValue yield(LState *L, const LValue *args, size_t count);
//------------------ Closes a coroutine
MultiValue close_thread(LState *L, const LValue &thread);

//------------------ Opens CLX state
LState *open(int argc = 0, char *argv[] = nullptr);
//------------------ Opens CLX state without registering any std library (driver-emitted filtered open
//------------------ sequences register only the referenced base functions afterwards)
LState *open_bare(int argc = 0, char *argv[] = nullptr);
//------------------ Opens all standard libraries
void openlibs(LState *L);
//------------------ Opens only the string library (--minimal)
void openlibs_minimal(LState *L);
//------------------ Closes CLX state
void close(LState *L);

//------------------ Sets lazy-initialized functions on a table
void set_lazy_funcs(LState *L, const LValue &table, const LazyReg *regs, size_t count);

}

#endif
