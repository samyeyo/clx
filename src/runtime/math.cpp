// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  math.cpp · Math Library                    │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include <cmath>
#include <random>

namespace clx {

//------------------ rng: returns thread-local Mersenne Twister RNG
static std::mt19937 &rng() {
    static std::mt19937 gen(std::random_device { }());
    return gen;
}

//------------------ math_abs: returns absolute value of a number
static MultiValue math_abs(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'abs' (number expected, got no value)");
    return MultiValue(clx::number(std::abs(clx::check_number(L, args[0]))));
}

//------------------ math_acos: returns arc cosine of a number
static MultiValue math_acos(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'acos' (number expected, got no value)");
    return MultiValue(clx::number(std::acos(clx::check_number(L, args[0]))));
}

//------------------ math_asin: returns arc sine of a number
static MultiValue math_asin(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'asin' (number expected, got no value)");
    return MultiValue(clx::number(std::asin(clx::check_number(L, args[0]))));
}

//------------------ math_atan: returns arc tangent (or atan2 if 2 args)
static MultiValue math_atan(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'atan' (number expected, got no value)");
    double y = clx::check_number(L, args[0]);
    if (count > 1) {
        double x = clx::check_number(L, args[1]);
        return MultiValue(clx::number(std::atan2(y, x)));
    }
    return MultiValue(clx::number(std::atan(y)));
}

//------------------ math_ceil: returns smallest integer >= x
static MultiValue math_ceil(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'ceil' (number expected, got no value)");
    if (clx::is_integer(args[0]))
        return MultiValue(args[0]);
    return MultiValue(clx::integer(static_cast<int64_t>(std::ceil(clx::check_number(L, args[0])))));
}

//------------------ math_cos: returns cosine of a number
static MultiValue math_cos(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'cos' (number expected, got no value)");
    return MultiValue(clx::number(std::cos(clx::check_number(L, args[0]))));
}

//------------------ math_deg: converts radians to degrees
static MultiValue math_deg(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'deg' (number expected, got no value)");
    return MultiValue(clx::number(clx::check_number(L, args[0]) * (180.0 / 3.14159265358979323846)));
}

//------------------ math_exp: returns e^x
static MultiValue math_exp(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'exp' (number expected, got no value)");
    return MultiValue(clx::number(std::exp(clx::check_number(L, args[0]))));
}

//------------------ math_floor: returns largest integer <= x
static MultiValue math_floor(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'floor' (number expected, got no value)");
    if (clx::is_integer(args[0]))
        return MultiValue(args[0]);
    return MultiValue(clx::integer(static_cast<int64_t>(std::floor(clx::check_number(L, args[0])))));
}

//------------------ math_fmod: returns remainder of x/y
static MultiValue math_fmod(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        clx::error(L, "bad argument #2 to 'fmod' (number expected, got no value)");
    return MultiValue(clx::number(std::fmod(clx::check_number(L, args[0]), clx::check_number(L, args[1]))));
}

//------------------ math_max: returns the maximum of its arguments
static MultiValue math_max(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'max' (number expected, got no value)");
    LValue m = args[0];
    for (size_t i = 1; i < count; ++i)
        if ((m < args[i]).as_bool())
            m = args[i];
    return MultiValue(m);
}

//------------------ math_min: returns the minimum of its arguments
static MultiValue math_min(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'min' (number expected, got no value)");
    LValue m = args[0];
    for (size_t i = 1; i < count; ++i)
        if ((args[i] < m).as_bool())
            m = args[i];
    return MultiValue(m);
}

//------------------ math_rad: converts degrees to radians
static MultiValue math_rad(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'rad' (number expected, got no value)");
    return MultiValue(clx::number(clx::check_number(L, args[0]) * (3.14159265358979323846 / 180.0)));
}

//------------------ math_sin: returns sine of a number
static MultiValue math_sin(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'sin' (number expected, got no value)");
    return MultiValue(clx::number(std::sin(clx::check_number(L, args[0]))));
}

//------------------ math_sqrt: returns square root of a number
static MultiValue math_sqrt(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'sqrt' (number expected, got no value)");
    return MultiValue(clx::number(std::sqrt(clx::check_number(L, args[0]))));
}

//------------------ math_tan: returns tangent of a number
static MultiValue math_tan(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'tan' (number expected, got no value)");
    return MultiValue(clx::number(std::tan(clx::check_number(L, args[0]))));
}

//------------------ math_tointeger: converts number to integer if possible
static MultiValue math_tointeger(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        return MultiValue();
    int64_t i;
    if (to_integer(args[0], i))
        return MultiValue(clx::integer(i));
    return MultiValue();
}

//------------------ math_frexp: breaks float into mantissa and exponent
static MultiValue math_frexp(LState *L, const LValue *args, size_t count) {
    double x = clx::check_number(L, args[0]);
    int e;
    double m = std::frexp(x, &e);
    return MultiValue({ clx::number(m), clx::integer(e) });
}

//------------------ math_ldexp: computes m * 2^e
static MultiValue math_ldexp(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        clx::error(L, "bad argument #2 to 'ldexp' (number expected, got no value)");
    double m = clx::check_number(L, args[0]);
    int64_t e = clx::check_integer(L, args[1]);
    return MultiValue(clx::number(std::ldexp(m, static_cast<int>(e))));
}

//------------------ math_log: returns log(x) or log(x, base)
static MultiValue math_log(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'log' (number expected, got no value)");
    double x = clx::check_number(L, args[0]);
    if (count > 1) {
        double base = clx::check_number(L, args[1]);
        return MultiValue(clx::number(std::log(x) / std::log(base)));
    }
    return MultiValue(clx::number(std::log(x)));
}

//------------------ math_modf: splits number into integer and fractional parts
static MultiValue math_modf(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, "bad argument #1 to 'modf' (number expected, got no value)");
    double x = clx::check_number(L, args[0]);
    double intpart;
    double fracpart = std::modf(x, &intpart);
    return MultiValue({ clx::number(intpart), clx::number(fracpart) });
}

