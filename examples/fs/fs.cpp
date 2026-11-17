// fs.cpp — a small file-system module for clx, built on C++ std::filesystem.
//
//   local fs = require "fs"
//   fs.write("hello.txt", "Hello!")
//   print(fs.read("hello.txt"))          --> Hello!
//
// build.sh (build.bat on Windows) builds demo.lua, a `require "fs"` chunk with the
// module linked in as a native archive.
// The STEP comments below are in the order the file builds up, from the simplest call to objects.

#include "clx.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace lua  = clx::lua;
namespace stdfs = std::filesystem;

// Our functions live in their own namespace so that names like `remove`, `read` or `rename`
// can never clash with functions of the C library.
namespace fsmod {

//==========================================================================================
// STEP 1 — Two small helpers, then our first functions
//==========================================================================================

// Lua strings are UTF-8. These helpers turn a Lua string into a std::filesystem::path (and back),
// so file names with accents or non-Latin letters also work on Windows.
static stdfs::path to_path(std::string_view text) {
    return stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

static std::string to_text(const stdfs::path& path) {
    std::u8string u = path.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// fs.exists(path) -> true / false
static bool exists(std::string path) {
    std::error_code ec;                       // std::filesystem reports problems here instead of throwing
    return stdfs::exists(to_path(path), ec);
}

static bool is_file(std::string path) {
    std::error_code ec;
    return stdfs::is_regular_file(to_path(path), ec);
}

static bool is_dir(std::string path) {
    std::error_code ec;
    return stdfs::is_directory(to_path(path), ec);
}

static std::string cwd() {
    std::error_code ec;
    return to_text(stdfs::current_path(ec));
}

static std::string join(std::string a, std::string b) {
    return to_text(to_path(a) / to_path(b));
}

//==========================================================================================
// STEP 2 — Functions that can fail: return  nil, "what went wrong"
//==========================================================================================

// The usual Lua way to report a problem is to return nil plus a message.
static lua::Results fail(const std::string& why) {
    return lua::Results({ lua::Value(lua::nil), lua::Value(why) });
}

// And on success we return the result.
static lua::Results ok(lua::Value result = true) {
    return lua::Results({ std::move(result) });
}

// fs.size(path) -> number of bytes, or nil, message
static lua::Results size(std::string path) {
    std::error_code ec;
    auto bytes = stdfs::file_size(to_path(path), ec);
    if (ec)
        return fail(path + ": " + ec.message());
    return ok(static_cast<int64_t>(bytes));
}

// fs.read(path) -> the whole file as a string, or nil, message
static lua::Results read(std::string path) {
    std::error_code ec;
    if (stdfs::is_directory(to_path(path), ec))
        return fail(path + ": is a directory");
    std::ifstream in(to_path(path), std::ios::binary);
    if (!in)
        return fail(path + ": cannot open file for reading");
    try {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return ok(text);
    } catch (const std::exception& e) {
        return fail(path + ": " + e.what());
    }
}

static lua::Results save(const std::string& path, std::string_view text, std::ios::openmode mode) {
    std::ofstream out(to_path(path), std::ios::binary | mode);
    if (!out)
        return fail(path + ": cannot open file for writing");
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out)
        return fail(path + ": write failed");
    return ok();
}

// fs.write(path, text) -> true, or nil, message      (replaces the file)
static lua::Results write(std::string path, std::string_view text) {
    return save(path, text, std::ios::trunc);
}

// fs.append(path, text) -> true, or nil, message     (adds to the end of the file)
static lua::Results append(std::string path, std::string_view text) {
    return save(path, text, std::ios::app);
}

// fs.mkdir(path) -> true, or nil, message            (also creates missing parent folders)
static lua::Results mkdir(std::string path) {
    std::error_code ec;
    stdfs::create_directories(to_path(path), ec);
    if (ec)
        return fail(path + ": " + ec.message());
    return ok();
}

// fs.remove(path [, recursive]) -> how many items were deleted, or nil, message
// `std::optional<bool>` means "this argument may be left out".
static lua::Results remove(std::string path, std::optional<bool> recursive) {
    std::error_code ec;
    auto target = to_path(path);
    std::uintmax_t count = recursive.value_or(false) ? stdfs::remove_all(target, ec)
                                                     : (stdfs::remove(target, ec) ? 1 : 0);
    if (ec)
        return fail(path + ": " + ec.message());
    return ok(static_cast<int64_t>(count));
}

// fs.copy(from, to [, overwrite]) -> true, or nil, message
static lua::Results copy(std::string from, std::string to, std::optional<bool> overwrite) {
    std::error_code ec;
    auto options = stdfs::copy_options::recursive;
    if (overwrite.value_or(false))
        options |= stdfs::copy_options::overwrite_existing;
    stdfs::copy(to_path(from), to_path(to), options, ec);
    if (ec)
        return fail(from + ": " + ec.message());
    return ok();
}

// fs.rename(from, to) -> true, or nil, message
static lua::Results rename(std::string from, std::string to) {
    std::error_code ec;
    stdfs::rename(to_path(from), to_path(to), ec);
    if (ec)
        return fail(from + ": " + ec.message());
    return ok();
}

//==========================================================================================
// STEP 3 — Building tables for Lua
//==========================================================================================

// fs.list(folder [, recursive]) -> sorted table of names, or nil, message
static lua::Results list(std::string folder, std::optional<bool> recursive) {
    std::error_code ec;
    auto root = to_path(folder);
    lua::Table names;                                         // a brand new, empty Lua table

    if (recursive.value_or(false)) {
        for (stdfs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            names.push_back(to_text(it->path().lexically_relative(root)));
    } else {
        for (stdfs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            names.push_back(to_text(it->path().filename()));
    }

    if (ec)
        return fail(folder + ": " + ec.message());
    names.sort();                                             // Lua's table.sort, as a C++ method
    return ok(names);
}

static const char* kind_of(const stdfs::file_status& status) {
    if (stdfs::is_directory(status))    return "dir";
    if (stdfs::is_regular_file(status)) return "file";
    return "other";
}

// fs.info(path) -> { name = "...", kind = "file"|"dir"|"other", size = 123, modified = 1700000000 }
static lua::Results info(std::string path) {
    std::error_code ec;
    auto target = to_path(path);
    auto status = stdfs::status(target, ec);
    if (ec || !stdfs::exists(status))
        return fail(path + ": no such file or directory");

    // Build a table the same way you would write it in Lua:  { name = ..., kind = ... }
    lua::Table result{
        { "name", to_text(target.filename()) },
        { "kind", kind_of(status) },
    };

    // ...and add more fields afterwards with  table["field"] = value
    if (stdfs::is_regular_file(status))
        result["size"] = static_cast<int64_t>(stdfs::file_size(target, ec));

    auto stamp = stdfs::last_write_time(target, ec);
    if (!ec) {
        auto system_time = std::chrono::clock_cast<std::chrono::system_clock>(stamp);
        result["modified"] = std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
    }
    return ok(result);
}

//==========================================================================================
// STEP 4 — Letting Lua give you a function to call
//==========================================================================================

// fs.each(folder, function(name, kind) ... end) -> number of entries visited
// The callback may return false to stop early.
static lua::Results each(std::string folder, lua::Function callback) {
    std::error_code ec;
    int64_t visited = 0;

    for (stdfs::directory_iterator it(to_path(folder), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ignored;
        std::string name = to_text(it->path().filename());
        const char* kind = kind_of(it->status(ignored));

        lua::Value answer = callback(name, kind);             // call the Lua function
        ++visited;

        if (!answer.is_nil() && !answer.truthy())             // it returned `false`
            break;
    }

    if (ec)
        return fail(folder + ": " + ec.message());
    return ok(visited);
}

//==========================================================================================
// STEP 5 — A Path object that Lua can hold:  fs.path("a") / "b" / "c.txt"
//==========================================================================================

struct Path {
    stdfs::path value;

    explicit Path(std::string text) : value(to_path(text)) {}
    explicit Path(stdfs::path p) : value(std::move(p)) {}

    std::string text() const      { return to_text(value); }
    std::string filename() const  { return to_text(value.filename()); }
    std::string stem() const      { return to_text(value.stem()); }
    std::string extension() const { return to_text(value.extension()); }
    lua::UserData parent() const  { return lua::UserData::create<Path>(value.parent_path()); }
    bool exists() const           { std::error_code ec; return stdfs::exists(value, ec); }
};

static void add_path_object(lua::Table& module) {
    lua::Class<Path>("fs.Path")
        .constructor<std::string>(module, "path")      // fs.path("some/where")
        .method("filename",  &Path::filename)          // p:filename()
        .method("stem",      &Path::stem)
        .method("extension", &Path::extension)
        .method("parent",    &Path::parent)
        .method("exists",    &Path::exists)
        .function("__tostring", [](const lua::UserData& p) {
            return p.as_ref<Path>(1).text();           // print(p)
        })
        .function("__eq", [](const lua::UserData& a, const lua::UserData& b) {
            return a.as_ref<Path>(1).value == b.as_ref<Path>(2).value;     // p1 == p2
        })
        .function("__div", [](const lua::UserData& left, const lua::Value& right) {
            // p / "name"   or   p / otherPath
            stdfs::path tail = right.is_string() ? to_path(right.as<std::string>())
                                                 : right.as<lua::UserData>().as_ref<Path>(2).value;
            return lua::UserData::create<Path>(left.as_ref<Path>(1).value / tail);
        });
}

} // namespace fsmod

//==========================================================================================
// STEP 6 — Put everything in one table, and let `require "fs"` find it
//==========================================================================================

lua::Table open_fs() {
    lua::Table fs;

    fs["exists"] = fsmod::exists;
    fs["is_file"] = fsmod::is_file;
    fs["is_dir"]  = fsmod::is_dir;
    fs["cwd"]     = fsmod::cwd;
    fs["join"]    = fsmod::join;

    fs["size"]   = fsmod::size;
    fs["read"]   = fsmod::read;
    fs["write"]  = fsmod::write;
    fs["append"] = fsmod::append;
    fs["mkdir"]  = fsmod::mkdir;
    fs["remove"] = fsmod::remove;
    fs["copy"]   = fsmod::copy;
    fs["rename"] = fsmod::rename;

    fs["list"] = fsmod::list;
    fs["info"] = fsmod::info;
    fs["each"] = fsmod::each;

    fsmod::add_path_object(fs);
    return fs;
}

void register_fs(lua::State& L) {
    L["package"]["preload"]["fs"] = [](std::string_view /*module name*/) { return open_fs(); };
}

//------------------ Module opener: `clx demo.lua --modules fs` looks for this symbol in fs.a
CLX_API clx::LValue luaopen_fs(clx::LState* L) {
    lua::Scope scope(L);
    return open_fs().raw();
}
