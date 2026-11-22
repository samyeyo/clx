// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  io.cpp · I/O Library                       │
// └─────────────────────────────────────────────┘

#ifdef WIN32
#define NOMINMAX
#endif

#include "clx.h"
#include "internal.h"
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#include <cstdio>
#include <io.h>
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#endif

namespace clx {

struct FileUd {
    FILE *fp;
    bool close_on_gc;
    bool is_pipe;
    bool is_std;
};

//------------------ cannot_close_std: stock's answer to io.close on stdin/stdout/stderr
static inline MultiValue cannot_close_std(LState *L) {
    return MultiValue({ LValue(), LValue(L->intern_string("cannot close standard file")) });
}

//------------------ per-FILE* output batching: userspace buffering drops the two mutexes per write; std handles share one FILE*
struct OutBufEnt {
    FILE *fp = nullptr;
    char *buf = nullptr;
    size_t len = 0;
    size_t cap = 0;
    int mode = 0;
    bool mode_init = false;
};

static constexpr size_t kOutBufSlots = 16;
static constexpr size_t kOutBufCap = 8192;
static OutBufEnt out_bufs[kOutBufSlots];

static OutBufEnt *outbuf_find(FILE *fp) {
    for (size_t i = 0; i < kOutBufSlots; ++i)
        if (out_bufs[i].fp == fp)
            return &out_bufs[i];
    return nullptr;
}

static void outbuf_flush_all();

static OutBufEnt *outbuf_get(FILE *fp) {
    if (OutBufEnt *e = outbuf_find(fp))
        return e;
    for (size_t i = 0; i < kOutBufSlots; ++i) {
        if (out_bufs[i].fp == nullptr) {
            out_bufs[i].fp = fp;
            static bool atexit_registered = false;
            if (!atexit_registered) {
                std::atexit(outbuf_flush_all);
                atexit_registered = true;
            }
            return &out_bufs[i];
        }
    }
    return nullptr;
}

static void outbuf_flush_ent(OutBufEnt *e) {
    if (e->len > 0 && e->fp) {
        std::fwrite(e->buf, 1, e->len, e->fp);
        e->len = 0;
    }
}

//------------------ outbuf_default_mode: stock-matching buffering — stderr unbuffered, tty line-buffered, else batched
static int outbuf_default_mode(FILE *fp) {
    if (fp == stderr)
        return 2;
#if defined(_WIN32) || defined(_WIN64)
    return (_isatty(_fileno(fp)) ? 1 : 0);
#else
    return (isatty(fileno(fp)) ? 1 : 0);
#endif
}

static void outbuf_write(FILE *fp, const char *p, size_t n) {
    if (n == 0 || !fp)
        return;
    //------------------ CLX_IO_NOBUF escape hatch (A/B + exotic FILE*): cached, getenv is O(environ).
    static int nobuf = -1;
    if (nobuf < 0)
        nobuf = std::getenv("CLX_IO_NOBUF") ? 1 : 0;
    if (nobuf) {
        std::fwrite(p, 1, n, fp);
        return;
    }
    OutBufEnt *e = outbuf_get(fp);
    if (!e) {
        std::fwrite(p, 1, n, fp);
        return;
    }
    if (!e->mode_init) {
        e->mode = outbuf_default_mode(fp);
        e->mode_init = true;
    }
    if (e->mode == 2) {
        std::fwrite(p, 1, n, fp);
        return;
    }
    if (!e->buf) {
        e->buf = static_cast<char *>(std::malloc(kOutBufCap));
        if (!e->buf) {
            std::fwrite(p, 1, n, fp);
            return;
        }
        e->cap = kOutBufCap;
    }
    if (n >= e->cap) {
        outbuf_flush_ent(e);
        std::fwrite(p, 1, n, fp);
        return;
    }
    if (e->len + n > e->cap)
        outbuf_flush_ent(e);
    std::memcpy(e->buf + e->len, p, n);
    e->len += n;
    if (e->mode == 1 && std::memchr(p, '\n', n))
        outbuf_flush_ent(e);
}

//------------------ outbuf_flush_fp — push one FILE*'s batch to libc (read/seek/flush/close paths).
static void outbuf_flush_fp(FILE *fp) {
    if (fp)
        if (OutBufEnt *e = outbuf_find(fp))
            outbuf_flush_ent(e);
}

//------------------ outbuf_set_mode — record a setvbuf mode (0 full, 1 line, 2 no) for a FILE*.
static void outbuf_set_mode(FILE *fp, int mode) {
    if (!fp)
        return;
    OutBufEnt *e = outbuf_get(fp);
    if (!e)
        return;
    outbuf_flush_fp(fp);
    e->mode = mode;
    e->mode_init = true;
}

static void outbuf_drop_fp(FILE *fp) {
    if (!fp)
        return;
    if (OutBufEnt *e = outbuf_find(fp)) {
        outbuf_flush_ent(e);
        std::free(e->buf);
        e->buf = nullptr;
        e->len = e->cap = 0;
        e->mode = 0;
        e->mode_init = false;
        e->fp = nullptr;
    }
}

//------------------ outbuf_flush_all: flush every batched FILE* on state close and atexit (os.exit bypasses close)
static void outbuf_flush_all() {
    for (size_t i = 0; i < kOutBufSlots; ++i)
        outbuf_flush_ent(&out_bufs[i]);
}

struct LinesIterUd {
    FileUd *f;
    char buf[4096];
};

static FileUd *as_file(LState *L, const LValue &v, const char *fname = "io.read") {
    if (v.type != UserData)
        throw_runtime_error(bad_arg_msg(L, 1, fname, std::string("FILE* expected, got ") + stock_type_name(v)));
    LUserdata *ud = static_cast<LUserdata *>(v.as_pointer());
    if (ud->size < sizeof(FileUd))
        throw_runtime_error(bad_arg_msg(L, 1, fname, "FILE* expected"));
    FileUd *f = static_cast<FileUd *>(ud->data());
    if (!f->fp && !f->is_pipe)
        throw_runtime_error("attempt to use a closed file");
    return f;
}

//------------------ read_line — read one line from fp, strip \n (keep_eol=false) or keep it, intern result
static LValue read_line(LState *L, FILE *fp, bool keep_eol = false) {
    char buf[4096];
    if (!std::fgets(buf, sizeof(buf), fp))
        return LValue();
    size_t n = std::strlen(buf);
    bool has_eol = (n > 0 && buf[n - 1] == '\n');
    if (n < sizeof(buf) - 1 || has_eol) {
        if (has_eol && !keep_eol)
            n--;
        return LValue(L->intern_string(buf, n));
    }
    StringBuilder sb;
    sb.append_owned(buf, static_cast<uint32_t>(n));
    while (std::fgets(buf, sizeof(buf), fp)) {
        n = std::strlen(buf);
        bool eol = (n > 0 && buf[n - 1] == '\n');
        bool done = (n < sizeof(buf) - 1 || eol);
        sb.append_owned(buf, static_cast<uint32_t>(keep_eol ? n : (eol ? n - 1 : n)));
        if (done)
            break;
    }
    return sb.to_lvalue(L);
}

//------------------ read_all — read entire file into interned string
static LValue read_all(LState *L, FILE *fp) {
    static constexpr size_t CHUNK = 65536;

    //------------------ no ftell/fseek size hint: MSVC's ftell does not count a byte pushed back by ungetc
    std::string content;
    char buf[CHUNK];
    size_t r;
    while ((r = std::fread(buf, 1, sizeof(buf), fp)) > 0)
        content.append(buf, r);
    if (content.empty())
        return LValue(L->intern_string("", 0));
    return LValue(L->intern_string(content.data(), content.size()));
}

//------------------ read format matching (Lua 5.3+ convention)
enum class ReadFmt { Line, LineKeepEol, All, Number, Bytes, Invalid };

static ReadFmt match_read_fmt(const char *fmt, int64_t *count) {
    const char *p = fmt;
    if (*p == '*')
        p++;
    switch (*p) {
    case 'a':
        return ReadFmt::All;
    case 'l':
        return ReadFmt::Line;
    case 'L':
        return ReadFmt::LineKeepEol;
    case 'n':
        return ReadFmt::Number;
    case '#': {
        char *end = nullptr;
        long long v = std::strtoll(p + 1, &end, 10);
        if (end == p + 1 || *end != '\0')
            return ReadFmt::Invalid;
        if (count)
            *count = v;
        return ReadFmt::Bytes;
    }
    default: {
        char *end = nullptr;
        long long v = std::strtoll(p, &end, 10);
        if (end != p && *end == '\0') {
            if (count)
                *count = v;
            return ReadFmt::Bytes;
        }
        return ReadFmt::Invalid;
    }
    }
}

//------------------ the iterator holds its state and formats in traced gc cells, so `local it = f:lines(); it()` reads a line

static bool take_stream_error(LState *L, FileUd *f, MultiValue &out);
static MultiValue file_read_args(
    LState *L, FileUd *f, const LValue *a, size_t c, bool lenient, const char *fname, bool raise_on_error);

static LValue make_lines_iter(LState *L, const LValue &file_val, const LValue *fmts, size_t nfmts, bool close_at_eof) {
    FileUd *f = as_file(L, file_val, "lines");
    LValue ud_val = newuserdata(L, sizeof(LinesIterUd));
    LUserdata *u = static_cast<LUserdata *>(ud_val.as_pointer());
    LinesIterUd *ud = static_cast<LinesIterUd *>(u->data());
    ud->f = f;

    RootRange rr(L, &ud_val, 1);
    std::vector<LUpValue> cells;
    cells.push_back(std::make_shared<LValue>(ud_val));
    for (size_t i = 0; i < nfmts; ++i)
        cells.push_back(std::make_shared<LValue>(fmts[i]));

    auto iter = [cells, close_at_eof](LState *L, const LValue *, size_t) -> MultiValue {
        LUserdata *u = static_cast<LUserdata *>(cells[0]->as_pointer());
        LinesIterUd *ud = static_cast<LinesIterUd *>(u->data());
        if (!ud->f->fp)
            throw_runtime_error("file is already closed");
        std::vector<LValue> read_args(cells.size());
        for (size_t i = 0; i < cells.size(); ++i)
            read_args[i] = *cells[i];
        MultiValue result = file_read_args(L, ud->f, read_args.data(), read_args.size(), false, "lines", true);
        //------------------ stock's io.lines(filename) iterator closes the file itself once a read fails
        if (close_at_eof && result.count > 0 && result[0].type == Nil && ud->f->fp) {
            outbuf_drop_fp(ud->f->fp);
            std::fclose(ud->f->fp);
            ud->f->fp = nullptr;
        }
        return result;
    };

    return L->create_closure(iter, nullptr, cells);
}

//------------------ read_number: keeps the integer subtype for a bare numeral; a fraction or exponent makes it a float
static LValue read_number(LState *L, FILE *fp) {
    constexpr size_t kMaxLenNum = 200;
    std::string tok;
    int ch;
    bool overflow = false;
    auto nextc = [&]() {
        if (tok.size() >= kMaxLenNum) {
            overflow = true;
            return false;
        }
        tok.push_back(static_cast<char>(ch));
        ch = std::fgetc(fp);
        return true;
    };
    do {
        ch = std::fgetc(fp);
    } while (ch != EOF && std::isspace(static_cast<unsigned char>(ch)) != 0);
    if (ch == EOF)
        return LValue();
    bool hex = false;
    size_t count = 0;
    if (ch == '+' || ch == '-')
        nextc();
    if (ch == '0' && nextc()) {
        if (ch == 'x' || ch == 'X') {
            hex = true;
            nextc();
        } else {
            count = 1;
        }
    }
    auto read_digits = [&](bool is_hex) {
        size_t n = 0;
        auto digit = [is_hex](int c) {
            if (c == EOF)
                return false;
            return is_hex ? std::isxdigit(static_cast<unsigned char>(c)) != 0
                          : std::isdigit(static_cast<unsigned char>(c)) != 0;
        };
        while (digit(ch) && nextc())
            ++n;
        return n;
    };
    count += read_digits(hex);
    bool is_float = false;
    if (ch == '.') {
        is_float = true;
        nextc();
        count += read_digits(hex);
    }
    int echar = hex ? 'p' : 'e';
    if (count > 0 && (ch == echar || ch == std::toupper(echar))) {
        nextc();
        if (ch == '+' || ch == '-')
            nextc();
        read_digits(false);
        is_float = true;
    }
    if (ch != EOF)
        std::ungetc(ch, fp);
    bool only_sign = tok.size() == 1 && (tok[0] == '+' || tok[0] == '-');
    if (overflow || tok.empty() || only_sign)
        return LValue();
    LValue sv = LValue(L->intern_string(tok.c_str(), tok.size()));
    if (!is_float) {
        int64_t iv;
        if (to_integer_coerced(sv, iv))
            return integer(iv);
    }
    double d;
    if (sv.to_number(d))
        return number(d);
    return LValue();
}

//------------------ take_stream_error: stock's luaL_fileresult failure shape (nil, message, errno), false when ferror is clear
static bool take_stream_error(LState *L, FileUd *f, MultiValue &out) {
    if (!std::ferror(f->fp))
        return false;
    int en = errno;
    std::clearerr(f->fp);
    out = MultiValue({ LValue(), LValue(L->intern_string(std::strerror(en))), integer(en) });
    return true;
}

static MultiValue file_read_args(
    LState *L, FileUd *f, const LValue *a, size_t c, bool lenient, const char *fname, bool raise_on_error) {
    outbuf_flush_fp(f->fp);
    if (c == 1) {
        MultiValue result(read_line(L, f->fp));
        MultiValue failed;
        if (take_stream_error(L, f, failed)) {
            if (raise_on_error)
                throw_runtime_error(failed[1].as_string());
            return failed;
        }
        return result;
    }

    std::vector<LValue> results;
    results.reserve(c - 1);
    for (size_t i = 1; i < c; ++i) {
        if (a[i].type == Int64 || a[i].type == Double) {
            int64_t n = check_integer(L, a[i]);
            if (n <= 0) {
                int probe = std::fgetc(f->fp);
                if (probe != EOF)
                    std::ungetc(probe, f->fp);
                results.push_back(probe != EOF ? LValue(L->intern_string("", 0)) : LValue());
                continue;
            }
            size_t sz = static_cast<size_t>(n);
            char *dst = new char[sz + 1];
            size_t r = std::fread(dst, 1, sz, f->fp);
            dst[r] = '\0';
            if (r == 0)
                results.push_back(LValue());
            else
                results.push_back(make_string_pooled(L, dst, r));
            delete[] dst;
        } else if (a[i].type == String) {
            int64_t count = 0;
            ReadFmt rf = match_read_fmt(a[i].as_string(), &count);
            switch (rf) {
            case ReadFmt::All:
                results.push_back(read_all(L, f->fp));
                break;
            case ReadFmt::Line:
                results.push_back(read_line(L, f->fp));
                break;
            case ReadFmt::LineKeepEol:
                results.push_back(read_line(L, f->fp, true));
                break;
            case ReadFmt::Number:
                results.push_back(read_number(L, f->fp));
                break;
            case ReadFmt::Bytes: {
                int64_t n = std::max<int64_t>(count, 0);
                if (n == 0) {
                    int probe = std::fgetc(f->fp);
                    if (probe != EOF)
                        std::ungetc(probe, f->fp);
                    results.push_back(probe != EOF ? LValue(L->intern_string("", 0)) : LValue());
                    break;
                }
                size_t sz = static_cast<size_t>(n);
                char *dst = new char[sz + 1];
                size_t r = std::fread(dst, 1, sz, f->fp);
                dst[r] = '\0';
                if (r == 0)
                    results.push_back(LValue());
                else
                    results.push_back(make_string_pooled(L, dst, r));
                delete[] dst;
                break;
            }
            case ReadFmt::Invalid:
            default:
                if (!lenient)
                    throw_runtime_error(bad_arg_msg(L, (int)(i + 1), fname, "invalid format"));
                break;
            }
        }
        if (!results.empty() && results.back().type == Nil)
            break;
    }
    MultiValue failed;
    if (take_stream_error(L, f, failed)) {
        if (raise_on_error)
            throw_runtime_error(failed[1].as_string());
        return failed;
    }
    return MultiValue(results.data(), results.size(), L);
}

static void file_write_args(LState *L, FileUd *f, const LValue *a, size_t c, bool lenient = false) {
    for (size_t i = 1; i < c; ++i) {
        if (a[i].type == String) {
            outbuf_write(f->fp, a[i].as_string(), a[i].string_len());
        } else if (a[i].type == Double || a[i].type == Int64) {
            char buf[64];
            int n;
            if (a[i].type == Int64)
                n = std::snprintf(buf, sizeof(buf), "%" PRId64, a[i].as_integer());
            else
                n = std::snprintf(buf, sizeof(buf), "%.14g", a[i].as_number());
            if (n > 0)
                outbuf_write(f->fp, buf, static_cast<size_t>(n));
        } else if (!lenient) {
            char buf[128];
            throw_runtime_error(bad_arg_msg(L, (int)(i + 1), "io.write", "string expected"));
        }
    }
}

//------------------ fail_nil: returns nil, errmsg, errno per Lua file-library convention
static MultiValue fail_nil(LState *L, const char *filename) {
    int en = errno;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: %s", filename, std::strerror(en));
    return MultiValue({ LValue(), LValue(L->intern_string(buf)), clx::integer(en) });
}

//------------------ open_or_throw: io.input/io.output/io.lines raise when the file cannot be opened
static FILE *open_or_throw(const char *filename, const char *mode) {
    FILE *fp = std::fopen(filename, mode);
    if (!fp) {
        int en = errno;
        std::string msg = std::string("cannot open file '") + filename + "' (" + std::strerror(en) + ")";
        throw_runtime_error(msg);
    }
    return fp;
}

//------------------ file_tostring: stock's __tostring for file handles
static MultiValue file_tostring(LState *L, const LValue *a, size_t c) {
    if (c < 1 || a[0].type != UserData)
        return MultiValue();
    LUserdata *ud = static_cast<LUserdata *>(a[0].as_pointer());
    if (ud->size < sizeof(FileUd))
        return MultiValue();
    FileUd *f = static_cast<FileUd *>(ud->data());
    if (!f->fp)
        return MultiValue(LValue(L->intern_string("file (closed)")));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "file (%p)", static_cast<void *>(f->fp));
    return MultiValue(LValue(L->intern_string(buf)));
}

