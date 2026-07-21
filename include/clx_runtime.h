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
#elif defined(__APPLE__) && defined(__aarch64__)
struct CoroutineContext {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp;
    uint64_t lr;
    uint64_t sp;
    uint64_t d8, d9, d10, d11, d12, d13, d14, d15;
};

extern "C" {
    void clx_coro_save(CoroutineContext* ctx);
    void clx_coro_switch(CoroutineContext* from, CoroutineContext* to);
    void clx_coro_init(CoroutineContext* ctx, void* stack_top, void* entry);
}
#else
#if defined(__APPLE__)
#define _XOPEN_SOURCE
#endif
#include <ucontext.h>
#endif

#if defined(_MSC_VER)
#define CLX_MUSTTAIL
#elif defined(__GNUC__) && !defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(gnu::musttail)
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

//------------------ Value type enum (shadow type discriminator)
enum class ValueType : uint8_t {
    Nil      = 0,
    Boolean  = 1,
    Int64    = 2,
    Double   = 3,
    String   = 4,
    Table    = 5,
    Function = 6,
    UserData = 7,
    Thread   = 8
};

//------------------ ValueType short aliases (clx::Nil, clx::String, etc.)
constexpr auto Nil      = ValueType::Nil;
constexpr auto Boolean  = ValueType::Boolean;
constexpr auto Int64    = ValueType::Int64;
constexpr auto Double   = ValueType::Double;
constexpr auto String   = ValueType::String;
constexpr auto Table    = ValueType::Table;
constexpr auto Function = ValueType::Function;
constexpr auto UserData = ValueType::UserData;
constexpr auto Thread   = ValueType::Thread;

//------------------ Type name strings by ValueType index
static constexpr const char* VALUE_TYPE_NAMES[] = {
    "nil", "boolean", "integer", "number", "string", "table", "function", "userdata", "thread"
};

//------------------ GC object header
struct LHeader {
    uint8_t type;
    uint8_t marked;
    LHeader* next;
};

struct LState;
struct TValue;
struct LValue;

//------------------ Binary metamethod dispatcher
LValue call_bin_metamethod(LState* L, const LValue& a, const LValue& b, const char* event);

//------------------ 8-byte value payload --- pure data, no type bits
union alignas(8) TValuePayload {
    uint64_t u64;
    int64_t  i64;
    double   f64;
    void*    ptr;
};

struct TValue {
    TValuePayload payload;

    CLX_INLINE_HOT TValue() { payload.u64 = 0; }
    CLX_INLINE_HOT TValue(int64_t i) { payload.i64 = i; }
    CLX_INLINE_HOT TValue(double d) { payload.f64 = d; }
    CLX_INLINE_HOT TValue(void* p) { payload.ptr = p; }
    CLX_INLINE_HOT TValue(uint64_t u) { payload.u64 = u; }
};

//------------------ Shadow stack slot (payload pointer + type pointer)
struct TypedSlot {
    TValue*    val;
    ValueType* type;
    CLX_INLINE_HOT TypedSlot() : val(nullptr), type(nullptr) {}
    CLX_INLINE_HOT TypedSlot(TValue* v, ValueType* t) : val(v), type(t) {}
};

//------------------ 16-byte convenience wrapper (runtime C++ API)
struct LValue {
    TValue   val;
    ValueType type;

    CLX_INLINE_HOT LValue() : val(), type(ValueType::Nil) {}
    CLX_INLINE_HOT explicit LValue(bool b) : val(static_cast<uint64_t>(b ? 1ULL : 0ULL)), type(ValueType::Boolean) {}
    CLX_INLINE_HOT explicit LValue(double n) : val(n), type(ValueType::Double) {}
    CLX_INLINE_HOT explicit LValue(int64_t i) : val(i), type(ValueType::Int64) {}
    CLX_INLINE_HOT explicit LValue(const char* s) : type(ValueType::String) { val.payload.ptr = const_cast<char*>(s); }
    CLX_INLINE_HOT LValue(const TValue& v, ValueType t) : val(v), type(t) {}
    CLX_INLINE_HOT explicit LValue(ValueType t, LHeader* p) : val(static_cast<void*>(p)), type(t) {}
    CLX_INLINE_HOT explicit LValue(LHeader* p) : val(static_cast<void*>(p)), type(static_cast<ValueType>(p->type)) {}
    CLX_INLINE_HOT explicit LValue(std::nullptr_t) : val(), type(ValueType::Nil) {}

    CLX_INLINE_HOT bool is_gc_obj() const {
        uint8_t t = static_cast<uint8_t>(type);
        return t >= static_cast<uint8_t>(ValueType::Table) && t <= static_cast<uint8_t>(ValueType::Thread);
    }

    CLX_INLINE_HOT double as_number() const {
        if (type == ValueType::Int64) return static_cast<double>(val.payload.i64);
        if (type == ValueType::Double) return val.payload.f64;
        return 0.0;
    }
    CLX_INLINE_HOT int64_t as_integer() const {
        if (type == ValueType::Int64) return val.payload.i64;
        if (type == ValueType::Double) return static_cast<int64_t>(val.payload.f64);
        return 0;
    }
    CLX_INLINE_HOT bool as_bool() const {
        if (type == ValueType::Nil) return false;
        if (type == ValueType::Boolean) return val.payload.u64 != 0;
        return true;
    }
    CLX_INLINE_HOT const char* as_string() const {
        if (type == ValueType::String && (val.payload.u64 >> 56))
            return reinterpret_cast<const char*>(&val.payload.u64);
        if (type == ValueType::String && val.payload.u64 == 0)
            return "";
        return static_cast<const char*>(val.payload.ptr);
    }
    CLX_INLINE_HOT LHeader* as_pointer() const {
        return static_cast<LHeader*>(val.payload.ptr);
    }

    CLX_INLINE_HOT uint32_t string_len() const {
        if (type == ValueType::String) {
            uint64_t top = val.payload.u64 >> 56;
            if (top) return static_cast<uint32_t>(top);
            if (val.payload.u64 == 0) return 0;
            uint32_t len;
            clx_memcpy(&len, static_cast<const char*>(val.payload.ptr) - 8, 4);
            return len;
        }
        return 0;
    }

    static CLX_INLINE_HOT LValue istr(const char* s, size_t len) {
        LValue v;
        v.type = ValueType::String;
        v.val.payload.u64 = 0;
        for (size_t i = 0; i < len && i < 6; i++)
            v.val.payload.u64 |= static_cast<uint64_t>(static_cast<uint8_t>(s[i])) << (i * 8);
        v.val.payload.u64 |= static_cast<uint64_t>(len) << 56;
        return v;
    }

    CLX_INLINE_HOT bool to_number(double& out) const {
        if (type == ValueType::Double) { out = val.payload.f64; return true; }
        if (type == ValueType::Int64) { out = static_cast<double>(val.payload.i64); return true; }
        if (type == ValueType::String) {
            char* end;
            out = std::strtod(as_string(), &end);
            return end != as_string();
        }
        return false;
    }

