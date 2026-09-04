// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  package.cpp · Package/Module System        │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace clx {

//------------------ Default package.path / package.cpath templates (Lua 5.5-compatible; './' works on Windows too)
#if defined(_WIN32)
#define CLX_LPATH_DEFAULT ".\\?.lua;.\\?\\init.lua"
#define CLX_CPATH_DEFAULT ".\\?.dll"
#define CLX_DIRSEP '\\'
#else
#define CLX_LPATH_DEFAULT "./?.lua;./?/init.lua"
#define CLX_CPATH_DEFAULT "./?.so"
#define CLX_DIRSEP '/'
#endif

//------------------ Reads a field of the package table, returns nil LValue when absent
static LValue get_package_field(LState *L, const char *key, size_t keylen) {
    LValue pack_val = get_global(L, "package");
    if (pack_val.type != Table)
        return LValue();
    LTable *pack = static_cast<LTable *>(pack_val.as_pointer());
    return pack->gettable(LValue(L->intern_string(key, keylen)));
}

//------------------ Checks whether a file is readable (fopen probe, closed immediately)
static bool file_readable(const std::string &filename) {
    FILE *f = std::fopen(filename.c_str(), "re");
    if (!f)
        return false;
    std::fclose(f);
    return true;
}

//------------------ Splits a ';'-separated path template list into individual templates
static void split_path(const char *path, size_t len, std::vector<std::string> &out) {
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || path[i] == ';') {
            if (i > start)
                out.emplace_back(path + start, i - start);
            start = i + 1;
        }
    }
}

//------------------ Replaces every occurrence of 'from' with 'to' in s
static std::string replace_char(std::string s, char from, char to) {
    if (from == to)
        return s;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == from)
            s[i] = to;
    }
    return s;
}

//------------------ package_searchpath: resolve module name through path templates; when tried is non-null it
// receives one "\n\tno file '<candidate>'" line per probed template. Returns the first readable file or "".
std::string package_searchpath(LState *L, const char *name, size_t name_len, const char *path, size_t path_len,
    char sep, char rep, std::string *tried) {
    (void)L;
    std::string filename = replace_char(std::string(name, name_len), sep, rep);
    std::vector<std::string> templates;
    split_path(path, path_len, templates);
    for (const std::string &tpl : templates) {
        std::string candidate;
        for (size_t i = 0; i < tpl.size(); ++i) {
            if (tpl[i] == '?')
                candidate += filename;
            else
                candidate += tpl[i];
        }
        if (file_readable(candidate))
            return candidate;
        if (tried) {
            *tried += "\n\tno file '";
            *tried += candidate;
            *tried += "'";
        }
    }
    return std::string();
}

//------------------ pkg_searchpath - package.searchpath(name, path [, sep [, rep]])
static MultiValue pkg_searchpath(LState *L, const LValue *args, size_t count) {
    if (count < 1 || args[0].type != String)
        throw_runtime_error("bad argument #1 to 'searchpath' (string expected)");
    if (count < 2 || args[1].type != String)
        throw_runtime_error("bad argument #2 to 'searchpath' (string expected)");
    char sep = (count >= 3 && args[2].type == String && args[2].string_len() > 0) ? args[2].as_string()[0] : '.';
    char rep = (count >= 4 && args[3].type == String && args[3].string_len() > 0) ? args[3].as_string()[0] : CLX_DIRSEP;

    std::string tried;
    std::string found = package_searchpath(
        L, args[0].as_string(), args[0].string_len(), args[1].as_string(), args[1].string_len(), sep, rep, &tried);
    if (!found.empty())
        return MultiValue(LValue(L->intern_string(found.data(), found.size())));
    return MultiValue(LValue(), LValue(L->intern_string(tried.data(), tried.size())));
}

