#ifdef WIN32
#define NOMINMAX
#endif

#include "clx.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cinttypes>
#include <vector>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#include <cstdio>
#define popen _popen
#define pclose _pclose
#endif

namespace clx {

struct FileUd {
    FILE* fp;
    bool close_on_gc;
    bool is_pipe;
};

struct LinesIterUd {
    FileUd* f;
    char buf[4096];
};

struct LinesFileUd {
    FILE* fp;
    char buf[4096];
    bool first;
};

static FileUd* as_file(LState* L, const LValue& v) {
    if (v.type() != LType::Userdata)
        throw_runtime_error("bad argument (FILE* expected, got userdata)");
    LUserdata* ud = static_cast<LUserdata*>(v.as_pointer());
    if (ud->size < sizeof(FileUd))
        throw_runtime_error("bad argument (not a valid file handle)");
    FileUd* f = static_cast<FileUd*>(ud->data());
    if (!f->fp && !f->is_pipe)
        throw_runtime_error("attempt to use a closed file");
    return f;
}

//------------------ read_line — read one line from fp, strip \n, intern result
static LValue read_line(LState* L, FILE* fp) {
    char buf[4096];
    if (!std::fgets(buf, sizeof(buf), fp))
        return LValue();
    size_t n = std::strlen(buf);
    if (n < sizeof(buf) - 1 || buf[n - 1] == '\n') {
        if (n > 0 && buf[n - 1] == '\n') n--;
        return LValue(L->intern_string(buf, n));
    }
    StringBuilder sb;
    sb.append(L->intern_string(buf, n), static_cast<uint32_t>(n));
    while (std::fgets(buf, sizeof(buf), fp)) {
        n = std::strlen(buf);
        bool done = (n < sizeof(buf) - 1 || buf[n - 1] == '\n');
        if (done && n > 0 && buf[n - 1] == '\n') n--;
        sb.append(L->intern_string(buf, n), static_cast<uint32_t>(n));
        if (done) break;
    }
    return LValue(sb.to_string(L));
}

//------------------ read_all — read entire file into interned string
static LValue read_all(LState* L, FILE* fp) {
    static constexpr size_t SINGLE_SHOT_LIMIT = 64 * 1024 * 1024;
    static constexpr size_t CHUNK = 65536;


    size_t hint = 0;
    long start = std::ftell(fp);
    if (start >= 0 && std::fseek(fp, 0, SEEK_END) == 0) {
        long end = std::ftell(fp);
        std::fseek(fp, start, SEEK_SET);
        if (end > start) hint = static_cast<size_t>(end - start);
    }

    if (hint > 0 && hint <= SINGLE_SHOT_LIMIT) {

        uint32_t len32 = static_cast<uint32_t>(hint);
        char* mem = new char[8 + hint + 1];
        clx_memcpy(mem + 4, &len32, 4);
        char* dst = mem + 8;
        size_t r = std::fread(dst, 1, hint, fp);
        dst[r] = '\0';
        if (r > 0) {
            uint32_t h = r <= 8 ? swar_hash_8(dst, r) : wyhash_str(dst, r);
            if (const char* hit = L->string_pool.lookup(dst, r, h)) {
                delete[] mem;
                return LValue(hit);
            }
            clx_memcpy(mem, &h, 4);

            if (r != hint) { len32 = static_cast<uint32_t>(r); clx_memcpy(mem + 4, &len32, 4); }
            return LValue(L->string_pool.intern_preallocated(dst, h, r));
        }
        delete[] mem;
        return LValue(L->intern_string("", 0));
    }


    std::string content;
    if (hint > 0) content.reserve(std::min(hint, SINGLE_SHOT_LIMIT));
    char buf[CHUNK];
    size_t r;
    while ((r = std::fread(buf, 1, sizeof(buf), fp)) > 0)
        content.append(buf, r);
    if (content.empty()) return LValue(L->intern_string("", 0));
    return LValue(L->intern_string(content.data(), content.size()));
}

static LValue make_file(LState* L, FILE* fp, bool close_on_gc, bool is_pipe = false) {
    LUserdata* ud = static_cast<LUserdata*>(newuserdata(L, sizeof(FileUd)).as_pointer());
    FileUd* f = static_cast<FileUd*>(ud->data());
    f->fp = fp;
    f->close_on_gc = close_on_gc;
    f->is_pipe = is_pipe;

    LTable* mt = static_cast<LTable*>(L->create_table().as_pointer());
    LValue file_mt(LType::Table, mt);

    auto meth_close = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        FileUd* f = as_file(L, a[0]);
        if (!f->fp) return MultiValue(clx::boolean(true));
        int r;
        if (f->is_pipe) r = pclose(f->fp);
        else r = fclose(f->fp);
        f->fp = nullptr;
        if (r != 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "cannot close file: %s", std::strerror(errno));
            throw_runtime_error(buf);
        }
        return MultiValue(clx::boolean(true));
    };

    auto meth_flush = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        FileUd* f = as_file(L, a[0]);
        if (fflush(f->fp) != 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "cannot flush file: %s", std::strerror(errno));
            throw_runtime_error(buf);
        }
        return MultiValue(clx::boolean(true));
    };

    auto meth_lines = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        FileUd* f = as_file(L, a[0]);
        LUserdata* u = static_cast<LUserdata*>(newuserdata(L, sizeof(LinesIterUd)).as_pointer());
        LinesIterUd* ud = static_cast<LinesIterUd*>(u->data());
        ud->f = f;

        auto iter = [](LState* L, const LValue* a, size_t c) -> MultiValue {
            if (c < 1) return MultiValue();
            LUserdata* u = static_cast<LUserdata*>(a[0].as_pointer());
            LinesIterUd* ud = static_cast<LinesIterUd*>(u->data());
            if (!ud->f->fp) return MultiValue();
            LValue line = read_line(L, ud->f->fp);
            if (line.type() == LType::Nil) return MultiValue();
            return MultiValue(line);
        };

        return MultiValue({
            L->create_closure(CFunctionType(iter)),
            LValue(u),
            clx::nil()
        });
    };

    auto meth_read = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        FileUd* f = as_file(L, a[0]);

        if (c == 1) {
            LValue line = read_line(L, f->fp);
            return MultiValue(line);
        }
        std::vector<LValue> results;
        results.reserve(c - 1);
        for (size_t i = 1; i < c; ++i) {
            if (a[i].type() == LType::Integer || a[i].type() == LType::Number) {
                int64_t n = check_integer(L, a[i]);
                if (n <= 0) {
                    results.push_back(LValue(L->intern_string("", 0)));
                    continue;
                }
                size_t sz = static_cast<size_t>(n);
                uint32_t len32 = static_cast<uint32_t>(sz);
                char* mem = new char[8 + sz + 1];
                clx_memcpy(mem + 4, &len32, 4);
                size_t r = std::fread(mem + 8, 1, sz, f->fp);
                mem[8 + r] = '\0';
                uint32_t h = r <= 8 ? swar_hash_8(mem + 8, r) : wyhash_str(mem + 8, r);
                clx_memcpy(mem, &h, 4);
                results.push_back(LValue(L->string_pool.intern_preallocated(mem + 8, h, r)));
            } else if (a[i].type() == LType::String) {
                const char* fmt = a[i].as_string();
                if (std::strcmp(fmt, "*a") == 0 || std::strcmp(fmt, "*all") == 0) {
                    results.push_back(read_all(L, f->fp));
                } else if (std::strcmp(fmt, "*l") == 0 || std::strcmp(fmt, "*line") == 0) {
                    LValue line = read_line(L, f->fp);
                    results.push_back(line);
                } else if (std::strcmp(fmt, "*n") == 0 || std::strcmp(fmt, "*number") == 0) {
                    double d;
                    if (std::fscanf(f->fp, "%lf", &d) == 1)
                        results.push_back(number(d));
                    else
                        results.push_back(LValue());
                } else {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "bad argument #%zu to 'read' (invalid format)", i + 1);
                    throw_runtime_error(buf);
                }
            }
        }
        return MultiValue(results.data(), results.size());
    };

    auto meth_seek = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        FileUd* f = as_file(L, a[0]);
        const char* whence = "cur";
        if (c >= 2 && a[1].type() != LType::Nil)
            whence = check_string(L, a[1]);
        int origin = SEEK_CUR;
        if (std::strcmp(whence, "set") == 0) origin = SEEK_SET;
        else if (std::strcmp(whence, "cur") == 0) origin = SEEK_CUR;
        else if (std::strcmp(whence, "end") == 0) origin = SEEK_END;
        int64_t offset = 0;
        if (c >= 3 && a[2].type() != LType::Nil)
            offset = check_integer(L, a[2]);
        if (std::fseek(f->fp, offset, origin) != 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "cannot seek file: %s", std::strerror(errno));
            throw_runtime_error(buf);
        }
        long pos = std::ftell(f->fp);
        return MultiValue(integer(static_cast<int64_t>(pos)));
    };

    auto meth_setvbuf = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        FileUd* f = as_file(L, a[0]);
        if (c < 2)
            throw_runtime_error("bad argument #2 to 'setvbuf' (string expected, got no value)");
        const char* mode = check_string(L, a[1]);
        int smode;
        if (std::strcmp(mode, "full") == 0) smode = _IOFBF;
        else if (std::strcmp(mode, "line") == 0) smode = _IOLBF;
        else if (std::strcmp(mode, "no") == 0) smode = _IONBF;
        else {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "bad argument #2 to 'setvbuf' (invalid mode '%s')", mode);
            throw_runtime_error(buf);
        }
        size_t size = BUFSIZ;
        if (c >= 3 && a[2].type() != LType::Nil)
            size = static_cast<size_t>(check_integer(L, a[2]));
        if (setvbuf(f->fp, nullptr, smode, size) != 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "cannot setvbuf: %s", std::strerror(errno));
            throw_runtime_error(buf);
        }
        return MultiValue(boolean(true));
    };


    auto meth_write = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        FileUd* f = as_file(L, a[0]);
        for (size_t i = 1; i < c; ++i) {
            if (a[i].type() == LType::String) {
                std::fwrite(a[i].as_string(), 1, a[i].string_len(), f->fp);
            } else if (a[i].type() == LType::Number || a[i].type() == LType::Integer) {

                char buf[64];
                int n;
                if (a[i].type() == LType::Integer)
                    n = std::snprintf(buf, sizeof(buf), "%" PRId64, a[i].as_integer());
                else
                    n = std::snprintf(buf, sizeof(buf), "%.14g", a[i].as_number());
                if (n > 0) std::fwrite(buf, 1, static_cast<size_t>(n), f->fp);
            } else {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "bad argument #%zu to 'write' (string/number expected)", i + 1);
                throw_runtime_error(buf);
            }
        }
        return MultiValue(a[0]);
    };

    auto meth_gc = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        if (c < 1 || a[0].type() != LType::Userdata) return MultiValue();
        LUserdata* ud = static_cast<LUserdata*>(a[0].as_pointer());
        if (ud->size < sizeof(FileUd)) return MultiValue();
        FileUd* f = static_cast<FileUd*>(ud->data());
        if (f->fp && f->close_on_gc) {
            if (f->is_pipe) pclose(f->fp);
            else fclose(f->fp);
            f->fp = nullptr;
        }
        return MultiValue();
    };

    mt->bind_all(L, {
        {"close",    meth_close},
        {"flush",    meth_flush},
        {"lines",    meth_lines},
        {"read",     meth_read},
        {"seek",     meth_seek},
        {"setvbuf",  meth_setvbuf},
        {"write",    meth_write},
        {"__gc",     meth_gc}
    });
    mt->settable(LValue(L->intern_string("__index")), LValue(LType::Table, mt));
    ud->metatable = mt;
    return LValue(LType::Userdata, ud);
}

