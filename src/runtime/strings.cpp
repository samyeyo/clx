// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  strings.cpp · String Library               │
// └─────────────────────────────────────────────┘

#include "clx_runtime.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <climits>
#include <cstddef>
#include <memory>

namespace clx {


//------------------ get_string — extract string arg with bounds/type checking
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

//------------------ get_integer — extract integer arg with type coercion
static int64_t get_integer(LState* L, const LValue* args, size_t count, int idx) {
    if (idx < 1 || idx > (int)count) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bad argument #%d to a string function (number expected, got no value)", idx);
        throw_runtime_error(buf);
    }
    int64_t i;
    if (to_integer(args[idx - 1], i)) return i;
    double d;
    if (args[idx - 1].to_number(d)) return (int64_t)d;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "bad argument #%d to a string function (number expected, got %s)", idx,
        args[idx - 1].type == String ? "string" : "table");
    throw_runtime_error(buf);
}


//------------------ posrelat — convert relative position to absolute (1-based)
static size_t posrelat(int64_t pos, size_t len) {
    if (pos > 0) return (size_t)pos;
    else if (pos == 0) return 1;
    else if (pos < -(int64_t)len) return 1;
    else return len + (size_t)pos + 1;
}


//------------------ getendpos — convert relative end position to absolute
static size_t getendpos(int64_t pos, size_t len) {
    if (pos > (int64_t)len) return len;
    else if (pos >= 0) return (size_t)pos;
    else if (pos < -(int64_t)len) return 0;
    else return len + (size_t)pos + 1;
}



//------------------ str_len — string.len: return string length
MultiValue str_len(LState* L, const LValue* args, size_t count) {
    if (count == 0) throw_runtime_error("bad argument #1 to 'len' (string expected, got no value)");
    size_t l;
    get_string(L, args, count, l, 1);
    return MultiValue(LValue(static_cast<int64_t>(l)));
}


//------------------ str_sub — string.sub: extract substring by range
MultiValue str_sub(LState* L, const LValue* args, size_t count) {
    size_t l;
    const char* s = get_string(L, args, count, l, 1);
    int64_t start_i = (count >= 2 && args[1].type != Nil) ? get_integer(L, args, count, 2) : 1;
    int64_t end_i   = (count >= 3 && args[2].type != Nil) ? get_integer(L, args, count, 3) : (int64_t)l;


    size_t start, end;
    if (start_i >= 1 && end_i >= start_i && (size_t)end_i <= l) {
        start = (size_t)start_i;
        end   = (size_t)end_i;
    } else {
        start = posrelat(start_i, l);
        end   = getendpos(end_i, l);
    }

    if (start <= end) {
        size_t subl     = end - start + 1;
        const char* src = s + start - 1;

        return MultiValue(make_string_pooled(L, src, subl));
    } else {
        return MultiValue(LValue(L->intern_string("", 0)));
    }
}


//------------------ str_reverse — string.reverse: reverse string characters
MultiValue str_reverse(LState* L, const LValue* args, size_t count) {
    size_t l;
    const char* s = get_string(L, args, count, l, 1);
    if (l <= 6) {
        char buf[8];
        for (size_t i = 0; i < l; i++) buf[i] = s[l - i - 1];
        return MultiValue(LValue::istr(buf, static_cast<uint32_t>(l)));
    }
    char* dst = new char[l + 1];
    for (size_t i = 0; i < l; i++)
        dst[i] = s[l - i - 1];
    dst[l] = '\0';
    MultiValue result = MultiValue(make_string_pooled(L, dst, l));
    delete[] dst;
    return result;
}


//------------------ str_lower — string.lower: convert to lowercase
MultiValue str_lower(LState* L, const LValue* args, size_t count) {
    size_t l;
    const char* s = get_string(L, args, count, l, 1);
    if (l <= 6) {
        char buf[8];
        for (size_t i = 0; i < l; i++) buf[i] = (char)std::tolower((unsigned char)s[i]);
        return MultiValue(LValue::istr(buf, static_cast<uint32_t>(l)));
    }
    char* dst = new char[l + 1];
    for (size_t i = 0; i < l; i++)
        dst[i] = (char)std::tolower((unsigned char)s[i]);
    dst[l] = '\0';
    MultiValue result = MultiValue(make_string_pooled(L, dst, l));
    delete[] dst;
    return result;
}


//------------------ str_upper — string.upper: convert to uppercase
MultiValue str_upper(LState* L, const LValue* args, size_t count) {
    size_t l;
    const char* s = get_string(L, args, count, l, 1);
    if (l <= 6) {
        char buf[8];
        for (size_t i = 0; i < l; i++) buf[i] = (char)std::toupper((unsigned char)s[i]);
        return MultiValue(LValue::istr(buf, static_cast<uint32_t>(l)));
    }
    char* dst = new char[l + 1];
    for (size_t i = 0; i < l; i++)
        dst[i] = (char)std::toupper((unsigned char)s[i]);
    dst[l] = '\0';
    MultiValue result = MultiValue(make_string_pooled(L, dst, l));
    delete[] dst;
    return result;
}


