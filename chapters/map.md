# Book Map

The book is organized around the decisions that age badly when they are made casually. It starts with ownership, failure, and type design because those choices determine what later architecture can safely assume. From there it moves into interfaces and system boundaries, then into concurrency, performance, tooling, and long-term maintenance. The sequence is deliberate: architecture is hard to reason about without a stable model of lifetime and failure, concurrency is hard to tame without clear boundaries, and optimization is mostly wasted effort when correctness is still vague.

Undefined behavior is a cross-cutting concern rather than a standalone topic. Chapters on ownership address lifetime UB, concurrency chapters address data-race UB, and tooling chapters cover the sanitizers and static analysis that detect UB mechanically. Where a chapter touches a UB risk, it calls out the specific class and the verification strategy. C++26 notes appear only where they would change a recommendation that otherwise holds for C++23 code.

## Part I: Foundations Under Production Pressure

### Chapter 1: Ownership, Lifetime, and Resource Management

This chapter starts with the problem that breaks more C++ systems than almost any other: nobody can state, with confidence, who owns a resource, how long it lives, and what guarantees protect its cleanup path. RAII, move semantics, handles, and smart pointers are treated as boundary tools, not trivia. The failures are familiar and expensive: leaks, dangling references, double cleanup, shutdown bugs, and APIs whose contracts are impossible to audit in review. The goal is to make ownership legible enough that both code and architecture become easier to defend.

### Chapter 2: Errors, Results, and Failure Boundaries

*Prerequisites: Chapter 1 (ownership determines which cleanup paths run when failure occurs).*

Failure handling in C++ gets expensive when teams mix exceptions, status codes, logging side effects, and partial recovery without a clear boundary policy. This chapter looks at where exceptions are appropriate, where `std::expected` or explicit result objects improve control, and how to preserve useful failure information across layers without polluting every call site. The point is not elegance. It is keeping error policy consistent across libraries, services, and diagnostics so failures can be reasoned about before an incident and debugged during one.

### Chapter 3: Value Semantics, Identity, and Invariants

*Prerequisites: Chapter 1 (ownership and lifetime underpin copy, move, and aliasing decisions).*

Many C++ design mistakes come from treating all objects as interchangeable bags of fields instead of entities with distinct semantic roles. This chapter asks what should behave like a value, what carries identity, what can be copied, and what must preserve stronger invariants across mutation. In production, those choices affect thread safety, API clarity, caching behavior, serialization, and the ability to test code without accidental aliasing. Stronger semantic modeling shrinks bug surface and makes reviews less ambiguous.

### Chapter 4: Generic Code, Concepts, and Compile-Time Cost

*Prerequisites: Chapters 1–3 (generic code must respect the ownership, failure, and value-semantic contracts of the types it operates on).*

Templates are one of C++'s biggest advantages and one of its easiest ways to wreck build times, diagnostics, and maintainability. This chapter is about deciding when generic code actually removes duplication or enables a better abstraction, and when it just hides complexity in unreadable instantiation stacks. Concepts, constrained customization, and policy-based techniques are evaluated against their real cost: longer builds, brittle diagnostics, accidental coupling, and APIs that only specialists can extend safely.

## Part II: Design, Interfaces, and Boundaries

### Chapter 5: Interfaces, APIs, and Dependency Direction

*Prerequisites: Chapters 1–2 (ownership and failure policy must be stable before interface shape can be evaluated).*

Once ownership and failure policy are stable, the next pressure is keeping interfaces narrow enough that the system can evolve. This chapter covers public API shape, input and output types, customization seams, and dependency direction between components. The core problem is boundary design: too little structure leads to ad hoc coupling, while too much abstraction freezes bad decisions into permanent surface area. The focus is on exposing capability without exporting unnecessary policy, representation, or lifetime hazards.

### Chapter 6: Libraries, Modules, ABI, and Versioning

*Prerequisites: Chapter 5 (API shape determines what crosses a binary boundary).*

