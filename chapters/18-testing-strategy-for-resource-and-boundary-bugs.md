# Testing Strategy for Resource and Boundary Bugs

Most expensive C++ bugs are not "algorithm returns the wrong number" bugs. They are resource and boundary bugs: a file descriptor that stays open on an error path, a temporary file that survives a failed commit, a cancellation path that leaks work, a parser that accepts malformed input until one particular byte pattern explodes under load, or a library boundary that quietly translates a domain failure into process termination.

Happy-path unit tests do not put enough pressure on these designs. They tend to validate nominal behavior while leaving lifetime transitions, cleanup guarantees, and edge contracts mostly unexercised. In modern C++, that is a bad bargain. Ownership and failure handling are explicit enough that you can design tests around them, and you should. If a component owns scarce resources, crosses process or API boundaries, or behaves differently under timeout, cancellation, malformed input, or partial failure, its test strategy should be built around those facts.

This chapter is about test design, not tooling. The goal is to decide what evidence to ask for before code ships. Sanitizers, static analysis, and build diagnostics belong in the next chapter. Runtime logs, metrics, traces, and crash evidence belong in the chapter after that. Here the question is simpler: what tests prove that ownership, cleanup, and boundary behavior stay correct when the system is under stress?

## Start from failure shape, not test pyramid slogans

Generic testing advice becomes weak quickly in C++ because the expensive failures are not evenly distributed. If a service spends most of its risk budget in shutdown, cancellation, temporary-file replacement, buffer lifetime, and external protocol translation, then the test suite should spend most of its effort there as well.

That means starting with failure shape.

For each component, ask four questions.

1. Which resources must be released, rolled back, or committed exactly once?
2. Which boundaries translate errors, ownership, or representation between subsystems?
3. Which inputs or schedules are too large to enumerate but cheap to generate?
4. Which behaviors depend on time, concurrency, or cancellation rather than simple call order?

Those questions push you toward different test forms. Resource cleanup usually needs deterministic failure injection and postcondition checks. Boundary translation usually needs contract tests against realistic payloads and error classes. Large input surfaces usually need properties and fuzzing. Time-sensitive concurrency usually needs controllable clocks, executors, and shutdown orchestration instead of sleep-based tests.

Coverage numbers do not answer those questions. A line can run and still fail to prove that rollback happened, ownership remained valid, or shutdown drained background work without use-after-free risk. Treat coverage as a lagging completeness signal, not as the organizing principle of the suite.

## Test resource lifecycles at the level the business cares about

The right test for a resource bug almost never asserts that some helper was called. It asserts the observable contract around acquisition, commit, rollback, and release.

Consider a service that rewrites an on-disk snapshot atomically. The production rule is not "call `write`, then `rename`, then `remove` on failure." The production rule is "either the new snapshot becomes visible, or the old one remains intact and the staging file is cleaned up." A useful test targets that rule directly.

### Intentional partial: a seam that makes rollback testable

```cpp
struct file_system {
    virtual ~file_system() = default;

    virtual auto write(std::filesystem::path const& path,
                       std::span<char const> bytes)
        -> std::expected<void, std::error_code> = 0;

    virtual auto rename(std::filesystem::path const& from,
                        std::filesystem::path const& to)
        -> std::expected<void, std::error_code> = 0;

    virtual void remove(std::filesystem::path const& path) noexcept = 0;
};

enum class snapshot_error {
    staging_write_failed,
    commit_failed,
};

auto write_snapshot_atomically(file_system& fs,
                               std::filesystem::path const& target,
                               std::span<char const> bytes)
    -> std::expected<void, snapshot_error>
{
    auto staging = target;
    staging += ".tmp";

    if (auto r = fs.write(staging, bytes); !r) {
        return std::unexpected(snapshot_error::staging_write_failed);
    }

    if (auto r = fs.rename(staging, target); !r) {
        fs.remove(staging);
        return std::unexpected(snapshot_error::commit_failed);
    }

    return {};
}
```

