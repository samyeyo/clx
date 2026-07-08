// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  utf8.cpp · UTF-8 Library                   │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include <cstring>
#include <cstdio>
namespace clx {
//------------------ CodesUd: userdata struct for utf8.codes iterator state
struct CodesUd {
    size_t pos;
    size_t len;
    const char* s;
};
//------------------ utf8_charpattern: pattern string matching any single UTF-8 byte sequence
static const char utf8_charpattern[] = "[%z\x01-\x7F\xC2-\xF4\x80-\xBF*]";

//------------------ get_string: extracts a string argument from Lua args by index
static const char* get_string(LState* L, const LValue* args, size_t count, size_t& len, int idx) {
    if (idx < 1 || idx > (int)count) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bad argument #%d to a string function (string expected, got no value)", idx);
        throw_runtime_error(buf);
    }
    if (args[idx - 1].type != String) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bad argument #%d to a string function (string expected, got %s)", idx,
            args[idx - 1].type == Double ? "number" : "table");
        throw_runtime_error(buf);
    }
    len = args[idx - 1].string_len();
    return args[idx - 1].as_string();
}

//------------------ utf8_decode: decodes one UTF-8 codepoint at position, returns byte length
static int utf8_decode(const char* s, size_t len, size_t pos, uint32_t& codepoint) {
    if (pos >= len) return 0;
    unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c <= 0x7F) {
        codepoint = c;
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF && pos + 1 < len) {
        unsigned char c2 = static_cast<unsigned char>(s[pos + 1]);
        if ((c2 & 0xC0) != 0x80) return -1;
        codepoint = ((c & 0x1F) << 6) | (c2 & 0x3F);
        return 2;
    } else if (c >= 0xE0 && c <= 0xEF && pos + 2 < len) {
        unsigned char c2 = static_cast<unsigned char>(s[pos + 1]);
        unsigned char c3 = static_cast<unsigned char>(s[pos + 2]);
        if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return -1;
        codepoint = ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        return 3;
    } else if (c >= 0xF0 && c <= 0xF4 && pos + 3 < len) {
        unsigned char c2 = static_cast<unsigned char>(s[pos + 1]);
        unsigned char c3 = static_cast<unsigned char>(s[pos + 2]);
        unsigned char c4 = static_cast<unsigned char>(s[pos + 3]);
        if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80 || (c4 & 0xC0) != 0x80) return -1;
        codepoint = ((c & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F);
        return 4;
    }
    return -1;
}

