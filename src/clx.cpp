// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  clx.cpp · CLX Compiler Driver              │
// └─────────────────────────────────────────────┘

#ifdef _WIN32
#include <windows.h>
#endif

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <array>
#include <cstdio>
#include <stdexcept>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "syntax/parser.h"
#include "codegen/codegen.h"

namespace fs = std::filesystem;

std::vector<std::string> precompiled_modules;

//------------------ ENUM: BuildMode - output mode (executable binary, object file, or static library)
enum class BuildMode { Executable, Object, Static };

//------------------ STRUCT: Compiler - holds detected C++ compiler name and command
struct Compiler {
    std::string name;
    std::string cmd;
};


//------------------ CLX: execute - runs a shell command, captures stdout and exit code
std::string execute(const std::string& cmd, int& out_code) {
    std::array<char, 128> buffer;
    std::string result;
    std::cerr << cmd << std::endl;
#ifdef _WIN32
    auto pipe = _popen(cmd.c_str(), "r");
#else
    auto pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
#ifdef _WIN32
    out_code = _pclose(pipe);
#else
    out_code = pclose(pipe);
#endif
    return result;
}


//------------------ CLX: detect_compiler - auto-detects available C++ compiler (clang++/g++/cl)
Compiler detect_compiler() {
    int code = -1;

#ifdef _WIN32
    std::string redirect = " >nul 2>&1";
#else
    std::string redirect = " >/dev/null 2>&1";
#endif

    execute("clang++ --version" + redirect, code);
    if (code == 0) return {"clang", "clang++"};

    execute("g++ --version" + redirect, code);
    if (code == 0) return {"gcc", "g++"};

#ifdef _WIN32
    execute("cl" + redirect, code);
    if (code == 0) return {"msvc", "cl"};
#endif

    return {"unknown", ""};
}


//------------------ CLX: print_help - displays usage information
void print_help() {
    std::cout << "Usage: clx [options] <file.lua> [<compiler-options>]\n\n"
              << "clx Compiler Options:\n"
              << "  --output <name>       Specify output file name\n"
              << "  --executable          Build executable (default)\n"
              << "  --object              Compile to object file (.o/.obj)\n"
               << "  --static              Compile to static library (.a/.lib)\n"
               << "  --debug               Enable debug symbols\n"
               << "  --size                Optimize for size (default)\n"
               << "  --fast                Optimize for speed\n"
               << "  --cpp                 Generate C++ source file and exit\n"
               << "  --minimal             Exclude non-essential Lua modules; keeps base + package\n"
               << "  --version             Print version and exit\n"
               << "  --help                Display this help message\n\n"
              << "Compiler Options:\n"
              << "  Any options starting with '-' not recognized by clx are passed to the C++ compiler.\n"
              << "  If no compiler options are provided, default optimization flags are used.\n"
              << "  Example: clx file.lua -O3 -march=native\n";
}

