// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  os.cpp · OS Library                        │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sys/wait.h>
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
MultiValue os_clock(LState *L, const LValue *args, size_t count) {
    return MultiValue(clx::number(static_cast<double>(std::clock()) / CLOCKS_PER_SEC));
}

//------------------ date_specifier_ok: strftime conversion characters os.date accepts, so an invalid
//------------------ specifier raises stock's error instead of leaking raw text through strftime
static bool date_specifier_ok(char c) {
    return std::strchr("aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%", c) != nullptr;
}

//------------------ date_modifier_ok: conversions that accept the 'E' and 'O' alternatives
static bool date_modifier_ok(char c, char mod) {
    const char *set = (mod == 'E') ? "cCxXyY" : "deHImMSuUVwWy";
    return std::strchr(set, c) != nullptr;
}

//------------------ format_date: stock's os.date format walker — plain bytes (embedded NULs included) are
//------------------ copied verbatim, each conversion is expanded by one strftime call, invalid specifiers
//------------------ raise "bad argument #1 to 'date' (invalid conversion specifier '...')"
static MultiValue format_date(LState *L, const char *s, size_t len, struct tm *stm) {
    const char *se = s + len;
    std::string out;
    while (s < se) {
        if (*s != '%') {
            out.push_back(*s++);
            continue;
        }
        const char *spec_beg = s++;
        if (s < se && *s == '%') {
            out.push_back('%');
            s++;
            continue;
        }
        while (s < se && std::strchr("-_0^#", *s) != nullptr)
            s++;
        while (s < se && std::isdigit(static_cast<unsigned char>(*s)))
            s++;
        bool ok = (s < se);
        if (ok && (*s == 'E' || *s == 'O')) {
            char mod = *s++;
            ok = (s < se) && date_modifier_ok(*s, mod);
        }
        if (ok)
            ok = date_specifier_ok(*s);
        if (!ok) {
            std::string bad(spec_beg, se);
            throw_runtime_error(bad_arg_msg(L, 1, "os.date", ("invalid conversion specifier '" + bad + "'").c_str()));
        }
        s++;
        std::string spec(spec_beg, s);
        char buf[256];
        size_t n = std::strftime(buf, sizeof(buf), spec.c_str(), stm);
        if (n == 0)
            throw_runtime_error("date result cannot be represented in this installation");
        out.append(buf, n);
    }
    return MultiValue(clx::string(L, out.data(), out.size()));
}

//------------------ os_date: returns formatted date/time string or table
MultiValue os_date(LState *L, const LValue *args, size_t count) {
    LValue fmt = clx::string(L, "%c");
    if (count >= 1 && args[0].type != Nil) {
        double coerced;
        if (args[0].type != String && !args[0].to_number(coerced))
            throw_runtime_error(bad_arg_msg(L, 1, "os.date", "string expected"));
        fmt = args[0];
    }
    time_t t = std::time(nullptr);
    if (count >= 2 && args[1].type != Nil)
        t = static_cast<time_t>(check_integer(L, args[1]));

    const char *s = fmt.as_string();
    size_t len = fmt.string_len();
    bool utc = (len > 0 && s[0] == '!');
    if (utc) {
        s++;
        len--;
    }

    struct tm stm;
    if ((utc ? gmtime(&t, &stm) : localtime(&t, &stm)) == nullptr)
        throw_runtime_error("date result cannot be represented in this installation");

    if (len == 2 && s[0] == '*' && s[1] == 't') {
        LValue tbl = L->create_table();
        LTable *tb = static_cast<LTable *>(tbl.as_pointer());
        tb->settable(LValue(L->intern_string("year")), LValue(static_cast<int64_t>(stm.tm_year + 1900)));
        tb->settable(LValue(L->intern_string("month")), LValue(static_cast<int64_t>(stm.tm_mon + 1)));
        tb->settable(LValue(L->intern_string("day")), LValue(static_cast<int64_t>(stm.tm_mday)));
        tb->settable(LValue(L->intern_string("hour")), LValue(static_cast<int64_t>(stm.tm_hour)));
        tb->settable(LValue(L->intern_string("min")), LValue(static_cast<int64_t>(stm.tm_min)));
        tb->settable(LValue(L->intern_string("sec")), LValue(static_cast<int64_t>(stm.tm_sec)));
        tb->settable(LValue(L->intern_string("wday")), LValue(static_cast<int64_t>(stm.tm_wday + 1)));
        tb->settable(LValue(L->intern_string("yday")), LValue(static_cast<int64_t>(stm.tm_yday + 1)));
        tb->settable(LValue(L->intern_string("isdst")), LValue(static_cast<bool>(stm.tm_isdst > 0)));
        return MultiValue(tbl);
    }
    return format_date(L, s, len, &stm);
}

