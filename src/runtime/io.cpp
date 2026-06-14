#include "clx.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
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
            if (!std::fgets(ud->buf, sizeof(ud->buf), ud->f->fp)) return MultiValue();
            size_t n = std::strlen(ud->buf);
            if (n > 0 && ud->buf[n - 1] == '\n') ud->buf[n - 1] = '\0';
            return MultiValue(clx::string(L, ud->buf));
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
            char buf[4096];
            if (!std::fgets(buf, sizeof(buf), f->fp))
                return MultiValue(LValue());
            size_t n = std::strlen(buf);
            if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
            return MultiValue(string(L, buf));
        }
        std::vector<LValue> results;
        for (size_t i = 1; i < c; ++i) {
            if (a[i].type() == LType::Integer || a[i].type() == LType::Number) {
                int64_t n = check_integer(L, a[i]);
                if (n <= 0) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "bad argument #%zu to 'read' (invalid size)", i + 1);
                    throw_runtime_error(buf);
                }
                char* buf = new char[static_cast<size_t>(n) + 1];
                size_t r = std::fread(buf, 1, static_cast<size_t>(n), f->fp);
                buf[r] = '\0';
                results.push_back(string(L, buf));
                delete[] buf;
            } else if (a[i].type() == LType::String) {
                const char* fmt = a[i].as_string();
                if (std::strcmp(fmt, "*a") == 0 || std::strcmp(fmt, "*all") == 0) {
                    std::string content;
                    char buf[4096];
                    size_t r;
                    while ((r = std::fread(buf, 1, sizeof(buf), f->fp)) > 0)
                        content.append(buf, r);
                    results.push_back(string(L, content.c_str(), content.size()));
                } else if (std::strcmp(fmt, "*l") == 0 || std::strcmp(fmt, "*line") == 0) {
                    char buf[4096];
                    if (!std::fgets(buf, sizeof(buf), f->fp))
                        results.push_back(LValue());
                    else {
                        size_t n = std::strlen(buf);
                        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
                        results.push_back(string(L, buf));
                    }
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
            std::string s;
            if (a[i].type() == LType::Number || a[i].type() == LType::Integer) {
                s = a[i].to_string(L);
            } else if (a[i].type() == LType::String) {
                s = a[i].as_string();
            } else {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "bad argument #%zu to 'write' (string/number expected)", i + 1);
                throw_runtime_error(buf);
            }
            if (std::fputs(s.c_str(), f->fp) == EOF) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "cannot write file: %s", std::strerror(errno));
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
        {"close", meth_close},
        {"flush", meth_flush},
        {"lines", meth_lines},
        {"read", meth_read},
        {"seek", meth_seek},
        {"setvbuf", meth_setvbuf},
        {"write", meth_write},
        {"__gc", meth_gc}
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
            if (!std::fgets(ud->buf, sizeof(ud->buf), ud->f->fp)) return MultiValue();
            size_t n = std::strlen(ud->buf);
            if (n > 0 && ud->buf[n - 1] == '\n') ud->buf[n - 1] = '\0';
            return MultiValue(string(L, ud->buf));
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
            char buf[4096];
            if (!std::fgets(buf, sizeof(buf), f->fp))
                return MultiValue(LValue());
            size_t n = std::strlen(buf);
            if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
            return MultiValue(string(L, buf));
        }
        std::vector<LValue> results;
        for (size_t i = 1; i < c; ++i) {
            if (a[i].type() == LType::Integer || a[i].type() == LType::Number) {
                int64_t n = check_integer(L, a[i]);
                char* buf = new char[static_cast<size_t>(n) + 1];
                size_t r = std::fread(buf, 1, static_cast<size_t>(n), f->fp);
                buf[r] = '\0';
                results.push_back(string(L, buf));
                delete[] buf;
            } else if (a[i].type() == LType::String) {
                const char* fmt = a[i].as_string();
                if (std::strcmp(fmt, "*a") == 0 || std::strcmp(fmt, "*all") == 0) {
                    std::string content;
                    char buf[4096];
                    size_t r;
                    while ((r = std::fread(buf, 1, sizeof(buf), f->fp)) > 0)
                        content.append(buf, r);
                    results.push_back(string(L, content.c_str(), content.size()));
                } else if (std::strcmp(fmt, "*l") == 0 || std::strcmp(fmt, "*line") == 0) {
                    char buf[4096];
                    if (!std::fgets(buf, sizeof(buf), f->fp))
                        results.push_back(LValue());
                    else {
                        size_t n = std::strlen(buf);
                        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
                        results.push_back(string(L, buf));
                    }
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
            std::string s;
            if (a[i].type() == LType::Number || a[i].type() == LType::Integer)
                s = a[i].to_string(L);
            else if (a[i].type() == LType::String)
                s = a[i].as_string();
            else
                continue;
            std::fputs(s.c_str(), f->fp);
        }
        return MultiValue(a[0]);
    };

    mt->bind_all(L, {
        {"close", meth_close},
        {"flush", meth_flush},
        {"lines", meth_lines},
        {"read", meth_read},
        {"seek", meth_seek},
        {"write", meth_write}
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
            if (f->fp) {
                fclose(f->fp);
                f->fp = nullptr;
            }
        }
        return MultiValue(boolean(true));
    }
    FileUd* f = as_file(L, args[0]);
    if (f->fp) {
        fclose(f->fp);
        f->fp = nullptr;
    }
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
            if (!std::fgets(ud->buf, sizeof(ud->buf), ud->f->fp)) return MultiValue();
            size_t n = std::strlen(ud->buf);
            if (n > 0 && ud->buf[n - 1] == '\n') ud->buf[n - 1] = '\0';
            return MultiValue(string(L, ud->buf));
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
        if (ud->first) { ud->first = false; }
        if (!std::fgets(ud->buf, sizeof(ud->buf), ud->fp)) {
            fclose(ud->fp);
            ud->fp = nullptr;
            return MultiValue();
        }
        size_t n = std::strlen(ud->buf);
        if (n > 0 && ud->buf[n - 1] == '\n') ud->buf[n - 1] = '\0';
        return MultiValue(string(L, ud->buf));
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
        char buf[4096];
        if (!std::fgets(buf, sizeof(buf), f->fp))
            return MultiValue(LValue());
        size_t n = std::strlen(buf);
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        return MultiValue(string(L, buf));
    }

    std::vector<LValue> results;
    for (size_t i = 0; i < count; ++i) {
        if (args[i].type() == LType::Integer || args[i].type() == LType::Number) {
            int64_t n = check_integer(L, args[i]);
            char* buf = new char[static_cast<size_t>(n) + 1];
            size_t r = std::fread(buf, 1, static_cast<size_t>(n), f->fp);
            buf[r] = '\0';
            results.push_back(string(L, buf));
            delete[] buf;
        } else if (args[i].type() == LType::String) {
            const char* fmt = args[i].as_string();
            if (std::strcmp(fmt, "*a") == 0 || std::strcmp(fmt, "*all") == 0) {
                std::string content;
                char buf[4096];
                size_t r;
                while ((r = std::fread(buf, 1, sizeof(buf), f->fp)) > 0)
                    content.append(buf, r);
                results.push_back(string(L, content.c_str(), content.size()));
            } else if (std::strcmp(fmt, "*l") == 0 || std::strcmp(fmt, "*line") == 0) {
                char buf[4096];
                if (!std::fgets(buf, sizeof(buf), f->fp))
                    results.push_back(LValue());
                else {
                    size_t n = std::strlen(buf);
                    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
                    results.push_back(string(L, buf));
                }
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
        if (args[i].type() == LType::String)
            std::fwrite(args[i].as_string(), 1, args[i].string_len(), f->fp);
        else if (args[i].type() == LType::Number || args[i].type() == LType::Integer) {
            std::string s = args[i].to_string(L);
            std::fputs(s.c_str(), f->fp);
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
        {"close", io_close},
        {"flush", io_flush},
        {"input", io_input},
        {"lines", io_lines},
        {"open", io_open},
        {"output", io_output},
        {"read", io_read},
        {"type", io_type},
        {"write", io_write},
        {"tmpfile", io_tmpfile}
    });
    set_global(L, "io", t);
}

}