static LValue make_file(LState *L, FILE *fp, bool close_on_gc, bool is_pipe = false) {
    LUserdata *ud = static_cast<LUserdata *>(newuserdata(L, sizeof(FileUd)).as_pointer());
    FileUd *f = static_cast<FileUd *>(ud->data());
    f->fp = fp;
    f->close_on_gc = close_on_gc;
    f->is_pipe = is_pipe;
    f->is_std = false;

    LTable *mt = static_cast<LTable *>(L->create_table().as_pointer());
    LValue file_mt(Table, mt);
    L->root_value(LValue(UserData, ud));
    L->root_value(file_mt);

    auto meth_close = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        if (c < 1)
            throw_runtime_error(bad_arg_msg(L, 1, "close", "FILE* expected, got no value"));
        FileUd *f = as_file(L, a[0]);
        if (!f->fp)
            return MultiValue(clx::boolean(true));
        outbuf_drop_fp(f->fp);
        int r;
        if (f->is_pipe)
            r = pclose(f->fp);
        else
            r = fclose(f->fp);
        f->fp = nullptr;
        if (f->is_pipe)
            return exec_result(L, r);
        if (r != 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "cannot close file: %s", std::strerror(errno));
            throw_runtime_error(buf);
        }
        return MultiValue(clx::boolean(true));
    };

    auto meth_flush = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        FileUd *f = as_file(L, a[0]);
        outbuf_flush_fp(f->fp);
        if (fflush(f->fp) != 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "cannot flush file: %s", std::strerror(errno));
            throw_runtime_error(buf);
        }
        return MultiValue(clx::boolean(true));
    };

    auto meth_lines = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        if (c > 251)
            throw_runtime_error(bad_arg_msg(L, 252, "lines", "too many arguments"));
        return make_lines_iter(L, a[0], c > 1 ? a + 1 : nullptr, c > 1 ? c - 1 : 0, false);
    };

    auto meth_read = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        return file_read_args(L, as_file(L, a[0]), a, c, false, "read", false);
    };

    auto meth_seek = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        FileUd *f = as_file(L, a[0]);
        outbuf_flush_fp(f->fp);
        const char *whence = "cur";
        if (c >= 2 && a[1].type != Nil)
            whence = check_string(L, a[1]);
        int origin = SEEK_CUR;
        if (std::strcmp(whence, "set") == 0)
            origin = SEEK_SET;
        else if (std::strcmp(whence, "cur") == 0)
            origin = SEEK_CUR;
        else if (std::strcmp(whence, "end") == 0)
            origin = SEEK_END;
        int64_t offset = 0;
        if (c >= 3 && a[2].type != Nil)
            offset = check_integer(L, a[2]);
        if (std::fseek(f->fp, offset, origin) != 0) {
            int en = errno;
            return MultiValue({ LValue(), LValue(L->intern_string(std::strerror(en))), clx::integer(en) });
        }
        long pos = std::ftell(f->fp);
        return MultiValue(integer(static_cast<int64_t>(pos)));
    };

    auto meth_setvbuf = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        FileUd *f = as_file(L, a[0]);
        if (c < 2)
            throw_runtime_error(bad_arg_msg(L, 2, "io.setvbuf", "string expected, got no value"));
        const char *mode = check_string(L, a[1]);
        int smode;
        if (std::strcmp(mode, "full") == 0)
            smode = _IOFBF;
        else if (std::strcmp(mode, "line") == 0)
            smode = _IOLBF;
        else if (std::strcmp(mode, "no") == 0)
            smode = _IONBF;
        else {
            char buf[128];
            throw_runtime_error(bad_arg_msg(L, 2, "io.setvbuf", std::string("invalid option '") + mode + "'"));
        }
        size_t size = BUFSIZ;
        if (c >= 3 && a[2].type != Nil)
            size = static_cast<size_t>(check_integer(L, a[2]));
        outbuf_flush_fp(f->fp);
        if (setvbuf(f->fp, nullptr, smode, size) != 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "cannot setvbuf: %s", std::strerror(errno));
            throw_runtime_error(buf);
        }
        outbuf_set_mode(f->fp, (smode == _IONBF) ? 2 : (smode == _IOLBF) ? 1 : 0);
        return MultiValue(boolean(true));
    };

    auto meth_write = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        FileUd *f = as_file(L, a[0]);
        file_write_args(L, f, a, c);
        //------------------ writes are buffered above, so the handle is flushed before an error return can be reported
        outbuf_flush_fp(f->fp);
        MultiValue failed;
        if (take_stream_error(L, f, failed))
            return failed;
        return MultiValue(a[0]);
    };

    auto meth_gc = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        if (c < 1 || a[0].type != UserData)
            return MultiValue();
        LUserdata *ud = static_cast<LUserdata *>(a[0].as_pointer());
        if (ud->size < sizeof(FileUd))
            return MultiValue();
        FileUd *f = static_cast<FileUd *>(ud->data());
        if (f->fp && f->close_on_gc) {
            outbuf_drop_fp(f->fp);
            if (f->is_pipe)
                pclose(f->fp);
            else
                fclose(f->fp);
            f->fp = nullptr;
        }
        return MultiValue();
    };

    mt->bind_all(L,
        { { "close", meth_close }, { "flush", meth_flush }, { "lines", meth_lines }, { "read", meth_read },
            { "seek", meth_seek }, { "setvbuf", meth_setvbuf }, { "write", meth_write }, { "__gc", meth_gc },
            { "__close", meth_gc }, { "__tostring", file_tostring } });
    mt->settable(LValue(L->intern_string("__name")), LValue::istr("FILE*", 5));
    mt->settable(LValue(L->intern_string("__index")), LValue(Table, mt));
    ud->metatable = mt;
    meta_ud_list_add(L, ud);
    L->unroot_value(LValue(UserData, ud));
    L->unroot_value(file_mt);
    return LValue(UserData, ud);
}

