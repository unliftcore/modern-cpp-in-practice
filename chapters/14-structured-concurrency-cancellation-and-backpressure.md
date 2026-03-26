# Structured Concurrency, Cancellation, and Backpressure

This chapter assumes you already understand local coroutine lifetime and suspension hazards. The focus now is system shape: how groups of tasks begin, end, fail, and apply pressure to each other under real load.

## The Production Problem

Many asynchronous systems fail even when each individual task looks reasonable.

A request fans out to four backends and returns once three respond, but the fourth keeps running after the client disconnects. A worker pipeline accepts input faster than downstream storage can commit it, so memory usage climbs until the process is killed. A shutdown path waits forever because background tasks were detached instead of being part of a supervised tree. A retry storm consumes the very capacity needed for recovery. None of these is primarily a local coroutine bug. They are orchestration bugs.

Structured concurrency is the discipline of making concurrent work follow lexical and ownership structure. Tasks belong to parents. Lifetimes are bounded. Failure propagates somewhere definite. Cancellation is not advisory folklore. Backpressure is part of admission policy, not a dashboard surprise after rollout.

This chapter is about those system-level rules. Chapter 12 dealt with shared mutable state. Chapter 13 dealt with what one coroutine owns across suspension. Here the unit of reasoning is a set of tasks that together implement a request path, stream processor, or bounded service stage.

## Unstructured Work Scales Failure, Not Just Throughput

The simplest way to start concurrent work is to launch tasks wherever needed and hope completion sorts itself out. That style is attractive because it minimizes immediate coordination. It is also how systems accumulate invisible work.

Detached tasks, ad hoc thread pools, and fire-and-forget retries have three predictable consequences:

1. Lifetime becomes non-local. The code that started work is no longer responsible for proving when it ends.
2. Failure becomes observational. Errors surface only if someone remembered to log or poll them.
3. Capacity becomes fictional. The system keeps accepting work because no parent scope owns admission pressure.

A service can survive this for months if traffic is light and shutdown is rare. Under burst load, deployment churn, or slow downstream dependencies, the hidden work becomes the system.

## Fire-and-Forget: A Catalog of Failures

Before contrasting with structured concurrency, it is worth seeing exactly how unstructured work fails. "Fire-and-forget" is not one anti-pattern; it is several, each with a distinct failure mode.

### Resource leaks from ownerless work

```cpp
// Anti-pattern: detached task leaks a database connection on cancellation.
void on_request(request req) {
    std::jthread([req = std::move(req)] {
        auto conn = db_pool.acquire();        // acquired, never returned on some paths
        auto result = conn.execute(req.query);
        send_response(req.client, result);
    }).detach(); // no owner, no cancellation, no cleanup guarantee
}
```

If the process begins shutting down, detached threads do not receive stop requests. The database connection is not returned to the pool. Multiply this by thousands of in-flight requests during a rolling deployment: the database sees connection exhaustion, and the old process hangs in `std::thread` destructor calls or, worse, exits while threads still reference destroyed globals.

### Unobserved exceptions vanish silently

```cpp
// Anti-pattern: exception in detached task is never observed.
void start_background_sync() {
    auto handle = std::async(std::launch::async, [] {
        auto data = fetch_remote_config(); // throws on network error
        apply_config(data);
    });
    // handle is destroyed here — std::async's destructor blocks,
    // but if this were a custom fire-and-forget task, the exception
    // would be silently swallowed.
}
```

With `std::async`, the destructor blocks (which may be its own surprise). But with most custom task types that support detach, destroying the handle without observing the result means exceptions evaporate. The system continues with stale configuration, and the failure appears only as a mysterious behavioral regression hours later.

### Shutdown hangs from orphaned work

