# Chapter 11: Senders, Receivers, and Structured Concurrency

> **Prerequisites:** This chapter requires the shared-state foundations from Chapter 9 and the coroutine mechanics from Chapter 10. The sender/receiver model is an alternative composition layer for asynchronous work; it does not replace the need to understand data races, lock design, or coroutine frame lifetimes. Readers should be able to write a cooperative cancellation flow with `std::stop_token`, reason about which thread resumes a suspended coroutine, and explain why `std::future::get()` is a blocking composition that wastes a thread. Without that background, the motivation for senders will seem academic rather than structural.
>
> The code samples use C++23 as the baseline. All `std::execution` material is C++26 (P2300) and is marked accordingly. The reference implementation **stdexec** (github.com/NVIDIA/stdexec) is used where a running example is needed before compiler support ships. GCC 14+, Clang 18+, MSVC 17.10+.

## 11.1 The Production Problem

Consider a service that handles an incoming request by performing three steps concurrently: validating credentials against an auth service, fetching user preferences from a database, and retrieving feature flags from a configuration store. All three must complete before the response can be assembled. A 150 ms deadline covers the entire operation. If any step fails, the others must be cancelled. If the server is shutting down, all in-flight operations must drain cleanly — no dangling callbacks, no writes to freed memory, no threads blocked on abandoned work.

Chapter 10 showed that coroutines give you suspension points. Chapter 9 showed that mutexes and atomics give you shared-state protection. Neither gives you a compositional framework for expressing "run A, B, and C concurrently, cancel the rest if one fails, respect a deadline, and guarantee that all child work finishes before the parent scope exits." That composition is what teams build ad hoc — and get wrong.

The three failure modes from Chapter 10 recur here with a new dimension:

1. **Leaked work.** An async operation completes after its parent scope has been destroyed. The completion handler writes into a dangling frame or a recycled connection object.
2. **Lost errors.** A concurrent branch throws, but the joining logic only inspects the first result. The exception is silently destroyed.
3. **Uncancellable operations.** The deadline fires, but the downstream HTTP call has no cancellation path. The thread is consumed until the remote server responds or the TCP timeout expires minutes later.

The root cause in every case is the same: the code does not express the relationship between parent and child operations structurally. Lifetime, error propagation, and cancellation are bolted on after the fact, through shared flags, reference counting, and manual bookkeeping. Every new concurrent branch is a new opportunity for a mismatch.

## 11.2 The Naive Approach: Ad Hoc Async Composition

### 11.2.1 Anti-pattern: Unstructured std::async Fan-Out

```cpp
// Anti-pattern: unstructured fan-out with no cancellation or error aggregation
Response handle_request(const Request& req) {
    auto auth_future = std::async(std::launch::async, [&] {
        return auth_service.validate(req.token());  // RISK: no timeout
    });
    auto prefs_future = std::async(std::launch::async, [&] {
        return db.get_preferences(req.user_id());   // RISK: captures db by ref
    });
    auto flags_future = std::async(std::launch::async, [&] {
        return config_store.get_flags(req.user_id()); // RISK: no cancellation
    });

    auto auth = auth_future.get();    // BUG: blocks indefinitely if auth hangs
    auto prefs = prefs_future.get();  // BUG: if auth threw, prefs/flags still running
    auto flags = flags_future.get();

    return assemble(auth, prefs, flags);
}
```

Every problem from Chapter 10's callback baseline reappears. If `auth_future.get()` throws, the other two futures are still running — their threads are consumed, their results are discarded by the future destructor (which blocks in `std::async`'s case), and the cancellation signal never reaches them. Adding a `std::stop_source` requires threading it through every lambda, registering stop callbacks on every blocking call, and coordinating the flag manually. This is the infrastructure that should not need to be reinvented per call site.

### 11.2.2 Anti-pattern: Callback Chains with Shared State

