// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  clx_runtime.h · Internal Runtime Header    │
// └─────────────────────────────────────────────┘

#ifndef CLX_RUNTIME_H
#define CLX_RUNTIME_H

#include <cstdint>
#include <vector>
#include <string>
#include <bit>
#include <iostream>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <memory>
#include <functional>
#include <unordered_map>
#include <initializer_list>

#if defined(_WIN32)
#include <windows.h>
#else
#include <ucontext.h>
#endif

#if defined(_MSC_VER)
#define CLX_MUSTTAIL
#elif defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::musttail)
#define CLX_MUSTTAIL [[clang::musttail]]
#elif __has_cpp_attribute(gnu::musttail)
#define CLX_MUSTTAIL [[gnu::musttail]]
#else
#define CLX_MUSTTAIL
#endif
#else
#define CLX_MUSTTAIL
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#define clx_strlen  strlen
#define clx_memcpy  memcpy
#define clx_memcmp  memcmp
#else
#define clx_strlen  __builtin_strlen
#define clx_memcpy  __builtin_memcpy
#define clx_memcmp  __builtin_memcmp
#endif

#if defined(_WIN32) && defined(CLX_BUILD_AS_DLL)
    #define CLX_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
    #define CLX_API __attribute__((visibility("default")))
#else
    #define CLX_API
#endif

#if defined(_MSC_VER)
#define CLX_INLINE_HOT __forceinline
#define CLX_INLINE_COLD __declspec(noinline) inline
#define CLX_INLINE inline
#elif defined(__GNUC__) || defined(__clang__)
#define CLX_INLINE_HOT inline __attribute__((always_inline))
#define CLX_INLINE_COLD inline __attribute__((noinline))
#define CLX_INLINE inline
#else
#define CLX_INLINE_HOT inline
#define CLX_INLINE_COLD inline
#define CLX_INLINE inline
#endif

namespace clx {

#if defined(_MSC_VER)

//------------------ 128-bit multiply helper
static CLX_INLINE_HOT uint64_t clx_umul128(uint64_t a, uint64_t b, uint64_t* hi) {
    return _umul128(a, b, hi);
}
#else

//------------------ 128-bit multiply helper
static CLX_INLINE_HOT uint64_t clx_umul128(uint64_t a, uint64_t b, uint64_t* hi) {
    __uint128_t r = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
    *hi = static_cast<uint64_t>(r >> 64);
    return static_cast<uint64_t>(r);
}
#endif

//------------------ Value type enum
enum class LType : uint8_t {
    Number = 0, Nil = 1, Bool = 2, Integer = 3, String = 4,
    Table = 5, Function = 6, Userdata = 7, Thread = 8
};

//------------------ Type name strings by LType index
static constexpr const char* TYPE_NAMES[] = {
    "number", "nil", "boolean", "number", "string", "table", "function", "userdata", "thread"
};

//------------------ NaN-boxing tag constants
constexpr uint64_t TAG_NAN     = 0xFFF0000000000000ULL;
constexpr uint64_t TAG_NIL     = TAG_NAN | (static_cast<uint64_t>(LType::Nil) << 48);
constexpr uint64_t TAG_BOOL    = TAG_NAN | (static_cast<uint64_t>(LType::Bool) << 48);
constexpr uint64_t TAG_INTEGER = TAG_NAN | (static_cast<uint64_t>(LType::Integer) << 48);
constexpr uint64_t TAG_STRING  = TAG_NAN | (static_cast<uint64_t>(LType::String) << 48);
constexpr uint64_t TAG_TABLE   = TAG_NAN | (static_cast<uint64_t>(LType::Table) << 48);
constexpr uint64_t TAG_FUNCTION= TAG_NAN | (static_cast<uint64_t>(LType::Function) << 48);
constexpr uint64_t TAG_USERDATA= TAG_NAN | (static_cast<uint64_t>(LType::Userdata) << 48);
constexpr uint64_t TAG_THREAD  = TAG_NAN | (static_cast<uint64_t>(LType::Thread) << 48);

//------------------ Inline-string base tag
constexpr uint64_t TAG_ISTR_BASE = TAG_NAN | (0x9ULL << 48);

//------------------ Payload mask (lower 48 bits)
constexpr uint64_t PAYLOAD_MASK = 0x0000FFFFFFFFFFFFULL;

//------------------ GC object header
struct LHeader {
    uint8_t type;
    uint8_t marked;
    LHeader* next;
};

//------------------ Forward declarations
struct LState;
struct LValue;

//------------------ Binary metamethod dispatcher
LValue call_bin_metamethod(LState* L, const LValue& a, const LValue& b, const char* event);

//------------------ Tagged value (NaN-boxed)
struct LValue {
    uint64_t val;

    CLX_INLINE_HOT LValue() : val(TAG_NIL) {}
    CLX_INLINE_HOT explicit LValue(bool b) : val(TAG_BOOL | (b ? 1ULL : 0ULL)) {}
    CLX_INLINE_HOT explicit LValue(double n) : val(std::bit_cast<uint64_t>(n)) {
        if (val == 0xFFF8000000000000ULL) val = 0x7FF8000000000000ULL;
    }
    CLX_INLINE_HOT explicit LValue(int64_t i) : val(TAG_INTEGER | (static_cast<uint64_t>(i) & PAYLOAD_MASK)) {}
    CLX_INLINE_HOT explicit LValue(const char* s) : val(TAG_STRING | (reinterpret_cast<uint64_t>(s) & PAYLOAD_MASK)) {}
    CLX_INLINE_HOT explicit LValue(LType t, LHeader* p) : val(TAG_NAN | (static_cast<uint64_t>(t) << 48) | (reinterpret_cast<uint64_t>(p) & PAYLOAD_MASK)) {}

    CLX_INLINE_HOT explicit LValue(LHeader* p) : val(TAG_NAN | (static_cast<uint64_t>(p->type) << 48) | (reinterpret_cast<uint64_t>(p) & PAYLOAD_MASK)) {}

    CLX_INLINE_HOT bool is_istr() const {
        return (val >> 48) >= 0xFFF9;
    }

    CLX_INLINE_HOT uint32_t istr_len() const {
        return static_cast<uint32_t>((val >> 48) - 0xFFF9);
    }

    CLX_INLINE_HOT bool is_gc_obj() const {
        uint16_t top = val >> 48;
        return top >= 0xFFF5 && top <= 0xFFF8;
    }

     CLX_INLINE_HOT LType type() const {
         uint16_t top = val >> 48;
         if (top < 0xFFF1) [[likely]] return LType::Number;
         if (top <= 0xFFF8) return static_cast<LType>(top & 0xF);
         if (top <= 0xFFFE) return LType::String;
         return LType::Number;
     }

    CLX_INLINE_HOT double as_number() const {
        if (type() == LType::Integer) return static_cast<double>(as_integer());
        return std::bit_cast<double>(val);
    }
    CLX_INLINE_HOT int64_t as_integer() const {
        uint64_t payload = val & PAYLOAD_MASK;
        if (payload & 0x0000800000000000ULL) return static_cast<int64_t>(payload | 0xFFFF000000000000ULL);
        return static_cast<int64_t>(payload);
    }
    CLX_INLINE_HOT bool as_bool() const {
        if (type() == LType::Nil) return false;
        if (type() == LType::Bool) return (val & 1) != 0;
        return true;
    }
    CLX_INLINE_HOT const char* as_string() const {
        if (is_istr())
            return reinterpret_cast<const char*>(&val);
        return reinterpret_cast<const char*>(val & PAYLOAD_MASK);
    }
    CLX_INLINE_HOT LHeader* as_pointer() const { return reinterpret_cast<LHeader*>(val & PAYLOAD_MASK); }

