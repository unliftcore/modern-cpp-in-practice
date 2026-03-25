# Runtime Polymorphism, Type Erasure, and Callbacks

This chapter assumes you already know how to define a good source-level boundary. The question now is how to represent runtime variability at that boundary without lying about cost, lifetime, or ownership.

## The Production Problem

Sooner or later, production C++ code must choose behavior at runtime. A service picks a retry strategy from configuration. A scheduler stores work submitted by arbitrary callers. A library accepts hooks for telemetry, filtering, or authentication. A plugin host loads behavior from separately compiled modules. None of these problems can be solved with templates alone, because the concrete type is not known at compile time where the decision matters.

This is where teams often reach for a familiar abstraction and keep using it everywhere. Some use virtual functions for everything. Some wrap everything in `std::function`. Some build hand-rolled type erasure around `void*` and function pointers because they fear allocations. The result is usually an interface that works but hides important facts: whether the callable is owned, whether it can allocate, whether it can throw, whether it can outlive captured state, and whether dispatch cost matters in the actual hot path.

This chapter separates the main runtime indirection tools in modern C++23: classic virtual dispatch, type erasure, and callback forms. The goal is not to crown one as best. The goal is to choose the smallest tool that matches the lifetime, ownership, and performance requirements of the boundary you are building.

## First Decide What Kind of Variability You Need

Not all runtime flexibility is the same.

There are at least four common cases:

1. A stable object protocol with multiple long-lived implementations.
2. A callable submitted for later execution.
3. A short-lived hook invoked synchronously during one operation.
4. A plugin or extension point crossing packaging or ABI boundaries.

These cases look similar from ten thousand feet because each involves calling "something dynamic." They differ sharply in what the callee needs to own, how long the behavior must live, and which costs matter.

If you skip this classification step, the wrong abstraction can look acceptable for years. A synchronous hook might accidentally require heap allocation because it is modeled as `std::function`. A background task system might store borrowed callback state because its API looked convenient. A plugin system might expose C++ class hierarchies across compiler boundaries and call that architecture. The mistakes are structural, not syntactic.

## Virtual Dispatch: Good for Stable Object Protocols

Virtual functions are still the clearest tool when you need a stable protocol over a family of long-lived objects. A storage backend interface, a message sink, or a strategy object chosen once and reused heavily can all fit this model.

The strengths are straightforward:

1. Ownership is usually explicit in the surrounding object graph.
2. The protocol is easy to document as named operations.
3. The interface can evolve carefully through additional methods only when necessary.
4. Tooling, debuggers, and reviewers understand it immediately.

The weaknesses are just as real. Hierarchies tempt over-generalization. Per-call dispatch is indirect and hard to inline. The interface must commit to object identity and mutation semantics even when a simpler callable would do. Public inheritance also couples the caller to one representation of customization: objects with vtables.

Use virtual dispatch when the abstraction is naturally an object protocol. Do not use it just because behavior varies.

## Type Erasure: Good for Owned Runtime Flexibility

Type erasure is the right tool when you need to store or pass runtime-selected behavior without exposing the concrete type, but you do not need an inheritance hierarchy in the user model.

`std::function` is the most familiar example, but in C++23 `std::move_only_function` is often the better default when copyability is not part of the contract. Many submitted tasks, completion handlers, and deferred operations are naturally move-only because they own buffers, promises, file handles, or cancellation state. Requiring copyability there is not flexible. It is misleading.

Type erasure buys three things:

1. The caller can provide arbitrary callable types.
2. The callee can own the callable past the current stack frame.
3. The public contract can talk about invocation semantics instead of concrete class design.

It also introduces costs that must be treated as design facts rather than implementation trivia: possible heap allocation, indirect call overhead, larger object representation, and sometimes loss of `noexcept` or cv/ref-qualification detail unless you model it carefully.

For many systems, these costs are acceptable. For some, especially hot dispatch loops or high-rate scheduler internals, they are decisive. Measure in the actual workload before arguing from aesthetics.

## Callback Forms: Borrowed, Owned, and One-Shot

The word "callback" hides three different contracts.

### Borrowed synchronous callbacks

These are invoked during the current call and not retained. Logging visitors, per-record validation hooks, or traversal callbacks often fit here. The crucial property is that the callee does not store the callable.

In this case, forcing the API through an owning wrapper is usually unnecessary. A templated callable parameter may be simplest when the call stays internal. If you need a non-templated surface, a lightweight borrowed callable view can work, but standard C++23 does not yet ship `function_ref`. Many teams therefore use constrained templates for synchronous hooks inside a component and reserve type erasure for cases that genuinely need ownership.

### Owned deferred callbacks

A queue, scheduler, timer wheel, or asynchronous client often needs to retain work beyond the current stack frame. This is the natural home for `std::move_only_function` or a custom erased task type with explicit allocation rules.

Here the questions are concrete:

1. Does the queue own the callable?
2. Is copying part of the API, or only moving?
3. Can submission allocate?
4. Must the callable be `noexcept`?
5. What happens at shutdown to unrun callbacks?

These are interface questions, not implementation details.

### One-shot completion handlers

A completion path that fires exactly once is often better modeled as move-only from the start. This aligns the type with reality and prevents accidental sharing of stateful handlers.