//------------------ the standard handles are process singletons: io.stdin must be the very userdata the default slot holds
static LValue std_file_slots[3];

static LValue get_std_file(LState *L, FILE *fp) {
    LValue *slot = (fp == stdin) ? &std_file_slots[0] : (fp == stdout) ? &std_file_slots[1] : &std_file_slots[2];
    if (slot->type == UserData)
        return *slot;
    LUserdata *ud = static_cast<LUserdata *>(newuserdata(L, sizeof(FileUd)).as_pointer());
    FileUd *f = static_cast<FileUd *>(ud->data());
    f->fp = fp;
    f->close_on_gc = false;
    f->is_pipe = false;
    f->is_std = true;

    LTable *mt = static_cast<LTable *>(L->create_table().as_pointer());
    LValue file_mt(Table, mt);
    L->root_value(LValue(UserData, ud));
    L->root_value(file_mt);

    auto meth_close = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        if (c < 1)
            throw_runtime_error(bad_arg_msg(L, 1, "close", "FILE* expected, got no value"));
        (void)a;
        return cannot_close_std(L);
    };

    //------------------ stock binds __close to f_gc, a no-op on a closed handle, so closing twice must not raise
    auto meth_close_guard = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        if (c < 1 || a[0].type != UserData)
            return MultiValue();
        LUserdata *u = static_cast<LUserdata *>(a[0].as_pointer());
        if (u->size < sizeof(FileUd))
            return MultiValue();
        FileUd *f = static_cast<FileUd *>(u->data());
        if (!f->fp && !f->is_pipe)
            return MultiValue();
        return cannot_close_std(L);
    };

    auto meth_flush = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        LUserdata *u = static_cast<LUserdata *>(a[0].as_pointer());
        FileUd *f = static_cast<FileUd *>(u->data());
        outbuf_flush_fp(f->fp);
        fflush(f->fp);
        return MultiValue(boolean(true));
    };

    auto meth_lines = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        if (c > 251)
            throw_runtime_error(bad_arg_msg(L, 252, "lines", "too many arguments"));
        return make_lines_iter(L, a[0], c > 1 ? a + 1 : nullptr, c > 1 ? c - 1 : 0, false);
    };

    auto meth_read = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        FileUd *f = static_cast<FileUd *>(static_cast<LUserdata *>(a[0].as_pointer())->data());
        return file_read_args(L, f, a, c, false, "read", false);
    };

    auto meth_seek = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        FileUd *f = static_cast<FileUd *>(static_cast<LUserdata *>(a[0].as_pointer())->data());
        const char *whence = "cur";
        if (c >= 2 && a[1].type != Nil)
            whence = check_string(L, a[1]);
        int origin = SEEK_CUR;
        if (std::strcmp(whence, "set") == 0)
            origin = SEEK_SET;
        else if (std::strcmp(whence, "cur") == 0)
            origin = SEEK_CUR;
        else if (std::strcmp(whence, "end") == 0)
            origin = SEEK_END;
        int64_t offset = 0;
        if (c >= 3 && a[2].type != Nil)
            offset = check_integer(L, a[2]);
        outbuf_flush_fp(f->fp);
        if (std::fseek(f->fp, offset, origin) != 0) {
            int en = errno;
            return MultiValue({ LValue(), LValue(L->intern_string(std::strerror(en))), clx::integer(en) });
        }
        return MultiValue(integer(static_cast<int64_t>(std::ftell(f->fp))));
    };

    auto meth_write = [](LState *L, const LValue *a, size_t c) -> MultiValue {
        FileUd *f = static_cast<FileUd *>(static_cast<LUserdata *>(a[0].as_pointer())->data());
        file_write_args(L, f, a, c, true);
        return MultiValue(a[0]);
    };

    mt->bind_all(L,
        { { "close", meth_close }, { "flush", meth_flush }, { "lines", meth_lines }, { "read", meth_read },
            { "seek", meth_seek }, { "write", meth_write }, { "__close", meth_close_guard },
            { "__tostring", file_tostring } });
    mt->settable(LValue(L->intern_string("__name")), LValue::istr("FILE*", 5));
    mt->settable(LValue(L->intern_string("__index")), LValue(Table, mt));
    ud->metatable = mt;
    meta_ud_list_add(L, ud);
    //------------------ the cached singleton keeps its own root: unrooting it would leave the slot pointing at collected userdata
    *slot = LValue(UserData, ud);
    return *slot;
}