//------------------ pkg_searcher_preload - package.searchers[1]: loader lookup in package.preload
static MultiValue pkg_searcher_preload(LState *L, const LValue *args, size_t count) {
    if (count < 1 || args[0].type != String)
        return MultiValue(LValue(), LValue(L->intern_string("\n\tno value passed to preload searcher")));
    LValue name = args[0];
    LValue preload = get_package_field(L, "preload", 7);
    if (preload.type == Table) {
        LValue loader = static_cast<LTable *>(preload.as_pointer())->gettable(name);
        if (loader.type == Function)
            return MultiValue(loader);
    }
    std::string msg = "\n\tno field package.preload['";
    msg.append(args[0].as_string(), args[0].string_len());
    msg += "'";
    return MultiValue(LValue(), LValue(L->intern_string(msg.data(), msg.size())));
}

//------------------ pkg_searcher_luafile_aot - package.searchers[2] placeholder for AOT builds: locates the file
// via package.path but loading it requires a --dynamic build (embedded VM). Never returns a loader.
static MultiValue pkg_searcher_luafile_aot(LState *L, const LValue *args, size_t count) {
    if (count < 1 || args[0].type != String)
        return MultiValue(LValue(), LValue());
    LValue path = get_package_field(L, "path", 4);
    if (path.type != String)
        return MultiValue(LValue(), LValue(L->intern_string("\n\tpackage.path is not a string")));
    std::string tried;
    std::string found = package_searchpath(
        L, args[0].as_string(), args[0].string_len(), path.as_string(), path.string_len(), '.', CLX_DIRSEP, &tried);
    if (found.empty())
        return MultiValue(LValue(), LValue(L->intern_string(tried.data(), tried.size())));
    std::string msg = "\n\tfound '" + found + "' but loading Lua files requires compiling with --dynamic";
    return MultiValue(LValue(), LValue(L->intern_string(msg.data(), msg.size())));
}

//------------------ pkg_searcher_c - package.searchers[3]: C modules must be linked statically (see --modules)
static MultiValue pkg_searcher_c(LState *L, const LValue *args, size_t count) {
    (void)L;
    (void)args;
    (void)count;
    return MultiValue(LValue(),
        LValue(
            L->intern_string("\n\tno C loader (C modules must be linked statically at compile time; see --modules)")));
}

//------------------ pack_require: implements Lua's require() by iterating package.searchers
static MultiValue pack_require(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'require' (string expected, got no value)");

    const char *modname = check_string(L, args[0]);
    LValue env = (count > 1 && args[1].type != Nil) ? args[1] : LValue();
    LValue loaded_key = LValue(L->intern_string("loaded"));

    LValue pack_val = get_global(L, "package");
    if (pack_val.type != Table)
        throw_runtime_error("global 'package' is not a table");

    LTable *pack = static_cast<LTable *>(pack_val.as_pointer());

    LValue loaded_ptr = pack->gettable(loaded_key);
    if (loaded_ptr.type != Table)
        throw_runtime_error("'package.loaded' is not a table");
    LTable *loaded = static_cast<LTable *>(loaded_ptr.as_pointer());

    LValue mod_key = LValue(L->intern_string(modname));
    LValue already_loaded = loaded->gettable(mod_key);
    if (already_loaded.type != Nil) {
        return MultiValue(already_loaded);
    }

    LValue searchers = pack->gettable(LValue(L->intern_string("searchers")));
    if (searchers.type != Table)
        throw LRuntimeException(LValue(L->intern_string("'package.searchers' must be a table")));
    LTable *searchers_tbl = static_cast<LTable *>(searchers.as_pointer());

    std::string msgs;
    for (int64_t i = 1;; ++i) {
        LValue loader = searchers_tbl->gettable(LValue(i));
        if (loader.type == Nil || loader.type != Function)
            break;

        //------------------ Searchers run with the real global table (they need package/preload access)
        LValue loader_args[1] = { mod_key };
        L->shadow_stack[L->shadow_top++] = TypedSlot(&loader_args[0].val, &loader_args[0].type);
        MultiValue res = call_function(L, loader, loader_args, 1, __FILE__, __LINE__);
        L->shadow_top -= 1;

        if (res.count > 1 && res[1].type == String)
            msgs.append(res[1].as_string(), res[1].string_len());

        LValue loader_fn = (res.count > 0) ? res[0] : LValue();
        if (loader_fn.type == Function) {
            //------------------ The loader runs with the env passed to require (nil = real _G)
            L->shadow_stack[L->shadow_top++] = TypedSlot(&loader_args[0].val, &loader_args[0].type);
            LTable *saved_G = L->_G;
            LValue saved_G_val(ValueType::Table, saved_G);
            L->shadow_stack[L->shadow_top++] = TypedSlot(&saved_G_val.val, &saved_G_val.type);
            if (env.type == Table)
                L->_G = static_cast<LTable *>(env.as_pointer());
            MultiValue mod = call_function(L, loader_fn, loader_args, 1, __FILE__, __LINE__);
            L->_G = saved_G;
            L->shadow_top -= 2;

            LValue result = (mod.count > 0) ? mod[0] : LValue(true);
            if (result.type == Nil)
                result = LValue(true);
            loaded->settable(mod_key, result);
            return MultiValue(result);
        }
    }

    std::string err = "module '";
    err.append(modname);
    err += "' not found:" + msgs;
    throw LRuntimeException(LValue(L->intern_string(err.data(), err.size())));
}