    CLX_INLINE_HOT LValue operator==(const LValue& other) const {
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
    CLX_INLINE_HOT LValue operator!=(const LValue& other) const {
        return LValue(!(operator==(other)).as_bool());
    }
    CLX_INLINE_HOT LValue operator<(const LValue& other) const {
        if (type == ValueType::Int64 && other.type == ValueType::Int64)
            return LValue(val.payload.i64 < other.val.payload.i64);
        if (type == ValueType::Double && other.type == ValueType::Double)
            return LValue(val.payload.f64 < other.val.payload.f64);
        if ((type == ValueType::Int64 || type == ValueType::Double) &&
            (other.type == ValueType::Int64 || other.type == ValueType::Double)) {
            double l = (type == ValueType::Int64) ? static_cast<double>(val.payload.i64) : val.payload.f64;
            double r = (other.type == ValueType::Int64) ? static_cast<double>(other.val.payload.i64) : other.val.payload.f64;
            return LValue(l < r);
        }
        return slow_lt(other);
    }
    CLX_INLINE_HOT LValue operator>(const LValue& other) const {
        return other.operator<(*this);
    }
    CLX_INLINE_HOT LValue operator<=(const LValue& other) const {
        if (type == ValueType::Int64 && other.type == ValueType::Int64)
            return LValue(val.payload.i64 <= other.val.payload.i64);
        if (type == ValueType::Double && other.type == ValueType::Double)
            return LValue(val.payload.f64 <= other.val.payload.f64);
        if ((type == ValueType::Int64 || type == ValueType::Double) &&
            (other.type == ValueType::Int64 || other.type == ValueType::Double)) {
            double l = (type == ValueType::Int64) ? static_cast<double>(val.payload.i64) : val.payload.f64;
            double r = (other.type == ValueType::Int64) ? static_cast<double>(other.val.payload.i64) : other.val.payload.f64;
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
static_assert(sizeof(LValue) == 16, "LValue must be 16 bytes");

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
CLX_INLINE_HOT LUpValue make_upvalue(const LValue& val) {
    return std::make_shared<LValue>(val);
}

//------------------ C function type alias
using CFunctionType = std::function<MultiValue(LState*, const LValue*, size_t)>;

//------------------ C function closure
struct LCFunction : public LHeader {
    CFunctionType func;
    MultiValue (*direct)(LState*, const LValue*, size_t) = nullptr;
    LValue (*direct1)(LState*, const LValue*, size_t) = nullptr;
    LTable* env = nullptr;
    LCFunction(CFunctionType f);
};

//------------------ Fast path for LCFunction direct calls
MultiValue call_direct(LState* L, const LValue& func, const LValue* args, size_t count, const char* file, int line);

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

static constexpr uint64_t HASH_EMPTY     = 0xFFFFFFFFFFFFFFFFULL;
static constexpr uint64_t HASH_TOMBSTONE = 0xFFFFFFFFFFFFFFFEULL;

struct alignas(8) HashEntry {
    TValue    key;
    TValue    val;
    ValueType ktype;
    ValueType vtype;
};
static_assert(sizeof(HashEntry) == 24, "HashEntry must be 24 bytes");

struct LTable : public LHeader {
    TValue*    array;
    ValueType* array_types;
    size_t     array_size;
    size_t     array_cap;

    HashEntry* entries;
    size_t     hash_size;
    size_t     hash_count;
    size_t     hash_tombs;
    uint64_t*  hash_bitmap;

    uint32_t   hash_version;
    uint32_t   array_version;

    LTable*    metatable;
    bool       is_arena;

    static constexpr int IC_SIZE = 4;
    struct InlineCache {
        uint64_t key_payload  = 0;
        uint32_t entry_idx    = 0;
        uint32_t table_ver    = 0;
    };
    InlineCache ic[IC_SIZE] = {};

    LTable();
    ~LTable();

    LValue gettable(const LValue& key);
    void settable(const LValue& key, const LValue& val);

    LValue get_value(LState* L, const LValue& key);
    void set_value(LState* L, const LValue& key, const LValue& val);

    void bind(const char* name, const LValue& val);
    void bind(LState* L, const char* name, CFunctionType func);
    void bind_all(LState* L, std::initializer_list<LReg> funcs);

private:
    void resize_hash(size_t new_size);
};

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
static CLX_INLINE uint64_t swar_hash_8(const char* p, size_t len) {
    uint64_t data = 0;
    if (len > 0 && len <= 8) clx_memcpy(&data, p, len);
    return wyhash64(data ^ static_cast<uint64_t>(len));
}

//------------------ Wyhash for arbitrary-length strings
static CLX_INLINE uint64_t wyhash_str(const char* p, size_t len) {
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

//------------------ Reads baked hash from interned string header
static CLX_INLINE_COLD uint64_t string_baked_hash(const char* ptr) {
    uint64_t h;
    clx_memcpy(&h, ptr - 16, 8);
    return h;
}

//------------------ Hash an LValue by type
static CLX_INLINE_HOT uint64_t lvalue_hash(const LValue& key) {
    if (key.type == ValueType::String) {
        if (key.val.payload.u64 >> 56) {
            uint32_t len = static_cast<uint32_t>(key.val.payload.u64 >> 56);
            const char* data = reinterpret_cast<const char*>(&key.val.payload.u64);
            return swar_hash_8(data, len);
        }
        if (key.val.payload.u64 == 0) return swar_hash_8("", 0);
        const char* ptr = static_cast<const char*>(key.val.payload.ptr);
        uint64_t h;
        clx_memcpy(&h, ptr - 16, 8);
        return h;
    }
    if (key.type == ValueType::Int64) {
        uint64_t v = std::bit_cast<uint64_t>(static_cast<double>(key.val.payload.i64));
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
static CLX_INLINE_HOT bool lvalue_eq_fast(const LValue& a, const LValue& b) {
    if (a.val.payload.u64 == b.val.payload.u64 && a.type == b.type)
        return true;
    bool a_str = a.type == ValueType::String;
    bool b_str = b.type == ValueType::String;
    if (a_str && b_str) {
        uint32_t al = a.string_len();
        uint32_t bl = b.string_len();
        return al == bl && clx_memcmp(a.as_string(), b.as_string(), al) == 0;
    }
    if ((a.type == ValueType::Int64 && b.type == ValueType::Double) ||
        (a.type == ValueType::Double && b.type == ValueType::Int64)) {
        double da = a.as_number(), db = b.as_number();
        return da == db;
    }
    return false;
}

//------------------ Bump arena for interned strings
struct StringArena {
    struct Block {
        char*   base;
        size_t  used;
        size_t  capacity;
        Block*  next;
        Block(size_t cap) : base(new char[cap]()), used(0), capacity(cap), next(nullptr) {}
        ~Block() { delete[] base; }
    };

    Block*  head    = nullptr;
    Block*  current = nullptr;
    size_t  block_size;

    static constexpr size_t DEFAULT_BLOCK_SIZE = 65536;

    StringArena(size_t bs = DEFAULT_BLOCK_SIZE) : block_size(bs) {
        head = current = new Block(block_size);
    }
    ~StringArena() {
        Block* b = head;
        while (b) { Block* n = b->next; delete b; b = n; }
    }
    StringArena(const StringArena&) = delete;
    StringArena& operator=(const StringArena&) = delete;

    CLX_INLINE char* allocate(size_t size) {
        size = (size + 15) & ~size_t(15);
        if (!current || current->used + size > current->capacity) {
            Block* b = new Block(std::max(block_size, size));
            current->next = b;
            current = b;
        }
        char* result = current->base + current->used;
        current->used += size;
        return result;
    }
};

//------------------ String interning pool
struct StringPool {
    struct Slot {
        char*    baked;
        uint64_t hash;
        uint32_t len;
        bool empty() const { return baked == nullptr; }
    };

    Slot*       slots    = nullptr;
    size_t      capacity = 0;
    size_t      count    = 0;
    StringArena arena;
    uint64_t    guard = 0xDEADBEEFCAFEBABEULL;

    static constexpr size_t INIT_CAP = 64;

    StringPool() : arena(65536) { guard = 0xDEADBEEFCAFEBABEULL; rehash(INIT_CAP); }
    ~StringPool() {
        delete[] slots;
    }
    StringPool(const StringPool&) = delete;
    StringPool& operator=(const StringPool&) = delete;

    const char* intern(const char* str, size_t len, uint64_t h) {
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

    const char* intern_preallocated(char* prealloc, uint64_t h, size_t len) {
        if (count * 2 >= capacity) rehash(capacity * 2);
        size_t mask = capacity - 1;
        size_t idx  = h & mask;
        for (;;) {
            Slot& s = slots[idx];
            if (s.empty()) {
                s = make_slot(prealloc, len, h);
                delete[] (prealloc - 16);
                count++;
                return s.baked;
            }
            if (s.hash == h && s.len == static_cast<uint32_t>(len) &&
                clx_memcmp(s.baked, prealloc, len) == 0) {
                delete[] (prealloc - 16);
                return s.baked;
            }
            idx = (idx + 1) & mask;
        }
    }

    const char* lookup(const char* str, size_t len, uint64_t h) const {
        if (count == 0) return nullptr;
        size_t mask = capacity - 1;
        size_t idx  = h & mask;
        for (;;) {
            const Slot& s = slots[idx];
            if (s.empty()) return nullptr;
            if (s.hash == h && s.len == static_cast<uint32_t>(len) &&
                clx_memcmp(s.baked, str, len) == 0)
                return s.baked;
            idx = (idx + 1) & mask;
        }
    }

    void reserve(size_t n) {
        if (n > capacity) rehash(n);
    }

    struct PrecomputedEntry {
        const char* s;
        uint32_t    len;
        uint64_t    hash;
        uint32_t    slot;
    };

    void bulk_fill_precomputed(const PrecomputedEntry* entries, size_t n) {
        size_t total_arena = 0;
        for (size_t i = 0; i < n; ++i) {
            size_t entry_size = 16 + entries[i].len + 1;
            total_arena += (entry_size + 15) & ~size_t(15);
        }

        char* arena_base = arena.allocate(total_arena);
        char* arena_ptr = arena_base;

        for (size_t i = 0; i < n; ++i) {
            uint32_t len32 = entries[i].len;
            uint32_t h_low = static_cast<uint32_t>(entries[i].hash);
            clx_memcpy(arena_ptr,     &h_low, 4);
            clx_memcpy(arena_ptr + 8, &len32, 4);
            clx_memcpy(arena_ptr + 16, entries[i].s, entries[i].len);
            arena_ptr[16 + entries[i].len] = '\0';

            Slot& s = slots[entries[i].slot];
            s.baked = arena_ptr + 16;
            s.hash  = entries[i].hash;
            s.len   = len32;

            size_t entry_size = 16 + entries[i].len + 1;
            arena_ptr += (entry_size + 15) & ~size_t(15);
        }
        count += n;
    }

private:
    Slot make_slot(const char* str, size_t len, uint64_t h) {
        size_t total = 16 + len + 1;
        total = (total + 15) & ~size_t(15);
        char* mem = arena.allocate(total);
        uint32_t len32 = static_cast<uint32_t>(len);
        uint32_t h_low = static_cast<uint32_t>(h);
        clx_memcpy(mem,     &h_low, 4);
        clx_memcpy(mem + 8, &len32, 4);
        clx_memcpy(mem + 16, str, len);
        mem[16 + len] = '\0';
        return Slot{mem + 16, h, len32};
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
    bool close_requested;
#if defined(_WIN32)
    LPVOID fiber;
#elif defined(__APPLE__) && defined(__aarch64__)
    CoroutineContext ctx;
    char* stack_memory;
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
    LCFunction* current_func = nullptr;
    LHeader* allocated_objects;
    LTable* free_tables;
    LCFunction* free_functions;
    LUserdata* finalizable_ud;

    LThread* main_thread;
    LThread* running_thread;

    static constexpr size_t MAX_SHADOW_STACK = 262144;
    TypedSlot shadow_stack[MAX_SHADOW_STACK];
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

    CLX_INLINE LValue intern_lvalue(const char* str, size_t len) {
        if (len <= 6) return LValue::istr(str, len);
        return LValue(intern_string(str, len));
    }
    CLX_INLINE LValue intern_lvalue(const std::string& s) {
        return intern_lvalue(s.data(), s.size());
    }
    CLX_INLINE const char* intern_string(const char* str, size_t len) {
        uint64_t h = len <= 8 ? swar_hash_8(str, len) : wyhash_str(str, len);
        return string_pool.intern(str, len, h);
    }
    CLX_INLINE const char* intern_string(const std::string& s) {
        return intern_string(s.data(), s.size());
    }
    CLX_INLINE const char* intern_string(const char* str) {
        return intern_string(str, __builtin_strlen(str));
    }
    CLX_INLINE const char* intern_prehashed(const char* str, size_t len, uint64_t h) {
        return string_pool.intern(str, len, h);
    }

    LState();
    ~LState();

    LValue create_table(size_t asize = 0, size_t hsize = 0);
    LValue create_closure(CFunctionType func, LTable* env = nullptr);

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
    void invoke_gc_finalizer(LUserdata* ud, const char* tag);

    void register_module(const std::string& name, LValue (*func)(LState*));
};

inline std::string file_line_prefix(LState* L) {
    if (L->current_file && L->current_file[0] != '\0')
        return std::string(L->current_file) + ":" + std::to_string(L->current_line) + ": ";
    return "";
}

//------------------ String builder (part list)
class StringBuilder {
    static constexpr size_t INLINE_CAP = 8;
    static constexpr size_t ARENA_BLOCK = 4096;

    const char* parts_[INLINE_CAP];
    uint32_t    lens_[INLINE_CAP];
    const char** parts;
    uint32_t*    lens;
    size_t       count;
    size_t       cap;
    size_t       total_len;
    mutable const char*  cached;

    std::vector<std::unique_ptr<char[]>> arena_blocks;
    char* arena_cur = nullptr;
    char* arena_end = nullptr;

    void grow() {
        size_t new_cap = cap * 2;
        auto* np = new const char*[new_cap];
        auto* nl = new uint32_t[new_cap];
        for (size_t i = 0; i < count; ++i) { np[i] = parts[i]; nl[i] = lens[i]; }
        if (parts != parts_) { delete[] parts; delete[] lens; }
        parts = np; lens = nl; cap = new_cap;
    }

    char* arena_alloc(size_t n) {
        if (static_cast<size_t>(arena_end - arena_cur) < n) {
            size_t blk = n > ARENA_BLOCK ? n : ARENA_BLOCK;
            arena_blocks.push_back(std::make_unique<char[]>(blk));
            arena_cur = arena_blocks.back().get();
            arena_end = arena_cur + blk;
        }
        char* p = arena_cur;
        arena_cur += n;
        return p;
    }

public:
    StringBuilder()
        : parts(parts_), lens(lens_), count(0), cap(INLINE_CAP), total_len(0), cached(nullptr) {}

    ~StringBuilder() {
        if (parts != parts_) { delete[] parts; delete[] lens; }
    }

    StringBuilder(const StringBuilder& o)
        : parts(parts_), lens(lens_), count(0), cap(INLINE_CAP), total_len(0), cached(nullptr) {
        for (size_t i = 0; i < o.count; ++i)
            append_owned(o.parts[i], o.lens[i]);
    }
    StringBuilder& operator=(const StringBuilder& o) {
        if (this != &o) {
            if (parts != parts_) { delete[] parts; delete[] lens; }
            parts = parts_; lens = lens_; count = 0; cap = INLINE_CAP; total_len = 0; cached = nullptr;
            arena_blocks.clear(); arena_cur = nullptr; arena_end = nullptr;
            for (size_t i = 0; i < o.count; ++i)
                append_owned(o.parts[i], o.lens[i]);
        }
        return *this;
    }

    StringBuilder(StringBuilder&& o) noexcept
        : parts(o.parts == o.parts_ ? parts_ : o.parts),
          lens(o.lens == o.lens_ ? lens_ : o.lens),
          count(o.count), cap(o.cap),
          total_len(o.total_len), cached(o.cached),
          arena_blocks(std::move(o.arena_blocks)),
          arena_cur(o.arena_cur), arena_end(o.arena_end) {
        if (o.parts == o.parts_) {
            for (size_t i = 0; i < count; ++i) { parts_[i] = o.parts_[i]; lens_[i] = o.lens_[i]; }
        }
        o.parts = o.parts_; o.lens = o.lens_; o.count = 0; o.cap = INLINE_CAP;
        o.total_len = 0; o.cached = nullptr;
        o.arena_cur = nullptr; o.arena_end = nullptr;
    }

    StringBuilder& operator=(StringBuilder&& o) noexcept {
        if (parts != parts_) { delete[] parts; delete[] lens; }
        parts = o.parts == o.parts_ ? parts_ : o.parts;
        lens = o.lens == o.lens_ ? lens_ : o.lens;
        count = o.count; cap = o.cap;
        total_len = o.total_len; cached = o.cached;
        arena_blocks = std::move(o.arena_blocks);
        arena_cur = o.arena_cur; arena_end = o.arena_end;
        if (o.parts == o.parts_) {
            for (size_t i = 0; i < count; ++i) { parts_[i] = o.parts_[i]; lens_[i] = o.lens_[i]; }
        }
        o.parts = o.parts_; o.lens = o.lens_; o.count = 0; o.cap = INLINE_CAP;
        o.total_len = 0; o.cached = nullptr;
        o.arena_cur = nullptr; o.arena_end = nullptr;
        return *this;
    }

    CLX_INLINE_HOT void clear() {
        cached = nullptr;
        count = 0;
        total_len = 0;
        arena_cur = nullptr;
        arena_end = nullptr;
    }

    void reserve(size_t n) {
        if (n <= cap) return;
        auto* np = new const char*[n];
        auto* nl = new uint32_t[n];
        for (size_t i = 0; i < count; ++i) { np[i] = parts[i]; nl[i] = lens[i]; }
        if (parts != parts_) { delete[] parts; delete[] lens; }
        parts = np; lens = nl; cap = n;
    }

    CLX_INLINE StringBuilder& append(const char* s, uint32_t len) {
        cached = nullptr;
        if (count >= cap) grow();
        parts[count] = s; lens[count] = len; count++; total_len += len;
        return *this;
    }

    CLX_INLINE StringBuilder& append_owned(const char* s, uint32_t len) {
        char* p = arena_alloc(len);
        clx_memcpy(p, s, len);
        return append(p, len);
    }

    CLX_INLINE StringBuilder& append(StringBuilder& other) {
        cached = nullptr;
        for (size_t i = 0; i < other.count; ++i) append(other.parts[i], other.lens[i]);
        return *this;
    }

    CLX_INLINE StringBuilder& append(LState* L, const LValue& v) {
        if (v.type == ValueType::String) {
            if (v.val.payload.u64 >> 56) {
                const char* s = L->intern_string(v.as_string(), v.string_len());
                return append(s, v.string_len());
            }
            return append(v.as_string(), v.string_len());
        }
        if (v.type == ValueType::Int64) {
            char buf[32];
            size_t n = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "%lld",
                static_cast<long long>(v.val.payload.i64)));
            return append_owned(buf, static_cast<uint32_t>(n));
        }
        if (v.type == ValueType::Double) {
            char buf[32];
            size_t n = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "%.14g", v.val.payload.f64));
            return append_owned(buf, static_cast<uint32_t>(n));
        }
        if (v.type == ValueType::Nil) return append("nil", 3);
        if (v.type == ValueType::Boolean) return append(v.as_bool() ? "true" : "false", v.as_bool() ? 4 : 5);
        return append("(unknown)", 9);
    }

    const char* to_string(LState* L) const {
        if (cached) return cached;
        if (count == 0) { cached = L->intern_string("", 0); return cached; }
        uint32_t len32 = static_cast<uint32_t>(total_len);
        char* mem = new char[16 + total_len + 1]();
        clx_memcpy(mem + 8, &len32, 4);
        char* p = mem + 16;
        for (size_t i = 0; i < count; ++i) { clx_memcpy(p, parts[i], lens[i]); p += lens[i]; }
        *p = '\0';
        uint64_t h = total_len <= 8 ? swar_hash_8(mem + 16, total_len) : wyhash_str(mem + 16, total_len);
        clx_memcpy(mem, &h, 8);
        cached = L->string_pool.intern_preallocated(mem + 16, h, total_len);
        return cached;
    }

    CLX_INLINE_HOT size_t size() const { return total_len; }
    CLX_INLINE_HOT bool empty() const { return count == 0; }

    CLX_INLINE LValue to_lvalue(LState* L) const {
        if (count == 0) return LValue::istr("", 0);
        if (total_len <= 6) {
            char buf[8];
            char* p = buf;
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

//------------------ Per-function bump-pointer arena
#ifndef CLX_ARENA_DEFAULT_FIELDS
#define CLX_ARENA_DEFAULT_FIELDS 8
#endif

struct FuncArena {
    char*  base;
    char*  ptr;
    size_t capacity;
};

CLX_INLINE_HOT void arena_init(FuncArena* a, size_t size) {
    a->base = static_cast<char*>(std::malloc(size));
    a->ptr = a->base;
    a->capacity = size;
}

CLX_INLINE_HOT void* arena_alloc(FuncArena* a, size_t bytes, size_t align = 8) {
    uintptr_t current = reinterpret_cast<uintptr_t>(a->ptr);
    uintptr_t aligned = (current + align - 1) & ~(align - 1);
    a->ptr = reinterpret_cast<char*>(aligned + bytes);
    return reinterpret_cast<char*>(aligned);
}

CLX_INLINE_HOT void arena_reset(FuncArena* a) {
    if (a->base) std::free(a->base);
    a->base = nullptr;
    a->ptr = nullptr;
    a->capacity = 0;
}

CLX_INLINE_HOT LValue arena_create_table(LState* L, FuncArena* a, size_t asize, size_t hsize) {
#if CLX_ARENA_DEFAULT_FIELDS > 0
    if (asize < CLX_ARENA_DEFAULT_FIELDS) asize = CLX_ARENA_DEFAULT_FIELDS;
    if (hsize < CLX_ARENA_DEFAULT_FIELDS) hsize = CLX_ARENA_DEFAULT_FIELDS;
#endif
    size_t header_sz = ((sizeof(LTable) + 7) & ~static_cast<size_t>(7));
    size_t array_sz = asize > 0 ? ((sizeof(TValue) * asize + 7) & ~static_cast<size_t>(7)) : 0;
    size_t types_sz = asize > 0 ? ((sizeof(ValueType) * asize + 7) & ~static_cast<size_t>(7)) : 0;
    size_t hash_sz = hsize > 0 ? ((sizeof(HashEntry) * hsize + 7) & ~static_cast<size_t>(7)) : 0;
    size_t total = header_sz + array_sz + types_sz + hash_sz;
    char* mem = static_cast<char*>(arena_alloc(a, total));
    LTable* t = new (mem) LTable();
    t->is_arena = true;
    if (asize > 0) {
        t->array = reinterpret_cast<TValue*>(mem + header_sz);
        t->array_types = reinterpret_cast<ValueType*>(mem + header_sz + array_sz);
        t->array_cap = asize;
        for (size_t i = 0; i < asize; ++i) {
            t->array[i] = TValue();
            t->array_types[i] = Nil;
        }
    }
    if (hsize > 0) {
        t->entries = reinterpret_cast<HashEntry*>(mem + header_sz + array_sz + types_sz);
        t->hash_size = hsize;
        for (size_t i = 0; i < hsize; ++i) {
            t->entries[i].key.payload.u64 = HASH_EMPTY;
            t->entries[i].ktype = Nil;
            t->entries[i].val = TValue();
            t->entries[i].vtype = Nil;
        }
    }
    return LValue(Table, t);
}

//------------------ Close guard (for <close> vars)
struct CloseGuard {
    LState* L;
    LValue val;

    CLX_INLINE_HOT CloseGuard(LState* state, const LValue& v) : L(state), val(v) {}
    ~CloseGuard();
};

//------------------ Reads a variable from environment table (shared_ptr upvalue)
CLX_INLINE_HOT LValue get_env_var(LState* L, LUpValue env, const char* name) {
    LTable* t = static_cast<LTable*>((*env).val.payload.ptr);
    LValue key = LValue(L->intern_string(name));
    return t->gettable(key);
}

//------------------ Reads a variable from environment table (direct LValue)
CLX_INLINE_HOT LValue get_env_var(LState* L, const LValue& env, const char* name) {
    LTable* t = static_cast<LTable*>(env.val.payload.ptr);
    LValue key = LValue(L->intern_string(name));
    return t->gettable(key);
}

//------------------ Writes a variable to environment table (shared_ptr upvalue)
CLX_INLINE_HOT void set_env_var(LState* L, LUpValue env, const char* name, const LValue& val) {
    LTable* t = static_cast<LTable*>((*env).val.payload.ptr);
    LValue key = LValue(L->intern_string(name));
    t->settable(key, val);
}

//------------------ Writes a variable to environment table (direct LValue)
CLX_INLINE_HOT void set_env_var(LState* L, const LValue& env, const char* name, const LValue& val) {
    LTable* t = static_cast<LTable*>(env.val.payload.ptr);
    LValue key = LValue(L->intern_string(name));
    t->settable(key, val);
}

//------------------ Addition with metamethod fallback
  CLX_INLINE_HOT LValue add(LState* L, const LValue& a, const LValue& b) {
     if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]] {
         int64_t ai = a.val.payload.i64, bi = b.val.payload.i64;
         int64_t r = ai + bi;
         if (((ai ^ bi) >= 0) && ((ai ^ r) < 0))
             return LValue(static_cast<double>(ai) + static_cast<double>(bi));
         return LValue(r);
     }
     if ((a.type == ValueType::Double || a.type == ValueType::Int64) && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
         double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
         double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
         return LValue(l + r);
     }
     double l, r;
     if (a.to_number(l) && b.to_number(r)) return LValue(l + r);
     return call_bin_metamethod(L, a, b, "__add");
 }

 CLX_INLINE_HOT LValue sub(LState* L, const LValue& a, const LValue& b) {
     if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]] {
         int64_t ai = a.val.payload.i64, bi = b.val.payload.i64;
         int64_t r = ai - bi;
         if (((ai ^ bi) < 0) && ((bi ^ r) < 0))
             return LValue(static_cast<double>(ai) - static_cast<double>(bi));
         return LValue(r);
     }
     if ((a.type == ValueType::Double || a.type == ValueType::Int64) && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
         double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
         double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
         return LValue(l - r);
     }
     double l, r;
     if (a.to_number(l) && b.to_number(r)) return LValue(l - r);
     return call_bin_metamethod(L, a, b, "__sub");
 }

 CLX_INLINE_HOT LValue mul(LState* L, const LValue& a, const LValue& b) {
     if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]] {
         int64_t ai = a.val.payload.i64, bi = b.val.payload.i64;
         if (ai == 0 || bi == 0) return LValue(int64_t(0));
         if (ai == INT64_MIN && bi == -1)
             return LValue(static_cast<double>(ai) * static_cast<double>(bi));
         int64_t r = ai * bi;
         if (r / bi != ai)
             return LValue(static_cast<double>(ai) * static_cast<double>(bi));
         return LValue(r);
     }
     if ((a.type == ValueType::Double || a.type == ValueType::Int64) && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
         double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
         double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
         return LValue(l * r);
     }
     double l, r;
     if (a.to_number(l) && b.to_number(r)) return LValue(l * r);
     return call_bin_metamethod(L, a, b, "__mul");
 }

 CLX_INLINE_HOT LValue div(LState* L, const LValue& a, const LValue& b) {
     if ((a.type == ValueType::Double || a.type == ValueType::Int64) && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
         double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
         double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
         return LValue(l / r);
     }
     double l, r;
     if (a.to_number(l) && b.to_number(r)) return LValue(l / r);
     return call_bin_metamethod(L, a, b, "__div");
 }

//------------------ Equality with metamethod fallback
CLX_INLINE_HOT LValue eq(LState* L, const LValue& a, const LValue& b) {
    if (a.val.payload.u64 == b.val.payload.u64 && a.type == b.type)
        return LValue(true);
    if (a.type == ValueType::Int64 && b.type == ValueType::Double)
        return LValue(static_cast<double>(a.val.payload.i64) == b.val.payload.f64);
    if (a.type == ValueType::Double && b.type == ValueType::Int64)
        return LValue(a.val.payload.f64 == static_cast<double>(b.val.payload.i64));
    if (a.type == b.type) {
        if (a.type == ValueType::Int64) return LValue(a.val.payload.i64 == b.val.payload.i64);
        if (a.type == ValueType::Double) return LValue(a.val.payload.f64 == b.val.payload.f64);
        if (a.type == ValueType::String) {
            uint32_t al = a.string_len(), bl = b.string_len();
            return LValue(al == bl && clx_memcmp(a.as_string(), b.as_string(), al) == 0);
        }
        if (a.type == ValueType::Boolean) return LValue(a.val.payload.u64 == b.val.payload.u64);
        if (a.type == ValueType::Nil) return LValue(true);
        if (a.type == ValueType::Table || a.type == ValueType::UserData) {
            LTable* mt = (a.type == ValueType::Table)
                ? static_cast<LTable*>(a.as_pointer())->metatable
                : static_cast<LUserdata*>(a.as_pointer())->metatable;
            LTable* mt_b = (b.type == ValueType::Table)
                ? static_cast<LTable*>(b.as_pointer())->metatable
                : static_cast<LUserdata*>(b.as_pointer())->metatable;
            if (mt && mt == mt_b) {
                LValue mm = mt->gettable(LValue(L->intern_string("__eq")));
                if (mm.type != ValueType::Nil)
                    return call_bin_metamethod(L, a, b, "__eq");
            }
        }
        return LValue(a.val.payload.ptr == b.val.payload.ptr);
    }
    return call_bin_metamethod(L, a, b, "__eq");
}

//------------------ Safe integer conversion (no metamethods)
CLX_INLINE bool to_integer(const LValue& v, int64_t& out) {
    if (v.type == ValueType::Int64) {
        out = v.val.payload.i64;
        return true;
    }
    if (v.type == ValueType::Double) {
        double d = v.val.payload.f64;
        if (std::floor(d) == d) {
            out = static_cast<int64_t>(d);
            return true;
        }
    }
    return false;
}

//------------------ Bitwise AND with metamethod fallback
CLX_INLINE LValue band(LState* L, const LValue& a, const LValue& b) {
    int64_t l, r;
    if (to_integer(a, l) && to_integer(b, r)) [[likely]] return LValue(l & r);
    return call_bin_metamethod(L, a, b, "__band");
}

//------------------ Bitwise OR with metamethod fallback
CLX_INLINE LValue bor(LState* L, const LValue& a, const LValue& b) {
    int64_t l, r;
    if (to_integer(a, l) && to_integer(b, r)) [[likely]] return LValue(l | r);
    return call_bin_metamethod(L, a, b, "__bor");
}

//------------------ Bitwise XOR with metamethod fallback
CLX_INLINE LValue bxor(LState* L, const LValue& a, const LValue& b) {
    int64_t l, r;
    if (to_integer(a, l) && to_integer(b, r)) [[likely]] return LValue(l ^ r);
    return call_bin_metamethod(L, a, b, "__bxor");
}

//------------------ Bitwise SHL with metamethod fallback
CLX_INLINE LValue shl(LState* L, const LValue& a, const LValue& b) {
    int64_t l, r;
    if (to_integer(a, l) && to_integer(b, r)) [[likely]] return LValue(l << r);
    return call_bin_metamethod(L, a, b, "__shl");
}

//------------------ Bitwise SHR with metamethod fallback
CLX_INLINE LValue shr(LState* L, const LValue& a, const LValue& b) {
    int64_t l, r;
    if (to_integer(a, l) && to_integer(b, r)) [[likely]] return LValue(l >> r);
    return call_bin_metamethod(L, a, b, "__shr");
}

//------------------ Bitwise NOT with metamethod fallback
CLX_INLINE LValue bnot(LState* L, const LValue& a) {
    int64_t v;
    if (to_integer(a, v)) [[likely]] return LValue(~v);
    return call_bin_metamethod(L, a, a, "__bnot");
}

 CLX_INLINE LValue lt(LState* L, const LValue& a, const LValue& b) {
     if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]] return LValue(a.val.payload.i64 < b.val.payload.i64);
     if ((a.type == ValueType::Double || a.type == ValueType::Int64) && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
         double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
         double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
         return LValue(l < r);
     }
     if (a.type == ValueType::String && b.type == ValueType::String) return a < b;
     return call_bin_metamethod(L, a, b, "__lt");
 }

 CLX_INLINE LValue le(LState* L, const LValue& a, const LValue& b) {
     if (a.type == ValueType::Int64 && b.type == ValueType::Int64) [[likely]] return LValue(a.val.payload.i64 <= b.val.payload.i64);
     if ((a.type == ValueType::Double || a.type == ValueType::Int64) && (b.type == ValueType::Double || b.type == ValueType::Int64)) [[likely]] {
         double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
         double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
         return LValue(l <= r);
     }
     if (a.type == ValueType::String && b.type == ValueType::String) return a <= b;
     return call_bin_metamethod(L, a, b, "__le");
 }

