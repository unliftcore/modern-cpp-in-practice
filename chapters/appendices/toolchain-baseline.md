# Appendix B: Toolchain Baseline

This appendix defines the reference build baseline behind the examples and recommendations in the manuscript. It is not a promise that every example behaves identically on every platform. It is the minimum environment the book assumes when it talks about C++23 support, warnings, sanitizers, and diagnosable release artifacts.

## Reference Compilers

| Toolchain | Baseline | Notes |
|---|---|---|
| GCC | 14+ | Strong general C++23 coverage; good Linux sanitizer environment |
| Clang | 18+ | Reference environment for sanitizer-heavy investigation and many diagnostics |
| MSVC | 17.10+ in Visual Studio 2022 | Required Windows baseline; expect some sanitizer and library support differences |

The baseline is intentionally conservative. If a sample depends on behavior or library support that is weaker on one of these toolchains, the surrounding chapter should say so explicitly.

## Default Build Expectations

All substantial examples should assume:

- `-std=c++23` or the equivalent vendor flag.
- Debug information in all developer and diagnostic builds.
- Assertions enabled in at least one fast local build configuration.
- Warning sets strong enough to catch narrowing, shadowing, ignored results, missing overrides, and suspicious conversions before review.
- Release artifacts that retain enough symbol and build identity information to support crash analysis later.

The point is not one universal flag block. The point is a stable diagnostic posture across supported compilers.

## Required Build Modes

| Build mode | Question it answers | Typical characteristics |
|---|---|---|
| Fast developer build | Can I iterate on logic quickly? | Debug info, assertions, low optimization or none |
| ASan + UBSan build | Did execution hit memory or undefined-behavior bugs? | Debug info, frame pointers, moderate optimization |
| TSan build | Did concurrent execution hit a data race or lock-order bug? | Separate job, heavier overhead, focused workload |
| Static-analysis build | Does the code trip known defect patterns before runtime? | Serious warnings plus analyzer or lint passes |
| Release-with-symbols build | Will the shipped binary still be diagnosable? | Production optimization, external symbols or symbol server, build IDs |

Trying to collapse these into one magic build usually weakens signal. Sanitizers distort timing. ThreadSanitizer is too heavy for every edit-build-run cycle. Release verification needs the same optimization and packaging shape the shipped artifact will use.

## Warning Policy

Treat warnings as a repository policy surface, not as a developer preference.

- Enable a serious warning baseline on every supported compiler.
- Treat warnings as errors for owned code once the warning baseline is under control.
- Keep suppressions local and justified.
- Isolate noisy third-party or generated code rather than muting diagnostics across the whole project.

The manuscript does not prescribe an identical flag list for every environment because vendors differ. It does prescribe the review posture: a warning should either identify a real risk or be turned off on purpose, not be ignored as background noise.

## Sanitizer Baseline

Use sanitizers as named verification lanes, not as occasional rescue tools.

- AddressSanitizer is the default memory-safety lane.
- UndefinedBehaviorSanitizer runs with ASan where supported and where the signal is useful.
- ThreadSanitizer runs separately on code that genuinely exercises shared-state and shutdown paths.
- Fuzzing, stress tests, parser tests, and cancellation tests should preferentially run under sanitizer builds when those paths are high value.

Clang on Linux is the reference environment for the strongest sanitizer coverage. That is not a reason to ignore Windows or MSVC; it is a reason to avoid pretending all sanitizer behavior is equally mature everywhere.

## Release Diagnostics Baseline

Release artifacts should preserve enough information to answer production questions later.

- Produce symbol files or an equivalent symbolization path.
- Attach a build ID or other exact binary identity.
- Record source revision or package version in build metadata.
- Keep stack unwinding usable in the shipped configuration.
- Preserve the commands or presets needed to reproduce the release build shape.

If a crash in production cannot be mapped back to an exact binary and symbols, the toolchain policy is not complete.

## Version Policy Notes

- The manuscript targets C++23 as the default working language.
- C++26 material belongs only where it changes a present-day design decision.
- If a feature is technically standardized but not yet dependable across the baseline toolchains, treat it as provisional and say so near the example.

The book is not trying to win a standards-timeline argument. It is trying to describe what an experienced engineer can rely on in a real codebase.