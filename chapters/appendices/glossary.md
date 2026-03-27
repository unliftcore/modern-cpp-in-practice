# Appendix D: Glossary

These definitions are the canonical meanings of the book's core terms. They are intentionally short. If a chapter relies on one of these words, it should use it in this sense unless it says otherwise.

## Core Contracts

**Ownership**
The responsibility for keeping a resource valid and releasing it exactly once when its useful lifetime ends.

**Borrowing**
Using data or a resource without taking over its lifetime. A borrow is valid only while the owner keeps the underlying object alive and in the required state.

**Lifetime**
The interval during which an object, resource, or view remains valid to use. Lifetime is broader than scope once work crosses callbacks, threads, queues, or suspension points.

**RAII**
Resource Acquisition Is Initialization: tying resource ownership to object lifetime so destruction performs cleanup automatically and composes with exceptions and early returns.

**Contract**
What a type or function promises about ownership, validity, failure, cost, and observable behavior. Contracts should be readable from the boundary, not inferred from implementation folklore.

**Invariant**
A property of an object or subsystem that must remain true for the design to be considered valid. Synchronization protects invariants, not merely fields.

**Value semantics**
A design in which objects behave like self-contained values: copying duplicates the value, moving transfers it, and equality is about state rather than object identity.

**Identity**
The fact that a particular object instance matters as itself rather than only as the value it currently holds. Identity usually brings aliasing, lifetime, or synchronization consequences.

## Failure and Boundaries

**Failure boundary**
The layer where failure is translated into the form the next layer is expected to handle: exception, `std::expected`, status, process exit, log and continue, or some other deliberate policy.

**Boundary translation**
Converting unstable or low-level failures into a smaller, stable vocabulary near the dependency that produced them.

**Undefined behavior**
Program behavior for which the C++ standard imposes no requirements. In practice it means the compiler is free to assume the bug does not occur, which can turn a local mistake into arbitrary results.

**Reviewable**
Easy enough to reason about that a competent reviewer can identify ownership, invariants, failure shape, and meaningful costs without reverse-engineering the whole subsystem.

## Concurrency and Throughput

**Structured concurrency**
Designing concurrent work so child tasks belong to parent scopes, have bounded lifetimes, and are canceled or completed before the parent is considered done.

**Cancellation**
The deliberate request to stop work that is no longer useful because a parent failed, a deadline expired, shutdown began, or overload policy requires shedding work.

**Suspension boundary**
A point such as `co_await` where control may resume later, elsewhere, or not at all unless the owning task and its data remain valid.

**Contention**
Delay caused by multiple threads or tasks competing for the same lock, core time, queue capacity, cache lines, or dependency budget.

**Backpressure**
The system's explicit answer to work arriving faster than it can be completed. Good backpressure is an admission policy, not an oversized queue.

**Throughput**
The amount of useful work completed per unit time. Higher throughput is not automatically better if it is bought with unacceptable tail latency, memory growth, or operational risk.

## Data and Performance

**Locality**
How well the runtime layout of code and data matches the access pattern of the hardware. Good locality usually reduces cache misses, pointer chasing, and memory stalls.

**Cost model**
The concrete explanation of where time, memory, allocation, copying, synchronization, and indirection costs enter a design.

**Hot path**
Code that runs often enough, or under enough pressure, that small costs in allocation, layout, or synchronization materially affect system behavior.

**Vocabulary type**
A standard type such as `std::span`, `std::string_view`, `std::optional`, `std::expected`, or `std::variant` used to make contracts legible at boundaries.

## Packaging and Operations

**ABI**
Application Binary Interface: the binary-level contract covering calling conventions, layout, symbol names, exception behavior, and other details that determine whether separately built components can link and run together.

**Observability**
The runtime evidence needed to explain behavior in production: logs, metrics, traces, crash artifacts, and the metadata that makes them diagnosable.

**Diagnostic build**
A build configuration optimized to find or explain bugs rather than only to maximize runtime speed, typically through warnings, assertions, sanitizers, debug info, and symbolization.