//------------------ math_random: returns pseudo-random number
static MultiValue math_random(LState *L, const LValue *args, size_t count) {
    auto &gen = rng();
    if (count == 0) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return MultiValue(clx::number(dist(gen)));
    }
    int64_t m = clx::check_integer(L, args[0]);
    if (count == 1) {
        if (m == 0) {
            std::uniform_int_distribution<int64_t> dist;
            return MultiValue(clx::integer(dist(gen)));
        }
        if (m > 0) {
            std::uniform_int_distribution<int64_t> dist(1, m);
            return MultiValue(clx::integer(dist(gen)));
        }
        clx::error(L, "bad argument #1 to 'random' (interval is empty)");
    }
    int64_t n = clx::check_integer(L, args[1]);
    if (m > n)
        clx::error(L, "bad argument #2 to 'random' (interval is empty)");
    std::uniform_int_distribution<int64_t> dist(m, n);
    return MultiValue(clx::integer(dist(gen)));
}

//------------------ math_randomseed: seeds the random number generator
static MultiValue math_randomseed(LState *L, const LValue *args, size_t count) {
    auto &gen = rng();
    if (count == 0) {
        std::random_device rd;
        uint64_t seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        gen.seed(static_cast<std::mt19937::result_type>(seed));
        return MultiValue({ clx::integer(static_cast<int64_t>(seed >> 32)),
            clx::integer(static_cast<int64_t>(seed & 0xFFFFFFFFULL)) });
    }
    int64_t x = clx::check_integer(L, args[0]);
    int64_t y = (count > 1) ? clx::check_integer(L, args[1]) : 0;
    uint64_t seed = (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
    gen.seed(static_cast<std::mt19937::result_type>(seed));
    return MultiValue({ clx::integer(x), clx::integer(y) });
}

//------------------ math_type: returns "integer" or "float" for a number
static MultiValue math_type(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        return MultiValue();
    ValueType t = args[0].type;
    if (t == Int64)
        return MultiValue(LValue(L->intern_string("integer")));
    if (t == Double)
        return MultiValue(LValue(L->intern_string("float")));
    return MultiValue();
}

//------------------ math_ult: unsigned less-than comparison of integers
static MultiValue math_ult(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        clx::error(L, "bad argument #2 to 'ult' (number expected, got no value)");
    uint64_t a = static_cast<uint64_t>(clx::check_integer(L, args[0]));
    uint64_t b = static_cast<uint64_t>(clx::check_integer(L, args[1]));
    return MultiValue(LValue(a < b));
}

//------------------ math_funcs: lazy registration table for math library functions
static constexpr LazyReg math_funcs[] = {
    { "abs", math_abs },
    { "acos", math_acos },
    { "asin", math_asin },
    { "atan", math_atan },
    { "ceil", math_ceil },
    { "cos", math_cos },
    { "deg", math_deg },
    { "exp", math_exp },
    { "floor", math_floor },
    { "fmod", math_fmod },
    { "max", math_max },
    { "min", math_min },
    { "rad", math_rad },
    { "sin", math_sin },
    { "sqrt", math_sqrt },
    { "tan", math_tan },
    { "tointeger", math_tointeger },
    { "frexp", math_frexp },
    { "ldexp", math_ldexp },
    { "log", math_log },
    { "modf", math_modf },
    { "random", math_random },
    { "randomseed", math_randomseed },
    { "type", math_type },
    { "ult", math_ult },
};

//------------------ luastd_math: registers the math library into the global state
void luastd_math(LState *L) {
    LValue math_table = clx::table(L);
    clx::set_value(L, math_table, "pi", clx::number(3.14159265358979323846));
    clx::set_value(L, math_table, "huge", clx::number(INFINITY));
    clx::set_value(L, math_table, "maxinteger", clx::integer(9223372036854775807LL));
    clx::set_value(L, math_table, "mininteger", clx::integer(-9223372036854775807LL - 1));
    clx::set_lazy_funcs(L, math_table, math_funcs, 25);
    clx::set_global(L, "math", math_table);
}

}