static LValue default_input;
static LValue default_output;

static void set_default(LState *L, LValue &slot, const LValue &v) {
    if (L) {
        L->unroot_value(slot);
        L->root_value(v);
    }
    slot = v;
}

//------------------ open_mode_valid: stock's l_checkmode — [rwa], an optional '+', then only 'b'
static bool open_mode_valid(const char *mode) {
    if (*mode != 'r' && *mode != 'w' && *mode != 'a')
        return false;
    ++mode;
    if (*mode == '+')
        ++mode;
    for (; *mode; ++mode) {
        if (*mode != 'b')
            return false;
    }
    return true;
}

static MultiValue io_open(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error(bad_arg_msg(L, 1, "io.open", "string expected, got no value"));
    const char *filename = check_string(L, args[0]);
    const char *mode = "r";
    if (count >= 2 && args[1].type != Nil)
        mode = check_string(L, args[1]);
    if (!open_mode_valid(mode))
        throw_runtime_error(bad_arg_msg(L, 2, "io.open", "invalid mode"));
    FILE *fp = std::fopen(filename, mode);
    if (!fp)
        return fail_nil(L, filename);
    return MultiValue(make_file(L, fp, true));
}

static MultiValue io_close(LState *L, const LValue *args, size_t count) {
    if (count == 0 || args[0].type == Nil) {
        if (default_output.type == UserData) {
            LValue a = default_output;
            FileUd *f = as_file(L, a);
            if (f->is_std)
                return cannot_close_std(L);
            if (f->fp) {
                outbuf_drop_fp(f->fp);
                fclose(f->fp);
                f->fp = nullptr;
            }
        }
        return MultiValue(boolean(true));
    }
    FileUd *f = as_file(L, args[0]);
    if (f->is_std)
        return cannot_close_std(L);
    if (f->fp) {
        outbuf_drop_fp(f->fp);
        fclose(f->fp);
        f->fp = nullptr;
    }
    return MultiValue(boolean(true));
}