static LValue get_std_file(LState* L, FILE* fp) {
    LUserdata* ud = static_cast<LUserdata*>(newuserdata(L, sizeof(FileUd)).as_pointer());
    FileUd* f = static_cast<FileUd*>(ud->data());
    f->fp = fp;
    f->close_on_gc = false;
    f->is_pipe = false;

    LTable* mt = static_cast<LTable*>(L->create_table().as_pointer());
    LValue file_mt(LType::Table, mt);

    auto meth_close = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        return MultiValue(boolean(true));
    };

    auto meth_flush = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        LUserdata* u = static_cast<LUserdata*>(a[0].as_pointer());
        FileUd* f = static_cast<FileUd*>(u->data());
        fflush(f->fp);
        return MultiValue(boolean(true));
    };

    auto meth_lines = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        LUserdata* u0 = static_cast<LUserdata*>(a[0].as_pointer());
        FileUd* f = static_cast<FileUd*>(u0->data());
        LUserdata* u = static_cast<LUserdata*>(newuserdata(L, sizeof(LinesIterUd)).as_pointer());
        LinesIterUd* ud = static_cast<LinesIterUd*>(u->data());
        ud->f = f;

        auto iter = [](LState* L, const LValue* a, size_t c) -> MultiValue {
            if (c < 1) return MultiValue();
            LUserdata* u = static_cast<LUserdata*>(a[0].as_pointer());
            LinesIterUd* ud = static_cast<LinesIterUd*>(u->data());
            if (!ud->f->fp) return MultiValue();
            LValue line = read_line(L, ud->f->fp);
            if (line.type() == LType::Nil) return MultiValue();
            return MultiValue(line);
        };

        return MultiValue({
            L->create_closure(CFunctionType(iter)),
            LValue(u),
            nil()
        });
    };

    auto meth_read = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        LUserdata* u0 = static_cast<LUserdata*>(a[0].as_pointer());
        FileUd* f = static_cast<FileUd*>(u0->data());
        if (c == 1) {
            LValue line = read_line(L, f->fp);
            return MultiValue(line);
        }
        std::vector<LValue> results;
        results.reserve(c - 1);
        for (size_t i = 1; i < c; ++i) {
            if (a[i].type() == LType::Integer || a[i].type() == LType::Number) {
                int64_t n = check_integer(L, a[i]);
                if (n <= 0) { results.push_back(LValue(L->intern_string("", 0))); continue; }
                size_t sz = static_cast<size_t>(n);
                uint32_t len32 = static_cast<uint32_t>(sz);
                char* mem = new char[8 + sz + 1];
                clx_memcpy(mem + 4, &len32, 4);
                size_t r = std::fread(mem + 8, 1, sz, f->fp);
                mem[8 + r] = '\0';
                uint32_t h = r <= 8 ? swar_hash_8(mem + 8, r) : wyhash_str(mem + 8, r);
                clx_memcpy(mem, &h, 4);
                results.push_back(LValue(L->string_pool.intern_preallocated(mem + 8, h, r)));
            } else if (a[i].type() == LType::String) {
                const char* fmt = a[i].as_string();
                if (std::strcmp(fmt, "*a") == 0 || std::strcmp(fmt, "*all") == 0) {
                    results.push_back(read_all(L, f->fp));
                } else if (std::strcmp(fmt, "*l") == 0 || std::strcmp(fmt, "*line") == 0) {
                    results.push_back(read_line(L, f->fp));
                } else if (std::strcmp(fmt, "*n") == 0 || std::strcmp(fmt, "*number") == 0) {
                    double d;
                    if (std::fscanf(f->fp, "%lf", &d) == 1)
                        results.push_back(number(d));
                    else
                        results.push_back(LValue());
                }
            }
        }
        return MultiValue(results.data(), results.size());
    };

    auto meth_seek = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        LUserdata* u0 = static_cast<LUserdata*>(a[0].as_pointer());
        FileUd* f = static_cast<FileUd*>(u0->data());
        const char* whence = "cur";
        if (c >= 2 && a[1].type() != LType::Nil) whence = check_string(L, a[1]);
        int origin = SEEK_CUR;
        if (std::strcmp(whence, "set") == 0) origin = SEEK_SET;
        else if (std::strcmp(whence, "cur") == 0) origin = SEEK_CUR;
        else if (std::strcmp(whence, "end") == 0) origin = SEEK_END;
        int64_t offset = 0;
        if (c >= 3 && a[2].type() != LType::Nil) offset = check_integer(L, a[2]);
        std::fseek(f->fp, offset, origin);
        return MultiValue(integer(static_cast<int64_t>(std::ftell(f->fp))));
    };

    auto meth_write = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        LUserdata* u0 = static_cast<LUserdata*>(a[0].as_pointer());
        FileUd* f = static_cast<FileUd*>(u0->data());
        for (size_t i = 1; i < c; ++i) {
            if (a[i].type() == LType::String) {
                std::fwrite(a[i].as_string(), 1, a[i].string_len(), f->fp);
            } else if (a[i].type() == LType::Number || a[i].type() == LType::Integer) {
                char buf[64];
                int n;
                if (a[i].type() == LType::Integer)
                    n = std::snprintf(buf, sizeof(buf), "%" PRId64, a[i].as_integer());
                else
                    n = std::snprintf(buf, sizeof(buf), "%.14g", a[i].as_number());
                if (n > 0) std::fwrite(buf, 1, static_cast<size_t>(n), f->fp);
            }
        }
        return MultiValue(a[0]);
    };

    mt->bind_all(L, {
        {"close",  meth_close},
        {"flush",  meth_flush},
        {"lines",  meth_lines},
        {"read",   meth_read},
        {"seek",   meth_seek},
        {"write",  meth_write}
    });
    mt->settable(LValue(L->intern_string("__index")), LValue(LType::Table, mt));
    ud->metatable = mt;
    return LValue(LType::Userdata, ud);
}