//------------------ os_difftime: returns difference in seconds between two times
MultiValue os_difftime(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        throw_runtime_error(bad_arg_msg(L, 1, "os.difftime", "number expected, got no value"));
    double t2 = check_number(L, args[0]);
    double t1 = check_number(L, args[1]);
    return MultiValue(clx::number(t2 - t1));
}

//------------------ exec_result: stock luaL_execresult — a wait status decoded into (ok, "exit"|"signal",
//------------------ code), so os.execute and a popen handle's close report the same triple
MultiValue exec_result(LState *L, int status) {
    if (status == -1) {
        int en = errno;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", std::strerror(en));
        return MultiValue({ LValue(), LValue(L->intern_string(buf)), clx::integer(en) });
    }
#if defined(_WIN32) || defined(_WIN64)
    int code = status;
    const char *what = "exit";
#else
    int code = WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status);
    const char *what = WIFSIGNALED(status) ? "signal" : "exit";
#endif
    return MultiValue({ code == 0 ? clx::boolean(true) : clx::LValue(), clx::string(L, what),
        clx::integer(static_cast<int64_t>(code)) });
}

//------------------ os_execute: executes a system command
MultiValue os_execute(LState *L, const LValue *args, size_t count) {
    if (count == 0 || args[0].type == Nil) {
        int r = std::system(nullptr);
        return MultiValue(clx::boolean(r != 0));
    }
    const char *cmd = check_string(L, args[0]);
    return exec_result(L, std::system(cmd));
}

//------------------ os_exit: terminates the program with a status code
MultiValue os_exit(LState *L, const LValue *args, size_t count) {
    int64_t code = 0;
    if (count >= 1 && args[0].type != Nil) {
        code = check_integer(L, args[0]);
    }
    std::exit(static_cast<int>(code));
    return MultiValue();
}

//------------------ os_getenv: gets an environment variable
MultiValue os_getenv(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error(bad_arg_msg(L, 1, "os.getenv", "string expected, got no value"));
    const char *name = check_string(L, args[0]);
    const char *val = std::getenv(name);
    if (!val)
        return MultiValue(LValue());
    return MultiValue(clx::string(L, val));
}

//------------------ os_remove: deletes a file
MultiValue os_remove(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error(bad_arg_msg(L, 1, "os.remove", "string expected, got no value"));
    const char *path = check_string(L, args[0]);
    //------------------ std::filesystem::remove() reports a missing file as "not removed" with a clear
    //------------------ error_code, so stock's nil, "name: reason", errno needs the C library call instead
    if (std::remove(path) != 0) {
        int en = errno;
        char buf[384];
        std::snprintf(buf, sizeof(buf), "%s: %s", path, std::strerror(en));
        return MultiValue({ LValue(), LValue(L->intern_string(buf)), clx::integer(en) });
    }
    return MultiValue(clx::boolean(true));
}

//------------------ os_rename: renames a file or directory
MultiValue os_rename(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        throw_runtime_error(bad_arg_msg(L, 1, "os.rename", "string expected, got no value"));
    const char *oldname = check_string(L, args[0]);
    const char *newname = check_string(L, args[1]);
    std::error_code ec;
    std::filesystem::rename(oldname, newname, ec);
    if (ec) {
        //------------------ stock os.rename reports the failure as nil, reason, errno
        int en = ec.value();
        return MultiValue({ LValue(), LValue(L->intern_string(std::strerror(en))), clx::integer(en) });
    }
    return MultiValue(clx::boolean(true));
}