static MultiValue io_flush(LState *L, const LValue *args, size_t count) {
    if (default_output.type == UserData) {
        LUserdata *u = static_cast<LUserdata *>(default_output.as_pointer());
        FileUd *f = static_cast<FileUd *>(u->data());
        outbuf_flush_fp(f->fp);
        fflush(f->fp);
    } else {
        outbuf_flush_fp(stdout);
        fflush(stdout);
    }
    return MultiValue(boolean(true));
}

static MultiValue io_input(LState *L, const LValue *args, size_t count) {
    if (count == 0 || args[0].type == Nil) {
        if (default_input.type != UserData)
            set_default(L, default_input, get_std_file(L, stdin));
        return MultiValue(default_input);
    }
    if (args[0].type == UserData) {
        set_default(L, default_input, args[0]);
    } else if (args[0].type == String) {
        const char *filename = args[0].as_string();
        FILE *fp = open_or_throw(filename, "r");
        set_default(L, default_input, make_file(L, fp, true));
    }
    return MultiValue(default_input);
}

static MultiValue io_lines(LState *L, const LValue *args, size_t count) {
    size_t nfmts = (count > 1) ? count - 1 : 0;
    if (nfmts > 250)
        throw_runtime_error(bad_arg_msg(L, 252, "lines", "too many arguments"));

    if (count > 0 && args[0].type != Nil) {
        const char *filename = check_string(L, args[0]);
        //------------------ the filename form returns the iterator, two placeholders and the handle (its to-be-closed variable)
        FILE *fp = open_or_throw(filename, "r");
        LValue file_val = make_file(L, fp, true);
        RootRange rr(L, &file_val, 1);
        LValue iter = make_lines_iter(L, file_val, nfmts ? args + 1 : nullptr, nfmts, true);
        return MultiValue({ iter, nil(), nil(), file_val });
    }

    if (default_input.type != UserData)
        set_default(L, default_input, get_std_file(L, stdin));
    return MultiValue(make_lines_iter(L, default_input, nfmts ? args + 1 : nullptr, nfmts, false));
}