```cpp
// Anti-pattern: shutdown cannot complete because background tasks were never tracked.
class ingestion_service {
    void ingest(message msg) {
        // "just kick off enrichment in the background"
        pool_.submit([msg = std::move(msg), this] {
            auto enriched = enrich(msg);       // calls external service, may block
            store_.write(enriched);
        });
    }

    void shutdown() {
        store_.close();    // closes storage
        pool_.shutdown();  // waits for in-flight tasks
        // BUG: in-flight tasks may call store_.write() after store_ is closed
        // BUG: enrich() may block indefinitely — pool shutdown hangs
    }
};
```

The pool has tasks, but the service has no model of what those tasks need or how to cancel them. Shutdown either hangs (waiting for a blocked external call) or races (closing dependencies while tasks still use them). In production, this turns a clean restart into a process kill, which turns into data loss.

### The structured alternative in brief

The structured answer to all three problems is the same principle: the scope that creates work owns its completion.

```cpp
// Structured: parent scope owns child tasks, propagates cancellation, awaits completion.
task<void> on_request(request req, std::stop_token stop) {
    auto conn = co_await db_pool.acquire(stop);  // respects cancellation
    auto result = co_await conn.execute(req.query, stop);
    co_await send_response(req.client, result);
    // conn returned to pool when coroutine frame is destroyed
    // if stop is triggered, co_await points observe it and unwind cleanly
}
```

The parent request scope can cancel the token on client disconnect or deadline. The coroutine's awaitables check the token at each suspension point. Resources are released through normal RAII. No work outlives its owner. The contrast with fire-and-forget is not style; it is the difference between a system that can shut down and one that cannot.

## Structured Concurrency Means Parent Scopes Own Child Work

The central idea is simple: if a scope starts child tasks to complete its job, those children should finish, fail, or be canceled before the scope is considered done.

That gives you three properties that ad hoc async code rarely has by default:

1. The lifetime of work is bounded by a parent operation.
2. Failures can be aggregated or escalated in one place.
3. Cancellation and shutdown can follow a tree instead of searching the process for loose ends.

This does not require one specific library. It requires design discipline. A request handler that fans out to multiple backends should not return while those backend calls continue running unless the business contract explicitly permits detached follow-up work and names its owner. A batch consumer should not enqueue downstream tasks without also deciding who drains them on shutdown and who absorbs overload.

Structured concurrency is therefore an ownership rule for time. If Chapter 1 taught that every resource needs an owner, this chapter applies the same principle to concurrent work.

## Cancellation Must Be a First-Class Contract

Cancellation is often described as a courtesy. In production it is load control.

Once a client disconnects, a deadline expires, or a parent task fails, continuing child work may waste CPU, memory, database capacity, and retry budget. Worse, uncanceled work competes with useful work. Systems under pressure often fail because they keep doing tasks that no longer matter.

Modern C++ gives useful building blocks such as `std::stop_source`, `std::stop_token`, and `std::jthread`. The example project's `Server::run(std::stop_token)` (`examples/web-api/src/modules/http.cppm`) uses exactly these building blocks: the accept loop checks `stop_token.stop_requested()` on every iteration, and `select()` with a one-second timeout ensures the check happens promptly even when no clients are connecting. This is cooperative cancellation in a real server — the token is the contract, and the timeout-based polling is the mechanism.

But the primitives alone are not enough. The harder question is semantic:

1. Which operations are cancelable?
2. At what boundaries is cancellation observed?
3. What cleanup is guaranteed before completion is reported?
4. Is partial progress committed, rolled back, or made visible with compensation?

If those questions are unanswered, wiring a stop token through a few functions is theater.

Cancellation also needs direction. Parent-to-child propagation should be the default. Child-to-parent escalation depends on policy: one child failure may cancel siblings, may degrade the result, or may be recorded while work continues. The point is that the rule must be explicit at the scope that owns the group.

## Anti-pattern: Fan-Out Without Bounded Ownership

```cpp
// Anti-pattern: child work outlives the request and overload has no admission limit.
task<aggregate_reply> handle_request(request req) {
    auto a = fetch_profile(req.user_id);
    auto b = fetch_inventory(req.item_id);
    auto c = fetch_pricing(req.item_id);

    co_return aggregate_reply{
        co_await a,
        co_await b,
        co_await c,
    };
}
```