//------------------ Length operator with metamethod fallback
CLX_INLINE_COLD LValue len(LState* L, const LValue& a) {
    if (a.type == ValueType::String) {
        return LValue(static_cast<int64_t>(a.string_len()));
    }

    if (a.type != ValueType::Table) return call_bin_metamethod(L, a, a, "__len");
    LTable* t = static_cast<LTable*>(a.as_pointer());

    if (t->metatable) {
        LValue mm = t->metatable->gettable(LValue(L->intern_string("__len", 5)));
        if (mm.type != ValueType::Nil) return call_bin_metamethod(L, a, a, "__len");
    }

    int64_t lo = 0;
    int64_t hi = static_cast<int64_t>(t->array_size) + 1;
    while (true) {
        LValue p = t->gettable(LValue(static_cast<int64_t>(hi)));
        if (p.type == ValueType::Nil) break;
        lo = hi;
        hi = (hi < 8) ? 8 : hi * 2;
    }
    while (hi - lo > 1) {
        int64_t mid = lo + (hi - lo) / 2;
        LValue p = t->gettable(LValue(static_cast<int64_t>(mid)));
        if (p.type != ValueType::Nil) lo = mid; else hi = mid;
    }
    return LValue(lo);
}

//------------------ Modulo with metamethod fallback
CLX_INLINE LValue mod(LState* L, const LValue& a, const LValue& b) {
    if ((a.type == ValueType::Double || a.type == ValueType::Int64) && (b.type == ValueType::Double || b.type == ValueType::Int64)) {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(l - std::floor(l / r) * r);
    }
    double l, r;
    if (a.to_number(l) && b.to_number(r)) return LValue(l - std::floor(l / r) * r);
    return call_bin_metamethod(L, a, b, "__mod");
}