```cpp
// Anti-pattern: callback spaghetti with manual lifetime management
void handle_request(const Request& req, std::function<void(Response)> on_done) {
    auto ctx = std::make_shared<RequestContext>();  // RISK: shared_ptr as lifetime crutch
    ctx->pending.store(3);

    auth_service.validate_async(req.token(), [ctx, on_done](AuthResult r) {
        ctx->auth = std::move(r);
        if (ctx->pending.fetch_sub(1) == 1) {
            on_done(assemble(ctx->auth, ctx->prefs, ctx->flags)); // BUG: which thread?
        }
    });
    db.get_preferences_async(req.user_id(), [ctx, on_done](Prefs p) {
        ctx->prefs = std::move(p);
        if (ctx->pending.fetch_sub(1) == 1) {
            on_done(assemble(ctx->auth, ctx->prefs, ctx->flags));
        }
    });
    config_store.get_flags_async(req.user_id(), [ctx, on_done](Flags f) {
        ctx->flags = std::move(f);
        if (ctx->pending.fetch_sub(1) == 1) {
            on_done(assemble(ctx->auth, ctx->prefs, ctx->flags));
        }
    });
}
```

The `shared_ptr<RequestContext>` is a red flag: it means nobody can articulate when `ctx` should die, so they let reference counting figure it out. The atomic counter is a hand-rolled `when_all`. Error handling is absent — if any callback receives an error, the others still run, and the partial results are assembled or silently discarded. Adding a deadline means adding a timer, a cancellation flag, and race conditions between the timer firing and the callbacks completing.

This pattern is not obviously wrong the way a data race is. It compiles. It passes smoke tests. It fails under load, under cancellation, and during shutdown — exactly when correctness matters most.

## 11.3 The Sender/Receiver Model

The sender/receiver model (P2300, targeted for C++26) provides a structured answer to the composition problem. The core ideas:

- A **sender** is a lazy description of asynchronous work. It does not start until it is connected to a receiver and the resulting operation state is started. This is analogous to how a range view does not compute elements until iterated.
- A **receiver** consumes the result through exactly one of three channels: **`set_value`** (success), **`set_error`** (failure), or **`set_stopped`** (cancellation). Every sender must complete through exactly one channel. This is a protocol, not a convention.
- An **operation state** is the object returned by connecting a sender to a receiver. It owns all the resources needed for the operation. Starting it initiates the work; its lifetime bounds the work's lifetime.

### 11.3.1 The Three Completion Channels

The three-channel protocol eliminates the ambiguity that plagues callback-based systems. In a callback world, "did the callback fire with an error, or did it not fire at all?" is a question with no reliable answer. In the sender model:

- **`set_value(receiver, values...)`** — the operation succeeded. The values are the result.
- **`set_error(receiver, error)`** — the operation failed. The error is typically `std::exception_ptr` or `std::error_code`, but the model is generic.
- **`set_stopped(receiver)`** — the operation was cancelled. This is distinct from error: cancellation is not a failure, it is a cooperative termination that the parent requested.

A receiver must accept all three. A sender must invoke exactly one. The compiler (through concepts) enforces that a receiver handles the channels a sender may complete with. This is a type-level contract, not a runtime hope.

### 11.3.2 Senders Are Values

A sender is a regular C++ value. It can be stored in a variable, passed to a function, returned from a function, and composed with algorithms — all before any work begins. This is the key difference from `std::async` (which starts work eagerly) and from a coroutine (which has a handle and a frame, making it move-only and harder to compose generically).

```cpp
// C++26 (std::execution)
// A sender that describes work but does not execute it
auto work = stdexec::just(42)
          | stdexec::then([](int x) { return x * 2; });

// No work has happened yet. `work` is a value describing a pipeline.
// Starting it requires connecting to a receiver:
auto [result] = stdexec::sync_wait(std::move(work)).value();
// result == 84
```

`just(42)` creates a sender that completes immediately with the value 42. `then` adapts it: when the upstream sender completes with a value, apply the function and forward the result. `sync_wait` is the bridge from async to sync — it connects a receiver, starts the operation, and blocks the calling thread until completion. It is the only blocking point, and it is explicit.

## 11.4 Schedulers and Execution Contexts

