# Chapter 10: Coroutines and Generators

*Prerequisites: Chapters 1–2 and 9. Task lifetime depends on ownership. Failure propagation and synchronization must already be understood.*

> **Prerequisites:** This chapter assumes you can already reason about resource ownership across scopes (Chapter 1), define failure boundaries that do not leak side effects (Chapter 2), and coordinate shared mutable state under contention (Chapter 9). Asynchronous programming amplifies every weakness in those foundations. If ownership is vague, a suspended coroutine will dangle. If failure policy is inconsistent, a cancelled task will swallow errors or propagate them into the wrong handler. If synchronization is ad hoc, resumption on the wrong thread will corrupt state that was safe in a single-threaded mental model.
>
> The code samples use C++23 as the baseline. `std::generator` (C++23) is covered in depth. `std::execution` (P2300) is deferred to Chapter 11.

## 10.1 The Production Problem

A service receives an RPC, queries a database, calls two downstream services in parallel, merges the results, and replies. The whole operation has a 200 ms deadline. Any of the downstream calls may hang. Shutdown may arrive mid-flight. The database connection belongs to a pool whose lifetime is scoped to the server, not the request.

This is not a concurrency problem in the Chapter 9 sense — there is no contention over shared counters or lock ordering. The difficulty is structural: work is split across time, each piece has a different lifetime, and cancellation must propagate without leaking resources or corrupting shared infrastructure. The synchronous call chain that reviewers can follow top-to-bottom no longer exists. Control flow is scattered across callbacks, continuations, or suspension points, and the "stack" at any given moment may not contain the frames that matter.

Teams that solve this casually end up with one of three failure modes:

1. **Leaked work.** A cancelled request's downstream calls complete after the reply is sent, writing results into freed memory or into a response object that no longer exists.
2. **Stuck shutdown.** The server's destructor joins a thread pool that is blocked on I/O that was never cancelled, producing a hang on process exit.
3. **Silent error loss.** An exception thrown inside a coroutine is stored in a future that nobody awaits, and the failure vanishes.

## 10.2 The Callback and Future Baseline

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

## 10.3 Coroutine-Based Task Model

C++20 coroutines provide the suspension mechanism. They do not provide a task type, an executor, a cancellation protocol, or a scheduler. Those are design decisions that the team must make or import from a library. This section builds the pieces incrementally.

### 10.3.1 A Minimal Awaitable Task

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

### 10.3.2 Coroutine Lifetime Is an Ownership Problem

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

### 10.3.3 Executors and Thread Pools

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

### 10.3.4 Structured Concurrency: When All and When Any

Sequential `co_await` is insufficient for the production problem described in Section 10.1. We need to run the database query and the service call concurrently. This requires a `when_all` combinator.

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

## 10.4 Cooperative Cancellation

### 10.4.1 The Standard Mechanism: `std::stop_token`

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

### 10.4.2 Integrating Stop Tokens with I/O

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

### 10.4.3 Request Timeouts as Cancellation

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

### 10.4.4 Shutdown Coordination

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

## 10.5 `std::generator` — Lazy Sequences (C++23)

C++23 introduces `std::generator<T>` (`<generator>` header), a synchronous coroutine-based range type. Where `Task<T>` models a single asynchronous result, `std::generator<T>` models a lazy sequence of values produced on demand. It is the coroutine counterpart to a Python generator or C# `IEnumerable<T>` with `yield return`.

### 10.5.1 The Basic Primitive

A `std::generator<T>` is a `std::ranges::input_range` whose elements are computed one at a time, each time the caller advances the iterator. The coroutine body uses `co_yield` to produce values and suspends between yields.

```cpp
#include <generator>
#include <ranges>
#include <print>

std::generator<int> iota_gen(int start, int end) {
    for (int i = start; i < end; ++i)
        co_yield i;
}

void example() {
    for (int v : iota_gen(0, 10))
        std::println("{}", v);
}
```

