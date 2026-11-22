// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  strings.cpp · String Library               │
// └─────────────────────────────────────────────┘

#include "../include/clx_simd.h"
#include "clx_runtime.h"
#include "internal.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>

namespace clx {

//------------------ get_string — extract string arg with bounds/type checking (stock luaL_checkstring errors)
static const char *get_string(LState *L, const LValue *args, size_t count, size_t &len, int idx, const char *fname) {
    if (idx < 1 || idx > (int)count)
        throw_runtime_error(bad_arg_msg(L, idx, fname, "string expected, got no value"));
    const LValue &v = args[idx - 1];
    if (v.type == String) {
        len = v.string_len();
        return v.as_string();
    }
    if (v.type == Int64) {
        char buf[32];
        int n = std::snprintf(buf, sizeof(buf), "%lld", (long long)v.as_integer());
        const char *s = L->intern_string(buf, (size_t)n);
        len = (size_t)n;
        return s;
    }
    if (v.type == Double) {
        char buf[64];
        int n = clx_format_double(buf, sizeof(buf), v.as_number());
        const char *s = L->intern_string(buf, (size_t)n);
        len = (size_t)n;
        return s;
    }
    throw_runtime_error(
        bad_arg_msg(L, idx, fname, std::string("string expected, got ") + stock_type_name(v)));
    return nullptr;
}

//------------------ get_integer — extract integer arg; errors like stock luaL_checkinteger on lossy floats
static int64_t get_integer(LState *L, const LValue *args, size_t count, int idx, const char *fname) {
    if (idx < 1 || idx > (int)count)
        throw_runtime_error(bad_arg_msg(L, idx, fname, "number expected, got no value"));
    int64_t i;
    if (to_integer(args[idx - 1], i))
        return i;
    double d;
    if (args[idx - 1].to_number(d))
        throw_runtime_error(bad_arg_msg(L, idx, fname, "number has no integer representation"));
    throw_runtime_error(
        bad_arg_msg(L, idx, fname, std::string("number expected, got ") + stock_type_name(args[idx - 1])));
}

//------------------ posrelat — convert relative position to absolute (1-based)
static size_t posrelat(int64_t pos, size_t len) {
    if (pos > 0)
        return (size_t)pos;
    else if (pos == 0)
        return 1;
    else if (pos < -(int64_t)len)
        return 1;
    else
        return len + (size_t)pos + 1;
}

//------------------ getendpos — convert relative end position to absolute
static size_t getendpos(int64_t pos, size_t len) {
    if (pos > (int64_t)len)
        return len;
    else if (pos >= 0)
        return (size_t)pos;
    else if (pos < -(int64_t)len)
        return 0;
    else
        return len + (size_t)pos + 1;
}

//------------------ str_len — string.len: return string length
MultiValue str_len(LState *L, const LValue *args, size_t count) {
    size_t l;
    get_string(L, args, count, l, 1, "string.len");
    return MultiValue(static_cast<int64_t>(l));
}

//------------------ str_sub — string.sub: extract substring by range
MultiValue str_sub(LState *L, const LValue *args, size_t count) {
    size_t l;
    const char *s = get_string(L, args, count, l, 1, "string.sub");
    int64_t start_i = (count >= 2 && args[1].type != Nil) ? get_integer(L, args, count, 2, "string.sub") : 1;
    int64_t end_i = (count >= 3 && args[2].type != Nil) ? get_integer(L, args, count, 3, "string.sub") : (int64_t)l;

    size_t start, end;
    if (start_i >= 1 && end_i >= start_i && (size_t)end_i <= l) {
        start = (size_t)start_i;
        end = (size_t)end_i;
    } else {
        start = posrelat(start_i, l);
        end = getendpos(end_i, l);
    }

    if (start <= end) {
        size_t subl = end - start + 1;
        const char *src = s + start - 1;

        return MultiValue(make_string_pooled(L, src, subl));
    } else {
        return MultiValue(LValue(L->intern_string("", 0)));
    }
}

//------------------ str_reverse — string.reverse: reverse string characters
MultiValue str_reverse(LState *L, const LValue *args, size_t count) {
    size_t l;
    const char *s = get_string(L, args, count, l, 1, "string.reverse");
    if (l <= 6) {
        char buf[8];
        for (size_t i = 0; i < l; i++)
            buf[i] = s[l - i - 1];
        return MultiValue(LValue::istr(buf, static_cast<uint32_t>(l)));
    }
    char *dst = new char[l + 1];
    clx_reverse_bytes(dst, s, l);
    dst[l] = '\0';
    MultiValue result = MultiValue(make_string_pooled(L, dst, l));
    delete[] dst;
    return result;
}

//------------------ fold_high_bytes: outside the C locale the host's case tables also cover bytes above ASCII
static void fold_high_bytes(char *dst, size_t len, int to_lower) {
    if (ctype_is_c())
        return;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(dst[i]);
        if (c >= 0x80)
            dst[i] = static_cast<char>(to_lower ? std::tolower(c) : std::toupper(c));
    }
}

//------------------ str_lower — string.lower: convert to lowercase
MultiValue str_lower(LState *L, const LValue *args, size_t count) {
    size_t l;
    const char *s = get_string(L, args, count, l, 1, "string.lower");
    if (l <= 6) {
        char buf[8];
        clx_case_fold(buf, s, l, 1);
        fold_high_bytes(buf, l, 1);
        return MultiValue(LValue::istr(buf, static_cast<uint32_t>(l)));
    }
    char *dst = new char[l + 1];
    clx_case_fold(dst, s, l, 1);
    fold_high_bytes(dst, l, 1);
    dst[l] = '\0';
    MultiValue result = MultiValue(make_string_pooled(L, dst, l));
    delete[] dst;
    return result;
}

//------------------ str_upper — string.upper: convert to uppercase
MultiValue str_upper(LState *L, const LValue *args, size_t count) {
    size_t l;
    const char *s = get_string(L, args, count, l, 1, "string.upper");
    if (l <= 6) {
        char buf[8];
        clx_case_fold(buf, s, l, 0);
        fold_high_bytes(buf, l, 0);
        return MultiValue(LValue::istr(buf, static_cast<uint32_t>(l)));
    }
    char *dst = new char[l + 1];
    clx_case_fold(dst, s, l, 0);
    fold_high_bytes(dst, l, 0);
    dst[l] = '\0';
    MultiValue result = MultiValue(make_string_pooled(L, dst, l));
    delete[] dst;
    return result;
}

//------------------ str_rep: string.rep, N repetitions with an optional separator (only an explicit nil means 1)
MultiValue str_rep(LState *L, const LValue *args, size_t count) {
    size_t len;
    const char *s = get_string(L, args, count, len, 1, "string.rep");
    if (count < 2)
        throw_runtime_error(bad_arg_msg(L, 2, "string.rep", "number expected, got no value"));
    int64_t n = (args[1].type != Nil) ? get_integer(L, args, count, 2, "string.rep") : 1;
    size_t lsep = 0;
    const char *sep = "";
    if (count >= 3 && args[2].type == String) {
        sep = args[2].as_string();
        lsep = args[2].string_len();
    }
    if (n <= 0 || (len | lsep) == 0)
        return MultiValue(LValue(L->intern_string("", 0)));
    if (len + lsep < len || static_cast<uint64_t>(n) > (INT64_MAX - lsep) / (len + lsep))
        throw_runtime_error("resulting string too large");
    size_t totallen = ((size_t)n * (len + lsep)) - lsep;
    if (totallen <= 6) {
        char buf[8];
        char *bp = buf;
        for (int64_t i = 0; i < n - 1; i++) {
            std::memcpy(bp, s, len);
            bp += len;
            if (lsep > 0) {
                std::memcpy(bp, sep, lsep);
                bp += lsep;
            }
        }
        std::memcpy(bp, s, len);
        return MultiValue(LValue::istr(buf, static_cast<uint32_t>(totallen)));
    }
    char *dst = new char[totallen + 1];
    char *p = dst;
    for (int64_t i = 0; i < n - 1; i++) {
        std::memcpy(p, s, len);
        p += len;
        if (lsep > 0) {
            std::memcpy(p, sep, lsep);
            p += lsep;
        }
    }
    std::memcpy(p, s, len);
    dst[totallen] = '\0';
    MultiValue result = MultiValue(make_string_pooled(L, dst, totallen));
    delete[] dst;
    return result;
}

