# Chapter 14: Testing, Fuzzing, Sanitizers, and Observability

*Prerequisites: Chapter 13 (build and diagnostic infrastructure must be in place before sanitizers and fuzzers can run effectively). Chapter 1 (ownership and lifetime). Chapter 2 (failure boundaries). Chapter 7 (shared state and synchronization).*

---

> **Prerequisites:** This chapter assumes your build system already produces sanitizer-instrumented binaries reliably (Chapter 13), that you have a working CI pipeline, and that your team understands ownership semantics (Chapter 1) and failure boundaries (Chapter 2). Sanitizers are not useful if you cannot build with them. Fuzzers are not useful if your code lacks well-defined input boundaries. Observability is not useful if your error model is unclear. Get the foundations right first.
>
> You should also be comfortable with the synchronization material from Chapter 7. Many of the bug classes discussed here — data races, lock-order inversions, use-after-free across threads — are concurrency failures that only surface under specific scheduling. The verification strategies in this chapter exist precisely because those failures resist deterministic reproduction.

---

## 14.1 The Production Problem

A C++ service passes its unit tests, its integration tests, and three weeks of staging traffic. Then a customer sends a 4 MB JSON blob with a deeply nested array, a coroutine resumes after its owning task has been cancelled, and a background thread reads a config field that another thread is tearing down. The resulting crash dump shows a stack corrupted beyond recovery. The post-mortem takes four days.

These are not exotic scenarios. They are the normal failure modes of production C++:

- **Undefined behavior** that compilers silently exploit, producing binaries that work until an optimization level or toolchain changes.
- **Boundary violations** that unit tests never exercise because the test inputs are small, well-formed, and hand-chosen.
- **Lifetime errors** that only manifest under specific interleaving of threads or shutdown sequences.
- **Silent corruption** that passes all assertions but produces wrong results — often discovered by customers, not engineers.

Conventional testing catches logic errors in code paths the author imagined. It does not catch the paths nobody imagined. The question for every component is: which bug classes must be caught mechanically before deployment, because they are too expensive or too nondeterministic to debug in production?

---

## 14.2 The Naive Approach: Example-Driven Tests Alone

Most C++ projects rely on a test suite structured like this:

```cpp
// Anti-pattern: tests that only cover the happy path with small inputs
TEST(JsonParser, ParsesSimpleObject) {
    auto result = parse_json(R"({"key": "value"})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->at("key"), "value");
}

TEST(JsonParser, RejectsEmptyInput) {
    auto result = parse_json("");
    ASSERT_FALSE(result.has_value());
}

// BUG: no test for deeply nested input, enormous strings,
// invalid UTF-8, null bytes mid-stream, or concurrent access
// to shared parser state.
```

This test suite verifies two code paths out of thousands. It provides confidence in the implementation for exactly the inputs the author thought to write down. The problems:

1. **Coverage is author-bounded.** You only test what you imagine. The attacker, the malformed network packet, and the edge-case customer do not share your imagination.
2. **No UB detection.** A test that passes does not mean the code is correct. Signed overflow, out-of-bounds reads, and use-after-free can produce "correct" output on your toolchain today and crash on a different optimization level tomorrow.
3. **No concurrency coverage.** Running a test on a single thread tells you nothing about data races. Thread Sanitizer exists because humans cannot enumerate interleavings.
4. **No production feedback loop.** When the service fails at 3 AM, the test suite provides no diagnostic infrastructure to connect the failure to a root cause.

Example-driven tests are necessary. They are not sufficient.

---

## 14.3 The Modern Approach: Layered Verification

Production-grade C++ verification is not a single tool. It is a strategy that layers four complementary techniques, each catching bug classes the others miss.

### 14.3.1 Structured Unit and Integration Tests

Good tests are not about quantity. They are about covering decision boundaries, error paths, and contract violations.

**Test structure that works at scale:**