```cpp
TEST(write_snapshot_atomically_cleans_up_staging_file_on_commit_failure)
{
    fake_file_system fs;
    fs.fail_rename_with(make_error_code(std::errc::device_or_resource_busy));

    auto result = write_snapshot_atomically(
        fs,
        "cache/index.bin",
        std::as_bytes(std::span{"new snapshot"sv.data(), "new snapshot"sv.size()}));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), snapshot_error::commit_failed);
    EXPECT_FALSE(fs.exists("cache/index.bin.tmp"));
    EXPECT_EQ(fs.read("cache/index.bin"), "old snapshot");
}
```

This is the right kind of seam because it sits at the business boundary. The test does not mock half the standard library. It creates one replaceable interface around the external effect and checks the postconditions the caller depends on.

That tradeoff matters. Over-mocking infrastructure produces brittle tests that ratify the implementation order of syscalls rather than the safety properties of the operation. Under-seaming the design leaves failure paths untestable except by broad integration tests. The middle ground is to isolate the resource boundary once, then write tests against commit and rollback behavior.

### When over-mocking hides real bugs

Consider the difference between a test that checks implementation details and one that checks the safety property. Teams often write tests like this:

```cpp
// BAD: This test passes, but proves nothing about cleanup.
TEST(write_snapshot_calls_remove_on_rename_failure)
{
    strict_mock_file_system fs;
    EXPECT_CALL(fs, write(_, _)).WillOnce(Return(std::expected<void, std::error_code>{}));
    EXPECT_CALL(fs, rename(_, _)).WillOnce(Return(
        std::unexpected(make_error_code(std::errc::device_or_resource_busy))));
    EXPECT_CALL(fs, remove("cache/index.bin.tmp")).Times(1);

    write_snapshot_atomically(fs, "cache/index.bin", as_bytes("data"sv));
}
```

This test verifies that `remove` is called. It does not verify that the staging file is actually gone or that the original file is untouched. If someone refactors the cleanup to use `std::filesystem::remove_all` or changes the staging path convention, this test breaks -- but a real bug where `remove` silently fails and leaves the staging file behind would pass. The earlier test against `fake_file_system` is stronger because it asserts observable postconditions, not call sequences.

### Resource-leak tests: verify cleanup, not just happy-path ownership

Scoped RAII is not enough if error paths skip construction or move ownership incorrectly. A surprisingly common pattern is a resource that leaks only on a specific failure path:

```cpp
class connection_pool {
public:
    auto acquire() -> std::expected<pooled_connection, pool_error>;
    void release(pooled_connection conn) noexcept;
};

// This function has a leak on the second acquire failure.
auto transfer(connection_pool& pool, transfer_request const& req)
    -> std::expected<receipt, transfer_error>
{
    auto src = pool.acquire();
    if (!src) return std::unexpected(transfer_error::no_connection);

    auto dst = pool.acquire();
    if (!dst) {
        // BUG: forgot to release src back to the pool.
        return std::unexpected(transfer_error::no_connection);
    }

    // ... perform transfer, release both on success ...
    pool.release(std::move(*src));
    pool.release(std::move(*dst));
    return receipt{};
}
```

A test that only exercises the success path never sees the leak. A test that only checks the return value on failure also misses it. The test that catches it asserts pool state:

```cpp
TEST(transfer_releases_source_connection_when_dest_acquire_fails)
{
    counting_connection_pool pool{.max_connections = 1};

    auto result = transfer(pool, make_request());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(pool.available(), 1);  // Source connection must be returned.
}
```

This is the pattern: if you own a scarce resource, your failure-path tests should assert that the resource is released, not just that an error was returned.

### Exception safety: the gap between "compiles" and "correct"

Even `noexcept`-free code paths deserve testing when exception safety matters. A container or cache that provides the strong exception guarantee should be tested for it:

```cpp
TEST(cache_insert_preserves_existing_entries_on_allocation_failure)
{
    lru_cache<std::string, std::string> cache(/*capacity=*/4);
    cache.insert("key1", "value1");
    cache.insert("key2", "value2");

    failing_allocator::arm_failure_after(1);  // Fail during insert internals.

    auto result = cache.insert("key3", "value3");
    EXPECT_FALSE(result.has_value());

    // Strong guarantee: pre-existing entries are intact.
    EXPECT_EQ(cache.get("key1"), "value1");
    EXPECT_EQ(cache.get("key2"), "value2");
    EXPECT_EQ(cache.size(), 2);
}
```

If the cache provides only the basic guarantee, the test should still verify that no resources leaked and the cache is in a valid (if modified) state. The worst outcome is no test at all -- the cache silently corrupts its internal structure on exception, and callers discover the problem under production allocation pressure.

The same pattern applies to sockets, transactions, lock-guarded registries, temporary directories, subprocess handles, and thread-owning services. Ask what the stable contract is on success, partial failure, retry, and shutdown. Test that.

## Boundary tests should prove translation, not just parsing

Modern C++ code often spends its complexity budget at boundaries: network protocols, file formats, process boundaries, plugin APIs, database clients, and C interfaces. Bugs here are expensive because they corrupt assumptions on both sides. A boundary test should verify three things.

First, valid inputs map to the internal representation without lifetime tricks. If a parser stores `std::string_view` into longer-lived state, boundary tests should prove that the view refers to stable ownership or that the representation copies when necessary. Second, invalid or partial inputs fail with the right error category. A parse failure, transport failure, and business-rule rejection should not collapse into one generic error path unless that is explicitly the API contract. Third, output formatting or translation back out of the component preserves invariants such as ordering, escaping, units, and versioning.

Use realistic artifacts here. For a configuration loader, keep real sample files beside the tests. For an HTTP or RPC edge, keep representative payloads, including malformed headers, oversized bodies, duplicate fields, bad encodings, and unsupported versions. For a library with a C API, write tests at the ABI-facing surface rather than only against the internal C++ wrapper. If the boundary promises not to throw, test that promise under allocator pressure and invalid inputs.

These tests do not have to be large. They do have to be concrete. "Round-trips one JSON object" is weak. "Rejects duplicate primary key fields with a schema error and leaves previous configuration active" is strong.

### Boundary edge cases that curated examples miss

Pay attention to boundary conditions that look harmless in isolation but interact badly:

```cpp
// A parser that stores string_view into a longer-lived config object.
// This test passes because the input string outlives the config.
TEST(config_parser_reads_server_name)
{
    std::string input = R"({"server": "prod-01"})";
    auto cfg = parse_config(input);
    EXPECT_EQ(cfg.server_name(), "prod-01");  // PASSES -- but fragile.
}

// This test exposes the dangling view.
TEST(config_survives_input_destruction)
{
    auto cfg = []{
        std::string input = R"({"server": "prod-01"})";
        return parse_config(input);
    }();
    // input is destroyed. If server_name() holds a string_view into it,
    // this is use-after-free. It may still "pass" without sanitizers.
    EXPECT_EQ(cfg.server_name(), "prod-01");
}
```

The first test is the one most teams write. The second is the one that catches the real bug. Pair it with AddressSanitizer (next chapter) to turn the silent corruption into a hard failure.

Other frequently missed boundary edge cases worth explicit tests:

- Empty input, single-byte input, and input exactly at buffer boundaries.
- Payloads where string fields contain embedded nulls, since `std::string_view::size()` and C `strlen()` disagree.
- Error responses from dependencies that arrive as valid protocol frames with unexpected status codes, not just connection failures.
- Inputs that are valid in one version of a schema but illegal in another, especially when version negotiation is involved.

The example project demonstrates several of these patterns concretely. In `examples/web-api/tests/test_http.cpp`, `test_parse_request_malformed()` feeds the string `"not a valid request"` into the parser and asserts that `parse_request()` returns `std::nullopt` rather than crashing or producing a half-initialized `Request`. This is a malformed-input boundary test that catches parsers which assume well-formed input. The test also exercises missing headers (`test_header_missing()`), confirming that the `std::optional`-returning `header()` method handles absence correctly rather than returning a dangling view or a default-constructed string.

