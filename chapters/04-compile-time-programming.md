# Chapter 4: Compile-Time Programming: constexpr, consteval, and Metaprogramming

> **Prerequisites:** Chapters 1–3. Compile-time code must respect the same ownership contracts (Chapter 1), failure boundaries (Chapter 2), and value-semantic invariants (Chapter 3) as runtime code. A `constexpr` function that silently copies a resource handle or a `consteval` constructor that skips an invariant check is not "better because it runs at compile time" — it is broken earlier.

## 4.1 The Production Problem

A network protocol library defines message types with numeric tag identifiers. Each tag maps to a parser function. The routing table — tag to parser — is built at startup, populated by a registration function called from scattered translation units. The system works until three problems compound:

1. **Startup latency.** The routing table is constructed during `main()`'s first milliseconds. In a latency-sensitive trading gateway, those milliseconds delay the first message. Worse, the construction order depends on static initialization across translation units — a fragile, unspecified ordering that has produced empty-table bugs after seemingly unrelated link-order changes.

2. **Silent misconfiguration.** A developer adds a new message type but forgets the registration call. The code compiles. The new message type is simply dropped at runtime with a generic "unknown tag" log line, buried among thousands of startup messages. The bug ships to staging.

3. **Redundant runtime checks.** Every message dispatch validates that the tag exists in the table, that the parser pointer is non-null, and that the message size falls within the parser's declared bounds. These checks execute millions of times per second, but the data they validate has not changed since startup. The checks are correct. They are also waste.

The question this chapter addresses is not "can this computation run at compile time?" — the language has made that increasingly easy. The question is "should it?" Moving computation to compile time can eliminate entire classes of startup bugs, remove runtime overhead, and turn misconfigurations into compiler errors. It can also make code harder to debug, inflate build times, and create maintenance burdens that exceed the runtime savings. The engineering judgment matters more than the syntax.

## 4.2 Naive or Legacy Approach

### 4.2.1 Anti-pattern: Runtime Table Built from Scattered Registrations

The pre-C++17 approach typically uses a self-registering pattern:

```cpp
// Anti-pattern: runtime routing table with scattered registration
using ParserFn = std::expected<Message, ParseError>(*)(std::span<const std::byte>);

class MessageRegistry {
public:
    static MessageRegistry& instance() {
        static MessageRegistry reg;
        return reg;
    }

    void register_parser(uint16_t tag, ParserFn fn, size_t min_size, size_t max_size) {
        // RISK: called during static init — order is unspecified across TUs
        entries_[tag] = {fn, min_size, max_size};
    }

    std::expected<Message, ParseError> dispatch(uint16_t tag,
                                                 std::span<const std::byte> data) const {
        auto it = entries_.find(tag);
        if (it == entries_.end())
            return std::unexpected{ParseError::unknown_tag};
        if (data.size() < it->second.min_size || data.size() > it->second.max_size)
            return std::unexpected{ParseError::bad_length};
        return it->second.fn(data);
    }

private:
    struct Entry { ParserFn fn; size_t min_size; size_t max_size; };
    std::unordered_map<uint16_t, Entry> entries_;
};

// In heartbeat.cpp — repeated for every message type
namespace {
    const bool registered = [] {
        MessageRegistry::instance().register_parser(
            0x01, &parse_heartbeat, 4, 4);
        return true;
    }();
} // BUG: link order can cause this to run before or after first dispatch
```

This pattern has well-known failure modes:

- **Static initialization order fiasco.** If `dispatch()` is called before all registration lambdas have executed, the table is incomplete. The symptom is intermittent: the same binary works in one link configuration and fails in another.
- **No compile-time validation.** Duplicate tags, overlapping size ranges, null function pointers — all are runtime errors. The compiler has all the information needed to detect them, but the design does not ask.
- **Hash table overhead on every dispatch.** The `unordered_map` lookup, branch prediction costs, and cache misses are paid on every message, despite the table being immutable after startup.

## 4.3 Modern C++ Approach

### 4.3.1 `constexpr` Functions: The Baseline Tool

A `constexpr` function can be evaluated at compile time if its arguments are compile-time constants, but it remains callable at runtime. This dual nature is the feature, not a limitation — the same validation logic serves both contexts.

```cpp
struct ParserEntry {
    uint16_t tag;
    ParserFn fn;
    size_t min_size;
    size_t max_size;
};

constexpr bool validate_table(std::span<const ParserEntry> entries) {
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].fn == nullptr)
            return false; // null parser
        if (entries[i].min_size > entries[i].max_size)
            return false; // inverted bounds
        for (size_t j = i + 1; j < entries.size(); ++j) {
            if (entries[i].tag == entries[j].tag)
                return false; // duplicate tag
        }
    }
    return true;
}
```

