// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  clx.cpp · CLX Compiler Driver              │
// └─────────────────────────────────────────────┘

#ifdef _WIN32
#include <windows.h>
#endif

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "codegen/codegen.h"
#include "optimizer/optimizer.h"
#include "syntax/parser.h"

namespace fs = std::filesystem;

#ifndef CLX_VERSION
#error "CLX_VERSION not defined — rebuild with CMake"
#endif

std::vector<std::string> precompiled_modules;

static bool dynamic_loading_enabled = false;

//------------------ ENUM: BuildMode - output mode (executable binary, object file, or static library)
enum class BuildMode { Executable,
    Object,
    Static };

//------------------ STRUCT: Compiler - holds C++ compiler name and command
struct Compiler {
    std::string name;
    std::string cmd;
};

//------------------ clx install paths (embedded from CMake/GNUInstallDirs)

static fs::path clx_install_prefix()
{
#ifdef CLX_INSTALL_PREFIX
    if (CLX_INSTALL_PREFIX[0] == '\0')
        return {};
    return fs::absolute(fs::path(CLX_INSTALL_PREFIX));
#else
    return {};
#endif
}

static fs::path clx_install_libdir()
{
#ifdef CLX_INSTALL_LIBDIR
    if (CLX_INSTALL_LIBDIR[0] == '\0')
        return {};
    return fs::path(CLX_INSTALL_LIBDIR);
#else
    return {};
#endif
}

static fs::path clx_install_includedir()
{
#ifdef CLX_INSTALL_INCLUDEDIR
    if (CLX_INSTALL_INCLUDEDIR[0] == '\0')
        return {};
    return fs::path(CLX_INSTALL_INCLUDEDIR);
#else
    return {};
#endif
}

//------------------ clx lib roots - candidate root dirs that contain clx's own libraries and native modules

static std::vector<fs::path> clx_lib_roots(const fs::path& exe_dir, const fs::path& build_root)
{
    std::vector<fs::path> roots;

    roots.push_back(build_root / "build");
    roots.push_back(build_root / "lib");
    roots.push_back(build_root / "lib64");

    auto pref = clx_install_prefix();
    auto libdir = clx_install_libdir();
    if (!pref.empty()) {
        if (libdir.is_absolute())
            roots.push_back(libdir);
        else
            roots.push_back(libdir.empty() ? pref / "lib" : pref / libdir);
    }

    return roots;
}

//------------------ ModuleLinkKind: how a precompiled module archive must be declared and invoked
enum class ModuleLinkKind { Cpp,
    C,
    CPrefixed };

//------------------ clx_slurp_file: raw bytes of a file, empty when unreadable
static std::string clx_slurp_file(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open())
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

//------------------ clx_archive_has_c_symbol: NUL-delimited C symbol present in an archive string table
static bool clx_archive_has_c_symbol(const std::string& data, const std::string& sym)
{
    if (data.size() < sym.size() + 1)
        return false;
    size_t pos = 0;
    while ((pos = data.find(sym, pos)) != std::string::npos) {
        char prev = (pos == 0) ? '\0' : data[pos - 1];
        bool leading = (pos == 0) || (prev == '\0') || (prev == '_');
        if (leading && data[pos + sym.size()] == '\0')
            return true;
        ++pos;
    }
    return false;
}

//------------------ clx_mod_symbol: C identifier for a require-name, dots become underscores like stock luaopen_a_b_c
static std::string clx_mod_symbol(const std::string& mod)
{
    std::string s = mod;
    for (auto& c : s)
        if (c == '.')
            c = '_';
    return s;
}

//------------------ clx_scan_module_kind: classify an archive as C++, C, or a clx-renamed C opener
static ModuleLinkKind clx_scan_module_kind(const fs::path& lib, const std::string& mod)
{
    std::string data = clx_slurp_file(lib);
    if (data.empty())
        return ModuleLinkKind::Cpp;

    std::string sym = clx_mod_symbol(mod);
    std::string raw = "luaopen_" + sym;
    if (data.find("_Z" + std::to_string(raw.size()) + raw) != std::string::npos)
        return ModuleLinkKind::Cpp;
    if (data.find("?luaopen_" + sym + "@@") != std::string::npos)
        return ModuleLinkKind::Cpp;
    if (clx_archive_has_c_symbol(data, "clx_luaopen_" + sym))
        return ModuleLinkKind::CPrefixed;
    if (clx_archive_has_c_symbol(data, raw))
        return ModuleLinkKind::C;
    return ModuleLinkKind::Cpp;
}

//------------------ CLX: execute - runs a shell command, captures stdout and exit code
std::string execute(const std::string& cmd, int& out_code)
{
    std::string result;
#ifdef _WIN32
    auto run_one = [&](const std::string& one_cmd) -> int {
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
        HANDLE h_read, h_write;
        if (!CreatePipe(&h_read, &h_write, &sa, 0))
            return -1;
        SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si = { sizeof(STARTUPINFOA) };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = h_write;
        si.hStdError = h_write;
        si.hStdInput = NULL;

        std::vector<char> cmd_buf(one_cmd.begin(), one_cmd.end());
        cmd_buf.push_back(0);

        PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
        CloseHandle(h_write);

        if (!ok) {
            CloseHandle(h_read);
            return -1;
        }

        std::array<char, 4096> buffer;
        DWORD n;
        while (ReadFile(h_read, buffer.data(), buffer.size() - 1, &n, NULL) && n > 0) {
            buffer[n] = 0;
            result += buffer.data();
        }
        CloseHandle(h_read);

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return static_cast<int>(code);
    };

    size_t pos;
    out_code = 0;
    std::string remaining = cmd;
    while ((pos = remaining.find(" && ")) != std::string::npos) {
        out_code = run_one(remaining.substr(0, pos));
        if (out_code != 0)
            return result;
        remaining = remaining.substr(pos + 4);
    }
    out_code = run_one(remaining);
#else
    auto pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return "";
    std::array<char, 128> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    int status = pclose(pipe);
    out_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#endif
    return result;
}

