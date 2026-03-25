# ── Tests ────────────────────────────────────────────────────────────────────
# Ch 19: "Test the boundary, not the internals."
# We test domain logic and HTTP parsing without needing a running server.

# Simple test runner — no external test framework dependency.
# Each test file has its own main() and uses assert().

add_executable(test_task       test_task.cpp)
add_executable(test_repository test_repository.cpp)
add_executable(test_http       test_http.cpp)
add_executable(test_router     test_router.cpp)
add_executable(test_json       test_json.cpp)

foreach(target test_task test_repository test_http test_router test_json)
    target_link_libraries(${target} PRIVATE webapi)
    add_test(NAME ${target} COMMAND ${target})
endforeach()