    CLX_INLINE_HOT uint32_t string_len() const {
        if (is_istr()) return istr_len();
        uint32_t len;
        clx_memcpy(&len, as_string() - 4, 4);
        return len;
    }

    static CLX_INLINE_HOT LValue istr(const char* s, size_t len) {
        uint64_t tag = TAG_ISTR_BASE + (static_cast<uint64_t>(len) << 48);
        uint64_t payload = 0;
        for (size_t i = 0; i < len && i < 5; i++)
            payload |= static_cast<uint64_t>(static_cast<uint8_t>(s[i])) << (i * 8);
        LValue v;
        v.val = tag | payload;
        return v;
    }

    CLX_INLINE_HOT bool to_number(double& out) const {
        if (type() == LType::Number) { out = as_number(); return true; }
        if (type() == LType::Integer) { out = static_cast<double>(as_integer()); return true; }
        if (type() == LType::String) {
            char* end;
            out = std::strtod(as_string(), &end);
            return end != as_string();
        }
        return false;
    }

    CLX_INLINE_HOT LValue operator==(const LValue& other) const {
        if (val == other.val) return LValue(true);
        if (type() == LType::Number && other.type() == LType::Number) return LValue(as_number() == other.as_number());
        return slow_eq(other);
    }
    CLX_INLINE_HOT LValue operator!=(const LValue& other) const {
        return LValue(!(operator==(other)).as_bool());
    }
    CLX_INLINE_HOT LValue operator<(const LValue& other) const {
        if (type() == LType::Integer && other.type() == LType::Integer) return LValue(as_integer() < other.as_integer());
        if (type() == LType::Number && other.type() == LType::Number) return LValue(as_number() < other.as_number());
        if ((type() == LType::Number || type() == LType::Integer) && (other.type() == LType::Number || other.type() == LType::Integer)) {
            double l = (type() == LType::Integer) ? static_cast<double>(as_integer()) : as_number();
            double r = (other.type() == LType::Integer) ? static_cast<double>(other.as_integer()) : other.as_number();
            return LValue(l < r);
        }
        return slow_lt(other);
    }
    CLX_INLINE_HOT LValue operator>(const LValue& other) const {
        return other.operator<(*this);
    }
    CLX_INLINE_HOT LValue operator<=(const LValue& other) const {
        if (type() == LType::Integer && other.type() == LType::Integer) return LValue(as_integer() <= other.as_integer());
        if (type() == LType::Number && other.type() == LType::Number) return LValue(as_number() <= other.as_number());
        if ((type() == LType::Number || type() == LType::Integer) && (other.type() == LType::Number || other.type() == LType::Integer)) {
            double l = (type() == LType::Integer) ? static_cast<double>(as_integer()) : as_number();
            double r = (other.type() == LType::Integer) ? static_cast<double>(other.as_integer()) : other.as_number();
            return LValue(l <= r);
        }
        return slow_le(other);
    }
    CLX_INLINE_HOT LValue operator>=(const LValue& other) const {
        return other.operator<=(*this);
    }

    std::string to_string(LState* L = nullptr) const;

private:
    LValue slow_eq(const LValue& other) const;
    LValue slow_lt(const LValue& other) const;
    LValue slow_le(const LValue& other) const;
};

//------------------ Multi-value return container
struct MultiValue {
    size_t count = 0;
    clx::LValue* overflow = nullptr;
    clx::LValue inline_vals[8];

    MultiValue() = default;

    MultiValue(const clx::LValue& single) : count(1) {
        inline_vals[0] = single;
    }

    MultiValue(const clx::LValue* arr, size_t c) : count(c) {
        if (c <= 8) {
            for (size_t i = 0; i < c; ++i) inline_vals[i] = arr[i];
        } else {
            for (size_t i = 0; i < 8; ++i) inline_vals[i] = arr[i];
            overflow = new clx::LValue[c - 8];
            for (size_t i = 8; i < c; ++i) overflow[i - 8] = arr[i];
        }
    }

    MultiValue(std::initializer_list<clx::LValue> init) : MultiValue(init.begin(), init.size()) {}
    MultiValue(const std::vector<clx::LValue>& vec) : MultiValue(vec.data(), vec.size()) {}

    MultiValue(const MultiValue& other) : count(other.count) {
        size_t inline_c = (count < 8) ? count : 8;
        for (size_t i = 0; i < inline_c; ++i) inline_vals[i] = other.inline_vals[i];
        if (other.overflow) {
            overflow = new clx::LValue[count - 8];
            for (size_t i = 0; i < count - 8; ++i) overflow[i] = other.overflow[i];
        }
    }

    MultiValue(MultiValue&& other) noexcept : count(other.count), overflow(other.overflow) {
        size_t inline_c = (count < 8) ? count : 8;
        for (size_t i = 0; i < inline_c; ++i) inline_vals[i] = other.inline_vals[i];
        other.overflow = nullptr;
        other.count = 0;
    }

    MultiValue& operator=(MultiValue&& other) noexcept {
        if (this != &other) {
            if (overflow) delete[] overflow;
            count = other.count;
            overflow = other.overflow;
            size_t inline_c = (count < 8) ? count : 8;
            for (size_t i = 0; i < inline_c; ++i) inline_vals[i] = other.inline_vals[i];
            other.overflow = nullptr;
            other.count = 0;
        }
        return *this;
    }

    MultiValue& operator=(const MultiValue& other) {
        if (this != &other) {
            if (overflow) { delete[] overflow; overflow = nullptr; }
            count = other.count;
            size_t inline_c = (count < 8) ? count : 8;
            for (size_t i = 0; i < inline_c; ++i) inline_vals[i] = other.inline_vals[i];
            if (other.overflow) {
                overflow = new clx::LValue[count - 8];
                for (size_t i = 0; i < count - 8; ++i) overflow[i] = other.overflow[i];
            }
        }
        return *this;
    }

    ~MultiValue() {
        if (overflow) delete[] overflow;
    }

    clx::LValue& operator[](size_t i) {
        return (i < 8) ? inline_vals[i] : overflow[i - 8];
    }

    const clx::LValue& operator[](size_t i) const {
        return (i < 8) ? inline_vals[i] : overflow[i - 8];
    }
};

//------------------ Forward declarations
struct LState;
struct LTable;

//------------------ Shared upvalue type
using LUpValue = std::shared_ptr<LValue>;

//------------------ Function call dispatcher (with error context)
MultiValue call_function(LState* L, const LValue& func, const LValue* args, size_t count, const char* file, int line);
//------------------ Protected function call
MultiValue pcall_function(LState* L, const LValue& func, const LValue* args, size_t count);

//------------------ Gets metafield from object
LValue getmetafield(LState* L, const LValue& obj, const char* field);
//------------------ Calls metamethod by event name
bool callmeta(LState* L, const LValue& obj, const char* event);

//------------------ Creates a shared upvalue
inline LUpValue make_upvalue(const LValue& val) {
    return std::make_shared<LValue>(val);
}

//------------------ C function type alias
using CFunctionType = std::function<MultiValue(LState*, const LValue*, size_t)>;

//------------------ C function closure
struct LCFunction : public LHeader {
    CFunctionType func;
    MultiValue (*direct)(LState*, const LValue*, size_t) = nullptr;
    LValue (*direct1)(LState*, const LValue*, size_t) = nullptr;
    LCFunction(CFunctionType f);
};

//------------------ Fast path for LCFunction direct calls
CLX_INLINE_HOT MultiValue call_direct(LState* L, const LValue& func, const LValue* args, size_t count) {
    if (func.type() == LType::Function) {
        LCFunction* f = static_cast<LCFunction*>(func.as_pointer());
        if (f->direct) return f->direct(L, args, count);
    }
    return call_function(L, func, args, count, "", 0);
}

//------------------ Userdata block
struct LUserdata : public LHeader {
    LTable* metatable;
    size_t size;