//------------------ str_byte — string.byte: get byte codes of substring
MultiValue str_byte(LState *L, const LValue *args, size_t count) {
    size_t l;
    const char *s = get_string(L, args, count, l, 1, "string.byte");
    int64_t pi = (count >= 2 && args[1].type != Nil) ? get_integer(L, args, count, 2, "string.byte") : 1;
    size_t posi = posrelat(pi, l);
    int64_t pe = (count >= 3 && args[2].type != Nil) ? get_integer(L, args, count, 3, "string.byte") : pi;
    size_t pose = getendpos(pe, l);
    if (posi > pose)
        return MultiValue();
    int n = (int)(pose - posi) + 1;
    LValue vals[8];
    LValue *vals_ptr = vals;
    std::vector<LValue> large_vals;
    if (n > 8) {
        large_vals.resize(n);
        vals_ptr = large_vals.data();
    }
    for (int i = 0; i < n; i++)
        vals_ptr[i] = LValue(static_cast<int64_t>((unsigned char)s[posi + i - 1]));
    if (n == 1)
        return MultiValue(vals_ptr[0]);
    return MultiValue(vals_ptr, n, L);
}

//------------------ str_char — string.char: build string from byte values
MultiValue str_char(LState *L, const LValue *args, size_t count) {
    if (count <= 6) {
        char buf[8];
        for (size_t i = 0; i < count; i++) {
            int64_t c;
            if (!to_integer(args[i], c) || c < 0 || c > 255) {
                throw_runtime_error(bad_arg_msg(L, (int)(i + 1), "string.char", "value out of range"));
            }
            buf[i] = (char)(unsigned char)c;
        }
        return MultiValue(LValue::istr(buf, count));
    }
    char *dst = new char[count + 1];
    for (size_t i = 0; i < count; i++) {
        int64_t c;
        if (!to_integer(args[i], c) || c < 0 || c > 255) {
            delete[] dst;
            throw_runtime_error(bad_arg_msg(L, (int)(i + 1), "string.char", "value out of range"));
        }
        dst[i] = (char)(unsigned char)c;
    }
    dst[count] = '\0';
    MultiValue result = MultiValue(make_string_pooled(L, dst, count));
    delete[] dst;
    return result;
}

//------------------ str_dump: VM-backed under --dynamic; in bare builds a function dumps to its own callable snapshot
MultiValue str_dump(LState *L, const LValue *args, size_t count) {
    if (clx_string_dump_hook)
        return clx_string_dump_hook(L, args, count);
    if (count < 1 || args[0].type != Function || args[0].as_pointer() == nullptr)
        throw_runtime_error(bad_arg_msg(L, 1, "string.dump", "Lua function expected"));
    return MultiValue(args[0]);
}

//------------------ get2digits — skip up to 2 decimal digits (stock lstrlib.c)
static const char *get2digits(const char *s) {
    if (std::isdigit((unsigned char)*s))
        s++;
    if (std::isdigit((unsigned char)*s))
        s++;
    return s;
}

//---------- checkformat — validate flags/width/precision of a conversion form (stock lstrlib.c)
static void checkformat(const char *form, const char *flags, bool precision) {
    const char *spec = form + 1;
    spec += std::strspn(spec, flags);
    if (*spec != '0') {
        spec = get2digits(spec);
        if (*spec == '.' && precision) {
            spec++;
            spec = get2digits(spec);
        }
    }
    if (!std::isalpha((unsigned char)*spec)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "invalid conversion specification: '%s'", form);
        throw_runtime_error(buf);
    }
}

//------------------ addlenmod — insert a length modifier before the conversion char (stock lstrlib.c)
static void addlenmod(char *form, const char *lenmod) {
    size_t l = std::strlen(form);
    size_t lm = std::strlen(lenmod);
    char spec = form[l - 1];
    std::memcpy(form + l - 1, lenmod, lm);
    form[l + lm - 1] = spec;
    form[l + lm] = '\0';
}

//---------- throw_format_arg_error — stock luaL_argerror wording for string.format
static void throw_format_arg_error(LState *L, int idx, const char *what) {
    throw_runtime_error(bad_arg_msg(L, idx, "string.format", what));
}

