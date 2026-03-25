# Chapter 6: Ranges, Views, and Functional Composition

> **Prerequisites:** Chapters 3--5. Value semantics (Chapter 3) determine what views can safely reference: a view into a temporary value type is a dangling reference. Concepts (Chapter 5) define how range algorithms constrain their inputs: `std::ranges::sort` requires `std::sortable`, not an unchecked `typename Iterator`. Move semantics (Chapter 1) govern how elements flow through pipelines without unnecessary copies.

## 6.1 The Production Problem

Hand-written loops are the default iteration tool in most C++ codebases. They work. They are also a recurring source of structural problems that compound as code ages:

- **Scattered intent.** A `for` loop that filters, transforms, and accumulates mixes three concerns in one block. A reviewer must read the entire body to determine what the loop does, whether it modifies its input, and where the result lands. When the loop grows, these concerns interleave further.
- **Resistance to composition.** Two loops that each perform a useful transformation cannot be combined without manually inlining one into the other. The result is a single, longer loop whose pieces cannot be tested or reused independently.
- **Intermediate allocations.** A pipeline that filters a vector, transforms the survivors, and takes the first ten results often materializes intermediate vectors at each step. The allocations are unnecessary when only the final result matters.
- **Iterator misuse.** Classic `<algorithm>` calls require begin/end iterator pairs. Passing iterators from different containers, using invalidated iterators after mutation, or confusing half-open ranges are defects that the type system does not prevent.
- **Index gymnastics.** Nested loops with manual index management -- `for (size_t i = 0; i < n; ++i)` -- are fertile ground for off-by-one errors, signed/unsigned comparison warnings, and iterator invalidation when the container is modified mid-loop.

The ranges library, stabilized in C++20 and extended in C++23, addresses these problems by treating ranges as first-class objects that can be composed, constrained, and lazily evaluated. It does not eliminate loops. It provides a vocabulary for expressing iteration pipelines where intent is visible, composition is mechanical, and the type system catches structural errors at compile time.

---

## 6.2 Classic Algorithms vs. Range Algorithms

### 6.2.1 What changes

The classic `<algorithm>` header operates on iterator pairs. The `<ranges>` header operates on range objects -- anything that provides `begin()` and `end()`. This eliminates the most common misuse pattern: passing iterators from different containers.

```cpp
#include <algorithm>
#include <ranges>
#include <vector>

std::vector<int> data = {5, 3, 1, 4, 2};

// Classic: iterator pair -- nothing prevents passing mismatched iterators.
std::sort(data.begin(), data.end());

// Range: the container is a single argument.
std::ranges::sort(data);
```

Range algorithms also accept projections -- a callable applied to each element before comparison or predicate evaluation. This replaces a common pattern where you write a custom comparator that extracts a field.

```cpp
struct Order {
    std::string customer;
    double total;
    std::chrono::system_clock::time_point timestamp;
};

std::vector<Order> orders = /* ... */;

// Classic: custom comparator to sort by total.
std::sort(orders.begin(), orders.end(),
          [](const Order& a, const Order& b) { return a.total < b.total; });

// Range with projection: intent is explicit.
std::ranges::sort(orders, std::less{}, &Order::total);
```

The projection version is shorter, but more importantly it separates two concerns: the comparison strategy (`std::less{}`) and the value to compare on (`&Order::total`). When you later need to sort by timestamp, you change the projection, not the comparator.

### 6.2.2 What stays the same

Range algorithms have the same complexity guarantees as their classic counterparts. `std::ranges::sort` is O(n log n). `std::ranges::find` is O(n). The underlying implementations are typically the same code paths with a range-based entry point.

Iterator categories carry over, repackaged as concepts: `std::input_iterator`, `std::forward_iterator`, `std::bidirectional_iterator`, `std::random_access_iterator`, `std::contiguous_iterator`. Passing a forward-only range to an algorithm that requires random access is a compile-time error, not a silent performance cliff.

### 6.2.3 Anti-pattern: wrapping ranges back into iterator pairs

```cpp
// Anti-pattern: extracting iterators from a range to use classic algorithms.
auto filtered = data | std::views::filter(predicate);
// BUG: std::sort requires random-access iterators.
// A filter_view provides bidirectional iterators at best.
// This will not compile, but the instinct to "just get iterators" leads
// to worse patterns: materializing into a vector solely to sort.
std::sort(filtered.begin(), filtered.end());
```