The O(n^2) duplicate check is deliberate. At compile time, n is the number of message types in the protocol — typically dozens, not thousands. The quadratic scan compiles in microseconds and avoids pulling in a hash set, which would be overkill for a compile-time check on a small dataset. This is a recurring judgment call: algorithms that are unacceptable at runtime with large n are fine at compile time with small n.

### 4.3.2 `consteval`: Mandatory Compile-Time Evaluation

`consteval` (C++20) forces a function to be evaluated at compile time. If the arguments are not compile-time constants, the program is ill-formed. Use `consteval` when a runtime call is always a bug — when the function exists solely to produce a compile-time artifact.

```cpp
consteval auto build_dispatch_table() {
    constexpr ParserEntry raw_entries[] = {
        {0x01, &parse_heartbeat,     4,    4},
        {0x02, &parse_new_order,    32,  256},
        {0x03, &parse_cancel,       16,   16},
        {0x04, &parse_execution,    48,  512},
    };

    // Validate at compile time — a bad entry is a compiler error, not a runtime log line
    static_assert(validate_table(raw_entries),
                  "dispatch table has null parsers, inverted bounds, or duplicate tags");

    // Build a sorted array for binary search at runtime
    std::array<ParserEntry, std::size(raw_entries)> sorted{};
    for (size_t i = 0; i < std::size(raw_entries); ++i)
        sorted[i] = raw_entries[i];

    // Compile-time insertion sort — fine for small n
    for (size_t i = 1; i < sorted.size(); ++i) {
        auto key = sorted[i];
        size_t j = i;
        while (j > 0 && sorted[j - 1].tag > key.tag) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = key;
    }

    return sorted;
}

// The table exists in read-only data. No startup cost. No initialization order.
constexpr auto dispatch_table = build_dispatch_table();
```

The dispatch function now uses binary search on a flat, sorted array — cache-friendly and branchless on modern hardware:

```cpp
std::expected<Message, ParseError> dispatch(uint16_t tag,
                                             std::span<const std::byte> data) {
    auto it = std::lower_bound(
        dispatch_table.begin(), dispatch_table.end(), tag,
        [](const ParserEntry& e, uint16_t t) { return e.tag < t; });

    if (it == dispatch_table.end() || it->tag != tag)
        return std::unexpected{ParseError::unknown_tag};
    if (data.size() < it->min_size || data.size() > it->max_size)
        return std::unexpected{ParseError::bad_length};
    return it->fn(data);
}
```

What changed:

- **Startup latency eliminated.** The table is embedded in the binary's read-only segment. There is no constructor, no registration, no ordering dependency.
- **Misconfiguration is a compile error.** A duplicate tag, a null parser, or inverted size bounds fails `static_assert` during the build. The bug cannot reach staging.
- **Dispatch is faster.** Binary search on a contiguous sorted array beats hash table lookup for small tables, especially under cache pressure.

### 4.3.3 `if constexpr`: Compile-Time Branching

`if constexpr` (C++17) discards the false branch at compile time. It replaces a large class of SFINAE gymnastics and tag-dispatch overloads. The key rule: the discarded branch must still be syntactically valid, but it is not instantiated. This means you can write branch-specific code that would fail type-checking for other types, as long as the check is on a dependent expression.

```cpp
template <typename T>
std::string serialize(const T& value) {
    if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(value);
    } else if constexpr (requires { value.to_string(); }) {
        return value.to_string();
    } else if constexpr (std::is_same_v<T, std::string>) {
        return value;
    } else {
        static_assert(false, "no serialization path for this type");
    }
}
```

The `static_assert(false, ...)` in the final branch is valid in C++23 — earlier standards required the assert condition to depend on a template parameter. This pattern gives a clean error message when an unsupported type reaches the function, rather than the cascading template instantiation failures you get from unconstrained overloads.

**When to prefer `if constexpr` over concepts.** Use `if constexpr` when you are implementing a single function that handles multiple categories of types with different strategies. Use concepts (Chapter 5) when you are constraining a template's interface — declaring what types are accepted at all. They are complementary: a concept-constrained function can still use `if constexpr` internally to optimize for specific type traits.

### 4.3.4 `constexpr` Containers and Algorithms (C++23)

C++23 made `std::vector` and `std::string` usable in `constexpr` context, along with most of `<algorithm>`. This is a significant expansion — prior to C++23, compile-time computation was limited to arrays and manual loops.