//------------------ str_format — string.format: printf-style formatting (stock lstrlib.c semantics)
MultiValue str_format(LState *L, const LValue *args, size_t count) {
    size_t sfl;
    const char *strfrmt = get_string(L, args, count, sfl, 1, "string.format");
    const char *end = strfrmt + sfl;
    int arg = 1;

    StringBuilder result;
    if (sfl > 32)
        result.reserve(sfl / 4 + 8);
    while (strfrmt < end) {
        if (*strfrmt != '%') {
            const char *lit = strfrmt;
            while (strfrmt < end && *strfrmt != '%')
                strfrmt++;
            result.append(lit, static_cast<uint32_t>(strfrmt - lit));
            continue;
        }
        strfrmt++;
        if (strfrmt < end && *strfrmt == '%') {
            result.append("%", 1);
            strfrmt++;
            continue;
        }
        if (++arg > (int)count)
            throw_format_arg_error(L, arg, "no value");

        static const char kSpanChars[] = "-+#0 123456789.";
        size_t span = 0;
        while (strfrmt + span < end && strfrmt[span] != '\0' && std::strchr(kSpanChars, strfrmt[span]))
            span++;
        char form[64];
        if (span + 1 >= 22)
            throw_runtime_error("invalid format (too long)");
        form[0] = '%';
        std::memcpy(form + 1, strfrmt, span);
        char spec = (strfrmt + span < end) ? strfrmt[span] : '\0';
        form[span + 1] = spec;
        form[span + 2] = '\0';
        strfrmt = (strfrmt + span + 1 <= end) ? strfrmt + span + 1 : end;

        char buf[512];
        int written = 0;

        switch (spec) {
        case 'c': {
            checkformat(form, "-", false);
            int64_t n = get_integer(L, args, count, arg, "string.format");
            written = std::snprintf(buf, sizeof(buf), form, (int)n);
            break;
        }
        case 'd':
        case 'i': {
            int64_t n = get_integer(L, args, count, arg, "string.format");
            checkformat(form, "-+0 ", true);
            addlenmod(form, "ll");
            written = std::snprintf(buf, sizeof(buf), form, (long long)n);
            break;
        }
        case 'u': {
            int64_t n = get_integer(L, args, count, arg, "string.format");
            checkformat(form, "-0", true);
            addlenmod(form, "ll");
            written = std::snprintf(buf, sizeof(buf), form, (unsigned long long)n);
            break;
        }
        case 'o':
        case 'x':
        case 'X': {
            int64_t n = get_integer(L, args, count, arg, "string.format");
            checkformat(form, "-#0", true);
            addlenmod(form, "ll");
            written = std::snprintf(buf, sizeof(buf), form, (unsigned long long)n);
            break;
        }
        case 'a':
        case 'A': {
            checkformat(form, "-+#0 ", true);
            double d;
            if (!args[arg - 1].to_number(d))
                throw_format_arg_error(L, arg, "number expected");
            written = std::snprintf(buf, sizeof(buf), form, d);
            break;
        }
        case 'f':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            double d;
            if (!args[arg - 1].to_number(d))
                throw_format_arg_error(L, arg, "number expected");
            checkformat(form, "-+#0 ", true);
            written = std::snprintf(buf, sizeof(buf), form, d);
            break;
        }
        case 'p': {
            checkformat(form, "-", false);
            //------------------ stock formats lua_topointer, non-NULL for every GC value: report the object address, not null
            const void *ptr = nullptr;
            switch (args[arg - 1].type) {
            case ValueType::Table:
            case ValueType::Function:
            case ValueType::Thread:
            case ValueType::UserData:
            case ValueType::LightUserData:
                ptr = args[arg - 1].as_pointer();
                break;
            case ValueType::String:
                ptr = args[arg - 1].as_string();
                break;
            default:
                break;
            }
            if (ptr == nullptr) {
                ptr = "(null)";
                form[std::strlen(form) - 1] = 's';
            }
            written = std::snprintf(buf, sizeof(buf), form, ptr);
            break;
        }
        case 'q': {
            if (form[2] != '\0')
                throw_runtime_error("specifier '%q' cannot have modifiers");
            const LValue &qv = (arg <= count) ? args[arg - 1] : LValue();
            //------------------ stock addliteral: numbers get a Lua numeral, nil/boolean their keyword, nothing else has one
            if (qv.type == ValueType::Int64 || qv.type == ValueType::Double) {
                char lit[64];
                int nw;
                if (qv.type == ValueType::Int64) {
                    //------------------ mininteger has no decimal literal that reads back as an integer
                    if (qv.val.payload.i64 == INT64_MIN)
                        nw = std::snprintf(lit, sizeof(lit), "0x%llx", static_cast<unsigned long long>(qv.val.payload.i64));
                    else
                        nw = std::snprintf(lit, sizeof(lit), "%lld", static_cast<long long>(qv.val.payload.i64));
                } else {
                    double d = qv.val.payload.f64;
                    if (d == HUGE_VAL)
                        nw = std::snprintf(lit, sizeof(lit), "1e9999");
                    else if (d == -HUGE_VAL)
                        nw = std::snprintf(lit, sizeof(lit), "-1e9999");
                    else if (d != d)
                        nw = std::snprintf(lit, sizeof(lit), "(0/0)");
                    else {
                        nw = std::snprintf(lit, sizeof(lit), "%a", d);
                        //------------------ a locale decimal comma would make the numeral unparsable
                        for (int _ci = 0; _ci < nw; ++_ci)
                            if (lit[_ci] == ',')
                                lit[_ci] = '.';
                    }
                }
                if (nw > 0)
                    result.append_owned(lit, static_cast<uint32_t>(nw));
                break;
            }
            if (qv.type == ValueType::Nil || qv.type == ValueType::Boolean) {
                std::string kw = qv.to_string(L);
                result.append_owned(kw.data(), static_cast<uint32_t>(kw.size()));
                break;
            }
            if (qv.type != ValueType::String)
                throw_runtime_error("bad argument #" + std::to_string(arg)
                    + " to 'format' (value has no literal form)");
            size_t slen = qv.string_len();
            const char *ss = qv.as_string();
            result.append("\"", 1);
            for (size_t i = 0; i < slen; i++) {
                unsigned char qc = (unsigned char)ss[i];
                if (qc == '"' || qc == '\\' || qc == '\n') {
                    char esc[2] = { '\\', (char)qc };
                    result.append_owned(esc, 2);
                } else if (std::iscntrl(qc)) {
                    int nextc = (i + 1 < slen) ? (unsigned char)ss[i + 1] : 0;
                    char esc[8];
                    int nw = std::snprintf(esc, sizeof(esc), std::isdigit(nextc) ? "\\%03d" : "\\%d", (int)qc);
                    if (nw > 0)
                        result.append_owned(esc, (uint32_t)nw);
                } else {
                    result.append(ss + i, 1);
                }
            }
            result.append("\"", 1);
            break;
        }
        case 's': {
            const LValue &sv_arg = args[arg - 1];
            if (sv_arg.type == String && form[2] == '\0') {
                result.append(sv_arg.as_string(), sv_arg.string_len());
                break;
            }
            std::string sv = sv_arg.to_string(L);
            size_t l = sv.size();
            if (form[2] == '\0') {
                result.append_owned(sv.data(), static_cast<uint32_t>(l));
                break;
            }
            if (std::memchr(sv.data(), 0, l) != nullptr)
                throw_format_arg_error(L, arg, "string contains zeros");
            checkformat(form, "-", true);
            if (std::strchr(form, '.') == nullptr && l >= 100) {
                result.append_owned(sv.data(), static_cast<uint32_t>(l));
                break;
            }
            written = std::snprintf(buf, sizeof(buf), form, sv.c_str());
            break;
        }
        default: {
            std::string msg = "invalid conversion '" + std::string(form) + "' to 'format'";
            throw_runtime_error(msg.c_str());
        }
        }
        if (written > 0)
            result.append_owned(buf, static_cast<uint32_t>(written));
    }
    return MultiValue(result.to_lvalue(L));
}

#define LUA_MAXCAPTURES 32
#define CAP_UNFINISHED (-1)
#define CAP_POSITION (-2)
#define L_ESC '%'
#define SPECIALS "^$*+?.([%-"
#define MAXCCALLS 200

//------------------ MatchState — pattern-matching state (captures, depth, bounds)
struct MatchState {
    const char *src_init;
    const char *src_end;
    const char *p_end;
    LState *L;
    int matchdepth;
    int level;

    struct {
        const char *init;
        ptrdiff_t len;
    } capture[LUA_MAXCAPTURES];
};

//------------------ check_capture — validate capture index, throw if invalid
static int check_capture(MatchState *ms, int l) {
    l -= '1';
    if (l < 0 || l >= ms->level || ms->capture[l].len == CAP_UNFINISHED) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "invalid capture index %%%d", l + 1);
        throw_runtime_error(buf);
    }
    return l;
}

//------------------ capture_to_close — find nearest unfinished capture
static int capture_to_close(MatchState *ms) {
    int level = ms->level;
    for (level--; level >= 0; level--)
        if (ms->capture[level].len == CAP_UNFINISHED)
            return level;
    throw_runtime_error("invalid pattern capture");
}

static const char *match(MatchState *ms, const char *s, const char *p);

//------------------ classend — skip past a character class (incl. brackets/escapes)
static const char *classend(MatchState *ms, const char *p) {
    switch (*p++) {
    case L_ESC:
        if (p == ms->p_end)
            throw_runtime_error("malformed pattern (ends with '%')");
        return p + 1;
    case '[':
        if (*p == '^')
            p++;
        do {
            if (p == ms->p_end)
                throw_runtime_error("malformed pattern (missing ']')");
            if (*p == L_ESC && *(p + 1) != '\0')
                p++;
            p++;
        } while (*p != ']');
        return p + 1;
    default:
        return p;
    }
}

//------------------ match_class — test char against Lua character class (%a, %d, etc.)
struct MatchClassTable {
    unsigned short bit[256];

    MatchClassTable() { rebuild(); }

    void rebuild() {
        for (int c = 0; c < 256; ++c) {
            unsigned short b = 0;
            if (std::isalpha(c))
                b |= 1 << 0;
            if (std::iscntrl(c))
                b |= 1 << 1;
            if (std::isdigit(c))
                b |= 1 << 2;
            if (std::isgraph(c))
                b |= 1 << 3;
            if (std::islower(c))
                b |= 1 << 4;
            if (std::ispunct(c))
                b |= 1 << 5;
            if (std::isspace(c))
                b |= 1 << 6;
            if (std::isupper(c))
                b |= 1 << 7;
            if (std::isalnum(c))
                b |= 1 << 8;
            if (std::isxdigit(c))
                b |= 1 << 9;
            bit[c] = b;
        }
    }
};

//------------------ g_match_class: class bits for the locale it was built in, rebuilt when os.setlocale bumps the generation
static MatchClassTable g_match_class;
static uint64_t g_match_class_generation = 0;