//------------------ utf8_encode: encodes a codepoint as UTF-8 into buf, returns byte length
static int utf8_encode(uint32_t codepoint, char* buf) {
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
    } else if (codepoint <= 0x10FFFF) {
        buf[0] = static_cast<char>(0xF0 | (codepoint >> 18));
        buf[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        buf[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        buf[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 4;
    }
    return 0;
}

//------------------ utf8_char: creates a UTF-8 string from codepoint values
static MultiValue utf8_char(LState* L, const LValue* args, size_t count) {
    if (count == 0) return MultiValue(LValue(L->intern_string("", 0)));
    StringBuilder sb;
    for (size_t i = 0; i < count; ++i) {
        uint32_t cp = static_cast<uint32_t>(check_integer(L, args[i]));
        if (cp > 0x10FFFF)
            throw_runtime_error("bad argument #1 to 'char' (value out of range)");
        char buf[8];
        int n = utf8_encode(cp, buf);
        sb.append(L, LValue(L->intern_string(buf, static_cast<size_t>(n))));
    }
    return MultiValue(sb.to_lvalue(L));
}

//------------------ codes_iter: iterator function for utf8.codes
static MultiValue codes_iter(LState* L, const LValue* a, size_t c) {
    if (c < 2) return MultiValue();
    LUserdata* u = static_cast<LUserdata*>(a[0].as_pointer());
    CodesUd* ud = static_cast<CodesUd*>(u->data());
    if (ud->pos >= ud->len) return MultiValue();
    uint32_t cp;
    int n = utf8_decode(ud->s, ud->len, ud->pos, cp);
    if (n <= 0)
        throw_runtime_error("bad argument #1 to 'codes' (invalid UTF-8 code)");
    size_t cur = ud->pos + 1;
    ud->pos += n;
    return MultiValue({clx::integer(static_cast<int64_t>(cur)), clx::integer(static_cast<int64_t>(cp))});
}

//------------------ utf8_codes: returns an iterator over UTF-8 codepoints in a string
static MultiValue utf8_codes(LState* L, const LValue* args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'codes' (string expected, got no value)");
    size_t len;
    const char* s = L->intern_string(get_string(L, args, count, len, 1), len);
    LValue u_lv = newuserdata(L, sizeof(CodesUd));
    L->shadow_stack[L->shadow_top++] = {&u_lv.val, &u_lv.type};
    LUserdata* u = static_cast<LUserdata*>(u_lv.as_pointer());
    CodesUd* ud = static_cast<CodesUd*>(u->data());
    ud->pos = 0;
    ud->len = len;
    ud->s = s;
    LValue func = L->create_closure(CFunctionType(codes_iter));
    L->shadow_top--;
    return MultiValue({func, u_lv, clx::nil()});
}

//------------------ utf8_codepoint: returns codepoints from a range in a UTF-8 string
static MultiValue utf8_codepoint(LState* L, const LValue* args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'codepoint' (string expected, got no value)");
    size_t len;
    const char* s = get_string(L, args, count, len, 1);
    size_t i = 1;
    if (count >= 2 && args[1].type != Nil)
        i = static_cast<size_t>(check_integer(L, args[1]));
    size_t j = i;
    if (count >= 3 && args[2].type != Nil)
        j = static_cast<size_t>(check_integer(L, args[2]));
    if (i < 1) i = 1;
    if (j > len) j = len;
    if (i > j) return MultiValue();
    if (i == j) {
        uint32_t cp;
        int n = utf8_decode(s, len, i - 1, cp);
        if (n <= 0)
            throw_runtime_error("bad argument #1 to 'codepoint' (invalid UTF-8 code)");
        return MultiValue(clx::integer(static_cast<int64_t>(cp)));
    }
    size_t nv = j - i + 1;
    LValue* vals = new LValue[nv];
    size_t out = 0;
    size_t pos = i - 1;
    while (pos < j && pos < len) {
        uint32_t cp;
        int nbytes = utf8_decode(s, len, pos, cp);
        if (nbytes <= 0) {
            delete[] vals;
            throw_runtime_error("bad argument #1 to 'codepoint' (invalid UTF-8 code)");
        }
        vals[out++] = clx::integer(static_cast<int64_t>(cp));
        pos += nbytes;
    }
    MultiValue res(vals, out);
    delete[] vals;
    return res;
}

//------------------ utf8_len: returns the number of UTF-8 codepoints in a range
static MultiValue utf8_len(LState* L, const LValue* args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'len' (string expected, got no value)");
    size_t len;
    const char* s = get_string(L, args, count, len, 1);
    size_t i = 1;
    if (count >= 2 && args[1].type != Nil)
        i = static_cast<size_t>(check_integer(L, args[1]));
    size_t j = len;
    if (count >= 3 && args[2].type != Nil)
        j = static_cast<size_t>(check_integer(L, args[2]));
    if (i < 1) i = 1;
    if (j > len) j = len;
    if (i > j) return MultiValue(clx::integer(0));
    size_t pos = i - 1;
    size_t codepoint_count = 0;
    while (pos < j && pos < len) {
        uint32_t cp;
        int n = utf8_decode(s, len, pos, cp);
        if (n <= 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "bad argument #1 to 'len' (invalid UTF-8 code at position %zu)", pos + 1);
            throw_runtime_error(buf);
        }
        pos += n;
        codepoint_count++;
    }
    return MultiValue(clx::integer(static_cast<int64_t>(codepoint_count)));
}

//------------------ utf8_offset: returns byte offset of a codepoint position in a UTF-8 string
static MultiValue utf8_offset(LState* L, const LValue* args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'offset' (string expected, got no value)");
    size_t len;
    const char* s = get_string(L, args, count, len, 1);
    int64_t n = check_integer(L, count >= 2 ? args[1] : LValue());
    size_t i = 1;
    if (count >= 3 && args[2].type != Nil)
        i = static_cast<size_t>(check_integer(L, args[2]));
    if (i < 1 || i > len)
        throw_runtime_error("bad argument #3 to 'offset' (position out of range)");
    size_t byte_pos;
    if (n >= 0) {
        byte_pos = i - 1;
        size_t target = static_cast<size_t>(n);
        size_t count_pos = 0;
        while (byte_pos < len && count_pos + 1 < target) {
            uint32_t cp;
            int nb = utf8_decode(s, len, byte_pos, cp);
            if (nb <= 0)
                throw_runtime_error("bad argument #1 to 'offset' (invalid UTF-8 code)");
            byte_pos += nb;
            count_pos++;
        }
        if (count_pos + 1 < target)
            return MultiValue(LValue());
    } else {
        byte_pos = i - 1;
        size_t target = static_cast<size_t>(-n);
        size_t count_pos = 0;
        while (byte_pos > 0 && count_pos < target) {
            byte_pos--;
            while (byte_pos > 0 && (static_cast<unsigned char>(s[byte_pos]) & 0xC0) == 0x80)
                byte_pos--;
            count_pos++;
        }
        if (count_pos < target)
            return MultiValue(LValue());
    }
    return MultiValue(clx::integer(static_cast<int64_t>(byte_pos + 1)));
}

//------------------ luastd_utf8: registers the utf8 library into the global state
void luastd_utf8(LState* L) {
    LValue t = L->create_table();
    LTable* tbl = static_cast<LTable*>(t.as_pointer());
    tbl->bind_all(L, {
        {"char", utf8_char},
        {"codes", utf8_codes},
        {"codepoint", utf8_codepoint},
        {"len", utf8_len},
        {"offset", utf8_offset}
    });
    tbl->settable(LValue(L->intern_string("charpattern")),
                  LValue(L->intern_string(utf8_charpattern, sizeof(utf8_charpattern) - 1)));
    set_global(L, "utf8", t);
}
}