//------------------ Floor-division with metamethod fallback
CLX_INLINE LValue idiv(LState* L, const LValue& a, const LValue& b) {
    if ((a.type == ValueType::Double || a.type == ValueType::Int64) && (b.type == ValueType::Double || b.type == ValueType::Int64)) {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(std::floor(l / r));
    }
    double l, r;
    if (a.to_number(l) && b.to_number(r)) return LValue(std::floor(l / r));
    return call_bin_metamethod(L, a, b, "__idiv");
}

//------------------ Power with metamethod fallback
CLX_INLINE LValue pow(LState* L, const LValue& a, const LValue& b) {
    if ((a.type == ValueType::Double || a.type == ValueType::Int64) && (b.type == ValueType::Double || b.type == ValueType::Int64)) {
        double l = (a.type == ValueType::Int64) ? static_cast<double>(a.val.payload.i64) : a.val.payload.f64;
        double r = (b.type == ValueType::Int64) ? static_cast<double>(b.val.payload.i64) : b.val.payload.f64;
        return LValue(std::pow(l, r));
    }
    double l, r;
    if (a.to_number(l) && b.to_number(r)) return LValue(std::pow(l, r));
    return call_bin_metamethod(L, a, b, "__pow");
}

//------------------ Unary minus with metamethod fallback
CLX_INLINE LValue unm(LState* L, const LValue& a) {
    if (a.type == ValueType::Int64) return LValue(-static_cast<double>(a.val.payload.i64));
    if (a.type == ValueType::Double) return LValue(-a.val.payload.f64);
    double d;
    if (a.to_number(d)) return LValue(-d);
    return call_bin_metamethod(L, a, a, "__unm");
}