```cpp
consteval auto compute_crc32_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1u)));
        table[i] = crc;
    }
    return table;
}

constexpr auto crc32_table = compute_crc32_table();

constexpr uint32_t crc32(std::span<const std::byte> data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (auto b : data)
        crc = (crc >> 8) ^ crc32_table[(crc ^ static_cast<uint8_t>(b)) & 0xFF];
    return crc ^ 0xFFFFFFFFu;
}
```

However, `constexpr` allocation has a critical limitation: **memory allocated at compile time cannot escape into runtime.** A `constexpr` function can use `std::vector` internally as a workspace, but the vector must be destroyed before the function returns a value to runtime. The result must be copied into a non-allocating type like `std::array`.

```cpp
// Anti-pattern: trying to return compile-time allocated memory to runtime
consteval std::vector<int> make_primes(int n) {
    std::vector<int> primes;
    // ... populate ...
    return primes; // BUG: compile error — constexpr allocation cannot leak to runtime
}

// Correct: use vector as workspace, return array
consteval auto make_primes() {
    std::vector<int> workspace;
    for (int candidate = 2; workspace.size() < 100; ++candidate) {
        bool is_prime = true;
        for (int p : workspace) {
            if (p * p > candidate) break;
            if (candidate % p == 0) { is_prime = false; break; }
        }
        if (is_prime) workspace.push_back(candidate);
    }

    std::array<int, 100> result{};
    std::ranges::copy(workspace, result.begin());
    return result;
}

constexpr auto first_100_primes = make_primes();
```

This pattern — allocate into `std::vector` for convenience, then copy to `std::array` for the result — is the standard idiom for non-trivial compile-time computation in C++23.

### 4.3.5 `static_assert` and Type Traits: Compile-Time Contracts

`static_assert` turns a compile-time boolean into a hard error with a custom message. Combined with type traits and concepts, it enforces structural contracts that would otherwise be documented in comments and violated in practice.

```cpp
template <typename Clock>
class RateLimiter {
    static_assert(Clock::is_steady,
        "RateLimiter requires a steady clock — system_clock can jump backward");
    static_assert(std::is_nothrow_invocable_v<decltype(Clock::now)>,
        "Clock::now() must be noexcept for use in hot paths");

public:
    explicit RateLimiter(typename Clock::duration window, size_t max_events)
        : window_{window}, max_events_{max_events} {}

    bool allow() {
        auto now = Clock::now();
        while (!timestamps_.empty() && now - timestamps_.front() > window_)
            timestamps_.pop_front();
        if (timestamps_.size() >= max_events_)
            return false;
        timestamps_.push_back(now);
        return true;
    }

private:
    typename Clock::duration window_;
    size_t max_events_;
    std::deque<typename Clock::time_point> timestamps_;
};
```

The `static_assert` on `is_steady` prevents a subtle bug: if the system clock jumps backward (e.g., NTP adjustment), the rate limiter could think all timestamps are in the future and block indefinitely. Without the assertion, this bug surfaces only under specific clock conditions in production.

### 4.3.6 Template Metaprogramming: Where `constexpr` Cannot Reach

`constexpr` handles value computation. It does not handle type computation — selecting, transforming, or generating types at compile time. For that, template metaprogramming remains necessary.

A common production need: given a protocol buffer or IDL-generated set of message types, create a `std::variant` that can hold any of them, indexed by a compile-time map from tag to type.

```cpp
template <typename... Entries>
struct TagMap {};

template <uint16_t Tag, typename T>
struct TagEntry {
    static constexpr uint16_t tag = Tag;
    using type = T;
};

// Extract the variant type from a tag map
template <typename Map>
struct VariantFromMap;

template <typename... Entries>
struct VariantFromMap<TagMap<Entries...>> {
    using type = std::variant<typename Entries::type...>;
};

// Usage: define the protocol's type map once
using ProtocolMessages = TagMap<
    TagEntry<0x01, Heartbeat>,
    TagEntry<0x02, NewOrder>,
    TagEntry<0x03, Cancel>,
    TagEntry<0x04, Execution>
>;

using AnyMessage = VariantFromMap<ProtocolMessages>::type;
// AnyMessage is std::variant<Heartbeat, NewOrder, Cancel, Execution>
```

This is type-level computation: no `constexpr` function can produce a `std::variant` type from a parameter pack. The template machinery is the only tool available.

The discipline: use template metaprogramming for type manipulation, use `constexpr` for value computation. Mixing them — encoding values as types (`std::integral_constant`) and performing arithmetic through template specializations — was necessary before C++14 `constexpr` but is now an anti-pattern that inflates compile times and destroys readability.