//------------------ sync_match_class: called once per pattern operation, never per character
static void sync_match_class() {
    uint64_t gen = locale_generation();
    if (gen != g_match_class_generation) {
        g_match_class.rebuild();
        g_match_class_generation = gen;
    }
}

static int match_class(int c, int cl) {
    const MatchClassTable &tbl = g_match_class;
    int res;
    switch (std::tolower(cl)) {
    case 'a':
        res = (tbl.bit[(unsigned char)c] & (1 << 0)) != 0;
        break;
    case 'c':
        res = (tbl.bit[(unsigned char)c] & (1 << 1)) != 0;
        break;
    case 'd':
        res = (tbl.bit[(unsigned char)c] & (1 << 2)) != 0;
        break;
    case 'g':
        res = (tbl.bit[(unsigned char)c] & (1 << 3)) != 0;
        break;
    case 'l':
        res = (tbl.bit[(unsigned char)c] & (1 << 4)) != 0;
        break;
    case 'p':
        res = (tbl.bit[(unsigned char)c] & (1 << 5)) != 0;
        break;
    case 's':
        res = (tbl.bit[(unsigned char)c] & (1 << 6)) != 0;
        break;
    case 'u':
        res = (tbl.bit[(unsigned char)c] & (1 << 7)) != 0;
        break;
    case 'w':
        res = (tbl.bit[(unsigned char)c] & (1 << 8)) != 0;
        break;
    case 'x':
        res = (tbl.bit[(unsigned char)c] & (1 << 9)) != 0;
        break;
    case 'z':
        res = (c == 0);
        break;
    default:
        return (cl == c);
    }
    return (std::islower(cl) ? res : !res);
}

//------------------ matchbracketclass — test char against [...] bracket expression
static int matchbracketclass(int c, const char *p, const char *ec) {
    int sig = 1;
    if (*(p + 1) == '^') {
        sig = 0;
        p++;
    }
    while (++p < ec) {
        if (*p == L_ESC) {
            p++;
            if (match_class((unsigned char)c, (unsigned char)*p))
                return sig;
        } else if ((*(p + 1) == '-') && (p + 2 < ec)) {
            p += 2;
            if ((unsigned char)*(p - 2) <= (unsigned char)c && (unsigned char)c <= (unsigned char)*p)
                return sig;
        } else if ((unsigned char)*p == (unsigned char)c)
            return sig;
    }
    return !sig;
}

//------------------ singlematch — match one character against a class (no quantifier)
static int singlematch(MatchState *ms, const char *s, const char *p, const char *ep) {
    if (s >= ms->src_end)
        return 0;
    int c = (unsigned char)*s;
    switch (*p) {
    case '.':
        return 1;
    case L_ESC:
        return match_class(c, (unsigned char)*(p + 1));
    case '[':
        return matchbracketclass(c, p, ep - 1);
    default:
        return ((unsigned char)*p == (unsigned char)c);
    }
}

//------------------ matchbalance — match %bxy balanced pair
static const char *matchbalance(MatchState *ms, const char *s, const char *p) {
    if (p >= ms->p_end - 1)
        throw_runtime_error("malformed pattern (missing arguments to '%b')");
    if (*s != *p)
        return NULL;
    int b = *p;
    int e = *(p + 1);
    int cont = 1;
    while (++s < ms->src_end) {
        if (*s == e) {
            if (--cont == 0)
                return s + 1;
        } else if (*s == b)
            cont++;
    }
    return NULL;
}

//------------------ max_expand — greedy quantifier (*) match
static const char *max_expand(MatchState *ms, const char *s, const char *p, const char *ep) {
    ptrdiff_t i = 0;
    while (singlematch(ms, s + i, p, ep))
        i++;
    while (i >= 0) {
        const char *res = match(ms, s + i, ep + 1);
        if (res)
            return res;
        i--;
    }
    return NULL;
}

//------------------ min_expand — non-greedy quantifier (-) match
static const char *min_expand(MatchState *ms, const char *s, const char *p, const char *ep) {
    for (;;) {
        const char *res = match(ms, s, ep + 1);
        if (res)
            return res;
        else if (singlematch(ms, s, p, ep))
            s++;
        else
            return NULL;
    }
}

//------------------ start_capture — begin a capture group (open paren)
static const char *start_capture(MatchState *ms, const char *s, const char *p, int what) {
    int level = ms->level;
    if (level >= LUA_MAXCAPTURES)
        throw_runtime_error("too many captures");
    ms->capture[level].init = s;
    ms->capture[level].len = what;
    ms->level = level + 1;
    const char *res = match(ms, s, p);
    if (res == NULL)
        ms->level--;
    return res;
}

//------------------ end_capture — close a capture group (close paren)
static const char *end_capture(MatchState *ms, const char *s, const char *p) {
    int l = capture_to_close(ms);
    ms->capture[l].len = s - ms->capture[l].init;
    const char *res = match(ms, s, p);
    if (res == NULL)
        ms->capture[l].len = CAP_UNFINISHED;
    return res;
}

//------------------ match_capture — match previously captured group (%1..%9)
static const char *match_capture(MatchState *ms, const char *s, int l) {
    l = check_capture(ms, l);
    size_t len = (size_t)ms->capture[l].len;
    if ((size_t)(ms->src_end - s) >= len && std::memcmp(ms->capture[l].init, s, len) == 0)
        return s + len;
    return NULL;
}

//------------------ match — main recursive pattern-matching engine
static const char *match(MatchState *ms, const char *s, const char *p) {
    if (ms->matchdepth-- == 0)
        throw_runtime_error(file_line_prefix(ms->L) + "pattern too complex");
init:
    if (p != ms->p_end) {
        switch (*p) {
        case '(':
            s = (*(p + 1) == ')') ? start_capture(ms, s, p + 2, CAP_POSITION)
                                  : start_capture(ms, s, p + 1, CAP_UNFINISHED);
            break;
        case ')':
            s = end_capture(ms, s, p + 1);
            break;
        case '$':
            if ((p + 1) != ms->p_end)
                goto dflt;
            s = (s == ms->src_end) ? s : NULL;
            break;
        case L_ESC:
            switch (*(p + 1)) {
            case 'b': {
                s = matchbalance(ms, s, p + 2);
                if (s) {
                    p += 4;
                    goto init;
                }
                break;
            }
            case 'f': {
                const char *ep;
                char previous;
                p += 2;
                if (*p != '[')
                    throw_runtime_error("missing '[' after '%f' in pattern");
                ep = classend(ms, p);
                previous = (s == ms->src_init) ? '\0' : *(s - 1);
                if (!matchbracketclass((unsigned char)previous, p, ep - 1)
                    && matchbracketclass((unsigned char)*s, p, ep - 1)) {
                    p = ep;
                    goto init;
                }
                s = NULL;
                break;
            }
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                s = match_capture(ms, s, (unsigned char)*(p + 1));
                if (s) {
                    p += 2;
                    goto init;
                }
                break;
            default:
                goto dflt;
            }
            break;
        default:
        dflt: {
            const char *ep = classend(ms, p);
            if (!singlematch(ms, s, p, ep)) {
                if (*ep == '*' || *ep == '?' || *ep == '-') {
                    p = ep + 1;
                    goto init;
                } else
                    s = NULL;
            } else {
                switch (*ep) {
                case '?': {
                    const char *res = match(ms, s + 1, ep + 1);
                    if (res)
                        s = res;
                    else {
                        p = ep + 1;
                        goto init;
                    }
                    break;
                }
                case '+':
                    s++;
                case '*':
                    s = max_expand(ms, s, p, ep);
                    break;
                case '-':
                    s = min_expand(ms, s, p, ep);
                    break;
                default:
                    s++;
                    p = ep;
                    goto init;
                }
            }
            break;
        }
        }
    }
    ms->matchdepth++;
    return s;
}