static MultiValue io_output(LState *L, const LValue *args, size_t count) {
    if (count == 0 || args[0].type == Nil) {
        if (default_output.type != UserData)
            set_default(L, default_output, get_std_file(L, stdout));
        return MultiValue(default_output);
    }
    if (args[0].type == UserData) {
        set_default(L, default_output, args[0]);
    } else if (args[0].type == String) {
        const char *filename = args[0].as_string();
        FILE *fp = open_or_throw(filename, "w");
        set_default(L, default_output, make_file(L, fp, true));
    }
    return MultiValue(default_output);
}

//------------------ default_file: the default input/output slot as a live FileUd, named even once closed
static FileUd *default_file(LState *L, const LValue &slot, const char *which) {
    if (slot.type == UserData) {
        LUserdata *ud = static_cast<LUserdata *>(slot.as_pointer());
        if (ud->size >= sizeof(FileUd)) {
            FileUd *f = static_cast<FileUd *>(ud->data());
            if (f->fp || f->is_pipe)
                return f;
        }
    }
    throw_runtime_error(std::string("default ") + which + " file is closed");
}

static MultiValue io_read(LState *L, const LValue *args, size_t count) {
    if (default_input.type != UserData)
        set_default(L, default_input, get_std_file(L, stdin));

    LValue file = default_input;
    FileUd *f = default_file(L, file, "input");
    outbuf_flush_fp(f->fp);

    if (count == 0) {
        MultiValue result(read_line(L, f->fp));
        MultiValue failed;
        if (take_stream_error(L, f, failed))
            return failed;
        return result;
    }

    std::vector<LValue> results;
    results.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (args[i].type == Int64 || args[i].type == Double) {
            int64_t n = check_integer(L, args[i]);
            if (n <= 0) {
                int probe = std::fgetc(f->fp);
                if (probe != EOF)
                    std::ungetc(probe, f->fp);
                results.push_back(probe != EOF ? LValue(L->intern_string("", 0)) : LValue());
                if (results.back().type == Nil)
                    break;
                continue;
            }
            size_t sz = static_cast<size_t>(n);
            uint32_t len32 = static_cast<uint32_t>(sz);
            char *mem = new char[16 + sz + 1]();
            clx_memcpy(mem + 8, &len32, 4);
            size_t r = std::fread(mem + 16, 1, sz, f->fp);
            mem[16 + r] = '\0';
            uint64_t h = r <= 8 ? swar_hash_8(mem + 16, r) : wyhash_str(mem + 16, r);
            clx_memcpy(mem, &h, 8);
            if (r == 0)
                results.push_back(LValue());
            else
                results.push_back(LValue(L->string_pool.intern_preallocated(mem + 16, h, r)));
        } else if (args[i].type == String) {
            int64_t count_opt = 0;
            ReadFmt rf = match_read_fmt(args[i].as_string(), &count_opt);
            switch (rf) {
            case ReadFmt::All:
                results.push_back(read_all(L, f->fp));
                break;
            case ReadFmt::Line:
                results.push_back(read_line(L, f->fp));
                break;
            case ReadFmt::LineKeepEol:
                results.push_back(read_line(L, f->fp, true));
                break;
            case ReadFmt::Number: {
                double d;
                if (std::fscanf(f->fp, "%lf", &d) == 1)
                    results.push_back(number(d));
                else
                    results.push_back(LValue());
                break;
            }
            case ReadFmt::Bytes: {
                int64_t n = std::max<int64_t>(count_opt, 0);
                if (n == 0) {
                    results.push_back(LValue(L->intern_string("", 0)));
                    break;
                }
                size_t sz = static_cast<size_t>(n);
                uint32_t len32 = static_cast<uint32_t>(sz);
                char *mem = new char[16 + sz + 1]();
                clx_memcpy(mem + 8, &len32, 4);
                size_t r = std::fread(mem + 16, 1, sz, f->fp);
                mem[16 + r] = '\0';
                uint64_t h = r <= 8 ? swar_hash_8(mem + 16, r) : wyhash_str(mem + 16, r);
                clx_memcpy(mem, &h, 8);
                if (r == 0)
                    results.push_back(LValue());
                else
                    results.push_back(LValue(L->string_pool.intern_preallocated(mem + 16, h, r)));
                break;
            }
            case ReadFmt::Invalid:
            default:
                throw_runtime_error(bad_arg_msg(L, (int)(i + 1), "io.read", "invalid format"));
            }
        }
        if (!results.empty() && results.back().type == Nil)
            break;
    }
    MultiValue failed;
    if (take_stream_error(L, f, failed))
        return failed;
    return MultiValue(results.data(), results.size(), L);
}

