cmake_minimum_required(VERSION 3.28)
project(web-api-example
    VERSION   0.1.0
    LANGUAGES CXX
    DESCRIPTION "Example REST API — Modern C++ in Practice"
)

# ── C++23 baseline (Appendix B: Toolchain Baseline) ─────────────────────────
set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Require Ninja generator for C++20 module support ────────────────────────
# CMake's module dependency scanning for the Clang lane requires Ninja or
# Ninja Multi-Config. MSVC can configure with Visual Studio generators.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR
    "The Clang module lane requires the Ninja generator. "
        "Please configure with: cmake -G Ninja -B build ...")
endif()

# ── Warning policy (Ch 20 & Appendix B) ─────────────────────────────────────
# "Treat warnings as repository policy, not developer preference."
add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
    $<$<CXX_COMPILER_ID:Clang>:
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-align -Wunused -Woverloaded-virtual
        -Wconversion -Wsign-conversion -Wnull-dereference
        -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
    >
    $<$<CXX_COMPILER_ID:GNU>:
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-align -Wunused -Woverloaded-virtual
        -Wconversion -Wsign-conversion -Wnull-dereference
        -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
    >
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4 /permissive- /EHsc /Zc:__cplusplus
    >
)

# ── libc++ (required for C++23 modules + jthread with Clang) ────────────────
# We need libc++ 20+ headers for std::jthread / std::stop_token support.
# Detect the LLVM installation path from the compiler's location.
add_library(project_stdlib INTERFACE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT MSVC)
    get_filename_component(_CLANG_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_LLVM_PREFIX "${_CLANG_DIR}" DIRECTORY)

    # Try compiler's own directory first, then fall back to /usr/lib/llvm-20
    if(EXISTS "${_LLVM_PREFIX}/include/c++/v1/__config")
        set(_LIBCXX_INCLUDE "${_LLVM_PREFIX}/include/c++/v1")
        set(_LIBCXX_LIB     "${_LLVM_PREFIX}/lib")
    elseif(EXISTS "/usr/lib/llvm-20/include/c++/v1/__config")
        set(_LIBCXX_INCLUDE "/usr/lib/llvm-20/include/c++/v1")
        set(_LIBCXX_LIB     "/usr/lib/llvm-20/lib")
    else()
        message(FATAL_ERROR
            "Cannot find libc++ 20+ headers. Install libc++-20-dev or set "
            "CMAKE_CXX_COMPILER to a Clang that ships with libc++ 20+.")
    endif()

    target_compile_options(project_stdlib INTERFACE
        -stdlib=libc++
        -stdlib++-isystem "${_LIBCXX_INCLUDE}"
    )
    target_link_options(project_stdlib INTERFACE
        -stdlib=libc++
        -L${_LIBCXX_LIB}
        -Wl,-rpath,${_LIBCXX_LIB}
    )
    target_link_libraries(project_stdlib INTERFACE c++)
endif()

# ── Sanitizer lanes (Ch 20: "named lanes, not rescue tools") ────────────────
option(ENABLE_ASAN  "Enable AddressSanitizer + UBSan"  OFF)
option(ENABLE_TSAN  "Enable ThreadSanitizer"            OFF)

add_library(project_sanitizers INTERFACE)
if(ENABLE_ASAN)
    if(MSVC)
        target_compile_options(project_sanitizers INTERFACE /fsanitize=address)
        target_link_options(project_sanitizers    INTERFACE /fsanitize=address)
    else()
        target_compile_options(project_sanitizers INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(project_sanitizers    INTERFACE -fsanitize=address,undefined)
    endif()
endif()
if(ENABLE_TSAN)
    if(MSVC)
        message(FATAL_ERROR "ThreadSanitizer is not supported with MSVC in this example.")
    else()
        target_compile_options(project_sanitizers INTERFACE -fsanitize=thread)
        target_link_options(project_sanitizers    INTERFACE -fsanitize=thread)
    endif()
endif()

# ── Module library (Ch 11: C++20 Modules) ───────────────────────────────────
# Each .cppm file is a named module interface unit.
# CMake scans for `import` declarations and builds the dependency graph.
add_library(webapi)
target_sources(webapi
    PUBLIC FILE_SET CXX_MODULES FILES
        src/modules/error.cppm
        src/modules/json.cppm
        src/modules/task.cppm
        src/modules/http.cppm
        src/modules/repository.cppm
        src/modules/router.cppm
        src/modules/middleware.cppm
        src/modules/handlers.cppm
)
target_link_libraries(webapi PUBLIC project_warnings project_stdlib project_sanitizers)
if(WIN32)
    target_link_libraries(webapi PUBLIC ws2_32)
endif()

# ── Main executable ─────────────────────────────────────────────────────────
add_executable(web-api-server src/main.cpp)
target_link_libraries(web-api-server PRIVATE webapi)

# ── Tests (Ch 19: "Testing Strategy") ───────────────────────────────────────
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