    void* data() { return reinterpret_cast<char*>(this) + sizeof(LUserdata); }
};

//------------------ C function registration entry
struct LReg {
    const char* name;
    CFunctionType func;
};

//------------------ Raw C function pointer type
using RawCFunction = MultiValue(*)(LState*, const LValue*, size_t);

//------------------ C function registration (lazy init)
struct LazyReg {
    const char* name;
    RawCFunction func;
};

//------------------ Hash/array hybrid table
struct LTable : public LHeader {
    LValue* array;
    size_t  array_size;
    size_t  array_cap;
    int32_t* bucket;
    size_t  hash_size;
    LTable* metatable;
    size_t  hash_count;

    uint64_t* keys;
    LValue*  vals;
    int32_t* nexts;
    int32_t  free_head;
    uint32_t shape_version;

    LTable();
    ~LTable();

    LValue* gettable(const LValue& key);
    void settable(const LValue& key, const LValue& val);

    LValue get_value(LState* L, const LValue& key);
    void set_value(LState* L, const LValue& key, const LValue& val);

    void bind(const char* name, const LValue& val);
    void bind(LState* L, const char* name, CFunctionType func);
    void bind_all(LState* L, std::initializer_list<LReg> funcs);

private:
    void resize_hash(size_t new_size);
    void hash_insert_direct(const LValue& key, const LValue& val);
};

//------------------ Wyhash secret constants
static constexpr uint64_t WY_SECRET0 = 0xa0761d6478bd642fULL;
static constexpr uint64_t WY_SECRET1 = 0xe7037ed1a0b428dbULL;

//------------------ 64-bit wyhash mix
static CLX_INLINE_HOT uint32_t wyhash64(uint64_t v) {
    v ^= WY_SECRET0;
    uint64_t lo, hi;
    lo = clx_umul128(v, v ^ WY_SECRET1, &hi);
    return static_cast<uint32_t>(lo ^ hi);
}

//------------------ SWAR hash for ≤8 byte strings
static CLX_INLINE uint32_t swar_hash_8(const char* p, size_t len) {
    uint64_t data = 0;
    if (len > 0 && len <= 8) clx_memcpy(&data, p, len);
    return wyhash64(data ^ static_cast<uint64_t>(len));
}

//------------------ Wyhash for arbitrary-length strings
static CLX_INLINE uint32_t wyhash_str(const char* p, size_t len) {
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
    return static_cast<uint32_t>(seed ^ (seed >> 32));
}

//------------------ Reads baked hash from interned string header
static CLX_INLINE_COLD uint32_t string_baked_hash(const char* ptr) {
    uint32_t h;
    clx_memcpy(&h, ptr - 8, 4);
    return h;
}

//------------------ Hash an LValue by type
static CLX_INLINE_HOT uint32_t lvalue_hash(uint64_t val) {
    uint16_t top = static_cast<uint16_t>(val >> 48);
    if (top == static_cast<uint16_t>(TAG_STRING >> 48)) {
        const char* ptr = reinterpret_cast<const char*>(val & PAYLOAD_MASK);
        uint32_t h;
        clx_memcpy(&h, ptr - 8, 4);
        return h;
    }
    if (top >= 0xFFF9) {
        uint32_t len = static_cast<uint32_t>(top - 0xFFF9);
        const char* ptr = reinterpret_cast<const char*>(&val);
        return swar_hash_8(ptr, len);
    }
    if (top == static_cast<uint16_t>(TAG_INTEGER >> 48)) {
        int64_t i = static_cast<int64_t>(val & PAYLOAD_MASK);
        if (i & 0x0000800000000000LL) i |= 0xFFFF000000000000LL;
        val = std::bit_cast<uint64_t>(static_cast<double>(i));
    }
    val ^= WY_SECRET0;
    uint64_t lo, hi;
    lo = clx_umul128(val, val ^ WY_SECRET1, &hi);
    return static_cast<uint32_t>(lo ^ hi);
}

//------------------ Fast cross-type equality (string/int/number)
static CLX_INLINE_HOT bool lvalue_eq_fast(uint64_t a, uint64_t b) {
    if (a == b) return true;
    uint16_t ta = static_cast<uint16_t>(a >> 48);
    uint16_t tb = static_cast<uint16_t>(b >> 48);
    bool a_istr = ta >= 0xFFF9;
    bool b_istr = tb >= 0xFFF9;
    bool a_str = ta == static_cast<uint16_t>(TAG_STRING >> 48);
    bool b_str = tb == static_cast<uint16_t>(TAG_STRING >> 48);
    if ((a_istr && b_str) || (a_str && b_istr)) {
        LValue va, vb;
        va.val = a;
        vb.val = b;
        uint32_t alen = va.string_len();
        uint32_t blen = vb.string_len();
        return alen == blen && clx_memcmp(va.as_string(), vb.as_string(), alen) == 0;
    }
    uint16_t tag_int_top = static_cast<uint16_t>(TAG_INTEGER >> 48);
    bool a_int = ta == tag_int_top;
    bool b_int = tb == tag_int_top;
    bool a_num = ta < 0xFFF1;
    bool b_num = tb < 0xFFF1;
    if ((a_int && b_num) || (a_num && b_int)) {
        LValue va, vb;
        va.val = a;
        vb.val = b;
        return va.as_number() == vb.as_number();
    }
    return false;
}

//------------------ String interning pool
struct StringPool {
    struct Slot {
        char*    baked;
        uint32_t hash;
        uint32_t len;
        bool empty() const { return baked == nullptr; }
    };

    Slot*  slots    = nullptr;
    size_t capacity = 0;
    size_t count    = 0;

    static constexpr size_t INIT_CAP = 64;

    StringPool() { rehash(INIT_CAP); }
    ~StringPool() {
        if (!slots) return;
        for (size_t i = 0; i < capacity; ++i)
            if (slots[i].baked) delete[] (slots[i].baked - 8);
        delete[] slots;
    }
    StringPool(const StringPool&) = delete;
    StringPool& operator=(const StringPool&) = delete;

    const char* intern(const char* str, size_t len, uint32_t h) {
        if (count * 2 >= capacity) rehash(capacity * 2);
        size_t mask = capacity - 1;
        size_t idx  = h & mask;
        for (;;) {
            Slot& s = slots[idx];
            if (s.empty()) {
                s = make_slot(str, len, h);
                count++;
                return s.baked;
            }
            if (s.hash == h && s.len == static_cast<uint32_t>(len) &&
                clx_memcmp(s.baked, str, len) == 0)
                return s.baked;
            idx = (idx + 1) & mask;
        }
    }

    const char* intern_preallocated(char* prealloc, uint32_t h, size_t len) {
        if (count * 2 >= capacity) rehash(capacity * 2);
        size_t mask = capacity - 1;
        size_t idx  = h & mask;
        for (;;) {
            Slot& s = slots[idx];
            if (s.empty()) {
                s = Slot{prealloc, h, static_cast<uint32_t>(len)};
                count++;
                return prealloc;
            }
            if (s.hash == h && s.len == static_cast<uint32_t>(len) &&
                clx_memcmp(s.baked, prealloc, len) == 0) {
                delete[] (prealloc - 8);
                return s.baked;
            }
            idx = (idx + 1) & mask;
        }
    }

