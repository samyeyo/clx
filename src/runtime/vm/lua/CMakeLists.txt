# ─────────────────────────────────────────────
# │ CMakeLists.txt · vendored Lua 5.5.0 bridge │
# ─────────────────────────────────────────────
#
# This file is OUR wrapper around the upstream Lua interpreter sources.
# It adds NO modifications to the upstream files (no Lua source edits).
# It compiles every .c file except lua.c and luac.c (no standalone CLI
# binaries; clx only wants the interpreter).
#
# Upstream Lua is MIT-licensed. Vendoring copy: docs are in
# src/runtime/vm/lua/doc/, the source-overrides Makefile in
# src/runtime/vm/lua/Makefile.
#
# Resulting target: clx_lua (static library). clx_static and
# clx_static_size link against it without exposing our clx:: symbols.

cmake_minimum_required(VERSION 3.15)

# After add_subdirectory("../lua") CMake's CMAKE_CURRENT_SOURCE_DIR becomes
# src/runtime/vm/lua and CMAKE_CURRENT_BINARY_DIR becomes build/clx_lua.
# Prepend ${CMAKE_CURRENT_SOURCE_DIR}/src/ so each filename resolves to the
# actual upstream file path.
set(LUA_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(LUA_SRC_SOURCES
    ${LUA_SRC_DIR}/lapi.c
    ${LUA_SRC_DIR}/lauxlib.c
    ${LUA_SRC_DIR}/lbaselib.c
    ${LUA_SRC_DIR}/lcode.c
    ${LUA_SRC_DIR}/lcorolib.c
    ${LUA_SRC_DIR}/lctype.c
    ${LUA_SRC_DIR}/ldblib.c
    ${LUA_SRC_DIR}/ldebug.c
    ${LUA_SRC_DIR}/ldo.c
    ${LUA_SRC_DIR}/ldump.c
    ${LUA_SRC_DIR}/lfunc.c
    ${LUA_SRC_DIR}/lgc.c
    ${LUA_SRC_DIR}/linit.c
    ${LUA_SRC_DIR}/liolib.c
    ${LUA_SRC_DIR}/llex.c
    ${LUA_SRC_DIR}/lmathlib.c
    ${LUA_SRC_DIR}/lmem.c
    ${LUA_SRC_DIR}/loadlib.c
    ${LUA_SRC_DIR}/lobject.c
    ${LUA_SRC_DIR}/lopcodes.c
    ${LUA_SRC_DIR}/loslib.c
    ${LUA_SRC_DIR}/lparser.c
    ${LUA_SRC_DIR}/lstate.c
    ${LUA_SRC_DIR}/lstring.c
    ${LUA_SRC_DIR}/lstrlib.c
    ${LUA_SRC_DIR}/ltable.c
    ${LUA_SRC_DIR}/ltablib.c
    ${LUA_SRC_DIR}/ltm.c
    ${LUA_SRC_DIR}/lundump.c
    ${LUA_SRC_DIR}/lutf8lib.c
    ${LUA_SRC_DIR}/lvm.c
    ${LUA_SRC_DIR}/lzio.c
)

add_library(clx_lua STATIC ${LUA_SRC_SOURCES})

# Upstream Lua is C99. Force the version explicitly so MSVC picks /std:c11
# on Visual Studio 2019+ (which has full C99).
set_target_properties(clx_lua PROPERTIES
    C_STANDARD 99
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# Minimum-size optimisation: per-function/per-data sections let the final
# executable's --gc-sections drop unreferenced Lua internals; -Os keeps
# compilation small without disabling the inlining Lua's tight inner
# loops (table rehash, string intern search) need.
#
# We intentionally do NOT enable INTERPROCEDURAL_OPTIMIZATION here:
# clx_lua is a static archive consumed by a downstream clang++ link
# (clx.cpp's driver) that may or may not pass -flto. Cross-TU LTO bitcode
# inside the .a is then incompatible with non-LTO link steps and breaks
# the build (linker reports "Unexistent dir" / bitcode mismatch). The
# per-function sections + final --gc-sections give most of the size
# benefit without the LTO coupling risk.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(clx_lua PRIVATE
        -Os
        -ffunction-sections
        -fdata-sections
    )
elseif(MSVC)
    target_compile_options(clx_lua PRIVATE
        /Os
        /Gw
        /Gy
    )
endif()

# Stripping a static archive saves a few KB on disk but has no effect on the
# final executable size once the archive is re-linked by `ld`. Skipping the
# strip step keeps the build portable (BSD strip on macOS does not parse
# GNU-style long options like --strip-debug, and the gain is negligible
# given -Os + IPO already minimise the inline code).

# Platform target-flags. The upstream Makefile sets these; we replicate via
# CMake so the Lua platform flags match the upstream build on macOS, Linux,
# and Windows without touching luaconf.h.
if(APPLE)
    # PUBLIC (not PRIVATE): the platform flag also affects how luaconf.h is
    # read by clx_static / clx_static_size, which include <lua.h>. Without
    # the flag being PUBLIC, clx's headers expand differently from vendored
    # Lua's — e.g., luai_jmpbuf becomes jmp_buf in one and sigjmp_buf in the
    # other, producing ABI drift in lua_State and a runtime SIGBUS on arm64.
    target_compile_definitions(clx_lua PUBLIC LUA_USE_MACOSX)
elseif(UNIX)
    target_compile_definitions(clx_lua PUBLIC LUA_USE_LINUX)
endif()
# On MSVC, luaconf.h already detects Windows — no extra define needed.

# Public include path so the bridge sources (and the user binary linked
# against libclx_lua.a) can `#include "lua.h"` / `"lauxlib.h"` / `"lualib.h"`.
target_include_directories(clx_lua PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Bundle the clx bridge sources into libclx_lua.a. These provide the
# registration (`clx_register_load_builtins`), the `VMFunction` /
# `VMTableProxy` / `NativeBridge` value objects, and the bidirectional
# `vm_convert` helpers. They reference clx::LState, clx::LValue, clx::add,
# etc., so the clx C++ headers must be on the include path. The user binary
# (when built with `clx --dynamic foo.lua`) links libclx.a + libclx_lua.a;
# the bridge's clx:: references resolve to libclx.a at link time.
target_sources(clx_lua PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../load_builtin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../dynamic_vm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vm_convert.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vm_function_object.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vm_table_proxy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vm_native_bridge.cpp
)

set_target_properties(clx_lua PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# clx C++ headers (clx_runtime.h / clx.h) so the bridge sources compile.
# From src/runtime/vm/lua/CMakeLists.txt, the project root is 4 levels up.
target_include_directories(clx_lua PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include
)

# Keep all symbols hidden by default inside the static library; clx only
# exposes what it chooses to expose via its own CLX_API annotations. This
# keeps the binary size modest.
set_target_properties(clx_lua PROPERTIES
    C_VISIBILITY_PRESET hidden
)

# Quiet a few well-known harmless warnings from the upstream sources
# (unused parameter, implicit fallthrough, etc.) without modifying sources.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(clx_lua PRIVATE
        -Wno-unused-but-set-variable
        -Wno-implicit-fallthrough
        -Wno-misleading-indentation
    )
endif()
