// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  os.cpp · OS Library                        │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <chrono>
#include <atomic>
#if defined(_WIN32) || defined(_WIN64)
#include <cstdio>

//------------------ gmtime: thread-safe gmtime wrapper
inline struct tm *gmtime(const time_t *timep, struct tm *result) {
    if (gmtime_s(result, timep) == 0)
        return result;
    return nullptr;
}

//------------------ localtime: thread-safe localtime wrapper
inline struct tm *localtime(const time_t *timep, struct tm *result) {
    if (localtime_s(result, timep) == 0)
        return result;
    return nullptr;
}
#else

//------------------ gmtime: thread-safe gmtime wrapper (POSIX)
inline struct tm *gmtime(const time_t *timep, struct tm *result) {
    return gmtime_r(timep, result);
}

//------------------ localtime: thread-safe localtime wrapper (POSIX)
inline struct tm *localtime(const time_t *timep, struct tm *result) {
    return localtime_r(timep, result);
}
#endif
namespace clx {

//------------------ os_clock: returns CPU time in seconds
static MultiValue os_clock(LState *L, const LValue *args, size_t count) {
    return MultiValue(clx::number(static_cast<double>(std::clock()) / CLOCKS_PER_SEC));
}

//------------------ os_date: returns formatted date/time string or table
static MultiValue os_date(LState *L, const LValue *args, size_t count) {
    const char *format = opt_string(L, count >= 1 ? args[0] : LValue(), "%c");
    time_t t = std::time(nullptr);
    if (count >= 2 && args[1].type != Nil) {
        int64_t ti = check_integer(L, args[1]);
        t = static_cast<time_t>(ti);
    }
    if (format[0] == '!' && format[1] != '\0') {
        format += 1;
        struct tm gmt;
        gmtime(&t, &gmt);
        if (format[0] == 't') {
            return MultiValue(clx::number(static_cast<double>(mktime(&gmt))));
        }
        char buf[256];
        size_t n = std::strftime(buf, sizeof(buf), format, &gmt);
        if (n == 0)
            buf[0] = '\0';
        return MultiValue(clx::string(L, buf));
    }
    if (format[0] == '*') {
        if (format[1] == 't') {
            struct tm tm;
            localtime(&t, &tm);
            LValue tbl = L->create_table();
            LTable *tb = static_cast<LTable *>(tbl.as_pointer());
            tb->settable(LValue(L->intern_string("year")), LValue(static_cast<int64_t>(tm.tm_year + 1900)));
            tb->settable(LValue(L->intern_string("month")), LValue(static_cast<int64_t>(tm.tm_mon + 1)));
            tb->settable(LValue(L->intern_string("day")), LValue(static_cast<int64_t>(tm.tm_mday)));
            tb->settable(LValue(L->intern_string("hour")), LValue(static_cast<int64_t>(tm.tm_hour)));
            tb->settable(LValue(L->intern_string("min")), LValue(static_cast<int64_t>(tm.tm_min)));
            tb->settable(LValue(L->intern_string("sec")), LValue(static_cast<int64_t>(tm.tm_sec)));
            tb->settable(LValue(L->intern_string("wday")), LValue(static_cast<int64_t>(tm.tm_wday + 1)));
            tb->settable(LValue(L->intern_string("yday")), LValue(static_cast<int64_t>(tm.tm_yday + 1)));
            tb->settable(LValue(L->intern_string("isdst")), LValue(static_cast<bool>(tm.tm_isdst)));
            return MultiValue(tbl);
        }
    }
    struct tm tm;
    localtime(&t, &tm);
    char buf[256];
    size_t n = std::strftime(buf, sizeof(buf), format, &tm);
    if (n == 0)
        buf[0] = '\0';
    return MultiValue(clx::string(L, buf));
}

//------------------ os_difftime: returns difference in seconds between two times
static MultiValue os_difftime(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        throw_runtime_error("bad argument #1 to 'difftime' (number expected, got no value)");
    double t2 = check_number(L, args[0]);
    double t1 = check_number(L, args[1]);
    return MultiValue(clx::number(t2 - t1));
}

//------------------ os_execute: executes a system command
static MultiValue os_execute(LState *L, const LValue *args, size_t count) {
    if (count == 0 || args[0].type == Nil) {
        int r = std::system(nullptr);
        return MultiValue(clx::boolean(r != 0));
    }
    const char *cmd = check_string(L, args[0]);
    int r = std::system(cmd);
    return MultiValue(clx::integer(static_cast<int64_t>(r)));
}

//------------------ os_exit: terminates the program with a status code
static MultiValue os_exit(LState *L, const LValue *args, size_t count) {
    int64_t code = 0;
    if (count >= 1 && args[0].type != Nil) {
        code = check_integer(L, args[0]);
    }
    std::exit(static_cast<int>(code));
    return MultiValue();
}

//------------------ os_getenv: gets an environment variable
static MultiValue os_getenv(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'getenv' (string expected, got no value)");
    const char *name = check_string(L, args[0]);
    const char *val = std::getenv(name);
    if (!val)
        return MultiValue(LValue());
    return MultiValue(clx::string(L, val));
}

//------------------ os_remove: deletes a file
static MultiValue os_remove(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'remove' (string expected, got no value)");
    const char *path = check_string(L, args[0]);
    std::error_code ec;
    if (!std::filesystem::remove(path, ec) && ec) {
        if (ec.value() == static_cast<int>(std::errc::no_such_file_or_directory)) {
            return MultiValue(clx::boolean(true));
        }
        char buf[384];
        std::snprintf(buf, sizeof(buf), "cannot remove file '%s': %s", path, ec.message().c_str());
        throw_runtime_error(buf);
    }
    return MultiValue(clx::boolean(true));
}

//------------------ os_rename: renames a file or directory
static MultiValue os_rename(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        throw_runtime_error("bad argument #1 to 'rename' (string expected, got no value)");
    const char *oldname = check_string(L, args[0]);
    const char *newname = check_string(L, args[1]);
    std::error_code ec;
    std::filesystem::rename(oldname, newname, ec);
    if (ec) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "cannot rename file '%s' to '%s': %s", oldname, newname, ec.message().c_str());
        throw_runtime_error(buf);
    }
    return MultiValue(clx::boolean(true));
}