//------------------ CLX: main - entry point, parses CLI arguments, compiles Lua to C++, links output
int main(int argc, char* argv[]) {
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
            std::cout << "clx 0.1.0\nMIT License - Copyright (c) 2026 Tine Samir\n";
            return 0;
        } else if (arg == "--output") {
            if (i + 1 < argc) custom_output_name = argv[++i];
        } else if (arg == "--minimal") {
            minimal_active = true;
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

    if (debug_mode) size_mode = false;

    bool dce_mode = (mode == BuildMode::Executable && !debug_mode);

    if (input_files.empty()) {
        std::cerr << "Error: No input file specified.\n";
        return 1;
    }

    std::string cc_compile_str = "";
    std::string cc_link_str = "";
    bool link_seen = false;
    for (const auto& opt : cc_options) {
        if (opt == "/link") { link_seen = true; continue; }
        if (link_seen) cc_link_str += " " + opt;
        else           cc_compile_str += " " + opt;
    }
    if (!link_seen) {
        cc_link_str = cc_compile_str;
        cc_compile_str = "";
    }
    std::string cc_options_str = cc_compile_str + cc_link_str;

    std::string opt_flags;
    std::string msvc_opt_flags;
    std::string gcc_dce_cl = dce_mode ? " -ffunction-sections -fdata-sections" : "";
#ifdef __APPLE__
    std::string gcc_dce_link = dce_mode ? " -Wl,-dead_strip" : "";
    std::string gcc_strip_link = debug_mode ? "" : "";
#else
    std::string gcc_dce_link = dce_mode ? " -Wl,--gc-sections" : "";
    std::string gcc_strip_link = debug_mode ? "" : " -s";
#endif
    std::string msvc_dce_cl = dce_mode ? " /Gy" : "";
    std::string msvc_dce_link = dce_mode ? " /link /LTCG /OPT:REF /OPT:ICF" : "";

    if (debug_mode) {
        opt_flags = "-O0 -g -fno-rtti";
        msvc_opt_flags = "/Od /Zi /MDd /EHsc";
    } else if (size_mode) {
        opt_flags = "-Os -flto=auto -fno-rtti -fvisibility=hidden";
        msvc_opt_flags = "/O1 /GL /GR- /MD /EHsc /GS- /fp:fast /Gw /Gy";
    } else {
        opt_flags = "-O3 -g -flto=auto -fno-rtti -fvisibility=hidden";
        msvc_opt_flags = "/O2 /Ot /GL /GR- /MD /EHsc /GS- /fp:fast /Gw /Gy";
    }

    std::string output_name = custom_output_name.empty() ? fs::path(input_files[0]).stem().string() : custom_output_name;

    if (fs::path(output_name).extension() == ".exe") {
        output_name = fs::path(output_name).stem().string();
    }

    std::vector<std::string> cpp_files;

    for (const auto& input_file : input_files) {
        std::ifstream t(input_file);
        if (!t.is_open()) {
            std::cerr << "Error: Cannot open input file " << input_file << "\n";
            return 1;
        }
        std::string source((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());

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
        fs::path cpp_file;

        if (emit_cpp) {
            cpp_file = module_name + ".cpp";
        } else {
            cpp_file = fs::temp_directory_path() / (module_name + "_tmp.cpp");
        }

        clx::CodeEmitter emitter(ctx, cpp_file.string().c_str());
        emitter.emit(root, module_name);
        cpp_files.push_back(cpp_file.string());
    }

    if (emit_cpp) {
        return 0;
    }

    if (mode == BuildMode::Executable) {
        std::string main_module = fs::path(input_files[0]).stem().string();
        std::ofstream appender(cpp_files[0], std::ios::app);

        for (const auto& file : input_files) {
            std::string mod = fs::path(file).stem().string();
            appender << "\nextern clx::LValue luaopen_" << mod << "(clx::LState* L);\n";
        }
        for (const auto& mod : precompiled_modules) {
            appender << "\nextern clx::LValue luaopen_" << mod << "(clx::LState* L);\n";
        }

        appender << "int main(int argc, char* argv[]) {\n";
        appender << "    clx::LState* L = clx::open(argc, argv);\n";
        if (!minimal_active) appender << "    clx::openlibs(L);\n";
        appender << "    try {\n";

        for (size_t i = 1; i < input_files.size(); ++i) {
            std::string mod = fs::path(input_files[i]).stem().string();
            std::string lua_mod = input_files[i];
            size_t dot = lua_mod.rfind('.');
            if (dot != std::string::npos) lua_mod = lua_mod.substr(0, dot);
            for (auto& c : lua_mod) if (c == '/' || c == '\\') c = '.';
            appender << "        L->register_module(\"" << lua_mod << "\", luaopen_" << mod << ");\n";
            if (lua_mod != mod) appender << "        L->register_module(\"" << mod << "\", luaopen_" << mod << ");\n";
        }
        for (const auto& mod : precompiled_modules) {
            appender << "        L->register_module(\"" << mod << "\", luaopen_" << mod << ");\n";
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

    Compiler cc = detect_compiler();
    if (cc.name == "unknown") {
        std::cerr << "Error: No C++ compiler found.\n";
        return 2;
    }

    std::string include_opt;
    std::string lib_link;

    fs::path exe_dir;
#ifdef _WIN32
    char path_buffer[MAX_PATH];
    GetModuleFileNameA(NULL, path_buffer, MAX_PATH);
    exe_dir = fs::path(path_buffer).parent_path();
#else
    {
        char buf[4096];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len != -1) {
            buf[len] = '\0';
            exe_dir = fs::path(buf).parent_path();
        } else {
            exe_dir = fs::absolute(fs::path(argv[0])).parent_path();
        }
    }
#endif
    fs::path build_root = exe_dir.parent_path();

#ifdef _WIN32
    std::string include_path;
    if (fs::exists(build_root / "include")) {
        include_path = (build_root / "include").string();
    } else {
        include_path = "include";
    }
    include_opt = " /I\"" + include_path + "\"";

    fs::path win_lib = build_root / "lib";
    std::string lib_file = "clx.lib";
    fs::path lib_path = win_lib / lib_file;
    if (!fs::exists(lib_path)) {
        fs::path release_lib = win_lib / "Release" / lib_file;
        if (fs::exists(release_lib))
            lib_path = release_lib;
    }
    lib_link = " \"" + lib_path.string() + "\"";
#else
    fs::path include_path = build_root / "include";
    if (fs::exists(include_path)) {
        include_opt = " -I " + include_path.string();
    } else {
        include_opt = " -I include";
    }

    fs::path lib_path;
    std::string lib_name;
    if (false) {
        lib_name = "-lclx";
        if (fs::exists((lib_path = build_root / "build") / "libclx.so") ||
            fs::exists((lib_path = build_root / "lib") / "libclx.so") ||
            fs::exists((lib_path = build_root / "lib64") / "libclx.so"))
            lib_link = " -L " + lib_path.string() + " " + lib_name;
        else
            lib_link = " -L build " + lib_name;
    } else {
#ifdef __APPLE__
        lib_name = size_mode ? "-lclx_size" : "-lclx ";
#else
        lib_name = size_mode ? "-l:libclx_size.a" : "-l:libclx.a";
#endif
        std::string lib_file = size_mode ? "libclx_size.a" : "libclx.a";
        if (fs::exists((lib_path = build_root / "build") / lib_file) ||
            fs::exists((lib_path = build_root / "lib") / lib_file) ||
            fs::exists((lib_path = build_root / "lib64") / lib_file) ||
            fs::exists((lib_path = build_root / "lib" / "x86_64-linux-gnu") / lib_file))
            lib_link = " -L " + lib_path.string() + " " + lib_name;
        else
            lib_link = " -L build " + lib_name;
    }
#endif

    std::vector<fs::path> mod_search_dirs;
    mod_search_dirs.push_back(fs::current_path());
    {
        fs::path p = build_root / "lib" / "clx";
        if (fs::exists(p))
            mod_search_dirs.push_back(p);
    }
#ifndef _WIN32
    {
        fs::path p("/usr/local/lib/clx");
        if (fs::exists(p))
            mod_search_dirs.push_back(p);
    }
#endif

    std::string all_cpp_files = "";
    for (const auto& f : cpp_files) {
        all_cpp_files += "\"" + f + "\" ";
    }

    std::string obj_files;
    for (const auto& f : cpp_files) {
        obj_files += fs::path(f).stem().string() + ".o ";
    }

    for (const auto& mod : precompiled_modules) {
#ifdef _WIN32
        std::string target_lib = mod + ".lib";
#else
        std::string target_lib = mod + ".a";
#endif
        bool found = false;
        for (const auto& dir : mod_search_dirs) {
            fs::path full = dir / target_lib;
            if (fs::exists(full)) {
#ifdef _WIN32
                if (dir == fs::current_path())
                    all_cpp_files += "\"" + target_lib + "\" ";
                else
                    all_cpp_files += "\"" + fs::absolute(full).string() + "\" ";
#else
                if (dir == fs::current_path())
                    all_cpp_files += "\"" + full.string() + "\" ";
                else
                    all_cpp_files += " -L \"" + dir.string() + "\" -l:" + target_lib + " ";
#endif
                found = true;
                break;
            }
        }
        if (!found) {
#ifdef _WIN32
            all_cpp_files += "\"" + target_lib + "\" ";
#else
            all_cpp_files += "-l:" + target_lib + " ";
#endif
        }
    }

    std::string cmd;
    if (cc.name == "msvc") {
        std::string tmp_dir = fs::temp_directory_path().string();
        if (!tmp_dir.empty() && tmp_dir.back() != '\\') {
            tmp_dir += '\\';
        }
        std::string fo_arg = " /Fo\"" + tmp_dir + "\\\"";

        std::string out_ext;
        if (mode != BuildMode::Object && mode != BuildMode::Static) {
            out_ext = ".exe";
            if (output_name.size() >= 4 && output_name.compare(output_name.size() - 4, 4, ".exe") == 0) out_ext = "";
        }
        std::string fe_arg = out_ext.empty() ? "" : " /Fe\"" + output_name + out_ext + "\"";

        std::string msvc_obj_files;
        for (const auto& f : cpp_files) {
            msvc_obj_files += "\"" + tmp_dir + "\\" + fs::path(f).stem().string() + ".obj\" ";
        }

        if (mode == BuildMode::Object) {
            cmd = cc.cmd + " /nologo /c " + msvc_opt_flags + msvc_dce_cl + " /std:c++20" + include_opt + " " + all_cpp_files + cc_compile_str + fo_arg;
        } else if (mode == BuildMode::Static) {
            std::string lib_out = output_name;
            if (!(lib_out.size() >= 4 && lib_out.compare(lib_out.size() - 4, 4, ".lib") == 0))
                lib_out += ".lib";
            cmd = cc.cmd + " /nologo /c " + msvc_opt_flags + msvc_dce_cl + " /std:c++20" + include_opt + " " + all_cpp_files + cc_compile_str + fo_arg + " && lib /nologo /OUT:\"" + lib_out + "\" " + msvc_obj_files;
        } else {
            cmd = cc.cmd + " /nologo " + msvc_opt_flags + msvc_dce_cl + " /std:c++20" + include_opt + cc_compile_str + " " + all_cpp_files + fo_arg + lib_link + fe_arg + " " + msvc_dce_link + cc_link_str;
        }
    } else {
        if (mode == BuildMode::Object)
            cmd = cc.cmd + " -c " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " -fPIC " + all_cpp_files + " -o \"" + (fs::temp_directory_path() / (fs::path(input_files[0]).stem().string() + "_tmp.o")).string() + "\"" + cc_options_str;
        else if (mode == BuildMode::Static) {
            std::string ext = ".a";
            if (output_name.size() >= ext.size() && output_name.compare(output_name.size() - ext.size(), ext.size(), ext) == 0)
                cmd = cc.cmd + " -c " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " " + all_cpp_files + cc_options_str + " && ar rcs " + output_name + " " + obj_files;
            else
                cmd = cc.cmd + " -c " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " " + all_cpp_files + cc_options_str + " && ar rcs " + output_name + ".a " + obj_files;
        } else {
#ifdef _WIN32
            std::string ext = ".exe";
            if (output_name.size() >= ext.size() && output_name.compare(output_name.size() - ext.size(), ext.size(), ext) == 0)
                cmd = cc.cmd + " " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " " + all_cpp_files + cc_options_str + lib_link + gcc_dce_link + gcc_strip_link + " -o " + output_name;
            else
                cmd = cc.cmd + " " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " " + all_cpp_files + cc_options_str + lib_link + gcc_dce_link + gcc_strip_link + " -o " + output_name + ".exe";
#else
            cmd = cc.cmd + " " + opt_flags + gcc_dce_cl + " -std=c++20" + include_opt + " " + all_cpp_files + cc_options_str + lib_link + " -rdynamic" + gcc_dce_link + gcc_strip_link + " -o " + output_name;
            #endif
        }
    }

#ifndef _WIN32
#endif

    int exit_code = 0;
    std::string output = execute(cmd, exit_code);

    if (exit_code != 0) {
        std::cerr << output << std::endl;
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