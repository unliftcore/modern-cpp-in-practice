cmake_minimum_required(VERSION 3.25)
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

# ── Warning policy (Ch 20 & Appendix B) ─────────────────────────────────────
# "Treat warnings as repository policy, not developer preference."
add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU>:
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-align -Wunused -Woverloaded-virtual
        -Wconversion -Wsign-conversion -Wnull-dereference
        -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
    >
    $<$<CXX_COMPILER_ID:Clang>:
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-align -Wunused -Woverloaded-virtual
        -Wconversion -Wsign-conversion -Wnull-dereference
        -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
    >
)

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

# ── Header-only library for the web-api framework ───────────────────────────
add_library(webapi INTERFACE)
target_include_directories(webapi INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(webapi INTERFACE project_warnings project_sanitizers)

# ── Main executable ─────────────────────────────────────────────────────────
add_executable(web-api-server src/main.cpp)
target_link_libraries(web-api-server PRIVATE webapi)

# ── Tests (Ch 19: "Testing Strategy") ───────────────────────────────────────
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
