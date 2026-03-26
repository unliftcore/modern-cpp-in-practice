# Sanitizers, Static Analysis, and Build Diagnostics

Testing strategy tells you what behavior to exercise. This chapter is about the mechanical bug-finding stack that should run alongside that exercise: compiler warnings, sanitizers, static analysis, and build configurations that preserve useful diagnostics. These tools do not tell you whether the design is correct. They tell you whether the program stepped into a bug class that humans routinely miss in review.

That distinction matters. A test can assert that cancellation leaves no visible partial state. AddressSanitizer can tell you that the cleanup path touched freed memory while attempting to honor that contract. A contract test can prove that a parser rejects malformed input. UndefinedBehaviorSanitizer can tell you that one rejection path signed-overflowed while computing a buffer size. Observability can later tell you that a production build is crashing in a path you never exercised under sanitizer. Each layer answers a different question.

For native systems, the cost of skipping this layer is predictable. The bug is found later, reproduced less reliably, and diagnosed with worse evidence. If the build only produces optimized binaries with stripped symbols, weak warnings, and no analyzer or sanitizer jobs, the team has chosen slower debugging as policy.

## Treat Diagnostics as Build Products, Not Developer Preferences

The first mistake is organizational, not technical. Teams often treat warnings and analysis as optional local tooling, which means they drift by compiler, machine, and mood. Production C++ needs the opposite posture. Diagnostic fidelity should be part of the build contract.

At minimum, your repository should define named build modes that answer distinct questions.

| Build mode | Primary question | Typical characteristics |
|---|---|---|
| Fast developer build | Can I iterate quickly on logic? | Debug info, assertions, no or low optimization |
| Address/UB sanitizer build | Did execution hit memory or undefined-behavior bugs? | `-O1`, debug info, frame pointers, ASan and UBSan |
| Thread sanitizer build | Did concurrent execution hit a data race or lock-order problem? | Dedicated job, reduced parallelism, TSan only |
| Static analysis build | Does the code trigger warning patterns or analyzable defects before execution? | Compiler warnings, clang-tidy, analyzer jobs |
| Release-with-symbols build | Will production behavior remain diagnosable? | Release optimization, external symbols, build IDs, stable source mapping |

Trying to collapse those into one universal configuration usually fails. TSan carries too much overhead for every build. ASan and UBSan alter memory layout and timing. Deep analysis jobs are slower than normal edit-compile-run loops. The right answer is not one magical build. The right answer is a deliberate matrix.

That matrix should live in versioned build scripts or presets, not in tribal knowledge. If the repository cannot tell a new engineer exactly how to produce a sanitized binary or a release artifact with symbols, the workflow is not mature enough.

The example project in `examples/web-api/` demonstrates this with named CMake options that map directly to the build lanes above:

```cmake
# examples/web-api/CMakeLists.txt
option(ENABLE_ASAN  "Enable AddressSanitizer + UBSan"  OFF)
option(ENABLE_TSAN  "Enable ThreadSanitizer"            OFF)

add_library(project_sanitizers INTERFACE)
if(ENABLE_ASAN)
    target_compile_options(project_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(project_sanitizers    INTERFACE
        -fsanitize=address,undefined)
endif()
if(ENABLE_TSAN)
    target_compile_options(project_sanitizers INTERFACE -fsanitize=thread)
    target_link_options(project_sanitizers    INTERFACE -fsanitize=thread)
endif()
```

A new engineer clones the repository and runs `cmake -G Ninja -DENABLE_ASAN=ON` or `-DENABLE_TSAN=ON`. The lanes are discoverable, version-controlled, and produce distinct binaries. That is what "named build modes" looks like in practice.

## Warnings Are a Policy Surface

Compiler warnings are the cheapest analysis you have, and teams still waste them. One common failure mode is warning inflation: thousands of preexisting warnings train everyone to ignore the channel. The other is warning minimalism: fear of noise causes the team to enable so little that suspicious code passes silently.

The practical target is narrower and stricter.

- Enable a serious warning set on all supported compilers.
- Treat warnings as errors for owned code once the warning baseline is under control.
- Keep suppressions local, versioned, and explained.
- Review new suppressions like code changes, because that is what they are.

This is not about aesthetic cleanliness. Warnings often expose real review problems: narrowing conversions, missing overrides, ignored return values, shadowing that hides ownership state, switch exhaustiveness gaps, or accidental copies in hot paths. Some warnings are stylistic and should stay off. That is fine. The point is to make the enabled set defensible and stable.