```cpp
// Test the contract, not the implementation.
// Each test targets a specific behavioral guarantee.

TEST(SessionCache, EvictsLeastRecentlyUsedWhenFull) {
    SessionCache cache{.max_entries = 3};
    cache.insert("a", make_session());
    cache.insert("b", make_session());
    cache.insert("c", make_session());

    // Touch "a" to make it most-recently-used.
    [[maybe_unused]] auto _ = cache.get("a");

    // Insert a fourth entry. "b" should be evicted (least recent).
    cache.insert("d", make_session());

    EXPECT_FALSE(cache.get("b").has_value());  // evicted
    EXPECT_TRUE(cache.get("a").has_value());   // retained
    EXPECT_TRUE(cache.get("d").has_value());   // just inserted
}

TEST(SessionCache, GetFromEmptyCacheReturnsNullopt) {
    SessionCache cache{.max_entries = 100};
    EXPECT_FALSE(cache.get("nonexistent").has_value());
}

TEST(SessionCache, InsertAfterShutdownFails) {
    SessionCache cache{.max_entries = 10};
    cache.shutdown();
    auto result = cache.insert("a", make_session());
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CacheError::shut_down);
}
```

The principles:

- **Test behavioral contracts**, not internal state. If the test reaches into private members or depends on field layout, it will break on every refactor and protect nothing.
- **Cover error paths explicitly.** Every `std::expected`, every error code, every exception specification in a public API should have a test that forces that path.
- **Use typed test fixtures for invariant families.** If multiple container types must satisfy the same interface contract, write a single parameterized test suite. Google Test's `TYPED_TEST_SUITE`, Catch2's `TEMPLATE_TEST_CASE`, and Doctest's equivalent all support this.
- **Isolate external dependencies.** Tests that hit the network, filesystem, or database are integration tests. Label them, run them separately, and do not let their flakiness erode trust in the fast-test signal.

**Concurrency tests require deliberate scheduling pressure:**

```cpp
TEST(SessionCache, ConcurrentInsertAndEvictDoNotCorrupt) {
    SessionCache cache{.max_entries = 50};
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 10'000;

    std::vector<std::jthread> workers;
    workers.reserve(kThreads);

    std::latch start_gate{kThreads};

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            start_gate.arrive_and_wait();  // maximize contention
            for (int i = 0; i < kOpsPerThread; ++i) {
                auto key = std::format("t{}-i{}", t, i);
                cache.insert(key, make_session());
                cache.get(key);  // may or may not find it (eviction races)
            }
        });
    }
    // jthread joins automatically.

    // The invariant: cache size never exceeds max_entries.
    EXPECT_LE(cache.size(), 50u);
}
```

This test does not assert on specific interleaving outcomes. It asserts on the invariant that must hold regardless of scheduling. Run it under Thread Sanitizer (Section 3.3) to detect races the assertions cannot.

### 14.3.2 Fuzzing

Fuzzing explores the input space that humans never enumerate. A fuzzer generates millions of inputs per second, guided by coverage feedback, and reports any input that triggers a crash, assertion failure, timeout, or sanitizer violation.

**When to fuzz:** Any component that parses, deserializes, decodes, decompresses, or transforms untrusted input. Also: any component whose contract is complex enough that manual boundary enumeration is unreliable.

**libFuzzer target structure (Clang):**

```cpp
// fuzz_targets/fuzz_json_parser.cpp
#include "json_parser.h"
#include <cstdint>
#include <cstddef>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Construct a string_view without copying.
    std::string_view input{reinterpret_cast<const char*>(data), size};

    // parse_json is expected to handle arbitrary input without UB.
    // It may return an error — that is fine.
    // It must not crash, leak, or invoke undefined behavior.
    auto result = parse_json(input);

    // Optional: if parsing succeeded, round-trip and verify.
    if (result.has_value()) {
        auto serialized = serialize_json(*result);
        auto reparsed = parse_json(serialized);
        assert(reparsed.has_value() && "Round-trip must not fail");
    }

    return 0;
}
```

**Build and run:**

```bash
# Clang 18+ with libFuzzer and AddressSanitizer
clang++ -std=c++23 -g -O1 \
    -fsanitize=fuzzer,address,undefined \
    -fno-omit-frame-pointer \
    fuzz_targets/fuzz_json_parser.cpp \
    -o fuzz_json_parser \
    -L build/lib -ljson_parser

# Run with a corpus directory. The fuzzer saves interesting inputs.
mkdir -p corpus/json_parser
./fuzz_json_parser corpus/json_parser/ \
    -max_len=65536 \
    -timeout=5 \
    -jobs=4
```