When you find yourself extracting iterators from a view to feed a classic algorithm, stop. Either the algorithm has a range overload in `std::ranges`, or the pipeline needs restructuring -- typically by materializing at the right boundary (see section 6.5).

---

## 6.3 Lazy View Pipelines

Views are lightweight, non-owning range adaptors that compute their elements on demand. They do not allocate memory for results. They do not modify the underlying data. They compose via the pipe operator (`|`).

### 6.3.1 Core view adaptors

The standard library provides a set of view adaptors that cover most production iteration patterns. All are in the `std::views` namespace.

```cpp
#include <ranges>
#include <vector>
#include <string>
#include <print>

std::vector<std::string> log_lines = /* thousands of raw log lines */;

// Pipeline: filter to errors, extract the message field, take first 20.
auto recent_errors = log_lines
    | std::views::filter([](const std::string& line) {
          return line.starts_with("ERROR");
      })
    | std::views::transform([](const std::string& line) {
          return line.substr(line.find(']') + 2);  // extract message after timestamp
      })
    | std::views::take(20);

for (const auto& msg : recent_errors) {
    std::println("{}", msg);
}
```

Each step in this pipeline is lazy. `filter` does not scan all lines up front. `transform` does not allocate a new vector of substrings. `take` does not count elements until iteration begins. When the loop requests the 20th element, evaluation stops. If the first 20 errors are in the first 100 lines, the remaining thousands are never touched.

Key adaptors and their roles:

| Adaptor | Purpose | Iterator category preserved? |
|---|---|---|
| `views::filter(pred)` | Yields elements where `pred` returns true | Degrades to at most bidirectional |
| `views::transform(fn)` | Applies `fn` to each element | Preserves category |
| `views::take(n)` | Yields the first `n` elements | Preserves category |
| `views::drop(n)` | Skips the first `n` elements | Preserves category for random-access |
| `views::split(delim)` | Splits a range on a delimiter | Forward only |
| `views::join` | Flattens a range of ranges | Degrades to at most bidirectional |
| `views::enumerate` | Pairs each element with its index (C++23) | Preserves category |
| `views::zip(r1, r2, ...)` | Produces tuples from parallel ranges (C++23) | Minimum of input categories |
| `views::chunk(n)` | Groups elements into chunks of size `n` (C++23) | Forward |
| `views::stride(n)` | Yields every nth element (C++23) | Preserves category |
| `views::cartesian_product` | All combinations from multiple ranges (C++23) | Forward |

### 6.3.2 Composing pipelines: reading order matters

A range pipeline reads left to right: data flows from the source, through each adaptor, to the consumer. This matches natural language order ("filter, then transform, then take") and reverses the nesting that function-call composition requires.

```cpp
// Function-call style: read inside-out.
auto result = take(transform(filter(data, pred), fn), 20);

// Pipe style: read left to right.
auto result = data | views::filter(pred) | views::transform(fn) | views::take(20);
```

The pipe style is not just syntactic sugar. It makes the pipeline's data flow reviewable at a glance. When a stage is added, removed, or reordered, the change is a single line in a diff, not a restructuring of nested parentheses.

### 6.3.3 Projection-based algorithms as an alternative to transform

When the goal is to apply an algorithm (sort, find, min, max) to a derived value without materializing the transformation, projections are cleaner than `views::transform` piped into an algorithm.

```cpp
struct Sensor {
    std::string id;
    double reading;
    bool is_valid;
};

std::vector<Sensor> sensors = /* ... */;

// Find the sensor with the maximum valid reading.
auto it = std::ranges::max_element(
    sensors | std::views::filter(&Sensor::is_valid),
    std::less{},
    &Sensor::reading
);
```

The projection (`&Sensor::reading`) tells `max_element` to compare readings. The filter view removes invalid sensors before comparison. No intermediate container is allocated.

---

## 6.4 `std::generator`: Coroutine-Based Range Sources

C++23 introduces `std::generator<T>`, a coroutine return type that produces a lazy range of values via `co_yield`. It bridges coroutines and the ranges library, enabling custom sequence production without writing a full iterator type.

### 6.4.1 Replacing hand-rolled iterators