Be especially careful with blanket suppression at target level. When a third-party header or generated source is noisy, isolate it rather than muting the same diagnostic across the repository. Teams often create future blind spots by solving one vendor problem with project-wide suppression.

## Sanitizers Turn Silent Corruption Into Actionable Failures

Sanitizers are valuable because they change failure mode. Instead of a memory bug manifesting as a distant crash or impossible state, it stops near the violation with a stack trace and an explanation of the bug class.

For most production C++ codebases, three sanitizer configurations carry the highest value.

### AddressSanitizer and Leak Detection

AddressSanitizer is the standard first line because it finds a wide set of bugs that otherwise waste enormous time: use-after-free, heap buffer overflow, stack use-after-return in some configurations, double free, and related memory-lifetime violations. Leak detection, where available, adds another useful signal for test processes and short-lived tools.

ASan is especially effective when paired with the testing strategies from the previous chapter. Failure-path tests, fuzzers, and integration scenarios drive execution into branches where ownership mistakes live. ASan then converts those mistakes into reproducible failures.

### The bug that "works" without ASan

This is the canonical case that wastes days of debugging time in codebases that skip sanitizer builds:

```cpp
auto get_session_name(session_registry& registry, session_id id)
    -> std::string_view
{
    auto it = registry.find(id);
    if (it == registry.end()) return {};
    return it->second.name();  // Returns view into the session object.
}

void log_and_remove_session(session_registry& registry, session_id id)
{
    auto name = get_session_name(registry, id);
    registry.erase(id);             // Session destroyed. name is now dangling.
    audit_log("removed session: {}", name);  // Use-after-free.
}
```

Without ASan, this code will usually pass tests and even run correctly in production for months. The freed memory still contains the old string data until something else overwrites it. The test passes. Code review might not catch it -- the function looks straightforward. When it does fail, the symptom is garbled log output or a crash in an unrelated allocation, nowhere near the actual bug.

Under ASan, this produces an immediate, precise failure:

```
==41032==ERROR: AddressSanitizer: heap-use-after-free on address 0x6020000000d0
READ of size 12 at 0x6020000000d0 thread T0
    #0 0x55a3c1 in log_and_remove_session(session_registry&, session_id)
        src/session_manager.cpp:47
    #1 0x55a812 in handle_disconnect src/connection.cpp:103

0x6020000000d0 is located 0 bytes inside of 32-byte region
freed by thread T0 here:
    #0 0x4c1a30 in operator delete(void*)
    #1 0x55a7f1 in session_registry::erase(session_id)
        src/session_manager.cpp:31

previously allocated by thread T0 here:
    #0 0x4c1820 in operator new(unsigned long)
    #1 0x55a620 in session_registry::insert(session_id, session_info)
        src/session_manager.cpp:22
```

The report identifies the exact read, the exact free, and the exact allocation. Compare that with the alternative: a corrupted log entry three weeks from now that nobody connects to this code path.

Typical build characteristics look like this:

```bash
clang++ -std=c++23 -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined
```

The exact flags vary by toolchain, but the principles are stable: keep enough optimization to preserve realistic structure, keep debug info, and keep frame pointers so stacks are usable.

### UndefinedBehaviorSanitizer

UBSan is the companion that catches dangerous behavior not always visible as memory corruption: misaligned access, invalid shifts, bad enum values, null dereference in some contexts, signed overflow depending on configuration, and other undefined or suspicious operations. The important operational lesson is that undefined behavior is often input-sensitive and build-sensitive. The same code may pass tests for months, then fail only on a new compiler or after an inlining change. UBSan helps surface those hazards while the bug is still local enough to fix sanely.

Do not over-interpret it, though. UBSan is not a proof system. It only reports behavior that the exercised execution encountered and that the enabled checks can see.

A concrete example: signed overflow in size calculations is a common source of security bugs that compilers are free to exploit.

```cpp
auto compute_buffer_size(std::int32_t width, std::int32_t height, std::int32_t channels)
    -> std::int32_t
{
    return width * height * channels;  // Signed overflow if product exceeds INT32_MAX.
}
```