Key properties:

- **Lazy.** No value is computed until the caller pulls it. If the caller breaks out of the loop after three elements, only three elements are ever produced.
- **Single-pass.** `std::generator` models `input_range`, not `forward_range`. You cannot iterate it twice. This reflects the fundamental nature of a coroutine: it has one execution state, and advancing it is irreversible.
- **Synchronous.** There is no `co_await` inside a generator body (unless you are combining generators with async machinery outside the standard). Suspension happens only at `co_yield` points, and resumption is driven by iterator increment, not by an executor.

### 10.5.2 Generator-Based Data Streaming

Generators excel at producing data incrementally without materializing an entire collection in memory. This is particularly valuable for I/O-bound sequences and data transformation pipelines.

**Reading a file line by line:**

```cpp
#include <fstream>
#include <generator>
#include <string>

std::generator<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("Cannot open " + path.string());

    std::string line;
    while (std::getline(file, line))
        co_yield std::move(line);
    // file is closed when the generator frame is destroyed
}
```

This generator holds one line in memory at a time, regardless of file size. The `std::ifstream` lives inside the coroutine frame and is destroyed when the generator is destroyed — standard RAII, applied to a coroutine.

**Parsing a binary protocol:**

```cpp
struct Packet {
    std::uint32_t id;
    std::vector<std::byte> payload;
};

std::generator<Packet> parse_packets(std::span<const std::byte> buffer) {
    std::size_t offset = 0;
    while (offset + sizeof(PacketHeader) <= buffer.size()) {
        auto header = read_header(buffer.subspan(offset));
        if (offset + header.total_size > buffer.size())
            break;  // incomplete packet at end of buffer

        co_yield Packet{
            .id = header.id,
            .payload = {buffer.begin() + offset + sizeof(PacketHeader),
                        buffer.begin() + offset + header.total_size}
        };
        offset += header.total_size;
    }
}
```

The caller drives the parse at its own pace. If it only needs the first packet matching some criterion, it stops iterating and no further parsing occurs.

**Producing test data:**

```cpp
std::generator<Request> synthetic_requests(std::size_t count) {
    std::mt19937 rng(42);  // deterministic seed for reproducibility
    std::uniform_int_distribution<int> key_dist(1, 10'000);

    for (std::size_t i = 0; i < count; ++i) {
        co_yield Request{
            .id = i,
            .key = key_dist(rng),
            .timestamp = Clock::now()
        };
    }
}
```

Unlike building a `std::vector<Request>` of a million entries up front, this allocates one `Request` at a time. For load tests and fuzzing, this difference can be the line between fitting in memory and not.

### 10.5.3 Generators as Range Sources

Because `std::generator<T>` satisfies `std::ranges::input_range`, it composes directly with `std::ranges` algorithms and view adaptors.

```cpp
#include <algorithm>
#include <generator>
#include <ranges>
#include <vector>

auto active_user_ids(Database& db) -> std::generator<UserId> {
    for (auto& row : db.query("SELECT id, status FROM users"))
        if (row["status"] == "active")
            co_yield UserId{row["id"]};
}

void process_batch(Database& db) {
    // Compose the generator with range adaptors — still lazy
    auto first_100 = active_user_ids(db)
        | std::views::transform([](UserId id) { return enrich(id); })
        | std::views::take(100);

    // Materialize only when needed
    std::vector<EnrichedUser> batch(std::ranges::begin(first_100),
                                     std::ranges::end(first_100));
    // Only 100 rows were fetched and enriched, regardless of table size
}
```

The pipeline `active_user_ids(db) | transform | take(100)` remains lazy end to end. The database query produces rows, the generator filters and yields IDs, `transform` enriches each one, and `take` stops after 100. No intermediate container is allocated.

Constraints to be aware of:

- **Input range only.** View adaptors that require `forward_range` (such as `std::views::split` in some implementations, or `std::views::slide`) will not compile with a generator. Design around single-pass consumption.
- **Move semantics on yielded values.** `std::generator<T>` yields by reference internally. The caller receives a reference to the value stored in the coroutine frame. If you need to store values past the next iteration, copy or move them out explicitly.

### 10.5.4 Recursive Generators

A common need is to flatten a tree structure into a linear sequence. Without language support, this requires an explicit stack. With `std::generator`, you can use `co_yield std::ranges::elements_of(sub_generator)` to delegate to a child generator, producing all of its elements as if they were yielded directly.

```cpp
#include <generator>
#include <ranges>

struct TreeNode {
    int value;
    std::vector<TreeNode> children;
};

std::generator<int> flatten(const TreeNode& node) {
    co_yield node.value;
    for (const auto& child : node.children)
        co_yield std::ranges::elements_of(flatten(child));
}
```

`std::ranges::elements_of` is the signal to the generator machinery that the inner generator should be "inlined" — its values are yielded directly through the outer generator without creating a deeply nested call chain. This is implemented via symmetric transfer between generator frames, so the stack depth remains constant regardless of tree depth.

Without `elements_of`, the naive approach would be:

```cpp
// Correct but less efficient — creates nested generator frames on the heap
std::generator<int> flatten_naive(const TreeNode& node) {
    co_yield node.value;
    for (const auto& child : node.children)
        for (int v : flatten_naive(child))
            co_yield v;
}
```

This version works, but each recursive call creates a new generator frame and each value must be relayed through every level of the recursion. `elements_of` avoids the relay cost by transferring control directly to the innermost active generator.

### 10.5.5 Memory Efficiency: Generator Frames vs. Materializing Containers

The core tradeoff: a generator pays for one coroutine frame allocation (typically a few hundred bytes, containing locals and the suspension state) and produces values one at a time. A container pays for the storage of all values simultaneously.

| Approach | Memory | Allocation pattern | Reuse |
|----------|--------|--------------------|-------|
| `std::vector<T>` of N elements | O(N) | One large allocation (amortized) | Random access, multi-pass |
| `std::generator<T>` yielding N elements | O(1) amortized | One frame (small, fixed) | Single-pass only |

When to prefer a generator:

- The sequence is large or unbounded (log streams, network data, combinatorial enumerations).
- The consumer typically does not need all elements (early exit, `take`, `find`).
- The producer has side effects tied to production order (file reads, database cursors).

When to prefer a container:

- Multiple passes are needed (sorting, binary search, random access).
- The data is small enough that materialization cost is negligible.
- The algorithm requires `forward_range` or stronger.

A generator does not eliminate allocation — it changes the allocation profile. The coroutine frame itself is heap-allocated (unless elided by the compiler). For very tight loops producing trivially cheap values, the per-element overhead of suspension and resumption may exceed the cost of just filling a vector. Profile before choosing.

### 10.5.6 Generator Lifetime and Dangling Hazards

The coroutine frame behind a `std::generator` is owned by the generator object. When the generator is destroyed, the frame is destroyed, and all locals within it — including RAII objects like file handles, locks, and allocated buffers — are cleaned up. This is well-defined and predictable.

The hazard comes from yielded references.

```cpp
// Dangerous: yielding references to frame-local data
std::generator<const std::string&> dangerous_refs() {
    std::string current;
    for (int i = 0; i < 10; ++i) {
        current = "item_" + std::to_string(i);
        co_yield current;  // yields a reference to 'current'
    }
}

void consumer() {
    std::vector<const std::string*> ptrs;
    for (const auto& s : dangerous_refs()) {
        ptrs.push_back(&s);  // BUG: pointer into coroutine frame
    }
    // All pointers in ptrs are now dangling — 'current' was overwritten
    // on each iteration, and the frame is destroyed after the loop
}
```

