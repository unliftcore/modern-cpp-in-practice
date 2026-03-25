# Chapter 5: Interfaces, APIs, and Dependency Direction

*Prerequisites: Chapters 1–2 (ownership and failure policy must be stable before interface shape can be evaluated).*

> **Prerequisites:** This chapter assumes you have an ownership model you can articulate (Chapter 1) and a failure policy that distinguishes between recoverable errors and programming bugs (Chapter 2). Without those, interface design degenerates into guesswork: callers cannot know what they are responsible for, and implementers cannot know what they are allowed to assume. If your ownership or error contracts are still ad hoc, fix them first. Nothing in this chapter compensates for that.
>
> You should also be comfortable with value semantics and invariant design (Chapter 3) and have at least passing familiarity with concepts and constrained templates (Chapter 4), since both arise when designing customization points.

## 5.1 The Production Problem

Large C++ systems do not usually fail because individual functions are wrong. They fail because interfaces between components encode too many assumptions, expose too much internal structure, or point dependencies in a direction that makes future change expensive.

The symptoms are familiar:

- A library header pulls in 40 transitive includes, and every consumer rebuilds when an internal data structure changes.
- A callback signature bakes in a concrete type, so adding a second implementation requires refactoring every call site.
- A "utility" module depends on the application's configuration types, creating a cycle that prevents unit testing without dragging in the entire service.
- A public API returns a raw pointer, and three teams have different opinions about who frees it.

These are not language bugs. They are design decisions that seemed harmless locally but compounded into coupling that resists change. The cost shows up months or years later: a refactoring that should take a day takes a quarter, because the boundary was never a boundary — it was a window into the implementation.

The core tension is between **expressiveness** and **commitment**. Every type, parameter, or return value in a public interface is a promise. The more you expose, the more you promise. The art is exposing enough capability for callers to do their work without committing to representation, policy, or lifetime details that belong on the other side of the wall.

## 5.2 The Naive and Legacy Approach

### 5.2.1 God headers and transitive coupling

A common legacy pattern is the "convenience header" that re-exports everything:

```cpp
// Anti-pattern: god header that couples every consumer to every internal type
// service/include/service/service.h
#pragma once

#include "service/config.h"         // pulls in YAML parser types
#include "service/metrics.h"        // pulls in prometheus client
#include "service/internal/cache.h" // pulls in LRU implementation details
#include "service/internal/codec.h" // pulls in protobuf generated headers

namespace service {

struct ServiceContext {
    Config config;                          // BUG: exposes YAML node types to consumers
    MetricsRegistry metrics;                // BUG: couples consumers to metrics library
    internal::LruCache<std::string, Blob> cache;  // RISK: internal type in public surface
    // ...
};

// Every consumer includes this header and transitively depends on
// YAML, prometheus, protobuf, and the LRU implementation.
ServiceContext& get_global_context();

}  // namespace service
```

The cost is not just compile time — though that is bad enough. The cost is that any change to the cache eviction policy, the metrics library, or the config representation requires touching every translation unit that includes this header. The internal decision has become a system-wide constraint.

### 5.2.2 Stringly-typed and void-pointer interfaces

Legacy C++ and C-interop APIs often collapse type information to avoid header dependencies:

```cpp
// Anti-pattern: type-erased interface that discards safety
class PluginHost {
public:
    void register_handler(const std::string& event_name,
                          void (*callback)(void* context, const void* event_data),
                          void* context);  // RISK: no ownership, no type safety
};
```

This avoids coupling to concrete types, but at the cost of type safety, lifetime clarity, and debuggability. The `void*` context has no owner, no destructor, and no way for a code reviewer to verify correctness without reading the implementation of every callback. The `event_data` pointer has no schema; misinterpreting its layout is a silent corruption bug.

### 5.2.3 Dependency arrows that point the wrong way

A subtler failure mode is when low-level components depend on high-level policy:

```cpp
// Anti-pattern: low-level utility depends on application-layer types
// util/rate_limiter.h
#include "app/config.h"   // BUG: utility depends on application
#include "app/metrics.h"  // BUG: creates circular dependency risk

class RateLimiter {
    AppConfig& config_;   // RISK: ties utility lifetime to application object
    AppMetrics& metrics_;
public:
    RateLimiter(AppConfig& config, AppMetrics& metrics);
    bool allow(const std::string& key);
};
```

The rate limiter cannot be tested without constructing the full application configuration. It cannot be reused in another service. It cannot be compiled independently. The dependency arrow points from the general to the specific, which is backwards.

## 5.3 The Modern C++ Approach

### 5.3.1 Principle: narrow interfaces, wide implementations

A good interface exposes *what* a component can do, not *how* it does it or *what* it depends on. In C++ terms, this means:

1. **Parameter types express requirements, not representations.** Take `std::span<const std::byte>` instead of `const std::vector<uint8_t>&`. Take `std::string_view` instead of `const std::string&`. Take a concept-constrained template parameter instead of a concrete callback type.

2. **Return types express ownership, not implementation.** Return `std::unique_ptr<T>` when transferring ownership. Return `T` when the value is cheap to move. Return `std::expected<T, E>` when failure is part of the contract (Chapter 2). Never return a raw pointer to a heap object.

3. **Headers expose the interface; sources own the implementation.** Use forward declarations, the Pimpl idiom, or abstract base classes to keep implementation types out of public headers.

### 5.3.2 Designing function signatures

A function signature is a contract. Every parameter type, every qualifier, every default argument is a commitment. Here is a concrete example: a component that parses a document from a byte buffer.

```cpp
// Version 1: over-committed
// Requires std::vector, forces a copy if caller has a span or mmap'd region
Document parse(const std::vector<uint8_t>& buffer);

// Version 2: right-sized input, clear ownership of output
// Accepts any contiguous byte range; caller retains their storage choice
Document parse(std::span<const std::byte> input);

// Version 3: adds failure channel per Chapter 2 policy
std::expected<Document, ParseError> parse(std::span<const std::byte> input);
```