This code is tidy and underspecified.

What cancels the three child operations if the client times out after the first await? What prevents ten thousand concurrent requests from starting thirty thousand backend calls immediately? If `fetch_inventory` hangs, do the others keep running? If one call fails fast, should the rest be canceled or allowed to complete because partial results are useful?

The problem is not that fan-out is bad. It is that the code does not show a supervision policy.

In a structured design, the request scope owns a cancellation source or token, child tasks are started within that scope, deadlines are attached, and concurrency against downstream dependencies is bounded by permits or semaphores. The exact abstraction varies by codebase. The essential property is that the request does not create anonymous work.

## Deadlines and Budgets Beat Best-Effort Timeouts

Timeouts are often implemented locally and inconsistently. One dependency has a 200 ms timeout, another has 500 ms, and the caller has a 300 ms deadline that nobody propagates. The result is wasted work and confusing telemetry.

A better model is budget propagation. A parent operation carries a deadline or remaining budget. Child operations derive their own limits from that budget instead of inventing unrelated ones. This keeps cancellation and latency intent aligned.

The tradeoff is that downstream APIs must accept deadline or cancellation context explicitly, and timeout behavior becomes visible in signatures or task builders. That is a good cost. Hidden timeout policy is usually worse than noisy timeout policy.

## Backpressure Is Admission Control, Not Complaint Logging

Backpressure means the system has a deliberate answer to "what happens when work arrives faster than we can finish it?"

Without that answer, work piles up in queues, buffers, retry loops, and coroutine frames. Memory climbs first, latency second, and only then does the outage become obvious. An unbounded queue is not elasticity. It is a promise to convert overload into delayed failure.

Real backpressure mechanisms are concrete:

1. Bounded queues that reject or defer new work.
2. Semaphores or permits that limit concurrent access to scarce dependencies.
3. Producer throttling when downstream stages are saturated.
4. Load shedding when serving all traffic would destroy latency for all traffic.
5. Batch sizing and flush policy that match downstream commit cost.

Each mechanism encodes business policy. Which work can be dropped? Which must wait? Which clients receive an explicit overload signal? Those are product decisions expressed as concurrency control.

## Bounded Concurrency Is Usually Better Than Bigger Pools

When a dependency slows down, many teams first increase pool sizes or queue depths. That often amplifies the problem.

If a database can sustain fifty useful concurrent requests, allowing two hundred in-flight operations mostly increases contention and timeout overlap. The same applies to CPU-heavy parsing stages, compression work, and remote service calls with their own internal bottlenecks.

Bound concurrency where the scarce resource actually is. Make that bound visible in code and telemetry. Then decide what should happen when the bound is reached: wait, fail fast, degrade, or redirect. Bigger pools without policy only hide overload until the whole system is saturated.

## Pipelines Need Pressure to Travel Upstream

Pipelines are where backpressure discipline becomes unavoidable.

Consider a message consumer that parses records, enriches them with remote lookups, and writes batches to storage. If parsing outruns storage, some stage must slow down. If enrichment outruns parsing, the enrichment stage should not keep creating more in-flight requests just because it can. If shutdown begins, all stages need a coordinated drain or cancel policy.

Good pipeline design therefore names:

1. The maximum in-flight work per stage.
2. The maximum queue depth between stages.
3. Whether a full queue blocks producers, drops input, or triggers load shedding.
4. Whether cancellation drains partially completed batches or discards them.
5. Which metrics reveal saturation before memory pressure becomes critical.

This is not optional infrastructure polish. It is the difference between a system that degrades and one that accumulates work until it dies.

## Failure Propagation Needs Policy, Not Hope

Once work is structured into groups, failure handling becomes a design choice instead of an accident.

Common policies include:

1. Fail-fast groups, where one child failure cancels siblings because the result is useless without all parts.
2. Best-effort groups, where some child failures are tolerated and recorded.
3. Quorum groups, where enough successful children satisfy the operation and the rest are canceled.
4. Supervisory loops, where failures restart isolated child work under rate limits and budgets.

All four are valid in the right domain. What matters is that the code and abstraction make the policy apparent. Silent continuation after child failure is not resilience. It is ambiguity.

## Shutdown Is the Truth Serum

Systems with weak concurrency structure often look fine until shutdown.

A clean shutdown path forces all the hidden questions into the open. Which tasks are still running? Which can be interrupted safely? Which queues must drain? Which side effects may be committed after shutdown begins? Which background loops own the stop source, and who awaits their completion?

This is why shutdown tests are disproportionately valuable. They expose detached work, missing cancellation points, unbounded queues, and tasks with no owner. If a subsystem cannot describe how it stops under load, it does not fully own its concurrency model.

The example project implements a complete structured shutdown chain that puts these principles into practice. The flow spans three files:

**Signal handler sets the flag** (`examples/web-api/src/main.cpp`):

```cpp
std::atomic<bool> shutdown_requested{false};

extern "C" void signal_handler(int /*sig*/) {
    shutdown_requested.store(true, std::memory_order_release);
}
```

**`Server::run_until` bridges the flag to a `jthread` stop token** (`examples/web-api/src/modules/http.cppm`):

```cpp
void run_until(const std::atomic<bool>& should_stop) {
    std::jthread server_thread{[this](std::stop_token st) {
        run(st);
    }};
    while (!should_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server_thread.request_stop();
    // jthread auto-joins on destruction
}
```

**`Server::run` checks the token on every accept loop iteration**:

```cpp
void run(std::stop_token stop_token) {
    // ...
    while (!stop_token.stop_requested()) {
        // select() with 1-second timeout, then check stop again
        // accept and handle connection ...
    }
    std::println("Server shutting down gracefully");
}
```

The ownership chain is clear: `main` owns the `Server`, the `Server` owns the `jthread`, and the `jthread` owns the `stop_source`. When Ctrl+C arrives, the signal handler sets the atomic flag, `run_until` observes it and calls `request_stop()`, the accept loop exits on the next iteration, and the `jthread` destructor joins the thread. No detached work, no orphaned connections, no race between shutdown and in-flight requests. This is structured concurrency applied to a real service lifecycle.

## Verification and Telemetry for Structured Async Systems

Unit tests alone do not validate structured concurrency or backpressure. You need evidence that the system behaves sanely under stress.

Useful verification includes:

1. Load tests that drive the system past nominal capacity and confirm bounded memory.
2. Cancellation tests that inject disconnects, deadline expiry, and partial child failure.
3. Shutdown tests that start work, trigger stop, and verify prompt quiescence.
4. Metrics for in-flight tasks, queue depth, permit utilization, rejection rate, deadline expiry, and cancellation latency.
5. Traces or logs that show parent-child linkage so orphaned work is visible.

If observability cannot reveal where work is accumulating or which parent owns it, the structure exists only in the author's head.

## Review Questions for Structured Concurrency

Before approving an asynchronous orchestration design, ask:

1. Who owns each group of child tasks?
2. What event cancels them: parent completion, failure, deadline, shutdown, or overload?
3. What bounds concurrent work against each scarce dependency?
4. What happens when the queue or permit limit is reached?
5. Does failure cancel siblings, degrade gracefully, or wait for quorum?
6. Can shutdown complete promptly under peak load?
7. Which metrics prove that backpressure is working?

If the answers are unclear, the system is probably running on optimism and spare capacity.

## Takeaways

Structured concurrency is ownership applied to time and task trees.

Do not launch anonymous work. Make parent scopes own child lifetimes, propagate cancellation deliberately, and bound concurrency where resources are actually scarce. Treat backpressure as admission policy, not as a tuning afterthought. A system that cannot say when work stops, who cancels it, and how overload is limited does not yet have a concurrency model. It only has asynchronous code.