static LValue default_input;
static LValue default_output;

static MultiValue io_open(LState* L, const LValue* args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'open' (string expected, got no value)");
    const char* filename = check_string(L, args[0]);
    const char* mode = "r";
    if (count >= 2 && args[1].type() != LType::Nil)
        mode = check_string(L, args[1]);
    FILE* fp = std::fopen(filename, mode);
    if (!fp) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "cannot open file '%s': %s", filename, std::strerror(errno));
        throw_runtime_error(buf);
    }
    return MultiValue(make_file(L, fp, true));
}

static MultiValue io_close(LState* L, const LValue* args, size_t count) {
    if (count == 0 || args[0].type() == LType::Nil) {
        if (default_output.type() == LType::Userdata) {
            LValue a = default_output;
            FileUd* f = as_file(L, a);
            if (f->fp) { fclose(f->fp); f->fp = nullptr; }
        }
        return MultiValue(boolean(true));
    }
    FileUd* f = as_file(L, args[0]);
    if (f->fp) { fclose(f->fp); f->fp = nullptr; }
    return MultiValue(boolean(true));
}

static MultiValue io_flush(LState* L, const LValue* args, size_t count) {
    if (default_output.type() == LType::Userdata) {
        LUserdata* u = static_cast<LUserdata*>(default_output.as_pointer());
        FileUd* f = static_cast<FileUd*>(u->data());
        fflush(f->fp);
    } else {
        fflush(stdout);
    }
    return MultiValue(boolean(true));
}

