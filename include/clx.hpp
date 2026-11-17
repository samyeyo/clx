// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  clx.hpp · Object-oriented C++20 API        │
// └─────────────────────────────────────────────┘
//
//  A value-semantic, GC-safe object layer on top of clx.h:
//
//      namespace lua = clx::lua;                      // do NOT `using namespace clx;` together with
//                                                     // `using namespace clx::lua;` (clx::Table & co.
//                                                     // are ValueType constants and would collide)
//      lua::State L;                                  // opens clx + std libs, becomes the current state
//
//      lua::Table cfg{ {"name", "clx"}, {"debug", true}, 1, 2, 3 };    // Lua-style constructor
//      cfg["answer"] = 42;                                              // proxy assignment
//      cfg["greet"]  = [](std::string who) { return "hello " + who; };  // C++ callable -> Lua function
//
//      lua::String s = "Hello, world";
//      auto up   = s.upper();                         // string module as methods
//      auto [r, n] = s.gsub("o", "0");
//      cfg.insert("x");  cfg.sort();  cfg.concat(", ");   // table module as methods
//
//      for (auto [k, v] : cfg) { ... }                // pairs()
//      for (auto [i, v] : cfg.ipairs()) { ... }       // ipairs()
//
//      lua::Number n = lua::Number(7) / 2;            // 3.5 (Lua semantics: int / int -> float)
//      auto r = L["print"]("n =", n);                 // call a global
//
//  Lifetime rules
//      * Every object that refers to a Lua GC value (Table, Function, Thread, UserData) registers
//        itself as a GC root while alive (copies root again, moves transfer), so locals are always safe.
//      * All objects must be destroyed BEFORE the State that created them.
//      * Objects have reference semantics for Lua values: copying a Table copies the handle, not the table.
//      * String::view()/c_str() point into the String object (short strings are stored inline in the
//        handle): keep the String alive, and do not move it while using the view.

#ifndef CLX_HPP
#define CLX_HPP

#include "clx.h"

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ostream>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if __has_include(<format>)
#include <format>
#endif

namespace clx::lua {

//------------------ Forward declarations
class Value;
class Proxy;
class Results;
class Number;
class Boolean;
class String;
class Table;
class Function;
class Thread;
class UserData;
class State;
struct Nil;
struct PResult;

//------------------ Customization point: specialize to teach the API about your own types
//------------------     template<> struct clx::lua::Convert<Vec2> {
//------------------         static LValue to_lua(LState* L, const Vec2& v);
//------------------         static Vec2   from_lua(LState* L, const LValue& v, int argn);
//------------------     };
template<class T> struct Convert;

//------------------ Error type thrown by the runtime (catch this around calls into Lua)
using Error = clx::LRuntimeException;

//=====================================================================================================
//  Nil / Boolean
//=====================================================================================================

//------------------ Nil tag: `v = lua::nil;`, `f(lua::nil)`
struct Nil {
    friend constexpr bool operator==(Nil, Nil) noexcept { return true; }
};

inline constexpr Nil nil { };

//------------------ Boolean: constexpr wrapper with Lua truthiness helpers
class Boolean {
public:
    constexpr Boolean() noexcept = default;
    constexpr Boolean(bool b) noexcept
        : b_(b) { }

    constexpr explicit operator bool() const noexcept { return b_; }
    constexpr bool value() const noexcept { return b_; }
    constexpr Boolean operator!() const noexcept { return Boolean(!b_); }

    friend constexpr bool operator==(Boolean a, Boolean b) noexcept { return a.b_ == b.b_; }

    LValue raw() const noexcept { return LValue(b_); }

private:
    bool b_ = false;
};

//=====================================================================================================
//  Number: Lua 5.4 number (int64 | double) with exact Lua arithmetic/comparison semantics, constexpr
//=====================================================================================================
class Number {
public:
    constexpr Number() noexcept
        : i_(0)
        , is_int_(true) { }

    template<std::integral T>
        requires(!std::same_as<T, bool>)
    constexpr Number(T v) noexcept
        : i_(static_cast<int64_t>(v))
        , is_int_(true) { }

    template<std::floating_point T>
    constexpr Number(T v) noexcept
        : d_(static_cast<double>(v))
        , is_int_(false) { }

    //------------------ Builds a Number from a raw numeric LValue (Int64 or Double)
    static Number from_raw(const LValue& v) noexcept {
        return v.type == ValueType::Int64 ? Number(v.as_integer()) : Number(v.as_number());
    }

    LValue raw() const noexcept { return is_int_ ? LValue(i_) : LValue(d_); }

    constexpr bool is_integer() const noexcept { return is_int_; }
    constexpr bool is_float() const noexcept { return !is_int_; }

    //------------------ Value as double (always succeeds)
    constexpr double to_double() const noexcept { return is_int_ ? static_cast<double>(i_) : d_; }

    //------------------ Value as integer when it has an exact integer representation
    constexpr std::optional<int64_t> to_integer() const noexcept {
        if (is_int_)
            return i_;
        if (d_ >= -9223372036854775808.0 && d_ < 9223372036854775808.0) {
            int64_t t = static_cast<int64_t>(d_);
            if (static_cast<double>(t) == d_)
                return t;
        }
        return std::nullopt;
    }

    //------------------ Truncating accessors (no error)
    constexpr int64_t integer() const noexcept { return is_int_ ? i_ : static_cast<int64_t>(d_); }
    constexpr double number() const noexcept { return to_double(); }

    template<class T>
        requires(std::is_arithmetic_v<T> && !std::same_as<T, bool>)
    constexpr explicit operator T() const noexcept {
        if constexpr (std::is_integral_v<T>)
            return static_cast<T>(integer());
        else
            return static_cast<T>(to_double());
    }

    //------------------ Lua-compatible text ("3", "3.5", "1e+100")
    std::string to_string() const {
        if (is_int_)
            return std::to_string(i_);
        char buf[64];
        int n = clx::clx_format_double(buf, sizeof(buf), d_);
        return std::string(buf, static_cast<size_t>(n));
    }

    //------------------ math helpers (Lua semantics: floor/ceil give an integer when it fits)
    Number abs() const noexcept {
        if (is_int_)
            return Number(i_ < 0 ? wneg(i_) : i_);
        return Number(d_ < 0 ? -d_ : d_);
    }
    Number floor() const noexcept { return is_int_ ? *this : fit(std::floor(d_)); }
    Number ceil() const noexcept { return is_int_ ? *this : fit(std::ceil(d_)); }
    Number sqrt() const noexcept { return Number(std::sqrt(to_double())); }

    //------------------ Arithmetic (wrapping integers, float contagion, Lua floor-mod / floor-div)
    friend constexpr Number operator+(Number a, Number b) noexcept {
        if (a.is_int_ && b.is_int_)
            return Number(wadd(a.i_, b.i_));
        return Number(a.to_double() + b.to_double());
    }
    friend constexpr Number operator-(Number a, Number b) noexcept {
        if (a.is_int_ && b.is_int_)
            return Number(wsub(a.i_, b.i_));
        return Number(a.to_double() - b.to_double());
    }
    friend constexpr Number operator*(Number a, Number b) noexcept {
        if (a.is_int_ && b.is_int_)
            return Number(wmul(a.i_, b.i_));
        return Number(a.to_double() * b.to_double());
    }
    friend constexpr Number operator/(Number a, Number b) noexcept { return Number(a.to_double() / b.to_double()); }

    friend Number operator%(Number a, Number b) {
        if (a.is_int_ && b.is_int_) {
            if (b.i_ == 0)
                clx::throw_runtime_error("attempt to perform 'n%%0'");
            if (b.i_ == -1)
                return Number(int64_t(0));
            int64_t m = a.i_ % b.i_;
            if (m != 0 && (m ^ b.i_) < 0)
                m += b.i_;
            return Number(m);
        }
        double x = a.to_double(), y = b.to_double();
        double m = std::fmod(x, y);
        if ((m > 0) ? y < 0 : (m < 0 && y != m))
            m += y;
        return Number(m);
    }

    //------------------ Floor division (Lua `//`)
    static Number idiv(Number a, Number b) {
        if (a.is_int_ && b.is_int_) {
            if (b.i_ == 0)
                clx::throw_runtime_error("attempt to perform 'n//0'");
            if (b.i_ == -1)
                return Number(wneg(a.i_));
            int64_t q = a.i_ / b.i_;
            if ((a.i_ ^ b.i_) < 0 && a.i_ % b.i_ != 0)
                --q;
            return Number(q);
        }
        return Number(std::floor(a.to_double() / b.to_double()));
    }

    //------------------ Exponentiation (Lua `^`): always a float
    static Number pow(Number a, Number b) noexcept { return Number(std::pow(a.to_double(), b.to_double())); }

    friend constexpr Number operator-(Number a) noexcept {
        return a.is_int_ ? Number(wneg(a.i_)) : Number(-a.d_);
    }
    friend constexpr Number operator+(Number a) noexcept { return a; }

    //------------------ Bitwise (operands must have an integer representation, like Lua)
    friend Number operator&(Number a, Number b) { return Number(bits(a) & bits(b)); }
    friend Number operator|(Number a, Number b) { return Number(bits(a) | bits(b)); }
    friend Number operator^(Number a, Number b) { return Number(bits(a) ^ bits(b)); }
    friend Number operator~(Number a) { return Number(~bits(a)); }
    friend Number operator<<(Number a, Number b) { return Number(shift_left(bits(a), bits(b))); }
    friend Number operator>>(Number a, Number b) { return Number(shift_left(bits(a), wneg(bits(b)))); }

    Number& operator+=(Number o) noexcept { return *this = *this + o; }
    Number& operator-=(Number o) noexcept { return *this = *this - o; }
    Number& operator*=(Number o) noexcept { return *this = *this * o; }
    Number& operator/=(Number o) noexcept { return *this = *this / o; }
    Number& operator%=(Number o) { return *this = *this % o; }
    Number& operator++() noexcept { return *this += Number(1); }
    Number& operator--() noexcept { return *this -= Number(1); }
    Number operator++(int) noexcept {
        Number t = *this;
        ++*this;
        return t;
    }
    Number operator--(int) noexcept {
        Number t = *this;
        --*this;
        return t;
    }

    //------------------ Exact comparison: integers and floats compare by mathematical value
    friend constexpr std::partial_ordering operator<=>(Number a, Number b) noexcept {
        if (a.is_int_ && b.is_int_)
            return a.i_ <=> b.i_;
        if (!a.is_int_ && !b.is_int_)
            return a.d_ <=> b.d_;
        if (a.is_int_)
            return cmp_int_float(a.i_, b.d_);
        return 0 <=> cmp_int_float(b.i_, a.d_);
    }
    friend constexpr bool operator==(Number a, Number b) noexcept { return (a <=> b) == 0; }

private:
    static constexpr int64_t wadd(int64_t a, int64_t b) noexcept {
        return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
    }
    static constexpr int64_t wsub(int64_t a, int64_t b) noexcept {
        return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
    }
    static constexpr int64_t wmul(int64_t a, int64_t b) noexcept {
        return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
    }
    static constexpr int64_t wneg(int64_t a) noexcept { return static_cast<int64_t>(0ULL - static_cast<uint64_t>(a)); }