//------------------ Logical NOT
CLX_INLINE LValue logical_not(const LValue& a) {
    return LValue(!a.as_bool());
}

//------------------ Multi-value concat (string builder fast path)
CLX_INLINE_COLD LValue concat_multi(LState* L, const LValue* args, size_t count) {
    if (count <= 8) [[likely]] {
        const char* ptrs[8];
        size_t  lens[8];
        size_t total_len = 0;
        for (size_t i = 0; i < count; ++i) {
            if (args[i].type != ValueType::String && args[i].type != ValueType::Int64 && args[i].type != ValueType::Double) {
                return call_bin_metamethod(L, args[i], args[i == count - 1 ? i - 1 : i + 1], "__concat");
            }
            if (args[i].type == ValueType::String) {
                ptrs[i] = args[i].as_string();
                lens[i] = args[i].string_len();
            } else {
                char buf[32];
                if (args[i].type == ValueType::Int64) {
                    lens[i] = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "%lld",
                        static_cast<long long>(args[i].val.payload.i64)));
                } else {
                    lens[i] = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "%.14g", args[i].val.payload.f64));
                }
                char* s = new char[lens[i]];
                clx_memcpy(s, buf, lens[i]);
                ptrs[i] = s;
            }
            total_len += lens[i];
        }
        if (total_len <= 6) {
            char buf[8];
            char* bp = buf;
            for (size_t i = 0; i < count; ++i) {
                clx_memcpy(bp, ptrs[i], lens[i]);
                bp += lens[i];
                if (args[i].type != ValueType::String) delete[] ptrs[i];
            }
            return LValue::istr(buf, static_cast<uint32_t>(total_len));
        }
        uint32_t len32 = static_cast<uint32_t>(total_len);
        char* mem = new char[16 + total_len + 1]();
        clx_memcpy(mem,     &len32, 4);
        clx_memcpy(mem + 8, &len32, 4);
        char* p = mem + 16;
        for (size_t i = 0; i < count; ++i) {
            clx_memcpy(p, ptrs[i], lens[i]);
            p += lens[i];
            if (args[i].type != ValueType::String) delete[] ptrs[i];
        }
        *p = '\0';
        uint64_t h = total_len <= 8 ? swar_hash_8(mem + 16, total_len) : wyhash_str(mem + 16, total_len);
        clx_memcpy(mem, &h, 8);
        const char* result = L->string_pool.intern_preallocated(mem + 16, h, total_len);
        return LValue(result);
    }

    size_t total_len = 0;
    std::vector<std::string> parts;
    parts.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (args[i].type != ValueType::String && args[i].type != ValueType::Int64 && args[i].type != ValueType::Double) {
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

//------------------ Creates a string LValue with inline encoding for short strings
CLX_INLINE LValue make_string(LState* L, const char* s, size_t len) {
    if (len <= 6) return LValue::istr(s, len);
    return LValue(L->intern_string(s, len));
}

CLX_INLINE LValue make_string(LState* L, const char* s) {
    size_t len = __builtin_strlen(s);
    if (len <= 6) return LValue::istr(s, len);
    return LValue(L->intern_string(s, len));
}

CLX_INLINE_HOT LValue make_string(LState* L, const std::string& s) {
    return make_string(L, s.data(), s.size());
}

//------------------ Creates a string LValue, trying pool lookup before heap allocation
CLX_INLINE LValue make_string_pooled(LState* L, const char* s, size_t len) {
    if (len <= 6) return LValue::istr(s, len);
    uint64_t h = len <= 8 ? swar_hash_8(s, len) : wyhash_str(s, len);
    if (const char* hit = L->string_pool.lookup(s, len, h))
        return LValue(hit);
    return LValue(L->intern_string(s, len));
}

//------------------ Table read with __index fallback
CLX_INLINE_HOT LValue table_get(LState* L, const LValue& obj, const LValue& key) {
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
            direct = t->get_value(L, key);
        }
        if (direct.type != ValueType::Nil) return direct;
        mt = t->metatable;
    } else if (obj.type == ValueType::UserData) {
        LUserdata* ud = static_cast<LUserdata*>(obj.as_pointer());
        mt = ud->metatable;
    } else if (obj.type == ValueType::String) {
        mt = L->string_metatable;
        if (!mt) return LValue();
        LValue index = mt->gettable(LValue(L->intern_string("__index")));
        if (index.type == ValueType::Nil) return LValue();
        if (index.type == ValueType::Table)
            return table_get(L, index, key);
        if (index.type == ValueType::Function) {
            LValue args[2] = { obj, key };
            size_t prev = L->shadow_top;
            L->shadow_stack[L->shadow_top++] = {&args[0].val, &args[0].type};
            L->shadow_stack[L->shadow_top++] = {&args[1].val, &args[1].type};
            MultiValue mv = call_function(L, index, args, 2, "__index", 0);
            L->shadow_top = prev;
            return mv.count > 0 ? mv[0] : LValue();
        }
        return LValue();
    } else {
        return LValue();
    }

    if (!mt) return LValue();
    LValue index = mt->gettable(LValue(L->intern_string("__index")));
    if (index.type == ValueType::Nil) return LValue();
    if (index.type == ValueType::Function) {
        LValue args[2] = { obj, key };
        size_t prev = L->shadow_top;
        L->shadow_stack[L->shadow_top++] = {&args[0].val, &args[0].type};
        L->shadow_stack[L->shadow_top++] = {&args[1].val, &args[1].type};
        MultiValue mv = call_function(L, index, args, 2, "__index", 0);
        L->shadow_top = prev;
        return mv.count > 0 ? mv[0] : LValue();
    }
    if (index.type == ValueType::Table)
        return table_get(L, index, key);
    return LValue();
}