Before `std::generator`, producing a custom lazy sequence required implementing an iterator class with `begin()`, `end()`, `operator++`, `operator*`, and the associated type aliases. This is roughly 50--80 lines of boilerplate for a simple sequence. A generator reduces it to the algorithm itself.

```cpp
#include <generator>
#include <cstdint>

// Produce Fibonacci numbers lazily, without upper bound.
std::generator<std::uint64_t> fibonacci() {
    std::uint64_t a = 0, b = 1;
    while (true) {
        co_yield a;
        auto next = a + b;
        a = b;
        b = next;
    }
}

// Consume: take first 20 Fibonacci numbers.
for (auto val : fibonacci() | std::views::take(20)) {
    std::println("{}", val);
}
```

The generator is an input range. It is single-pass: once an element is yielded, the coroutine advances and the previous value is gone. This makes generators composable with views but unsuitable as sources for algorithms that require forward or random-access iteration.

### 6.4.2 Memory-efficient data streaming

Generators excel at streaming data from external sources where materializing the full dataset is impractical or impossible.

```cpp
#include <generator>
#include <fstream>
#include <string>
#include <ranges>

// Stream lines from a file without loading it into memory.
std::generator<std::string> read_lines(std::filesystem::path path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        co_yield std::move(line);
    }
}

// Pipeline: read a multi-gigabyte log, filter, transform, take.
auto results = read_lines("/var/log/service.log")
    | std::views::filter([](const std::string& s) { return s.contains("WARN"); })
    | std::views::transform([](const std::string& s) { return parse_warning(s); })
    | std::views::take(1000);
```

At any point during iteration, exactly one line is in memory (plus whatever the view adaptors cache internally). This is the same streaming model that Unix pipes provide, expressed in type-safe C++ with lazy evaluation.

### 6.4.3 Generator lifetime constraints

A generator's coroutine frame is heap-allocated (by default) and owned by the `std::generator` object. The frame is destroyed when the generator object is destroyed. References yielded from a generator must not outlive the generator.

```cpp
// Anti-pattern: dangling reference from a generator.
std::generator<std::string_view> bad_lines(std::filesystem::path path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        co_yield line;  // BUG: yields a string_view into 'line',
                        // which is overwritten on the next iteration.
                        // Every string_view points to the same buffer.
    }
}
```

The fix is to yield by value (`std::generator<std::string>`) or to yield a `std::string_view` only when the underlying data has a stable address (e.g., memory-mapped file). This is a specific instance of the broader view lifetime problem discussed in section 6.5.

---

## 6.5 View Lifetime and Dangling: First-Class Engineering Concerns

Views are non-owning. They hold references or iterators into an underlying range. When that range is destroyed, the view dangles. The compiler provides some protection via `std::ranges::dangling`, but not all dangling scenarios are caught statically.

### 6.5.1 The `dangling` sentinel

Range algorithms that return iterators will return `std::ranges::dangling` instead when called with an rvalue range that does not model `borrowed_range`. This is a compile-time firewall against the most common dangling pattern.

```cpp
#include <ranges>
#include <vector>
#include <algorithm>

// This returns std::ranges::dangling, not an iterator.
// Attempting to dereference it is a compile error.
auto it = std::ranges::find(std::vector<int>{1, 2, 3}, 2);
// static_assert(std::same_as<decltype(it), std::ranges::dangling>);
```

A `borrowed_range` is a range whose iterators remain valid even after the range object is moved or destroyed. `std::string_view`, `std::span`, and `std::ranges::subrange` are borrowed ranges. `std::vector` is not, because destroying the vector invalidates its iterators.

### 6.5.2 Anti-pattern: capturing views of temporaries

```cpp
// Anti-pattern: view into a temporary vector.
auto get_data() -> std::vector<int>;

// BUG: the temporary returned by get_data() is destroyed at the semicolon.
// 'view' holds dangling iterators.
auto view = get_data() | std::views::filter([](int x) { return x > 0; });

for (auto val : view) {  // RISK: undefined behavior
    process(val);
}
```

The fix is to materialize the source before creating the view:

```cpp
auto data = get_data();  // owns the data
auto view = data | std::views::filter([](int x) { return x > 0; });

for (auto val : view) {  // safe: 'data' outlives 'view'
    process(val);
}
```

Alternatively, use `std::ranges::to<std::vector>()` (C++23) to materialize a pipeline into an owning container at the point where the pipeline's result must persist:

```cpp
// Materialize the filtered, transformed result into a vector.
auto results = get_data()
    | std::views::filter([](int x) { return x > 0; })
    | std::views::transform([](int x) { return x * 2; })
    | std::ranges::to<std::vector>();
// 'results' is a std::vector<int>. No dangling.
```

### 6.5.3 View invalidation after mutation

Views cache iterators internally for performance. `filter_view` caches the result of `begin()` so that subsequent calls do not rescan. If you mutate the underlying container after constructing the view, the cached state may be stale.

```cpp
std::vector<int> data = {1, 2, 3, 4, 5};
auto evens = data | std::views::filter([](int x) { return x % 2 == 0; });

// Force begin() caching.
auto first = *evens.begin();  // 2

// Mutate the underlying container.
data.insert(data.begin(), 0);

// RISK: the filter_view's cached begin() iterator may be invalidated.
// Behavior is undefined.
for (auto val : evens) { /* ... */ }
```

The rule: treat views as invalidated whenever the underlying range is mutated. If you need to mutate and re-filter, construct a new view.

---

## 6.6 Custom Range Adaptors

When the standard adaptors do not express a domain-specific transformation, you can write a range adaptor. The simplest approach is a function that accepts a range and returns a view pipeline.

### 6.6.1 Adaptor as a function

```cpp
#include <ranges>
#include <string_view>

// A domain-specific adaptor: extract non-empty, trimmed fields from CSV.
auto csv_fields(std::string_view line) {
    return line
        | std::views::split(',')
        | std::views::transform([](auto&& rng) {
              auto sv = std::string_view(rng);
              // Trim leading/trailing spaces.
              auto start = sv.find_first_not_of(' ');
              if (start == std::string_view::npos) return std::string_view{};
              auto end = sv.find_last_not_of(' ');
              return sv.substr(start, end - start + 1);
          })
        | std::views::filter([](std::string_view field) {
              return !field.empty();
          });
}

// Usage: read CSV, extract non-empty fields from each line.
for (auto field : csv_fields("alice, , bob, charlie")) {
    std::println("[{}]", field);  // [alice] [bob] [charlie]
}
```

This approach is pragmatic. It composes standard views, names the composition, and returns a lazy view. The returned type is an implementation detail (a nested view pipeline) that callers iterate over but do not name.

### 6.6.2 When to write a view class

Write a custom view class only when the standard adaptors cannot express the iteration logic -- typically when the adaptor needs internal state that does not map to a predicate or transformation function. Examples: a sliding-window view with overlap, a deduplication view that tracks the previous element, or a rate-limiting view that injects delays.

The `std::ranges::view_interface<Derived>` CRTP base provides default implementations of `empty()`, `operator bool`, `front()`, `back()`, and `operator[]` based on `begin()` and `end()`. Implementing a custom view requires:

1. A `view` class template that models `std::ranges::view`.
2. An iterator type that implements the appropriate iterator concept.
3. A range adaptor closure object if you want pipe syntax.

This is 100+ lines of boilerplate for a nontrivial view. Prefer composing existing adaptors unless the custom logic is reused widely enough to justify the maintenance cost.

---

## 6.7 Performance Characteristics

### 6.7.1 What you gain

Lazy evaluation avoids intermediate allocations. A pipeline of `filter | transform | take` does not allocate a filtered vector, then a transformed vector, then slice it. It evaluates one element at a time, pulling through the pipeline on each iteration step. For large datasets or streaming sources, this reduces peak memory from O(n) per stage to O(1).

Projection-based algorithms avoid temporary copies. Sorting by a projected member does not extract the members into a separate container; it evaluates the projection inline during comparison.

### 6.7.2 What you pay

Each layer of view indirection adds a function call per element. In a pipeline of three views, every `++` on the outer iterator calls `++` on the next inner iterator, which calls `++` on the next, and so on. The compiler can often inline this chain, but:

- **Debug builds** do not inline. A five-stage pipeline in a debug build is five function calls per element advance. This can make debugging iteration-heavy code noticeably slower than an equivalent hand-written loop.
- **Complex predicates in `filter`** are evaluated on every `++` of the filter iterator, not just once. A filter that calls into a database or performs I/O creates a hidden performance cliff because each iterator increment triggers the predicate.
- **`filter_view` degrades iterator category.** A random-access range piped through `filter` becomes at most bidirectional. Algorithms that depended on random access (binary search, nth_element) cannot operate on the filtered result without materialization.
- **`split_view`** on `std::string` produces subranges that are not contiguous `string_view`s in all implementations. Converting each subrange to `string_view` may require an additional scan.