    static Number fit(double d) noexcept {
        if (d >= -9223372036854775808.0 && d < 9223372036854775808.0)
            return Number(static_cast<int64_t>(d));
        return Number(d);
    }

    static int64_t bits(Number n) {
        if (auto i = n.to_integer())
            return *i;
        clx::throw_runtime_error("number has no integer representation");
    }

    static constexpr int64_t shift_left(int64_t a, int64_t n) noexcept {
        if (n <= -64 || n >= 64)
            return 0;
        if (n >= 0)
            return static_cast<int64_t>(static_cast<uint64_t>(a) << n);
        return static_cast<int64_t>(static_cast<uint64_t>(a) >> (-n));
    }

    //------------------ Exact int-vs-float ordering (NaN is unordered)
    static constexpr std::partial_ordering cmp_int_float(int64_t i, double d) noexcept {
        if (d != d)
            return std::partial_ordering::unordered;
        if (d >= 9223372036854775808.0)
            return std::partial_ordering::less;
        if (d < -9223372036854775808.0)
            return std::partial_ordering::greater;
        int64_t t = static_cast<int64_t>(d);
        if (static_cast<double>(t) > d)
            --t;
        if (i < t)
            return std::partial_ordering::less;
        if (i > t)
            return std::partial_ordering::greater;
        return (static_cast<double>(t) == d) ? std::partial_ordering::equivalent : std::partial_ordering::less;
    }

    union {
        int64_t i_;
        double d_;
    };
    bool is_int_;
};

//=====================================================================================================
//  Conversion machinery (C++ <-> Lua)
//=====================================================================================================
namespace detail {

    //------------------ Per-thread "current state": lets objects be built without passing LState*
    inline thread_local LState* g_current = nullptr;

    [[noreturn]] CLX_INLINE_COLD void no_state() {
        clx::throw_runtime_error("clx::lua: no active lua::State (create a lua::State or lua::Scope first)");
    }

    CLX_INLINE_HOT LState* current() {
        if (!g_current)
            no_state();
        return g_current;
    }

    //------------------ Tag used by derived classes to adopt an already-built raw value
    struct adopt_t {
        explicit adopt_t() = default;
    };
    inline constexpr adopt_t adopt { };

    //------------------ Call-site descriptor reported in error messages raised through this API
    inline constexpr CallSite api_site = { "C++ API", 0, false, false };

    inline const LValue nil_value { };

    //------------------ Raises "bad argument #n (X expected, got Y)" (or "X expected, got Y" for argn == 0)
    [[noreturn]] CLX_INLINE_COLD void bad_type(LState* L, int argn, const char* expected, const LValue& v) {
        std::string msg;
        if (argn > 0)
            msg = "bad argument #" + std::to_string(argn) + " (" + expected + " expected, got "
                + clx::stock_type_name(v) + ")";
        else
            msg = std::string(expected) + " expected, got " + clx::stock_type_name(v);
        clx::error(L, msg.c_str());
    }

    //------------------ Type-list helper
    template<class... T> struct type_list { };

    template<class T> struct is_optional : std::false_type { };
    template<class T> struct is_optional<std::optional<T>> : std::true_type { };

    template<class T> struct is_vector : std::false_type { };
    template<class T, class A> struct is_vector<std::vector<T, A>> : std::true_type { };

    template<class T> struct is_tuple_like : std::false_type { };
    template<class... T> struct is_tuple_like<std::tuple<T...>> : std::true_type { };
    template<class A, class B> struct is_tuple_like<std::pair<A, B>> : std::true_type { };

    //------------------ A C++ callable we can expose as a Lua function
    template<class U>
    concept RawSignature = std::is_invocable_r_v<MultiValue, U&, LState*, const LValue*, size_t>;

    template<class U>
    concept CallableObject = RawSignature<U> || (std::is_pointer_v<U> && std::is_function_v<std::remove_pointer_t<U>>)
        || (std::is_class_v<U> && requires { &U::operator(); });

    //------------------ How a C++ type maps onto a Lua value
    enum class Kind {
        none,
        value,
        proxy,
        raw,
        nil,
        boolean_obj,
        number,
        boolean,
        integral,
        floating,
        enumeration,
        optional,
        text,
        callable,
        range
    };

    template<class U> consteval Kind kind_of() noexcept {
        if constexpr (std::derived_from<U, Value>)
            return Kind::value;
        else if constexpr (std::same_as<U, Proxy>)
            return Kind::proxy;
        else if constexpr (std::same_as<U, LValue>)
            return Kind::raw;
        else if constexpr (std::same_as<U, Nil> || std::same_as<U, std::nullptr_t>)
            return Kind::nil;
        else if constexpr (std::same_as<U, Boolean>)
            return Kind::boolean_obj;
        else if constexpr (std::same_as<U, Number>)
            return Kind::number;
        else if constexpr (std::same_as<U, bool>)
            return Kind::boolean;
        else if constexpr (std::integral<U>)
            return Kind::integral;
        else if constexpr (std::floating_point<U>)
            return Kind::floating;
        else if constexpr (std::is_enum_v<U>)
            return Kind::enumeration;
        else if constexpr (is_optional<U>::value)
            return kind_of<typename U::value_type>() != Kind::none ? Kind::optional : Kind::none;
        else if constexpr (std::is_convertible_v<const U&, std::string_view>)
            return Kind::text;
        else if constexpr (CallableObject<U>)
            return Kind::callable;
        else if constexpr (std::ranges::input_range<U>) {
            using E = std::remove_cvref_t<std::ranges::range_reference_t<U>>;
            if constexpr (requires(const E& e) { e.first; e.second; })
                return (kind_of<std::remove_cvref_t<decltype(std::declval<const E&>().first)>>() != Kind::none
                           && kind_of<std::remove_cvref_t<decltype(std::declval<const E&>().second)>>() != Kind::none)
                    ? Kind::range
                    : Kind::none;
            else
                return kind_of<E>() != Kind::none ? Kind::range : Kind::none;
        } else if constexpr (requires(LState* L, const U& x) {
                                 { Convert<U>::to_lua(L, x) } -> std::convertible_to<LValue>;
                             })
            return Kind::raw;
        else
            return Kind::none;
    }

    //------------------ Kinds that carry no state (no LState needed to build them)
    template<class U> consteval bool stateless() noexcept {
        constexpr Kind k = kind_of<U>();
        return k == Kind::nil || k == Kind::boolean_obj || k == Kind::number || k == Kind::boolean
            || k == Kind::integral || k == Kind::floating || k == Kind::enumeration;
    }

    template<class U> consteval bool can_from() noexcept {
        if constexpr (std::derived_from<U, Value>)
            return true;
        else if constexpr (std::same_as<U, LValue> || std::same_as<U, bool> || std::integral<U>
            || std::floating_point<U> || std::is_enum_v<U> || std::same_as<U, Number> || std::same_as<U, Boolean>
            || std::same_as<U, Nil> || std::same_as<U, std::string> || std::same_as<U, std::string_view>
            || std::same_as<U, const char*>)
            return true;
        else if constexpr (is_optional<U>::value || is_vector<U>::value)
            return can_from<typename U::value_type>();
        else
            return requires(LState* L, const LValue& v, int n) {
                { Convert<U>::from_lua(L, v, n) } -> std::convertible_to<U>;
            };
    }

} // namespace detail

//------------------ Concepts
template<class T>
concept ToLua = detail::kind_of<std::decay_t<T>>() != detail::Kind::none;

template<class T>
concept FromLua = detail::can_from<std::remove_cv_t<T>>();

template<class T>
concept ValueLike = std::derived_from<std::decay_t<T>, Value> || std::same_as<std::decay_t<T>, Proxy>;

template<class A, class B>
concept BinaryOperands = (ValueLike<A> || ValueLike<B>) && ToLua<A> && ToLua<B>;

//------------------ Conversion function declarations (defined at the end of the header)
template<class T> LValue to_lvalue(LState* L, T&& x);
template<class T> T from_lvalue(LState* L, const LValue& v, int argn = 0);

//------------------ RAII "current state" override, used while Lua calls into C++ callbacks
class Scope {
public:
    explicit Scope(LState* L) noexcept
        : prev_(detail::g_current) {
        detail::g_current = L;
    }
    ~Scope() { detail::g_current = prev_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    LState* prev_;
};

namespace detail {

    //------------------ The per-state registry table, created on first use: the runtime defers it to
    //------------------ debug.getregistry(), so a fresh state that never touches the debug library has none
    inline LTable* registry(LState* L) {
        if (!L->registry) {
            LValue r = L->create_table(0, 4);
            L->registry = static_cast<LTable*>(r.as_pointer());
            L->permanent_roots.push_back(r);
        }
        return L->registry;
    }

    //------------------ Fixed argument buffer that roots itself for the duration of a call
    template<size_t N> struct RawArgs {
        static constexpr size_t cap = N ? N : 1;
        LState* L;
        LValue a[cap];
        size_t n = 0;
        RootRange rr;

        explicit RawArgs(LState* s)
            : L(s)
            , rr(s, a, cap) { }

        template<class T> RawArgs& push(T&& x) {
            a[n++] = to_lvalue(L, std::forward<T>(x));
            return *this;
        }
    };

} // namespace detail

//=====================================================================================================
//  Value: any Lua value
//=====================================================================================================
class Value {
public:
    //------------------ nil
    Value() = default;

    Value(const Value& o)
        : L_(o.L_)
        , v_(o.v_) {
        root();
    }

    Value(Value&& o) noexcept
        : L_(o.L_)
        , v_(o.v_) {
        o.L_ = nullptr;
        o.v_ = LValue();
    }

    Value& operator=(const Value& o) {
        if (this != &o) {
            Value tmp(o);
            swap(*this, tmp);
        }
        return *this;
    }

    Value& operator=(Value&& o) noexcept {
        if (this != &o) {
            unroot();
            L_ = o.L_;
            v_ = o.v_;
            o.L_ = nullptr;
            o.v_ = LValue();
        }
        return *this;
    }

    ~Value() { unroot(); }

    //------------------ Implicit construction from anything convertible (bool, ints, floats, strings, tables-from-ranges,
    //------------------ C++ callables, optional<T>, ...). Uses the current State where one is needed.
    template<class T>
        requires(!std::derived_from<std::decay_t<T>, Value> && ToLua<T>)
    Value(T&& x)
        : L_(state_for(x))
        , v_(to_lvalue(L_, std::forward<T>(x))) {
        root();
    }