//------------------ str_rep — string.rep: repeat string N times with optional separator
MultiValue str_rep(LState* L, const LValue* args, size_t count) {
    size_t len;
    const char* s = get_string(L, args, count, len, 1);
    int64_t n = (count >= 2 && args[1].type != Nil) ? get_integer(L, args, count, 2) : 1;
    size_t lsep = 0;
    const char* sep = "";
    if (count >= 3 && args[2].type == String) {
        sep = args[2].as_string();
        lsep = args[2].string_len();
    }
    if (n <= 0 || (len | lsep) == 0)
        return MultiValue(LValue(L->intern_string("", 0)));
    size_t totallen = ((size_t)n * (len + lsep)) - lsep;
    if (totallen <= 6) {
        char buf[8];
        char* bp = buf;
        for (int64_t i = 0; i < n - 1; i++) {
            std::memcpy(bp, s, len); bp += len;
            if (lsep > 0) { std::memcpy(bp, sep, lsep); bp += lsep; }
        }
        std::memcpy(bp, s, len);
        return MultiValue(LValue::istr(buf, static_cast<uint32_t>(totallen)));
    }
    char* dst = new char[totallen + 1];
    char* p = dst;
    for (int64_t i = 0; i < n - 1; i++) {
        std::memcpy(p, s, len); p += len;
        if (lsep > 0) { std::memcpy(p, sep, lsep); p += lsep; }
    }
    std::memcpy(p, s, len);
    dst[totallen] = '\0';
    MultiValue result = MultiValue(make_string_pooled(L, dst, totallen));
    delete[] dst;
    return result;
}


//------------------ str_byte — string.byte: get byte codes of substring
MultiValue str_byte(LState* L, const LValue* args, size_t count) {
    size_t l;
    const char* s = get_string(L, args, count, l, 1);
    int64_t pi = (count >= 2 && args[1].type != Nil) ? get_integer(L, args, count, 2) : 1;
    size_t posi = posrelat(pi, l);
    int64_t pe = (count >= 3 && args[2].type != Nil) ? get_integer(L, args, count, 3) : pi;
    size_t pose = getendpos(pe, l);
    if (posi > pose) return MultiValue();
    int n = (int)(pose - posi) + 1;
    LValue vals[8];
    LValue* vals_ptr = vals;
    std::vector<LValue> large_vals;
    if (n > 8) {
        large_vals.resize(n);
        vals_ptr = large_vals.data();
    }
    for (int i = 0; i < n; i++)
        vals_ptr[i] = LValue(static_cast<int64_t>((unsigned char)s[posi + i - 1]));
    if (n == 1) return MultiValue(vals_ptr[0]);
    return MultiValue(vals_ptr, n);
}


//------------------ str_char — string.char: build string from byte values
MultiValue str_char(LState* L, const LValue* args, size_t count) {
    if (count <= 6) {
        char buf[8];
        for (size_t i = 0; i < count; i++) {
            int64_t c;
            if (!to_integer(args[i], c) || c < 0 || c > 255) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "bad argument #%zu to 'char' (value out of range)", i + 1);
                throw_runtime_error(buf);
            }
            buf[i] = (char)(unsigned char)c;
        }
        return MultiValue(LValue::istr(buf, count));
    }
    char* dst = new char[count + 1];
    for (size_t i = 0; i < count; i++) {
        int64_t c;
        if (!to_integer(args[i], c) || c < 0 || c > 255) {
            delete[] dst;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "bad argument #%zu to 'char' (value out of range)", i + 1);
            throw_runtime_error(buf);
        }
        dst[i] = (char)(unsigned char)c;
    }
    dst[count] = '\0';
    MultiValue result = MultiValue(make_string_pooled(L, dst, count));
    delete[] dst;
    return result;
}


//------------------ str_dump — string.dump: stub (not supported)
MultiValue str_dump(LState* L, const LValue* args, size_t count) {
    throw_runtime_error("string.dump not supported");
}