`std::generator<const std::string&>` yields a reference. The reference is valid only until the next iteration advances the generator (which may overwrite the local variable). Storing that reference or pointer past the next `++it` is undefined behavior.

Rules for safe generator usage:

1. **`std::generator<T>` (by value) is safest.** Each `co_yield` moves or copies the value to the caller. The caller owns the value independently of the generator frame.
2. **`std::generator<T&>` or `std::generator<const T&>` is an optimization.** It avoids the copy but requires the caller to consume or copy the value before advancing the iterator. This is the same contract as iterating over a `std::istream_iterator`.
3. **Never store references or pointers obtained from a generator past the next iteration.** Treat each yielded reference as a temporary.

The same lifetime discipline from Section 10.3.2 applies: if the generator yields data whose lifetime is tied to the frame, the consumer must not outlive that data. The compiler will not warn you. Code review and sanitizers are the safety net.

## 10.6 Tradeoffs and Boundaries

### 10.6.1 Coroutine Frame Allocation

Every coroutine frame is (by default) heap-allocated. The compiler may elide the allocation when it can prove the frame's lifetime is bounded by the caller — but this is an optimization, not a guarantee. For latency-sensitive paths with many small coroutines, frame allocation can dominate cost. Mitigation strategies:

- **Custom `operator new` on the promise type** to use a pool allocator or arena.
- **`std::pmr::memory_resource`-backed allocation** threaded through the coroutine's first parameter.
- **Reducing coroutine depth** by inlining short awaitables.

Profile before optimizing. The allocation is typically a few hundred bytes and may be served from a thread-local cache.

### 10.6.2 Debugging Suspended Coroutines

Coroutine stacks do not appear in conventional backtraces. When a coroutine is suspended, its frame exists on the heap, not the stack. This means:

- **Crash dumps show the resumption site, not the logical call chain.** A coroutine that crashes after resumption will show a backtrace through the executor's `resume()` call, not the business logic that initiated the operation.
- **GDB and LLDB support for coroutine frames is improving but incomplete.** As of GCC 14 and Clang 18, you can inspect coroutine promise objects manually, but tooling for walking coroutine continuation chains is not yet integrated into standard debugger workflows.
- **Logging must compensate.** Instrument suspension and resumption points with request IDs or correlation tokens. In production, this structured logging is often more useful than any debugger.

### 10.6.3 Coroutines vs. State Machines

Not every asynchronous workflow benefits from coroutines. A protocol parser with three states and well-defined transitions is often clearer as an explicit state machine (an enum and a switch). Coroutines add value when the workflow has many suspension points, non-trivial control flow (loops, branches, error handling), and when the linear reading of the code matters for review. If the coroutine body is just two `co_await` calls with no branching, the machinery may not pay for itself.

### 10.6.4 Generators vs. Iterators

Handwritten iterators remain appropriate when the iteration state is trivial (a pointer and an end pointer, or an index into an array). Generators shine when the iteration logic involves complex control flow — nested loops, recursion, conditional skipping, error handling — that would be painful to invert into an explicit iterator state machine. A generator that reads lines from a file, skips comments, parses fields, and yields records is far more readable than the equivalent hand-rolled iterator with the same logic split across `operator++`, `operator*`, and constructor.

## 10.7 Testing and Tooling Implications

### 10.7.1 Testing Coroutine-Based Code

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

### 10.7.2 Testing Generators

Generators are simpler to test than async tasks because they are synchronous and single-threaded. Strategies:

**Test early termination.** Verify that destroying a generator mid-iteration cleans up resources correctly. For a generator that opens a file, break out of the loop early and confirm the file handle is released (ASan will catch a leak if the frame is not destroyed).

```cpp
void test_early_exit() {
    {
        auto gen = read_lines("/tmp/test.txt");
        auto it = gen.begin();
        ++it;  // read one line
        // generator destroyed here — frame and file handle cleaned up
    }
    // ASan: no leak
}
```