static MultiValue io_write(LState *L, const LValue *args, size_t count) {
    if (default_output.type != UserData)
        set_default(L, default_output, get_std_file(L, stdout));
    FileUd *f = default_file(L, default_output, "output");
    for (size_t i = 0; i < count; ++i) {
        if (args[i].type == String) {
            outbuf_write(f->fp, args[i].as_string(), args[i].string_len());
        } else if (args[i].type == Double || args[i].type == Int64) {
            char buf[64];
            int n;
            if (args[i].type == Int64)
                n = std::snprintf(buf, sizeof(buf), "%" PRId64, args[i].as_integer());
            else
                n = std::snprintf(buf, sizeof(buf), "%.14g", args[i].as_number());
            if (n > 0)
                outbuf_write(f->fp, buf, static_cast<size_t>(n));
        }
    }
    return MultiValue(default_output);
}

//------------------ io_popen — open a process pipe for read or write
static MultiValue io_popen(LState *L, const LValue *args, size_t count) {
    if (count == 0 || args[0].type != String)
        throw_runtime_error(bad_arg_msg(L, 1, "io.popen", "string expected"));
    const char *command = args[0].as_string();
    const char *mode = "r";
    if (count >= 2 && args[1].type != Nil)
        mode = check_string(L, args[1]);
    if (std::strcmp(mode, "r") != 0 && std::strcmp(mode, "w") != 0)
        throw_runtime_error(bad_arg_msg(L, 2, "io.popen", "invalid mode"));
    FILE *fp = popen(command, mode);
    if (!fp) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "cannot popen '%s': %s", command, std::strerror(errno));
        throw_runtime_error(buf);
    }
    return MultiValue(make_file(L, fp, true, true));
}