//------------------ lmemfind — find substring in memory (plain-text search)
static const char *lmemfind(const char *s1, size_t l1, const char *s2, size_t l2) {
    if (l2 == 0)
        return s1;
    if (l2 > l1)
        return NULL;
    const char *init;
    l2--;
    l1 = l1 - l2;
    while (l1 > 0 && (init = (const char *)std::memchr(s1, *s2, l1)) != NULL) {
        init++;
        if (std::memcmp(init, s2 + 1, l2) == 0)
            return init - 1;
        l1 -= (size_t)(init - s1);
        s1 = init;
    }
    return NULL;
}

//------------------ get_onecapture — retrieve a single capture by index
static ptrdiff_t get_onecapture(MatchState *ms, int i, const char *s, const char *e, const char **cap) {
    if (i >= ms->level) {
        if (i != 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "invalid capture index %%%d", i + 1);
            throw_runtime_error(buf);
        }
        *cap = s;
        return (e - s);
    } else {
        ptrdiff_t capl = ms->capture[i].len;
        *cap = ms->capture[i].init;
        if (capl == CAP_UNFINISHED)
            throw_runtime_error("unfinished capture");
        return capl;
    }
}

//------------------ prepstate — initialise MatchState with source & pattern
static void prepstate(MatchState *ms, LState *L, const char *s, size_t ls, const char *p, size_t lp) {
    ms->L = L;
    ms->src_init = s;
    ms->src_end = s + ls;
    ms->p_end = p + lp;
}

//------------------ reprepstate — reset match depth and captures for new attempt
static void reprepstate(MatchState *ms) {
    ms->matchdepth = MAXCCALLS;
    ms->level = 0;
}

//------------------ nospecials — check if pattern has no magic characters
static int nospecials(const char *p, size_t l) {
    size_t upto = 0;
    do {
        size_t seg = clx_strlen(p + upto);
        if (clx_find_byte_of_set(p + upto, seg))
            return 0;
        upto += seg + 1;
    } while (upto <= l);
    return 1;
}

//------------------ literal_first_byte — check if pattern starts with a literal byte (for optimization)
static int literal_first_byte(const char *p, size_t lp, char *out_c) {
    if (lp == 0)
        return 0;
    unsigned char c0 = (unsigned char)p[0];
    if (c0 == '\0' || c0 == L_ESC)
        return 0;
    if (std::strchr(SPECIALS, c0))
        return 0;
    if (lp >= 2) {
        char q = p[1];
        if (q == '*' || q == '+' || q == '-' || q == '?')
            return 0;
    }
    *out_c = (char)c0;
    return 1;
}

//------------------ str_find_aux — shared implementation of find/match
static MultiValue str_find_aux(LState *L, const LValue *args, size_t count, int find) {
    const char *fname = find ? "string.find" : "string.match";
    size_t ls, lp;
    const char *s = get_string(L, args, count, ls, 1, fname);
    const char *p = get_string(L, args, count, lp, 2, fname);
    int64_t init_i = (count >= 3 && args[2].type != Nil) ? get_integer(L, args, count, 3, fname) : 1;
    size_t init = posrelat(init_i, ls) - 1;
    if (init > ls)
        return MultiValue(LValue());
    sync_match_class();

    bool plain = (count >= 4) ? args[3].as_bool() : false;

    if (lp == 5 && p[0] == '%' && p[1] == 's' && p[2] == '*' && p[3] == '(' && p[4] == ')' && !(find && plain)) {
        size_t e = init;
        while (e < ls && std::isspace((unsigned char)s[e]))
            e++;
        if (find)
            return MultiValue(static_cast<int64_t>(init + 1), static_cast<int64_t>(e),
                static_cast<int64_t>(e + 1));
        return MultiValue(static_cast<int64_t>(e + 1));
    }

    if (find && (plain || nospecials(p, lp))) {
        const char *s2 = lmemfind(s + init, ls - init, p, lp);
        if (s2) {
            return MultiValue(
                { LValue(static_cast<int64_t>((s2 - s) + 1)), LValue(static_cast<int64_t>((s2 - s) + lp)) });
        }
    } else {
        MatchState ms;
        const char *s1 = s + init;
        int anchor = (*p == '^');
        if (anchor) {
            p++;
            lp--;
        }
        prepstate(&ms, L, s, ls, p, lp);
        char first_c;
        bool can_skip = !anchor && literal_first_byte(p, lp, &first_c);
        do {
            reprepstate(&ms);
            const char *res = match(&ms, s1, p);
            if (res) {
                if (find) {
                    LValue results[LUA_MAXCAPTURES + 2];
                    size_t rc = 0;
                    results[rc++] = LValue(static_cast<int64_t>((s1 - s) + 1));
                    results[rc++] = LValue(static_cast<int64_t>(res - s));
                    int nlevels = ms.level;
                    for (int ci = 0; ci < nlevels; ci++) {
                        const char *cap;
                        ptrdiff_t capl = get_onecapture(&ms, ci, s1, res, &cap);
                        results[rc++] = (capl == CAP_POSITION) ? LValue(static_cast<int64_t>((cap - s) + 1))
                                                               : L->intern_lvalue(cap, (size_t)capl);
                    }
                    return MultiValue(results, rc, L);
                } else {
                    int nlevels = (ms.level == 0) ? 1 : ms.level;
                    LValue results[LUA_MAXCAPTURES];
                    size_t rc = 0;
                    for (int ci = 0; ci < nlevels; ci++) {
                        const char *cap;
                        ptrdiff_t capl = get_onecapture(&ms, ci, s1, res, &cap);
                        results[rc++] = (capl == CAP_POSITION) ? LValue(static_cast<int64_t>((cap - s) + 1))
                                                               : L->intern_lvalue(cap, (size_t)capl);
                    }
                    return MultiValue(results, rc, L);
                }
            }
            if (can_skip && s1 < ms.src_end) {
                const char *nx = (const char *)std::memchr(s1 + 1, first_c, (size_t)(ms.src_end - (s1 + 1)));
                if (!nx)
                    break;
                s1 = nx;
            } else {
                s1++;
            }
        } while (s1 <= ms.src_end && !anchor);
    }
    return MultiValue(LValue());
}

//------------------ str_find — string.find: locate pattern in string
MultiValue str_find(LState *L, const LValue *args, size_t count) {
    return str_find_aux(L, args, count, 1);
}

//------------------ str_match — string.match: capture pattern match
MultiValue str_match(LState *L, const LValue *args, size_t count) {
    return str_find_aux(L, args, count, 0);
}

//------------------ GMatchState — state for gmatch iterator
struct GMatchState {
    const char *src;
    const char *p;
    const char *lastmatch;
    MatchState ms;
};

//------------------ gmatch_aux — gmatch iterator body (called per-iteration)
static MultiValue gmatch_aux(LState *L, const LValue *args, size_t count, void *ud) {
    GMatchState *gm = (GMatchState *)ud;
    const char *src;
    sync_match_class();
    gm->ms.L = L;
    for (src = gm->src; src <= gm->ms.src_end; src++) {
        const char *e;
        reprepstate(&gm->ms);
        if ((e = match(&gm->ms, src, gm->p)) != NULL && e != gm->lastmatch) {
            gm->src = gm->lastmatch = e;
            int nlevels = (gm->ms.level == 0) ? 1 : gm->ms.level;
            LValue results[LUA_MAXCAPTURES];
            size_t rc = 0;
            for (int ci = 0; ci < nlevels; ci++) {
                const char *cap;
                ptrdiff_t capl = get_onecapture(&gm->ms, ci, src, e, &cap);
                results[rc++] = (capl == CAP_POSITION) ? LValue(static_cast<int64_t>((cap - gm->ms.src_init) + 1))
                                                       : L->intern_lvalue(cap, (size_t)capl);
            }
            if (rc == 1)
                return MultiValue(results[0]);
            return MultiValue(results, rc, L);
        }
    }
    return MultiValue();
}