```cpp
class TimerQueue {
public:
	using Task = std::move_only_function<void() noexcept>;

	void schedule_after(std::chrono::milliseconds delay, Task task);
};
```

This signature says something important. The queue takes ownership, may run later, and expects the task not to throw across the scheduler boundary. That is much more precise than a generic `std::function<void()>` parameter.

## Anti-pattern: Flexible Surface, Ambiguous Lifetime

Many callback bugs come from APIs that look flexible but fail to say who owns what.

```cpp
// Anti-pattern: ambiguous retention and capture lifetime.
class EventSource {
public:
	void on_message(std::function<void(std::string_view)> handler);
};

void wire(EventSource& source, Session& session) {
	source.on_message([&session](std::string_view payload) {
		session.record(payload);
	});
	// RISK: if EventSource stores the handler past Session lifetime, this dangles.
}
```

The problem is not only the reference capture. The problem is that the API failed to state whether the handler is borrowed for one operation, stored until unregistration, invoked concurrently, or called during destruction. The abstraction chose generality over a usable contract.

A stronger design names the ownership and lifetime model explicitly. If the callback is retained, registration should return a handle or registration object whose lifetime governs subscription. If the callback is synchronous, the API should not accept an owning erased callable just to appear generic.

## Virtual Protocol or Erased Callable?

A practical decision rule helps.

Choose a virtual interface when behavior is naturally described as a named object protocol with multiple operations or meaningful state transitions. A metrics sink with `record_counter`, `record_histogram`, and `flush` is an object protocol. A storage backend with `get`, `put`, and `remove` is an object protocol.

Choose an erased callable when the abstraction is fundamentally "here is work or logic to invoke later" rather than "here is an object with a semantic role." Retry policies, completion handlers, task submissions, and predicate hooks usually fit this pattern better.

When teams confuse the two, the code gets awkward fast. Virtual single-method types often signal that a callback would be clearer. Conversely, enormous callable signatures with tuples of parameters often signal that a real protocol object should exist.

## Cost Models Matter, but Usually in Specific Places

Runtime indirection always has a cost model. The mistake is to discuss it abstractly.

Virtual dispatch cost is usually tiny relative to IO, parsing, locking, allocation, or cache misses. In a hot numeric inner loop or per-packet classifier running millions of times per second, it may be very real. Type erasure may allocate; whether that matters depends on submission rate, allocator behavior, and tail latency sensitivity. Small-buffer optimization can help, but relying on unspecified thresholds is not a stable contract.

Do not guess. If dispatch is on a hot path, measure branch behavior, instruction footprint, allocation rate, and end-to-end throughput in representative scenarios. If it is not hot, choose the abstraction that makes ownership and lifetime easiest to reason about.

## Exceptions, Cancellation, and Shutdown Semantics

Runtime indirection often sits at boundaries where failure handling gets sloppy. A callback API that does not document whether exceptions may cross it is unfinished. A task queue that accepts work without defining shutdown semantics is unfinished. A plugin hook that can reenter the host while invariants are half-updated is unfinished.

Decide explicitly:

1. Can the callable throw? If not, encode that in the type when practical.
2. If invocation fails, who translates or logs the failure?
3. Can callbacks run concurrently?
4. How are pending callbacks cancelled or drained during shutdown?
5. Is reentrancy allowed?

These decisions matter more than whether the abstraction used a vtable or an erased function wrapper.

## Packaging Boundaries Change the Answer

Inside one program built as a unit, all three techniques are fair game. Across plugin boundaries or public SDKs, the answer changes. Exposing C++ runtime polymorphism across binary boundaries brings ABI assumptions with it. Erased callables that capture allocator or exception behavior can also become unstable across module or shared-library boundaries.

That is why chapter 11 treats packaging and ABI as a separate problem. Runtime flexibility inside a process is one design question. Binary contracts between separately built artifacts are another. Do not let a convenient in-process abstraction accidentally become your distribution contract.

## Verification and Review Questions

Review runtime indirection by asking what the type promises, not what the implementation currently does.

1. Does the boundary own the callable or merely borrow it?
2. Can the callable be retained, moved across threads, or invoked during shutdown?
3. Is copyability required by the contract, or only inherited from a convenient wrapper?
4. Are exception and cancellation semantics explicit?
5. Is dispatch cost material in this workload, or are hidden allocations the real issue?
6. Would a named protocol object be clearer than a giant callable signature, or vice versa?

Verification should include allocation tracing for callback-heavy paths, sanitizer coverage for lifetime bugs in captured state, and targeted benchmarks when dispatch sits in a measured hot path. Unit tests alone are weak here because the most expensive failures are usually lifetime races, shutdown bugs, and throughput collapse under sustained load.

## Takeaways

Runtime indirection is not one thing. It is a set of tools for different lifetime and ownership models.

Use virtual dispatch for stable object protocols. Use type erasure when you need owned runtime-selected behavior without exposing a hierarchy. Use callback forms that match whether invocation is synchronous, deferred, or one-shot. Prefer `std::move_only_function` over `std::function` when ownership is single and copyability would misstate the contract.

Most importantly, make lifetime, retention, exception behavior, and shutdown semantics visible at the boundary. Hidden allocations are annoying. Hidden lifetime rules are how systems fail.