//------------------ os_setlocale: sets or queries the program's locale
MultiValue os_setlocale(LState *L, const LValue *args, size_t count) {
    if (count == 0) {
        const char *cur = std::setlocale(LC_ALL, nullptr);
        return MultiValue(clx::string(L, cur ? cur : "C"));
    }
    //------------------ stock takes an optional string: nil (or a missing argument) queries the
    //------------------ current locale for the category instead of being a type error
    const char *locale = nullptr;
    if (args[0].type != Nil)
        locale = check_string(L, args[0]);
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
            throw_runtime_error(bad_arg_msg(L, 2, "os.setlocale", std::string("invalid option '") + cat + "'"));
        }
    }
    const char *result = std::setlocale(category, locale);
    if (!result)
        return MultiValue(LValue());
    return MultiValue(clx::string(L, result));
}

//------------------ os_time: returns current time or converts a table to a time value
MultiValue os_time(LState *L, const LValue *args, size_t count) {
    if (count == 0 || args[0].type == Nil)
        return MultiValue(clx::integer(static_cast<int64_t>(std::time(nullptr))));

    LTable *tbl = check_table(L, args[0]);
    struct tm tm;
    std::memset(&tm, 0, sizeof(tm));

    //------------------ field: stock's getfield — an absent field either takes its default or raises
    //------------------ "missing in date table", a non-integral one raises "is not an integer", and an
    //------------------ int whose shifted value leaves int range raises "is out-of-bound"
    auto field = [&](const char *name, bool has_default, int64_t dflt, int64_t delta) -> int {
        LValue p = tbl->gettable(LValue(L->intern_string(name)));
        std::string key(name);
        if (p.type == Nil) {
            if (!has_default)
                throw_runtime_error(("field '" + key + "' missing in date table").c_str());
            return static_cast<int>(dflt - delta);
        }
        int64_t iv;
        if (!to_integer(p, iv))
            throw_runtime_error(("field '" + key + "' is not an integer").c_str());
        bool in_range = (iv >= 0) ? (iv - delta <= INT_MAX) : (INT_MIN + delta <= iv);
        if (!in_range)
            throw_runtime_error(("field '" + key + "' is out-of-bound").c_str());
        return static_cast<int>(iv - delta);
    };

    tm.tm_year = field("year", false, 0, 1900);
    tm.tm_mon = field("month", false, 0, 1);
    tm.tm_mday = field("day", false, 0, 0);
    tm.tm_hour = field("hour", true, 12, 0);
    tm.tm_min = field("min", true, 0, 0);
    tm.tm_sec = field("sec", true, 0, 0);
    LValue isdst = tbl->gettable(LValue(L->intern_string("isdst")));
    tm.tm_isdst = (isdst.type == Nil) ? -1 : (to_boolean(isdst) ? 1 : 0);

    time_t t = mktime(&tm);
    if (t == static_cast<time_t>(-1))
        throw_runtime_error("time result cannot be represented in this installation");

    tbl->settable(LValue(L->intern_string("year")), LValue(static_cast<int64_t>(tm.tm_year + 1900)));
    tbl->settable(LValue(L->intern_string("month")), LValue(static_cast<int64_t>(tm.tm_mon + 1)));
    tbl->settable(LValue(L->intern_string("day")), LValue(static_cast<int64_t>(tm.tm_mday)));
    tbl->settable(LValue(L->intern_string("hour")), LValue(static_cast<int64_t>(tm.tm_hour)));
    tbl->settable(LValue(L->intern_string("min")), LValue(static_cast<int64_t>(tm.tm_min)));
    tbl->settable(LValue(L->intern_string("sec")), LValue(static_cast<int64_t>(tm.tm_sec)));
    tbl->settable(LValue(L->intern_string("wday")), LValue(static_cast<int64_t>(tm.tm_wday + 1)));
    tbl->settable(LValue(L->intern_string("yday")), LValue(static_cast<int64_t>(tm.tm_yday + 1)));
    tbl->settable(LValue(L->intern_string("isdst")), LValue(static_cast<bool>(tm.tm_isdst > 0)));
    return MultiValue(clx::integer(static_cast<int64_t>(t)));
}

//------------------ os_tmpname: generates a temporary file path
MultiValue os_tmpname(LState *L, const LValue *args, size_t count) {
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
