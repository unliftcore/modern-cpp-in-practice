# Web API Example — Modern C++ in Practice

A complete REST API for task management, built entirely with **C++23** features
and following the best practices from every part of the book.

## Quick Start

```bash
# Configure (requires Clang 20+, libc++ 20+, CMake 3.28+, Ninja)
cmake -G Ninja -B build \
    -DCMAKE_CXX_COMPILER=clang++-20 \
    -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build

# Run tests
cd build && ctest --output-on-failure

# Start the server
./build/web-api-server
```

The server listens on **port 8080**. Press `Ctrl+C` for graceful shutdown.

> **Note:** This example uses C++20 modules (`export module`, `import`).
> Clang with libc++ currently has the best module support. GCC's module
> implementation has known issues with standard library headers in the
> global module fragment. The focus is on demonstrating the syntax and
> architecture — not on cross-compiler portability.

## API Endpoints

| Method   | Path           | Description         |
|----------|----------------|---------------------|
| `GET`    | `/health`      | Health check        |
| `GET`    | `/tasks`       | List all tasks      |
| `GET`    | `/tasks/:id`   | Get task by ID      |
| `POST`   | `/tasks`       | Create a task       |
| `PUT`    | `/tasks/:id`   | Replace a task      |
| `PATCH`  | `/tasks/:id`   | Partial update      |
| `DELETE` | `/tasks/:id`   | Delete a task       |

### Example Requests

```bash
# List all tasks
curl http://localhost:8080/tasks

# Create a task
curl -X POST http://localhost:8080/tasks \
  -H "Content-Type: application/json" \
  -d '{"title":"Read Chapter 3","description":"Learn std::expected"}'

# Update completion status
curl -X PATCH http://localhost:8080/tasks/1 \
  -H "Content-Type: application/json" \
  -d '{"completed":true}'

# Delete a task
curl -X DELETE http://localhost:8080/tasks/1
```

## C++23 Features Demonstrated

| Feature | Where | Chapter |
|---------|-------|---------|
| C++20 modules (`export module`, `import`) | All `.cppm` files, `main.cpp`, tests | Ch 11 |
| `std::expected<T, E>` | `error.cppm`, `repository.cppm`, `handlers.cppm` | Ch 3 |
| `std::unexpected` | `error.cppm` | Ch 3 |
| `std::optional<T>` | `task.cppm`, `repository.cppm`, `http.cppm`, `json.cppm` | Ch 5 |
| `std::string_view` | Throughout — parameter passing, header lookup | Ch 4, 5 |
| `std::format`, `std::print`, `std::println` | Response construction, startup/error messages, logging | Ch 5 |
| Concepts (`concept`, `requires`) | `json.cppm`, `repository.cppm`, `middleware.cppm` | Ch 6 |
| `std::ranges` / `std::views` | `repository.cppm` filter/transform, `json.cppm` serialize | Ch 7 |
| `constexpr` / `consteval` | `error.cppm` HTTP status mapping, `http.cppm` method parsing | Ch 8 |
| `std::jthread` / `std::stop_token` | `http.cppm` server loop, `main.cpp` shutdown | Ch 14 |
| `[[nodiscard]]` | All error-bearing returns | Ch 3, 9 |
| Designated initializers | `Task{}`, `Response{}`, `Request{}` construction | Ch 2 |
| `operator<=>` (spaceship) | `Task` defaulted three-way comparison | Ch 2 |
| `std::shared_mutex` / `std::shared_lock` | `repository.cppm` reader-writer concurrency | Ch 12 |
| RAII | `Socket` wrapper, `jthread` auto-join, `scoped_lock` | Ch 1 |
| Move semantics | `Socket` move-only, handler composition | Ch 1 |
| `std::from_chars` | `handlers.cppm` path parameter parsing | Ch 5 |
| `starts_with` | `router.cppm` prefix matching, `http.cppm` | C++20/23 |
| `std::function` (type erasure) | `middleware.cppm`, `router.cppm` handler storage | Ch 10 |
| Range-based `for` with structured bindings | `http.cppm` header iteration | Ch 5 |
| `std::atomic` | `repository.cppm` ID generation | Ch 12 |

## Architecture & Best Practices

### Layered Design (Ch 9, 22)

```
main.cpp          — Wire layers, manage shutdown (import declarations)
  ├── handlers    — Translate HTTP ↔ domain (error boundary)
  ├── repository  — Thread-safe domain storage
  ├── task        — Domain value type with invariants
  ├── middleware   — Cross-cutting concerns (logging, validation)
  ├── router      — Request → handler dispatch
  └── http        — Minimal TCP server + request/response types
```