**Key design decisions for fuzz targets:**

- **One target per entry point.** Do not combine multiple APIs into a single fuzz target. Coverage guidance works best when the feedback loop is tight.
- **Use a seed corpus.** Place valid and known-tricky inputs in the corpus directory. The fuzzer mutates from these starting points, which dramatically accelerates coverage.
- **Set `-max_len` deliberately.** Unbounded input length slows the fuzzer. Choose a length that exercises depth limits without wasting cycles on megabytes of entropy.
- **Enable sanitizers.** A fuzz target without AddressSanitizer and UndefinedBehaviorSanitizer is wasting most of its potential. The fuzzer finds the input; the sanitizer detects the bug.

**Structure-aware fuzzing with FuzzedDataProvider:**

For APIs that consume structured input (not raw bytes), use `FuzzedDataProvider` to decompose the fuzzer's byte stream into typed values:

```cpp
#include <fuzzer/FuzzedDataProvider.h>
#include "session_cache.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzedDataProvider fuzz(data, size);

    auto max_entries = fuzz.ConsumeIntegralInRange<size_t>(1, 1000);
    SessionCache cache{.max_entries = max_entries};

    while (fuzz.remaining_bytes() > 0) {
        auto op = fuzz.ConsumeIntegralInRange<int>(0, 2);
        auto key = fuzz.ConsumeRandomLengthString(64);

        switch (op) {
            case 0: cache.insert(key, make_session()); break;
            case 1: cache.get(key); break;
            case 2: cache.remove(key); break;
        }
    }

    // Invariant checks after arbitrary operation sequences.
    assert(cache.size() <= max_entries);
    return 0;
}
```

This approach is essential for stateful APIs where the bug surfaces only after a specific sequence of operations — a pattern that hand-written tests almost never cover exhaustively.

**CI integration:** Run fuzzers for a fixed time budget (e.g., 60 seconds per target) on every PR. Store and replay the accumulated corpus across runs. OSS-Fuzz provides continuous fuzzing infrastructure for open-source projects; internal teams need an equivalent.

### 14.3.3 Sanitizers

Sanitizers are compiler-inserted runtime checks that detect bug classes no amount of assertion-writing can catch. They are not optional tools for debugging sessions. They are CI infrastructure.

| Sanitizer | Detects | Overhead | Flag |
|---|---|---|---|
| AddressSanitizer (ASan) | Heap/stack buffer overflow, use-after-free, use-after-return, double-free, memory leaks | ~2x slowdown, ~3x memory | `-fsanitize=address` |
| UndefinedBehaviorSanitizer (UBSan) | Signed overflow, null dereference, misaligned access, shift past bit-width, invalid enum/bool | ~1.2x slowdown | `-fsanitize=undefined` |
| ThreadSanitizer (TSan) | Data races, lock-order inversions | ~5-15x slowdown, ~5-10x memory | `-fsanitize=thread` |
| MemorySanitizer (MSan) | Reads of uninitialized memory | ~3x slowdown | `-fsanitize=memory` |

**Critical constraint: ASan and TSan cannot coexist in the same binary.** You need separate CI configurations:

```cmake
# CMake presets for sanitizer builds
# In CMakePresets.json:
# "asan" preset: -DSANITIZER=address
# "tsan" preset: -DSANITIZER=thread
# "msan" preset: -DSANITIZER=memory

if(SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(SANITIZER STREQUAL "memory")
    add_compile_options(-fsanitize=memory -fno-omit-frame-pointer -fsanitize-memory-track-origins=2)
    add_link_options(-fsanitize=memory)
endif()
```

**ASan + UBSan** should run on every commit. The overhead is tolerable, and the bug classes they catch — buffer overflows, use-after-free, signed integer overflow, null pointer dereference — are the most common sources of security vulnerabilities and silent corruption in C++.