//------------------ str_gmatch — string.gmatch: global match iterator
MultiValue str_gmatch(LState *L, const LValue *args, size_t count) {
    size_t ls, lp;
    const char *s = get_string(L, args, count, ls, 1, "string.gmatch");
    const char *p = get_string(L, args, count, lp, 2, "string.gmatch");
    s = L->intern_string(s, ls);
    p = L->intern_string(p, lp);
    int64_t init_i = (count >= 3 && args[2].type != Nil) ? get_integer(L, args, count, 3, "string.gmatch") : 1;
    size_t init = posrelat(init_i, ls) - 1;
    if (init > ls)
        init = ls + 1;

    auto gm = std::make_shared<GMatchState>();
    prepstate(&gm->ms, L, s, ls, p, lp);
    gm->src = s + init;
    gm->p = p;
    gm->lastmatch = NULL;

    auto iter = [gm](LState *L2, const LValue *a2, size_t c2) -> MultiValue {
        (void)a2;
        (void)c2;
        return gmatch_aux(L2, a2, c2, gm.get());
    };
    LValue iter_val = L->create_closure(iter);
    return MultiValue(iter_val);
}

//------------------ str_gsub — string.gsub: global search-and-replace
MultiValue str_gsub(LState *L, const LValue *args, size_t count) {
    size_t srcl, lp;
    const char *src = get_string(L, args, count, srcl, 1, "string.gsub");
    const char *p = get_string(L, args, count, lp, 2, "string.gsub");

    //------------------ stock's luaL_argexpected reports "no value" for an absent third argument
    if (count < 3)
        throw_runtime_error(bad_arg_msg(L, 3, "string.gsub", "string/function/table expected, got no value"));
    ValueType tr_type = args[2].type;
    int64_t max_s
        = (count >= 4 && args[3].type != Nil) ? get_integer(L, args, count, 4, "string.gsub") : (int64_t)srcl + 1;
    int anchor = (*p == '^');
    if (anchor) {
        p++;
        lp--;
    }

    MatchState ms;
    int64_t n = 0;
    bool changed = false;
    const char *lastmatch = NULL;
    const char *src_pos = src;

    prepstate(&ms, L, src, srcl, p, lp);
    char first_c;
    bool can_skip = !anchor && literal_first_byte(p, lp, &first_c);

    StringBuilder result;
    if (srcl > 64)
        result.reserve(std::min<size_t>(srcl / 4 + 8, 4096));

    while (n < max_s) {
        sync_match_class();
        reprepstate(&ms);
        const char *e;
        if ((e = match(&ms, src_pos, p)) != NULL && e != lastmatch) {
            n++;
            changed = true;

            if (tr_type == String) {
                const char *news = args[2].as_string();
                size_t l = args[2].string_len();
                size_t i = 0;
                while (i < l) {
                    if (news[i] == L_ESC && i + 1 < l) {
                        i++;
                        if (news[i] == L_ESC) {
                            result.append("%", 1);
                        } else if (news[i] == '0') {
                            size_t ml = e - src_pos;
                            result.append(src_pos, static_cast<uint32_t>(ml));
                        } else if (std::isdigit((unsigned char)news[i])) {
                            int ci = news[i] - '1';
                            const char *cap;
                            ptrdiff_t capl = get_onecapture(&ms, ci, src_pos, e, &cap);
                            if (capl == CAP_POSITION) {
                                char tmp[24];
                                int tw = std::snprintf(tmp, sizeof(tmp), "%lld", (long long)((cap - src) + 1));
                                result.append_owned(tmp, static_cast<uint32_t>(tw));
                            } else {
                                result.append(cap, static_cast<uint32_t>(capl));
                            }
                        } else {
                            //------------------ stock rejects a '%' that is not %% or a valid capture reference
                            throw_runtime_error("invalid use of '%' in replacement string");
                        }
                        i++;
                    } else {
                        size_t start = i;
                        while (i < l && news[i] != L_ESC)
                            i++;
                        result.append(&news[start], static_cast<uint32_t>(i - start));
                    }
                }

            } else if (tr_type == Function) {
                int nlevels = (ms.level == 0) ? 1 : ms.level;
                std::vector<LValue> fargs(nlevels);
                for (int ci = 0; ci < nlevels; ci++) {
                    const char *cap;
                    ptrdiff_t capl = get_onecapture(&ms, ci, src_pos, e, &cap);
                    if (capl == CAP_POSITION)
                        fargs[ci] = LValue(static_cast<int64_t>((cap - src) + 1));
                    else
                        fargs[ci] = LValue(L->intern_string(cap, (size_t)capl));
                }
                static constexpr CallSite gsub_site = { "gsub", 0, false, false };
                //------------------ the replacement runs under gsub's own C frame: stock does not allow it to yield
                struct YieldBlock {
                    LState *L;
                    YieldBlock(LState *l)
                        : L(l) { ++L->c_yield_blocked; }
                    ~YieldBlock() { --L->c_yield_blocked; }
                } yb(L);
                MultiValue fret = call_function_rooted(L, args[2], fargs.data(), fargs.size(), &gsub_site);
                LValue rv = (fret.count > 0) ? fret[0] : LValue();
                if (rv.type == Nil || (rv.type == Boolean && !rv.as_bool())) {
                    size_t ml = e - src_pos;
                    result.append(src_pos, static_cast<uint32_t>(ml));
                } else if (rv.type == String) {
                    result.append(L, rv);
                } else if (rv.type == ValueType::Int64 || rv.type == ValueType::Double) {
                    std::string rs = rv.to_string(L);
                    result.append_owned(rs.data(), static_cast<uint32_t>(rs.size()));
                } else {
                    throw_runtime_error("invalid replacement value (a " + std::string(stock_type_name(rv)) + ")");
                }

            } else if (tr_type == Table) {
                const char *cap;
                ptrdiff_t capl = get_onecapture(&ms, 0, src_pos, e, &cap);
                LValue key = (capl == CAP_POSITION) ? LValue(static_cast<int64_t>((cap - src) + 1))
                                                    : LValue(L->intern_string(cap, (size_t)capl));
                //------------------ stock indexes the replacement table with lua_gettable, so __index (even a function) supplies the value
                LValue val = table_get(L, args[2], key);
                if (val.type == Nil || (val.type == Boolean && !val.as_bool())) {
                    size_t ml = e - src_pos;
                    result.append(src_pos, static_cast<uint32_t>(ml));
                } else if (val.type == String) {
                    result.append(L, val);
                } else if (val.type == ValueType::Int64 || val.type == ValueType::Double) {
                    std::string vs = val.to_string(L);
                    result.append_owned(vs.data(), static_cast<uint32_t>(vs.size()));
                } else {
                    throw_runtime_error("invalid replacement value (a " + std::string(stock_type_name(val)) + ")");
                }
            } else if (tr_type == ValueType::Int64 || tr_type == ValueType::Double) {
                std::string rs = args[2].to_string(L);
                result.append_owned(rs.data(), static_cast<uint32_t>(rs.size()));
            } else {
                throw_runtime_error(bad_arg_msg(L, 3, "string.gsub",
                    std::string("string/function/table expected, got ") + stock_type_name(args[2])));
            }

            src_pos = lastmatch = e;
        } else if (src_pos < ms.src_end) {
            if (can_skip) {
                const char *nx = (const char *)std::memchr(src_pos + 1, first_c, (size_t)(ms.src_end - (src_pos + 1)));
                size_t span = nx ? (size_t)(nx - src_pos) : (size_t)(ms.src_end - src_pos);
                result.append(src_pos, static_cast<uint32_t>(span));
                src_pos += span;
                if (!nx)
                    break;
            } else {
                result.append(src_pos, 1);
                src_pos++;
            }
        } else
            break;
        if (anchor)
            break;
    }

    if (!changed)
        return MultiValue({ LValue(L->intern_string(src, srcl)), LValue(static_cast<int64_t>(0)) });

    size_t tail = ms.src_end - src_pos;
    if (tail > 0)
        result.append(src_pos, static_cast<uint32_t>(tail));

    return MultiValue({ result.to_lvalue(L), LValue(static_cast<int64_t>(n)) });
}