    void reserve(size_t n) {
        if (n > capacity) rehash(n);
    }

private:
    static Slot make_slot(const char* str, size_t len, uint32_t h) {
        char* mem = new char[8 + len + 1];
        uint32_t len32 = static_cast<uint32_t>(len);
        clx_memcpy(mem,     &h,     4);
        clx_memcpy(mem + 4, &len32, 4);
        clx_memcpy(mem + 8, str, len);
        mem[8 + len] = '\0';
        return Slot{mem + 8, h, len32};
    }

    void rehash(size_t new_cap) {
        Slot* old    = slots;
        size_t old_cap = capacity;
        slots    = new Slot[new_cap]();
        capacity = new_cap;
        size_t mask = new_cap - 1;
        for (size_t i = 0; i < old_cap; ++i) {
            if (!old[i].empty()) {
                size_t idx = old[i].hash & mask;
                while (!slots[idx].empty()) idx = (idx + 1) & mask;
                slots[idx] = old[i];
            }
        }
        delete[] old;
    }
};

//------------------ Coroutine thread status enum
enum ThreadStatus {
    THREAD_SUSPENDED = 0,
    THREAD_RUNNING = 1,
    THREAD_DEAD = 2,
    THREAD_NORMAL = 3
};

//------------------ Coroutine thread
struct LThread : public LHeader {
    LState* state;
    LValue function;
    int status;
    LThread* caller;
    MultiValue yield_args;
    MultiValue resume_args;
    bool is_main;
    bool has_error;
#if defined(_WIN32)
    LPVOID fiber;
#else
    ucontext_t ctx;
    char* stack_memory;
#endif

    LThread();
    ~LThread();
};

//------------------ VM state
struct LState {
    LTable* _G;
    LHeader* allocated_objects;
    LTable* free_tables;
    LCFunction* free_functions;
    LUserdata* finalizable_ud;

    LThread* main_thread;
    LThread* running_thread;

    static constexpr size_t MAX_SHADOW_STACK = 262144;
    LValue* shadow_stack[MAX_SHADOW_STACK];
    size_t shadow_top;

    void* stack_bottom;
    std::vector<void*> lib_handles;
    std::unordered_map<std::string, LValue (*)(LState*)> static_modules;
    const char* current_file;
    int current_line;

    size_t object_count;
    size_t gc_bytes_threshold;
    size_t allocated_bytes = 0;

    size_t get_memory_usage() const {
        return allocated_bytes;
    }

    LValue str_index;
    LValue str_newindex;
    LValue str_gc;
    LValue str_call;
    LValue str_close;
    LValue str_pairs;
    LValue str_tostring;
    LTable* string_metatable;

    StringPool string_pool;

    CLX_INLINE const char* intern_string(const char* str, size_t len) {
        uint32_t h = len <= 8 ? swar_hash_8(str, len) : wyhash_str(str, len);
        return string_pool.intern(str, len, h);
    }
    CLX_INLINE const char* intern_string(const std::string& s) {
        return intern_string(s.data(), s.size());
    }
    CLX_INLINE const char* intern_string(const char* str) {
        return intern_string(str, __builtin_strlen(str));
    }
    CLX_INLINE const char* intern_prehashed(const char* str, size_t len, uint32_t h) {
        return string_pool.intern(str, len, h);
    }

    LState();
    ~LState();

    LValue create_table(size_t asize = 0, size_t hsize = 0);
    LValue create_closure(CFunctionType func);

    std::vector<LHeader*> gc_worklist;

    
    enum class GCPhase : uint8_t { Idle, Sweeping };
    GCPhase  gc_phase         = GCPhase::Idle;
    LHeader* gc_sweep_cursor  = nullptr;
    LHeader* gc_prev          = nullptr;
    LHeader* gc_finalizable   = nullptr;
    LHeader* gc_finalizable_ud = nullptr;
    static constexpr size_t GC_STEP_BUDGET = 512;
    bool gc_running        = true;
    int  gc_pause          = 200;    
    int  gc_stepmul        = 200;    
    int  gc_stepsize       = 13;     
    void collect_garbage();
    bool gc_step();  

    void register_module(const std::string& name, LValue (*func)(LState*));
};

//------------------ String builder (part list)
class StringBuilder {
    static constexpr size_t INLINE_CAP = 8;

    const char* parts_[INLINE_CAP];
    uint32_t    lens_[INLINE_CAP];
    const char** parts;
    uint32_t*    lens;
    size_t       count;
    size_t       cap;
    size_t       total_len;
    mutable const char*  cached;

    void grow() {
        size_t new_cap = cap * 2;
        auto* np = new const char*[new_cap];
        auto* nl = new uint32_t[new_cap];
        for (size_t i = 0; i < count; ++i) { np[i] = parts[i]; nl[i] = lens[i]; }
        if (parts != parts_) { delete[] parts; delete[] lens; }
        parts = np; lens = nl; cap = new_cap;
    }

public:
    StringBuilder()
        : parts(parts_), lens(lens_), count(0), cap(INLINE_CAP), total_len(0), cached(nullptr) {}

    ~StringBuilder() {
        if (parts != parts_) { delete[] parts; delete[] lens; }
    }

    StringBuilder(const StringBuilder& o)
        : parts(parts_), lens(lens_), count(0), cap(INLINE_CAP), total_len(0), cached(nullptr) {
        for (size_t i = 0; i < o.count; ++i) {
            char* p = new char[o.lens[i]];
            clx_memcpy(p, o.parts[i], o.lens[i]);
            append(p, o.lens[i]);
            delete[] p;
        }
    }
    StringBuilder& operator=(const StringBuilder& o) {
        if (this != &o) {
            if (parts != parts_) { delete[] parts; delete[] lens; }
            parts = parts_; lens = lens_; count = 0; cap = INLINE_CAP; total_len = 0; cached = nullptr;
            for (size_t i = 0; i < o.count; ++i) {
                char* p = new char[o.lens[i]];
                clx_memcpy(p, o.parts[i], o.lens[i]);
                append(p, o.lens[i]);
                delete[] p;
            }
        }
        return *this;
    }

    StringBuilder(StringBuilder&& o) noexcept
        : parts(o.parts == o.parts_ ? parts_ : o.parts),
          lens(o.lens == o.lens_ ? lens_ : o.lens),
          count(o.count), cap(o.cap),
          total_len(o.total_len), cached(o.cached) {
        if (o.parts == o.parts_) {
            for (size_t i = 0; i < count; ++i) { parts_[i] = o.parts_[i]; lens_[i] = o.lens_[i]; }
        }
        o.parts = o.parts_; o.lens = o.lens_; o.count = 0; o.cap = INLINE_CAP;
        o.total_len = 0; o.cached = nullptr;
    }

    StringBuilder& operator=(StringBuilder&& o) noexcept {
        if (parts != parts_) { delete[] parts; delete[] lens; }
        parts = o.parts == o.parts_ ? parts_ : o.parts;
        lens = o.lens == o.lens_ ? lens_ : o.lens;
        count = o.count; cap = o.cap;
        total_len = o.total_len; cached = o.cached;
        if (o.parts == o.parts_) {
            for (size_t i = 0; i < count; ++i) { parts_[i] = o.parts_[i]; lens_[i] = o.lens_[i]; }
        }
        o.parts = o.parts_; o.lens = o.lens_; o.count = 0; o.cap = INLINE_CAP;
        o.total_len = 0; o.cached = nullptr;
        return *this;
    }

