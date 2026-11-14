// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  utf8.cpp · UTF-8 Library                   │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include "clx_runtime.h"
#include <cstdio>
#include <cstring>

namespace clx {
//------------------ CodesUd: userdata struct for utf8.codes iterator state
struct CodesUd {
    size_t pos;
    size_t len;
    const char *s;
    bool lax;
};

//------------------ utf8_charpattern: stock UTF8PATT bytes
static const char utf8_charpattern[] = "[\0-\x7F\xC2-\xFD][\x80-\xBF]*";

//------------------ get_string: extracts a string argument from Lua args by index
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

//------------------ utf8_decode: stock utf8_decode with strict flag
static const char *utf8_decode_strict(const char *s, size_t len, size_t pos, uint32_t &codepoint, bool strict) {
    static const uint32_t limits[] = { ~(uint32_t)0, 0x80, 0x800, 0x10000u, 0x200000u, 0x4000000u };
    if (pos >= len)
        return nullptr;
    unsigned int c = (unsigned char)s[pos];
    uint32_t res = 0;
    if (c < 0x80) {
        res = c;
    } else if (c >= 0xFE) {
        return nullptr;
    } else {
        int count = 0;
        for (; c & 0x40; c <<= 1) {
            if (pos + (size_t)count + 1 >= len)
                return nullptr;
            unsigned int cc = (unsigned char)s[pos + ++count];
            if ((cc & 0xC0) != 0x80)
                return nullptr;
            res = (res << 6) | (cc & 0x3F);
        }
        if (count > 5)
            return nullptr;
        res |= ((uint32_t)(c & 0x7F) << (count * 5));
        if (res > 0x7FFFFFFFu || res < limits[count])
            return nullptr;
        pos += count;
    }
    if (strict) {
        if (res > 0x10FFFFu || (0xD800u <= res && res <= 0xDFFFu))
            return nullptr;
    }
    codepoint = res;
    return s + pos + 1;
}

//------------------ utf8_decode: decodes one UTF-8 codepoint at position, returns byte length
static int utf8_decode(const char *s, size_t len, size_t pos, uint32_t &codepoint) {
    uint32_t cp = 0;
    const char *next = utf8_decode_strict(s, len, pos, cp, true);
    if (!next)
        return -1;
    codepoint = cp;
    return (int)(next - (s + pos));
}

//------------------ utf8_decode_lax: decodes with lax flag, returns byte length or -1
static int utf8_decode_lax(const char *s, size_t len, size_t pos, uint32_t &codepoint, bool lax) {
    uint32_t cp = 0;
    const char *next = utf8_decode_strict(s, len, pos, cp, !lax);
    if (!next)
        return -1;
    codepoint = cp;
    return (int)(next - (s + pos));
}

//------------------ u_posrelat: stock relative position translation
static int64_t u_posrelat(int64_t pos, size_t len) {
    if (pos >= 0)
        return pos;
    else if (0u - (size_t)pos > len)
        return 0;
    else
        return (int64_t)len + pos + 1;
}

//------------------ utf8_encode: encodes a codepoint as UTF-8 into buf, returns byte length
static int utf8_encode(uint32_t codepoint, char *buf) {
    if (codepoint <= 0x7F) {
        buf[0] = static_cast<char>(codepoint);
        return 1;
    } else if (codepoint <= 0x7FF) {
        buf[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        buf[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 2;
    } else if (codepoint <= 0xFFFF) {
        buf[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        buf[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        buf[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 3;
    } else if (codepoint <= 0x1FFFFF) {
        buf[0] = static_cast<char>(0xF0 | (codepoint >> 18));
        buf[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        buf[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        buf[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 4;
    } else if (codepoint <= 0x3FFFFFF) {
        buf[0] = static_cast<char>(0xF8 | (codepoint >> 24));
        buf[1] = static_cast<char>(0x80 | ((codepoint >> 18) & 0x3F));
        buf[2] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        buf[3] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        buf[4] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 5;
    } else if (codepoint <= 0x7FFFFFFF) {
        buf[0] = static_cast<char>(0xFC | (codepoint >> 30));
        buf[1] = static_cast<char>(0x80 | ((codepoint >> 24) & 0x3F));
        buf[2] = static_cast<char>(0x80 | ((codepoint >> 18) & 0x3F));
        buf[3] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        buf[4] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        buf[5] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 6;
    }
    return 0;
}

//------------------ utf8_char: creates a UTF-8 string from codepoint values
static MultiValue utf8_char(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        return MultiValue(LValue(L->intern_string("", 0)));
    StringBuilder sb;
    for (size_t i = 0; i < count; ++i) {
        int64_t v = check_integer(L, args[i]);
        if (v < 0 || (uint64_t)v > 0x7FFFFFFFull)
            throw_runtime_error(bad_arg_msg(L, (int)(i + 1), "utf8.char", "value out of range"));
        char buf[8];
        int n = utf8_encode((uint32_t)v, buf);
        sb.append_owned(buf, static_cast<uint32_t>(n));
    }
    return MultiValue(sb.to_lvalue(L));
}

//------------------ codes_iter: iterator function for utf8.codes
static MultiValue codes_iter(LState *L, const LValue *a, size_t c) {
    if (c < 1)
        return MultiValue();
    //------------------ the state may be the string itself (stock's iter_aux signature, which callers
    //------------------ can invoke directly) or the userdata carrying it and the lax flag
    const char *s = nullptr;
    size_t len = 0;
    bool lax = false;
    if (a[0].type == ValueType::String) {
        s = a[0].as_string();
        len = a[0].string_len();
    } else if (a[0].type == ValueType::UserData) {
        LUserdata *u = static_cast<LUserdata *>(a[0].as_pointer());
        CodesUd *ud = static_cast<CodesUd *>(u->data());
        s = ud->s;
        len = ud->len;
        lax = ud->lax;
    } else {
        return MultiValue();
    }
    //------------------ the control is the byte offset of the next character; a negative or huge value
    //------------------ wraps like stock's unsigned cast and simply ends the iteration
    uint64_t n = 0;
    if (c >= 2) {
        if (a[1].type == ValueType::Int64)
            n = static_cast<uint64_t>(a[1].as_integer());
        else if (a[1].type == ValueType::Double)
            n = static_cast<uint64_t>(static_cast<int64_t>(a[1].as_number()));
    }
    while (n < len && ((static_cast<unsigned char>(s[n]) & 0xC0) == 0x80))
        n++;
    if (n >= len)
        return MultiValue();
    uint32_t cp;
    int dlen = utf8_decode_lax(s, len, n, cp, lax);
    if (dlen <= 0)
        throw_runtime_error(file_line_prefix(L) + "invalid UTF-8 code");
    if (n + (size_t)dlen < len && (((unsigned char)s[n + dlen] & 0xC0) == 0x80))
        throw_runtime_error(file_line_prefix(L) + "invalid UTF-8 code");
    return MultiValue({ clx::integer(static_cast<int64_t>(n + 1)), clx::integer(static_cast<int64_t>(cp)) });
}

//------------------ utf8_codes: returns an iterator over UTF-8 codepoints in a string
static MultiValue utf8_codes(LState *L, const LValue *args, size_t count) {
    size_t len;
    const char *raw = get_string(L, args, count, len, 1, "utf8.codes");
    bool lax = (count >= 2) ? to_boolean(args[1]) : false;
    if (len > 0 && (((unsigned char)raw[0] & 0xC0) == 0x80))
        throw_runtime_error(bad_arg_msg(L, 1, "utf8.codes", "invalid UTF-8 code"));
    const char *s = L->intern_string(raw, len);
    LValue u_lv = newuserdata(L, sizeof(CodesUd));
    RootRange rr(L, &u_lv, 1);
    LUserdata *u = static_cast<LUserdata *>(u_lv.as_pointer());
    CodesUd *ud = static_cast<CodesUd *>(u->data());
    ud->pos = 0;
    ud->len = len;
    ud->s = s;
    ud->lax = lax;
    LValue func = L->create_closure(CFunctionType(codes_iter));
    return MultiValue({ func, u_lv, clx::nil() });
}

//------------------ utf8_codepoint: returns codepoints from a range in a UTF-8 string
static MultiValue utf8_codepoint(LState *L, const LValue *args, size_t count) {
    size_t len;
    const char *s = get_string(L, args, count, len, 1, "utf8.codepoint");
    //------------------ stock runs both indices through u_posrelat, so a negative end index (-1) counts
    //------------------ back from the end of the string instead of yielding an empty interval
    int64_t posi = u_posrelat((count >= 2 && args[1].type != Nil) ? check_integer(L, args[1]) : 1, len);
    int64_t pose = u_posrelat((count >= 3 && args[2].type != Nil) ? check_integer(L, args[2]) : posi, len);
    bool lax = (count >= 4) ? to_boolean(args[3]) : false;
    if (posi < 1)
        throw_runtime_error(bad_arg_msg(L, 2, "utf8.codepoint", "out of bounds"));
    if (pose > (int64_t)len)
        throw_runtime_error(bad_arg_msg(L, 3, "utf8.codepoint", "out of bounds"));
    if (posi > pose)
        return MultiValue();
    size_t nv = (size_t)(pose - posi) + 1;
    LValue *vals = new LValue[nv];
    size_t out = 0;
    const char *se = s + pose;
    const char *p = s + posi - 1;
    while (p < se) {
        uint32_t code = 0;
        size_t off = (size_t)(p - s);
        const char *next = utf8_decode_strict(s, len, off, code, !lax);
        if (!next) {
            delete[] vals;
            throw_runtime_error(file_line_prefix(L) + "invalid UTF-8 code");
        }
        vals[out++] = clx::integer((int64_t)(int32_t)code);
        p = next;
    }
    MultiValue res(vals, out, L);
    delete[] vals;
    return res;
}

//------------------ utf8_len: returns the number of UTF-8 codepoints in a range
static MultiValue utf8_len(LState *L, const LValue *args, size_t count) {
    size_t len;
    const char *s = get_string(L, args, count, len, 1, "utf8.len");
    int64_t posi_arg = (count >= 2 && args[1].type != Nil) ? check_integer(L, args[1]) : 1;
    int64_t posj_arg = (count >= 3 && args[2].type != Nil) ? check_integer(L, args[2]) : -1;
    bool lax = (count >= 4) ? to_boolean(args[3]) : false;
    int64_t posi = u_posrelat(posi_arg, len);
    int64_t posj = u_posrelat(posj_arg, len);
    if (!(1 <= posi && posi - 1 <= (int64_t)len))
        throw_runtime_error(bad_arg_msg(L, 2, "utf8.len", "initial position out of bounds"));
    if (!((posj - 1) < (int64_t)len))
        throw_runtime_error(bad_arg_msg(L, 3, "utf8.len", "final position out of bounds"));
    int64_t n = 0;
    while (posi <= posj) {
        size_t off = (size_t)(posi - 1);
        uint32_t cp = 0;
        const char *next = utf8_decode_strict(s, len, off, cp, !lax);
        if (!next)
            return MultiValue({ LValue(), clx::integer(posi) });
        posi = (int64_t)(next - s) + 1;
        n++;
    }
    return MultiValue(clx::integer(n));
}

//------------------ utf8_offset: stock byteoffset returning start and end positions
static MultiValue utf8_offset(LState *L, const LValue *args, size_t count) {
    size_t len;
    const char *s = get_string(L, args, count, len, 1, "utf8.offset");
    if (count < 2 || args[1].type == Nil)
        throw_runtime_error(bad_arg_msg(L, 2, "utf8.offset", "number expected, got no value"));
    int64_t n = check_integer(L, args[1]);
    int64_t defpos = (n >= 0) ? 1 : (int64_t)len + 1;
    int64_t posi_arg = (count >= 3 && args[2].type != Nil) ? check_integer(L, args[2]) : defpos;
    int64_t posi = u_posrelat(posi_arg, len);
    if (!(1 <= posi && posi - 1 <= (int64_t)len))
        throw_runtime_error(bad_arg_msg(L, 3, "utf8.offset", "position out of bounds"));
    posi--;
    auto iscont = [&](int64_t p) -> bool {
        return p >= 0 && p < (int64_t)len && (((unsigned char)s[p] & 0xC0) == 0x80);
    };
    if (n == 0) {
        while (posi > 0 && iscont(posi))
            posi--;
    } else {
        if (iscont(posi))
            throw_runtime_error(file_line_prefix(L) + "initial position is a continuation byte");
        if (n < 0) {
            while (n < 0 && posi > 0) {
                do {
                    posi--;
                } while (posi > 0 && iscont(posi));
                n++;
            }
        } else {
            n--;
            while (n > 0 && posi < (int64_t)len) {
                do {
                    posi++;
                } while (iscont(posi));
                n--;
            }
        }
    }
    if (n != 0)
        return MultiValue(LValue());
    int64_t start = posi + 1;
    if ((s[posi] & 0x80) != 0) {
        if (iscont(posi))
            throw_runtime_error(file_line_prefix(L) + "initial position is a continuation byte");
        while (iscont(posi + 1))
            posi++;
    }
    return MultiValue({ clx::integer(start), clx::integer(posi + 1) });
}

//------------------ luastd_utf8: registers the utf8 library into the global state
void luastd_utf8(LState *L) {
    LValue t = L->create_table();
    LTable *tbl = static_cast<LTable *>(t.as_pointer());
    static constexpr clx::LazyReg utf8_funcs[] = { { "char", utf8_char }, { "codes", utf8_codes },
        { "codepoint", utf8_codepoint }, { "len", utf8_len }, { "offset", utf8_offset } };
    clx::set_lazy_funcs(L, t, utf8_funcs, std::size(utf8_funcs));
    tbl->settable(LValue(L->intern_string("charpattern", 11)),
        LValue(L->intern_string(utf8_charpattern, sizeof(utf8_charpattern) - 1)));
    set_global(L, "utf8", t);
}
}
