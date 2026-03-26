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

## Anti-pattern: Deep Inheritance Hierarchies and Fragile Base Classes

Virtual dispatch becomes a liability when it grows into deep or wide hierarchies where the base class accumulates obligations over time.

```cpp
// Anti-pattern: a growing base class that every derived type must satisfy.
class Widget {
public:
    virtual void draw(Canvas& c) = 0;
    virtual void handle_input(const InputEvent& e) = 0;
    virtual Size preferred_size() const = 0;
    virtual void set_theme(const Theme& t) = 0;
    virtual void serialize(Archive& ar) = 0;    // added in v2
    virtual void animate(Duration dt) = 0;       // added in v3
    virtual AccessibilityInfo accessibility() = 0; // added in v4
    virtual ~Widget() = default;
};
```

Every new virtual method forces every derived class to implement it or inherit a possibly wrong default. Classes that only need drawing must still address input, serialization, animation, and accessibility. Testing a simple leaf widget requires constructing `Canvas`, `InputEvent`, `Theme`, `Archive`, and `AccessibilityInfo` objects. The base class becomes a change amplifier: a single addition to `Widget` triggers recompilation and potential modification of every derived class across the codebase.

This is the fragile base class problem. The hierarchy appears extensible but is actually brittle because the base class interface keeps growing to serve every consumer.

### Diamond inheritance and semantic ambiguity

Multiple inheritance of interface hierarchies introduces diamond problems that `virtual` inheritance only partially addresses.

```cpp
class Readable {
public:
    virtual std::expected<std::size_t, IoError>
    read(std::span<std::byte> buffer) = 0;
    virtual void close() = 0;  // close the read side
    virtual ~Readable() = default;
};

class Writable {
public:
    virtual std::expected<std::size_t, IoError>
    write(std::span<const std::byte> data) = 0;
    virtual void close() = 0;  // close the write side
    virtual ~Writable() = default;
};

// Diamond: what does close() mean here? Read side? Write side? Both?
class ReadWriteStream : public virtual Readable, public virtual Writable {
public:
    // Single close() must now serve two different semantic contracts.
    // Callers holding a Readable* expect close() to close the read side.
    // Callers holding a Writable* expect close() to close the write side.
    // There is no way to satisfy both through one override.
    void close() override { /* ??? */ }
};
```

Virtual inheritance solves the layout duplication but not the semantic conflict. The result is code that compiles but whose behavior depends on which base pointer the caller holds. This ambiguity is structural. It does not go away with more careful implementation.

### Contrast: type erasure avoids these problems

Type erasure sidesteps hierarchies entirely. Each erased wrapper defines its own minimal contract without forcing unrelated types into a common base.

```cpp
// No base class. No hierarchy. No diamond.
// Any type that is callable with the right signature works.
using DrawAction = std::move_only_function<void(Canvas&)>;
using InputHandler = std::move_only_function<bool(const InputEvent&)>;

struct WidgetBehavior {
    DrawAction draw;
    InputHandler handle_input;
};

// A simple widget only provides what it needs.
// No obligation to implement serialize, animate, or accessibility.
WidgetBehavior make_label(std::string text) {
    return {
        .draw = [t = std::move(text)](Canvas& c) { c.draw_text(t); },
        .handle_input = [](const InputEvent&) { return false; }
    };
}
```

There is no base class to grow. Adding animation support does not force label widgets to change. Testing `draw` does not require constructing an `InputEvent`. Each concern is independently composable. The cost is that you lose the named-object-protocol clarity of a class hierarchy, which may matter if the protocol is genuinely stable and rich. The tradeoff is worth evaluating case by case.

## Type Erasure: Good for Owned Runtime Flexibility

Type erasure is the right tool when you need to store or pass runtime-selected behavior without exposing the concrete type, but you do not need an inheritance hierarchy in the user model.

`std::function` is the most familiar example, but in C++23 `std::move_only_function` is often the better default when copyability is not part of the contract. Many submitted tasks, completion handlers, and deferred operations are naturally move-only because they own buffers, promises, file handles, or cancellation state. Requiring copyability there is not flexible. It is misleading.

Type erasure buys three things:

1. The caller can provide arbitrary callable types.
2. The callee can own the callable past the current stack frame.
3. The public contract can talk about invocation semantics instead of concrete class design.