    CLX_INLINE_HOT void clear() {
        cached = nullptr;
        count = 0;
        total_len = 0;
    }

    CLX_INLINE StringBuilder& append(const char* s, uint32_t len) {
        cached = nullptr;
        if (count >= cap) grow();
        parts[count] = s; lens[count] = len; count++; total_len += len;
        return *this;
    }

    CLX_INLINE StringBuilder& append(LState* L, const LValue& v) {
        if (v.type() == LType::String) {
            return append(v.as_string(), v.string_len());
        }
        if (v.type() == LType::Integer) {
            char buf[32];
            size_t n = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "%lld",
                static_cast<long long>(v.as_integer())));
            char* s = new char[n]; clx_memcpy(s, buf, n);
            return append(s, static_cast<uint32_t>(n));
        }
        if (v.type() == LType::Number) {
            char buf[32];
            size_t n = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "%.14g", v.as_number()));
            char* s = new char[n]; clx_memcpy(s, buf, n);
            return append(s, static_cast<uint32_t>(n));
        }
        if (v.type() == LType::Nil) return append("nil", 3);
        if (v.type() == LType::Bool) return append(v.as_bool() ? "true" : "false", v.as_bool() ? 4 : 5);
        return append("(unknown)", 9);
    }

    const char* to_string(LState* L) const {
        if (cached) return cached;
        if (count == 0) { cached = L->intern_string("", 0); return cached; }
        uint32_t len32 = static_cast<uint32_t>(total_len);
        char* mem = new char[8 + total_len + 1];
        clx_memcpy(mem + 4, &len32, 4);
        char* p = mem + 8;
        for (size_t i = 0; i < count; ++i) { clx_memcpy(p, parts[i], lens[i]); p += lens[i]; }
        *p = '\0';
        uint32_t h = total_len <= 8 ? swar_hash_8(mem + 8, total_len) : wyhash_str(mem + 8, total_len);
        clx_memcpy(mem, &h, 4);
        cached = L->string_pool.intern_preallocated(mem + 8, h, total_len);
        return cached;
    }

    CLX_INLINE_HOT size_t size() const { return total_len; }
    CLX_INLINE_HOT bool empty() const { return count == 0; }
};

//------------------ Runtime error exception
class LRuntimeException : public std::exception {
public:
    clx::LValue error_obj;
    mutable std::string cached_msg;

    LRuntimeException(clx::LValue err);
    virtual ~LRuntimeException() noexcept;
    virtual const char* what() const noexcept override;
};

//------------------ Throws an LRuntimeException
[[noreturn]] CLX_INLINE_COLD void throw_runtime_error(const char* msg) {
    throw LRuntimeException(LValue(msg));
}

//------------------ Stack scope guard
struct ScopeGuard {
    LState* L;
    size_t prev_top;
    CLX_INLINE_HOT ScopeGuard(LState* state) : L(state), prev_top(state->shadow_top) {}
    CLX_INLINE_HOT ~ScopeGuard() { L->shadow_top = prev_top; }
};

//------------------ Close guard (for <close> vars)
struct CloseGuard {
    LState* L;
    LValue val;

    CLX_INLINE_HOT CloseGuard(LState* state, const LValue& v) : L(state), val(v) {}
    ~CloseGuard();
};

//------------------ Reads a variable from environment table
inline LValue get_env_var(LState* L, LUpValue env, const char* name) {
    LTable* t = static_cast<LTable*>((*env).as_pointer());
    LValue key = LValue(L->intern_string(name));
    LValue* val = t->gettable(key);
    return val ? *val : LValue();
}

//------------------ Writes a variable to environment table
inline void set_env_var(LState* L, LUpValue env, const char* name, const LValue& val) {
    LTable* t = static_cast<LTable*>((*env).as_pointer());
    LValue key = LValue(L->intern_string(name));
    t->settable(key, val);
}

//------------------ Addition with metamethod fallback
  CLX_INLINE_HOT LValue clx_add(LState* L, const LValue& a, const LValue& b) {
     if (a.type() == LType::Integer && b.type() == LType::Integer) [[likely]] return LValue(a.as_integer() + b.as_integer());
     if ((a.type() == LType::Number || a.type() == LType::Integer) && (b.type() == LType::Number || b.type() == LType::Integer)) [[likely]] {
         double l = (a.type() == LType::Integer) ? static_cast<double>(a.as_integer()) : a.as_number();
         double r = (b.type() == LType::Integer) ? static_cast<double>(b.as_integer()) : b.as_number();
         return LValue(l + r);
     }
     double l, r;
     if (a.to_number(l) && b.to_number(r)) return LValue(l + r);
     return call_bin_metamethod(L, a, b, "__add");
 }

 CLX_INLINE_HOT LValue clx_sub(LState* L, const LValue& a, const LValue& b) {
     if (a.type() == LType::Integer && b.type() == LType::Integer) [[likely]] return LValue(a.as_integer() - b.as_integer());
     if ((a.type() == LType::Number || a.type() == LType::Integer) && (b.type() == LType::Number || b.type() == LType::Integer)) [[likely]] {
         double l = (a.type() == LType::Integer) ? static_cast<double>(a.as_integer()) : a.as_number();
         double r = (b.type() == LType::Integer) ? static_cast<double>(b.as_integer()) : b.as_number();
         return LValue(l - r);
     }
     double l, r;
     if (a.to_number(l) && b.to_number(r)) return LValue(l - r);
     return call_bin_metamethod(L, a, b, "__sub");
 }

 CLX_INLINE_HOT LValue clx_mul(LState* L, const LValue& a, const LValue& b) {
     if (a.type() == LType::Integer && b.type() == LType::Integer) [[likely]] return LValue(a.as_integer() * b.as_integer());
     if ((a.type() == LType::Number || a.type() == LType::Integer) && (b.type() == LType::Number || b.type() == LType::Integer)) [[likely]] {
         double l = (a.type() == LType::Integer) ? static_cast<double>(a.as_integer()) : a.as_number();
         double r = (b.type() == LType::Integer) ? static_cast<double>(b.as_integer()) : b.as_number();
         return LValue(l * r);
     }
     double l, r;
     if (a.to_number(l) && b.to_number(r)) return LValue(l * r);
     return call_bin_metamethod(L, a, b, "__mul");
 }

 CLX_INLINE_HOT LValue clx_div(LState* L, const LValue& a, const LValue& b) {
     if ((a.type() == LType::Number || a.type() == LType::Integer) && (b.type() == LType::Number || b.type() == LType::Integer)) [[likely]] {
         double l = (a.type() == LType::Integer) ? static_cast<double>(a.as_integer()) : a.as_number();
         double r = (b.type() == LType::Integer) ? static_cast<double>(b.as_integer()) : b.as_number();
         return LValue(l / r);
     }
     double l, r;
     if (a.to_number(l) && b.to_number(r)) return LValue(l / r);
     return call_bin_metamethod(L, a, b, "__div");
 }