#define LUAL_PACKPADBYTE 0x00
#define MAXINTSIZE 16
#define NB CHAR_BIT
#define MC ((1 << NB) - 1)
#define SZINT ((int)sizeof(int64_t))

static const union {
    int dummy;
    char little;
} nativeendian = { 1 };

//------------------ Header — pack/unpack format state (endian, alignment)
struct Header {
    LState *L;
    int islittle;
    unsigned maxalign;
};

enum KOption { Kint, Kuint, Kfloat, Knumber, Kdouble, Kchar, Kstring, Kzstr, Kpadding, Kpaddalign, Knop };

//------------------ digit — test if char is a decimal digit
static int digit(int c) {
    return '0' <= c && c <= '9';
}

//------------------ getnum — parse decimal number from format string
static size_t getnum(const char **fmt, size_t df) {
    if (!digit(**fmt))
        return df;
    size_t a = 0;
    do {
        a = a * 10 + (unsigned)(*(*fmt)++ - '0');
    } while (digit(**fmt) && a <= ((size_t)-1 - 9) / 10);
    return a;
}

//------------------ getnumlimit — parse number with size-limit validation
static unsigned getnumlimit(Header *h, const char **fmt, size_t df) {
    size_t sz = getnum(fmt, df);
    if ((sz - 1u) >= MAXINTSIZE) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "integral size (%zu) out of limits [1,%d]", sz, MAXINTSIZE);
        throw_runtime_error(buf);
    }
    return (unsigned)sz;
}

//------------------ initheader — initialise pack header (endian, alignment)
static void initheader(LState *L, Header *h) {
    h->L = L;
    h->islittle = nativeendian.little;
    h->maxalign = 1;
}

//------------------ getoption — parse a single pack format option char
static KOption getoption(Header *h, const char **fmt, size_t *size) {
    struct cD {
        char c;
        double u;
    };

    int opt = *(*fmt)++;
    *size = 0;
    switch (opt) {
    case 'b':
        *size = sizeof(char);
        return Kint;
    case 'B':
        *size = sizeof(char);
        return Kuint;
    case 'h':
        *size = sizeof(short);
        return Kint;
    case 'H':
        *size = sizeof(short);
        return Kuint;
    case 'l':
        *size = sizeof(long);
        return Kint;
    case 'L':
        *size = sizeof(long);
        return Kuint;
    case 'j':
        *size = sizeof(int64_t);
        return Kint;
    case 'J':
        *size = sizeof(int64_t);
        return Kuint;
    case 'T':
        *size = sizeof(size_t);
        return Kuint;
    case 'f':
        *size = sizeof(float);
        return Kfloat;
    case 'n':
        *size = sizeof(double);
        return Knumber;
    case 'd':
        *size = sizeof(double);
        return Kdouble;
    case 'i':
        *size = getnumlimit(h, fmt, sizeof(int));
        return Kint;
    case 'I':
        *size = getnumlimit(h, fmt, sizeof(int));
        return Kuint;
    case 's':
        *size = getnumlimit(h, fmt, sizeof(size_t));
        return Kstring;
    case 'c': {
        *size = getnum(fmt, (size_t)-1);
        if (*size == (size_t)-1)
            throw_runtime_error("missing size for format option 'c'");
        return Kchar;
    }
    case 'z':
        return Kzstr;
    case 'x':
        *size = 1;
        return Kpadding;
    case 'X':
        return Kpaddalign;
    case ' ':
        break;
    case '<':
        h->islittle = 1;
        break;
    case '>':
        h->islittle = 0;
        break;
    case '=':
        h->islittle = nativeendian.little;
        break;
    case '!': {
        const size_t maxalign = offsetof(cD, u);
        h->maxalign = getnumlimit(h, fmt, maxalign);
        break;
    }
    default: {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "invalid format option '%c'", opt);
        throw_runtime_error(buf);
    }
    }
    return Knop;
}

//------------------ getdetails — resolve option + compute padding alignment
static KOption getdetails(Header *h, size_t totalsize, const char **fmt, size_t *psize, unsigned *ntoalign) {
    KOption opt = getoption(h, fmt, psize);
    size_t align = *psize;
    if (opt == Kpaddalign) {
        if (**fmt == '\0' || getoption(h, fmt, &align) == Kchar || align == 0)
            throw_runtime_error("invalid next option for option 'X'");
    }
    if (align <= 1 || opt == Kchar) {
        *ntoalign = 0;
    } else {
        if (align > h->maxalign)
            align = h->maxalign;
        if ((align & (align - 1)) != 0) {
            *ntoalign = 0;
            throw_runtime_error("format asks for alignment not power of 2");
        } else {
            unsigned szmoda = (unsigned)(totalsize & (align - 1));
            *ntoalign = (unsigned)((align - szmoda) & (align - 1));
        }
    }
    return opt;
}

//------------------ packint — write integer to buffer with endianness
static void packint(std::string &buf, uint64_t n, int islittle, unsigned size, int neg) {
    size_t pos = buf.size();
    buf.resize(pos + size);
    unsigned i;
    char *buff = &buf[pos];
    buff[islittle ? 0 : size - 1] = (char)(n & MC);
    for (i = 1; i < size; i++) {
        n >>= NB;
        buff[islittle ? i : size - 1 - i] = (char)(n & MC);
    }
    if (neg && size > SZINT) {
        for (i = SZINT; i < size; i++)
            buff[islittle ? i : size - 1 - i] = (char)MC;
    }
}

//------------------ copywithendian — copy bytes with optional endian swap
static void copywithendian(char *dest, const char *src, unsigned size, int islittle) {
    if (islittle == nativeendian.little)
        std::memcpy(dest, src, size);
    else {
        dest += size - 1;
        while (size-- != 0)
            *dest-- = *src++;
    }
}

//------------------ unpackint — read integer from buffer with endianness
static int64_t unpackint(const char *str, int islittle, int size, int issigned) {
    uint64_t res = 0;
    int i;
    int limit = (size <= SZINT) ? size : SZINT;
    for (i = limit - 1; i >= 0; i--) {
        res <<= NB;
        res |= (uint64_t)(unsigned char)str[islittle ? i : size - 1 - i];
    }
    if (size < SZINT) {
        if (issigned) {
            uint64_t mask = (uint64_t)1 << (size * NB - 1);
            res = ((res ^ mask) - mask);
        }
    } else if (size > SZINT) {
        int mask = (!issigned || (int64_t)res >= 0) ? 0 : MC;
        for (i = limit; i < size; i++) {
            if ((unsigned char)str[islittle ? i : size - 1 - i] != (unsigned char)mask) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%d-byte integer does not fit into Lua Integer", size);
                throw_runtime_error(buf);
            }
        }
    }
    return (int64_t)res;
}