//------------------ str_format — string.format: printf-style formatting
MultiValue str_format(LState* L, const LValue* args, size_t count) {
    size_t sfl;
    const char* strfrmt = get_string(L, args, count, sfl, 1);
    const char* end = strfrmt + sfl;
    int arg = 2;

    StringBuilder result;
    if (sfl > 32) result.reserve(sfl / 4 + 8);
    while (strfrmt < end) {
        if (*strfrmt != '%') {

            const char* lit = strfrmt;
            while (strfrmt < end && *strfrmt != '%') strfrmt++;
            result.append(lit, static_cast<uint32_t>(strfrmt - lit));
            continue;
        }
        strfrmt++;
        if (*strfrmt == '%') {
            result.append("%", 1);
            strfrmt++;
            continue;
        }

        char fmt_buf[64];
        int fmt_pos = 0;
        fmt_buf[fmt_pos++] = '%';

        while (*strfrmt && std::strchr("-+#0 ", *strfrmt) && fmt_pos < 62)
            fmt_buf[fmt_pos++] = *strfrmt++;

        if (*strfrmt == '*') {
            if (arg > (int)count)
                throw_runtime_error("no value for format width");
            arg++;
            strfrmt++;
        } else {
            while (*strfrmt && std::isdigit((unsigned char)*strfrmt) && fmt_pos < 62)
                fmt_buf[fmt_pos++] = *strfrmt++;
        }

        if (*strfrmt == '.') {
            fmt_buf[fmt_pos++] = '.';
            strfrmt++;
            if (*strfrmt == '*') {
                if (arg > (int)count)
                    throw_runtime_error("no value for format precision");
                arg++;
                strfrmt++;
            } else {
                while (*strfrmt && std::isdigit((unsigned char)*strfrmt) && fmt_pos < 62)
                    fmt_buf[fmt_pos++] = *strfrmt++;
            }
        }

        if (*strfrmt && std::strchr("hlLjzt", *strfrmt)) {
            if (*strfrmt == 'l' && *(strfrmt + 1) == 'l') {
                if (fmt_pos + 2 < 64) {
                    fmt_buf[fmt_pos++] = 'l';
                    fmt_buf[fmt_pos++] = 'l';
                }
                strfrmt += 2;
            } else {
                fmt_buf[fmt_pos++] = *strfrmt++;
            }
        }

        char spec = *strfrmt++;
        fmt_buf[fmt_pos++] = spec;
        fmt_buf[fmt_pos] = '\0';

        if (arg > (int)count)
            throw_runtime_error("no value for format argument");

        char buf[512];
        int written = 0;

        switch (spec) {
            case 'd': case 'i': {
                int64_t n = get_integer(L, args, count, arg++);
                written = std::snprintf(buf, sizeof(buf), fmt_buf, (long long)n);
                break;
            }
            case 'u': {
                int64_t n = get_integer(L, args, count, arg++);
                written = std::snprintf(buf, sizeof(buf), fmt_buf, (unsigned long long)n);
                break;
            }
            case 'o': case 'x': case 'X': {
                int64_t n = get_integer(L, args, count, arg++);
                written = std::snprintf(buf, sizeof(buf), fmt_buf, (unsigned long long)n);
                break;
            }
            case 'f': case 'e': case 'E': case 'g': case 'G': {
                double d;
                if (!args[arg - 1].to_number(d)) {
                    char ebuf[128];
                    std::snprintf(ebuf, sizeof(ebuf), "bad argument #%d to 'format' (number expected)", arg);
                    throw_runtime_error(ebuf);
                }
                arg++;
                written = std::snprintf(buf, sizeof(buf), fmt_buf, d);
                break;
            }
            case 'c': {
                int64_t n = get_integer(L, args, count, arg++);
                written = std::snprintf(buf, sizeof(buf), fmt_buf, (int)n);
                break;
            }
            case 's': {
                std::string sv = args[arg - 1].to_string(L);
                arg++;
                written = std::snprintf(buf, sizeof(buf), fmt_buf, sv.c_str());
                break;
            }
            case 'p': {
                void* ptr = (void*)0xDEAD;
                written = std::snprintf(buf, sizeof(buf), fmt_buf, ptr);
                arg++;
                break;
            }
            case 'q': {
                size_t slen;
                const char* ss = get_string(L, args, count, slen, arg++);
                buf[0] = '"';
                size_t pos = 1;
                for (size_t i = 0; i < slen && pos < sizeof(buf) - 4; i++) {
                    if (ss[i] == '"' || ss[i] == '\\') { buf[pos++] = '\\'; buf[pos++] = ss[i]; }
                    else if (ss[i] == '\n') { buf[pos++] = '\\'; buf[pos++] = 'n'; }
                    else if ((unsigned char)ss[i] < 32) {
                        int nw = std::snprintf(buf + pos, sizeof(buf) - pos, "\\%03d", (unsigned char)ss[i]);
                        if (nw > 0) pos += nw;
                    } else buf[pos++] = ss[i];
                }
                buf[pos++] = '"';
                written = (int)pos;
                break;
            }
            default:
                result.append_owned(fmt_buf, static_cast<uint32_t>(fmt_pos));
                continue;
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
    const char* src_init;
    const char* src_end;
    const char* p_end;
    LState* L;
    int matchdepth;
    int level;
    struct {
        const char* init;
        ptrdiff_t len;
    } capture[LUA_MAXCAPTURES];
};


//------------------ check_capture — validate capture index, throw if invalid
static int check_capture(MatchState* ms, int l) {
    l -= '1';
    if (l < 0 || l >= ms->level || ms->capture[l].len == CAP_UNFINISHED) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "invalid capture index %%%d", l + 1);
        throw_runtime_error(buf);
    }
    return l;
}

//------------------ capture_to_close — find nearest unfinished capture
static int capture_to_close(MatchState* ms) {
    int level = ms->level;
    for (level--; level >= 0; level--)
        if (ms->capture[level].len == CAP_UNFINISHED) return level;
    throw_runtime_error("invalid pattern capture");
}

static const char* match(MatchState* ms, const char* s, const char* p);

//------------------ classend — skip past a character class (incl. brackets/escapes)
static const char* classend(MatchState* ms, const char* p) {
    switch (*p++) {
        case L_ESC:
            if (p == ms->p_end) throw_runtime_error("malformed pattern (ends with '%')");
            return p + 1;
        case '[':
            if (*p == '^') p++;
            do {
                if (p == ms->p_end) throw_runtime_error("malformed pattern (missing ']')");
                if (*p == L_ESC && *(p + 1) != '\0') p++;
                p++;
            } while (*p != ']');
            return p + 1;
        default:
            return p;
    }
}

//------------------ match_class — test char against Lua character class (%a, %d, etc.)
static int match_class(int c, int cl) {
    int res;
    switch (std::tolower(cl)) {
        case 'a': res = std::isalpha(c); break;
        case 'c': res = std::iscntrl(c); break;
        case 'd': res = std::isdigit(c); break;
        case 'g': res = std::isgraph(c); break;
        case 'l': res = std::islower(c); break;
        case 'p': res = std::ispunct(c); break;
        case 's': res = std::isspace(c); break;
        case 'u': res = std::isupper(c); break;
        case 'w': res = std::isalnum(c); break;
        case 'x': res = std::isxdigit(c); break;
        case 'z': res = (c == 0); break;
        default: return (cl == c);
    }
    return (std::islower(cl) ? res : !res);
}

//------------------ matchbracketclass — test char against [...] bracket expression
static int matchbracketclass(int c, const char* p, const char* ec) {
    int sig = 1;
    if (*(p + 1) == '^') { sig = 0; p++; }
    while (++p < ec) {
        if (*p == L_ESC) {
            p++;
            if (match_class((unsigned char)c, (unsigned char)*p)) return sig;
        } else if ((*(p + 1) == '-') && (p + 2 < ec)) {
            p += 2;
            if ((unsigned char)*(p - 2) <= (unsigned char)c && (unsigned char)c <= (unsigned char)*p) return sig;
        } else if ((unsigned char)*p == (unsigned char)c) return sig;
    }
    return !sig;
}