```cpp
// Anti-pattern: encoding value computation as type computation
template <int N>
struct Factorial {
    // RISK: O(N) template instantiations — each produces debug info, symbols, and compiler state
    static constexpr int value = N * Factorial<N - 1>::value;
};
template <>
struct Factorial<0> {
    static constexpr int value = 1;
};

// Modern equivalent: one function, zero template instantiations
consteval int factorial(int n) {
    int result = 1;
    for (int i = 2; i <= n; ++i)
        result *= i;
    return result;
}
```

The `consteval` version compiles faster, produces better diagnostics, is debuggable with a standard debugger, and is readable by any C++ programmer. The template version requires understanding recursive instantiation, explicit specialization, and the mental model of computation-via-types. Reserve that complexity for problems that actually require type computation.

## 4.4 Tradeoffs and Boundaries

### 4.4.1 Build Time Impact

Every `constexpr` evaluation and every template instantiation runs inside the compiler. Heavy compile-time computation shifts work from the user's CPU to the developer's build infrastructure.

Concrete examples of where this matters:

- **Large lookup tables.** Generating a 64 KB lookup table at compile time is fast. Generating a 64 MB table at compile time may hit compiler memory limits or slow the build by minutes. The rule of thumb: if the data would be unreasonable to check into version control as a source file, it is probably unreasonable to generate at compile time.
- **Recursive template instantiation.** Each instantiation generates compiler state — types, symbols, debug information. Deep recursion (hundreds of levels) can exhaust compiler stack or memory. GCC and Clang default to a recursion depth of 900–1024; MSVC defaults to 500. Hitting these limits usually indicates a design problem, not a need to raise the limit.
- **`constexpr` allocation.** Every `std::vector::push_back` at compile time runs the allocator inside the compiler's constexpr evaluator. This is significantly slower than runtime allocation. Prefer pre-sizing when the element count is known.

Measurement matters more than intuition. Use `-ftime-trace` (Clang) or `-ftime-report` (GCC) to profile where build time is actually spent. The bottleneck is often header inclusion and template instantiation, not `constexpr` evaluation.

### 4.4.2 Debuggability

Code that runs only at compile time cannot be stepped through with a runtime debugger. When a `consteval` function produces a wrong result, your debugging tools are:

- **`static_assert` with a message.** Embed assertions at intermediate steps to narrow the failure.
- **Compiler error messages.** A deliberately introduced type error in the `consteval` path will print the offending value in the error message on some compilers — a hack, but effective.
- **Extract to a `constexpr` function.** Replace `consteval` with `constexpr` temporarily, call the function at runtime under a debugger, then restore `consteval` once the bug is fixed.

This debugging cost is real. It is one reason to keep `consteval` functions small, well-tested, and limited to computations where compile-time evaluation is a hard requirement, not a preference.

### 4.4.3 Maintenance Cost

`constexpr` code is a subset of C++. It cannot perform I/O, use `reinterpret_cast`, call non-`constexpr` functions, or use `thread_local` storage. These restrictions are reasonable for most compile-time computation, but they constrain evolution: a `constexpr` function that later needs to log, allocate dynamically beyond transient scope, or call a third-party library will lose its `constexpr` qualification, breaking all callers that depend on compile-time evaluation.

The guideline: make a function `constexpr` when it naturally falls within the constexpr subset and there is a concrete caller that benefits from compile-time evaluation. Do not speculatively mark functions `constexpr` on the assumption that someone might want compile-time evaluation someday. The `constexpr` qualifier is a public API commitment.

### 4.4.4 When NOT to Move Computation to Compile Time

**Configuration that changes between deployments.** If the routing table depends on a configuration file loaded at startup, it cannot be `constexpr`. Even if the current deployment is static, locking the table into the binary removes the ability to reconfigure without recompiling — a significant operational constraint.

**Data that is too large or too dynamic.** A machine learning model's weight matrix, a geographic database, a large rule engine — these do not belong in `constexpr`. The compiler is not a database engine.

**Code that needs runtime polymorphism.** Virtual function calls are not `constexpr` (though C++23 relaxes some restrictions). If the parser selection depends on runtime plugin loading, compile-time tables are insufficient.

**Code that teams cannot read.** If moving a computation to compile time requires template metaprogramming that only one engineer understands, the maintenance cost exceeds the runtime benefit. A readable runtime function that runs once at startup and is validated by tests is often the better engineering choice.

## 4.5 Testing and Tooling Implications

### 4.5.1 Testing `constexpr` and `consteval` Code

`constexpr` functions have a significant testing advantage: you can test them both at compile time and at runtime, catching bugs in both evaluation contexts.