//------------------ Table write with __newindex fallback
CLX_INLINE_HOT void table_set(LState* L, const LValue& obj, const LValue& key, const LValue& val) {
    LTable* mt = nullptr;

    if (obj.type == ValueType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        if (key.type == ValueType::Int64) {
            int64_t k = key.val.payload.i64;
            if (static_cast<uint64_t>(k - 1) < t->array_cap) {
                t->array[k - 1] = val.val;
                t->array_types[k - 1] = val.type;
                if (static_cast<size_t>(k) > t->array_size) t->array_size = static_cast<size_t>(k);
                return;
            }
        } else if (key.type == ValueType::Double) {
            double d = key.val.payload.f64;
            int64_t idx = static_cast<int64_t>(d);
            if (d == static_cast<double>(idx) && static_cast<uint64_t>(idx - 1) < t->array_cap) {
                t->array[idx - 1] = val.val;
                t->array_types[idx - 1] = val.type;
                if (static_cast<size_t>(idx) > t->array_size) t->array_size = static_cast<size_t>(idx);
                return;
            }
        }
        t->set_value(L, key, val);
        return;
    } else if (obj.type == ValueType::UserData) {
        LUserdata* ud = static_cast<LUserdata*>(obj.as_pointer());
        mt = ud->metatable;
        if (!mt) return;
        LValue newindex = mt->gettable(LValue(L->intern_string("__newindex")));
        if (newindex.type == ValueType::Nil) return;
        if (newindex.type == ValueType::Function) {
            LValue args[3] = { obj, key, val };
            size_t prev = L->shadow_top;
            L->shadow_stack[L->shadow_top++] = {&args[0].val, &args[0].type};
            L->shadow_stack[L->shadow_top++] = {&args[1].val, &args[1].type};
            L->shadow_stack[L->shadow_top++] = {&args[2].val, &args[2].type};
            call_function(L, newindex, args, 3, "__newindex", 0);
            L->shadow_top = prev;
        } else if (newindex.type == ValueType::Table) {
            table_set(L, newindex, key, val);
        }
        return;
    }
}