    //------------------ Builds a Value in an explicit state
    template<class T> static Value make(LState* L, T&& x) {
        Value r;
        r.L_ = L;
        r.v_ = to_lvalue(L, std::forward<T>(x));
        r.root();
        return r;
    }

    //------------------ Wraps a raw LValue (rooted from now on)
    static Value from_raw(LState* L, const LValue& v) { return Value(L, v); }

    friend void swap(Value& a, Value& b) noexcept {
        std::swap(a.L_, b.L_);
        std::swap(a.v_, b.v_);
    }

    //------------------ Raw access
    CLX_INLINE_HOT const LValue& raw() const noexcept { return v_; }
    LState* state() const { return L_ ? L_ : detail::current(); }
    const void* id() const noexcept { return clx::topointer(v_); }

    //------------------ Type inspection
    CLX_INLINE_HOT ValueType type() const noexcept { return v_.type; }
    const char* type_name() const noexcept { return clx::stock_type_name(v_); }

    CLX_INLINE_HOT bool is_nil() const noexcept { return v_.type == ValueType::Nil; }
    CLX_INLINE_HOT bool is_boolean() const noexcept { return v_.type == ValueType::Boolean; }
    CLX_INLINE_HOT bool is_number() const noexcept { return clx::is_number(v_); }
    CLX_INLINE_HOT bool is_integer() const noexcept { return v_.type == ValueType::Int64; }
    CLX_INLINE_HOT bool is_float() const noexcept { return v_.type == ValueType::Double; }
    CLX_INLINE_HOT bool is_string() const noexcept { return v_.type == ValueType::String; }
    CLX_INLINE_HOT bool is_table() const noexcept { return v_.type == ValueType::Table; }
    CLX_INLINE_HOT bool is_function() const noexcept { return v_.type == ValueType::Function; }
    CLX_INLINE_HOT bool is_userdata() const noexcept { return v_.type == ValueType::UserData; }
    CLX_INLINE_HOT bool is_thread() const noexcept { return v_.type == ValueType::Thread; }

    //------------------ Lua truthiness: everything except nil and false
    CLX_INLINE_HOT bool truthy() const noexcept { return v_.as_bool(); }
    CLX_INLINE_HOT explicit operator bool() const noexcept { return v_.as_bool(); }

    //------------------ is<T>() / as<T>() / get<T>() / value_or(def)
    template<class T> bool is() const {
        using U = std::remove_cv_t<T>;
        if constexpr (std::same_as<U, Value> || std::same_as<U, LValue> || std::same_as<U, bool>)
            return true;
        else if constexpr (std::derived_from<U, Value>)
            return U::accepts(v_);
        else if constexpr (std::same_as<U, Nil>)
            return is_nil();
        else if constexpr (std::same_as<U, Boolean>)
            return true;
        else if constexpr (std::same_as<U, Number>)
            return is_number();
        else if constexpr (std::integral<U> || std::is_enum_v<U>) {
            int64_t i;
            return clx::to_integer(v_, i);
        } else if constexpr (std::floating_point<U>) {
            double d;
            return v_.to_number(d);
        } else if constexpr (std::same_as<U, std::string>)
            return is_string() || is_number();
        else if constexpr (std::same_as<U, std::string_view> || std::same_as<U, const char*>)
            return is_string();
        else if constexpr (detail::is_optional<U>::value)
            return is_nil() || is<typename U::value_type>();
        else if constexpr (detail::is_vector<U>::value)
            return is_table();
        else
            return true;
    }

    template<class T> T as() const { return from_lvalue<std::remove_cv_t<T>>(state(), v_, 0); }

    //------------------ Implicit *checked* conversion to the Lua object classes: `lua::Table t = f(...);`
    //------------------ (raises a Lua error if the dynamic type does not match; never converts to C++ scalars)
    template<class T>
        requires(std::derived_from<T, Value> && !std::same_as<T, Value>)
    operator T() const {
        return T::cast(state(), v_, 0);
    }

    template<class T> std::optional<T> get() const {
        if (!is<T>())
            return std::nullopt;
        return as<T>();
    }

    template<class T> T value_or(T def) const { return is<T>() ? as<T>() : std::move(def); }

    //------------------ Indexing (metamethod aware). `v["k"]`, `v[1]`; result is assignable.
    template<class K>
        requires ToLua<K>
    Proxy operator[](K&& key) const;

    //------------------ Metatable access
    Value metatable() const { return from_raw(state(), clx::getmetatable(state(), v_)); }
    void set_metatable(const Value& mt) const { clx::setmetatable(state(), v_, mt.v_); }

    //------------------ Length: the `#` operator (honours __len)
    int64_t size() const {
        LState* L = state();
        LValue r = clx::len(L, v_);
        int64_t n;
        if (!clx::to_integer(r, n))
            clx::error(L, "object length is not an integer");
        return n;
    }

    //------------------ Text form (tostring)
    std::string to_string() const { return v_.to_string(state()); }

    //------------------ Raw (metamethod-free) identity comparison
    bool raw_equal(const Value& o) const noexcept { return clx::rawequal(v_, o.v_); }

    //------------------ Calls ---------------------------------------------------------------------
    //------------------ f(args...) -> first result (nil if none)
    template<class... A>
        requires(ToLua<A> && ...)
    Value operator()(A&&... a) const {
        MultiValue mv = call_raw(std::forward<A>(a)...);
        return first(state(), mv);
    }

    //------------------ f.call_multi(args...) -> all results
    template<class... A>
        requires(ToLua<A> && ...)
    Results call_multi(A&&... a) const;

    //------------------ f.call<N>(args...) -> exactly N results (padded with nil), usable with structured bindings
    template<size_t N, class... A>
        requires(ToLua<A> && ...)
    std::array<Value, N> call(A&&... a) const {
        MultiValue mv = call_raw(std::forward<A>(a)...);
        LState* L = state();
        std::array<Value, N> out;
        for (size_t i = 0; i < N && i < mv.count; ++i)
            out[i] = from_raw(L, mv[i]);
        return out;
    }

    //------------------ Protected call: never throws a Lua error, returns { ok, values }
    template<class... A>
        requires(ToLua<A> && ...)
    PResult pcall(A&&... a) const;

    //------------------ Method call: obj.invoke("name", args...) == obj:name(args...)
    template<class... A>
        requires(ToLua<A> && ...)
    Value invoke(std::string_view name, A&&... a) const {
        LState* L = state();
        Value fn = from_raw(L, clx::table_get(L, v_, clx::make_string(L, name.data(), name.size())));
        return fn(*this, std::forward<A>(a)...);
    }

    template<class... A>
        requires(ToLua<A> && ...)
    Results invoke_multi(std::string_view name, A&&... a) const;

    //------------------ Unary operators
    Value operator-() const { return from_raw(state(), clx::unm(state(), v_)); }
    Value operator~() const { return from_raw(state(), clx::bnot(state(), v_)); }

    //------------------ Compound arithmetic
    template<class T>
        requires ToLua<T>
    Value& operator+=(T&& x) {
        return *this = *this + std::forward<T>(x);
    }
    template<class T>
        requires ToLua<T>
    Value& operator-=(T&& x) {
        return *this = *this - std::forward<T>(x);
    }
    template<class T>
        requires ToLua<T>
    Value& operator*=(T&& x) {
        return *this = *this * std::forward<T>(x);
    }
    template<class T>
        requires ToLua<T>
    Value& operator/=(T&& x) {
        return *this = *this / std::forward<T>(x);
    }
    template<class T>
        requires ToLua<T>
    Value& operator%=(T&& x) {
        return *this = *this % std::forward<T>(x);
    }

protected:
    Value(LState* L, const LValue& v)
        : L_(L)
        , v_(v) {
        root();
    }

    CLX_INLINE_HOT void root() const {
        if (L_ && v_.is_gc_obj())
            L_->root_value(v_);
    }

    CLX_INLINE_HOT void unroot() const noexcept {
        if (L_ && v_.is_gc_obj())
            L_->unroot_value(v_);
    }

    //------------------ State to capture when building from a C++ value
    template<class T> static LState* state_for(const T& x) {
        using U = std::decay_t<T>;
        if constexpr (ValueLike<U>)
            return x.state();
        else if constexpr (detail::stateless<U>())
            return detail::g_current;
        else
            return detail::current();
    }

    template<class... A> MultiValue call_raw(A&&... a) const {
        LState* L = state();
        detail::RawArgs<sizeof...(A)> args(L);
        (args.push(std::forward<A>(a)), ...);
        const LValue* ap = args.a;
        return clx::call_function_rooted(L, v_, ap, args.n, &detail::api_site);
    }

    static Value first(LState* L, const MultiValue& mv) { return mv.count ? from_raw(L, mv[0]) : Value(); }

    LState* L_ = nullptr;
    LValue v_;

    friend class Results;
};

//=====================================================================================================
//  Proxy: the lvalue returned by operator[]; reads lazily, assigns through
//=====================================================================================================
class Proxy {
public:
    Proxy(Value obj, Value key)
        : obj_(std::move(obj))
        , key_(std::move(key)) { }

    Proxy(const Proxy&) = default;
    Proxy(Proxy&&) = default;

    //------------------ Assigning a Proxy to a Proxy copies the *value* (it does not rebind)
    Proxy& operator=(const Proxy& rhs) {
        assign(rhs.get());
        return *this;
    }
    Proxy& operator=(Proxy&& rhs) {
        assign(rhs.get());
        return *this;
    }

    template<class T>
        requires(ToLua<T> && !std::same_as<std::decay_t<T>, Proxy>)
    Proxy& operator=(T&& x) {
        assign(Value::make(state(), std::forward<T>(x)));
        return *this;
    }

    //------------------ Read the current value (honours __index)
    Value get() const {
        LState* L = state();
        return Value::from_raw(L, clx::table_get(L, obj_.raw(), key_.raw()));
    }

    //------------------ Write a value (honours __newindex)
    void assign(const Value& v) const { clx::table_set(state(), obj_.raw(), key_.raw(), v.raw()); }

    LState* state() const { return obj_.state(); }

    template<class T> T as() const { return get().template as<T>(); }
    template<class T> std::optional<T> get_as() const { return get().template get<T>(); }
    bool is_nil() const { return get().is_nil(); }
    bool exists() const { return !get().is_nil(); }
    ValueType type() const { return get().type(); }
    bool truthy() const { return get().truthy(); }

    //------------------ Implicit conversions to concrete C++ types: `int x = t["a"]; std::string s = t["b"];`
    template<class T>
        requires(FromLua<T> && !std::same_as<std::remove_cv_t<T>, Value> && !std::same_as<std::remove_cv_t<T>, std::string_view>
            && !std::same_as<std::remove_cv_t<T>, const char*>)
    operator T() const {
        return get().template as<std::remove_cv_t<T>>();
    }

