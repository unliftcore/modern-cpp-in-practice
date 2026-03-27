# Preface

This is a production guide to modern C++23, not a language course and not a modernization diary. It is written for programmers who already know how software gets built, tested, shipped, and debugged, but who need a reliable way to make good C++ decisions under real design pressure. The subject is not syntax coverage. It is ownership, lifetime, interface shape, failure boundaries, concurrency, data layout, performance evidence, and verification.

The book assumes you do not need help writing a loop or setting up an editor. You need help deciding whether a function should borrow with `std::string_view` or `std::span<const std::byte>`, where ownership should transfer, whether failure should throw or return `std::expected`, when a range pipeline clarifies an algorithm, when a coroutine frame becomes a lifetime risk, and what evidence is strong enough to justify a performance claim. If you want a feature tour, beginner scaffolding, or a header-by-header catalog, this is the wrong book.

That assumption is deliberate. You do not need to arrive as a C++ expert. You do need to arrive as an engineer who is already comfortable reading nontrivial code, reasoning about APIs, tracing control flow across subsystems, and dealing with testing, debugging, performance work, and operational tradeoffs. The book is organized for that reader.

The manuscript moves in seven parts. Parts I and II establish the mental models and everyday vocabulary that keep ordinary code reviewable. Part III moves outward into interfaces, polymorphism, libraries, modules, and ABI reality. Parts IV and V address concurrency, data layout, allocation, and measurement. Part VI covers the verification and diagnostic stack that keeps native systems honest. Part VII closes with complete production shapes: a service, a reusable library, and a reviewer workflow.

That structure exists because the expensive failures in C++ are rarely local. A service keeps request state alive through `shared_ptr`, fans work out into background tasks, logs failure by side effect, and hangs during shutdown. A library accepts borrowed views at the boundary, then stores them past the caller's lifetime. A performance refactor removes one copy and quietly adds contention and allocator pressure elsewhere. Each local choice can look reasonable. The failure shows up when ownership, time, error transport, and cost models interact.

Undefined behavior is a related pressure that runs through the entire book rather than living in one quarantine chapter. A dangling reference, a data race, an invalidated iterator, or a view pipeline that quietly outlives its source can turn a plausible design into a system that fails only under optimization, load, or a different toolchain. The ownership, concurrency, performance, and tooling chapters each address the UB risks most relevant to their domain. Treat UB awareness as a thread that connects the manuscript, not as a one-time warning label.

## How to read this book

You can read straight through. The sequence is meant to build judgment in layers: first ownership and invariants, then everyday library and language tools, then architecture, concurrency, performance, verification, and full production patterns. But the chapter set also supports targeted entry if you already know the class of problem you are solving:

| Starting point | Recommended chapters |
|---|---|
| **Designing interfaces or library boundaries** | Chapters 4, 9, 10, 11, and 22 |
| **Building or repairing a native service** | Chapters 1, 3, 12, 13, 14, 20, and 21 |
| **Writing generic and reusable implementation code** | Chapters 5, 6, 7, 8, and 22 |
| **Working on hot paths, memory behavior, or measurement** | Chapters 15, 16, and 17 |
| **Hardening verification and build diagnostics** | Chapters 18, 19, and 20 |
| **Reviewing production C++ changes** | Chapters 1, 3, 4, 14, 19, 22, and 23 |

Each chapter declares its prerequisites near the top so you can enter where the problem is and fill in background only when needed. The appendices are compact support material: a decision-oriented feature index, a toolchain and diagnostics baseline, a short-form review checklist, and the canonical glossary for the book's core terms.

The primary baseline is C++23. C++26 appears only when it changes a decision you would make today or removes a meaningful cost from an existing pattern. Forward-looking material is there to keep the recommendations honest about the near future, not to turn the book into standards commentary.

If the book succeeds, it should leave you with more than vocabulary. You should be able to look at a design or a diff and explain the trade clearly: what it buys, what it risks, what it commits callers or operators to, and what evidence would prove the choice sound. That is the difference between knowing modern C++ features and using modern C++ well.