//------------------ os_setlocale: sets or queries the program's locale
static MultiValue os_setlocale(LState *L, const LValue *args, size_t count) {
    if (count == 0) {
        const char *cur = std::setlocale(LC_ALL, nullptr);
        return MultiValue(clx::string(L, cur ? cur : "C"));
    }
    const char *locale = check_string(L, args[0]);
    int category = LC_ALL;
    if (count >= 2 && args[1].type != Nil) {
        const char *cat = check_string(L, args[1]);
        if (std::strcmp(cat, "all") == 0)
            category = LC_ALL;
        else if (std::strcmp(cat, "collate") == 0)
            category = LC_COLLATE;
        else if (std::strcmp(cat, "ctype") == 0)
            category = LC_CTYPE;
        else if (std::strcmp(cat, "monetary") == 0)
            category = LC_MONETARY;
        else if (std::strcmp(cat, "numeric") == 0)
            category = LC_NUMERIC;
        else if (std::strcmp(cat, "time") == 0)
            category = LC_TIME;
        else {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "bad argument #2 to 'setlocale' (invalid category '%s')", cat);
            throw_runtime_error(buf);
        }
    }
    const char *result = std::setlocale(category, locale);
    if (!result)
        return MultiValue(LValue());
    return MultiValue(clx::string(L, result));
}

//------------------ os_time: returns current time or converts a table to a time value
static MultiValue os_time(LState *L, const LValue *args, size_t count) {
    if (count == 0 || args[0].type == Nil) {
        return MultiValue(clx::number(static_cast<double>(std::time(nullptr))));
    }
    LTable *tbl = check_table(L, args[0]);
    struct tm tm;
    std::memset(&tm, 0, sizeof(tm));
    auto get_field = [&](const char *name) -> int {
        LValue p = tbl->gettable(LValue(L->intern_string(name)));
        if (p.type != Nil) {
            double d;
            if (p.to_number(d))
                return static_cast<int>(d);
        }
        return 0;
    };
    tm.tm_sec = get_field("sec");
    tm.tm_min = get_field("min");
    tm.tm_hour = get_field("hour");
    tm.tm_mday = get_field("day");
    tm.tm_mon = get_field("month") - 1;
    tm.tm_year = get_field("year") - 1900;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == static_cast<time_t>(-1))
        throw_runtime_error("time result cannot be represented in this system");
    return MultiValue(clx::number(static_cast<double>(t)));
}

//------------------ os_tmpname: generates a temporary file path
static MultiValue os_tmpname(LState *L, const LValue *args, size_t count) {
    try {
        static std::atomic<uint64_t> counter { 0 };
        auto temp_dir = std::filesystem::temp_directory_path();
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
        char filename[64];
        std::snprintf(filename, sizeof(filename), "clx_%llx_%llx.tmp", static_cast<unsigned long long>(now),
            static_cast<unsigned long long>(id));
        auto full_path = temp_dir / filename;
        return MultiValue(clx::string(L, full_path.string().c_str()));
    } catch (...) {
        throw_runtime_error("cannot generate a temporary file name");
    }
    return MultiValue();
}

//------------------ luastd_os: registers the os library into the global state
void luastd_os(LState *L) {
    LValue t = L->create_table();
    LTable *tbl = static_cast<LTable *>(t.as_pointer());
    static constexpr clx::LazyReg os_funcs[]
        = { { "clock", os_clock }, { "date", os_date }, { "difftime", os_difftime }, { "execute", os_execute },
              { "exit", os_exit }, { "getenv", os_getenv }, { "remove", os_remove }, { "rename", os_rename },
              { "setlocale", os_setlocale }, { "time", os_time }, { "tmpname", os_tmpname } };
    clx::set_lazy_funcs(L, t, os_funcs, std::size(os_funcs));
    set_global(L, "os", t);
}
}