C++ systems often fail at scale not because the code is locally wrong, but because binary boundaries and version promises were designed casually. This chapter deals with library packaging, modules as an organizational tool, ABI-sensitive interfaces, plugin-style integration, and the cost of long-term compatibility. The pressure comes from mixed toolchains, incremental deployment, and code that must survive separate compilation units and release cadences. Readers should leave with a clearer sense of when a source-level abstraction is enough and when a stable binary contract changes the design entirely.

## Part III: Concurrency and Time

The chapters in this part assume that ownership, failure, and interface boundaries from Parts I and II are already in place. Concurrency amplifies every weakness in those foundations. This part also covers the infrastructure decisions that shape concurrent systems — thread pool design, executor models, and the emerging `std::execution` framework — woven into the chapters where they arise rather than isolated in a separate overview.

### Chapter 7: Shared State, Synchronization, and Contention

*Prerequisites: Chapters 1 and 3 (ownership and value semantics determine what can be safely shared).*

Concurrency failures are rarely caused by a lack of primitives. They come from poorly chosen sharing boundaries and vague invariants about who may touch what and when. This chapter examines threads, mutexes, atomics, and synchronization patterns under contention, partial ordering, deadlock risk, and observability pressure. The real decision is not whether to use concurrency, but how to bound it so correctness remains reviewable and performance does not collapse under skewed workload.

### Chapter 8: Tasks, Coroutines, and Cancellation

*Prerequisites: Chapters 1–2 and 7 (task lifetime depends on ownership; failure propagation and synchronization must already be understood).*

Modern C++ offers more ways to express asynchronous work, but those mechanisms only help if teams can define lifetimes, cancellation semantics, and ownership across suspended execution. This chapter addresses task models, coroutine-based APIs, executor selection and thread pool design, and cooperative cancellation. The hard part is maintaining clarity when control flow is split across time: request timeouts, shutdown coordination, resource cleanup after suspension, and debugging stacks that no longer resemble synchronous call chains. Async structure is treated as a contract problem, not just a syntax problem. Where `std::execution` (P2300) changes the executor model or simplifies composition, it is covered as a C++26 note.

### Chapter 9: Pipelines, Backpressure, and Service Throughput

*Prerequisites: Chapters 7–8 (pipeline stages depend on synchronization and task infrastructure).*

Systems that process streams of work fail when local concurrency choices ignore queue growth, head-of-line blocking, burst behavior, and feedback paths. This chapter focuses on composing stages, bounding queues, handling overload, and making throughput and latency tradeoffs explicit. The failure mode shows up under load, when a design that looks clean in unit tests becomes unstable in the presence of retries, fan-out, and uneven work distribution. Readers should come away with a vocabulary for designing flow control instead of discovering it accidentally in postmortems.

## Part IV: Data, Performance, and Measurement

### Chapter 10: Data Structures, Layout, and Memory Behavior

*Prerequisites: Chapter 1 (ownership-aware layout) and Chapter 3 (value semantics determine copy and move cost).*

High-level design decisions often hide low-level costs until the system reaches real scale. This chapter covers container choice, representation tradeoffs, ownership-aware data layout, and how memory behavior interacts with cache locality, traversal patterns, and update frequency. The focus is on choosing structures that fit workload shape — the "what and why" of data representation. When a container choice is also an API contract (exposing iterators, constraining element lifetime, or promising contiguous storage), that boundary implication is part of the analysis. Chapter 11 picks up where this one ends: measuring whether the choices actually deliver.

### Chapter 11: Cost Models, Allocation, and Locality

*Prerequisites: Chapter 10 (the data layout choices whose costs this chapter quantifies).*

Performance work in C++ is often derailed by vague claims such as "zero-cost" or "fast enough" that never become a concrete model of where time and memory go. This chapter builds that model around allocation patterns, indirection, copying, locality, branch behavior, and representation choices. Where Chapter 10 asks which data structure fits the workload, this chapter asks how to verify the assumption: what does the allocator actually do, where does the cache miss, and which abstraction is paying for itself versus pushing cost into hotter paths. The aim is disciplined reasoning before optimization and clearer tradeoffs when optimization becomes necessary.