#ifdef _WIN32
//------------------ CLX: find_on_path - true if exe_name exists in one of the PATH directories
static bool find_on_path(const char* exe_name)
{
    const char* path = std::getenv("PATH");
    if (!path || !*path)
        return false;
    std::string entries(path);
    size_t start = 0;
    while (start <= entries.size()) {
        size_t end = entries.find(';', start);
        if (end == std::string::npos)
            end = entries.size();
        std::string dir = entries.substr(start, end - start);
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"')
            dir = dir.substr(1, dir.size() - 2);
        if (dir.empty()) {
            start = end + 1;
            continue;
        }
        if (dir.back() != '\\' && dir.back() != '/')
            dir += '\\';
        std::error_code ec;
        if (fs::exists(dir + exe_name, ec))
            return true;
        start = end + 1;
    }
    return false;
}
#endif

//------------------ CLX: cxx_dialect - flag dialect of a compiler executable: "MSVC", "ClangCL" or the fallback
static std::string cxx_dialect(const std::string& exe, const std::string& fallback)
{
    std::string base = fs::path(exe).filename().stem().string();
    for (auto& c : base)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (base == "cl")
        return "MSVC";
    if (base == "clang-cl")
        return "ClangCL";
    return fallback;
}

//------------------ CLX: get_compiler - resolve the C++ compiler: CLX_CXX environment
Compiler get_compiler()
{
#ifndef CLX_DEFAULT_CXX
#error "CLX_DEFAULT_CXX not defined — rebuild with CMake"
#endif
#ifndef CLX_DEFAULT_CXX_NAME
#error "CLX_DEFAULT_CXX_NAME not defined — rebuild with CMake"
#endif
    if (const char* env = std::getenv("CLX_CXX"); env && *env)
        return { cxx_dialect(env, "Clang"), std::string(env) };
    fs::path embedded(CLX_DEFAULT_CXX);
#ifdef _WIN32
    std::string embedded_name = embedded.filename().string();
    for (auto& c : embedded_name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (embedded_name == "cl" || embedded_name == "cl.exe") {
        const char* include_env = std::getenv("INCLUDE");
        if (find_on_path("cl.exe") || !include_env || !*include_env)
            return { "MSVC", "cl" };
    }
#endif
    if (embedded.is_absolute() && !fs::exists(embedded)) {
#ifdef _WIN32
        bool embedded_clang_cl = embedded_name.rfind("clang-cl", 0) == 0;
        const char* candidates[2];
        if (embedded_clang_cl) {
            candidates[0] = "clang-cl";
            candidates[1] = "cl";
        } else {
            candidates[0] = "cl";
            candidates[1] = "clang-cl";
        }
        for (const char* candidate : candidates) {
            std::string probe = std::string(candidate) + ".exe";
            if (find_on_path(probe.c_str()))
                return { cxx_dialect(candidate, "MSVC"), candidate };
        }
        return { cxx_dialect(candidates[0], "MSVC"), candidates[0] };
#else
        return { CLX_DEFAULT_CXX_NAME, "c++" };
#endif
    }
    return { cxx_dialect(CLX_DEFAULT_CXX, CLX_DEFAULT_CXX_NAME), CLX_DEFAULT_CXX };
}

//------------------ CLX: print_help - displays usage information
void print_help()
{
    std::cout << "Usage: clx [options] <file.lua> [<compiler-options>]\n\n"
              << "clx Compiler Options:\n"
              << "  -o, --output <name>   Specify output file name\n"
              << "  --executable          Build executable (default)\n"
              << "  --object              Compile to object file (.o/.obj)\n"
              << "  --static              Compile to static library (.a/.lib)\n"
              << "  --debug               Enable debug symbols\n"
              << "  --size                Optimize for size (default)\n"
              << "  --fast                Optimize for speed\n"
              << "  --cpp                 Generate C++ source file and exit\n"               << "  --minimal             Exclude non-essential Lua modules; keeps base + package + string\n"
              << "  --dynamic             Link the embedded Lua 5.5 VM (load/loadfile/dofile)\n"
              << "  --version             Print version and exit\n"
              << "  --cxx                 Print the C++ compiler dialect and path clx drives\n"
              << "  --help                Display this help message\n\n"
              << "Compiler Options:\n"
              << "  Any options starting with '-' not recognized by clx are passed to the C++ compiler.\n"
              << "  If no compiler options are provided, default optimization flags are used.\n"
              << "  Example: clx file.lua -O3 -march=native\n";
}

//------------------ CLX: main - entry point, parses CLI arguments, compiles Lua to C++, links output
int main(int argc, char* argv[])
{
    if (argc < 2) {
        print_help();
        return 1;
    }

    std::vector<std::string> input_files;
    std::vector<std::string> cc_options;
    BuildMode mode = BuildMode::Executable;
    std::string custom_output_name = "";
    bool debug_mode = false;
    bool size_mode = true;
    bool emit_cpp = false;
    bool minimal_active = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "--version") {
            std::cout << "clx " CLX_VERSION "\nMIT License - Copyright (c) 2026 Tine Samir\n";
            return 0;
        } else if (arg == "--cxx") {
            Compiler cxx = get_compiler();
            std::cout << cxx.name;
            if (!cxx.cmd.empty())
                std::cout << " (" << cxx.cmd << ")";
            std::cout << "\n";
            return 0;
        } else if (arg == "--output" || arg == "-o") {
            if (i + 1 < argc)
                custom_output_name = argv[++i];
        } else if (arg == "--minimal") {
            minimal_active = true;
        } else if (arg == "--dynamic") {
            dynamic_loading_enabled = true;
        } else if (arg == "--modules") {
            if (i + 1 < argc) {
                std::string mods = argv[++i];
                size_t pos = 0;
                while ((pos = mods.find(',')) != std::string::npos) {
                    precompiled_modules.push_back(mods.substr(0, pos));
                    mods.erase(0, pos + 1);
                }
                if (!mods.empty()) {
                    precompiled_modules.push_back(mods);
                }
                {
                    std::vector<std::string> seen;
                    for (auto& m : precompiled_modules) {
                        if (std::find(seen.begin(), seen.end(), m) == seen.end())
                            seen.push_back(m);
                    }
                    precompiled_modules.swap(seen);
                }
            }
        } else if (arg == "--object") {
            mode = BuildMode::Object;
        } else if (arg == "--static") {
            mode = BuildMode::Static;
        } else if (arg == "--executable") {
            mode = BuildMode::Executable;
        } else if (arg == "--debug") {
            debug_mode = true;
        } else if (arg == "--fast") {
            size_mode = false;
        } else if (arg == "--size") {
            size_mode = true;
        } else if (arg == "--cpp") {
            emit_cpp = true;
        } else if (arg.rfind("-", 0) == 0
#ifdef _WIN32
            || arg.rfind("/", 0) == 0
#endif
        ) {
            cc_options.push_back(arg);
        } else {
            input_files.push_back(arg);
        }
    }

    if (debug_mode)
        size_mode = false;

    bool dce_mode = (mode == BuildMode::Executable && !debug_mode);

    if (input_files.empty()) {
        std::cerr << "Error: No input file specified.\n";
        return 1;
    }

    Compiler cc = get_compiler();
    if (cc.cmd.empty()) {
        std::cerr << "Error: No C++ compiler found.\n";
        return 2;
    }

    std::string cc_compile_str = "";
    std::string cc_link_str = "";
    bool link_seen = false;
    for (const auto& opt : cc_options) {
        if (opt == "/link") {
            link_seen = true;
            continue;
        }
        if (link_seen)
            cc_link_str += " " + opt;
        else
            cc_compile_str += " " + opt;
    }
    if (!link_seen) {
        cc_link_str = cc_compile_str;
        cc_compile_str = "";
    }
    std::string cc_options_str = cc_compile_str + cc_link_str;

    const bool is_clang_cl = cc.name == "ClangCL";
    const bool msvc_dialect = is_clang_cl || cc.name == "MSVC";

    std::string opt_flags;
    std::string msvc_opt_flags;
    std::string gcc_dce_cl = dce_mode ? " -ffunction-sections -fdata-sections" : "";
    std::string gcc_dce_link = dce_mode