**TSan** should run on every commit that touches concurrent code. Its overhead is high, so some teams run the full test suite under TSan nightly and a targeted concurrent-test subset per commit. The important thing is that it runs. Data races are undefined behavior. They do not always crash. They sometimes produce correct results for months before silently corrupting data under a slightly different load pattern.

**MSan** is the hardest to deploy because it requires the entire dependency tree — including the C++ standard library — to be built with MSan instrumentation. Uninstrumented code produces false positives. Clang's `libc++` can be built with MSan; `libstdc++` typically cannot. If you can set it up, MSan catches a class of bugs (reading uninitialized heap memory) that no other tool reliably detects. If you cannot, ASan still catches the most critical memory errors.

**Suppression files and false positives:**

Sanitizers occasionally report issues in third-party code you cannot fix. Use suppression files, not code changes:

```
# asan_suppressions.txt
interceptor_via_fun:third_party_library_init

# tsan_suppressions.txt
race:ThirdPartyLogger::Write
```

Set `ASAN_OPTIONS`, `TSAN_OPTIONS`, etc. in your CI environment:

```bash
export ASAN_OPTIONS="suppressions=asan_suppressions.txt:halt_on_error=1:detect_leaks=1"
export TSAN_OPTIONS="suppressions=tsan_suppressions.txt:halt_on_error=1:second_deadlock_stack=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
```

Set `halt_on_error=1`. Sanitizers that log warnings and continue are sanitizers that get ignored.

### 14.3.4 Observability: Debugging What Tests Cannot Catch

Tests and sanitizers run before deployment. Observability operates after deployment — and sometimes it is the only way to diagnose failures that depend on real traffic patterns, hardware behavior, or timing.

Observability is not logging. It is structured diagnostic data that connects a production failure to a root cause without requiring reproduction.

**The three signals:**

1. **Structured logs** with correlation IDs, timestamps, and machine-parseable fields. Not `printf`-style string interpolation.
2. **Metrics** (counters, histograms, gauges) for aggregate behavior: request rate, error rate, latency distribution, allocation rate, queue depth.
3. **Distributed traces** that follow a request across thread, coroutine, and service boundaries.

**Structured logging in C++:**

```cpp
#include <source_location>
#include <format>

enum class LogLevel : uint8_t { debug, info, warn, error, fatal };

struct LogEntry {
    LogLevel level;
    std::string_view message;
    std::string_view trace_id;
    std::source_location location;
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now();
};

// Usage in application code:
void handle_request(const Request& req, std::string_view trace_id) {
    auto result = process(req);
    if (!result.has_value()) {
        log(LogEntry{
            .level = LogLevel::error,
            .message = std::format("processing failed: {}",
                                   result.error().message()),
            .trace_id = trace_id,
            .location = std::source_location::current(),
        });
        // respond with error
        return;
    }
    // ...
}
```

`std::source_location` (C++20) eliminates the `__FILE__`/`__LINE__` macro pattern and integrates cleanly with structured log types. The trace ID propagates through the call chain so that every log entry for a single request can be correlated in your log aggregation system.

**Metrics for C++ services:**

Expose counters and histograms at decision boundaries:

```cpp
// Lightweight metric interface — back it with Prometheus, StatsD,
// OpenTelemetry, or whatever your infrastructure provides.

struct ServiceMetrics {
    Counter requests_total;
    Counter requests_failed;
    Histogram request_latency_ms;
    Gauge active_connections;
    Counter allocations_total;   // from a custom allocator
    Counter cache_evictions;
};

void handle_request(const Request& req, ServiceMetrics& metrics) {
    metrics.requests_total.increment();
    metrics.active_connections.increment();
    auto guard = scope_exit([&] { metrics.active_connections.decrement(); });

    auto start = std::chrono::steady_clock::now();
    auto result = process(req);
    auto elapsed = std::chrono::steady_clock::now() - start;

    metrics.request_latency_ms.observe(
        std::chrono::duration<double, std::milli>(elapsed).count());

    if (!result.has_value()) {
        metrics.requests_failed.increment();
    }
}
```