static MultiValue io_input(LState* L, const LValue* args, size_t count) {
    if (count == 0 || args[0].type() == LType::Nil) {
        if (default_input.type() != LType::Userdata)
            default_input = get_std_file(L, stdin);
        return MultiValue(default_input);
    }
    if (args[0].type() == LType::Userdata) {
        default_input = args[0];
    } else if (args[0].type() == LType::String) {
        const char* filename = args[0].as_string();
        FILE* fp = std::fopen(filename, "r");
        if (!fp) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "cannot open file '%s': %s", filename, std::strerror(errno));
            throw_runtime_error(buf);
        }
        default_input = make_file(L, fp, true);
    }
    return MultiValue(default_input);
}

static MultiValue io_lines(LState* L, const LValue* args, size_t count) {
    if (count == 0) {
        if (default_input.type() != LType::Userdata)
            default_input = get_std_file(L, stdin);
        LValue file = default_input;
        FileUd* f = as_file(L, file);

        LUserdata* u = static_cast<LUserdata*>(newuserdata(L, sizeof(LinesIterUd)).as_pointer());
        LinesIterUd* ud = static_cast<LinesIterUd*>(u->data());
        ud->f = f;

        auto iter = [](LState* L, const LValue* a, size_t c) -> MultiValue {
            if (c < 1) return MultiValue();
            LUserdata* u = static_cast<LUserdata*>(a[0].as_pointer());
            LinesIterUd* ud = static_cast<LinesIterUd*>(u->data());
            if (!ud->f->fp) return MultiValue();
            LValue line = read_line(L, ud->f->fp);
            if (line.type() == LType::Nil) return MultiValue();
            return MultiValue(line);
        };

        return MultiValue({
            L->create_closure(CFunctionType(iter)),
            LValue(u),
            nil()
        });
    }

    const char* filename = check_string(L, args[0]);
    FILE* fp = std::fopen(filename, "r");
    if (!fp) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "cannot open file '%s': %s", filename, std::strerror(errno));
        throw_runtime_error(buf);
    }

    LUserdata* u = static_cast<LUserdata*>(newuserdata(L, sizeof(LinesFileUd)).as_pointer());
    LinesFileUd* ud = static_cast<LinesFileUd*>(u->data());
    ud->fp = fp;
    ud->first = true;

    auto iter = [](LState* L, const LValue* a, size_t c) -> MultiValue {
        if (c < 1) return MultiValue();
        LUserdata* u = static_cast<LUserdata*>(a[0].as_pointer());
        LinesFileUd* ud = static_cast<LinesFileUd*>(u->data());
        if (!ud->fp) return MultiValue();
        if (ud->first) ud->first = false;
        if (!std::fgets(ud->buf, sizeof(ud->buf), ud->fp)) {
            fclose(ud->fp);
            ud->fp = nullptr;
            return MultiValue();
        }
        size_t n = std::strlen(ud->buf);
        if (n > 0 && ud->buf[n - 1] == '\n') n--;
        return MultiValue(LValue(L->intern_string(ud->buf, n)));
    };

    return MultiValue({
        L->create_closure(CFunctionType(iter)),
        LValue(u),
        nil()
    });
}