//------------------ Equality with metamethod fallback
CLX_INLINE_HOT LValue clx_eq(LState* L, const LValue& a, const LValue& b) {
    if (a.val == b.val) return LValue(true);
    if (a.type() == LType::Integer && b.type() == LType::Number) return LValue(static_cast<double>(a.as_integer()) == b.as_number());
    if (a.type() == LType::Number && b.type() == LType::Integer) return LValue(a.as_number() == static_cast<double>(b.as_integer()));
    if (a.type() == b.type()) {
        if (a.type() == LType::Integer) return LValue(a.as_integer() == b.as_integer());
        if (a.type() == LType::Number) return LValue(a.as_number() == b.as_number());
        if (a.type() == LType::String) return LValue(std::string_view(a.as_string()) == std::string_view(b.as_string()));
        if (a.type() == LType::Bool) return LValue(a.as_bool() == b.as_bool());
        if (a.type() == LType::Nil) return LValue(true);
        if (a.as_pointer() == b.as_pointer()) return LValue(true);
    }
    return call_bin_metamethod(L, a, b, "__eq");
}

//------------------ Safe integer conversion (no metamethods)
CLX_INLINE bool clx_to_integer(const LValue& v, int64_t& out) {
    if (v.type() == LType::Integer) {
        out = v.as_integer();
        return true;
    }
    if (v.type() == LType::Number) {
        double d = v.as_number();
        if (std::floor(d) == d) {
            out = static_cast<int64_t>(d);
            return true;
        }
    }
    return false;
}

//------------------ Bitwise AND with metamethod fallback
CLX_INLINE LValue clx_band(LState* L, const LValue& a, const LValue& b) {
    int64_t l, r;
    if (clx_to_integer(a, l) && clx_to_integer(b, r)) [[likely]] return LValue(l & r);
    return call_bin_metamethod(L, a, b, "__band");
}

//------------------ Bitwise OR with metamethod fallback
CLX_INLINE LValue clx_bor(LState* L, const LValue& a, const LValue& b) {
    int64_t l, r;
    if (clx_to_integer(a, l) && clx_to_integer(b, r)) [[likely]] return LValue(l | r);
    return call_bin_metamethod(L, a, b, "__bor");
}

//------------------ Bitwise XOR with metamethod fallback
CLX_INLINE LValue clx_bxor(LState* L, const LValue& a, const LValue& b) {
    int64_t l, r;
    if (clx_to_integer(a, l) && clx_to_integer(b, r)) [[likely]] return LValue(l ^ r);
    return call_bin_metamethod(L, a, b, "__bxor");
}

//------------------ Bitwise SHL with metamethod fallback
CLX_INLINE LValue clx_shl(LState* L, const LValue& a, const LValue& b) {
    int64_t l, r;
    if (clx_to_integer(a, l) && clx_to_integer(b, r)) [[likely]] return LValue(l << r);
    return call_bin_metamethod(L, a, b, "__shl");
}

//------------------ Bitwise SHR with metamethod fallback
CLX_INLINE LValue clx_shr(LState* L, const LValue& a, const LValue& b) {
    int64_t l, r;
    if (clx_to_integer(a, l) && clx_to_integer(b, r)) [[likely]] return LValue(l >> r);
    return call_bin_metamethod(L, a, b, "__shr");
}

//------------------ Bitwise NOT with metamethod fallback
CLX_INLINE LValue clx_bnot(LState* L, const LValue& a) {
    int64_t v;
    if (clx_to_integer(a, v)) [[likely]] return LValue(~v);
    return call_bin_metamethod(L, a, a, "__bnot");
}

 CLX_INLINE LValue clx_lt(LState* L, const LValue& a, const LValue& b) {
     if (a.type() == LType::Integer && b.type() == LType::Integer) [[likely]] return LValue(a.as_integer() < b.as_integer());
     if ((a.type() == LType::Number || a.type() == LType::Integer) && (b.type() == LType::Number || b.type() == LType::Integer)) [[likely]] {
         double l = (a.type() == LType::Integer) ? static_cast<double>(a.as_integer()) : a.as_number();
         double r = (b.type() == LType::Integer) ? static_cast<double>(b.as_integer()) : b.as_number();
         return LValue(l < r);
     }
     if (a.type() == LType::String && b.type() == LType::String) return a < b;
     return call_bin_metamethod(L, a, b, "__lt");
 }

 CLX_INLINE LValue clx_le(LState* L, const LValue& a, const LValue& b) {
     if (a.type() == LType::Integer && b.type() == LType::Integer) [[likely]] return LValue(a.as_integer() <= b.as_integer());
     if ((a.type() == LType::Number || a.type() == LType::Integer) && (b.type() == LType::Number || b.type() == LType::Integer)) [[likely]] {
         double l = (a.type() == LType::Integer) ? static_cast<double>(a.as_integer()) : a.as_number();
         double r = (b.type() == LType::Integer) ? static_cast<double>(b.as_integer()) : b.as_number();
         return LValue(l <= r);
     }
     if (a.type() == LType::String && b.type() == LType::String) return a <= b;
     return call_bin_metamethod(L, a, b, "__le");
 }

//------------------ Length operator with metamethod fallback
CLX_INLINE_COLD LValue clx_len(LState* L, const LValue& a) {
    if (a.type() == LType::String) {
        return LValue(static_cast<int64_t>(a.string_len()));
    }

    if (a.type() != LType::Table) return call_bin_metamethod(L, a, a, "__len");
    LTable* t = static_cast<LTable*>(a.as_pointer());

    if (t->metatable) {
        LValue* mm = t->metatable->gettable(LValue(L->intern_string("__len", 5)));
        if (mm && mm->type() != LType::Nil) return call_bin_metamethod(L, a, a, "__len");
    }

    int64_t lo = 0;
    int64_t hi = static_cast<int64_t>(t->array_size) + 1;
    while (true) {
        LValue* p = t->gettable(LValue(hi));
        if (!p || p->type() == LType::Nil) break;
        lo = hi;
        hi = (hi < 8) ? 8 : hi * 2;
    }
    while (hi - lo > 1) {
        int64_t mid = lo + (hi - lo) / 2;
        LValue* p = t->gettable(LValue(mid));
        if (p && p->type() != LType::Nil) lo = mid; else hi = mid;
    }
    return LValue(lo);
}

//------------------ Modulo with metamethod fallback
CLX_INLINE LValue clx_mod(LState* L, const LValue& a, const LValue& b) {
    if ((a.type() == LType::Number || a.type() == LType::Integer) && (b.type() == LType::Number || b.type() == LType::Integer)) {
        double l = (a.type() == LType::Integer) ? static_cast<double>(a.as_integer()) : a.as_number();
        double r = (b.type() == LType::Integer) ? static_cast<double>(b.as_integer()) : b.as_number();
        return LValue(l - std::floor(l / r) * r);
    }
    double l, r;
    if (a.to_number(l) && b.to_number(r)) return LValue(l - std::floor(l / r) * r);
    return call_bin_metamethod(L, a, b, "__mod");
}

//------------------ Floor-division with metamethod fallback
CLX_INLINE LValue clx_idiv(LState* L, const LValue& a, const LValue& b) {
    if ((a.type() == LType::Number || a.type() == LType::Integer) && (b.type() == LType::Number || b.type() == LType::Integer)) {
        double l = (a.type() == LType::Integer) ? static_cast<double>(a.as_integer()) : a.as_number();
        double r = (b.type() == LType::Integer) ? static_cast<double>(b.as_integer()) : b.as_number();
        return LValue(std::floor(l / r));
    }
    double l, r;
    if (a.to_number(l) && b.to_number(r)) return LValue(std::floor(l / r));
    return call_bin_metamethod(L, a, b, "__idiv");
}

