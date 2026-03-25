# Chapter 8: Tasks, Coroutines, and Cancellation

*Prerequisites: Chapters 1–2 and 7. Task lifetime depends on ownership. Failure propagation and synchronization must already be understood.*

> **Prerequisites:** This chapter assumes you can already reason about resource ownership across scopes (Chapter 1), define failure boundaries that do not leak side effects (Chapter 2), and coordinate shared mutable state under contention (Chapter 7). Asynchronous programming amplifies every weakness in those foundations. If ownership is vague, a suspended coroutine will dangle. If failure policy is inconsistent, a cancelled task will swallow errors or propagate them into the wrong handler. If synchronization is ad hoc, resumption on the wrong thread will corrupt state that was safe in a single-threaded mental model.
>
> The code samples use C++23 as the baseline. `std::execution` (P2300) appears in C++26 notes where it changes a recommendation.

## 8.1 The Production Problem

A service receives an RPC, queries a database, calls two downstream services in parallel, merges the results, and replies. The whole operation has a 200 ms deadline. Any of the downstream calls may hang. Shutdown may arrive mid-flight. The database connection belongs to a pool whose lifetime is scoped to the server, not the request.

This is not a concurrency problem in the Chapter 7 sense — there is no contention over shared counters or lock ordering. The difficulty is structural: work is split across time, each piece has a different lifetime, and cancellation must propagate without leaking resources or corrupting shared infrastructure. The synchronous call chain that reviewers can follow top-to-bottom no longer exists. Control flow is scattered across callbacks, continuations, or suspension points, and the "stack" at any given moment may not contain the frames that matter.

Teams that solve this casually end up with one of three failure modes:

1. **Leaked work.** A cancelled request's downstream calls complete after the reply is sent, writing results into freed memory or into a response object that no longer exists.
2. **Stuck shutdown.** The server's destructor joins a thread pool that is blocked on I/O that was never cancelled, producing a hang on process exit.
3. **Silent error loss.** An exception thrown inside a coroutine is stored in a future that nobody awaits, and the failure vanishes.

## 8.2 The Callback and Future Baseline

Before coroutines, the standard approach was `std::async`, `std::future`, and manual callback chains.

```cpp
// Anti-pattern: fire-and-forget async with no cancellation path
std::future<Response> handle_request(const Request& req) {
    auto db_future = std::async(std::launch::async, [&] {
        return db_pool.query(req.key());  // BUG: captures db_pool by ref;
    });                                    // pool may outlive future, or not

    auto svc_future = std::async(std::launch::async, [&] {
        return downstream.call(req.payload());  // RISK: no timeout, no cancellation
    });

    return std::async(std::launch::async, [df = std::move(db_future),
                                           sf = std::move(svc_future)]() mutable {
        auto db_result = df.get();   // blocks indefinitely
        auto svc_result = sf.get();  // blocks indefinitely
        return merge(db_result, svc_result);
    });
}
```

Problems with this baseline:

- **No cancellation.** `std::future` has no mechanism to signal that the result is no longer needed. Once launched, the work runs to completion or blocks forever.
- **Lifetime hazard.** The lambda captures `db_pool` and `downstream` by reference. If the server shuts down before the futures complete, those references dangle.
- **Thread explosion.** Each call to `std::async` with `launch::async` may spawn a new thread. Under load, this produces thousands of threads, most of which are blocked on I/O.
- **No deadline.** There is no way to express "abandon this entire operation after 200 ms" without bolting on a separate timer and a shared cancellation flag — which is exactly the infrastructure this code was trying to avoid.
- **Blocking composition.** Combining two futures requires a third thread that blocks on both. You cannot express "when both complete, do X" without occupying a thread for the wait.

`std::future` was designed for simple fork-join parallelism. It is not a task system, and treating it as one creates the problems above.

## 8.3 Coroutine-Based Task Model

C++20 coroutines provide the suspension mechanism. They do not provide a task type, an executor, a cancellation protocol, or a scheduler. Those are design decisions that the team must make or import from a library. This section builds the pieces incrementally.

### 8.3.1 A Minimal Awaitable Task

A production task type needs to express: "this operation will produce a value (or an error) at some future point, and the caller can `co_await` it without blocking a thread."

The following sample is intentionally partial. It shows the ownership and continuation mechanics, not a production-ready coroutine type. In particular, it omits `void` specialization, allocator customization, cancellation wiring, and the full result-state machinery you would ship in a library.