### Module Structure

Each layer is a named module (`export module webapi.<name>`). Consumers use
`import webapi.<name>` — no include paths, no header guards, no transitive
header pollution.

```
src/modules/
  ├── error.cppm       → export module webapi.error
  ├── json.cppm        → export module webapi.json
  ├── task.cppm        → export module webapi.task      (imports error, json)
  ├── http.cppm        → export module webapi.http
  ├── repository.cppm  → export module webapi.repository (imports error, task)
  ├── router.cppm      → export module webapi.router     (imports http)
  ├── middleware.cppm   → export module webapi.middleware (imports http)
  └── handlers.cppm    → export module webapi.handlers   (imports all above)
```

### Key Design Decisions

1. **Modules replace headers** (Ch 11): Each `.cppm` file is a self-contained
   module interface unit. Standard library headers appear in the global module
   fragment; POSIX headers are isolated there too.

2. **Ownership is visible in types** (Ch 1): `Socket` is move-only, `jthread`
   auto-joins, `scoped_lock` guards scope.

3. **Values, not entities** (Ch 2): `Task` has defaulted `<=>`, is copyable,
   and carries no hidden identity.

4. **Errors are typed** (Ch 3): `Result<T> = std::expected<T, Error>`. Every
   failure path returns an `Error` with code + detail. `[[nodiscard]]` prevents
   ignoring them.

5. **Parameters match intent** (Ch 4): Borrowed data uses `std::string_view`,
   owned data uses `std::string`, callables are constrained by concepts.

6. **Concepts at boundaries** (Ch 6): `JsonSerializable`, `JsonDeserializable`,
   `TaskUpdater` constrain template parameters with named requirements.

7. **Ranges for queries** (Ch 7): `find_completed()` uses `std::views::filter`,
   `serialize_array()` iterates any `input_range`.

8. **Compile-time where possible** (Ch 8): HTTP status mapping and method
   parsing are `constexpr`. Concept satisfaction is `static_assert`-ed.

9. **Middleware via composition** (Ch 10): Type-erased `std::function` handlers
   compose without inheritance.

10. **Thread safety via RAII locks** (Ch 12): `shared_mutex` with `shared_lock`
    for reads, `scoped_lock` for writes.

11. **Graceful shutdown** (Ch 14): `std::jthread` + `std::stop_token` for
    cooperative cancellation inside the server loop.

## Build Variants (Appendix B)

```bash
# Debug (default)
cmake -G Ninja -B build -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_BUILD_TYPE=Debug

# With AddressSanitizer + UBSan
cmake -G Ninja -B build-asan -DCMAKE_CXX_COMPILER=clang++-20 -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug

# With ThreadSanitizer
cmake -G Ninja -B build-tsan -DCMAKE_CXX_COMPILER=clang++-20 -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug

# Release with symbols
cmake -G Ninja -B build-rel -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

## Requirements

- **Clang 20+** with libc++ 20+ (for complete C++23 module and `std::jthread` support)
- **libc++ 20** headers and runtime (`libc++-20-dev`, `libc++abi-20-dev`)
- **CMake 3.28+** (for `FILE_SET CXX_MODULES` support)
- **Ninja** generator (required for CMake module dependency scanning)

## Project Structure

```
examples/web-api/
├── CMakeLists.txt                 # Build system (C++23, modules, warnings, sanitizers)
├── README.md                      # This file
├── src/
│   ├── modules/
│   │   ├── error.cppm             # Error types — std::expected, constexpr
│   │   ├── json.cppm              # JSON utils — concepts, ranges
│   │   ├── task.cppm              # Domain model — value semantics, validation
│   │   ├── repository.cppm        # Storage — thread-safe, RAII locks, ranges
│   │   ├── http.cppm              # HTTP types — RAII sockets, jthread server
│   │   ├── middleware.cppm        # Pipeline — type erasure, composition
│   │   ├── handlers.cppm          # Route handlers — error boundary translation
│   │   └── router.cppm            # Routing — pattern matching, dispatch
│   └── main.cpp                   # Entry point — wiring, shutdown, import decls
└── tests/
    ├── CMakeLists.txt
    ├── test_task.cpp              # Domain model tests
    ├── test_repository.cpp        # Storage tests (incl. concurrency)
    ├── test_http.cpp              # HTTP parsing tests
    ├── test_router.cpp            # Routing logic tests
    └── test_json.cpp              # JSON serialization tests
```