In `examples/web-api/tests/test_task.cpp`, the boundary validation tests follow the same approach. `test_task_validation_rejects_empty_title()` and `test_task_validation_rejects_long_title()` (the latter constructing a 257-character string) verify that domain invariants hold at the extremes. These are not implementation-order tests -- they assert the business rule that a task title must be non-empty and within a length bound, and they check that the error is reported through `std::expected` with the correct `ErrorCode::bad_request`, not swallowed or converted to an exception.

## Failure injection is more valuable than more mocks

C++ error paths are where ownership mistakes become production incidents. If you only test success, you are effectively declaring the error-handling code unreviewed.

Deterministic failure injection is the practical answer. Introduce failures where the component crosses a resource boundary or scheduling boundary: file open, rename, memory allocation inside a bounded component, task submission, timer expiration, downstream RPC call, or durable commit. Then verify that the operation leaves the system in a valid state.

The important word is deterministic. Randomly failing syscalls can be useful in chaos environments, but they are weak as regression tests. A regression test should be able to say exactly which operation fails and what state must hold afterward.

Design the seams accordingly.

- File and network adapters should be replaceable at the operation boundary.
- Clock and timer sources should be injectable so timeout tests do not sleep.
- Task scheduling should allow a test executor that advances work deliberately.
- Shutdown and cancellation should expose a completion point that tests can await.

This design pressure is healthy. If a component cannot be forced through its failure modes without global monkey-patching, it is usually too entangled with the environment.

Avoid one common overreach: simulating allocator failure everywhere. Allocation-failure testing can be useful for hard real-time or infrastructure components with strong recovery guarantees, but in many codebases it produces noise and unrealistic control flow. Use it where the contract actually depends on low-memory survival. For most service code, I/O failure, timeout, cancellation, and partial-commit behavior are the higher-value targets.

## Property tests and fuzzers belong at input-rich boundaries

Some boundaries are too large for curated examples alone. Parsers, decoders, compressors, SQL-like query fragments, binary message readers, path normalizers, and command-line interpreters all accept vast input spaces. Here property-based tests and fuzzing pay for themselves.

The point is not novelty. The point is to encode invariants that should survive many inputs.

Examples of good properties:

- Parsing valid configuration preserves semantic equality after serialize-then-parse.
- Invalid UTF-8 never produces a successful normalized identifier.
- A message decoder either returns a fully formed value or a structured error; it never leaves partially initialized output observable.
- Path normalization is idempotent for already-normalized relative paths within the accepted domain.

Fuzzing is especially strong for native code because malformed inputs often drive control flow into rarely tested branches where lifetime mistakes and undefined behavior live. But keep the chapter boundary straight: fuzzing is still a testing strategy. Its value comes from generating pressure on contracts and invariants. The next chapter explains how sanitizers make fuzzing much more productive by turning silent memory corruption into actionable failures.

Use seed corpora that look like production traffic, not just arbitrary bytes. Otherwise the fuzzer spends too much time exploring input shapes your real system would reject at an outer layer. For protocol readers, include truncated messages, duplicate fields, bad lengths, unsupported versions, and compression edge cases. For text formats, include overlong tokens, invalid escapes, and mixed line endings.

## Concurrency and cancellation tests need controllable time

Many C++ teams know that sleep-based tests are flaky, then keep writing them because the production code hard-wires real clocks and thread pools. The result is a false economy: tests pass locally, fail in CI, and still miss the real shutdown bug.

If a component depends on deadlines, retries, stop requests, or background draining, design it so tests can control time and scheduling. `std::stop_token` and `std::jthread` help express cancellation intent, but they do not remove the need for deterministic orchestration. A task queue that runs on an injectable executor is easier to verify than one that immediately spawns detached work. A retry loop that takes a clock and sleep strategy is easier to test than one that directly calls `std::this_thread::sleep_for`.