A **scheduler** is a handle to an execution context — a thread pool, an event loop, an inline context for testing. Schedulers produce senders via `schedule()`:

```cpp
// C++26 (std::execution)
stdexec::scheduler auto sched = pool.get_scheduler();
auto work = stdexec::schedule(sched)
          | stdexec::then([] { return do_expensive_computation(); });
```

`schedule(sched)` returns a sender that, when started, enqueues its continuation onto the execution context. This makes thread transitions explicit and composable. The equivalent of "run this on the thread pool" is not a runtime switch statement or a dispatch queue — it is a sender adapter that the type system tracks.

### 11.4.1 Transferring Between Contexts

```cpp
// C++26 (std::execution)
auto work = stdexec::schedule(io_sched)
          | stdexec::then([] { return read_socket(); })
          | stdexec::transfer(compute_sched)
          | stdexec::then([](Buffer buf) { return decompress(buf); })
          | stdexec::transfer(io_sched)
          | stdexec::then([](Payload p) { return write_response(p); });
```

Each `transfer` is a visible, reviewable context switch. There is no implicit "which thread am I on?" question. A reviewer reads the pipeline and knows exactly where each step executes. Compare this with a coroutine that resumes on "whatever thread the I/O completion happens on" — a source of subtle data races when the resumed code touches state that is not thread-safe.

### 11.4.2 Thread Pool Design as an Architectural Decision

The sender model separates the description of work from the execution policy. This means thread pool design becomes an explicit architectural decision rather than an emergent property of `std::async` calls:

- **How many pools?** A common production pattern is two: one for CPU-bound work (sized to core count) and one for I/O-bound work (larger, tolerant of blocking). Senders make the assignment explicit through scheduler selection.
- **What is the queue discipline?** FIFO, priority, work-stealing? The scheduler abstraction lets you swap policies without changing business logic.
- **What happens at capacity?** Back-pressure (reject new work), unbounded queueing (eventual OOM), or caller-runs (inline execution)? This is a scheduler policy, not a per-call-site decision.

## 11.5 Sender Algorithms

The power of the model is in the algorithms. Each algorithm takes senders and produces senders, enabling composition.

### 11.5.1 `then` and `upon_error`

`then` adapts a sender's value channel: if the upstream completes with `set_value`, apply the function. If it completes with `set_error` or `set_stopped`, forward unchanged.

`upon_error` adapts the error channel: if the upstream completes with `set_error`, apply the function (which may return a recovery value or rethrow).

```cpp
// C++26 (std::execution)
auto work = read_from_cache(key)
          | stdexec::then([](CacheEntry e) { return e.value; })
          | stdexec::upon_error([&](std::exception_ptr ep) {
                log_cache_miss(key);
                return fetch_from_db(key);  // fallback
            });
```

### 11.5.2 `let_value` and `let_error`

`let_value` is the monadic bind for senders. The function receives the upstream value and returns a new sender. This enables dynamic pipelines where the next step depends on the result of the previous step.

```cpp
// C++26 (std::execution)
auto work = stdexec::just(request)
          | stdexec::let_value([](const Request& req) {
                // Return a new sender based on the request
                if (req.needs_auth())
                    return validate_and_fetch(req);
                else
                    return fetch_public(req);
            });
```

The distinction from `then` is critical: `then` transforms a value synchronously. `let_value` chains to another asynchronous operation. This is the same distinction as `transform` vs. `and_then` on `std::optional`, or `map` vs. `flat_map` in functional programming.

`let_error` is the corresponding operation on the error channel — use it to implement retry logic or error-dependent recovery that itself requires async work.

### 11.5.3 `when_all`: Structured Concurrency

`when_all` is where the model pays for itself. It takes multiple senders, starts them concurrently, and completes when all have finished.

```cpp
// C++26 (std::execution)
auto work = stdexec::when_all(
    validate_auth(req.token()),
    fetch_preferences(req.user_id()),
    fetch_flags(req.user_id())
) | stdexec::then([](AuthResult auth, Prefs prefs, Flags flags) {
    return assemble(auth, prefs, flags);
});
```