### 6.7.3 Benchmarking guidance

Benchmark range pipelines against hand-written loops at the optimization level you ship (`-O2` or `/O2`). At `-O0`, pipelines will always look worse due to missed inlining. At `-O2`, the difference is typically within noise for straightforward pipelines (filter + transform + take). It widens for deeply nested or stateful views.

Use `-fno-omit-frame-pointer` and a profiler (perf, Instruments, VTune) to verify that the pipeline is actually inlined in hot paths. If the profiler shows view-adaptor frame entries in a hot loop, the compiler failed to inline, and you should consider materializing the intermediate result or rewriting the hot section as a loop.

---

## 6.8 When Pipelines Help vs. When They Obscure

### 6.8.1 Pipelines improve clarity when

- The data flow is a linear chain: filter, transform, accumulate. Each stage is a single, named concern.
- The pipeline replaces a loop body that mixes iteration logic with business logic.
- Laziness provides a real benefit: avoiding materialization, enabling early termination, or streaming from an unbounded source.
- The pipeline's stages are independently testable (each lambda or projection can be unit-tested in isolation).

### 6.8.2 Pipelines obscure control flow when

- A stage has side effects (logging, metrics, mutation). Side effects in a lazy pipeline execute at iteration time, not at pipeline-construction time. This is confusing and fragile.
- The pipeline requires `break`, `continue`, or early `return` based on cross-element state. Ranges have no equivalent of `break` inside a pipeline -- `take_while` is the closest, but it cannot express arbitrary control flow.
- Error handling must occur mid-pipeline. A transform that can fail produces an `expected<T, E>` or `optional<T>`, and subsequent stages must handle the failure case. The pipeline becomes a chain of `transform` + `filter` on `expected`, which is harder to read than a loop with explicit error handling.
- The pipeline is longer than five or six stages. Beyond this point, the type of the resulting view becomes a deeply nested template that degrades compiler diagnostics and debugger output.

### 6.8.3 The materialization boundary

The practical pattern is: use a pipeline for the transformation logic, then materialize the result into an owning container at the boundary where the result must persist, be passed to a non-range API, or be iterated multiple times.

```cpp
// Pipeline for transformation logic.
// Materialization for the API boundary.
auto active_user_ids = users
    | std::views::filter([](const User& u) { return u.is_active(); })
    | std::views::transform(&User::id)
    | std::ranges::to<std::vector>();

// 'active_user_ids' is a std::vector<UserId> that can be passed
// to APIs, stored, sorted, binary-searched, etc.
send_notification_batch(active_user_ids);
```

This pattern keeps the pipeline short, avoids lifetime hazards (the vector owns its data), and produces a concrete type that downstream code can work with without knowing about ranges.

---

## 6.9 Testing and Tooling

### 6.9.1 Testing view pipelines

Test each stage of a pipeline independently. Extract lambdas and projections into named callables and test them as pure functions.

```cpp
// Named predicate, testable in isolation.
constexpr auto is_valid_reading = [](const Sensor& s) {
    return s.is_valid && s.reading >= 0.0 && s.reading <= 1000.0;
};

// Named projection, testable in isolation.
constexpr auto extract_reading = &Sensor::reading;

// Pipeline assembled from tested components.
auto valid_readings = sensors
    | std::views::filter(is_valid_reading)
    | std::views::transform(extract_reading);
```

Test the composed pipeline by materializing results and comparing against expected output:

```cpp
#include <ranges>
#include <vector>
#include <cassert>

std::vector<Sensor> test_data = {
    {"s1", 42.0, true},
    {"s2", -1.0, true},    // invalid: negative
    {"s3", 100.0, false},  // invalid: not valid
    {"s4", 99.0, true},
};

auto results = test_data
    | std::views::filter(is_valid_reading)
    | std::views::transform(extract_reading)
    | std::ranges::to<std::vector>();

assert(results == std::vector<double>{42.0, 99.0});
```

### 6.9.2 Debugging pipelines