The metrics that matter most in C++ services are often allocation-related: allocation rate, arena reset frequency, peak memory, and fragmentation. These are invisible to application-level logic but determine whether the service survives under load.

**What to instrument in C++ specifically:**

- Custom allocator statistics (bytes allocated, high-water mark, fragmentation).
- Lock contention: time spent waiting on mutexes. If you use `std::mutex`, wrap it with timing.
- Queue depths in pipeline stages (Chapter 9). A growing queue is the earliest signal of backpressure failure.
- Thread pool utilization: how many workers are active versus idle.
- Destructor timing for expensive teardown paths. Slow destructors during shutdown are a common source of timeout-triggered crashes.

---

## 14.4 Tradeoffs and Boundaries

**Sanitizers are not free.** ASan doubles memory usage. TSan can impose a 15x slowdown. Running the full test suite under every sanitizer on every commit may be prohibitively slow for large projects. The common compromise: ASan+UBSan on every commit, TSan on a targeted subset per commit and the full suite nightly.

**Fuzzing requires maintenance.** Fuzz targets rot like any other test. When APIs change, targets must be updated. Corpora accumulate inputs that no longer exercise interesting paths after refactoring. Assign ownership of fuzz targets to the teams that own the underlying code.

**MSan is operationally expensive.** It requires a fully instrumented dependency tree. For many teams, the cost of maintaining an MSan build exceeds the bug-detection value. Valgrind's Memcheck is an alternative that does not require recompilation but runs 20-50x slower.

**Observability adds runtime cost.** Every metric increment, every structured log entry, every trace span has a cost. In latency-sensitive paths, conditional compilation or sampling is necessary. The trap is instrumenting everything in development, then disabling it in production — exactly where you need it.

**Coverage metrics lie.** Line coverage tells you which code was executed, not which contracts were verified. 90% line coverage with no assertion density is less useful than 60% coverage with strong invariant checks. Use coverage to find code that is never tested, not to declare that tested code is correct.

**The sanitizer gap on MSVC.** As of MSVC 17.10+, ASan is supported but TSan and MSan are not. If your production target is Windows, you face a harder problem: data-race detection requires either cross-compilation to Clang for CI or reliance on Intel Inspector, Dr. Memory, or similar external tools. Do not skip race detection because your primary compiler does not support TSan. Find another way.

---

## 14.5 Testing and Tooling Implications

### 14.5.1 CI Pipeline Structure

A minimal but effective CI matrix for a C++ project:

| CI Job | Compiler | Sanitizer | Test Scope | Frequency |
|---|---|---|---|---|
| Fast tests | GCC 14 | None | Unit + integration | Every commit |
| ASan + UBSan | Clang 18 | ASan + UBSan | Unit + integration | Every commit |
| TSan | Clang 18 | TSan | Concurrency tests | Every commit (subset) + nightly (full) |
| Fuzz | Clang 18 | ASan + UBSan | Fuzz targets, 60s each | Every PR + continuous |
| MSan (optional) | Clang 18 | MSan | Unit + integration | Nightly |
| Coverage | GCC 14 | None | Unit | Weekly (reporting only) |

### 14.5.2 Test Organization

Separate tests by what they verify and how long they take:

```
tests/
├── unit/               # Fast, isolated, no I/O
├── integration/        # External dependencies, slower
├── concurrent/         # Specifically exercises shared state
├── fuzz_targets/       # libFuzzer entry points
├── fuzz_corpora/       # Seed inputs for fuzz targets
│   └── json_parser/
└── sanitizer_suppressions/
    ├── asan.txt
    ├── tsan.txt
    └── ubsan.txt
```

### 14.5.3 Reproducing Sanitizer Failures

When a sanitizer reports a failure, the first priority is a minimal reproducer. Sanitizer output includes a stack trace with source locations. For fuzz-found bugs, the fuzzer saves the crashing input:

```bash
# Reproduce a fuzz-found crash
./fuzz_json_parser crash-abc123def456

# Minimize the crashing input
./fuzz_json_parser -minimize_crash=1 -exact_artifact_path=minimized.bin crash-abc123def456
```

