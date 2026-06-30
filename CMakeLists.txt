cmake_minimum_required(VERSION 3.15)
project(clx VERSION 0.1.0 LANGUAGES CXX)

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
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Default install prefix" FORCE)
    endif()

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
    set(CMAKE_ARCH_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin)
    set(CMAKE_ARCH_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    set(CMAKE_ARCH_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/lib)

    set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/bin;${CMAKE_CURRENT_SOURCE_DIR}/lib")
else()
    set(CMAKE_INSTALL_PREFIX /usr/local)
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

add_library(clx_static STATIC ${RUNTIME_SOURCES})

if(ipo_supported)
    set_property(TARGET clx_static PROPERTY INTERPROCEDURAL_OPTIMIZATION $<$<CONFIG:Release>:TRUE>)
endif()

target_include_directories(clx_static BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

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

if(UNIX)
    set_target_properties(clx_static_size PROPERTIES OUTPUT_NAME "clx_size")
    endif()
if(WIN32)
    set_target_properties(clx_static_size PROPERTIES OUTPUT_NAME "clx_size")
    endif()

set(SOURCES
    src/clx.cpp
    src/syntax/lexer.cpp
    src/syntax/parser.cpp
    src/syntax/visitor.cpp
    src/optimizer/optimizer.cpp
    src/codegen/codegen.cpp
)

add_executable(clx ${SOURCES})
target_include_directories(clx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(clx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" SYSTEM_PROC)
if(SYSTEM_PROC MATCHES "amd64|x86_64")
    set(ARCH_IS_X86 TRUE)
elseif(SYSTEM_PROC MATCHES "arm64|aarch64")
    set(ARCH_IS_ARM TRUE)
endif()

if(ipo_supported)
    set_target_properties(clx PROPERTIES INTERPROCEDURAL_OPTIMIZATION $<$<CONFIG:Release>:TRUE>)
    endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    if(ARCH_IS_X86)
        target_compile_options(clx PRIVATE $<$<CONFIG:Release>:-O3;-flto=auto;-ffloat-store;-fno-math-errno;-mavx2>)
        target_compile_options(clx_static PRIVATE $<$<CONFIG:Release>:-O3;-flto=auto;-ffloat-store;-fno-math-errno;-mavx2>)
        target_compile_options(clx_static_size PRIVATE $<$<CONFIG:Release>:-Os;-flto=auto;-ffloat-store;-fno-math-errno;-mavx2>)
            elseif(ARCH_IS_ARM)
        target_compile_options(clx PRIVATE $<$<CONFIG:Release>:-O3;-flto=auto;-fstrict-aliasing;-fno-math-errno;-mcpu=native>)
        target_compile_options(clx_static PRIVATE $<$<CONFIG:Release>:-O3;-flto=auto;-fstrict-aliasing;-fno-math-errno;-mcpu=native>)
        target_compile_options(clx_static_size PRIVATE $<$<CONFIG:Release>:-Os;-flto=auto;-fstrict-aliasing;-fno-math-errno;-mcpu=native>)
            endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set_target_properties(clx PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_target_properties(clx_static PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set_target_properties(clx_static_size PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    
    if(ARCH_IS_X86)
        target_compile_options(clx PRIVATE $<$<CONFIG:Release>:/O2;/Ob2;/arch:AVX2;/fp:strict>)
        target_compile_options(clx_static PRIVATE $<$<CONFIG:Release>:/O2;/Ob2;/arch:AVX2;/fp:strict>)
        target_compile_options(clx_static_size PRIVATE $<$<CONFIG:Release>:/O1;/Ob1;/arch:AVX2;/fp:strict>)
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
install(FILES include/clx.h include/clx_runtime.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})