It also introduces costs that must be treated as design facts rather than implementation trivia: possible heap allocation, indirect call overhead, larger object representation, and sometimes loss of `noexcept` or cv/ref-qualification detail unless you model it carefully.

For many systems, these costs are acceptable. For some, especially hot dispatch loops or high-rate scheduler internals, they are decisive. Measure in the actual workload before arguing from aesthetics.

The middleware system in `examples/web-api/src/modules/middleware.cppm` demonstrates type erasure composition in practice. `Middleware` is defined as `std::function<http::Response(const http::Request&, const http::Handler&)>` -- a type-erased callable that wraps a handler and produces a new handler. The `apply()` function composes one middleware with one handler; `chain()` composes a range of middlewares around a base handler by folding in reverse order:

```cpp
// examples/web-api/src/modules/middleware.cppm
template <std::ranges::input_range R>
    requires std::same_as<std::ranges::range_value_t<R>, Middleware>
[[nodiscard]] http::Handler
chain(R&& middlewares, http::Handler base) {
    http::Handler current = std::move(base);
    for (auto it = std::ranges::rbegin(middlewares);
         it != std::ranges::rend(middlewares); ++it)
    {
        current = apply(*it, std::move(current));
    }
    return current;
}
```

No inheritance hierarchy is involved. Each middleware (logging, CORS, content-type enforcement) is an independent callable. They compose through `chain()` without knowing about each other. The result is a single `http::Handler` that can be handed to the server. This is exactly the strength of type erasure: composable behavior without coupling implementations into a class tree.

### Common pitfall: `std::function` forces copyability on move-only state

`std::function` requires its target to be copyable. This seems harmless until real callback state enters the picture.

```cpp
// This will not compile. std::function requires CopyConstructible.
auto handler = std::function<void()>{
    [conn = std::make_unique<DbConnection>()](){ conn->heartbeat(); }
};
```

Teams work around this by wrapping unique pointers in shared pointers, adding reference counting and shared mutation to code that was naturally single-owner. The workaround compiles but weakens the ownership model.

```cpp
// Workaround: shared_ptr "fixes" compilation but lies about ownership.
auto conn = std::make_shared<DbConnection>();
auto handler = std::function<void()>{
    [conn]() { conn->heartbeat(); }
};
// Now conn is shared. Who shuts it down? When? The ownership story is gone.
```

`std::move_only_function` avoids this entirely. If your callback is submitted, queued, or deferred and will not be copied, it is the correct default in C++23.

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

The `main.cpp` in `examples/web-api/` demonstrates scoped lifetime discipline for captured references. The `TaskRepository` is declared before the router and handlers, and the server is declared last. Because C++ destroys local variables in reverse order of construction, the repository outlives every handler and middleware that captures a reference to it. This ordering is not accidental -- it is the structural guarantee that no erased callable will ever dangle on its captured `repo` reference. When callbacks capture by reference, the enclosing scope must enforce that the referent outlives the callable. Scoping order in `main()` is where that guarantee lives.

## Virtual Protocol or Erased Callable?

A practical decision rule helps.

Choose a virtual interface when behavior is naturally described as a named object protocol with multiple operations or meaningful state transitions. A metrics sink with `record_counter`, `record_histogram`, and `flush` is an object protocol. A storage backend with `get`, `put`, and `remove` is an object protocol.

Choose an erased callable when the abstraction is fundamentally "here is work or logic to invoke later" rather than "here is an object with a semantic role." Retry policies, completion handlers, task submissions, and predicate hooks usually fit this pattern better.

When teams confuse the two, the code gets awkward fast. Virtual single-method types often signal that a callback would be clearer. Conversely, enormous callable signatures with tuples of parameters often signal that a real protocol object should exist.

The example project in `examples/web-api/` shows both sides of this choice in the same codebase. `Router` in `router.cppm` is an object protocol: it has a fluent registration API (`get`, `post`, `put_prefix`, etc.) and manages a route table with multiple named operations. This is naturally a named object with state, not a single callable. By contrast, `http::Handler` is defined as `std::function<Response(const Request&)>` -- a type-erased callable. Each handler is "here is work to invoke when a request matches." The router's `to_handler()` method collapses the entire route table into a single erased callable, bridging the two models cleanly.

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