```cpp
#include <concepts>
#include <coroutine>
#include <exception>
#include <utility>
#include <variant>

template <typename T>
class Task {
public:
    using value_type = T;

    struct promise_type {
        static_assert(!std::same_as<T, std::monostate>,
            "Minimal Task sample assumes T is not std::monostate.");

        std::variant<std::monostate, T, std::exception_ptr> result_;
        std::coroutine_handle<> continuation_;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        auto final_suspend() noexcept {
            struct Awaiter {
                std::coroutine_handle<> cont;
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<>) noexcept {
                    if (!cont) return std::noop_coroutine();
                    return cont;
                }
                void await_resume() noexcept {}
            };
            return Awaiter{continuation_};
        }

        void return_value(T value) {
            result_.template emplace<1>(std::move(value));
        }

        void unhandled_exception() {
            result_.template emplace<2>(std::current_exception());
        }
    };

    // Awaiter: when someone co_awaits this Task
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) {
        handle_.promise().continuation_ = caller;
        return handle_;  // symmetric transfer: resume the task
    }

    T await_resume() {
        auto& result = handle_.promise().result_;
        if (result.index() == 2)
            std::rethrow_exception(std::get<2>(result));
        return std::move(std::get<1>(result));
    }

    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
    ~Task() { if (handle_) handle_.destroy(); }

    Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}
    Task& operator=(Task&&) = delete;

private:
    std::coroutine_handle<promise_type> handle_;
};
```

Key design decisions in this type:

- **`initial_suspend` returns `suspend_always`.** The task is lazy — it does not start until awaited. This is critical for structured concurrency: the caller controls when work begins, so lifetime is bounded by the awaiting scope.
- **`final_suspend` resumes the continuation via symmetric transfer.** This avoids stack overflow when long coroutine chains complete. Without symmetric transfer, each resumption adds a frame to the stack.
- **Exceptions are captured, not lost.** `unhandled_exception()` stores the exception pointer, and `await_resume()` rethrows it. This preserves the failure boundary: the awaiter sees the error at the `co_await` expression, not in an unrelated context.
- **Move-only.** A task represents unique ownership of a coroutine frame. Copying it would create a double-destroy hazard identical to copying a `unique_ptr`.

### 8.3.2 Coroutine Lifetime Is an Ownership Problem

A coroutine frame is a heap allocation (unless the compiler elides it) that outlives the function that created it. It captures parameters, locals, and the suspension point. This makes it a resource, and Chapter 1's ownership rules apply directly.

```cpp
// Anti-pattern: dangling reference inside a coroutine
Task<std::string> format_greeting(const std::string& name) {
    co_await some_async_work();
    co_return "Hello, " + name;  // BUG: 'name' may be dangling after suspension
}

void caller() {
    std::string n = "Alice";
    auto task = format_greeting(n);
    n = "Bob";          // mutates while coroutine holds a reference
    sync_wait(task);    // coroutine resumes, reads modified or destroyed 'name'
}
```

The fix is the same as for any lifetime problem: the coroutine must own its data or the caller must guarantee the data outlives the coroutine.

```cpp
// Fixed: take by value so the coroutine frame owns the copy
Task<std::string> format_greeting(std::string name) {
    co_await some_async_work();
    co_return "Hello, " + name;  // safe: name is part of the coroutine frame
}
```

This is not a style preference. It is a correctness requirement. Every parameter to a coroutine that survives a suspension point must either be owned by value inside the frame or be provably stable for the coroutine's entire lifetime. References to stack-local variables are almost never safe.

### 8.3.3 Executors and Thread Pools

A coroutine that suspends on I/O must resume somewhere. "Somewhere" is the executor — the policy that determines which thread runs the continuation. Without an explicit executor, resumption happens on whatever thread completes the I/O operation, which may be an I/O completion thread, a timer thread, or an unrelated worker.

A minimal thread pool executor:

```cpp
class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads) {
        for (std::size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this] { run(); });
    }

    ~ThreadPool() {
        {
            std::scoped_lock lk(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) w.join();
    }

    void schedule(std::coroutine_handle<> h) {
        {
            std::scoped_lock lk(mutex_);
            queue_.push_back(h);
        }
        cv_.notify_one();
    }

    // Awaitable that reschedules the current coroutine onto this pool
    auto schedule() {
        struct Awaiter {
            ThreadPool* pool;
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                pool->schedule(h);
            }
            void await_resume() noexcept {}
        };
        return Awaiter{this};
    }

private:
    void run() {
        while (true) {
            std::coroutine_handle<> task;
            {
                std::unique_lock lk(mutex_);
                cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = queue_.front();
                queue_.pop_front();
            }
            task.resume();
        }
    }

    std::vector<std::jthread> workers_;
    std::deque<std::coroutine_handle<>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};
```