    //------------------ Chained indexing: `t["a"]["b"] = 1`
    template<class K>
        requires ToLua<K>
    Proxy operator[](K&& key) const {
        Value inner = get();
        return Proxy(inner, Value::make(inner.state(), std::forward<K>(key)));
    }

    //------------------ Call the stored function (no self): `t["f"](1, 2)`
    template<class... A>
        requires(ToLua<A> && ...)
    Value operator()(A&&... a) const {
        return get()(std::forward<A>(a)...);
    }

    //------------------ Method call: `t["obj"].invoke("name", ...)`
    template<class... A>
        requires(ToLua<A> && ...)
    Value invoke(std::string_view name, A&&... a) const {
        return get().invoke(name, std::forward<A>(a)...);
    }

    //------------------ `t["f"].call<2>(...)`, `t["f"].call_multi(...)`, `t["f"].pcall(...)`
    template<size_t N, class... A>
        requires(ToLua<A> && ...)
    std::array<Value, N> call(A&&... a) const {
        return get().template call<N>(std::forward<A>(a)...);
    }

    template<class... A>
        requires(ToLua<A> && ...)
    Results call_multi(A&&... a) const;

    template<class... A>
        requires(ToLua<A> && ...)
    PResult pcall(A&&... a) const;

private:
    Value obj_;
    Value key_;
};

template<class K>
    requires ToLua<K>
Proxy Value::operator[](K&& key) const {
    LState* L = state();
    return Proxy(*this, Value::make(L, std::forward<K>(key)));
}

//=====================================================================================================
//  Results / PResult: multiple return values
//=====================================================================================================
class Results {
public:
    Results() = default;
    explicit Results(std::vector<Value> v)
        : v_(std::move(v)) { }

    static Results from_multi(LState* L, const MultiValue& mv) {
        Results r;
        r.v_.reserve(mv.count);
        for (size_t i = 0; i < mv.count; ++i)
            r.v_.push_back(Value::from_raw(L, mv[i]));
        return r;
    }

    size_t size() const noexcept { return v_.size(); }
    bool empty() const noexcept { return v_.empty(); }
    auto begin() const noexcept { return v_.begin(); }
    auto end() const noexcept { return v_.end(); }

    //------------------ Out-of-range index yields nil (like a missing Lua return value)
    Value operator[](size_t i) const { return i < v_.size() ? v_[i] : Value(); }
    Value front() const { return (*this)[0]; }

    //------------------ True when the first value is truthy (so `if (s.find("x"))` works)
    explicit operator bool() const { return !v_.empty() && v_[0].truthy(); }

    //------------------ Fixed-size unpack for structured bindings: auto [a, b] = r.unpack<2>();
    template<size_t N> std::array<Value, N> unpack() const {
        std::array<Value, N> out;
        for (size_t i = 0; i < N && i < v_.size(); ++i)
            out[i] = v_[i];
        return out;
    }

    //------------------ Convert the first value: `for (std::string w : s.gmatch("%a+"))`
    template<class T>
        requires(FromLua<T> && !std::same_as<std::remove_cv_t<T>, bool> && !std::same_as<std::remove_cv_t<T>, std::string_view>
            && !std::same_as<std::remove_cv_t<T>, const char*>)
    operator T() const {
        return v_.empty() ? from_lvalue<std::remove_cv_t<T>>(detail::current(), detail::nil_value, 1)
                          : v_[0].template as<std::remove_cv_t<T>>();
    }

    MultiValue to_multi(LState* L) const {
        std::vector<LValue> raw;
        raw.reserve(v_.size());
        for (const Value& v : v_)
            raw.push_back(v.raw());
        return MultiValue(raw.data(), raw.size(), L);
    }

private:
    std::vector<Value> v_;
};

//------------------ Outcome of a protected call
struct PResult {
    bool ok = false;
    Results values;

    explicit operator bool() const noexcept { return ok; }
    //------------------ First returned value on success
    Value value() const { return ok ? values.front() : Value(); }
    //------------------ Error object on failure
    Value error() const { return ok ? Value() : values.front(); }
    //------------------ Error as text
    std::string message() const { return ok ? std::string() : values.front().to_string(); }
};

template<class... A>
    requires(ToLua<A> && ...)
Results Value::call_multi(A&&... a) const {
    MultiValue mv = call_raw(std::forward<A>(a)...);
    return Results::from_multi(state(), mv);
}

template<class... A>
    requires(ToLua<A> && ...)
Results Value::invoke_multi(std::string_view name, A&&... a) const {
    LState* L = state();
    Value fn = from_raw(L, clx::table_get(L, v_, clx::make_string(L, name.data(), name.size())));
    return fn.call_multi(*this, std::forward<A>(a)...);
}

template<class... A>
    requires(ToLua<A> && ...)
PResult Value::pcall(A&&... a) const {
    LState* L = state();
    detail::RawArgs<sizeof...(A)> args(L);
    (args.push(std::forward<A>(a)), ...);
    const LValue* ap = args.a;
    MultiValue mv = clx::pcall_function(L, v_, ap, args.n);
    PResult r;
    //------------------ the protected call reports like Lua's pcall: (true, results...) | (false, error)
    if (mv.count > 0 && mv[0].type == ValueType::Boolean) {
        r.ok = mv[0].as_bool();
        r.values.~Results();
        new (&r.values) Results();
        std::vector<Value> rest;
        for (size_t i = 1; i < mv.count; ++i)
            rest.push_back(Value::from_raw(L, mv[i]));
        r.values = Results(std::move(rest));
    } else {
        r.ok = true;
        r.values = Results::from_multi(L, mv);
    }
    return r;
}

template<class... A>
    requires(ToLua<A> && ...)
Results Proxy::call_multi(A&&... a) const {
    return get().call_multi(std::forward<A>(a)...);
}

template<class... A>
    requires(ToLua<A> && ...)
PResult Proxy::pcall(A&&... a) const {
    return get().pcall(std::forward<A>(a)...);
}

//=====================================================================================================
//  Operators on Value / Proxy (and mixed with C++ values)
//=====================================================================================================
namespace detail {

    template<class A, class B> LState* op_state(const A& a, const B& b) {
        if constexpr (ValueLike<A>)
            return a.state();
        else
            return b.state();
    }

    //------------------ Operand as a Value reference (no copy when it already is one)
    template<class T> decltype(auto) as_value(LState* L, T&& x) {
        if constexpr (std::derived_from<std::decay_t<T>, Value>)
            return static_cast<const Value&>(x);
        else
            return Value::make(L, std::forward<T>(x));
    }

    template<class T>
    concept StringObject = std::derived_from<std::decay_t<T>, String>;

    template<class S>
    concept StringLike = std::convertible_to<const S&, std::string_view> && !std::derived_from<S, Value>;

} // namespace detail

#define CLX_LUA_ARITH(OP, FN)                                                                                          \
    template<class A, class B>                                                                                         \
        requires BinaryOperands<A, B>                                                                                  \
    Value operator OP(A&& a, B&& b) {                                                                                  \
        LState* L = detail::op_state(a, b);                                                                            \
        auto&& av = detail::as_value(L, std::forward<A>(a));                                                           \
        auto&& bv = detail::as_value(L, std::forward<B>(b));                                                           \
        return Value::from_raw(L, clx::FN(L, av.raw(), bv.raw()));                                                     \
    }

CLX_LUA_ARITH(-, sub)
CLX_LUA_ARITH(*, mul)
CLX_LUA_ARITH(/, div)
CLX_LUA_ARITH(%, mod)
CLX_LUA_ARITH(&, band)
CLX_LUA_ARITH(|, bor)
CLX_LUA_ARITH(^, bxor)
CLX_LUA_ARITH(<<, shl)
CLX_LUA_ARITH(>>, shr)

#undef CLX_LUA_ARITH

//------------------ Floor division and exponentiation have no C++ operator: free functions
template<class A, class B>
    requires BinaryOperands<A, B>
Value idiv(A&& a, B&& b) {
    LState* L = detail::op_state(a, b);
    auto&& av = detail::as_value(L, std::forward<A>(a));
    auto&& bv = detail::as_value(L, std::forward<B>(b));
    return Value::from_raw(L, clx::idiv(L, av.raw(), bv.raw()));
}

template<class A, class B>
    requires BinaryOperands<A, B>
Value pow(A&& a, B&& b) {
    LState* L = detail::op_state(a, b);
    auto&& av = detail::as_value(L, std::forward<A>(a));
    auto&& bv = detail::as_value(L, std::forward<B>(b));
    return Value::from_raw(L, clx::pow(L, av.raw(), bv.raw()));
}

//------------------ Lua concatenation (`..`)
template<class A, class B>
    requires BinaryOperands<A, B>
Value concat(A&& a, B&& b) {
    LState* L = detail::op_state(a, b);
    auto&& av = detail::as_value(L, std::forward<A>(a));
    auto&& bv = detail::as_value(L, std::forward<B>(b));
    return Value::from_raw(L, clx::concat(L, av.raw(), bv.raw()));
}

//------------------ `+` is arithmetic on Values and concatenation as soon as one side is a String
template<class A, class B>
    requires BinaryOperands<A, B>
auto operator+(A&& a, B&& b);

//------------------ Comparison (metamethod aware, bool result)
template<class A, class B>
    requires BinaryOperands<A, B>
bool operator==(A&& a, B&& b) {
    //------------------ String == string-like compares bytes without creating a Lua string
    if constexpr (detail::StringObject<A> && detail::StringLike<std::decay_t<B>>)
        return a.view() == std::string_view(b);
    else if constexpr (detail::StringObject<B> && detail::StringLike<std::decay_t<A>>)
        return b.view() == std::string_view(a);
    else {
    LState* L = detail::op_state(a, b);
    auto&& av = detail::as_value(L, std::forward<A>(a));
    auto&& bv = detail::as_value(L, std::forward<B>(b));
    return clx::eq(L, av.raw(), bv.raw()).as_bool();
    }
}

template<class A, class B>
    requires BinaryOperands<A, B>
bool operator<(A&& a, B&& b) {
    LState* L = detail::op_state(a, b);
    auto&& av = detail::as_value(L, std::forward<A>(a));
    auto&& bv = detail::as_value(L, std::forward<B>(b));
    return clx::lt(L, av.raw(), bv.raw()).as_bool();
}

template<class A, class B>
    requires BinaryOperands<A, B>
bool operator<=(A&& a, B&& b) {
    LState* L = detail::op_state(a, b);
    auto&& av = detail::as_value(L, std::forward<A>(a));
    auto&& bv = detail::as_value(L, std::forward<B>(b));
    return clx::le(L, av.raw(), bv.raw()).as_bool();
}

template<class A, class B>
    requires BinaryOperands<A, B>
bool operator>(A&& a, B&& b) {
    LState* L = detail::op_state(a, b);
    auto&& av = detail::as_value(L, std::forward<A>(a));
    auto&& bv = detail::as_value(L, std::forward<B>(b));
    return clx::lt(L, bv.raw(), av.raw()).as_bool();
}

template<class A, class B>
    requires BinaryOperands<A, B>
bool operator>=(A&& a, B&& b) {
    LState* L = detail::op_state(a, b);
    auto&& av = detail::as_value(L, std::forward<A>(a));
    auto&& bv = detail::as_value(L, std::forward<B>(b));
    return clx::le(L, bv.raw(), av.raw()).as_bool();
}

//------------------ Three-way comparison for std::sort, std::map, ... (NaN-like cases are "unordered")
template<class A, class B>
    requires BinaryOperands<A, B>
std::partial_ordering operator<=>(A&& a, B&& b) {
    LState* L = detail::op_state(a, b);
    auto&& av = detail::as_value(L, std::forward<A>(a));
    auto&& bv = detail::as_value(L, std::forward<B>(b));
    if (clx::eq(L, av.raw(), bv.raw()).as_bool())
        return std::partial_ordering::equivalent;
    if (clx::lt(L, av.raw(), bv.raw()).as_bool())
        return std::partial_ordering::less;
    if (clx::lt(L, bv.raw(), av.raw()).as_bool())
        return std::partial_ordering::greater;
    return std::partial_ordering::unordered;
}

//=====================================================================================================
//  GMatchRange: `for (auto m : s.gmatch(pattern))`
//=====================================================================================================
class GMatchRange {
public:
    class iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = Results;