Compare this with the ad hoc `shared_ptr<RequestContext>` + atomic counter from Section 11.2.2. The `when_all` version:

- **Cancels siblings on failure.** If `validate_auth` completes with an error, the other two senders receive a stop request. No manual cancellation flag.
- **Propagates errors.** The error reaches the downstream `upon_error` or `let_error` handler. No silent swallowing.
- **Guarantees child completion before parent proceeds.** The `then` after `when_all` does not run until all three children have completed (successfully, with an error, or stopped). No dangling work.

This last guarantee is the definition of **structured concurrency**: child operations complete before the parent scope exits. It is the async equivalent of RAII's guarantee that destructors run on scope exit.

### 11.5.4 `when_any` and First-Completion Semantics

`when_any` completes when the first child completes, and cancels the rest.

```cpp
// C++26 (std::execution)
auto work = stdexec::when_any(
    fetch_from_primary(key),
    fetch_from_replica(key)
) | stdexec::then([](auto result) {
    return result;  // first successful response wins
});
```

Use cases: hedged requests (send to multiple backends, use the first response), timeouts as a race between work and a timer, and speculative execution.

**Caution:** `when_any` cancels the losers, but cancellation is cooperative. If the losing sender's underlying I/O does not respond to stop requests, the operation state will not be destroyed until the I/O completes. This is not a bug in the model — it is a reminder that cancellation is a request, not a kill signal.

### 11.5.5 `into_variant`

When composing senders that may complete with different value types, `into_variant` wraps the result into a `std::variant`, enabling type-safe handling of heterogeneous outcomes.

```cpp
// C++26 (std::execution)
auto work = stdexec::when_all(
    get_cached_value(key) | stdexec::into_variant(),
    compute_value(key)    | stdexec::into_variant()
);
```

This is typically needed at composition boundaries where the sender type must be erased or unified. Prefer homogeneous pipelines where possible — `into_variant` adds a runtime dispatch cost and makes the downstream code handle multiple alternatives.

## 11.6 Structured Concurrency: The Core Guarantee

Structured concurrency is not a feature of `when_all`. It is a design principle: every piece of async work has a parent scope, and the child work completes before the parent scope exits. This is enforceable because senders are lazy — they do not start until connected and started within a scope.

### 11.6.1 The Async Scope Pattern

```cpp
// C++26 (std::execution) — conceptual; exact API may evolve
stdexec::async_scope scope;

for (const auto& item : work_items) {
    scope.spawn(process(item) | stdexec::on(pool.get_scheduler()));
}

// scope destructor (or explicit join) ensures all spawned work completes.
// This is the async equivalent of joining all threads before the function returns.
stdexec::sync_wait(scope.on_empty());
```

The scope guarantees that no work escapes. If `process(item)` captures a reference to a local variable, that variable is alive for the duration of the scope. Compare this with `std::async` fire-and-forget, where the future destructor may or may not block (it does for `std::async`, it does not for `std::packaged_task`), and where "the work is done" is never structurally guaranteed.

### 11.6.2 Why Structured Concurrency Matters for Production

The guarantees are practical, not theoretical:

- **Resource cleanup is deterministic.** A database connection borrowed for an async operation is returned when the scope exits, not when a reference count hits zero at an unpredictable time.
- **Error propagation is complete.** All errors from child work are available to the parent. No error is silently destroyed by a detached future.
- **Shutdown is tractable.** Stopping a server means requesting cancellation on the root scope. Cancellation propagates downward through the sender tree. The root scope's join blocks until all work has drained. No "wait and hope" shutdown logic.
- **Reasoning is local.** A reviewer reading a function can see all the async work it creates and know that it all finishes before the function returns. This is the async equivalent of "no raw `new` without a corresponding owner."

## 11.7 Senders and Coroutines: When to Use Which

Senders and coroutines are complementary, not competing. The choice depends on what you are expressing.

**Use coroutines when the logic is sequential with suspension points.** A request handler that awaits three things in sequence, with branching and error handling, reads best as a coroutine. The linear control flow is the value.