Version 3 accepts any contiguous byte sequence (vector, array, mmap'd region, subspan) and returns a value with an explicit failure channel. The caller does not need to know anything about the parser's internal allocations or strategy.

For functions that need to accept callable objects — callbacks, predicates, completion handlers — prefer templates constrained by concepts over `std::function`:

```cpp
// std::function forces a type-erased heap allocation for non-trivial callables.
// Use it at ABI boundaries or when you must store heterogeneous callables.
void set_error_handler(std::function<void(std::error_code)> handler);

// For non-virtual, header-visible APIs, a constrained template avoids the
// allocation and indirection cost while preserving type safety.
template <std::invocable<std::error_code> Handler>
void set_error_handler(Handler&& handler);
```

The tradeoff is real: the template version cannot be hidden behind a compilation firewall, and it generates per-instantiation code. Use `std::function` (or `std::move_only_function` in C++23 when the callable is non-copyable) at stable binary boundaries. Use constrained templates at internal boundaries where compile-time cost is acceptable and runtime cost is not.

### 5.3.3 Hiding implementation: the Pimpl pattern in modern C++

The Pimpl (pointer to implementation) idiom moves private members behind an opaque pointer, so the public header contains only the interface:

```cpp
// connection_pool.h — public header, no implementation details leaked
#pragma once
#include <cstdint>
#include <memory>
#include <expected>
#include <string_view>

namespace infra {

enum class PoolError { exhausted, connect_failed, shutting_down };

class ConnectionPool {
public:
    struct Options {
        std::uint32_t max_connections = 64;
        std::uint32_t connect_timeout_ms = 3000;
    };

    explicit ConnectionPool(std::string_view endpoint, Options opts = {});
    ~ConnectionPool();

    ConnectionPool(ConnectionPool&&) noexcept;
    ConnectionPool& operator=(ConnectionPool&&) noexcept;

    // Non-copyable: pool owns OS resources
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    struct Connection {
        // opaque handle; details in source file
        struct Impl;
        std::unique_ptr<Impl> impl_;

        ~Connection();
        Connection(Connection&&) noexcept;
        Connection& operator=(Connection&&) noexcept;

        void execute(std::string_view query);
    };

    [[nodiscard]] std::expected<Connection, PoolError> acquire();
    void release(Connection conn);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace infra
```

```cpp
// connection_pool.cpp — all heavy headers live here
#include "infra/connection_pool.h"
#include <mutex>
#include <vector>
#include <queue>
#include <condition_variable>
// ... driver headers, internal types, etc.

namespace infra {

struct ConnectionPool::Impl {
    std::string endpoint;
    ConnectionPool::Options opts;
    std::mutex mu;
    std::queue</* internal handle */int> idle;
    std::uint32_t active_count = 0;
    bool shutting_down = false;
    // full implementation here
};

struct ConnectionPool::Connection::Impl {
    int raw_handle;  // platform-specific
};

ConnectionPool::ConnectionPool(std::string_view endpoint, Options opts)
    : impl_(std::make_unique<Impl>()) {
    impl_->endpoint = endpoint;
    impl_->opts = opts;
}

ConnectionPool::~ConnectionPool() = default;
ConnectionPool::ConnectionPool(ConnectionPool&&) noexcept = default;
ConnectionPool& ConnectionPool::operator=(ConnectionPool&&) noexcept = default;

ConnectionPool::Connection::~Connection() = default;
ConnectionPool::Connection::Connection(Connection&&) noexcept = default;
ConnectionPool::Connection& ConnectionPool::Connection::operator=(Connection&&) noexcept = default;

std::expected<ConnectionPool::Connection, PoolError> ConnectionPool::acquire() {
    std::lock_guard lock(impl_->mu);
    if (impl_->shutting_down)
        return std::unexpected(PoolError::shutting_down);
    if (impl_->idle.empty() && impl_->active_count >= impl_->opts.max_connections)
        return std::unexpected(PoolError::exhausted);

    Connection conn;
    conn.impl_ = std::make_unique<Connection::Impl>();
    // ... actually acquire from pool ...
    return conn;
}

void ConnectionPool::release(Connection conn) {
    std::lock_guard lock(impl_->mu);
    // ... return handle to idle queue ...
}

}  // namespace infra
```

The header includes only `<memory>`, `<expected>`, `<cstdint>`, and `<string_view>`. Consumers never see the mutex, the queue, the driver headers, or any internal type. Changing the pool's eviction strategy, switching database drivers, or adding instrumentation requires recompiling only `connection_pool.cpp`.

**Cost of Pimpl:** Every method call goes through a pointer indirection. Construction requires a heap allocation. Move operations are cheap (pointer swap), but copies are deleted or must be explicitly deep-copied. For hot-path, small objects, Pimpl is the wrong tradeoff. For boundary types that are constructed infrequently and crossed by meaningful operations, the compilation firewall is worth the indirection.

### 5.3.4 Abstract interfaces for dependency inversion

When a component needs a capability that could have multiple implementations — storage backends, transport layers, clock sources — use an abstract base class to define the contract and inject the implementation:

```cpp
// Abstract interface: defines what the component needs, not how it is provided
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::steady_clock::time_point now() const = 0;
};

// Abstract interface for a metric sink
class MetricSink {
public:
    virtual ~MetricSink() = default;
    virtual void record(std::string_view name, double value) = 0;
};

// The rate limiter depends only on abstractions, not on application types
class RateLimiter {
    std::uint32_t max_per_second_;
    Clock& clock_;
    MetricSink& metrics_;
    // internal state ...
public:
    RateLimiter(std::uint32_t max_per_second, Clock& clock, MetricSink& metrics)
        : max_per_second_(max_per_second), clock_(clock), metrics_(metrics) {}

    bool allow(std::string_view key);
};
```

The dependency arrows now point from the concrete (application) toward the abstract (interface), and from the specific (rate limiter) toward the general (clock, metrics). The rate limiter can be tested with a fake clock and a null metric sink. It can be reused in any service that provides those two capabilities.

**When to use virtual interfaces vs. templates:** Virtual dispatch costs one indirect call per invocation. For interfaces called millions of times per second on a hot path, this matters. For interfaces called once per request or once per operation, it does not. The compile-time alternative is a concept-constrained template:

```cpp
template <typename C>
concept ClockLike = requires(const C& c) {
    { c.now() } -> std::convertible_to<std::chrono::steady_clock::time_point>;
};

template <ClockLike ClockT, typename MetricSinkT>
class RateLimiter {
    // ...
};
```

This eliminates virtual dispatch but makes `RateLimiter` a template, which means its implementation must be visible in the header (or explicitly instantiated), it generates code per instantiation, and it cannot be hidden behind a stable ABI. Choose virtual interfaces when the boundary is between separately compiled components. Choose templates when the boundary is within a single library and you need to eliminate dispatch overhead.

### 5.3.5 Span, string_view, and non-owning vocabulary types

Non-owning view types are the primary tool for decoupling an interface from the caller's storage decisions:

| Type | Purpose | Precondition |
|---|---|---|
| `std::string_view` | Read-only access to contiguous character data | Referent must outlive the view |
| `std::span<T>` | Read or write access to contiguous elements | Referent must outlive the span |
| `std::span<const T>` | Read-only access to contiguous elements | Referent must outlive the span |
| `std::function_ref` (C++26) | Non-owning reference to a callable | Callable must outlive the ref |

The critical constraint is lifetime: a view does not extend the lifetime of the data it references. Returning a `string_view` into a local buffer, storing a `span` that outlives the container it came from, or capturing a `string_view` in a lambda that escapes the current scope are all dangling-reference bugs. Views are safe as function parameters (the caller's storage is alive for the duration of the call). They are dangerous as data members or return types unless the lifetime relationship is architecturally guaranteed and documented.

```cpp
// Safe: view as parameter, referent is alive for the call
std::expected<Config, ParseError> parse_config(std::string_view text);

// Anti-pattern: view as data member with no lifetime guarantee
struct Request {
    std::string_view path;  // RISK: who owns the string? When does it die?
    std::string_view body;  // RISK: same problem
};
```

If the struct must store the data, store `std::string`. If it borrows the data and the lifetime is architecturally bounded (e.g., within a single request-processing scope), document the invariant explicitly and consider `[[clang::lifetimebound]]` where your toolchain supports it so the compiler can help detect escaped views.

### 5.3.6 Customization points: hidden friends and tag_invoke

When designing an extensible interface — a serialization framework, a hashing protocol, a formatting customization — the question is how third-party types opt in. The modern C++ approach is to use hidden friends (friend functions defined inside the class body) or a `tag_invoke`-style customization point object.

Hidden friends keep the customization point out of general overload resolution:

```cpp
namespace serialization {

// Customization point object
inline constexpr struct serialize_fn {
    template <typename T>
    auto operator()(const T& value, Buffer& buf) const
        -> decltype(serialize(tag<serialize_fn>{}, value, buf)) {
        return serialize(tag<serialize_fn>{}, value, buf);
    }
} serialize{};

}  // namespace serialization

// A user type opts in via hidden friend
namespace app {

struct Measurement {
    double value;
    std::uint64_t timestamp_ns;

    // Hidden friend: found only via ADL on Measurement
    friend void serialize(serialization::tag<serialization::serialize_fn>,
                          const Measurement& m, serialization::Buffer& buf) {
        buf.write_f64(m.value);
        buf.write_u64(m.timestamp_ns);
    }
};

}  // namespace app
```

The advantage over a virtual interface is that the type does not inherit from anything, the customization is found at compile time, and there is no vtable or allocation. The advantage over a raw free function is that hidden friends do not pollute the enclosing namespace and cannot be accidentally matched by unrelated overload resolution.

**C++26 note:** `std::tag_invoke` was proposed but not accepted into the standard. The customization point object pattern shown above remains the practical idiom. If your codebase uses a `tag_invoke` library (e.g., from stdexec/P2300), it works the same way.

## 5.4 Tradeoffs and Boundaries

### 5.4.1 Pimpl vs. abstract base class vs. template

| Concern | Pimpl | Abstract base | Template |
|---|---|---|---|
| Compilation firewall | Yes | Partial (vtable in source) | No |
| Runtime dispatch cost | Pointer indirection | Virtual call | None (monomorphized) |
| ABI stability | Good (opaque pointer) | Good (vtable) | Poor (instantiation in header) |
| Testability (mocking) | Requires seam | Natural (inject mock) | Natural (inject fake type) |
| Code size impact | Minimal | Minimal | Grows per instantiation |
| Suitable for hot path | Marginal | Marginal | Yes |

There is no single winner. A production codebase will use all three, at different boundaries, for different reasons. The decision heuristic: if the boundary is between teams or binaries, prefer Pimpl or abstract base classes. If the boundary is within a module and performance matters, prefer templates. If you need to mock the dependency in tests, abstract base classes give you the seam naturally; Pimpl requires an additional injection mechanism.

### 5.4.2 When not to add an interface

Not every function call needs a seam. Introducing an abstract interface for something that has exactly one implementation and no plausible second one adds indirection, a vtable, a header, and a mock that exists only to satisfy test infrastructure. The cost is real: more types to maintain, more allocations, more cognitive load in reviews.

Add an interface boundary when:

- Two or more implementations already exist or are concretely planned.
- The dependency is on an external system (database, network, clock, filesystem) that must be faked in tests.
- The component will be consumed by a separate team or binary with a different release cadence.
- The current implementation leaks types that prevent the consumer from compiling independently.

Do not add an interface boundary when the only justification is "we might need it someday." Speculative abstraction ages as badly as speculative optimization.

### 5.4.3 The Dependency Rule

Dependencies should point from volatile (high-churn, application-specific) code toward stable (low-churn, general-purpose) code. In a layered system:

```
Application logic  →  Domain interfaces  →  Infrastructure abstractions  →  Platform / OS
```

Every arrow points toward greater stability. A domain interface should never include an application header. An infrastructure abstraction should never include a domain type. If you find a dependency arrow pointing the wrong way, that is the architectural bug — not the symptom that prompted you to look.

In C++ the enforcement mechanism is header inclusion. If `util/rate_limiter.h` includes `app/config.h`, the dependency is real regardless of what the architecture diagram says. Tooling such as `include-what-you-use`, or build systems that enforce layered visibility (Bazel's `visibility` attribute, CMake's `PRIVATE`/`PUBLIC`/`INTERFACE` link dependencies), catches violations mechanically.

## 5.5 Testing and Tooling Implications

### 5.5.1 Testing through interfaces

Abstract interfaces give you the injection seam. A test constructs a fake implementation and passes it to the component under test:

```cpp
// Test double: deterministic clock for unit tests
class FakeClock : public Clock {
    std::chrono::steady_clock::time_point current_;
public:
    explicit FakeClock(std::chrono::steady_clock::time_point start) : current_(start) {}

    std::chrono::steady_clock::time_point now() const override { return current_; }

    void advance(std::chrono::milliseconds delta) { current_ += delta; }
};

// Test: rate limiter allows exactly max_per_second requests
TEST(RateLimiterTest, RespectsLimit) {
    FakeClock clock{std::chrono::steady_clock::time_point{}};
    NullMetricSink metrics;
    RateLimiter limiter(10, clock, metrics);

    for (int i = 0; i < 10; ++i)
        EXPECT_TRUE(limiter.allow("client-a"));

    EXPECT_FALSE(limiter.allow("client-a"));

    clock.advance(std::chrono::seconds(1));
    EXPECT_TRUE(limiter.allow("client-a"));
}
```

The test runs in microseconds, requires no network, no configuration file, and no real clock. This is only possible because the rate limiter depends on abstractions (Clock, MetricSink) rather than on concrete application types.

### 5.5.2 Compile-time dependency auditing

Three tools catch interface violations before they become technical debt:

1. **`include-what-you-use` (IWYU):** Reports unnecessary transitive includes and missing direct includes. Run it as a CI check on public headers to prevent accidental coupling.

2. **Build system visibility rules:** In Bazel, `visibility = ["//mylib:__subpackages__"]` prevents internal targets from being included by outside consumers. In CMake, `target_link_libraries(mylib PRIVATE internal_dep)` keeps the dependency out of the public interface.

3. **Header self-containment checks:** Every public header should compile on its own. A CI step that compiles each header in isolation (a "header compile test") catches missing includes and undeclared dependencies.

### 5.5.3 API surface linting

Clang-tidy checks relevant to interface design:

- `modernize-use-nodiscard` — flags functions whose return values are likely to be accidentally discarded.
- `bugprone-easily-swappable-parameters` — flags adjacent parameters of the same type that callers are likely to transpose.
- `misc-non-private-member-variables-in-classes` — flags public data members in classes (as opposed to structs), which leak representation.
- `readability-const-return-type` — flags `const` on return types, which inhibits move semantics.

These are not style preferences. Each check catches a class of interface defect that is difficult to find in review and expensive to fix after consumers depend on the signature.

## 5.6 Review Checklist

Use this checklist when reviewing code that defines or modifies a public interface.

**Ownership and lifetime**

- [ ] Every raw pointer parameter or return type has a documented ownership contract. Prefer `std::unique_ptr`, `std::span`, or `std::string_view` where ownership or borrowing semantics can be expressed in the type system.
- [ ] No `string_view` or `span` is stored as a data member without an explicit, documented lifetime guarantee.
- [ ] Move constructors and move assignment operators are declared `noexcept` (or deleted with justification).

**Coupling and dependency direction**

- [ ] Public headers include only what is necessary for the declaration. Implementation headers are included only in source files.
- [ ] No dependency arrow points from a lower-level module toward a higher-level one.
- [ ] Internal types (`detail::`, `internal::`, anonymous namespace) do not appear in public function signatures or return types.
- [ ] Changes to the implementation do not require recompiling consumers (Pimpl or abstract base class used at binary boundaries).

**Parameter and return types**

- [ ] Input parameters use non-owning vocabulary types (`string_view`, `span<const T>`) where appropriate, rather than concrete container types.
- [ ] Output types express ownership: `T` for values, `unique_ptr<T>` for heap-allocated owning transfers, `expected<T, E>` for fallible operations.
- [ ] Functions with non-discardable results are marked `[[nodiscard]]`.
- [ ] Adjacent parameters of the same type are wrapped in distinct types or re-ordered to reduce transposition risk.

**Customization and extension**

- [ ] Customization points use hidden friends or customization point objects, not uncontrolled free-function overloading.
- [ ] Virtual interfaces have a virtual destructor and are non-copyable (or have an explicitly documented clone protocol).
- [ ] Template-based extension points are constrained by concepts with clear, tested requirements.

**Testing seams**

- [ ] External dependencies (network, filesystem, clock, randomness) are injected through an abstract interface, enabling deterministic testing.
- [ ] The component can be compiled and tested without including application-layer headers.
- [ ] There is at least one test that exercises the interface through a mock or fake implementation of each injected dependency.