        iterator() = default;
        explicit iterator(const Value* fn)
            : fn_(fn)
            , done_(false) {
            advance();
        }

        const Results& operator*() const { return cur_; }
        iterator& operator++() {
            advance();
            return *this;
        }
        void operator++(int) { advance(); }
        friend bool operator==(const iterator& it, std::default_sentinel_t) noexcept { return it.done_; }

    private:
        void advance() {
            cur_ = fn_->call_multi();
            done_ = cur_.empty() || cur_[0].is_nil();
        }

        const Value* fn_ = nullptr;
        Results cur_;
        bool done_ = true;
    };

    explicit GMatchRange(Value fn)
        : fn_(std::move(fn)) { }

    iterator begin() const { return iterator(&fn_); }
    std::default_sentinel_t end() const noexcept { return std::default_sentinel; }

private:
    Value fn_;
};

//=====================================================================================================
//  Function
//=====================================================================================================
namespace detail {

    template<class T> struct fn_traits;

    template<class R, class... A> struct fn_traits<R(A...)> {
        using sig = type_list<R, A...>;
    };
    template<class R, class... A> struct fn_traits<R (*)(A...)> : fn_traits<R(A...)> { };
    template<class R, class... A> struct fn_traits<R (*)(A...) noexcept> : fn_traits<R(A...)> { };
    template<class C, class R, class... A> struct fn_traits<R (C::*)(A...)> : fn_traits<R(A...)> { };
    template<class C, class R, class... A> struct fn_traits<R (C::*)(A...) noexcept> : fn_traits<R(A...)> { };
    template<class C, class R, class... A> struct fn_traits<R (C::*)(A...) const> : fn_traits<R(A...)> { };
    template<class C, class R, class... A> struct fn_traits<R (C::*)(A...) const noexcept> : fn_traits<R(A...)> { };
    template<class F>
        requires requires { &F::operator(); }
    struct fn_traits<F> : fn_traits<decltype(&F::operator())> { };

    //------------------ Fetch and convert argument I (0-based); missing arguments read as nil
    template<class P> std::decay_t<P> arg(LState* L, const LValue* args, size_t n, size_t i) {
        const LValue& v = i < n ? args[i] : nil_value;
        return from_lvalue<std::decay_t<P>>(L, v, static_cast<int>(i) + 1);
    }

    //------------------ Convert a C++ return value into a Lua multi-return
    template<class R> MultiValue to_multi(LState* L, R&& r) {
        using U = std::decay_t<R>;
        if constexpr (std::same_as<U, MultiValue>)
            return std::forward<R>(r);
        else if constexpr (std::same_as<U, Results>)
            return r.to_multi(L);
        else if constexpr (is_tuple_like<U>::value) {
            std::vector<Value> hold;
            std::apply([&](auto&&... e) { (hold.push_back(Value::make(L, std::forward<decltype(e)>(e))), ...); },
                std::forward<R>(r));
            std::vector<LValue> raw;
            raw.reserve(hold.size());
            for (const Value& v : hold)
                raw.push_back(v.raw());
            return MultiValue(raw.data(), raw.size(), L);
        } else
            return MultiValue(to_lvalue(L, std::forward<R>(r)));
    }

    //------------------ Call a free callable with converted arguments
    template<class R, class... P, class F>
    MultiValue invoke_args(F& fn, LState* L, const LValue* args, size_t n, type_list<R, P...>) {
        return [&]<size_t... I>(std::index_sequence<I...>) -> MultiValue {
            if constexpr (std::is_void_v<R>) {
                fn(arg<P>(L, args, n, I)...);
                return MultiValue();
            } else
                return to_multi(L, fn(arg<P>(L, args, n, I)...));
        }(std::index_sequence_for<P...> { });
    }

    //------------------ Call a member function on `self`; Lua arguments start at index 1 (0 is `self`)
    template<class T, class M, class R, class... P>
    MultiValue invoke_member(M pm, T& self, LState* L, const LValue* args, size_t n, type_list<R, P...>) {
        return [&]<size_t... I>(std::index_sequence<I...>) -> MultiValue {
            if constexpr (std::is_void_v<R>) {
                (self.*pm)(arg<P>(L, args, n, I + 1)...);
                return MultiValue();
            } else
                return to_multi(L, (self.*pm)(arg<P>(L, args, n, I + 1)...));
        }(std::index_sequence_for<P...> { });
    }

    //------------------ Build a Lua function value from any supported C++ callable
    template<class F> LValue make_function(LState* L, F&& f) {
        using D = std::decay_t<F>;
        if constexpr (RawSignature<D>) {
            return L->create_closure(CFunctionType(std::forward<F>(f)));
        } else {
            using Sig = typename fn_traits<D>::sig;
            return L->create_closure(CFunctionType(
                [fn = D(std::forward<F>(f))](LState* S, const LValue* args, size_t n) mutable -> MultiValue {
                    Scope scope(S);
                    try {
                        return invoke_args(fn, S, args, n, Sig { });
                    } catch (const clx::LRuntimeException&) {
                        throw;
                    } catch (const std::exception& e) {
                        clx::error(S, e.what());
                    }
                }));
        }
    }

} // namespace detail

class Function : public Value {
public:
    //------------------ nil function (calling it raises a Lua error)
    Function() = default;

    //------------------ From any C++ callable: `lua::Function f = [](int a, int b) { return a + b; };`
    template<class F>
        requires(detail::CallableObject<std::decay_t<F>>)
    Function(F&& f)
        : Value(detail::current(), detail::make_function(detail::current(), std::forward<F>(f))) { }

    //------------------ Checked conversion from a Value (throws a Lua error on mismatch)
    explicit Function(const Value& v)
        : Function(cast(v.state(), v.raw())) { }

    static Function cast(LState* L, const LValue& v, int argn = 0) {
        if (v.type != ValueType::Function)
            detail::bad_type(L, argn, "function", v);
        return Function(L, v, detail::adopt);
    }

    static bool accepts(const LValue& v) noexcept { return v.type == ValueType::Function; }

private:
    Function(LState* L, const LValue& v, detail::adopt_t)
        : Value(L, v) { }
};

//=====================================================================================================
//  String
//=====================================================================================================
namespace detail {

    //------------------ Run a builtin library function (str_* / table_*) with `self` as first argument
    template<class... A> MultiValue builtin(RawCFunction fn, LState* L, const Value& self, A&&... a) {
        RawArgs<1 + sizeof...(A)> args(L);
        args.push(self);
        (args.push(std::forward<A>(a)), ...);
        return fn(L, args.a, args.n);
    }

} // namespace detail

class String : public Value {
public:
    //------------------ Empty string in the current state
    String()
        : String(detail::current(), std::string_view()) { }

    //------------------ From anything string-like: `lua::String s = "abc"; lua::String t = std::string("x");`
    template<detail::StringLike S>
    String(const S& s)
        : String(detail::current(), std::string_view(s)) { }

    String(LState* L, std::string_view sv)
        : Value(L, clx::make_string(L, sv.data(), sv.size())) { }

    //------------------ Checked conversion from a Value (numbers are converted like Lua does)
    explicit String(const Value& v)
        : String(cast(v.state(), v.raw())) { }

    static String cast(LState* L, const LValue& v, int argn = 0) {
        if (v.type == ValueType::String)
            return String(L, v, detail::adopt);
        if (clx::is_number(v))
            return String(L, clx::make_string(L, v.to_string(L)), detail::adopt);
        detail::bad_type(L, argn, "string", v);
    }

    static bool accepts(const LValue& v) noexcept { return v.type == ValueType::String || clx::is_number(v); }

    //------------------ Raw text access (valid while this object lives and is not moved)
    std::string_view view() const noexcept { return std::string_view(v_.as_string(), v_.string_len()); }
    const char* c_str() const noexcept { return v_.as_string(); }
    std::string str() const { return std::string(view()); }
    operator std::string_view() const noexcept { return view(); }

    size_t length() const noexcept { return v_.string_len(); }
    size_t size() const noexcept { return v_.string_len(); }
    bool empty() const noexcept { return v_.string_len() == 0; }
    const char* begin() const noexcept { return v_.as_string(); }
    const char* end() const noexcept { return v_.as_string() + v_.string_len(); }

    //------------------ Pure C++ helpers (no Lua call involved)
    bool starts_with(std::string_view p) const noexcept { return view().starts_with(p); }
    bool ends_with(std::string_view p) const noexcept { return view().ends_with(p); }
    bool contains(std::string_view p) const noexcept { return view().find(p) != std::string_view::npos; }

    //------------------ In-place concatenation
    template<class T>
        requires ToLua<T>
    String& operator+=(T&& x) {
        *this = *this + std::forward<T>(x);
        return *this;
    }

    //======================== string module as methods ==========================================
    //------------------ s:sub(i [, j])
    String sub(int64_t i, int64_t j = -1) const {
        MultiValue mv = detail::builtin(clx::str_sub, state(), *this, i, j);
        return String(state(), mv[0], detail::adopt);
    }
    String reverse() const { return one(clx::str_reverse); }
    String lower() const { return one(clx::str_lower); }
    String upper() const { return one(clx::str_upper); }
    String rep(int64_t n) const { return one(clx::str_rep, n); }
    String rep(int64_t n, std::string_view sep) const { return one(clx::str_rep, n, sep); }