//------------------ str_pack — string.pack: pack values into binary string
MultiValue str_pack(LState *L, const LValue *args, size_t count) {
    size_t fmt_len;
    const char *fmt = get_string(L, args, count, fmt_len, 1, "string.pack");
    int arg = 1;
    size_t totalsize = 0;
    Header h;
    initheader(L, &h);
    std::string buf;
    while (*fmt != '\0') {
        unsigned ntoalign;
        size_t size;
        KOption opt = getdetails(&h, totalsize, &fmt, &size, &ntoalign);
        if (size > static_cast<size_t>(INT64_MAX) - ntoalign - totalsize)
            throw_runtime_error(bad_arg_msg(L, arg, "string.pack", "result too long"));
        totalsize += ntoalign + size;
        buf.append(ntoalign, (char)LUAL_PACKPADBYTE);
        arg++;
        switch (opt) {
        case Kint: {
            int64_t n = get_integer(L, args, count, arg, "string.pack");
            if (size < SZINT) {
                int64_t lim = (int64_t)1 << ((size * NB) - 1);
                if (n < -lim || n >= lim)
                    throw_runtime_error("integer overflow");
            }
            packint(buf, (uint64_t)n, h.islittle, (unsigned)size, (n < 0));
            break;
        }
        case Kuint: {
            int64_t n = get_integer(L, args, count, arg, "string.pack");
            if (size < SZINT) {
                uint64_t lim = (uint64_t)1 << (size * NB);
                if ((uint64_t)n >= lim)
                    throw_runtime_error("unsigned overflow");
            }
            packint(buf, (uint64_t)n, h.islittle, (unsigned)size, 0);
            break;
        }
        case Kfloat: {
            double d;
            if (!args[arg - 1].to_number(d)) {
                throw_runtime_error(bad_arg_msg(L, arg, "string.pack", "number expected"));
            }
            float f = (float)d;
            size_t pos = buf.size();
            buf.resize(pos + sizeof(f));
            copywithendian(&buf[pos], (const char *)&f, sizeof(f), h.islittle);
            break;
        }
        case Knumber: {
            double d;
            if (!args[arg - 1].to_number(d)) {
                throw_runtime_error(bad_arg_msg(L, arg, "string.pack", "number expected"));
            }
            size_t pos = buf.size();
            buf.resize(pos + sizeof(d));
            copywithendian(&buf[pos], (const char *)&d, sizeof(d), h.islittle);
            break;
        }
        case Kdouble: {
            double d;
            if (!args[arg - 1].to_number(d)) {
                throw_runtime_error(bad_arg_msg(L, arg, "string.pack", "number expected"));
            }
            size_t pos = buf.size();
            buf.resize(pos + sizeof(d));
            copywithendian(&buf[pos], (const char *)&d, sizeof(d), h.islittle);
            break;
        }
        case Kchar: {
            size_t len;
            const char *s = get_string(L, args, count, len, arg, "string.pack");
            if (len > size) {
                throw_runtime_error(bad_arg_msg(L, arg, "string.pack", "string longer than given size"));
            }
            buf.append(s, len);
            if (len < size)
                buf.append(size - len, (char)LUAL_PACKPADBYTE);
            break;
        }
        case Kstring: {
            size_t len;
            const char *s = get_string(L, args, count, len, arg, "string.pack");
            if (size < (int)sizeof(uint64_t) && len >= ((uint64_t)1 << (size * NB))) {
                throw_runtime_error(bad_arg_msg(L, arg, "string.pack", "string length does not fit in given size"));
            }
            packint(buf, (uint64_t)len, h.islittle, (unsigned)size, 0);
            buf.append(s, len);
            totalsize += len;
            break;
        }
        case Kzstr: {
            size_t len;
            const char *s = get_string(L, args, count, len, arg, "string.pack");
            if (std::strlen(s) != len) {
                throw_runtime_error(bad_arg_msg(L, arg, "string.pack", "string contains zeros"));
            }
            buf.append(s, len);
            buf += '\0';
            totalsize += len + 1;
            break;
        }
        case Kpadding:
            buf += (char)LUAL_PACKPADBYTE;
        case Kpaddalign:
        case Knop:
            arg--;
            break;
        }
    }
    return MultiValue(LValue(L->intern_string(buf.data(), buf.size())));
}

//------------------ str_packsize — string.packsize: compute packed size
MultiValue str_packsize(LState *L, const LValue *args, size_t count) {
    size_t fmt_len;
    const char *fmt = get_string(L, args, count, fmt_len, 1, "string.packsize");
    size_t totalsize = 0;
    Header h;
    initheader(L, &h);
    while (*fmt != '\0') {
        unsigned ntoalign;
        size_t size;
        KOption opt = getdetails(&h, totalsize, &fmt, &size, &ntoalign);
        if (opt == Kstring || opt == Kzstr)
            throw_runtime_error("variable-length format");
        size += ntoalign;
        if (totalsize > (size_t)INT64_MAX - size)
            throw_runtime_error("format result too large");
        totalsize += size;
    }
    return MultiValue(static_cast<int64_t>(totalsize));
}

//------------------ str_unpack — string.unpack: unpack values from binary string
MultiValue str_unpack(LState *L, const LValue *args, size_t count) {
    size_t fmt_len;
    const char *fmt = get_string(L, args, count, fmt_len, 1, "string.unpack");
    size_t ld;
    const char *data = get_string(L, args, count, ld, 2, "string.unpack");
    int64_t pos_i = (count >= 3 && args[2].type != Nil) ? get_integer(L, args, count, 3, "string.unpack") : 1;
    size_t pos = posrelat(pos_i, ld) - 1;
    if (pos > ld)
        throw_runtime_error("initial position out of string");
    Header h;
    initheader(L, &h);
    std::vector<LValue> results;
    while (*fmt != '\0') {
        unsigned ntoalign;
        size_t size;
        KOption opt = getdetails(&h, pos, &fmt, &size, &ntoalign);
        if (ntoalign + size > ld - pos)
            throw_runtime_error("data string too short");
        pos += ntoalign;
        switch (opt) {
        case Kint:
        case Kuint: {
            int64_t res = unpackint(data + pos, h.islittle, (int)size, (opt == Kint));
            results.push_back(LValue(res));
            break;
        }
        case Kfloat: {
            float f;
            copywithendian((char *)&f, data + pos, sizeof(f), h.islittle);
            results.push_back(LValue((double)f));
            break;
        }
        case Knumber: {
            double f;
            copywithendian((char *)&f, data + pos, sizeof(f), h.islittle);
            results.push_back(LValue(f));
            break;
        }
        case Kdouble: {
            double f;
            copywithendian((char *)&f, data + pos, sizeof(f), h.islittle);
            results.push_back(LValue(f));
            break;
        }
        case Kchar: {
            results.push_back(LValue(L->intern_string(data + pos, size)));
            break;
        }
        case Kstring: {
            uint64_t len = (uint64_t)unpackint(data + pos, h.islittle, (int)size, 0);
            if (len > ld - pos - size)
                throw_runtime_error("data string too short");
            results.push_back(LValue(L->intern_string(data + pos + size, (size_t)len)));
            pos += (size_t)len;
            break;
        }
        case Kzstr: {
            size_t len = std::strlen(data + pos);
            if (pos + len >= ld)
                throw_runtime_error("unfinished string for format 'z'");
            results.push_back(LValue(L->intern_string(data + pos, len)));
            pos += len + 1;
            break;
        }
        case Kpaddalign:
        case Kpadding:
        case Knop:
            break;
        }
        pos += size;
    }
    results.push_back(LValue(static_cast<int64_t>(pos + 1)));
    return MultiValue(results.data(), results.size(), L);
}

//------------------ luastd_string — register string library and metatable
void luastd_string(LState *L) {
    LValue string_table = L->create_table();
    L->root_value(string_table);
    LTable *t = static_cast<LTable *>(string_table.as_pointer());

    static constexpr clx::LazyReg strings_funcs[] = { { "len", str_len }, { "sub", str_sub },
        { "reverse", str_reverse }, { "lower", str_lower }, { "upper", str_upper }, { "rep", str_rep },
        { "byte", str_byte }, { "char", str_char }, { "dump", str_dump }, { "format", str_format },
        { "find", str_find }, { "match", str_match }, { "gmatch", str_gmatch }, { "gsub", str_gsub },
        { "pack", str_pack }, { "packsize", str_packsize }, { "unpack", str_unpack } };
    clx::set_lazy_funcs(L, string_table, strings_funcs, std::size(strings_funcs));
    set_global(L, "string", string_table);
    L->unroot_value(string_table);

    LValue mt = L->create_table();
    L->string_metatable = static_cast<LTable *>(mt.as_pointer());
    L->root_value(mt);
    L->string_metatable->settable(LValue(L->intern_string("__index")), string_table);
}

}