**Test recursive generators for deep trees.** Ensure that `elements_of`-based recursion handles deep nesting without stack overflow. A tree of depth 10,000 should work because symmetric transfer keeps the stack flat.

**Test yielded value lifetimes.** If the generator yields references, write a test that deliberately copies the reference to a local and advances the iterator, then checks that the copied value (not the reference) is correct. This catches the class of bug where callers accidentally hold stale references.

### 10.7.3 Sanitizer Considerations

- **AddressSanitizer (ASan)** detects use-after-free on coroutine frames. If a coroutine is destroyed while another coroutine holds a pointer into its frame, ASan will catch the access. This is the most reliable tool for coroutine lifetime bugs. It also catches generator frame use-after-free when a consumer holds a reference past generator destruction.
- **ThreadSanitizer (TSan)** detects data races in coroutine resumption. A common source: two threads racing to resume the same coroutine handle, or a coroutine accessing shared state without synchronization after being resumed on an unexpected thread. Generators are typically single-threaded, so TSan findings on generator code usually indicate a design error (sharing a generator across threads).
- **UndefinedBehaviorSanitizer (UBSan)** catches the usual suspects — null dereference, signed overflow — but has no special coroutine awareness.

Run all three in CI. Coroutine bugs are among the hardest to reproduce without sanitizers because they depend on scheduling order, which varies between runs.

### 10.7.4 Compiler Warnings and Static Analysis

Enable `-Wcoroutine-missing-unhandled-exception` (Clang) to catch promise types that do not handle exceptions. A missing `unhandled_exception()` means an exception inside a coroutine is undefined behavior — silently, with no diagnostic at runtime.

Static analysis tools (clang-tidy, PVS-Studio) are beginning to flag dangling references in coroutine parameters, but coverage is not yet comprehensive. Defensive coding — taking parameters by value — remains more reliable than static analysis for this class of bug.

## 10.8 Review Checklist

Use this checklist during code review of coroutine-based, generator-based, or asynchronous code.

- [ ] **Coroutine parameter safety.** Every parameter that survives a suspension point is taken by value, or its lifetime is explicitly guaranteed by the caller.
- [ ] **Task ownership is clear.** Each `Task` object is owned by exactly one scope. No raw `coroutine_handle` is used outside of the task infrastructure itself.
- [ ] **No fire-and-forget.** Every spawned task is awaited, joined, or tracked by a scope that will cancel it on destruction. Detached tasks are treated as a code smell requiring explicit justification.
- [ ] **Cancellation is cooperative and threaded through.** `std::stop_token` (or equivalent) is passed to every layer that performs blocking work. Blocking operations register `stop_callback` to interrupt I/O.
- [ ] **Timeouts exist.** Every external call (network, database, downstream service) has a deadline, not just a hope.
- [ ] **Shutdown order is documented.** The sequence in which the server stops accepting work, cancels in-flight tasks, drains the pool, and destroys shared resources is explicit and tested.
- [ ] **Executor is explicit.** The code makes clear which thread or pool will resume each coroutine after suspension. Implicit resumption on I/O threads is flagged.
- [ ] **Exceptions are not lost.** The promise type implements `unhandled_exception()`. The awaiter rethrows stored exceptions. No `co_await` discards its result without checking for error.
- [ ] **Generator yielded-reference discipline.** If a generator yields by reference, callers copy or consume the value before advancing the iterator. No pointer or reference obtained from a generator outlives the next `++it`.
- [ ] **Generator early-exit safety.** Code that breaks out of a generator loop or destroys a generator mid-iteration does not leak resources. Verified under ASan.
- [ ] **Sanitizers pass.** ASan and TSan run against the coroutine and generator code paths in CI, including cancellation, shutdown, and early-exit tests.
- [ ] **Debugging affordances exist.** Correlation IDs or request tokens are logged at suspension and resumption points. The team can trace a request's async lifecycle from logs alone, without relying on stack traces.