```cpp
// Compile-time test: a failed static_assert is a build break
static_assert(factorial(0) == 1);
static_assert(factorial(5) == 120);
static_assert(factorial(10) == 3628800);

// Runtime test: same function, runtime assertions, works with sanitizers and debuggers
void test_factorial_runtime() {
    assert(factorial(0) == 1);
    assert(factorial(12) == 479001600);

    // Edge case: overflow behavior (depends on platform int width)
    // This test runs under UBSan to catch signed overflow
    assert(factorial(12) > 0);
}
```

For `consteval` functions, runtime testing requires a wrapper:

```cpp
consteval int checked_factorial(int n) {
    if (n < 0 || n > 12)
        throw "factorial argument out of range"; // compile-time error if triggered
    return factorial(n);
}

// Cannot call checked_factorial at runtime — wrap the result
static_assert(checked_factorial(5) == 120);
static_assert(checked_factorial(0) == 1);
// static_assert(checked_factorial(13) == ...); // compile error: throw in consteval
```

### 4.5.2 Build Time Profiling

Monitor compile-time overhead as part of continuous integration:

- **Clang:** `-ftime-trace` produces a Chrome trace JSON file. Open it in `chrome://tracing` or Perfetto. Look for long bars under "Source," "InstantiateFunction," and "ConstantEvaluate."
- **GCC:** `-ftime-report` prints a summary to stderr. Less visual but covers the same ground.
- **MSVC:** `/d1reportTime` (undocumented but widely used) or build insights via `vcperf`.

Set build-time budgets for translation units that contain significant `constexpr` or template-heavy code. A unit that takes 30 seconds to compile today will take 60 seconds after the next batch of message types is added, unless someone is watching.

### 4.5.3 Static Analysis

Clang-tidy checks relevant to compile-time code:

- `readability-function-cognitive-complexity` — flags `constexpr` functions that have grown too complex. Compile-time functions should be simple; complexity is a signal to reconsider the approach.
- `misc-no-recursion` — catches recursion in `constexpr` functions that could hit instantiation or evaluation depth limits.
- `cppcoreguidelines-avoid-magic-numbers` — literal values in compile-time tables deserve named constants or enums.
- `bugprone-easily-swappable-parameters` — particularly relevant for `constexpr` functions taking multiple numeric arguments.

### 4.5.4 Compiler Explorer as a Design Tool

For compile-time programming specifically, [Compiler Explorer](https://godbolt.org) is indispensable. It shows:

- Whether a `constexpr` variable was actually evaluated at compile time (look for constant values in the assembly, no constructor calls).
- The code generated for `if constexpr` branches — verify that dead branches are truly eliminated.
- Template instantiation results — how many specializations the compiler actually generated.

This is not just a learning tool. It is a design validation tool for production code. Before committing a compile-time optimization, verify on the reference compilers (GCC 14, Clang 18, MSVC 17.10) that the intended compile-time evaluation actually occurs.

## 4.6 Review Checklist

When reviewing code that uses compile-time programming, apply these checks:

- [ ] **Is compile-time evaluation justified?** Is there a concrete benefit — eliminated startup cost, prevented misconfiguration, removed runtime overhead — or is it speculative?
- [ ] **`constexpr` vs. `consteval`.** Should this function be callable at runtime? If yes, `constexpr`. If a runtime call is always a bug, `consteval`. Do not default to `consteval` without reason; it prevents runtime debugging.
- [ ] **`static_assert` at construction.** Are compile-time tables and constants validated with `static_assert`? Does the assertion message explain what went wrong, not just that something failed?
- [ ] **Build time impact.** Has the change been profiled with `-ftime-trace` or equivalent? Does the translation unit compile within the project's time budget?
- [ ] **Constexpr allocation does not escape.** If `std::vector` or `std::string` is used in `constexpr` context, is all compile-time allocation freed before the result is returned to runtime?
- [ ] **Value computation vs. type computation.** Is template metaprogramming used only for type manipulation? Are value computations expressed as `constexpr` functions, not recursive template instantiations?
- [ ] **Debuggability.** Can the function be temporarily converted to `constexpr` (from `consteval`) for runtime debugging? Are there `static_assert` checkpoints for intermediate results?
- [ ] **Configuration flexibility.** Does making this data compile-time lock out runtime reconfiguration that operations will need? Could the data change between deployments?
- [ ] **Readability.** Can a new team member understand this code in one reading? If not, is the compile-time constraint compelling enough to justify the complexity?
- [ ] **Invariants from prior chapters.** Does the compile-time code maintain ownership contracts (Chapter 1), propagate errors rather than silently discarding them (Chapter 2), and respect value semantics (Chapter 3)?