    //------------------ s:byte(i) -> optional code; s:byte(i, j) -> all codes
    std::optional<int64_t> byte(int64_t i = 1) const {
        MultiValue mv = detail::builtin(clx::str_byte, state(), *this, i);
        if (mv.count == 0 || mv[0].type != ValueType::Int64)
            return std::nullopt;
        return mv[0].as_integer();
    }
    Results byte(int64_t i, int64_t j) const {
        return Results::from_multi(state(), detail::builtin(clx::str_byte, state(), *this, i, j));
    }

    //------------------ string.char(...)
    template<class... C>
        requires(std::convertible_to<C, int64_t> && ...)
    static String chr(C... codes) {
        LState* L = detail::current();
        detail::RawArgs<sizeof...(C)> args(L);
        (args.push(static_cast<int64_t>(codes)), ...);
        MultiValue mv = clx::str_char(L, args.a, args.n);
        return String(L, mv[0], detail::adopt);
    }

    //------------------ ("%d items"):format(...)
    template<class... A>
        requires(ToLua<A> && ...)
    String format(A&&... a) const {
        MultiValue mv = detail::builtin(clx::str_format, state(), *this, std::forward<A>(a)...);
        return String(state(), mv[0], detail::adopt);
    }

    //------------------ s:find(pattern [, init [, plain]]) -> start, end, captures...  (first value nil when absent)
    Results find(std::string_view pattern, int64_t init = 1, bool plain = false) const {
        return Results::from_multi(state(), detail::builtin(clx::str_find, state(), *this, pattern, init, plain));
    }

    //------------------ s:match(pattern [, init]) -> captures...
    Results match(std::string_view pattern, int64_t init = 1) const {
        return Results::from_multi(state(), detail::builtin(clx::str_match, state(), *this, pattern, init));
    }

    //------------------ s:gmatch(pattern) as a C++ range of Results
    GMatchRange gmatch(std::string_view pattern) const {
        MultiValue mv = detail::builtin(clx::str_gmatch, state(), *this, pattern);
        return GMatchRange(Value::from_raw(state(), mv[0]));
    }

    //------------------ s:gsub(pattern, repl [, n]) -> { result, count }; repl may be string, table or callable
    template<class R>
        requires ToLua<R>
    std::pair<String, int64_t> gsub(std::string_view pattern, R&& repl) const {
        return gsub_result(detail::builtin(clx::str_gsub, state(), *this, pattern, std::forward<R>(repl)));
    }
    template<class R>
        requires ToLua<R>
    std::pair<String, int64_t> gsub(std::string_view pattern, R&& repl, int64_t max_n) const {
        return gsub_result(detail::builtin(clx::str_gsub, state(), *this, pattern, std::forward<R>(repl), max_n));
    }

    //------------------ fmt:pack(...) / fmt:unpack(data [, pos]) / fmt:packsize()
    template<class... A>
        requires(ToLua<A> && ...)
    String pack(A&&... a) const {
        MultiValue mv = detail::builtin(clx::str_pack, state(), *this, std::forward<A>(a)...);
        return String(state(), mv[0], detail::adopt);
    }
    Results unpack(const String& data, int64_t pos = 1) const {
        return Results::from_multi(state(), detail::builtin(clx::str_unpack, state(), *this, data, pos));
    }
    int64_t packsize() const {
        MultiValue mv = detail::builtin(clx::str_packsize, state(), *this);
        return mv[0].as_integer();
    }

private:
    String(LState* L, const LValue& v, detail::adopt_t)
        : Value(L, v) { }

    template<class... A> String one(RawCFunction fn, A&&... a) const {
        MultiValue mv = detail::builtin(fn, state(), *this, std::forward<A>(a)...);
        return String(state(), mv[0], detail::adopt);
    }

    std::pair<String, int64_t> gsub_result(const MultiValue& mv) const {
        return { String(state(), mv[0], detail::adopt), mv.count > 1 ? mv[1].as_integer() : 0 };
    }
};

//------------------ `+`: arithmetic for Values, concatenation when a String is involved
template<class A, class B>
    requires BinaryOperands<A, B>
auto operator+(A&& a, B&& b) {
    LState* L = detail::op_state(a, b);
    auto&& av = detail::as_value(L, std::forward<A>(a));
    auto&& bv = detail::as_value(L, std::forward<B>(b));
    if constexpr (detail::StringObject<A> || detail::StringObject<B>)
        return String::cast(L, clx::concat(L, av.raw(), bv.raw()));
    else
        return Value::from_raw(L, clx::add(L, av.raw(), bv.raw()));
}

//=====================================================================================================
//  Table
//=====================================================================================================
class Table : public Value {
public:
    //------------------ One element of a brace initializer: positional value OR {key, value}
    struct Item {
        Value key;
        Value value;
        bool keyed;

        template<class V>
            requires(!std::same_as<std::decay_t<V>, Item> && ToLua<V>)
        Item(V&& v)
            : value(Value::make(detail::current(), std::forward<V>(v)))
            , keyed(false) { }

        template<class K, class V>
            requires(ToLua<K> && ToLua<V>)
        Item(K&& k, V&& v)
            : key(Value::make(detail::current(), std::forward<K>(k)))
            , value(Value::make(detail::current(), std::forward<V>(v)))
            , keyed(true) { }
    };

    //------------------ pairs() iterator: yields std::pair<Value, Value>
    class iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = std::pair<Value, Value>;

        iterator() = default;
        explicit iterator(const Value& t)
            : tbl_(t)
            , done_(false) {
            advance();
        }

        value_type operator*() const { return { key_, val_ }; }
        iterator& operator++() {
            advance();
            return *this;
        }
        void operator++(int) { advance(); }
        friend bool operator==(const iterator& it, std::default_sentinel_t) noexcept { return it.done_; }

    private:
        void advance() {
            LState* L = tbl_.state();
            MultiValue mv = clx::next(L, tbl_.raw(), key_.raw());
            if (mv.count < 2 || mv[0].type == ValueType::Nil) {
                done_ = true;
                key_ = Value();
                val_ = Value();
            } else {
                key_ = Value::from_raw(L, mv[0]);
                val_ = Value::from_raw(L, mv[1]);
            }
        }

        Value tbl_, key_, val_;
        bool done_ = true;
    };

    //------------------ ipairs() range: yields std::pair<int64_t, Value> until the first nil
    class IndexRange {
    public:
        class iterator {
        public:
            using difference_type = std::ptrdiff_t;
            using value_type = std::pair<int64_t, Value>;

            iterator() = default;
            explicit iterator(const Value& t)
                : tbl_(t)
                , done_(false) {
                advance();
            }

            value_type operator*() const { return { idx_, val_ }; }
            iterator& operator++() {
                advance();
                return *this;
            }
            void operator++(int) { advance(); }
            friend bool operator==(const iterator& it, std::default_sentinel_t) noexcept { return it.done_; }

        private:
            void advance() {
                LState* L = tbl_.state();
                ++idx_;
                val_ = Value::from_raw(L, clx::table_get_int(L, tbl_.raw(), static_cast<size_t>(idx_)));
                done_ = val_.is_nil();
            }

            Value tbl_, val_;
            int64_t idx_ = 0;
            bool done_ = true;
        };

        explicit IndexRange(Value t)
            : t_(std::move(t)) { }
        iterator begin() const { return iterator(t_); }
        std::default_sentinel_t end() const noexcept { return std::default_sentinel; }

    private:
        Value t_;
    };

    //------------------ Tag for the capacity constructor
    struct reserve_t {
        explicit reserve_t() = default;
    };

    //------------------ Empty table in the current state
    Table()
        : Table(detail::current()) { }

    explicit Table(LState* L, size_t narr = 0, size_t nhash = 0)
        : Value(L, L->create_table(narr, nhash)) { }

    //------------------ Table with preallocated capacity: lua::Table t(lua::Table::reserve_t {}, 1000, 0)
    Table(reserve_t, size_t narr, size_t nhash = 0)
        : Table(detail::current(), narr, nhash) { }

    //------------------ Lua-style constructor: Table{ 1, 2, {"name", "x"}, {"n", 3} }
    Table(std::initializer_list<Item> init)
        : Table(detail::current(), positional(init), init.size() - positional(init)) {
        LState* L = state();
        int64_t pos = 1;
        for (const Item& it : init) {
            if (it.keyed)
                clx::table_set(L, v_, it.key.raw(), it.value.raw());
            else
                clx::raw_set_i(L, v_, pos++, it.value.raw());
        }
    }

    //------------------ From a C++ range (array) or a range of pairs (map): Table t(std::vector{1,2,3});
    template<std::ranges::input_range R>
        requires(ToLua<R> && !std::convertible_to<const std::decay_t<R>&, std::string_view>
            && !std::derived_from<std::decay_t<R>, Value>)
    explicit Table(R&& r)
        : Value(detail::current(), to_lvalue(detail::current(), std::forward<R>(r))) { }

    //------------------ Checked conversion from a Value (throws a Lua error on mismatch)
    explicit Table(const Value& v)
        : Table(cast(v.state(), v.raw())) { }

    static Table cast(LState* L, const LValue& v, int argn = 0) {
        if (v.type != ValueType::Table)
            detail::bad_type(L, argn, "table", v);
        return Table(L, v, detail::adopt);
    }

    static bool accepts(const LValue& v) noexcept { return v.type == ValueType::Table; }

    //------------------ Get / set (metamethod aware); the same as t[k] but without a Proxy
    template<class K>
        requires ToLua<K>
    Value get(K&& key) const {
        LState* L = state();
        Value k = Value::make(L, std::forward<K>(key));
        return from_raw(L, clx::table_get(L, v_, k.raw()));
    }

    template<class K, class V>
        requires(ToLua<K> && ToLua<V>)
    void set(K&& key, V&& val) const {
        LState* L = state();
        Value k = Value::make(L, std::forward<K>(key));
        Value v = Value::make(L, std::forward<V>(val));
        clx::table_set(L, v_, k.raw(), v.raw());
    }

    //------------------ Raw access (no metamethods)
    template<class K>
        requires ToLua<K>
    Value raw_get(K&& key) const {
        LState* L = state();
        Value k = Value::make(L, std::forward<K>(key));
        return from_raw(L, clx::raw_get(L, v_, k.raw()));
    }

    template<class K, class V>
        requires(ToLua<K> && ToLua<V>)
    void raw_set(K&& key, V&& val) const {
        LState* L = state();
        Value k = Value::make(L, std::forward<K>(key));
        Value v = Value::make(L, std::forward<V>(val));
        clx::raw_set(L, v_, k.raw(), v.raw());
    }

    //------------------ Typed field read with a default: t.field<int>("port", 8080)
    template<class T, class K>
        requires ToLua<K>
    T field(K&& key, T def) const {
        return get(std::forward<K>(key)).template value_or<T>(std::move(def));
    }

    //------------------ Sizes
    int64_t raw_size() const noexcept { return clx::rawlen(v_); }