#ifdef __APPLE__
        ? " -Wl,-dead_strip"
#else
        ? " -Wl,--gc-sections"
#endif
        : "";
    std::string gcc_strip_link =
#ifndef __APPLE__
        debug_mode ? "" : " -s"
#else

        debug_mode ? "" : " -Wl,-x"
#endif
        ;
    std::string msvc_dce_cl = dce_mode ? " /Gy" : "";
    std::string msvc_dce_link;
    if (dce_mode)
        msvc_dce_link = is_clang_cl ? " /link /OPT:REF /OPT:ICF" : " /link /LTCG /OPT:REF /OPT:ICF";

    std::string msvc_stack;
    std::string gcc_stack;
    if (mode == BuildMode::Executable) {
        std::string lower_opts = cc_options_str;
        for (auto& c : lower_opts)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool user_stack = lower_opts.find("/stack") != std::string::npos
            || lower_opts.find("--stack") != std::string::npos;
        if (!user_stack) {
            msvc_stack = (dce_mode || link_seen) ? " /STACK:0x800000" : " /link /STACK:0x800000";
            gcc_stack = " -Wl,--stack,8388608";
        }
    }

    std::string msvc_wpo = is_clang_cl ? "" : " /GL";
    if (debug_mode) {
        opt_flags = "-O0 -g";
        msvc_opt_flags = "/Od /Zi /MDd /EHsc /utf-8";
    } else if (size_mode) {
        opt_flags = (cc.name == "GNU")
            ? "-O1 -foptimize-sibling-calls -fno-inline-functions -fvisibility=hidden"
            : "-Os -fno-inline-functions -fvisibility=hidden";
        msvc_opt_flags = "/O1 /Ob0" + msvc_wpo + " /GR- /MD /EHsc /GS- /fp:fast /Gw /Gy /utf-8";
    } else {
#ifdef _WIN32
        opt_flags = "-O3 -fvisibility=hidden";
#else
        opt_flags = "-O3 -flto=auto -fvisibility=hidden";
#endif
        msvc_opt_flags = "/O2 /Ot" + msvc_wpo + " /GR- /MD /EHsc /GS- /fp:fast /Gw /Gy /utf-8";
    }