static MultiValue io_type(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        return MultiValue(nil());
    if (args[0].type != UserData)
        return MultiValue(nil());
    LUserdata *ud = static_cast<LUserdata *>(args[0].as_pointer());
    if (ud->size < sizeof(FileUd))
        return MultiValue(nil());
    FileUd *f = static_cast<FileUd *>(ud->data());
    if (!f->fp)
        return MultiValue(string(L, "closed file"));
    return MultiValue(string(L, "file"));
}

static MultiValue io_tmpfile(LState *L, const LValue *args, size_t count) {
    FILE *fp = std::tmpfile();
    if (!fp)
        throw_runtime_error("cannot create temporary file");
    return MultiValue(make_file(L, fp, true));
}

void luastd_io(LState *L) {
    clx_io_flush_all_ptr = outbuf_flush_all;
    clx_io_write_ptr = outbuf_write;
    LValue t = L->create_table();
    LTable *tbl = static_cast<LTable *>(t.as_pointer());
    tbl->bind_all(L,
        { { "close", io_close }, { "flush", io_flush }, { "input", io_input }, { "lines", io_lines },
            { "open", io_open }, { "output", io_output }, { "popen", io_popen }, { "read", io_read },
            { "type", io_type }, { "write", io_write }, { "tmpfile", io_tmpfile } });

    tbl->settable(LValue(L->intern_string("stdin")), get_std_file(L, stdin));
    tbl->settable(LValue(L->intern_string("stdout")), get_std_file(L, stdout));
    tbl->settable(LValue(L->intern_string("stderr")), get_std_file(L, stderr));

    set_global(L, "io", t);
}

}