static MultiValue io_output(LState* L, const LValue* args, size_t count) {
    if (count == 0 || args[0].type() == LType::Nil) {
        if (default_output.type() != LType::Userdata)
            default_output = get_std_file(L, stdout);
        return MultiValue(default_output);
    }
    if (args[0].type() == LType::Userdata) {
        default_output = args[0];
    } else if (args[0].type() == LType::String) {
        const char* filename = args[0].as_string();
        FILE* fp = std::fopen(filename, "w");
        if (!fp) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "cannot open file '%s': %s", filename, std::strerror(errno));
            throw_runtime_error(buf);
        }
        default_output = make_file(L, fp, true);
    }
    return MultiValue(default_output);
}

static MultiValue io_read(LState* L, const LValue* args, size_t count) {
    if (default_input.type() != LType::Userdata)
        default_input = get_std_file(L, stdin);

    LValue file = default_input;
    FileUd* f = as_file(L, file);

    if (count == 0) {
        return MultiValue(read_line(L, f->fp));
    }

    std::vector<LValue> results;
    results.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (args[i].type() == LType::Integer || args[i].type() == LType::Number) {
            int64_t n = check_integer(L, args[i]);
            if (n <= 0) { results.push_back(LValue(L->intern_string("", 0))); continue; }
            size_t sz = static_cast<size_t>(n);
            uint32_t len32 = static_cast<uint32_t>(sz);
            char* mem = new char[8 + sz + 1];
            clx_memcpy(mem + 4, &len32, 4);
            size_t r = std::fread(mem + 8, 1, sz, f->fp);
            mem[8 + r] = '\0';
            uint32_t h = r <= 8 ? swar_hash_8(mem + 8, r) : wyhash_str(mem + 8, r);
            clx_memcpy(mem, &h, 4);
            results.push_back(LValue(L->string_pool.intern_preallocated(mem + 8, h, r)));
        } else if (args[i].type() == LType::String) {
            const char* fmt = args[i].as_string();
            if (std::strcmp(fmt, "*a") == 0 || std::strcmp(fmt, "*all") == 0) {
                results.push_back(read_all(L, f->fp));
            } else if (std::strcmp(fmt, "*l") == 0 || std::strcmp(fmt, "*line") == 0) {
                results.push_back(read_line(L, f->fp));
            } else if (std::strcmp(fmt, "*n") == 0 || std::strcmp(fmt, "*number") == 0) {
                double d;
                if (std::fscanf(f->fp, "%lf", &d) == 1)
                    results.push_back(number(d));
                else
                    results.push_back(LValue());
            }
        }
    }
    return MultiValue(results.data(), results.size());
}

