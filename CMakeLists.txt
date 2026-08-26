cmake_minimum_required(VERSION 3.15)
project(clx VERSION 0.3.0 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported)
include(GNUInstallDirs)

set(MI_BUILD_SHARED OFF CACHE INTERNAL "")
set(MI_BUILD_STATIC OFF CACHE INTERNAL "")
set(MI_BUILD_OBJECT ON CACHE INTERNAL "")
set(MI_BUILD_TESTS OFF CACHE INTERNAL "")
set(MI_OVERRIDE ON CACHE INTERNAL "")

if(WIN32)
    # Standard Windows system install: default to "%ProgramFiles%/clx". The
    # project-local bin/ + lib/ dirs below are reserved for in-tree BUILD
    # OUTPUTS (portable layout); the install prefix is separate and can be
    # overridden with -DCMAKE_INSTALL_PREFIX=...
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        if(DEFINED ENV{ProgramFiles})
            set(CLX_DEFAULT_INSTALL_PREFIX "$ENV{ProgramFiles}/clx")
        else()
            set(CLX_DEFAULT_INSTALL_PREFIX "C:/Program Files/clx")
        endif()
        set(CMAKE_INSTALL_PREFIX "${CLX_DEFAULT_INSTALL_PREFIX}" CACHE PATH "Default install prefix" FORCE)
    endif()

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib)

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/lib)

    set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/bin;${CMAKE_CURRENT_SOURCE_DIR}/lib")
else()
    # Only default /usr/local when the user did not pass -DCMAKE_INSTALL_PREFIX.
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(CMAKE_INSTALL_PREFIX /usr/local)
    endif()
endif()

set(RUNTIME_SOURCES
    src/runtime/runtime.cpp 
    src/runtime/base.cpp 
    src/runtime/package.cpp 
    src/runtime/math.cpp 
    src/runtime/coroutine.cpp 
    src/runtime/strings.cpp 
    src/runtime/table.cpp 
    src/runtime/os.cpp 
    src/runtime/utf8.cpp 
    src/runtime/io.cpp
    src/runtime/openlibs.cpp
)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _LOWER_PROC)
if(_LOWER_PROC MATCHES "arm64|aarch64")
    list(APPEND RUNTIME_SOURCES src/runtime/coro_switch_aarch64.S)
elseif(_LOWER_PROC MATCHES "x86_64|amd64")
    if(UNIX AND NOT APPLE)
        list(APPEND RUNTIME_SOURCES src/runtime/coro_switch_x86_64.s)
    endif()
endif()

