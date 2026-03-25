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
# CMake's module dependency scanning requires Ninja or Ninja Multi-Config.
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR
        "C++20 modules require the Ninja generator. "
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
)

# ── libc++ (required for C++23 modules + jthread with Clang) ────────────────
# We use libc++ 20 headers for std::jthread / std::stop_token support, and
# explicitly link the matching runtime from /usr/lib/llvm-20/lib.
add_library(project_stdlib INTERFACE)
target_compile_options(project_stdlib INTERFACE
    -stdlib=libc++
    -stdlib++-isystem /usr/lib/llvm-20/include/c++/v1
)
target_link_options(project_stdlib INTERFACE
    -stdlib=libc++
    -L/usr/lib/llvm-20/lib
    -Wl,-rpath,/usr/lib/llvm-20/lib
)
target_link_libraries(project_stdlib INTERFACE c++)

# ── Sanitizer lanes (Ch 20: "named lanes, not rescue tools") ────────────────
option(ENABLE_ASAN  "Enable AddressSanitizer + UBSan"  OFF)
option(ENABLE_TSAN  "Enable ThreadSanitizer"            OFF)

add_library(project_sanitizers INTERFACE)
if(ENABLE_ASAN)
    target_compile_options(project_sanitizers INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(project_sanitizers    INTERFACE -fsanitize=address,undefined)
endif()
if(ENABLE_TSAN)
    target_compile_options(project_sanitizers INTERFACE -fsanitize=thread)
    target_link_options(project_sanitizers    INTERFACE -fsanitize=thread)
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

# ── Main executable ─────────────────────────────────────────────────────────
add_executable(web-api-server src/main.cpp)
target_link_libraries(web-api-server PRIVATE webapi)

# ── Tests (Ch 19: "Testing Strategy") ───────────────────────────────────────
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