static MultiValue io_write(LState* L, const LValue* args, size_t count) {
    if (default_output.type() != LType::Userdata)
        default_output = get_std_file(L, stdout);
    LUserdata* u = static_cast<LUserdata*>(default_output.as_pointer());
    FileUd* f = static_cast<FileUd*>(u->data());
    for (size_t i = 0; i < count; ++i) {
        if (args[i].type() == LType::String) {
            std::fwrite(args[i].as_string(), 1, args[i].string_len(), f->fp);
        } else if (args[i].type() == LType::Number || args[i].type() == LType::Integer) {
            char buf[64];
            int n;
            if (args[i].type() == LType::Integer)
                n = std::snprintf(buf, sizeof(buf), "%" PRId64, args[i].as_integer());
            else
                n = std::snprintf(buf, sizeof(buf), "%.14g", args[i].as_number());
            if (n > 0) std::fwrite(buf, 1, static_cast<size_t>(n), f->fp);
        }
    }
    return MultiValue(boolean(true));
}

static MultiValue io_type(LState* L, const LValue* args, size_t count) {
    if (count == 0) return MultiValue(nil());
    if (args[0].type() != LType::Userdata) return MultiValue(nil());
    LUserdata* ud = static_cast<LUserdata*>(args[0].as_pointer());
    if (ud->size < sizeof(FileUd)) return MultiValue(nil());
    FileUd* f = static_cast<FileUd*>(ud->data());
    if (!f->fp) return MultiValue(string(L, "closed file"));
    return MultiValue(string(L, "file"));
}

static MultiValue io_tmpfile(LState* L, const LValue* args, size_t count) {
    FILE* fp = std::tmpfile();
    if (!fp)
        throw_runtime_error("cannot create temporary file");
    return MultiValue(make_file(L, fp, true));
}

void luastd_io(LState* L) {
    LValue t = L->create_table();
    LTable* tbl = static_cast<LTable*>(t.as_pointer());
    tbl->bind_all(L, {
        {"close",   io_close},
        {"flush",   io_flush},
        {"input",   io_input},
        {"lines",   io_lines},
        {"open",    io_open},
        {"output",  io_output},
        {"read",    io_read},
        {"type",    io_type},
        {"write",   io_write},
        {"tmpfile", io_tmpfile}
    });
    set_global(L, "io", t);
}

}