#──────────────── Embedded Lua 5.5 VM (load / loadfile / dofile)
# The Lua VM is built separately from the default runtime; use the vendored sources or override CLX_LUA_SOURCES_DIR for external Lua sources.
set(CLX_VENDORED_LUA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/vm/lua")
if(NOT DEFINED CLX_LUA_SOURCES_DIR)
    set(CLX_LUA_SOURCES_DIR "${CLX_VENDORED_LUA_DIR}/src")
    if(NOT EXISTS "${CLX_LUA_SOURCES_DIR}/lua.h")
        set(CLX_LUA_SOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/lua-5.5/src")
    endif()
endif()
if(NOT EXISTS "${CLX_LUA_SOURCES_DIR}/lua.h")
    message(FATAL_ERROR
        "Lua 5.5 sources not at ${CLX_LUA_SOURCES_DIR}.\n"
        "  Options:\n"
        "    - Use the in-tree vendored sources at src/runtime/vm/lua\n"
        "    - Run deps/fetch_lua.sh to fetch Lua into deps/lua-5.5/src/\n"
        "    - Pass -DCLX_LUA_SOURCES_DIR=/path/to/lua-5.5/src when configuring")
endif()
if(EXISTS "${CLX_VENDORED_LUA_DIR}/CMakeLists.txt" AND
   "${CLX_LUA_SOURCES_DIR}" STREQUAL "${CLX_VENDORED_LUA_DIR}/src")
    # Build the in-tree Lua wrapper by default so clx --dynamic can find libclx_lua.a without adding a Lua-VM dependency to the default runtime.
    add_subdirectory("${CLX_VENDORED_LUA_DIR}"
                     "${CMAKE_BINARY_DIR}/clx_lua")
else()
    message(STATUS
        "clx: external Lua at ${CLX_LUA_SOURCES_DIR} — user must build liblua.a")
endif()
message(STATUS "clx: embedding Lua from ${CLX_LUA_SOURCES_DIR}")

# VM bridge sources live in the separate clx_lua library and are linked into user binaries only when clx is invoked with --dynamic.

add_library(clx_static STATIC ${RUNTIME_SOURCES})

if(ipo_supported)
    set_property(TARGET clx_static PROPERTY INTERPROCEDURAL_OPTIMIZATION $<$<CONFIG:Release>:TRUE>)
endif()

target_include_directories(clx_static BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# clx_static intentionally has no Lua-VM dependency; the bridge is linked only through clx_lua when --dynamic is used.

if(UNIX)
    set_target_properties(clx_static PROPERTIES OUTPUT_NAME "clx")
    endif()
if(WIN32)
    set_target_properties(clx_static PROPERTIES OUTPUT_NAME "clx")
    endif()

add_library(clx_static_size STATIC ${RUNTIME_SOURCES})

if(ipo_supported)
    set_property(TARGET clx_static_size PROPERTY INTERPROCEDURAL_OPTIMIZATION $<$<CONFIG:Release>:TRUE>)
endif()

target_include_directories(clx_static_size BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# clx_static_size has no Lua-VM dependency. Mirror of clx_static (above).

if(UNIX)
    set_target_properties(clx_static_size PROPERTIES OUTPUT_NAME "clx_size")
    endif()
if(WIN32)
    set_target_properties(clx_static_size PROPERTIES OUTPUT_NAME "clx_size")
    endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" SYSTEM_PROC)
if(SYSTEM_PROC MATCHES "amd64|x86_64")
    set(ARCH_IS_X86 TRUE)
elseif(SYSTEM_PROC MATCHES "arm64|aarch64")
    set(ARCH_IS_ARM TRUE)
endif()

#──────────────── CLX_ARCH — target arch override (enumerated)
# x86: sse2 (default, baseline), avx, avx2, native
# ARM: native (default, -mcpu=native), generic (no -mcpu flag)
set(CLX_ARCH "" CACHE STRING "Target arch (x86: sse2|avx|avx2|native | ARM: native|generic)")
if(CLX_ARCH STREQUAL "")
    if(ARCH_IS_X86)
        set(CLX_ARCH "sse2" CACHE STRING "Target arch (x86: sse2|avx|avx2|native | ARM: native|generic)" FORCE)
    elseif(ARCH_IS_ARM)
        set(CLX_ARCH "native" CACHE STRING "Target arch (x86: sse2|avx|avx2|native | ARM: native|generic)" FORCE)
    endif()
endif()

set(CLX_ARCH_GCC_FLAG "")
set(CLX_ARCH_MSVC_FLAG "")
if(ARCH_IS_X86)
    if(CLX_ARCH STREQUAL "sse2")
        set(CLX_ARCH_GCC_FLAG "-msse2")
        set(CLX_ARCH_MSVC_FLAG "/arch:SSE2")
    elseif(CLX_ARCH STREQUAL "avx")
        set(CLX_ARCH_GCC_FLAG "-mavx")
        set(CLX_ARCH_MSVC_FLAG "/arch:AVX")
    elseif(CLX_ARCH STREQUAL "avx2")
        set(CLX_ARCH_GCC_FLAG "-mavx2")
        set(CLX_ARCH_MSVC_FLAG "/arch:AVX2")
    elseif(CLX_ARCH STREQUAL "native")
        set(CLX_ARCH_GCC_FLAG "-march=native")
        set(CLX_ARCH_MSVC_FLAG "/arch:AVX2")
    elseif(NOT CLX_ARCH STREQUAL "")
        message(FATAL_ERROR "clx: unknown CLX_ARCH='${CLX_ARCH}' for x86 (allowed: sse2|avx|avx2|native)")
    endif()
elseif(ARCH_IS_ARM)
    if(CLX_ARCH STREQUAL "native")
        set(CLX_ARCH_GCC_FLAG "-mcpu=native")
    elseif(CLX_ARCH STREQUAL "generic")
        set(CLX_ARCH_GCC_FLAG "")
    elseif(NOT CLX_ARCH STREQUAL "")
        message(FATAL_ERROR "clx: unknown CLX_ARCH='${CLX_ARCH}' for ARM (allowed: native|generic)")
    endif()
endif()
if(NOT CLX_ARCH STREQUAL "")
    message(STATUS "clx: CLX_ARCH=${CLX_ARCH} (GCC flag: ${CLX_ARCH_GCC_FLAG} MSVC flag: ${CLX_ARCH_MSVC_FLAG})")
endif()

set(SOURCES
    src/clx.cpp
    src/syntax/lexer.cpp
    src/syntax/parser.cpp
    src/optimizer/optimizer.cpp
    src/codegen/codegen.cpp
)

add_executable(clx ${SOURCES})
target_include_directories(clx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(clx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(clx PRIVATE
    CLX_VERSION="${PROJECT_VERSION}"
    CLX_DEFAULT_CXX="${CMAKE_CXX_COMPILER}"
    CLX_DEFAULT_CXX_NAME="${CMAKE_CXX_COMPILER_ID}"
    CLX_INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}"
    CLX_INSTALL_LIBDIR="${CMAKE_INSTALL_LIBDIR}"
    CLX_INSTALL_BINDIR="${CMAKE_INSTALL_BINDIR}"
    CLX_INSTALL_INCLUDEDIR="${CMAKE_INSTALL_INCLUDEDIR}"
    CLX_ARCH="${CLX_ARCH}"
    CLX_ARCH_GCC_FLAG="${CLX_ARCH_GCC_FLAG}"
    CLX_ARCH_MSVC_FLAG="${CLX_ARCH_MSVC_FLAG}"
)

if(ipo_supported)
    set_target_properties(clx PROPERTIES INTERPROCEDURAL_OPTIMIZATION $<$<CONFIG:Release>:TRUE>)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    if(ARCH_IS_X86)
        target_compile_options(clx PRIVATE $<$<CONFIG:Release>:-O3;-flto=auto;-ffloat-store;-fno-math-errno;${CLX_ARCH_GCC_FLAG}>)
        target_compile_options(clx_static PRIVATE $<$<CONFIG:Release>:-O3;-flto=auto;-ffloat-store;-fno-math-errno;${CLX_ARCH_GCC_FLAG}>)
        target_compile_options(clx_static_size PRIVATE $<$<CONFIG:Release>:-Os;-flto=auto;-ffloat-store;-fno-math-errno;${CLX_ARCH_GCC_FLAG}>)
    elseif(ARCH_IS_ARM)
        target_compile_options(clx PRIVATE $<$<CONFIG:Release>:-O3;-flto=auto;-fstrict-aliasing;-fno-math-errno;${CLX_ARCH_GCC_FLAG}>)
        target_compile_options(clx_static PRIVATE $<$<CONFIG:Release>:-O3;-flto=auto;-fstrict-aliasing;-fno-math-errno;${CLX_ARCH_GCC_FLAG}>)
        target_compile_options(clx_static_size PRIVATE $<$<CONFIG:Release>:-Os;-flto=auto;-fstrict-aliasing;-fno-math-errno;${CLX_ARCH_GCC_FLAG}>)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    string(REPLACE "/O2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "/Ob2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "/O2" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/Ob2" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    set_target_properties(clx PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_target_properties(clx_static PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set_target_properties(clx_static_size PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    
    if(ARCH_IS_X86)
        target_compile_options(clx PRIVATE $<$<CONFIG:Release>:/O2;/Ob2;${CLX_ARCH_MSVC_FLAG};/fp:strict>)
        target_compile_options(clx_static PRIVATE $<$<CONFIG:Release>:/O2;/Ob2;${CLX_ARCH_MSVC_FLAG};/fp:strict>)
        target_compile_options(clx_static_size PRIVATE $<$<CONFIG:Release>:/O1;/Ob1;${CLX_ARCH_MSVC_FLAG};/fp:strict>)
    elseif(ARCH_IS_ARM)
        target_compile_options(clx PRIVATE $<$<CONFIG:Release>:/O2;/Ob2;/fp:strict>)
        target_compile_options(clx_static PRIVATE $<$<CONFIG:Release>:/O2;/Ob2;/fp:strict>)
        target_compile_options(clx_static_size PRIVATE $<$<CONFIG:Release>:/O1;/Ob1;/fp:strict>)
    endif()
endif()

if(APPLE)
    target_compile_options(clx PRIVATE -Wno-deprecated-declarations)
    target_compile_options(clx_static PRIVATE -Wno-deprecated-declarations)
    target_compile_options(clx_static_size PRIVATE -Wno-deprecated-declarations)
endif()

install(TARGETS clx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS clx_static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS clx_static_size ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(TARGET clx_lua)
    install(TARGETS clx_lua ARCHIVE
            DESTINATION ${CMAKE_INSTALL_LIBDIR} OPTIONAL)
endif()
install(FILES include/clx.h include/clx_runtime.h include/clx_simd.h
              OPTIONAL DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES doc/dyamic-lua.md
        OPTIONAL DESTINATION ${CMAKE_INSTALL_DOCDIR})