For TSan-reported races, the output shows both access stacks. Map them to code, identify the shared state, and determine which synchronization was missing. TSan does not produce false positives on correctly compiled code (modulo known, documented limitations with signal handlers and certain lock-free patterns). If TSan reports a race, assume it is real until proven otherwise.

### 14.5.4 Property-Based Testing

Between hand-written examples and fuzzing sits property-based testing: generate random but typed inputs and verify invariants rather than specific outputs.

```cpp
// Using a property-based testing library (e.g., rapidcheck with Google Test)
#include <rapidcheck/gtest.h>
#include "interval_set.h"

RC_GTEST_PROP(IntervalSet, InsertThenContains,
              (int lower, int upper)) {
    RC_PRE(lower <= upper);  // precondition

    IntervalSet set;
    set.insert(lower, upper);

    // Property: every point in the inserted range is contained.
    auto midpoint = lower + (upper - lower) / 2;
    RC_ASSERT(set.contains(midpoint));
    RC_ASSERT(set.contains(lower));
    RC_ASSERT(set.contains(upper));
}

RC_GTEST_PROP(IntervalSet, InsertDoesNotExceedBounds,
              (std::vector<std::pair<int, int>> intervals)) {
    IntervalSet set;
    int global_min = std::numeric_limits<int>::max();
    int global_max = std::numeric_limits<int>::min();

    for (auto [lo, hi] : intervals) {
        if (lo > hi) std::swap(lo, hi);
        set.insert(lo, hi);
        global_min = std::min(global_min, lo);
        global_max = std::max(global_max, hi);
    }

    if (!intervals.empty()) {
        RC_ASSERT(!set.contains(global_min - 1));
        RC_ASSERT(!set.contains(global_max + 1));
    }
}
```

Property-based tests are particularly effective for data structures, serialization round-trips, and mathematical invariants. They complement fuzzing: fuzzing explores raw byte-level input space; property-based testing explores typed value space with shrinking for minimal counterexamples.

---

## 14.6 Review Checklist

### 14.6.1 Test Coverage and Structure

- [ ] Every public API entry point has at least one test for its success path and one for each documented error condition.
- [ ] Tests assert on behavioral contracts, not internal state.
- [ ] Concurrent data structures have stress tests that run under TSan.
- [ ] Flaky tests are quarantined and tracked, not ignored or retried into silence.

### 14.6.2 Fuzzing

- [ ] Every parser, deserializer, and codec has a fuzz target.
- [ ] Fuzz targets are built with ASan + UBSan instrumented.
- [ ] A seed corpus exists and is checked into version control.
- [ ] Fuzz targets run in CI with a minimum time budget per target.
- [ ] Crashing inputs are minimized and converted to regression tests.

### 14.6.3 Sanitizers

- [ ] ASan + UBSan run on the full test suite for every commit.
- [ ] TSan runs on concurrency-related tests for every commit and the full suite nightly.
- [ ] `halt_on_error=1` is set for all sanitizers in CI. Warnings are not tolerated.
- [ ] Suppression files are version-controlled, reviewed, and annotated with rationale.
- [ ] MSan or an equivalent (Valgrind) runs at least nightly if the dependency tree supports it.
- [ ] MSVC builds have an alternative race-detection strategy if TSan is unavailable.

### 14.6.4 Observability

- [ ] Structured logging is in place with correlation IDs that span request lifetimes.
- [ ] Key decision boundaries emit metrics: error rates, latency histograms, queue depths.
- [ ] Custom allocator statistics are exported if custom allocators are used.
- [ ] Lock contention and thread pool utilization are measured in concurrent components.
- [ ] Slow destructor paths are identified and instrumented.
- [ ] Observability overhead is measured and bounded in latency-critical paths.

### 14.6.5 Process

- [ ] CI runs sanitizer builds as blocking checks, not advisory.
- [ ] Fuzz corpus is preserved across CI runs (not rebuilt from seed each time).
- [ ] New fuzz-found or sanitizer-found bugs produce a regression test before the fix is merged.
- [ ] Coverage reports are used to find untested code, not to declare tested code correct.
- [ ] The team has a documented policy for which sanitizers and fuzz budgets apply to which components.