//------------------ Integer-key table read (fast path)
CLX_INLINE_HOT LValue table_get_int(LState* L, const LValue& obj, size_t idx) {
    LTable* mt = nullptr;
    LValue key_val = LValue(static_cast<int64_t>(idx));

    if (obj.type == ValueType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        if (idx - 1 < t->array_cap) return LValue(t->array[idx - 1], t->array_types[idx - 1]);
        LValue result = t->get_value(L, key_val);
        if (result.type != ValueType::Nil) return result;
        mt = t->metatable;
    } else if (obj.type == ValueType::UserData) {
        LUserdata* ud = static_cast<LUserdata*>(obj.as_pointer());
        mt = ud->metatable;
    } else {
        return LValue();
    }

    if (!mt) return LValue();
    LValue index = mt->gettable(LValue(L->intern_string("__index")));
    if (index.type == ValueType::Nil) return LValue();
    if (index.type == ValueType::Function) {
        LValue args[2] = { obj, key_val };
        size_t prev = L->shadow_top;
        L->shadow_stack[L->shadow_top++] = {&args[0].val, &args[0].type};
        L->shadow_stack[L->shadow_top++] = {&args[1].val, &args[1].type};
        MultiValue mv = call_function(L, index, args, 2, "__index", 0);
        L->shadow_top = prev;
        return mv.count > 0 ? mv[0] : LValue();
    }
    if (index.type == ValueType::Table)
        return table_get(L, index, key_val);
    return LValue();
}