```cpp
Task<Response> handle(Request req) {
    auto auth = co_await validate(req.token());
    if (!auth.ok()) co_return error_response(401);
    auto data = co_await fetch_data(req.id());
    co_return assemble(auth, data);
}
```

**Use senders when composing concurrent or parallel work.** Fan-out, fan-in, racing, and pipeline construction are composition problems. Senders express them declaratively.

```cpp
// C++26 (std::execution)
auto work = stdexec::when_all(validate(token), fetch_data(id), get_flags(id))
          | stdexec::then(assemble);
```

**Interop:** A coroutine can `co_await` a sender. A sender algorithm can wrap a coroutine. The boundary is where sequential control flow meets compositional structure. In practice, business logic lives in coroutines; infrastructure (fan-out, retries, timeouts, scheduling) lives in sender pipelines.

```cpp
// C++26 (std::execution) — coroutine consuming a sender
Task<Response> handle(Request req) {
    auto [auth, prefs, flags] = co_await stdexec::when_all(
        validate(req.token()),
        fetch_preferences(req.user_id()),
        fetch_flags(req.user_id())
    );
    co_return assemble(auth, prefs, flags);
}
```

This combines the readability of coroutines (linear control flow, visible error handling) with the compositional power of senders (structured fan-out with cancellation). It is the pattern most production code should converge on.

## 11.8 Bridging to C++26: stdexec and Practical Adoption

As of early 2026, `std::execution` is in C++26 but compiler support is incomplete. The reference implementation **stdexec** (NVIDIA/stdexec) provides a production-quality implementation that tracks the standard closely.

### 11.8.1 Using stdexec Today

```cmake
# CMakeLists.txt
include(FetchContent)
FetchContent_Declare(
    stdexec
    GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
    GIT_TAG main
)
FetchContent_MakeAvailable(stdexec)
target_link_libraries(my_target PRIVATE STDEXEC::stdexec)
```

Replace `std::execution` with `stdexec` in the namespace. The API surface is intentionally aligned:

```cpp
#include <stdexec/execution.hpp>
namespace ex = stdexec;

auto work = ex::just(42)
          | ex::then([](int x) { return x * 2; });
auto [result] = ex::sync_wait(std::move(work)).value();
```

### 11.8.2 Building Sender-Like Abstractions Without stdexec

If adopting stdexec is not feasible (toolchain constraints, approval processes), the structured concurrency principles still apply. The minimum viable abstraction:

1. **A `WhenAll` that cancels siblings on failure.** This can be built on `std::jthread`, `std::stop_source`, and `std::latch`. It will be heavier than the sender version (it consumes threads), but it provides the structural guarantee.

```cpp
// C++23 — minimal structured when_all without std::execution
template <typename F1, typename F2>
auto when_all_blocking(std::stop_token st, F1&& f1, F2&& f2) {
    using R1 = std::invoke_result_t<F1, std::stop_token>;
    using R2 = std::invoke_result_t<F2, std::stop_token>;

    std::stop_source local_stop;
    std::optional<R1> result1;
    std::optional<R2> result2;
    std::exception_ptr ep1, ep2;

    // Propagate external cancellation to local scope
    std::stop_callback external_cb(st, [&] { local_stop.request_stop(); });

    {
        std::jthread t1([&](std::stop_token) {
            try {
                result1.emplace(f1(local_stop.get_token()));
            } catch (...) {
                ep1 = std::current_exception();
                local_stop.request_stop();  // cancel sibling
            }
        });

        std::jthread t2([&](std::stop_token) {
            try {
                result2.emplace(f2(local_stop.get_token()));
            } catch (...) {
                ep2 = std::current_exception();
                local_stop.request_stop();  // cancel sibling
            }
        });
        // jthread destructors join here — structured concurrency guarantee
    }

    if (ep1) std::rethrow_exception(ep1);
    if (ep2) std::rethrow_exception(ep2);
    return std::pair{std::move(*result1), std::move(*result2)};
}
```