Good concurrency tests typically assert one of these behaviors.

- A stop request prevents new work from starting.
- In-flight work observes cancellation at defined suspension points.
- Shutdown waits for owned work and does not use freed state afterward.
- Backpressure limits queue growth instead of converting overload into unbounded memory growth.
- Timeout paths return consistent error categories and release owned resources.

Notice that none of those are "called callback X before callback Y." They are lifecycle guarantees. That is where concurrency bugs become expensive.

The example project's `test_concurrent_access()` in `examples/web-api/tests/test_repository.cpp` shows this concretely. It spawns 8 `std::jthread`s, each creating 100 tasks concurrently, then asserts that the final repository size equals 800. This tests the invariant that the `shared_mutex`-protected `TaskRepository` does not lose or duplicate entries under concurrent writes. The same file also demonstrates update-validation testing: `test_update_validates()` mutates a task's title to an empty string inside the `update()` callback and asserts that the repository rejects the mutation with `ErrorCode::bad_request`. This is a boundary-meets-concurrency test -- it verifies that the re-validation step inside the write-locked `update()` path catches invariant violations even when the updater callable is provided by the caller.

The project's CMake configuration (`examples/web-api/CMakeLists.txt`) also supports running these tests under sanitizers via `ENABLE_ASAN` and `ENABLE_TSAN` options. Running the concurrent test under ThreadSanitizer provides mechanical evidence that the locking protocol is correct, rather than relying on the test passing by luck on a particular scheduler interleaving.

## Integration tests should validate whole cleanup stories

Not every resource bug can be proven with isolated tests. Some failures emerge only when the real file system, process model, sockets, or thread scheduling are involved. You still want focused unit and property tests, but you also need a smaller set of integration tests that validate end-to-end cleanup behavior.

For a service, that may mean starting the process with a temporary data directory, sending realistic requests, forcing a failure at the storage layer, then verifying restart behavior and on-disk state. For a library, it may mean exercising the public API from a tiny host program that loads configuration, starts background work, cancels it, and unloads cleanly. For tooling, it may mean invoking the real executable against fixture trees and checking exit codes, stderr, and file-system postconditions.

Keep these tests scenario-based and scarce. They are slower and harder to diagnose than unit tests. Their job is to validate a full cleanup story: partial writes do not become committed state, repeated starts do not inherit garbage from failed shutdown, and external contracts remain stable under failure.

## What to stop testing

Weak tests consume review time without improving confidence.

Stop writing tests that merely restate the current implementation structure.

- Tests that verify every helper invocation but never assert an externally meaningful postcondition.
- Mock-heavy tests that would fail if you merged two internal functions even though the contract stayed correct.
- Sleep-based async tests whose real assertion is "the machine was idle enough today."
- Snapshot tests for logs or error strings when the contract is the error category and structured fields, not the prose.
- Broad integration suites used as a substitute for precise failure-path tests.

The discipline is to spend test budget where the bug classes live. In C++, those bug classes cluster around ownership, boundaries, cancellation, and malformed input. Design for those explicitly.

## Takeaways

Testing strategy in modern C++ should follow failure economics, not generic layering slogans. Resource-owning code needs deterministic failure-path tests. Boundary-heavy code needs contract tests with realistic artifacts. Input-rich code needs properties and fuzzing. Concurrent code needs controllable time and scheduling. Integration tests should validate whole cleanup stories, not replace focused tests.

Use this chapter to decide what behavior must be proven before shipping. Use the next chapter to decide which compilers, sanitizers, analyzers, and build diagnostics should mechanically search for bugs while those tests run.

Review questions:

- What are the resource commit, rollback, and release guarantees of this component?
- Which boundary translations need concrete contract tests with realistic fixtures?
- Which failure points can be injected deterministically today, and which require redesign to become testable?
- Which input surfaces deserve property tests or fuzzing rather than example-only coverage?
- Which time, cancellation, or shutdown behaviors are currently tested by sleeping instead of by controlled scheduling?