    bool empty() const {
        MultiValue mv = clx::next(state(), v_, LValue());
        return mv.count == 0 || mv[0].type == ValueType::Nil;
    }

    //------------------ Append at the end of the sequence
    template<class V>
        requires ToLua<V>
    void push_back(V&& val) const {
        LState* L = state();
        Value v = Value::make(L, std::forward<V>(val));
        clx::raw_set_i(L, v_, clx::rawlen(v_) + 1, v.raw());
    }

    //------------------ Append every element of a C++ range
    template<std::ranges::input_range R> void append(R&& r) const {
        for (auto&& e : r)
            push_back(e);
    }

    //------------------ Iteration
    iterator begin() const { return iterator(*this); }
    std::default_sentinel_t end() const noexcept { return std::default_sentinel; }
    IndexRange ipairs() const { return IndexRange(*this); }
    auto pairs() const { return std::ranges::subrange(begin(), end()); }

    //------------------ Sequence to std::vector<T>
    template<class T = Value> std::vector<T> to_vector() const {
        LState* L = state();
        std::vector<T> out;
        int64_t n = clx::rawlen(v_);
        out.reserve(static_cast<size_t>(n));
        for (int64_t i = 1; i <= n; ++i)
            out.push_back(from_lvalue<T>(L, clx::raw_get_i(L, v_, i), static_cast<int>(i)));
        return out;
    }

    //------------------ Shallow copy (keeps the metatable)
    Table clone() const {
        LState* L = state();
        Table t(L, static_cast<size_t>(clx::rawlen(v_)), 0);
        for (auto&& [k, v] : *this)
            clx::raw_set(L, t.v_, k.raw(), v.raw());
        Value mt = metatable();
        if (!mt.is_nil())
            t.set_metatable(mt);
        return t;
    }

    //======================== table module as methods ===========================================
    //------------------ table.insert(t, v) / table.insert(t, pos, v)
    template<class V>
        requires ToLua<V>
    void insert(V&& val) const {
        detail::builtin(clx::table_insert, state(), *this, std::forward<V>(val));
    }
    template<class V>
        requires ToLua<V>
    void insert(int64_t pos, V&& val) const {
        detail::builtin(clx::table_insert, state(), *this, pos, std::forward<V>(val));
    }

    //------------------ table.remove(t [, pos]) -> removed value
    Value remove() const {
        MultiValue mv = detail::builtin(clx::table_remove, state(), *this);
        return mv.count ? from_raw(state(), mv[0]) : Value();
    }
    Value remove(int64_t pos) const {
        MultiValue mv = detail::builtin(clx::table_remove, state(), *this, pos);
        return mv.count ? from_raw(state(), mv[0]) : Value();
    }

    //------------------ table.concat(t [, sep [, i [, j]]])
    String concat(std::string_view sep = {}) const {
        MultiValue mv = detail::builtin(clx::table_concat, state(), *this, sep);
        return String::cast(state(), mv[0]);
    }
    String concat(std::string_view sep, int64_t i, int64_t j) const {
        MultiValue mv = detail::builtin(clx::table_concat, state(), *this, sep, i, j);
        return String::cast(state(), mv[0]);
    }

    //------------------ table.sort(t [, comp]); comp may be any C++ callable returning bool
    void sort() const { detail::builtin(clx::table_sort, state(), *this); }
    template<class Cmp>
        requires ToLua<Cmp>
    void sort(Cmp&& cmp) const {
        detail::builtin(clx::table_sort, state(), *this, std::forward<Cmp>(cmp));
    }

    //------------------ table.unpack(t [, i [, j]])
    Results unpack() const {
        return Results::from_multi(state(), detail::builtin(clx::table_unpack, state(), *this));
    }
    Results unpack(int64_t i) const {
        return Results::from_multi(state(), detail::builtin(clx::table_unpack, state(), *this, i));
    }
    Results unpack(int64_t i, int64_t j) const {
        return Results::from_multi(state(), detail::builtin(clx::table_unpack, state(), *this, i, j));
    }

    //------------------ table.move(a1, f, e, t [, a2]) -> destination table
    Table move(int64_t f, int64_t e, int64_t t) const {
        MultiValue mv = detail::builtin(clx::table_move, state(), *this, f, e, t);
        return cast(state(), mv[0]);
    }
    Table move(int64_t f, int64_t e, int64_t t, const Table& dest) const {
        MultiValue mv = detail::builtin(clx::table_move, state(), *this, f, e, t, dest);
        return cast(state(), mv[0]);
    }

    //------------------ table.pack(...) -> table with field n
    template<class... A>
        requires(ToLua<A> && ...)
    static Table pack(A&&... a) {
        LState* L = detail::current();
        detail::RawArgs<sizeof...(A)> args(L);
        (args.push(std::forward<A>(a)), ...);
        MultiValue mv = clx::table_pack(L, args.a, args.n);
        return cast(L, mv[0]);
    }

private:
    Table(LState* L, const LValue& v, detail::adopt_t)
        : Value(L, v) { }

    static size_t positional(std::initializer_list<Item> init) noexcept {
        size_t n = 0;
        for (const Item& it : init)
            n += it.keyed ? 0 : 1;
        return n;
    }
};

//=====================================================================================================
//  UserData / Class<T>: expose C++ objects to Lua
//=====================================================================================================
namespace detail {

    //------------------ Per-type metatable stored in the registry (created on first use)
    template<class T> Value usertype_metatable(LState* L, std::string_view display_name = {}) {
        static const std::string key = std::string("clx.cpp.") + typeid(T).name();
        LValue reg(ValueType::Table, registry(L));
        LValue found = clx::raw_get(L, reg, key.c_str());
        if (found.type == ValueType::Table) {
            //------------------ the metatable may have been created earlier by a UserData, so a name
            //------------------ given later to Class<T> must still take effect
            if (!display_name.empty())
                clx::raw_set(L, found, "__name", LValue(L->intern_string(display_name.data(), display_name.size())));
            return Value::from_raw(L, found);
        }

        Table mt(L);
        mt.raw_set("__index", mt);
        mt.raw_set("__name", display_name.empty() ? std::string_view(typeid(T).name()) : display_name);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            mt.raw_set("__gc", [](LState*, const LValue* args, size_t n) -> MultiValue {
                if (n > 0 && args[0].type == ValueType::UserData) {
                    LUserdata* ud = static_cast<LUserdata*>(args[0].as_pointer());
                    static_cast<T*>(ud->data())->~T();
                }
                return MultiValue();
            });
        }
        clx::raw_set(L, reg, key.c_str(), mt.raw());
        return mt;
    }

} // namespace detail

class UserData : public Value {
public:
    //------------------ Construct a T inside a new full userdata (destroyed by __gc)
    template<class T, class... A> static UserData create(A&&... args) {
        static_assert(alignof(T) <= alignof(void*), "clx::lua::UserData: over-aligned types are not supported");
        LState* L = detail::current();
        Value mt = detail::usertype_metatable<T>(L);
        LValue ud = clx::newuserdata(L, sizeof(T));
        RootRange rr(L, &ud, 1);
        new (static_cast<LUserdata*>(ud.as_pointer())->data()) T(std::forward<A>(args)...);
        clx::setmetatable(L, ud, mt.raw());
        return UserData(L, ud, detail::adopt);
    }

    //------------------ Typed access: nullptr when this userdata does not hold a T
    template<class T> T* get() const noexcept {
        if (v_.type != ValueType::UserData)
            return nullptr;
        LState* L = state();
        LValue reg(ValueType::Table, detail::registry(L));
        static const std::string key = std::string("clx.cpp.") + typeid(T).name();
        LValue mt = clx::raw_get(L, reg, key.c_str());
        LUserdata* ud = static_cast<LUserdata*>(v_.as_pointer());
        if (mt.type != ValueType::Table || ud->metatable != static_cast<LTable*>(mt.as_pointer()))
            return nullptr;
        return static_cast<T*>(ud->data());
    }

    template<class T> bool holds() const noexcept { return get<T>() != nullptr; }

    //------------------ Checked reference access (Lua error on mismatch)
    template<class T> T& as_ref(int argn = 0) const {
        T* p = get<T>();
        if (!p)
            detail::bad_type(state(), argn, typeid(T).name(), v_);
        return *p;
    }

    explicit UserData(const Value& v)
        : UserData(cast(v.state(), v.raw())) { }

    static UserData cast(LState* L, const LValue& v, int argn = 0) {
        if (v.type != ValueType::UserData)
            detail::bad_type(L, argn, "userdata", v);
        return UserData(L, v, detail::adopt);
    }

    static bool accepts(const LValue& v) noexcept { return v.type == ValueType::UserData; }

private:
    UserData(LState* L, const LValue& v, detail::adopt_t)
        : Value(L, v) { }
};

//------------------ Class<T>: describe how T looks from Lua
//------------------     lua::Class<Counter>("Counter")
//------------------         .method("inc", &Counter::inc)
//------------------         .method("get", &Counter::get)
//------------------         .constructor<int>(L.globals(), "new");
template<class T> class Class {
public:
    explicit Class(std::string_view name = {})
        : mt_(Table::cast(detail::current(), detail::usertype_metatable<T>(detail::current(), name).raw())) { }

    //------------------ Bind a member function: obj:name(args...)
    template<class M> Class& method(const char* name, M pm) {
        using Sig = typename detail::fn_traits<M>::sig;
        mt_.raw_set(name, [pm](LState* L, const LValue* args, size_t n) -> MultiValue {
            Scope scope(L);
            UserData ud = UserData::cast(L, n > 0 ? args[0] : detail::nil_value, 1);
            T& self = ud.as_ref<T>(1);
            try {
                return detail::invoke_member<T>(pm, self, L, args, n, Sig { });
            } catch (const clx::LRuntimeException&) {
                throw;
            } catch (const std::exception& e) {
                clx::error(L, e.what());
            }
        });
        return *this;
    }

    //------------------ Bind any callable under a metatable key (e.g. "__tostring", "__add", plain functions)
    template<class F> Class& function(const char* name, F&& f) {
        mt_.raw_set(name, std::forward<F>(f));
        return *this;
    }

    //------------------ Register `into.name(args...)` as a factory building a T
    template<class... Args> Class& constructor(const Table& into, const char* name) {
        into.set(name, +[](Args... a) -> UserData { return UserData::create<T>(std::move(a)...); });
        return *this;
    }

    Table metatable() const { return mt_; }

private:
    Table mt_;
};

//=====================================================================================================
//  Thread (coroutine)
//=====================================================================================================
class Thread : public Value {
public:
    //------------------ New suspended coroutine running `fn`
    explicit Thread(const Function& fn)
        : Value(detail::current(), clx::create_thread(detail::current(), fn.raw())) { }

    explicit Thread(const Value& v)
        : Thread(cast(v.state(), v.raw())) { }