//------------------ singlematch — match one character against a class (no quantifier)
static int singlematch(MatchState* ms, const char* s, const char* p, const char* ep) {
    if (s >= ms->src_end) return 0;
    int c = (unsigned char)*s;
    switch (*p) {
        case '.': return 1;
        case L_ESC: return match_class(c, (unsigned char)*(p + 1));
        case '[': return matchbracketclass(c, p, ep - 1);
        default: return ((unsigned char)*p == (unsigned char)c);
    }
}

//------------------ matchbalance — match %bxy balanced pair
static const char* matchbalance(MatchState* ms, const char* s, const char* p) {
    if (p >= ms->p_end - 1) throw_runtime_error("malformed pattern (missing arguments to '%b')");
    if (*s != *p) return NULL;
    int b = *p;
    int e = *(p + 1);
    int cont = 1;
    while (++s < ms->src_end) {
        if (*s == e) { if (--cont == 0) return s + 1; }
        else if (*s == b) cont++;
    }
    return NULL;
}

//------------------ max_expand — greedy quantifier (*) match
static const char* max_expand(MatchState* ms, const char* s, const char* p, const char* ep) {
    ptrdiff_t i = 0;
    while (singlematch(ms, s + i, p, ep)) i++;
    while (i >= 0) {
        const char* res = match(ms, s + i, ep + 1);
        if (res) return res;
        i--;
    }
    return NULL;
}

//------------------ min_expand — non-greedy quantifier (-) match
static const char* min_expand(MatchState* ms, const char* s, const char* p, const char* ep) {
    for (;;) {
        const char* res = match(ms, s, ep + 1);
        if (res) return res;
        else if (singlematch(ms, s, p, ep)) s++;
        else return NULL;
    }
}

//------------------ start_capture — begin a capture group (open paren)
static const char* start_capture(MatchState* ms, const char* s, const char* p, int what) {
    int level = ms->level;
    if (level >= LUA_MAXCAPTURES) throw_runtime_error("too many captures");
    ms->capture[level].init = s;
    ms->capture[level].len = what;
    ms->level = level + 1;
    const char* res = match(ms, s, p);
    if (res == NULL) ms->level--;
    return res;
}

//------------------ end_capture — close a capture group (close paren)
static const char* end_capture(MatchState* ms, const char* s, const char* p) {
    int l = capture_to_close(ms);
    ms->capture[l].len = s - ms->capture[l].init;
    const char* res = match(ms, s, p);
    if (res == NULL) ms->capture[l].len = CAP_UNFINISHED;
    return res;
}

//------------------ match_capture — match previously captured group (%1..%9)
static const char* match_capture(MatchState* ms, const char* s, int l) {
    l = check_capture(ms, l);
    size_t len = (size_t)ms->capture[l].len;
    if ((size_t)(ms->src_end - s) >= len && std::memcmp(ms->capture[l].init, s, len) == 0)
        return s + len;
    return NULL;
}