//------------------ Integer-key table write (fast path)
CLX_INLINE_HOT void table_set_int(LState* L, const LValue& obj, size_t idx, const LValue& val) {
    LValue key_val = LValue(static_cast<int64_t>(idx));

    if (obj.type == ValueType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        if (idx - 1 < t->array_cap) {
            t->array[idx - 1] = val.val;
            t->array_types[idx - 1] = val.type;
            if (static_cast<size_t>(idx) > t->array_size) t->array_size = idx;
            return;
        }
        t->set_value(L, key_val, val);
        return;
    }

    if (obj.type == ValueType::UserData) {
        LUserdata* ud = static_cast<LUserdata*>(obj.as_pointer());
        if (!ud->metatable) return;
        LValue newindex = ud->metatable->gettable(LValue(L->intern_string("__newindex")));
        if (newindex.type == ValueType::Nil) return;
        if (newindex.type == ValueType::Function) {
            LValue args[3] = { obj, key_val, val };
            size_t prev = L->shadow_top;
            L->shadow_stack[L->shadow_top++] = {&args[0].val, &args[0].type};
            L->shadow_stack[L->shadow_top++] = {&args[1].val, &args[1].type};
            L->shadow_stack[L->shadow_top++] = {&args[2].val, &args[2].type};
            call_function(L, newindex, args, 3, "__newindex", 0);
            L->shadow_top = prev;
        } else if (newindex.type == ValueType::Table) {
            table_set(L, newindex, key_val, val);
        }
    }
}

//------------------ Direct table write (skips existence check / metatable)
CLX_INLINE_HOT void table_set_direct(LState* L, const LValue& obj, const LValue& key, const LValue& val) {
    if (obj.type == ValueType::Table) {
        LTable* t = static_cast<LTable*>(obj.as_pointer());
        t->settable(key, val);
        return;
    }
    table_set(L, obj, key, val);
}

//------------------ Direct table arithmetic: t[k] = t[k] op amount
template<typename Op, typename NilCase>
CLX_INLINE_HOT void table_op(LState* L, const LValue& obj, const LValue& key, double amount, Op op, NilCase nil_case, LValue (*fallback)(LState*, const LValue&, const LValue&)) {
    if (obj.type == ValueType::Table) {
        LTable* t = static_cast<LTable*>(obj.val.payload.ptr);
        LValue val = t->gettable(key);
        if (val.type == ValueType::Nil) { t->settable(key, nil_case(amount)); return; }
        if (val.type == ValueType::Double) { t->settable(key, LValue(op(val.val.payload.f64, amount))); return; }
        if (val.type == ValueType::Int64) { t->settable(key, LValue(op(static_cast<double>(val.val.payload.i64), amount))); return; }
    }
    table_set(L, obj, key, fallback(L, table_get(L, obj, key), LValue(amount)));
}

CLX_INLINE_HOT void table_increment(LState* L, const LValue& obj, const LValue& key, double amount) {
    table_op(L, obj, key, amount, [](double a, double b) { return a + b; }, [](double b) { return LValue(b); }, add);
}
CLX_INLINE_HOT void table_decrement(LState* L, const LValue& obj, const LValue& key, double amount) {
    table_op(L, obj, key, amount, [](double a, double b) { return a - b; }, [](double b) { return LValue(-b); }, sub);
}
CLX_INLINE_HOT void table_multiply(LState* L, const LValue& obj, const LValue& key, double amount) {
    table_op(L, obj, key, amount, [](double a, double b) { return a * b; }, [](double b) { return LValue(0.0); }, mul);
}
CLX_INLINE_HOT void table_divide(LState* L, const LValue& obj, const LValue& key, double amount) {
    table_op(L, obj, key, amount, [](double a, double b) { return a / b; }, [](double b) { return LValue(0.0 / b); }, div);
}

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

MultiValue table_concat(LState*, const LValue*, size_t);
MultiValue table_insert(LState*, const LValue*, size_t);
MultiValue table_remove(LState*, const LValue*, size_t);
MultiValue table_sort(LState*, const LValue*, size_t);
MultiValue table_pack(LState*, const LValue*, size_t);
MultiValue table_unpack(LState*, const LValue*, size_t);
MultiValue table_move(LState*, const LValue*, size_t);

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
//------------------ Closes a coroutine
MultiValue close_thread(LState* L, const LValue& thread);

//------------------ Opens CLX state
LState* open(int argc = 0, char* argv[] = nullptr);
//------------------ Opens all standard libraries
void openlibs(LState* L);
//------------------ Closes CLX state
void close(LState* L);

//------------------ Sets lazy-initialized functions on a table
void set_lazy_funcs(LState* L, const LValue& table, const LazyReg* regs, size_t count);

}

#endif