### Chapter 12: Benchmarking, Profiling, and Regression Control

*Prerequisites: Chapter 11 (cost models provide the hypotheses that measurement validates).*

Optimizing without measurement is expensive superstition, but measuring poorly is only slightly better. This chapter covers benchmark design, profiler interpretation, workload selection, noise control, and performance regression detection in CI and release workflows. Teams often optimize a micro-case, misread a flame graph, or miss the fact that a harmless-looking change altered allocation rate or tail latency. Here, performance evidence is treated as an engineering artifact that must be reviewable and reproducible.

## Part V: Tooling and Verification

### Chapter 13: Builds, Diagnostics, and Static Analysis

*Prerequisites: none beyond Part I foundations.*

Large C++ systems become operationally expensive when build settings, warning policies, and analysis tooling are treated as afterthoughts. This chapter addresses build graph discipline, warning levels, compiler diagnostics, link-time configuration, and the practical use of static analysis. The challenge is maintaining signal in a codebase where local shortcuts can quietly erode portability, diagnosability, and reviewer confidence. Tooling is not auxiliary here. It is part of how teams keep language complexity under control.

### Chapter 14: Testing, Fuzzing, Sanitizers, and Observability

*Prerequisites: Chapter 13 (build and diagnostic infrastructure must be in place before sanitizers and fuzzers can run effectively).*

Modern C++ can fail in ways that conventional example-driven tests never see, especially when lifetime, concurrency, undefined behavior, and boundary assumptions interact. This chapter covers test structure, fuzzing targets, sanitizer strategy, and the operational signals needed to debug failures in production. The decision is how much verification is enough for a given component, and which bug classes must be caught before deployment because they are too expensive or too nondeterministic to debug afterward.

## Part VI: Evolution and Long-Term Maintenance

### Chapter 15: Legacy Migration, Interop, and Incremental Modernization

*Prerequisites: Parts I–II (the ownership, failure, and interface models that modernized code should target).*

Most teams do not begin with a clean C++23 codebase. They inherit old ownership models, macro-heavy interfaces, C compatibility constraints, partial platform support, and years of operational workarounds. This chapter focuses on modernization under constraint: how to introduce better boundaries, safer types, and stronger tooling without destabilizing shipping systems. The challenge is sequencing change so local improvements accumulate instead of producing a rewrite-shaped crater in the roadmap.

### Chapter 16: Large Codebases, Review Heuristics, and Maintainability

*Prerequisites: all earlier chapters (this chapter synthesizes the book's technical material into review and maintenance practices).*

The final chapter addresses the problems that become obvious only after a codebase is large: inconsistent local style, unclear review standards, abstraction drift, dependency sprawl, and knowledge that exists only in a few maintainers' heads. It turns the earlier technical material into review heuristics and maintenance policies that teams can apply over time. The goal is to make modern C++ sustainable, not merely expressive, by tying language choices back to code review, ownership boundaries, release practices, and the cost of future change.

## Appendices

### Appendix A: C++23/26 Feature Quick Reference

A compact table mapping language and library features to the chapters where they are discussed, with notes on compiler support status across GCC, Clang, and MSVC.

### Appendix B: Review Checklist Summary

A consolidated checklist drawn from the end-of-chapter review sections, organized by concern (ownership, failure, interfaces, concurrency, performance) for use in code review without flipping through the full text.

### Appendix C: Glossary

Canonical definitions for terms used across the book: ownership, lifetime, value semantics, identity, failure domain, failure boundary, invariant, contention, cost model, and others. Chapters reference this glossary rather than re-defining terms locally.

### Appendix D: Recommended Toolchain Configuration

Reference compiler flags, sanitizer configurations, static analysis profiles, and CI integration patterns for the toolchains covered in the book (GCC 14+, Clang 18+, MSVC 17.10+).
