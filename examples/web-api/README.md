# Web API Example — Modern C++ in Practice

A complete REST API for task management, built entirely with **C++23** features
and following the best practices from every part of the book.

## Quick Start

```bash
# Configure (requires GCC 13+ or Clang 18+)
cmake -B build -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build

# Run tests
cd build && ctest --output-on-failure

# Start the server
./build/web-api-server
```

The server listens on **port 8080**. Press `Ctrl+C` for graceful shutdown.

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
| `std::expected<T, E>` | `error.hpp`, `repository.hpp`, `handlers.hpp` | Ch 3 |
| `std::unexpected` | `error.hpp` | Ch 3 |
| `std::optional<T>` | `task.hpp`, `repository.hpp`, `http.hpp`, `json.hpp` | Ch 5 |
| `std::string_view` | Throughout — parameter passing, header lookup | Ch 4, 5 |
| `std::format` | All response construction, error messages, logging | Ch 5 |
| `std::variant` | Error code enum + detail (closed set of failures) | Ch 3 |
| Concepts (`concept`, `requires`) | `json.hpp`, `repository.hpp`, `middleware.hpp` | Ch 6 |
| `std::ranges` / `std::views` | `repository.hpp` filter/transform, `json.hpp` serialize | Ch 7 |
| `constexpr` / `consteval` | `error.hpp` HTTP status mapping, `http.hpp` method parsing | Ch 8 |
| `std::jthread` / `std::stop_token` | `main.cpp`, `http.hpp` server loop | Ch 14 |
| `std::stop_source` / `std::stop_callback` | `main.cpp` signal-to-thread forwarding | Ch 14 |
| `[[nodiscard]]` | All error-bearing returns | Ch 3, 9 |
| Designated initializers | `Task{}`, `Response{}`, `Request{}` construction | Ch 2 |
| `operator<=>` (spaceship) | `Task` defaulted three-way comparison | Ch 2 |
| `std::shared_mutex` / `std::shared_lock` | `repository.hpp` reader-writer concurrency | Ch 12 |
| RAII | `Socket` wrapper, `jthread` auto-join, `scoped_lock` | Ch 1 |
| Move semantics | `Socket` move-only, handler composition | Ch 1 |
| `std::from_chars` | `handlers.hpp` path parameter parsing | Ch 5 |
| `starts_with` | `router.hpp` prefix matching, `http.hpp` | C++20/23 |
| `std::function` (type erasure) | `middleware.hpp`, `router.hpp` handler storage | Ch 10 |
| Range-based `for` with structured bindings | `http.hpp` header iteration | Ch 5 |
| `std::atomic` | `repository.hpp` ID generation | Ch 12 |

## Architecture & Best Practices

### Layered Design (Ch 9, 22)

```
main.cpp          — Wire layers, manage shutdown
  ├── handlers    — Translate HTTP ↔ domain (error boundary)
  ├── repository  — Thread-safe domain storage
  ├── task        — Domain value type with invariants
  ├── middleware   — Cross-cutting concerns (logging, validation)
  ├── router      — Request → handler dispatch
  └── http        — Minimal TCP server + request/response types
```

### Key Design Decisions

1. **Ownership is visible in types** (Ch 1): `Socket` is move-only, `jthread` auto-joins,
   `scoped_lock` guards scope.

2. **Values, not entities** (Ch 2): `Task` has defaulted `<=>`, is copyable, and
   carries no hidden identity.

3. **Errors are typed** (Ch 3): `Result<T> = std::expected<T, Error>`. Every failure
   path returns an `Error` with code + detail. `[[nodiscard]]` prevents ignoring them.

4. **Parameters match intent** (Ch 4): Borrowed data uses `std::string_view`,
   owned data uses `std::string`, callables are constrained by concepts.

5. **Concepts at boundaries** (Ch 6): `JsonSerializable`, `JsonDeserializable`,
   `TaskUpdater` constrain template parameters with named requirements.

6. **Ranges for queries** (Ch 7): `find_completed()` uses `std::views::filter`,
   `serialize_array()` iterates any `input_range`.

7. **Compile-time where possible** (Ch 8): HTTP status mapping and method parsing
   are `constexpr`. Concept satisfaction is `static_assert`-ed.

8. **Middleware via composition** (Ch 10): Type-erased `std::function` handlers
   compose without inheritance.

9. **Thread safety via RAII locks** (Ch 12): `shared_mutex` with `shared_lock`
   for reads, `scoped_lock` for writes.

10. **Graceful shutdown** (Ch 14): `std::jthread` + `std::stop_token` +
    `std::stop_callback` chain from signal to server loop.

## Build Variants (Appendix B)

```bash
# Debug (default)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# With AddressSanitizer + UBSan
cmake -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug

# With ThreadSanitizer
cmake -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug

# Release with symbols
cmake -B build-rel -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

## Project Structure

```
examples/web-api/
├── CMakeLists.txt                 # Build system (C++23, warnings, sanitizers)
├── README.md                      # This file
├── include/webapi/
│   ├── error.hpp                  # Error types — std::expected, std::variant
│   ├── json.hpp                   # JSON utils — concepts, ranges
│   ├── task.hpp                   # Domain model — value semantics, validation
│   ├── repository.hpp             # Storage — thread-safe, RAII locks, ranges
│   ├── http.hpp                   # HTTP types — RAII sockets, jthread server
│   ├── middleware.hpp             # Pipeline — type erasure, composition
│   ├── handlers.hpp               # Route handlers — error boundary translation
│   └── router.hpp                 # Routing — pattern matching, dispatch
├── src/
│   └── main.cpp                   # Entry point — wiring, shutdown, jthread
└── tests/
    ├── CMakeLists.txt
    ├── test_task.cpp              # Domain model tests
    ├── test_repository.cpp        # Storage tests (incl. concurrency)
    ├── test_http.cpp              # HTTP parsing tests
    ├── test_router.cpp            # Routing logic tests
    └── test_json.cpp              # JSON serialization tests
```