#ifndef CLX_ARCH_GCC_FLAG
#define CLX_ARCH_GCC_FLAG ""
#endif
#ifndef CLX_ARCH_MSVC_FLAG
#define CLX_ARCH_MSVC_FLAG ""
#endif
    {
        std::string clx_arch_gcc = CLX_ARCH_GCC_FLAG;
        std::string clx_arch_msvc = CLX_ARCH_MSVC_FLAG;
        bool user_overrode_arch = cc_options_str.find("-mavx") != std::string::npos
            || cc_options_str.find("-msse") != std::string::npos || cc_options_str.find("-march=") != std::string::npos
            || cc_options_str.find("-mcpu=") != std::string::npos || cc_options_str.find("/arch:") != std::string::npos
            || cc_options_str.find("-arch") != std::string::npos;
        if (!user_overrode_arch) {
            if (!clx_arch_gcc.empty()) {
                opt_flags += " " + clx_arch_gcc;
            }
            if (!clx_arch_msvc.empty()) {
                msvc_opt_flags += " " + clx_arch_msvc;
            }
        }
    }

    std::string output_name
        = custom_output_name.empty() ? fs::path(input_files[0]).stem().string() : custom_output_name;

    if (fs::path(output_name).extension() == ".exe") {
        output_name = fs::path(output_name).replace_extension().string();
    }

    std::vector<std::string> cpp_files;

    //------------------ Install roots — computed before codegen so module archives can be classified
    fs::path exe_dir;
#ifdef _WIN32
    char path_buffer[MAX_PATH];
    GetModuleFileNameA(NULL, path_buffer, MAX_PATH);
    exe_dir = fs::path(path_buffer).parent_path();
#else
    {
        char buf[4096];
        ssize_t len = -1;
#ifdef __APPLE__
        uint32_t bufsize = sizeof(buf);
        if (_NSGetExecutablePath(buf, &bufsize) == 0) {
            char real[4096];
            if (realpath(buf, real)) {
                len = static_cast<ssize_t>(strlen(real));
                memcpy(buf, real, static_cast<size_t>(len) + 1);
            } else {
                len = static_cast<ssize_t>(bufsize > sizeof(buf) ? sizeof(buf) : bufsize);
            }
        }
#else
        len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
#endif
        if (len != -1) {
            buf[len] = '\0';
            exe_dir = fs::path(buf).parent_path();
        } else {
            exe_dir = fs::absolute(fs::path(argv[0])).parent_path();
        }
    }