//------------------ Power with metamethod fallback
CLX_INLINE LValue clx_pow(LState* L, const LValue& a, const LValue& b) {
    if ((a.type() == LType::Number || a.type() == LType::Integer) && (b.type() == LType::Number || b.type() == LType::Integer)) {
        double l = (a.type() == LType::Integer) ? static_cast<double>(a.as_integer()) : a.as_number();
        double r = (b.type() == LType::Integer) ? static_cast<double>(b.as_integer()) : b.as_number();
        return LValue(std::pow(l, r));
    }
    double l, r;
    if (a.to_number(l) && b.to_number(r)) return LValue(std::pow(l, r));
    return call_bin_metamethod(L, a, b, "__pow");
}

//------------------ Unary minus with metamethod fallback
CLX_INLINE LValue clx_unm(LState* L, const LValue& a) {
    if (a.type() == LType::Integer) return LValue(-static_cast<double>(a.as_integer()));
    if (a.type() == LType::Number) return LValue(-a.as_number());
    double d;
    if (a.to_number(d)) return LValue(-d);
    return call_bin_metamethod(L, a, a, "__unm");
}

//------------------ Logical NOT
CLX_INLINE LValue clx_not(const LValue& a) {
    return LValue(!a.as_bool());
}

//------------------ Multi-value concat (string builder fast path)
CLX_INLINE_COLD LValue clx_concat_multi(LState* L, const LValue* args, size_t count) {
    if (count <= 8) [[likely]] {
        const char* ptrs[8];
        size_t  lens[8];
        size_t total_len = 0;
        for (size_t i = 0; i < count; ++i) {
            if (args[i].type() != LType::String && args[i].type() > LType::Integer) {
                return call_bin_metamethod(L, args[i], args[i == count - 1 ? i - 1 : i + 1], "__concat");
            }
            if (args[i].type() == LType::String) {
                ptrs[i] = args[i].as_string();
                lens[i] = args[i].string_len();
            } else {
                char buf[32];
                if (args[i].type() == LType::Integer) {
                    lens[i] = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "%lld",
                        static_cast<long long>(args[i].as_integer())));
                } else {
                    lens[i] = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "%.14g", args[i].as_number()));
                }
                char* s = new char[lens[i]];
                clx_memcpy(s, buf, lens[i]);
                ptrs[i] = s;
            }
            total_len += lens[i];
        }
        uint32_t len32 = static_cast<uint32_t>(total_len);
        char* mem = new char[8 + total_len + 1];
        clx_memcpy(mem,     &len32, 4);
        clx_memcpy(mem + 4, &len32, 4);
        char* p = mem + 8;
        for (size_t i = 0; i < count; ++i) {
            clx_memcpy(p, ptrs[i], lens[i]);
            p += lens[i];
            if (args[i].type() != LType::String) delete[] ptrs[i];
        }
        *p = '\0';
        uint32_t h = total_len <= 8 ? swar_hash_8(mem + 8, total_len) : wyhash_str(mem + 8, total_len);
        clx_memcpy(mem, &h, 4);
        const char* result = L->string_pool.intern_preallocated(mem + 8, h, total_len);
        return LValue(result);
    }

    size_t total_len = 0;
    std::vector<std::string> parts;
    parts.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (args[i].type() != LType::String && args[i].type() > LType::Integer) {
            return call_bin_metamethod(L, args[i], args[i == count - 1 ? i - 1 : i + 1], "__concat");
        }
        parts.push_back(args[i].to_string(L));
        total_len += parts.back().length();
    }
    std::string res;
    res.reserve(total_len);
    for (const auto& p : parts) res += p;
    return LValue(L->intern_string(res));
}

//------------------ Table read with __index fallback
inline LValue clx_table_get(LState* L, const LValue& obj, const LValue& key) {
    LTable* mt = nullptr;
    LValue direct;

    if (obj.type() == LType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        if (key.type() == LType::Integer) {
            if (static_cast<uint64_t>(key.as_integer() - 1) < t->array_cap) direct = t->array[key.as_integer() - 1];
        } else if (key.type() == LType::Number) {
            double d = key.as_number();
            int64_t idx = static_cast<int64_t>(d);
            if (d == static_cast<double>(idx) && static_cast<uint64_t>(idx - 1) < t->array_cap) direct = t->array[idx - 1];
        }
        if (direct.type() == LType::Nil) {
            if (t->hash_size > 0) {
                uint32_t _h = lvalue_hash(key.val) & static_cast<uint32_t>(t->hash_size - 1);
                for (int32_t _c = t->bucket[_h]; _c != -1; _c = t->nexts[_c]) {
                    if (lvalue_eq_fast(t->keys[_c], key.val)) {
                        direct = t->vals[_c];
                        break;
                    }
                }
            }
            if (direct.type() == LType::Nil)
                direct = t->get_value(L, key);
        }
        if (direct.type() != LType::Nil) return direct;
        mt = t->metatable;
    } else if (obj.type() == LType::Userdata) {
        LUserdata* ud = static_cast<LUserdata*>(obj.as_pointer());
        mt = ud->metatable;
    } else if (obj.type() == LType::String) {
        mt = L->string_metatable;
        if (!mt) return LValue();
        LValue* index = mt->gettable(LValue(L->intern_string("__index")));
        if (!index) return LValue();
        if (index->type() == LType::Table)
            return clx_table_get(L, *index, key);
        if (index->type() == LType::Function) {
            LValue args[2] = { obj, key };
            size_t prev = L->shadow_top;
            L->shadow_stack[L->shadow_top++] = &args[0];
            L->shadow_stack[L->shadow_top++] = &args[1];
            MultiValue mv = call_function(L, *index, args, 2, "__index", 0);
            L->shadow_top = prev;
            return mv.count > 0 ? mv[0] : LValue();
        }
        return LValue();
    } else {
        return LValue();
    }

    if (!mt) return LValue();
    LValue* index = mt->gettable(LValue(L->intern_string("__index")));
    if (!index) return LValue();
    if (index->type() == LType::Function) {
        LValue args[2] = { obj, key };
        size_t prev = L->shadow_top;
        L->shadow_stack[L->shadow_top++] = &args[0];
        L->shadow_stack[L->shadow_top++] = &args[1];
        MultiValue mv = call_function(L, *index, args, 2, "__index", 0);
        L->shadow_top = prev;
        return mv.count > 0 ? mv[0] : LValue();
    }
    if (index->type() == LType::Table)
        return clx_table_get(L, *index, key);
    return LValue();
}

//------------------ Table write with __newindex fallback
inline void clx_table_set(LState* L, const LValue& obj, const LValue& key, const LValue& val) {
    LTable* mt = nullptr;

    if (obj.type() == LType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        if (key.type() == LType::Integer) {
            int64_t k = key.as_integer();
            if (static_cast<uint64_t>(k - 1) < t->array_cap) {
                t->array[k - 1] = val;
                if (static_cast<size_t>(k) > t->array_size) t->array_size = k;
                return;
            }
        } else if (key.type() == LType::Number) {
            double d = key.as_number();
            int64_t idx = static_cast<int64_t>(d);
            if (d == static_cast<double>(idx) && static_cast<uint64_t>(idx - 1) < t->array_cap) {
                t->array[idx - 1] = val;
                if (static_cast<size_t>(idx) > t->array_size) t->array_size = idx;
                return;
            }
        }
        t->set_value(L, key, val);
        return;
    } else if (obj.type() == LType::Userdata) {
        LUserdata* ud = static_cast<LUserdata*>(obj.as_pointer());
        mt = ud->metatable;
        if (!mt) return;
        LValue* newindex = mt->gettable(LValue(L->intern_string("__newindex")));
        if (!newindex) return;
        if (newindex->type() == LType::Function) {
            LValue args[3] = { obj, key, val };
            size_t prev = L->shadow_top;
            L->shadow_stack[L->shadow_top++] = &args[0];
            L->shadow_stack[L->shadow_top++] = &args[1];
            L->shadow_stack[L->shadow_top++] = &args[2];
            call_function(L, *newindex, args, 3, "__newindex", 0);
            L->shadow_top = prev;
        } else if (newindex->type() == LType::Table) {
            clx_table_set(L, *newindex, key, val);
        }
        return;
    }
}