View types are deeply nested templates. A `filter_view<transform_view<ref_view<vector<int>>, lambda>, lambda>` is not readable in a debugger's watch window. Strategies:

- **Materialize at debug boundaries.** Insert a `| std::ranges::to<std::vector>()` temporarily to inspect intermediate results. Remove it after debugging.
- **Use `std::ranges::for_each` with a breakpoint.** Replace a range-based `for` loop with `std::ranges::for_each(pipeline, [](auto& x) { use(x); })` and set a breakpoint inside the lambda.
- **Compiler support.** GCC 14+ and Clang 18+ provide improved pretty-printing for view types in GDB and LLDB respectively. MSVC's natvis files handle the common view types. Verify your debugger version before assuming views are undebuggable.

### 6.9.3 Static analysis considerations

- Clang-Tidy's `bugprone-dangling-handle` and `bugprone-use-after-move` checks can catch some view-lifetime errors, but they do not understand all view-adaptor patterns. Do not rely on them as a substitute for lifetime reasoning.
- The `[[nodiscard]]` attribute on view-returning functions prevents silently discarding a pipeline result (a common mistake when transitioning from mutating algorithms to views).

---

## 6.10 Tradeoffs Summary

| Concern | Range pipeline | Hand-written loop |
|---|---|---|
| **Clarity** | High for linear data flows | Higher for complex control flow |
| **Composition** | Mechanical (pipe operator) | Manual (inline or refactor into functions) |
| **Laziness** | Built-in | Must be hand-implemented |
| **Memory** | O(1) per stage (no intermediate containers) | Depends on implementation |
| **Debug performance** | Poor (no inlining at -O0) | Normal |
| **Error handling** | Awkward mid-pipeline | Natural with early return |
| **Lifetime safety** | Requires discipline (non-owning views) | Implicit (loop body scope) |
| **Compiler diagnostics** | Nested template errors | Straightforward errors |
| **Iterator category** | Some views degrade category | Full control |

The decision is not "ranges vs. loops." It is where to draw the boundary between pipeline-style composition and imperative control flow in a given function. The best production code uses both, choosing pipelines for data transformation and loops for stateful, branching, or error-handling-heavy logic.

---

## 6.11 Review Checklist

Use these questions during code review for any change that introduces or modifies range-based code.

- [ ] **Does the view outlive its source range?** Trace the lifetime of the underlying container or data source. If the view is stored in a member variable or returned from a function, verify that the source outlives every use of the view.
- [ ] **Are temporaries materialized before creating views?** A view constructed from a temporary rvalue (e.g., `get_data() | views::filter(...)`) dangles immediately unless the pipeline is consumed in the same expression or materialized with `ranges::to`.
- [ ] **Is the pipeline under six stages?** Longer pipelines produce deeply nested types that degrade diagnostics and compile times. Split into named intermediate views or materialize at a natural boundary.
- [ ] **Are predicates and projections pure functions?** Side effects in `filter` or `transform` lambdas execute at iteration time, not construction time, and may execute a different number of times than expected (e.g., `filter` may evaluate the predicate multiple times for bidirectional iteration). Ensure predicates are idempotent and side-effect-free.
- [ ] **Is `filter` used on a hot path where iterator-category degradation matters?** `filter_view` degrades random-access ranges to bidirectional. If the downstream code needs random access (for binary search, parallel algorithms, etc.), materialize the filtered result into a container.
- [ ] **Are generators single-pass only?** A `std::generator` produces an input range. Code that iterates a generator twice consumes it on the first pass. If multi-pass is needed, materialize into a container.
- [ ] **Does the pipeline avoid mid-stream error handling?** If a transform stage can fail, consider whether a loop with explicit error handling is clearer than a pipeline that wraps results in `optional` or `expected`.
- [ ] **Is the pipeline tested via materialization?** Tests should materialize pipeline results with `ranges::to<vector>()` and compare against expected values. Do not test views by inspecting their types.
- [ ] **Has debug-build performance been considered?** If the pipeline runs in a latency-sensitive path, verify that debug-build iteration is acceptable, or provide a loop-based fallback under a debug-mode flag.
- [ ] **Are view-invalidation rules respected?** If the underlying container is mutated after a view is constructed, the view must be discarded and recreated. Cached iterators inside view adaptors are not updated on mutation.