For `width=4096, height=4096, channels=4`, the product is 67,108,864 -- safe. For `width=32768, height=32768, channels=4`, the product is 4,294,967,296 which overflows a 32-bit signed integer. Without UBSan, the compiler may optimize downstream bounds checks away entirely because signed overflow is undefined. UBSan catches this at the multiplication:

```
runtime error: signed integer overflow: 32768 * 32768 cannot be
represented in type 'int'
```

The fix is to use unsigned arithmetic or to check for overflow before the multiplication. The point is that this class of bug is silent, optimizer-sensitive, and often security-relevant -- exactly what UBSan is for.

### ThreadSanitizer

TSan is expensive and often noisy around custom synchronization, lock-free code, and some coroutine or foreign-runtime integrations. It is still worth running because data races remain among the most expensive native bugs to diagnose after the fact.

### The data race that tests never catch

Data races are invisible to testing without TSan because they depend on scheduling. Consider a metrics counter shared between a request handler and a background reporter:

```cpp
struct service_stats {
    std::int64_t requests_handled = 0;   // No synchronization.
    std::int64_t bytes_processed = 0;
};

// Thread 1: request handler
void handle_request(service_stats& stats, request const& req) {
    process(req);
    stats.requests_handled++;    // Data race: unsynchronized write.
    stats.bytes_processed += req.size();
}

// Thread 2: periodic reporter
void report_stats(service_stats const& stats) {
    log_metrics("requests", stats.requests_handled);   // Data race: unsynchronized read.
    log_metrics("bytes", stats.bytes_processed);
}
```

This code will pass every test you write. It will run correctly for months on x86 where the memory model is relatively forgiving. It becomes a problem when the compiler reorders the writes, when the optimizer lifts the read into a register, or when someone ports to ARM. The bug is real today but the symptoms are deferred.

TSan catches it immediately:

```
WARNING: ThreadSanitizer: data race (pid=28511)
  Write of size 8 at 0x7f8e3c000120 by thread T1:
    #0 handle_request(service_stats&, request const&)
        src/handler.cpp:24
    #1 worker_loop src/server.cpp:88

  Previous read of size 8 at 0x7f8e3c000120 by thread T2:
    #0 report_stats(service_stats const&)
        src/reporter.cpp:12
    #1 reporter_loop src/server.cpp:102

  Location is global 'g_stats' of size 16 at 0x7f8e3c000120

  Thread T1 (tid=28513, running) created by main thread at:
    #0 pthread_create
    #1 start_workers src/server.cpp:71

  Thread T2 (tid=28514, running) created by main thread at:
    #0 pthread_create
    #1 start_reporter src/server.cpp:76
```

The fix is to use `std::atomic<std::int64_t>` with appropriate memory ordering, or to protect the struct with a mutex if the fields must be read consistently together. The important point is that no amount of conventional testing would have found this -- the test needs TSan to convert a scheduling-dependent corruption into a deterministic failure.

The operational pattern is usually different from ASan. Run TSan in a narrower CI lane or nightly job. Feed it tests that deliberately stress shared-state paths, shutdown, retries, and cancellation. Keep the suppression file short and justified. If TSan reports a race in supposedly benign statistics code, do not dismiss it reflexively. Benign races have a habit of becoming real ones after the next feature.

Avoid stacking TSan with other heavy sanitizers in the same build. Separate jobs make failures easier to interpret and keep the timing distortion manageable.

The example project's `TaskRepository` (in `examples/web-api/src/modules/repository.cppm`) is a concrete case where TSan validates a correct synchronization pattern. The repository protects its internal `std::vector<Task>` with a `std::shared_mutex`, using `std::shared_lock` for read paths (`find_by_id`, `find_all`) and `std::scoped_lock` for write paths (`create`, `update`, `remove`). Building with `-DENABLE_TSAN=ON` and exercising concurrent readers and writers confirms that this locking discipline has no data races -- exactly the kind of evidence that conventional tests alone cannot provide.

## Static Analysis Scales Review Attention

Static analysis is most useful when it is selective and boring. If the analyzer produces pages of stylistic noise, the team will stop reading it. If it is tuned toward patterns that actually matter in your codebase, it becomes a force multiplier for review.

Useful targets in modern C++ typically include:

- Dangling views or references caused by temporary lifetime mistakes.
- Missing or misapplied `override`, `noexcept`, or `[[nodiscard]]` where the API contract depends on them.
- Suspicious ownership transfer patterns involving raw pointers, moved-from objects, or smart-pointer aliasing.
- Error-handling mistakes such as ignored results, swallowed status values, or inconsistent translation at boundaries.
- Expensive accidental copies across hot or high-volume interfaces.
- Concurrency hazards such as locking inconsistencies or unsafe capture of shared state.

Compiler-integrated analysis, `clang-tidy`, the Clang static analyzer, and platform-specific tools such as MSVC `/analyze` each catch somewhat different things. Use more than one if your toolchain supports it, but keep the output curated. A small enforced rule set that consistently catches real problems is better than a sprawling configuration everybody bypasses.

This is also where repository-specific knowledge belongs. If your service code should never ignore `std::expected` results from transport adapters, add checks and wrappers that make that hard to do silently. If your library forbids exceptions at the ABI boundary, analyze for that policy directly or enforce it via build and API structure. Static analysis becomes substantially better when it knows what your contracts are.

## Preserve Diagnostic Quality in Release Artifacts

One of the most damaging habits in native development is treating debuggability as a debug-build-only concern. Production failures happen in release builds. If those artifacts do not preserve enough information to map crashes and latency problems back to code, you have made later observability dramatically weaker.

Release artifacts should normally preserve at least these properties.

- External symbol files or symbol servers so stacks can be symbolized after deployment.
- Build IDs or equivalent version fingerprints that unambiguously map a dump or trace to an exact binary.
- Source revision metadata embedded in the artifact or attached in deployment metadata.
- Enough unwind support for usable native stack traces.
- Stable compiler and linker settings recorded somewhere repeatable.

Depending on platform and sensitivity, that may also include frame pointers, split DWARF or PDB handling, map files, and archived link commands. The exact mechanics are toolchain-specific. The policy is not: if you cannot reproduce the diagnostic shape of the shipped binary, incident response slows down immediately.

This is why build diagnostics belong in the chapter with sanitizers and analysis rather than in the observability chapter. Observability consumes these artifacts later, but the decision to produce them is a build decision.

## CI Should Stage Cost, Not Pretend Cost Does Not Exist

A mature pipeline does not run every expensive check on every edit. It stages them by cost and by bug class.

For example:

- Pull request gate: fast build, serious warnings, targeted tests, and at least one ASan/UBSan configuration on changed targets.
- Scheduled or nightly jobs: broader sanitizer coverage, TSan, deeper static analysis, and fuzz targets with sanitizer enabled.
- Release qualification: clean release-with-symbols build, packaging checks, and verification that symbol publication and build metadata succeeded.

The tradeoff is obvious: slower checks find bugs later in the day. The answer is not to drop them. The answer is to place them where they are sustainable and visible.

Do not let sanitizer or analyzer failures become advisory-only noise. If a lane is too flaky to gate anything, fix the flakiness or narrow its scope. A permanently red analysis job is organizationally equivalent to not having the job.

## What These Tools Will Not Do

This tooling stack is powerful, but its limits should stay explicit.

- Sanitizers do not prove correctness; they only instrument exercised executions.
- Static analysis does not understand every project-specific invariant unless you encode those invariants into the code and configuration.
- Warning cleanliness does not imply good API design or good failure handling.
- A perfectly diagnosable build can still ship the wrong behavior.

That is why Part VI has three chapters instead of one. Testing strategy defines what must be exercised. Mechanical tooling catches classes of bugs while that exercise happens. Observability explains how to understand failures that still reach production.

## Takeaways

In production C++, diagnostics must be designed, not hoped for. Keep a versioned build matrix with distinct jobs for fast iteration, sanitizers, analysis, and release-with-symbols. Treat warnings as a policy surface. Run ASan and UBSan routinely, TSan deliberately, and static analysis selectively enough that people still read the output. Preserve symbolization and build identity in release artifacts.

The central tradeoff is cost versus signal. Sanitizers and analysis slow the pipeline and occasionally require suppressions. Shipping without them costs far more when a native bug escapes. Choose the cost while the code is still local.

Review questions:

- Which sanitizer configurations are mandatory for this target, and are they actually exercised by meaningful tests?
- Which warnings are enforced repository-wide, and where are suppressions reviewed?
- Which analyzer checks reflect real project contracts rather than generic style preferences?
- Can a release crash or dump be mapped back to an exact binary, symbol set, and source revision?
- Which expensive checks run later by design, and is that staging explicit rather than accidental?