    static Thread cast(LState* L, const LValue& v, int argn = 0) {
        if (v.type != ValueType::Thread)
            detail::bad_type(L, argn, "thread", v);
        return Thread(L, v, detail::adopt);
    }

    static bool accepts(const LValue& v) noexcept { return v.type == ValueType::Thread; }

    //------------------ coroutine.resume(co, ...) -> { ok, values... }
    template<class... A>
        requires(ToLua<A> && ...)
    PResult resume(A&&... a) const {
        Results r = lib("resume").call_multi(*this, std::forward<A>(a)...);
        PResult out;
        out.ok = r[0].truthy();
        std::vector<Value> rest;
        for (size_t i = 1; i < r.size(); ++i)
            rest.push_back(r[i]);
        out.values = Results(std::move(rest));
        return out;
    }

    //------------------ coroutine.status(co): "suspended" | "running" | "normal" | "dead"
    std::string status() const { return lib("status")(*this).template as<std::string>(); }
    bool dead() const { return status() == "dead"; }
    bool suspended() const { return status() == "suspended"; }

    //------------------ coroutine.close(co)
    PResult close() const { return PResult { true, lib("close").call_multi(*this) }; }

    //------------------ coroutine.yield(...) from inside a running coroutine
    template<class... A>
        requires(ToLua<A> && ...)
    static Results yield(A&&... a) {
        return lib("yield").call_multi(std::forward<A>(a)...);
    }

    static bool yieldable() { return clx::isyieldable(detail::current()); }

private:
    Thread(LState* L, const LValue& v, detail::adopt_t)
        : Value(L, v) { }

    static Value lib(const char* name) {
        LState* L = detail::current();
        Value co = Value::from_raw(L, clx::get_global(L, "coroutine"));
        return Value::from_raw(L, clx::table_get(L, co.raw(), clx::make_string(L, name)));
    }
};

//=====================================================================================================
//  State: owns (or wraps) an LState and makes it the current one
//=====================================================================================================
class State {
public:
    //------------------ Open a fresh clx state with the standard libraries
    State()
        : L_(clx::open())
        , owned_(true)
        , prev_(detail::g_current) {
        clx::openlibs(L_);
        detail::g_current = L_;
    }

    State(int argc, char** argv)
        : L_(clx::open(argc, argv))
        , owned_(true)
        , prev_(detail::g_current) {
        clx::openlibs(L_);
        detail::g_current = L_;
    }

    //------------------ Wrap an existing state (not closed on destruction)
    explicit State(LState* L)
        : L_(L)
        , owned_(false)
        , prev_(detail::g_current) {
        detail::g_current = L_;
    }

    ~State() {
        detail::g_current = prev_;
        if (owned_)
            clx::close(L_);
    }

    State(const State&) = delete;
    State& operator=(const State&) = delete;

    LState* raw() const noexcept { return L_; }
    static LState* current() { return detail::current(); }
    static LState* try_current() noexcept { return detail::g_current; }

    //------------------ The globals table (_G)
    Table globals() const { return Table::cast(L_, LValue(ValueType::Table, L_->_G)); }

    //------------------ Global variable access: L["print"]("hi"); L["answer"] = 42;
    template<class K>
        requires ToLua<K>
    Proxy operator[](K&& key) const {
        return globals()[std::forward<K>(key)];
    }

    template<class V>
        requires ToLua<V>
    void set(const char* name, V&& v) const {
        globals().set(name, std::forward<V>(v));
    }

    Value get(const char* name) const { return Value::from_raw(L_, clx::get_global(L_, name)); }

    //------------------ require("module")
    Value require(std::string_view name) const { return get("require")(name); }

private:
    LState* L_;
    bool owned_;
    LState* prev_;
};

//=====================================================================================================
//  Raising errors from C++
//=====================================================================================================
[[noreturn]] inline void error(std::string_view msg) {
    std::string s(msg);
    clx::error(detail::current(), s.c_str());
}

inline void check(bool cond, std::string_view msg) {
    if (!cond)
        error(msg);
}

//=====================================================================================================
//  Conversions: definitions
//=====================================================================================================
namespace detail {

    //------------------ Fill a new Lua table from a C++ range (array) or range of pairs (map)
    template<class R> LValue range_to_table(LState* L, R&& r) {
        using E = std::remove_cvref_t<std::ranges::range_reference_t<R>>;
        LValue t = L->create_table();
        RootRange rr(L, &t, 1);
        if constexpr (requires(const E& e) { e.first; e.second; }) {
            for (auto&& e : r) {
                Value k = Value::make(L, e.first);
                Value v = Value::make(L, e.second);
                clx::table_set(L, t, k.raw(), v.raw());
            }
        } else {
            int64_t i = 1;
            for (auto&& e : r) {
                Value v = Value::make(L, e);
                clx::raw_set_i(L, t, i++, v.raw());
            }
        }
        return t;
    }

} // namespace detail

template<class T> LValue to_lvalue(LState* L, T&& x) {
    using U = std::decay_t<T>;
    constexpr detail::Kind k = detail::kind_of<U>();
    static_assert(k != detail::Kind::none, "clx::lua: type is not convertible to a Lua value");

    if constexpr (k == detail::Kind::value)
        return x.raw();
    else if constexpr (k == detail::Kind::proxy)
        return x.get().raw();
    else if constexpr (k == detail::Kind::raw) {
        if constexpr (std::same_as<U, LValue>)
            return x;
        else
            return Convert<U>::to_lua(L, x);
    } else if constexpr (k == detail::Kind::nil)
        return LValue();
    else if constexpr (k == detail::Kind::boolean_obj)
        return LValue(x.value());
    else if constexpr (k == detail::Kind::number)
        return x.raw();
    else if constexpr (k == detail::Kind::boolean)
        return LValue(static_cast<bool>(x));
    else if constexpr (k == detail::Kind::integral)
        return LValue(static_cast<int64_t>(x));
    else if constexpr (k == detail::Kind::floating)
        return LValue(static_cast<double>(x));
    else if constexpr (k == detail::Kind::enumeration)
        return LValue(static_cast<int64_t>(x));
    else if constexpr (k == detail::Kind::optional)
        return x ? to_lvalue(L, *x) : LValue();
    else if constexpr (k == detail::Kind::text) {
        if constexpr (std::is_pointer_v<U>) {
            const U p = x;
            if (!p)
                return LValue();
        }
        std::string_view sv(x);
        return clx::make_string(L, sv.data(), sv.size());
    } else if constexpr (k == detail::Kind::callable)
        return detail::make_function(L, std::forward<T>(x));
    else
        return detail::range_to_table(L, x);
}

template<class T> T from_lvalue(LState* L, const LValue& v, int argn) {
    using U = std::remove_cv_t<T>;
    static_assert(detail::can_from<U>(), "clx::lua: type is not convertible from a Lua value");

    if constexpr (std::same_as<U, Value>)
        return Value::from_raw(L, v);
    else if constexpr (std::derived_from<U, Value>)
        return U::cast(L, v, argn);
    else if constexpr (std::same_as<U, LValue>)
        return v;
    else if constexpr (std::same_as<U, bool>)
        return v.as_bool();
    else if constexpr (std::same_as<U, Boolean>)
        return Boolean(v.as_bool());
    else if constexpr (std::same_as<U, Nil>) {
        if (v.type != ValueType::Nil)
            detail::bad_type(L, argn, "nil", v);
        return Nil { };
    } else if constexpr (std::same_as<U, Number>) {
        if (!clx::is_number(v))
            detail::bad_type(L, argn, "number", v);
        return Number::from_raw(v);
    } else if constexpr (std::integral<U> || std::is_enum_v<U>) {
        int64_t i;
        if (clx::to_integer(v, i))
            return static_cast<U>(i);
        double d;
        if (v.to_number(d)) {
            std::string msg = argn > 0 ? "bad argument #" + std::to_string(argn) + " (number has no integer representation)"
                                       : std::string("number has no integer representation");
            clx::error(L, msg.c_str());
        }
        detail::bad_type(L, argn, "number", v);
    } else if constexpr (std::floating_point<U>) {
        double d;
        if (!v.to_number(d))
            detail::bad_type(L, argn, "number", v);
        return static_cast<U>(d);
    } else if constexpr (std::same_as<U, std::string>) {
        if (v.type == ValueType::String)
            return std::string(v.as_string(), v.string_len());
        if (clx::is_number(v))
            return v.to_string(L);
        detail::bad_type(L, argn, "string", v);
    } else if constexpr (std::same_as<U, std::string_view>) {
        if (v.type != ValueType::String)
            detail::bad_type(L, argn, "string", v);
        return std::string_view(v.as_string(), v.string_len());
    } else if constexpr (std::same_as<U, const char*>) {
        if (v.type != ValueType::String)
            detail::bad_type(L, argn, "string", v);
        return v.as_string();
    } else if constexpr (detail::is_optional<U>::value) {
        if (v.type == ValueType::Nil)
            return std::nullopt;
        return U(from_lvalue<typename U::value_type>(L, v, argn));
    } else if constexpr (detail::is_vector<U>::value) {
        if (v.type != ValueType::Table)
            detail::bad_type(L, argn, "table", v);
        U out;
        int64_t n = clx::rawlen(v);
        out.reserve(static_cast<size_t>(n));
        for (int64_t i = 1; i <= n; ++i)
            out.push_back(from_lvalue<typename U::value_type>(L, clx::raw_get_i(L, v, i), argn));
        return out;
    } else
        return Convert<U>::from_lua(L, v, argn);
}

//=====================================================================================================
//  Streams, formatting, literals
//=====================================================================================================
template<class T>
    requires ValueLike<T>
std::ostream& operator<<(std::ostream& os, const T& v) {
    if constexpr (std::derived_from<T, Value>)
        return os << v.to_string();
    else
        return os << v.get().to_string();
}

inline std::ostream& operator<<(std::ostream& os, const Number& n) {
    return os << n.to_string();
}

inline std::ostream& operator<<(std::ostream& os, Nil) {
    return os << "nil";
}

namespace literals {

    //------------------ "text"_s -> lua::String in the current state
    inline String operator""_s(const char* s, size_t n) { return String(detail::current(), std::string_view(s, n)); }

} // namespace literals

} // namespace clx::lua

#if defined(__cpp_lib_format)
//------------------ std::format support: std::format("{}", table)
template<class T>
    requires std::derived_from<T, clx::lua::Value>
struct std::formatter<T, char> : std::formatter<std::string, char> {
    auto format(const T& v, std::format_context& ctx) const {
        return std::formatter<std::string, char>::format(v.to_string(), ctx);
    }
};

template<> struct std::formatter<clx::lua::Number, char> : std::formatter<std::string, char> {
    auto format(const clx::lua::Number& n, std::format_context& ctx) const {
        return std::formatter<std::string, char>::format(n.to_string(), ctx);
    }
};
#endif

#endif