Usage in a coroutine:

```cpp
Task<Response> handle_request(ThreadPool& pool, Request req) {
    co_await pool.schedule();  // ensure we run on the pool, not the acceptor thread

    auto db = co_await query_database(req.key());
    auto svc = co_await call_service(req.payload());
    co_return merge(db, svc);
}
```

The executor decision matters for three reasons:

1. **Thread safety.** If the coroutine resumes on an unexpected thread, any unsynchronized access to shared state becomes a data race.
2. **Priority and fairness.** Different work may need different scheduling: latency-sensitive requests on a small dedicated pool, background batch work on a separate pool with lower priority.
3. **Shutdown.** The pool must drain or cancel outstanding coroutines before destruction. If a coroutine holds a reference to the pool's scheduler, and the pool is destroyed while coroutines are still queued, the handles become dangling.

### 8.3.4 Structured Concurrency: When All and When Any

Sequential `co_await` is insufficient for the production problem described in Section 1. We need to run the database query and the service call concurrently. This requires a `when_all` combinator.

```cpp
template <typename... Tasks>
Task<std::tuple<typename Tasks::value_type...>>
when_all(ThreadPool& pool, Tasks... tasks) {
    // Each task is started concurrently on the pool.
    // A shared atomic counter tracks completion.
    // The awaiting coroutine is resumed when all tasks finish.
    // (Implementation elided for brevity — the critical design
    //  point is that the parent coroutine owns all child tasks,
    //  and none outlive the co_await expression.)
}
```

The ownership contract: the parent coroutine starts child tasks, and child tasks complete (or are cancelled) before the parent resumes. This is structured concurrency. It means:

- No child task outlives its parent scope.
- If a child throws, the parent can cancel siblings and propagate the error.
- Resource cleanup follows the same scoping rules as synchronous code.

`when_any` is harder because it introduces cancellation: when the first child completes, the others must be told to stop.

## 8.4 Cooperative Cancellation

### 8.4.1 The Standard Mechanism: `std::stop_token`

C++20 introduced `std::stop_source`, `std::stop_token`, and `std::stop_callback`. These form a cooperative cancellation protocol: the requester signals a stop, and the worker checks or registers a callback.

```cpp
Task<Response> handle_request(std::stop_token stop, Request req) {
    auto db = co_await query_database(req.key(), stop);

    if (stop.stop_requested())
        co_return Response::cancelled();

    auto svc = co_await call_service(req.payload(), stop);

    co_return merge(db, svc);
}
```

Cooperative cancellation means the callee decides when and how to stop. This is not a deficiency — it is a requirement. Forceful thread termination (`TerminateThread`, `pthread_cancel`) corrupts invariants, leaks locks, and leaves resources in unrecoverable states. Cooperative cancellation preserves the callee's ability to clean up.

### 8.4.2 Integrating Stop Tokens with I/O

Passing `stop_token` through the call chain is necessary but not sufficient. The token must actually interrupt blocking operations. For network I/O, this typically means registering a `stop_callback` that closes or cancels the underlying socket or file descriptor.

```cpp
Task<std::string> read_with_timeout(Socket& sock, std::stop_token stop) {
    // Register a callback that cancels the socket's pending I/O
    std::stop_callback on_cancel(stop, [&sock] {
        sock.cancel();  // platform-specific: epoll_ctl remove, CancelIoEx, etc.
    });

    co_return co_await sock.async_read();
    // If stop was requested, async_read returns an error (e.g., operation_aborted).
    // The coroutine can then return a cancellation result or rethrow.
}
```

The `stop_callback` destructor deregisters the callback, so if the read completes normally before cancellation, no spurious cancel fires. This is a clean fit with RAII.

### 8.4.3 Request Timeouts as Cancellation

A 200 ms deadline is a cancellation policy: if the operation has not completed in time, stop it. This maps directly onto `stop_source`:

```cpp
Task<Response> handle_with_deadline(ThreadPool& pool, Request req) {
    std::stop_source stop_src;
    auto stop = stop_src.get_token();

    // Start a timer that requests stop after 200 ms
    auto timer = schedule_timer(pool, 200ms, [&stop_src] {
        stop_src.request_stop();
    });

    auto result = co_await handle_request(stop, std::move(req));

    // If we got here before the timer, cancel the timer
    timer.cancel();

    co_return result;
}
```

This pattern unifies timeout, caller-initiated cancel, and shutdown under a single mechanism. The `stop_token` does not care why a stop was requested.

### 8.4.4 Shutdown Coordination

Server shutdown is cancellation at the widest scope. A production shutdown sequence:

1. Stop accepting new requests.
2. Signal all in-flight requests to cancel (via a root `stop_source`).
3. Wait for in-flight work to drain, with a hard deadline.
4. Destroy the thread pool (which joins worker threads).
5. Destroy shared resources (connection pools, caches).

```cpp
class Server {
    std::stop_source shutdown_;
    ThreadPool pool_{std::thread::hardware_concurrency()};
    ConnectionPool db_pool_;

public:
    void run() {
        while (!shutdown_.stop_requested()) {
            auto req = accept_request(shutdown_.get_token());
            if (!req) break;  // accept was cancelled
            // Intentional partial: `spawn` schedules the task on the pool and
            // associates it with shutdown tracking so cancellation can drain it.
            spawn(pool_, handle_with_deadline(pool_, std::move(*req)),
                  shutdown_.get_token());
        }
        // In-flight tasks see stop_requested() and wind down.
        // ThreadPool destructor joins workers after queue drains.
    }

    void shutdown() { shutdown_.request_stop(); }
};
```

The ordering matters. If the thread pool is destroyed before in-flight coroutines complete, coroutine handles that reference the pool's scheduler dangle. If the connection pool is destroyed before coroutines release their connections, destructors race. This is Chapter 1's destruction-order problem, amplified by asynchrony.

## 8.5 Tradeoffs and Boundaries

### 8.5.1 Coroutine Frame Allocation

Every coroutine frame is (by default) heap-allocated. The compiler may elide the allocation when it can prove the frame's lifetime is bounded by the caller — but this is an optimization, not a guarantee. For latency-sensitive paths with many small coroutines, frame allocation can dominate cost. Mitigation strategies:

- **Custom `operator new` on the promise type** to use a pool allocator or arena.
- **`std::pmr::memory_resource`-backed allocation** threaded through the coroutine's first parameter.
- **Reducing coroutine depth** by inlining short awaitables.

Profile before optimizing. The allocation is typically a few hundred bytes and may be served from a thread-local cache.

### 8.5.2 Debugging Suspended Coroutines

Coroutine stacks do not appear in conventional backtraces. When a coroutine is suspended, its frame exists on the heap, not the stack. This means:

- **Crash dumps show the resumption site, not the logical call chain.** A coroutine that crashes after resumption will show a backtrace through the executor's `resume()` call, not the business logic that initiated the operation.
- **GDB and LLDB support for coroutine frames is improving but incomplete.** As of GCC 14 and Clang 18, you can inspect coroutine promise objects manually, but tooling for walking coroutine continuation chains is not yet integrated into standard debugger workflows.
- **Logging must compensate.** Instrument suspension and resumption points with request IDs or correlation tokens. In production, this structured logging is often more useful than any debugger.

### 8.5.3 Coroutines vs. State Machines

Not every asynchronous workflow benefits from coroutines. A protocol parser with three states and well-defined transitions is often clearer as an explicit state machine (an enum and a switch). Coroutines add value when the workflow has many suspension points, non-trivial control flow (loops, branches, error handling), and when the linear reading of the code matters for review. If the coroutine body is just two `co_await` calls with no branching, the machinery may not pay for itself.

### 8.5.4 C++26 Note: `std::execution` (P2300)

The `std::execution` framework introduces senders and receivers as a standard model for composing asynchronous work. Key differences from hand-rolled coroutine task types:

- **Senders are lazy descriptions of work**, not running operations. They can be composed, adapted, and scheduled before any execution begins.
- **`execution::when_all`** provides standard structured concurrency with cancellation propagation.
- **`execution::on` and `execution::transfer`** make executor transitions explicit in the type system.
- **Cancellation propagates through the sender chain** via stop tokens wired into the receiver contract.