//------------------ Integer-key table read (fast path)
inline LValue clx_table_get_int(LState* L, const LValue& obj, size_t idx) {
    LTable* mt = nullptr;
    LValue key_val = LValue(static_cast<int64_t>(idx));

    if (obj.type() == LType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        if (idx - 1 < t->array_cap) return t->array[idx - 1];
        LValue result = t->get_value(L, key_val);
        if (result.type() != LType::Nil) return result;
        mt = t->metatable;
    } else if (obj.type() == LType::Userdata) {
        LUserdata* ud = static_cast<LUserdata*>(obj.as_pointer());
        mt = ud->metatable;
    } else {
        return LValue();
    }

    if (!mt) return LValue();
    LValue* index = mt->gettable(LValue(L->intern_string("__index")));
    if (!index) return LValue();
    if (index->type() == LType::Function) {
        LValue args[2] = { obj, key_val };
        size_t prev = L->shadow_top;
        L->shadow_stack[L->shadow_top++] = &args[0];
        L->shadow_stack[L->shadow_top++] = &args[1];
        MultiValue mv = call_function(L, *index, args, 2, "__index", 0);
        L->shadow_top = prev;
        return mv.count > 0 ? mv[0] : LValue();
    }
    if (index->type() == LType::Table)
        return clx_table_get(L, *index, key_val);
    return LValue();
}

//------------------ Integer-key table write (fast path)
inline void clx_table_set_int(LState* L, const LValue& obj, size_t idx, const LValue& val) {
    LValue key_val = LValue(static_cast<int64_t>(idx));

    if (obj.type() == LType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        if (idx - 1 < t->array_cap) {
            t->array[idx - 1] = val;
            if (static_cast<size_t>(idx) > t->array_size) t->array_size = idx;
            return;
        }
        t->set_value(L, key_val, val);
        return;
    }

    if (obj.type() == LType::Userdata) {
        LUserdata* ud = static_cast<LUserdata*>(obj.as_pointer());
        if (!ud->metatable) return;
        LValue* newindex = ud->metatable->gettable(LValue(L->intern_string("__newindex")));
        if (!newindex) return;
        if (newindex->type() == LType::Function) {
            LValue args[3] = { obj, key_val, val };
            size_t prev = L->shadow_top;
            L->shadow_stack[L->shadow_top++] = &args[0];
            L->shadow_stack[L->shadow_top++] = &args[1];
            L->shadow_stack[L->shadow_top++] = &args[2];
            call_function(L, *newindex, args, 3, "__newindex", 0);
            L->shadow_top = prev;
        } else if (newindex->type() == LType::Table) {
            clx_table_set(L, *newindex, key_val, val);
        }
    }
}

//------------------ Cached table access slot
struct CacheSlot {
    uint64_t table_val;
    uint32_t shape_version;
    LValue    cached;
    bool      valid;
};

//------------------ Cached slot table read
CLX_INLINE_HOT LValue clx_table_get_cs(LState* L, const LValue& obj, const LValue& key, CacheSlot* cs) {
    if (cs->valid && cs->table_val == obj.val && obj.type() == LType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        if (t->shape_version == cs->shape_version)
            return cs->cached;
    }
    LValue result = clx_table_get(L, obj, key);
    if (!result.is_gc_obj() && obj.type() == LType::Table) {
        cs->valid = true;
        cs->table_val = obj.val;
        cs->shape_version = static_cast<LTable*>(obj.as_pointer())->shape_version;
        cs->cached     = result;
    }
    return result;
}

//------------------ Cached slot table write
CLX_INLINE_HOT void clx_table_set_cs(LState* L, const LValue& obj, const LValue& key, const LValue& val, CacheSlot* cs) {
    clx_table_set(L, obj, key, val);
    if (!val.is_gc_obj() && obj.type() == LType::Table) {
        cs->valid = true;
        cs->table_val = obj.val;
        cs->shape_version = static_cast<LTable*>(obj.as_pointer())->shape_version;
        cs->cached     = val;
    }
}

//------------------ String library function declarations
MultiValue str_len(LState*, const LValue*, size_t);
MultiValue str_sub(LState*, const LValue*, size_t);
MultiValue str_reverse(LState*, const LValue*, size_t);
MultiValue str_lower(LState*, const LValue*, size_t);
MultiValue str_upper(LState*, const LValue*, size_t);
MultiValue str_rep(LState*, const LValue*, size_t);
MultiValue str_byte(LState*, const LValue*, size_t);
MultiValue str_char(LState*, const LValue*, size_t);
MultiValue str_dump(LState*, const LValue*, size_t);
MultiValue str_format(LState*, const LValue*, size_t);
MultiValue str_find(LState*, const LValue*, size_t);
MultiValue str_match(LState*, const LValue*, size_t);
MultiValue str_gmatch(LState*, const LValue*, size_t);
MultiValue str_gsub(LState*, const LValue*, size_t);
MultiValue str_pack(LState*, const LValue*, size_t);
MultiValue str_packsize(LState*, const LValue*, size_t);
MultiValue str_unpack(LState*, const LValue*, size_t);

//------------------ Table library function declarations
MultiValue table_concat(LState*, const LValue*, size_t);
MultiValue table_insert(LState*, const LValue*, size_t);
MultiValue table_remove(LState*, const LValue*, size_t);
MultiValue table_sort(LState*, const LValue*, size_t);
MultiValue table_pack(LState*, const LValue*, size_t);
MultiValue table_unpack(LState*, const LValue*, size_t);
MultiValue table_move(LState*, const LValue*, size_t);

//------------------ Standard library opener declarations
void luastd_base(LState* L);
void luastd_package(LState* L);
void luastd_math(LState* L);
void luastd_coroutine(LState* L);
void luastd_string(LState* L);
void luastd_table(LState* L);
void luastd_os(LState* L);
void luastd_utf8(LState* L);
void luastd_io(LState* L);
void luastd_debug(LState* L);

//------------------ Gets a global variable
LValue get_global(LState* L, const char* name);
//------------------ Sets a global variable
void set_global(LState* L, const char* name, const LValue& val);

//------------------ Creates a new coroutine thread
LValue create_thread(LState* L, const LValue& func, double stack_size = 1048576.0);
//------------------ Allocates a new userdata
LValue newuserdata(LState* L, size_t size);
//------------------ Resumes a coroutine
MultiValue resume(LState* L, const LValue& thread, const LValue* args, size_t count);
//------------------ Yields from a coroutine
MultiValue yield(LState* L, const LValue* args, size_t count);

//------------------ Opens CLX state
LState* clx_open(int argc = 0, char* argv[] = nullptr);
//------------------ Opens all standard libraries
void openlibs(LState* L);
//------------------ Closes CLX state
void clx_close(LState* L);

//------------------ Sets lazy-initialized functions on a table
void set_lazy_funcs(LState* L, const LValue& table, const LazyReg* regs, size_t count);

}

#endif