This is heavier than senders (two OS threads per call), but it provides the three properties that matter: sibling cancellation, error propagation, and child-completion-before-parent-exit. It is a viable bridge for teams that need structured concurrency today.

2. **An explicit async scope.** A class that tracks spawned `std::jthread` instances and joins them on destruction. This is the poor man's `async_scope`, and it prevents the most common leak: fire-and-forget threads that outlive their parent.

3. **A deadline wrapper.** A function that races work against a timer, using `std::stop_source` to cancel the work when the timer fires. This replaces ad hoc timeout logic scattered across call sites.

### 11.8.3 Migration Path

The recommended migration path:

1. **Adopt structured concurrency discipline immediately.** Use `std::jthread` + `std::stop_token` to build `when_all` and scope abstractions. Ban fire-and-forget threads in code review.
2. **Introduce stdexec as a library dependency** when the toolchain supports it. Wrap existing async operations as senders.
3. **Replace hand-rolled primitives with sender algorithms** (`when_all`, `let_value`, `transfer`). The type-level composition catches errors that the `jthread` version cannot.
4. **Switch to `std::execution`** when compiler support is production-ready. The stdexec API is designed for this transition — it is a namespace change, not a rewrite.

## 11.9 Tradeoffs and Limitations

### 11.9.1 Compile-Time Cost

Sender pipelines produce deeply nested template types. A pipeline of five `then` adapters produces a type that encodes the entire chain. This is worse than coroutines (which erase the type behind `coroutine_handle`) and comparable to ranges (which have the same nesting problem). Mitigations:

- **Type erasure at API boundaries.** `stdexec::any_sender_of<T>` erases the sender type, similar to `std::function` for callables. Use it at module boundaries; avoid it in hot inner loops.
- **Explicit instantiation.** For key pipeline shapes that appear frequently, explicit template instantiation in a `.cpp` file reduces redundant instantiation across translation units.

### 11.9.2 Debugging Difficulty

A sender pipeline that fails produces an error somewhere in the chain. Unlike a coroutine, there is no logical "stack" to inspect — the sender's operation state is a nested object on the heap or the stack of the operation state holder. Debugging strategies:

- **Instrument sender adapters.** A `tap` or `log_value` adapter that logs the value passing through a pipeline is indispensable in production.
- **Use sync_wait in tests.** Running sender pipelines synchronously in tests produces deterministic, debuggable execution.
- **Name your senders.** Break long pipelines into named intermediate variables. `auto validated = validate(req);` is debuggable. A 15-link pipe expression is not.

### 11.9.3 Error Messages

Template error messages from sender type mismatches are long. A missing `then` adapter or a function with the wrong signature produces errors that reference deeply nested template instantiations. This is the same problem as pre-concepts template errors. C++26 concepts on sender algorithms help, but do not eliminate the issue. The practical defense is to build and test incrementally — add one adapter at a time.

### 11.9.4 Ecosystem Maturity

As of early 2026, `std::execution` is standardized but not yet implemented in all major compilers. stdexec is the most complete implementation. Libraries that expose sender-based APIs are emerging but not yet ubiquitous. Teams adopting senders today should expect to write adapter layers between sender pipelines and callback-based or future-based libraries.

## 11.10 Testing and Tooling

### 11.10.1 Testing Sender Pipelines

**Use `sync_wait` as the test harness.** `sync_wait` blocks the test thread and returns the result (or rethrows the error). This makes sender-based code testable with standard synchronous test frameworks.

```cpp
TEST(Pipeline, ValuePath) {
    auto work = stdexec::just(10)
              | stdexec::then([](int x) { return x + 1; });
    auto [result] = stdexec::sync_wait(std::move(work)).value();
    EXPECT_EQ(result, 11);
}
```

**Use an inline scheduler for deterministic tests.** An inline scheduler runs work on the calling thread, eliminating non-determinism from thread scheduling.

```cpp
struct InlineScheduler {
    struct sender {
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t()>;
        // ... connects and immediately completes on the caller's thread
    };
    sender schedule() noexcept { return {}; }
    bool operator==(const InlineScheduler&) const = default;
};
```