Where available, `std::execution` removes the need for custom task types, `when_all` implementations, and manual stop-token threading. It does not remove the need to understand the ownership and cancellation semantics described in this chapter — it standardizes them. Until compiler support is production-ready, the patterns in this chapter remain necessary.

## 8.6 Testing and Tooling Implications

### 8.6.1 Testing Coroutine-Based Code

Coroutines split control flow across time, which makes deterministic testing harder. Strategies:

**Use a manual executor for tests.** Instead of a thread pool, use a single-threaded executor that advances work explicitly. This makes suspension and resumption points deterministic.

```cpp
class ManualExecutor {
    std::deque<std::coroutine_handle<>> queue_;
public:
    void schedule(std::coroutine_handle<> h) { queue_.push_back(h); }

    bool run_one() {
        if (queue_.empty()) return false;
        auto h = queue_.front();
        queue_.pop_front();
        h.resume();
        return true;
    }

    void drain() { while (run_one()) {} }
};
```

This lets tests control the interleaving: run one task to its first suspension, then run another, then check invariants. It is the coroutine equivalent of a mock clock.

**Test cancellation paths explicitly.** Request cancellation before, during, and after each suspension point. Verify that resources are released, that the correct error is propagated, and that no work continues after cancellation.

**Test shutdown ordering.** Destroy the executor (or server) while tasks are in flight. Run under AddressSanitizer and ThreadSanitizer. Use-after-free on coroutine frames and data races on shared cancellation state are the most common bugs.

### 8.6.2 Sanitizer Considerations

- **AddressSanitizer (ASan)** detects use-after-free on coroutine frames. If a coroutine is destroyed while another coroutine holds a pointer into its frame, ASan will catch the access. This is the most reliable tool for coroutine lifetime bugs.
- **ThreadSanitizer (TSan)** detects data races in coroutine resumption. A common source: two threads racing to resume the same coroutine handle, or a coroutine accessing shared state without synchronization after being resumed on an unexpected thread.
- **UndefinedBehaviorSanitizer (UBSan)** catches the usual suspects — null dereference, signed overflow — but has no special coroutine awareness.

Run all three in CI. Coroutine bugs are among the hardest to reproduce without sanitizers because they depend on scheduling order, which varies between runs.

### 8.6.3 Compiler Warnings and Static Analysis

Enable `-Wcoroutine-missing-unhandled-exception` (Clang) to catch promise types that do not handle exceptions. A missing `unhandled_exception()` means an exception inside a coroutine is undefined behavior — silently, with no diagnostic at runtime.

Static analysis tools (clang-tidy, PVS-Studio) are beginning to flag dangling references in coroutine parameters, but coverage is not yet comprehensive. Defensive coding — taking parameters by value — remains more reliable than static analysis for this class of bug.

## 8.7 Review Checklist

Use this checklist during code review of coroutine-based or asynchronous code.

- [ ] **Coroutine parameter safety.** Every parameter that survives a suspension point is taken by value, or its lifetime is explicitly guaranteed by the caller.
- [ ] **Task ownership is clear.** Each `Task` object is owned by exactly one scope. No raw `coroutine_handle` is used outside of the task infrastructure itself.
- [ ] **No fire-and-forget.** Every spawned task is awaited, joined, or tracked by a scope that will cancel it on destruction. Detached tasks are treated as a code smell requiring explicit justification.
- [ ] **Cancellation is cooperative and threaded through.** `std::stop_token` (or equivalent) is passed to every layer that performs blocking work. Blocking operations register `stop_callback` to interrupt I/O.
- [ ] **Timeouts exist.** Every external call (network, database, downstream service) has a deadline, not just a hope.
- [ ] **Shutdown order is documented.** The sequence in which the server stops accepting work, cancels in-flight tasks, drains the pool, and destroys shared resources is explicit and tested.
- [ ] **Executor is explicit.** The code makes clear which thread or pool will resume each coroutine after suspension. Implicit resumption on I/O threads is flagged.
- [ ] **Exceptions are not lost.** The promise type implements `unhandled_exception()`. The awaiter rethrows stored exceptions. No `co_await` discards its result without checking for error.
- [ ] **Sanitizers pass.** ASan and TSan run against the coroutine code paths in CI, including cancellation and shutdown tests.
- [ ] **Debugging affordances exist.** Correlation IDs or request tokens are logged at suspension and resumption points. The team can trace a request's async lifecycle from logs alone, without relying on stack traces.