//------------------ luastd_package: registers the package library (package table + require + searchers)
void luastd_package(LState *L) {
    LValue pack_tbl = L->create_table();
    LTable *pack = static_cast<LTable *>(pack_tbl.as_pointer());

    LValue loaded_tbl = L->create_table();
    LValue preload_tbl = L->create_table();

    pack->settable(LValue(L->intern_string("loaded")), loaded_tbl);
    pack->settable(LValue(L->intern_string("preload")), preload_tbl);
    pack->settable(LValue(L->intern_string("path")), LValue(L->intern_string(CLX_LPATH_DEFAULT)));
    pack->settable(LValue(L->intern_string("cpath")), LValue(L->intern_string(CLX_CPATH_DEFAULT)));
    pack->settable(LValue(L->intern_string("config")), LValue(L->intern_string("/\n;\n?\n!\n-\n")));

    LValue searchers_tbl = L->create_table();
    LTable *searchers = static_cast<LTable *>(searchers_tbl.as_pointer());
    searchers->settable(LValue(static_cast<int64_t>(1)), L->create_closure(pkg_searcher_preload));
    searchers->settable(LValue(static_cast<int64_t>(2)), L->create_closure(pkg_searcher_luafile_aot));
    searchers->settable(LValue(static_cast<int64_t>(3)), L->create_closure(pkg_searcher_c));
    pack->settable(LValue(L->intern_string("searchers")), searchers_tbl);

    pack->settable(LValue(L->intern_string("searchpath")), L->create_closure(pkg_searchpath));

    set_global(L, "package", pack_tbl);

    LValue require_func = L->create_closure(pack_require);
    set_global(L, "require", require_func);

    static_cast<LTable *>(loaded_tbl.as_pointer())->settable(LValue(L->intern_string("package")), pack_tbl);
}

//------------------ register_static_preload: registers a static preload loader for a module
void register_static_preload(LState *L, const char *name, LValue (*open_func)(LState *)) {
    LValue pack_val = get_global(L, "package");
    if (pack_val.type != Table)
        return;

    LTable *pack = static_cast<LTable *>(pack_val.as_pointer());
    LValue preload_ptr = pack->gettable(LValue(L->intern_string("preload")));
    if (preload_ptr.type != Table)
        return;

    LTable *preload = static_cast<LTable *>(preload_ptr.as_pointer());

    LValue func_obj = LValue(L->create_closure(
        [open_func](LState *L, const LValue *args, size_t count) -> MultiValue { return MultiValue(open_func(L)); }));

    preload->settable(LValue(L->intern_string(name)), func_obj);
}

}