//------------------ match — main recursive pattern-matching engine
static const char* match(MatchState* ms, const char* s, const char* p) {
    if (ms->matchdepth-- == 0) throw_runtime_error("pattern too complex");
init:
    if (p != ms->p_end) {
        switch (*p) {
            case '(':
                s = (*(p + 1) == ')') ? start_capture(ms, s, p + 2, CAP_POSITION) : start_capture(ms, s, p + 1, CAP_UNFINISHED);
                break;
            case ')':
                s = end_capture(ms, s, p + 1);
                break;
            case '$':
                if ((p + 1) != ms->p_end) goto dflt;
                s = (s == ms->src_end) ? s : NULL;
                break;
            case L_ESC:
                switch (*(p + 1)) {
                    case 'b': {
                        s = matchbalance(ms, s, p + 2);
                        if (s) { p += 4; goto init; }
                        break;
                    }
                    case 'f': {
                        const char* ep;
                        char previous;
                        p += 2;
                        if (*p != '[') throw_runtime_error("missing '[' after '%f' in pattern");
                        ep = classend(ms, p);
                        previous = (s == ms->src_init) ? '\0' : *(s - 1);
                        if (!matchbracketclass((unsigned char)previous, p, ep - 1) &&
                            matchbracketclass((unsigned char)*s, p, ep - 1)) {
                            p = ep; goto init;
                        }
                        s = NULL;
                        break;
                    }
                    case '0': case '1': case '2': case '3': case '4':
                    case '5': case '6': case '7': case '8': case '9':
                        s = match_capture(ms, s, (unsigned char)*(p + 1));
                        if (s) { p += 2; goto init; }
                        break;
                    default: goto dflt;
                }
                break;
            default:
            dflt: {
                const char* ep = classend(ms, p);
                if (!singlematch(ms, s, p, ep)) {
                    if (*ep == '*' || *ep == '?' || *ep == '-') { p = ep + 1; goto init; }
                    else s = NULL;
                } else {
                    switch (*ep) {
                        case '?': {
                            const char* res = match(ms, s + 1, ep + 1);
                            if (res) s = res;
                            else { p = ep + 1; goto init; }
                            break;
                        }
                        case '+': s++;
                        case '*': s = max_expand(ms, s, p, ep); break;
                        case '-': s = min_expand(ms, s, p, ep); break;
                        default: s++; p = ep; goto init;
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
static const char* lmemfind(const char* s1, size_t l1, const char* s2, size_t l2) {
    if (l2 == 0) return s1;
    if (l2 > l1) return NULL;
    const char* init;
    l2--;
    l1 = l1 - l2;
    while (l1 > 0 && (init = (const char*)std::memchr(s1, *s2, l1)) != NULL) {
        init++;
        if (std::memcmp(init, s2 + 1, l2) == 0) return init - 1;
        l1 -= (size_t)(init - s1);
        s1 = init;
    }
    return NULL;
}

//------------------ get_onecapture — retrieve a single capture by index
static ptrdiff_t get_onecapture(MatchState* ms, int i, const char* s, const char* e, const char** cap) {
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
        if (capl == CAP_UNFINISHED) throw_runtime_error("unfinished capture");
        return capl;
    }
}

//------------------ prepstate — initialise MatchState with source & pattern
static void prepstate(MatchState* ms, LState* L, const char* s, size_t ls, const char* p, size_t lp) {
    ms->L = L;
    ms->src_init = s;
    ms->src_end = s + ls;
    ms->p_end = p + lp;
}

//------------------ reprepstate — reset match depth and captures for new attempt
static void reprepstate(MatchState* ms) {
    ms->matchdepth = MAXCCALLS;
    ms->level = 0;
}

//------------------ nospecials — check if pattern has no magic characters
static int nospecials(const char* p, size_t l) {
    size_t upto = 0;
    do {
        if (std::strpbrk(p + upto, SPECIALS)) return 0;
        upto += std::strlen(p + upto) + 1;
    } while (upto <= l);
    return 1;
}


//------------------ str_find_aux — shared implementation of find/match
static MultiValue str_find_aux(LState* L, const LValue* args, size_t count, int find) {
    size_t ls, lp;
    const char* s = get_string(L, args, count, ls, 1);
    const char* p = get_string(L, args, count, lp, 2);
    int64_t init_i = (count >= 3 && args[2].type != Nil) ? get_integer(L, args, count, 3) : 1;
    size_t init = posrelat(init_i, ls) - 1;
    if (init > ls) return MultiValue();

    bool plain = (count >= 4) ? args[3].as_bool() : false;

    if (find && (plain || nospecials(p, lp))) {
        const char* s2 = lmemfind(s + init, ls - init, p, lp);
        if (s2) {
            return MultiValue({
                LValue(static_cast<int64_t>((s2 - s) + 1)),
                LValue(static_cast<int64_t>((s2 - s) + lp))
            });
        }
    } else {
        MatchState ms;
        const char* s1 = s + init;
        int anchor = (*p == '^');
        if (anchor) { p++; lp--; }
        prepstate(&ms, L, s, ls, p, lp);
        do {
            reprepstate(&ms);
            const char* res = match(&ms, s1, p);
            if (res) {
                if (find) {
                    LValue results[LUA_MAXCAPTURES + 2];
                    size_t rc = 0;
                    results[rc++] = LValue(static_cast<int64_t>((s1 - s) + 1));
                    results[rc++] = LValue(static_cast<int64_t>(res - s));
                    int nlevels = (ms.level == 0) ? 1 : ms.level;
                    for (int ci = 0; ci < nlevels; ci++) {
                        const char* cap;
                        ptrdiff_t capl = get_onecapture(&ms, ci, s1, res, &cap);
                        results[rc++] = (capl == CAP_POSITION)
                            ? LValue(static_cast<int64_t>((cap - s) + 1))
                            : L->intern_lvalue(cap, (size_t)capl);
                    }
                    return MultiValue(results, rc);
                } else {
                    int nlevels = (ms.level == 0) ? 1 : ms.level;
                    LValue results[LUA_MAXCAPTURES];
                    size_t rc = 0;
                    for (int ci = 0; ci < nlevels; ci++) {
                        const char* cap;
                        ptrdiff_t capl = get_onecapture(&ms, ci, s1, res, &cap);
                        results[rc++] = (capl == CAP_POSITION)
                            ? LValue(static_cast<int64_t>((cap - s) + 1))
                            : L->intern_lvalue(cap, (size_t)capl);
                    }
                    return MultiValue(results, rc);
                }
            }
            s1++;
        } while (s1 <= ms.src_end && !anchor);
    }
    return MultiValue();
}

//------------------ str_find — string.find: locate pattern in string
MultiValue str_find(LState* L, const LValue* args, size_t count) {
    return str_find_aux(L, args, count, 1);
}

//------------------ str_match — string.match: capture pattern match
MultiValue str_match(LState* L, const LValue* args, size_t count) {
    return str_find_aux(L, args, count, 0);
}


//------------------ GMatchState — state for gmatch iterator
struct GMatchState {
    const char* src;
    const char* p;
    const char* lastmatch;
    MatchState ms;
};

//------------------ gmatch_aux — gmatch iterator body (called per-iteration)
static MultiValue gmatch_aux(LState* L, const LValue* args, size_t count, void* ud) {
    GMatchState* gm = (GMatchState*)ud;
    const char* src;
    gm->ms.L = L;
    for (src = gm->src; src <= gm->ms.src_end; src++) {
        const char* e;
        reprepstate(&gm->ms);
        if ((e = match(&gm->ms, src, gm->p)) != NULL && e != gm->lastmatch) {
            gm->src = gm->lastmatch = e;
            int nlevels = (gm->ms.level == 0) ? 1 : gm->ms.level;
            LValue results[LUA_MAXCAPTURES];
            size_t rc = 0;
            for (int ci = 0; ci < nlevels; ci++) {
                const char* cap;
                ptrdiff_t capl = get_onecapture(&gm->ms, ci, src, e, &cap);
                results[rc++] = (capl == CAP_POSITION)
                    ? LValue(static_cast<int64_t>((cap - gm->ms.src_init) + 1))
                    : L->intern_lvalue(cap, (size_t)capl);
            }
            if (rc == 1) return MultiValue(results[0]);
            return MultiValue(results, rc);
        }
    }
    return MultiValue();
}

//------------------ str_gmatch — string.gmatch: global match iterator
MultiValue str_gmatch(LState* L, const LValue* args, size_t count) {
    size_t ls, lp;
    const char* s = get_string(L, args, count, ls, 1);
    const char* p = get_string(L, args, count, lp, 2);
    s = L->intern_string(s, ls);
    p = L->intern_string(p, lp);
    int64_t init_i = (count >= 3 && args[2].type != Nil) ? get_integer(L, args, count, 3) : 1;
    size_t init = posrelat(init_i, ls) - 1;
    if (init > ls) init = ls + 1;

    auto gm = std::make_shared<GMatchState>();
    prepstate(&gm->ms, L, s, ls, p, lp);
    gm->src = s + init;
    gm->p = p;
    gm->lastmatch = NULL;

    auto iter = [gm](LState* L2, const LValue* a2, size_t c2) -> MultiValue {
        (void)a2; (void)c2;
        return gmatch_aux(L2, a2, c2, gm.get());
    };
    LValue iter_val = L->create_closure(iter);
    return MultiValue(iter_val);
}


//------------------ str_gsub — string.gsub: global search-and-replace
MultiValue str_gsub(LState* L, const LValue* args, size_t count) {
    size_t srcl, lp;
    const char* src = get_string(L, args, count, srcl, 1);
    const char* p   = get_string(L, args, count, lp, 2);

    ValueType tr_type = (count >= 3) ? args[2].type : Nil;
    int64_t max_s = (count >= 4 && args[3].type != Nil) ? get_integer(L, args, count, 4) : (int64_t)srcl + 1;
    int anchor = (*p == '^');
    if (anchor) { p++; lp--; }

    MatchState ms;
    int64_t n = 0;
    bool changed = false;
    const char* lastmatch = NULL;
    const char* src_pos = src;

    prepstate(&ms, L, src, srcl, p, lp);


    StringBuilder result;
    if (srcl > 64) result.reserve(std::min<size_t>(srcl / 4 + 8, 4096));

    while (n < max_s) {
        reprepstate(&ms);
        const char* e;
        if ((e = match(&ms, src_pos, p)) != NULL && e != lastmatch) {
            n++;
            changed = true;

            if (tr_type == String) {
                const char* news = args[2].as_string();
                size_t l = args[2].string_len();
                StringBuilder repl;
                size_t i = 0;
                while (i < l) {
                    if (news[i] == L_ESC && i + 1 < l) {
                        i++;
                        if (news[i] == L_ESC) {
                            repl.append("%", 1);
                        } else if (news[i] == '0') {
                            size_t ml = e - src_pos;
                            repl.append(src_pos, static_cast<uint32_t>(ml));
                        } else if (std::isdigit((unsigned char)news[i])) {
                            int ci = news[i] - '1';
                            const char* cap;
                            ptrdiff_t capl = get_onecapture(&ms, ci, src_pos, e, &cap);
                            if (capl == CAP_POSITION) {
                                char tmp[24];
                                int tw = std::snprintf(tmp, sizeof(tmp), "%lld", (long long)((cap - src) + 1));
                                repl.append_owned(tmp, static_cast<uint32_t>(tw));
                            } else {
                                repl.append(cap, static_cast<uint32_t>(capl));
                            }
                        }
                        i++;
                    } else {
                        size_t start = i;
                        while (i < l && news[i] != L_ESC) i++;
                        repl.append(&news[start], static_cast<uint32_t>(i - start));
                    }
                }
                result.append(repl);

            } else if (tr_type == Function) {
                int nlevels = (ms.level == 0) ? 1 : ms.level;
                std::vector<LValue> fargs(nlevels);
                for (int ci = 0; ci < nlevels; ci++) {
                    const char* cap;
                    ptrdiff_t capl = get_onecapture(&ms, ci, src_pos, e, &cap);
                    if (capl == CAP_POSITION)
                        fargs[ci] = LValue(static_cast<int64_t>((cap - src) + 1));
                    else
                        fargs[ci] = LValue(L->intern_string(cap, (size_t)capl));
                }
                MultiValue fret = call_function(L, args[2], fargs.data(), fargs.size(), "gsub", 0);
                if (fret.count > 0 && fret[0].type == String) {

                    result.append(L, fret[0]);
                } else {
                    size_t ml = e - src_pos;
                    result.append(src_pos, static_cast<uint32_t>(ml));
                }

            } else if (tr_type == Table) {
                const char* cap;
                ptrdiff_t capl = get_onecapture(&ms, 0, src_pos, e, &cap);
                LValue key = (capl == CAP_POSITION)
                    ? LValue(static_cast<int64_t>((cap - src) + 1))
                    : LValue(L->intern_string(cap, (size_t)capl));
                LValue val = static_cast<LTable*>(args[2].as_pointer())->gettable(key);
                if (val.type == String) {
                    result.append(L, val);
                } else {
                    size_t ml = e - src_pos;
                    result.append(src_pos, static_cast<uint32_t>(ml));
                }
            }

            src_pos = lastmatch = e;
        } else if (src_pos < ms.src_end) {
            result.append(src_pos, 1);
            src_pos++;
        } else break;
        if (anchor) break;
    }

    if (!changed)
        return MultiValue({
            LValue(L->intern_string(src, srcl)),
            LValue(static_cast<int64_t>(0))
        });


    size_t tail = ms.src_end - src_pos;
    if (tail > 0)
        result.append(L->intern_string(src_pos, tail), static_cast<uint32_t>(tail));

    return MultiValue({
        result.to_lvalue(L),
        LValue(static_cast<int64_t>(n))
    });
}



#define LUAL_PACKPADBYTE 0x00
#define MAXINTSIZE 16
#define NB CHAR_BIT
#define MC ((1 << NB) - 1)
#define SZINT ((int)sizeof(int64_t))

static const union { int dummy; char little; } nativeendian = {1};

//------------------ Header — pack/unpack format state (endian, alignment)
struct Header {
    LState* L;
    int islittle;
    unsigned maxalign;
};

enum KOption {
    Kint,
    Kuint,
    Kfloat,
    Knumber,
    Kdouble,
    Kchar,
    Kstring,
    Kzstr,
    Kpadding,
    Kpaddalign,
    Knop
};

//------------------ digit — test if char is a decimal digit
static int digit(int c) { return '0' <= c && c <= '9'; }

//------------------ getnum — parse decimal number from format string
static size_t getnum(const char** fmt, size_t df) {
    if (!digit(**fmt)) return df;
    size_t a = 0;
    do {
        a = a * 10 + (unsigned)(*(*fmt)++ - '0');
    } while (digit(**fmt) && a <= ((size_t)-1 - 9) / 10);
    return a;
}

//------------------ getnumlimit — parse number with size-limit validation
static unsigned getnumlimit(Header* h, const char** fmt, size_t df) {
    size_t sz = getnum(fmt, df);
    if ((sz - 1u) >= MAXINTSIZE) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "integral size (%zu) out of limits [1,%d]", sz, MAXINTSIZE);
        throw_runtime_error(buf);
    }
    return (unsigned)sz;
}

//------------------ initheader — initialise pack header (endian, alignment)
static void initheader(LState* L, Header* h) {
    h->L = L;
    h->islittle = nativeendian.little;
    h->maxalign = 1;
}

//------------------ getoption — parse a single pack format option char
static KOption getoption(Header* h, const char** fmt, size_t* size) {
    struct cD { char c; double u; };
    int opt = *(*fmt)++;
    *size = 0;
    switch (opt) {
        case 'b': *size = sizeof(char); return Kint;
        case 'B': *size = sizeof(char); return Kuint;
        case 'h': *size = sizeof(short); return Kint;
        case 'H': *size = sizeof(short); return Kuint;
        case 'l': *size = sizeof(long); return Kint;
        case 'L': *size = sizeof(long); return Kuint;
        case 'j': *size = sizeof(int64_t); return Kint;
        case 'J': *size = sizeof(int64_t); return Kuint;
        case 'T': *size = sizeof(size_t); return Kuint;
        case 'f': *size = sizeof(float); return Kfloat;
        case 'n': *size = sizeof(double); return Knumber;
        case 'd': *size = sizeof(double); return Kdouble;
        case 'i': *size = getnumlimit(h, fmt, sizeof(int)); return Kint;
        case 'I': *size = getnumlimit(h, fmt, sizeof(int)); return Kuint;
        case 's': *size = getnumlimit(h, fmt, sizeof(size_t)); return Kstring;
        case 'c': {
            *size = getnum(fmt, (size_t)-1);
            if (*size == (size_t)-1)
                throw_runtime_error("missing size for format option 'c'");
            return Kchar;
        }
        case 'z': return Kzstr;
        case 'x': *size = 1; return Kpadding;
        case 'X': return Kpaddalign;
        case ' ': break;
        case '<': h->islittle = 1; break;
        case '>': h->islittle = 0; break;
        case '=': h->islittle = nativeendian.little; break;
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
static KOption getdetails(Header* h, size_t totalsize, const char** fmt,
                           size_t* psize, unsigned* ntoalign) {
    KOption opt = getoption(h, fmt, psize);
    size_t align = *psize;
    if (opt == Kpaddalign) {
        if (**fmt == '\0' || getoption(h, fmt, &align) == Kchar || align == 0)
            throw_runtime_error("invalid next option for option 'X'");
    }
    if (align <= 1 || opt == Kchar) {
        *ntoalign = 0;
    } else {
        if (align > h->maxalign) align = h->maxalign;
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
static void packint(std::string& buf, uint64_t n, int islittle, unsigned size, int neg) {
    size_t pos = buf.size();
    buf.resize(pos + size);
    unsigned i;
    char* buff = &buf[pos];
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
static void copywithendian(char* dest, const char* src, unsigned size, int islittle) {
    if (islittle == nativeendian.little)
        std::memcpy(dest, src, size);
    else {
        dest += size - 1;
        while (size-- != 0) *dest-- = *src++;
    }
}

//------------------ unpackint — read integer from buffer with endianness
static int64_t unpackint(const char* str, int islittle, int size, int issigned) {
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
MultiValue str_pack(LState* L, const LValue* args, size_t count) {
    size_t fmt_len;
    const char* fmt = get_string(L, args, count, fmt_len, 1);
    int arg = 1;
    size_t totalsize = 0;
    Header h;
    initheader(L, &h);
    std::string buf;
    while (*fmt != '\0') {
        unsigned ntoalign;
        size_t size;
        KOption opt = getdetails(&h, totalsize, &fmt, &size, &ntoalign);
        if (ntoalign + size > (size_t)-1 - totalsize)
            throw_runtime_error("result too long");
        totalsize += ntoalign + size;
        buf.append(ntoalign, (char)LUAL_PACKPADBYTE);
        arg++;
        switch (opt) {
            case Kint: {
                int64_t n = get_integer(L, args, count, arg);
                if (size < SZINT) {
                    int64_t lim = (int64_t)1 << ((size * NB) - 1);
                    if (n < -lim || n >= lim)
                        throw_runtime_error("integer overflow");
                }
                packint(buf, (uint64_t)n, h.islittle, (unsigned)size, (n < 0));
                break;
            }
            case Kuint: {
                int64_t n = get_integer(L, args, count, arg);
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
                    char ebuf[128];
                    std::snprintf(ebuf, sizeof(ebuf), "bad argument #%d to 'pack' (number expected)", arg);
                    throw_runtime_error(ebuf);
                }
                float f = (float)d;
                size_t pos = buf.size();
                buf.resize(pos + sizeof(f));
                copywithendian(&buf[pos], (const char*)&f, sizeof(f), h.islittle);
                break;
            }
            case Knumber: {
                double d;
                if (!args[arg - 1].to_number(d)) {
                    char ebuf[128];
                    std::snprintf(ebuf, sizeof(ebuf), "bad argument #%d to 'pack' (number expected)", arg);
                    throw_runtime_error(ebuf);
                }
                size_t pos = buf.size();
                buf.resize(pos + sizeof(d));
                copywithendian(&buf[pos], (const char*)&d, sizeof(d), h.islittle);
                break;
            }
            case Kdouble: {
                double d;
                if (!args[arg - 1].to_number(d)) {
                    char ebuf[128];
                    std::snprintf(ebuf, sizeof(ebuf), "bad argument #%d to 'pack' (number expected)", arg);
                    throw_runtime_error(ebuf);
                }
                size_t pos = buf.size();
                buf.resize(pos + sizeof(d));
                copywithendian(&buf[pos], (const char*)&d, sizeof(d), h.islittle);
                break;
            }
            case Kchar: {
                size_t len;
                const char* s = get_string(L, args, count, len, arg);
                if (len > size) {
                    char ebuf[128];
                    std::snprintf(ebuf, sizeof(ebuf), "bad argument #%d to 'pack' (string longer than given size)", arg);
                    throw_runtime_error(ebuf);
                }
                buf.append(s, len);
                if (len < size)
                    buf.append(size - len, (char)LUAL_PACKPADBYTE);
                break;
            }
            case Kstring: {
                size_t len;
                const char* s = get_string(L, args, count, len, arg);
                if (size < (int)sizeof(uint64_t) && len >= ((uint64_t)1 << (size * NB))) {
                    char ebuf[128];
                    std::snprintf(ebuf, sizeof(ebuf), "bad argument #%d to 'pack' (string length does not fit in given size)", arg);
                    throw_runtime_error(ebuf);
                }
                packint(buf, (uint64_t)len, h.islittle, (unsigned)size, 0);
                buf.append(s, len);
                totalsize += len;
                break;
            }
            case Kzstr: {
                size_t len;
                const char* s = get_string(L, args, count, len, arg);
                if (std::strlen(s) != len) {
                    char ebuf[128];
                    std::snprintf(ebuf, sizeof(ebuf), "bad argument #%d to 'pack' (string contains zeros)", arg);
                    throw_runtime_error(ebuf);
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
MultiValue str_packsize(LState* L, const LValue* args, size_t count) {
    size_t fmt_len;
    const char* fmt = get_string(L, args, count, fmt_len, 1);
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
    return MultiValue(LValue(static_cast<int64_t>(totalsize)));
}

//------------------ str_unpack — string.unpack: unpack values from binary string
MultiValue str_unpack(LState* L, const LValue* args, size_t count) {
    size_t fmt_len;
    const char* fmt = get_string(L, args, count, fmt_len, 1);
    size_t ld;
    const char* data = get_string(L, args, count, ld, 2);
    int64_t pos_i = (count >= 3 && args[2].type != Nil) ? get_integer(L, args, count, 3) : 1;
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
                copywithendian((char*)&f, data + pos, sizeof(f), h.islittle);
                results.push_back(LValue((double)f));
                break;
            }
            case Knumber: {
                double f;
                copywithendian((char*)&f, data + pos, sizeof(f), h.islittle);
                results.push_back(LValue(f));
                break;
            }
            case Kdouble: {
                double f;
                copywithendian((char*)&f, data + pos, sizeof(f), h.islittle);
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
    return MultiValue(results.data(), results.size());
}


//------------------ luastd_string — register string library and metatable
void luastd_string(LState* L) {
    LValue string_table = L->create_table();
    LTable* t = static_cast<LTable*>(string_table.as_pointer());

    static constexpr clx::LazyReg strings_funcs[] =
    {
        {"len",      str_len},
        {"sub",      str_sub},
        {"reverse",  str_reverse},
        {"lower",    str_lower},
        {"upper",    str_upper},
        {"rep",      str_rep},
        {"byte",     str_byte},
        {"char",     str_char},
        {"dump",     str_dump},
        {"format",   str_format},
        {"find",     str_find},
        {"match",    str_match},
        {"gmatch",   str_gmatch},
        {"gsub",     str_gsub},
        {"pack",     str_pack},
        {"packsize", str_packsize},
        {"unpack",   str_unpack}
    };
    clx::set_lazy_funcs(L, string_table, strings_funcs, std::size(strings_funcs));
    set_global(L, "string", string_table);

    LValue mt = L->create_table();
    L->string_metatable = static_cast<LTable*>(mt.as_pointer());
    L->string_metatable->settable(LValue(L->intern_string("__index")), string_table);
}

}