**Test cancellation explicitly.** Create a `std::stop_source`, request a stop before or during execution, and verify that the sender completes with `set_stopped` rather than `set_value` or `set_error`.

```cpp
TEST(Pipeline, CancellationPropagates) {
    std::stop_source ss;
    ss.request_stop();

    auto work = stdexec::just(42)
              | stdexec::let_value([](int) {
                    // This should not execute if stopped
                    ADD_FAILURE() << "should have been cancelled";
                    return stdexec::just(0);
                });
    // Connect with a receiver wired to the stop token
    // and verify set_stopped is called rather than set_value
}
```

**Test error propagation through `when_all`.** Verify that when one branch of `when_all` fails, the other branches are stopped and the error reaches the parent.

### 11.10.2 Sanitizer Considerations

- **AddressSanitizer (ASan):** Catches use-after-free in operation state objects. If a sender's operation state is destroyed while a child operation still holds a reference, ASan reports the access. This is the most common bug when hand-rolling sender adapters.
- **ThreadSanitizer (TSan):** Catches races in receiver completion. A receiver's `set_value` must not be called concurrently from multiple threads. `when_all` implementations must synchronize the transition from "last child completed" to "invoke parent receiver." TSan will catch a missing synchronization here.
- **LeakSanitizer (LSan):** Catches operation state that is started but never completed. If a sender pipeline is abandoned without completing, the operation state leaks. LSan will report it. In production, this indicates a missing scope join or a fire-and-forget sender.

Run all three in CI. Sender bugs, like coroutine bugs, are scheduling-dependent and may not reproduce without sanitizers.

### 11.10.3 Compiler Support Status

| Feature | GCC 14 | Clang 18 | MSVC 17.10 |
|---------|--------|----------|------------|
| `std::execution` (P2300) | Partial (stdexec) | Partial (stdexec) | Not yet |
| Sender concepts | Via stdexec | Via stdexec | Via stdexec |
| `sync_wait` | Via stdexec | Via stdexec | Via stdexec |
| `std::stop_token` (C++20) | Yes | Yes | Yes |
| `std::jthread` (C++20) | Yes | Yes | Yes |

For production use today, link against stdexec. The transition to `std::execution` will be a namespace change when compiler support lands.

## 11.11 Review Checklist

Use this checklist during code review of sender-based or structured-async code.

- [ ] **No fire-and-forget senders.** Every sender is connected, started, and its completion is observed. Detached senders are treated as a defect requiring explicit justification.
- [ ] **Structured scoping.** All concurrent child work completes before the parent scope exits. `async_scope` or equivalent is used for dynamic spawning. No orphaned operations.
- [ ] **Three-channel handling.** Receivers handle `set_value`, `set_error`, and `set_stopped`. No channel is silently ignored. `set_stopped` is distinct from `set_error` in the code's control flow.
- [ ] **Cancellation propagates.** `std::stop_token` is threaded through sender chains. Long-running or blocking operations register stop callbacks. `when_all` cancels siblings on failure.
- [ ] **Scheduler is explicit.** Every sender that performs work on a specific execution context uses `schedule` or `transfer`. No implicit assumptions about which thread a continuation runs on.
- [ ] **Deadlines exist.** External calls are raced against a timer sender. No unbounded waits.
- [ ] **Type erasure at boundaries.** Sender types are erased (`any_sender_of`) at API boundaries to limit template instantiation. Inner pipelines remain concrete for performance.
- [ ] **Pipelines are named and broken up.** No single pipeline expression exceeds ~5 adapters. Intermediate senders are named for debuggability and readability.
- [ ] **Error messages have been checked.** The pipeline compiles incrementally. Type mismatches are caught by adding one adapter at a time, not by reading a 200-line template error.
- [ ] **Sanitizers pass.** ASan, TSan, and LSan run against sender pipelines in CI, including cancellation, error, and shutdown paths.
- [ ] **stdexec version is pinned.** If using stdexec as a bridge, the version is pinned to a release tag, not `main`. API changes between commits are expected during standardization convergence.
