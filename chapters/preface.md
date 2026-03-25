# Preface

This is a production handbook for modern C++, not a language course. It is written for programmers who already know how serious software gets built and maintained, but who need a sharper mental model for making decisions in C++23 codebases. The subject is not syntax coverage. It is ownership, lifetime, interfaces, failure boundaries, concurrency, performance, tooling, and the maintenance cost that follows every design choice.

The book assumes you do not need help writing a loop, defining a class, or setting up an editor. You need help deciding what an API should expose, where an allocation boundary belongs, when dynamic polymorphism earns its cost, how exceptions should be contained, and how to keep a large codebase from turning opaque under feature pressure. It does not spend chapters on installation, hello world programs, generic object-oriented advice, standard library catalogs, or a tour of every feature added since C++11. If you need those things, other books serve that purpose well.

The assumed background is deliberate. You should already be comfortable reading nontrivial code, reasoning about APIs, following control flow across layers, and dealing with testing, debugging, and performance work in production systems. Experience in C, Rust, Java, Go, C#, Python, or another systems-adjacent environment is enough if you already think in terms of invariants, failure handling, and operational consequences. You do not need to arrive as a C++ expert. You do need to arrive as an engineer who understands that clarity and correctness get harder under pressure, not easier.

That pressure is why modern C++ is difficult. Consider a service that stores session state behind a `shared_ptr`. Locally the choice looks safe: no manual cleanup, no dangling pointer. But the pointer crosses a thread boundary, a shutdown path races against an in-flight request, and a post-mortem reveals that the destructor ran on the wrong thread, tearing down a connection pool that was still draining. The bug is not in any single feature. It is in the interaction between ownership, lifetime, concurrency, and shutdown order — all of which were decided separately, by different people, at different times. That kind of emergent failure is the central difficulty of production C++, and it is the kind of problem this book is organized around.

Undefined behavior is a related pressure that runs through the entire book rather than living in a single chapter. A signed overflow, a dangling reference, a data race — each gives the compiler permission to assume the impossible and optimize accordingly. The result is bugs that appear only under specific optimization levels, toolchains, or workloads, and that resist conventional debugging. Chapters on ownership, concurrency, and testing each address the UB risks most relevant to their domain, and the tooling chapters explain the sanitizers and analysis passes that catch what code review cannot. Treat UB awareness as a thread that connects the book, not a topic that belongs in one place.

## How to Read This Book

You can read straight through — the sequence builds from foundations toward scale, performance, tooling, and maintenance. But the book is also organized for targeted entry based on your current role or problem:

| Starting point | Recommended chapters |
|---|---|
| **Library or API author** — designing interfaces that other teams consume | Chapters 5, 6, 7, then 3 for failure boundaries |
| **Service developer** — building and operating backend systems | Chapters 1, 2, then Part III (concurrency) and Chapter 12 |
| **Code reviewer** — evaluating C++ changes for correctness and maintainability | Chapters 1, 3, 16, and the review checklists in each chapter |
| **Team lead or architect** — setting standards and modernization strategy | Chapters 15, 16, then Part II for boundary design |
| **Performance engineer** — diagnosing and fixing latency or resource problems | Part IV, then Chapter 10 for data layout, Chapter 7 for concurrency |

Each chapter declares its prerequisites at the top, so you can always check whether you need to read something earlier first.

The primary baseline is C++23. C++26 appears only when it changes a decision you would make today or removes a meaningful cost from an existing pattern. Forward-looking material is there to keep the book honest about the near future, not to turn the manuscript into standard-evolution commentary. The main body is written for code that ships now.

If the book succeeds, it should leave you with more than vocabulary. You should be able to stand in a code review and explain the cost of a design choice — not just say that one approach is "more modern" than another, but articulate what it buys, what it risks, and where the maintenance burden lands. That is the difference between knowing C++ features and using C++ well in production.