#endif
    fs::path build_root = exe_dir.parent_path();
    auto lib_roots = clx_lib_roots(exe_dir, build_root);

    std::vector<fs::path> mod_search_dirs;
    mod_search_dirs.push_back(fs::current_path());
    for (const auto& root : lib_roots) {
        fs::path p = root / "clx";
        if (fs::exists(p))
            mod_search_dirs.push_back(p);
    }
    {
        fs::path p = build_root / "lib" / "clx";
        if (fs::exists(p))
            mod_search_dirs.push_back(p);
    }
    for (const auto& opt : cc_options) {
        if (opt.size() > 2 && opt[0] == '-' && opt[1] == 'L') {
            std::string dir = opt.substr(2);
            if (!dir.empty() && fs::is_directory(dir))
                mod_search_dirs.push_back(fs::path(dir));
        }
    }

    //------------------ Module archives: locate once, then classify C++ vs C entry points
    std::map<std::string, fs::path> module_archives;
    std::map<std::string, ModuleLinkKind> module_kinds;
    for (const auto& mod : precompiled_modules) {
        if (module_archives.count(mod))
            continue;
        std::string target_lib = mod + (msvc_dialect ? ".lib" : ".a");
        fs::path resolved;
        for (const auto& dir : mod_search_dirs) {
            if (fs::exists(dir / target_lib)) {
                resolved = dir / target_lib;
                break;
            }
        }
        module_archives[mod] = resolved;
        module_kinds[mod] = clx_scan_module_kind(resolved.empty() ? fs::path(target_lib) : resolved, mod);
    }

    for (const auto& input_file : input_files) {
        std::ifstream t(input_file);
        if (!t.is_open()) {
            std::cerr << "Error: Cannot open input file " << input_file << "\n";
            return 1;
        }
        std::string source((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());

        if (source.size() >= 3 && (unsigned char)source[0] == 0xEF && (unsigned char)source[1] == 0xBB
            && (unsigned char)source[2] == 0xBF) {
            source.erase(0, 3);
        }

        fs::path p_input(input_file);
        std::string generic_input_path = p_input.generic_string();

        clx::ASTContext ctx;
        clx::Parser parser(source.c_str(), generic_input_path.c_str(), ctx);
        uint32_t root = 0xFFFFFFFF;
        try {
            root = parser.parse();
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }

        std::string module_name = p_input.stem().string();

        clx::AnalysisState analysis;
        clx::Optimizer optimizer(ctx, analysis);
        optimizer.run(ctx, root);

        fs::path cpp_file;

        if (emit_cpp) {
            fs::path cpp_dir = fs::path(output_name + ".cpp").parent_path();
            if (!cpp_dir.empty() && !fs::exists(cpp_dir))
                fs::create_directories(cpp_dir);
            cpp_file = output_name + ".cpp";
        } else {
            cpp_file = fs::temp_directory_path() / (module_name + "_tmp.cpp");
        }

        clx::CodeEmitter emitter(ctx, cpp_file.string().c_str(), analysis);
        try {
            emitter.emit(root, module_name);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
        cpp_files.push_back(cpp_file.string());
    }
    if (mode == BuildMode::Executable) {
        std::string main_module = fs::path(input_files[0]).stem().string();
        std::ofstream appender(cpp_files[0], std::ios::app);

        for (const auto& file : input_files) {
            std::string mod = fs::path(file).stem().string();
            appender << "\nextern clx::LValue luaopen_" << mod << "(clx::LState* L);\n";
        }
        for (const auto& file : input_files) {
            std::string m = fs::path(file).stem().string();
            auto hit = module_kinds.find(m);
            if (hit != module_kinds.end() && hit->second != ModuleLinkKind::Cpp) {
                std::cerr << "Error: input module \"" << m << "\" collides with the precompiled C module \"" << m << "\".\n";
                return 1;
            }
        }

        for (const auto& mod : precompiled_modules) {
            ModuleLinkKind kind = module_kinds.at(mod);
            if (kind == ModuleLinkKind::Cpp) {
                appender << "\nextern clx::LValue luaopen_" << clx_mod_symbol(mod) << "(clx::LState* L);\n";
            } else {
                std::string sym = (kind == ModuleLinkKind::CPrefixed) ? "clx_luaopen_" + clx_mod_symbol(mod) : "luaopen_" + clx_mod_symbol(mod);
                appender << "\nextern \"C\" int " << sym << "(struct lua_State*);\n";
                appender << "static clx::LValue clx_cmod_" << clx_mod_symbol(mod) << "(clx::LState* L) { return clx::luaapi_c_module_open(L, " << sym
                         << ", \"" << mod << "\"); }\n";
            }
        }

        if (dynamic_loading_enabled)
            appender << "extern \"C\" void clx_register_load_builtins(clx::LState *L);\n";
        appender << "int main(int argc, char* argv[]) {\n";
        appender << "    clx::LState* L = clx::open(argc, argv);\n";

        if (!minimal_active)
            appender << "    clx::openlibs(L);\n";
        else
            appender << "    clx::openlibs_minimal(L);\n";
        if (dynamic_loading_enabled && !minimal_active) {
            appender << "    // --dynamic enabled: link libclx_lua.a (POSIX) or clx_lua.lib (Windows)\n";
            appender << "    //   In-tree: build/clx_lua/libclx_lua.a (or build/clx_lua/clx_lua.lib)\n";
            appender << "    //   Installed: <libdir>/libclx_lua.a (or <ProgramFiles>/clx/lib/clx_lua.lib)\n";
            appender << "    clx_register_load_builtins(L);\n";
        }
        appender << "    try {\n";

        for (size_t i = 1; i < input_files.size(); ++i) {
            std::string mod = fs::path(input_files[i]).stem().string();
            std::string lua_mod = input_files[i];
            size_t dot = lua_mod.rfind('.');
            if (dot != std::string::npos)
                lua_mod = lua_mod.substr(0, dot);
            for (auto& c : lua_mod)
                if (c == '/' || c == '\\')
                    c = '.';
            appender << "        L->register_module(\"" << lua_mod << "\", luaopen_" << mod << ");\n";
            if (lua_mod != mod)
                appender << "        L->register_module(\"" << mod << "\", luaopen_" << mod << ");\n";
        }
        for (const auto& mod : precompiled_modules) {
            ModuleLinkKind kind = module_kinds.at(mod);
            appender << "        {\n";
            if (kind == ModuleLinkKind::Cpp) {
                appender << "            clx::LValue _m = luaopen_" << clx_mod_symbol(mod) << "(L);\n";
                appender << "            L->register_loaded_module(\"" << mod << "\", _m);\n";
                appender << "            L->register_module(\"" << mod << "\", luaopen_" << clx_mod_symbol(mod) << ");\n";
            } else {
                appender << "            clx::LValue _m = clx_cmod_" << clx_mod_symbol(mod) << "(L);\n";
                appender << "            L->register_loaded_module(\"" << mod << "\", _m);\n";
                appender << "            L->register_module(\"" << mod << "\", clx_cmod_" << clx_mod_symbol(mod) << ");\n";
            }
            appender << "        }\n";
        }

        appender << "        luaopen_" << main_module << "(L);\n";

        appender << "    } catch (const clx::LRuntimeException& e) {\n";
        appender << "        std::cerr << e.what() << \"\\n\";\n";
        appender << "        clx::close(L);\n";
        appender << "        return 1;\n";
        appender << "    } catch (const std::exception& e) {\n";
        appender << "        std::cerr << \"C++ Fatal Error: \" << e.what() << \"\\n\";\n";
        appender << "        clx::close(L);\n";
        appender << "        return 1;\n";
        appender << "    }\n";
        appender << "    clx::close(L);\n";
        appender << "    return 0;\n";
        appender << "}\n";
    }

    if (emit_cpp) {
        return 0;
    }

    std::string include_opt;
    std::string lib_link;
    std::string runtime_lib_dir;

    fs::path include_dir;
    if (!fs::exists(build_root / "include")) {
        auto inst_incl = clx_install_includedir();
        if (!inst_incl.empty()) {
            include_dir = inst_incl.is_absolute() ? inst_incl : clx_install_prefix() / inst_incl;
        }
    } else {
        include_dir = build_root / "include";
    }
    if (include_dir.empty() || !fs::exists(include_dir))
        include_dir = fs::current_path() / "include";

#ifdef _WIN32
    std::string include_path = fs::exists(include_dir) ? include_dir.string() : "include";
    if (msvc_dialect)
        include_opt = " /I\"" + include_path + "\"";
    else
        include_opt = " -I \"" + include_path + "\"";
#else
    include_opt = " -I " + (fs::exists(include_dir) ? include_dir.string() : "include");
#endif

    //------------------ Runtime library link line, from the same lib roots
    {
#ifdef _WIN32
        std::string lib_file;
        if (msvc_dialect)
            lib_file = size_mode ? "clx_size.lib" : "clx.lib";
        else
            lib_file = size_mode ? "libclx_size.a" : "libclx.a";
        fs::path lib_path;
        bool found = false;
        for (const auto& root : lib_roots) {
            lib_path = root / "Release" / lib_file;
            if (fs::exists(lib_path)) {
                found = true;
                break;
            }
            lib_path = root / lib_file;
            if (fs::exists(lib_path)) {
                found = true;
                break;
            }
        }
        if (!found) {
            lib_path = build_root / "lib" / lib_file;
            if (fs::exists(lib_path))
                found = true;
        }
        if (found)
            lib_link = " \"" + lib_path.string() + "\"";
        else
            lib_link = msvc_dialect ? " \"clx.lib\"" : (size_mode ? " -lclx_size" : " -lclx");
        runtime_lib_dir = found ? lib_path.parent_path().string() : std::string();
#else
        std::string lib_file = size_mode ? "libclx_size.a" : "libclx.a";
        std::string lib_dir;
        for (const auto& root : lib_roots) {
            if (fs::exists(root / lib_file)) {
                lib_dir = root.string();
                break;
            }
        }
        runtime_lib_dir = lib_dir;
#ifdef __APPLE__
        lib_link = lib_dir.empty() ? (size_mode ? " -lclx_size" : " -lclx")
                                   : " -L " + lib_dir + (size_mode ? " -lclx_size" : " -lclx");
#else
        lib_link = lib_dir.empty() ? " -l:" + lib_file : " -L " + lib_dir + " -l:" + lib_file;
#endif
#endif
    }

    //------------------ Lua 5.5 C API bridge archive, linked only when a precompiled C module needs it
    {
        bool need_capi = false;
        for (const auto& kind : module_kinds) {
            if (kind.second != ModuleLinkKind::Cpp) {
                need_capi = true;
                break;
            }
        }
        if (need_capi) {
#ifdef _WIN32
            std::string capi_file;
            if (msvc_dialect)
                capi_file = size_mode ? "clx_capi_size.lib" : "clx_capi.lib";
            else
                capi_file = size_mode ? "libclx_capi_size.a" : "libclx_capi.a";
            fs::path capi_path = fs::path(runtime_lib_dir) / capi_file;
            if (!fs::exists(capi_path))
                capi_path = build_root / "Release" / capi_file;
            if (!fs::exists(capi_path))
                capi_path = build_root / "lib" / capi_file;
            lib_link += " \"" + capi_path.string() + "\"";
#else
            std::string capi_file = size_mode ? "libclx_capi_size.a" : "libclx_capi.a";
            bool found = !runtime_lib_dir.empty() && fs::exists(fs::path(runtime_lib_dir) / capi_file);
#ifdef __APPLE__
            lib_link += found ? " -L " + runtime_lib_dir + (size_mode ? " -lclx_capi_size" : " -lclx_capi")
                              : (size_mode ? " -lclx_capi_size" : " -lclx_capi");
#else
            lib_link += found ? " -L " + runtime_lib_dir + " -l:" + capi_file : " -l:" + capi_file;
#endif
#endif
        }
    }

    std::string all_cpp_files = "";
    for (const auto& f : cpp_files) {
        all_cpp_files += "\"" + f + "\" ";
    }

    std::string obj_files;
    for (const auto& f : cpp_files) {
        obj_files += fs::path(f).stem().string() + ".o ";
    }

    for (const auto& mod : precompiled_modules) {
        std::string target_lib = mod + (msvc_dialect ? ".lib" : ".a");
        bool found = false;
        for (const auto& dir : mod_search_dirs) {
            fs::path full = dir / target_lib;
            if (fs::exists(full)) {
                if (dir == fs::current_path())
                    all_cpp_files += "\"" + target_lib + "\" ";
                else
                    all_cpp_files += "\"" + fs::absolute(full).string() + "\" ";
                found = true;
                break;
            }
        }
        if (!found) {
            all_cpp_files += "\"" + target_lib + "\" ";
        }
    }

    if (dynamic_loading_enabled) {
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)

        std::vector<std::string> lua_search_dirs;
        lua_search_dirs.push_back("deps/lua-5.5/src");
        lua_search_dirs.push_back((build_root / "build" / "clx_lua").string());
        for (const auto& opt : cc_options) {
            if (opt.size() > 2 && opt[0] == '-' && opt[1] == 'L') {
                std::string dir = opt.substr(2);
                if (!dir.empty())
                    lua_search_dirs.push_back(dir);
            }
        }
        for (const auto& dir : mod_search_dirs)
            lua_search_dirs.push_back(dir.string());
        for (const auto& root : lib_roots)
            lua_search_dirs.push_back(root.string());

        std::string found_bridge_lib;
        std::string found_bare_lua_lib;
        for (const auto& dir : lua_search_dirs) {
            fs::path p_bridge = fs::path(dir) / "libclx_lua.a";
            if (fs::exists(p_bridge) && found_bridge_lib.empty())
                found_bridge_lib = fs::absolute(p_bridge).string();
            fs::path p_bare = fs::path(dir) / "liblua.a";
            if (fs::exists(p_bare) && found_bare_lua_lib.empty())
                found_bare_lua_lib = fs::absolute(p_bare).string();
        }

        if (!found_bridge_lib.empty()) {
            lib_link += " \"" + found_bridge_lib + "\"";
        } else if (!found_bare_lua_lib.empty()) {

            std::cerr << "clx: --dynamic requires libclx_lua.a (vendored Lua 5.5 + "
                      << "clx bridge). Found a bare liblua.a at " << found_bare_lua_lib
                      << " but it does not define clx_register_load_builtins.\n"
                      << "Fix:\n"
                      << "  - Run `cmake --build build -j` to build clx_lua (libclx_lua.a), OR\n"
                      << "  - Run deps/fetch_lua.sh to fetch Lua 5.5 into deps/lua-5.5/src/, OR\n"
                      << "  - Configure clx with -DCLX_LUA_SOURCES_DIR=/path/to/lua-5.5/src, OR\n"
                      << "  - Drop --dynamic if you don't need load/loadfile/dofile.\n";
            return 1;
        } else {
            std::cerr << "clx: --dynamic requires libclx_lua.a (vendored Lua 5.5 + "
                      << "clx bridge). Could not find it in:\n";
            for (const auto& dir : lua_search_dirs)
                std::cerr << "  " << dir << "/\n";
            std::cerr << "Fix:\n"
                      << "  - Run `cmake --build build -j` to build clx_lua (libclx_lua.a), OR\n"
                      << "  - Run deps/fetch_lua.sh to fetch Lua 5.5 into deps/lua-5.5/src/, OR\n"
                      << "  - Configure clx with -DCLX_LUA_SOURCES_DIR=/path/to/lua-5.5/src, OR\n"
                      << "  - Drop --dynamic if you don't need load/loadfile/dofile.\n";
            return 1;
        }
#elif _WIN32

        std::vector<fs::path> lua_win_search_dirs;
        lua_win_search_dirs.push_back(fs::path(input_files[0]).parent_path());
        lua_win_search_dirs.push_back(build_root / "build" / "clx_lua");
        lua_win_search_dirs.push_back(build_root / "lib");
        for (const auto& dir : mod_search_dirs)
            lua_win_search_dirs.push_back(dir);
        for (const auto& root : lib_roots)
            lua_win_search_dirs.push_back(root);
        std::string found_bridge_lib;
        std::string bridge_lib_name = msvc_dialect ? "clx_lua.lib" : "libclx_lua.a";
        for (const auto& dir : lua_win_search_dirs) {
            fs::path p = dir / bridge_lib_name;
            if (fs::exists(p)) {
                found_bridge_lib = fs::absolute(p).string();
                break;
            }
        }
        if (found_bridge_lib.empty()) {
            std::cerr << "clx: --dynamic requires " << bridge_lib_name << " (vendored Lua 5.5 + "
                      << "clx bridge). Could not find it in:\n";
            for (const auto& dir : lua_win_search_dirs)
                std::cerr << "  " << dir.string() << "\n";
            std::cerr << "Fix:\n"
                      << "  - Run `cmake --build build` to build clx_lua, OR\n"
                      << "  - Drop --dynamic if you don't need load/loadfile/dofile.\n";
            return 1;
        }
        lib_link += " \"" + found_bridge_lib + "\" ";
#endif
    }

    std::string cmd;
    if (msvc_dialect) {
        std::string tmp_dir = fs::temp_directory_path().string();
        while (!tmp_dir.empty() && (tmp_dir.back() == '\\' || tmp_dir.back() == '/'))
            tmp_dir.pop_back();

        std::string librarian = "lib";
#ifdef _WIN32
        if (is_clang_cl && !find_on_path("lib.exe") && find_on_path("llvm-lib.exe"))
            librarian = "llvm-lib";
#endif

        std::string fo_arg = " /Fo\"" + tmp_dir + "\\\\\"";
        std::string out_ext;
        if (mode != BuildMode::Object && mode != BuildMode::Static) {
            out_ext = ".exe";
            if (output_name.size() >= 4 && output_name.compare(output_name.size() - 4, 4, ".exe") == 0)
                out_ext = "";
        }
        std::string fe_arg = out_ext.empty() ? "" : " /Fe\"" + output_name + out_ext + "\"";

        std::string msvc_obj_files;
        for (const auto& f : cpp_files) {
            msvc_obj_files += "\"" + tmp_dir + "\\" + fs::path(f).stem().string() + ".obj\" ";
        }

        if (mode == BuildMode::Object) {
            cmd = cc.cmd + " /nologo /c " + msvc_opt_flags + msvc_dce_cl + " /std:c++20" + include_opt + " "
                + all_cpp_files + cc_compile_str + fo_arg;
        } else if (mode == BuildMode::Static) {
            std::string lib_out = output_name;
            if (!(lib_out.size() >= 4 && lib_out.compare(lib_out.size() - 4, 4, ".lib") == 0))
                lib_out += ".lib";
            cmd = cc.cmd + " /nologo /c " + msvc_opt_flags + msvc_dce_cl + " /std:c++20" + include_opt + " "
                + all_cpp_files + cc_compile_str + fo_arg + " && " + librarian + " /nologo /OUT:\"" + lib_out
                + "\" " + msvc_obj_files;
        } else {
            cmd = cc.cmd + " /nologo " + msvc_opt_flags + msvc_dce_cl + " /std:c++20" + include_opt + cc_compile_str
                + " " + all_cpp_files + fo_arg + lib_link + fe_arg + " " + msvc_dce_link + cc_link_str + msvc_stack;
        }
    } else {
        if (mode == BuildMode::Object)
            cmd = cc.cmd + " -c " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " -fPIC " + all_cpp_files
                + " -o \""
                + (fs::temp_directory_path() / (fs::path(input_files[0]).stem().string() + "_tmp.o")).string() + "\""
                + cc_options_str;
        else if (mode == BuildMode::Static) {
            std::string ext = ".a";
            if (output_name.size() >= ext.size()
                && output_name.compare(output_name.size() - ext.size(), ext.size(), ext) == 0)
                cmd = cc.cmd + " -c " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " " + all_cpp_files
                    + cc_options_str + " && ar rcs " + output_name + " " + obj_files;
            else
                cmd = cc.cmd + " -c " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " " + all_cpp_files
                    + cc_options_str + " && ar rcs " + output_name + ".a " + obj_files;
        } else {
#ifdef _WIN32
            std::string ext = ".exe";
            if (output_name.size() >= ext.size()
                && output_name.compare(output_name.size() - ext.size(), ext.size(), ext) == 0)
                cmd = cc.cmd + " " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " " + all_cpp_files
                    + cc_options_str + lib_link + gcc_stack + gcc_dce_link + gcc_strip_link + " -o " + output_name;
            else
                cmd = cc.cmd + " " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " " + all_cpp_files
                    + cc_options_str + lib_link + gcc_stack + gcc_dce_link + gcc_strip_link + " -o " + output_name
                    + ".exe";
#else
            cmd = cc.cmd + " " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " " + all_cpp_files
#ifdef __APPLE__

                + cc_options_str + lib_link + gcc_dce_link + gcc_strip_link + " -o " + output_name;
#else

                + cc_options_str + lib_link + " -rdynamic" + gcc_dce_link + gcc_strip_link + " -o " + output_name;
#endif
#endif
        }
    }

#ifndef _WIN32
#endif

    int exit_code = 0;
    std::string output = execute(cmd, exit_code);

    if (exit_code != 0) {
        if (output.empty()) {
            std::cerr << "clx: could not run C++ compiler: \"" << cc.cmd << "\"\n";
#ifdef _WIN32
            std::cerr << "clx: install Visual Studio Build Tools (MSVC or the clang-cl/LLVM component) and run"
                         " from an \"x64 Native Tools Command Prompt\", or set CLX_CXX to a C++ compiler.\n";
#else
            std::cerr << "clx: install a C++ toolchain or set CLX_CXX to a C++ compiler.\n";
#endif
        } else {
            std::cerr << output << std::endl;
#ifdef _WIN32
            bool missing_header = (cc.name == "MSVC" && output.find("C1083") != std::string::npos)
                || (cc.name == "ClangCL" && output.find("file not found") != std::string::npos);
            if (missing_header) {
                const char* include_env = std::getenv("INCLUDE");
                if (!include_env || !*include_env)
                    std::cerr << "clx: no MSVC/Windows SDK header environment (INCLUDE) found. Run from an"
                                 " \"x64 Native Tools Command Prompt\" (or \"Developer Command Prompt\") for your"
                                 " Visual Studio version.\n";
            }
#endif
        }
        for (const auto& f : cpp_files) {
            fs::remove(f);
            fs::path base = fs::path(f).parent_path() / fs::path(f).stem();
            fs::remove(fs::path(base.string() + ".obj"));
            fs::remove(fs::path(base.string() + ".o"));
        }
        return exit_code;
    }

    if (mode == BuildMode::Object) {
        std::string final_obj_name = output_name;
        if (fs::path(final_obj_name).extension() != ".obj" && fs::path(final_obj_name).extension() != ".o") {
#ifdef _WIN32
            final_obj_name += ".obj";
#else
            final_obj_name += ".o";
#endif
        }
        std::string module_name = fs::path(input_files[0]).stem().string();
        fs::path generated_obj = fs::temp_directory_path() / (module_name + "_tmp.obj");

        if (!fs::exists(generated_obj)) {
            generated_obj = fs::temp_directory_path() / (module_name + "_tmp.o");
        }

        if (fs::exists(generated_obj)) {
            std::error_code ec;
            fs::rename(generated_obj, final_obj_name, ec);
            if (ec) {
                fs::copy_file(generated_obj, final_obj_name, fs::copy_options::overwrite_existing, ec);
                fs::remove(generated_obj, ec);
            }
        }
    }

    for (const auto& f : cpp_files) {
        fs::remove(f);
        fs::path base = fs::path(f).parent_path() / fs::path(f).stem();
        fs::remove(fs::path(base.string() + ".obj"));
        fs::remove(fs::path(base.string() + ".o"));
    }

    return exit_code;
}