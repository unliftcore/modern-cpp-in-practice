# Building a Small Service in Modern C++

Small services are where many C++ teams either get disciplined or get hurt. The codebase is still small enough that people are tempted to improvise, but the process already has real failure modes: overload, half-configured startup, partial writes, dependency timeouts, queue growth, shutdown races, and production debugging with incomplete evidence. The language does not rescue you from that.

This chapter is not a framework tutorial. The production question is narrower: what shape should a small C++23 service have if you want ownership, failure handling, concurrency, and operations to remain reviewable six months later? The answer is not "use all the latest features." The answer is to choose a service shape that keeps lifetime obvious, async work owned, resource limits explicit, and diagnosis possible under pressure.

The sample system is a small configuration-backed service that accepts requests, validates them, performs bounded background work, persists state, and exposes metrics and health information. The details are ordinary on purpose. Most production services are not conceptually novel. They fail because basic engineering boundaries were left vague.

## Define the Unit of Ownership Before the Unit of Deployment

The first architectural mistake in small services is organizing around endpoints, handlers, or framework callbacks instead of around owned resources. A deployable service owns a fixed set of long-lived things: configuration, listeners, executors, connection pools, storage adapters, telemetry sinks, and shutdown coordination. If those are not represented explicitly, the code drifts toward globals, shared singletons, detached work, and shutdown by hope.

The service object should therefore model ownership directly. It should be the place where long-lived dependencies are constructed, started, and stopped. That does not mean one giant god object. It means one clear root that owns the parts whose lifetimes must end together.

### Intentional partial: a service root that owns time and shutdown

```cpp
struct service_components {
	config cfg;
	request_router router;
	storage_client storage;
	bounded_executor executor;
	telemetry telemetry;
	http_listener listener;
};

class service {
public:
	explicit service(service_components components)
		: components_(std::move(components)) {}

	auto run(std::stop_token stop) -> std::expected<void, service_error>;
	void request_stop() noexcept;

private:
	auto start() -> std::expected<void, service_error>;
	auto drain() noexcept -> void;

	service_components components_;
	std::atomic<bool> stopping_{false};
};
```

This is intentionally boring. The service has one ownership root, one stop path, and one place to reason about startup and drain order. A small service does not need architectural theater.

That root should usually own concrete infrastructure types, not a graph of heap-allocated interfaces stitched together with shared ownership. Dependency inversion still matters, but the inversion point is usually at boundaries such as storage, transport, or telemetry adapters. Within the process, static ownership is simpler and cheaper than a forest of `std::shared_ptr` objects whose real owners no longer exist on paper.

### Anti-pattern: shared_ptr soup for request state

A common failure mode is using `std::shared_ptr` to extend request lifetimes across callbacks, queues, and retries without an explicit ownership model. The code compiles and appears safe, but nobody can say when request resources actually release, whether cancellation reaches all holders, or whether shutdown can complete deterministically.

```cpp
// BAD: shared_ptr soup — every callback extends lifetime indefinitely
void handle_request(std::shared_ptr<http_request> req) {
    auto ctx = std::make_shared<request_context>(req->parse_body());
    ctx->db_future = db_.async_query(ctx->query, [ctx](auto result) {
        ctx->result = result;
        cache_.async_store(ctx->key, ctx->result, [ctx](auto status) {
            ctx->respond(status);  // when does ctx die? who knows
        });
    });
    // ctx is now kept alive by two lambdas, the future, and possibly
    // a retry timer. cancellation cannot reach it. shutdown cannot
    // drain it. memory profile is non-deterministic.
}
```

The fix is to extract an owned work item and move it through the pipeline with clear handoff points.

```cpp
// BETTER: owned work item with explicit lifetime boundaries
struct request_work {
    parsed_query query;
    std::stop_token stop;
    response_sink sink;  // move-only, writes exactly once
};

void handle_request(http_request& req, std::stop_token stop) {
    auto work = request_work{
        .query = req.parse_body(),
        .stop  = stop,
        .sink  = req.take_response_sink(),
    };
    executor_.submit(std::move(work));
    // work is now owned by the executor. cancellation reaches it
    // through stop_token. shutdown drains the executor.
}
```

The owned work item makes the design questions visible: what data survives the request boundary, who can cancel it, and where does it end up during shutdown.

## Startup Should Either Produce a Running Service or Fail Cleanly

Many service incidents start before the first request. Configuration is partially loaded. One subsystem is healthy, another is not. Threads start before health state exists. Background timers begin before dependencies are validated. The process reports "ready" because some constructor returned.

The correct startup question is not whether each individual component can be initialized. It is whether the process can reach a coherent running state. Startup should therefore be staged around dependency validation and explicit failure boundaries.

Useful startup order usually looks like this:

1. Load and validate immutable configuration.
2. Construct resource-owning adapters with explicit limits.
3. Verify downstream dependencies needed for readiness.
4. Start listeners and background work only after the process is coherent.
5. Publish readiness only after the previous steps succeed.

In C++23, `std::expected` is often a better fit than exceptions for this path because startup naturally accumulates infrastructure failures that need translation into stable operational categories. A service that fails because the config file is malformed, the port is unavailable, or the storage schema is incompatible should expose those as deliberate startup failures, not as whatever exception text happened to leak through an implementation detail.

The tradeoff is verbosity. `std::expected` asks you to write out translation points explicitly. That cost is usually worth paying in service startup, where hidden exception paths make process state harder to reason about. Inside leaf functions or internal helpers, exceptions may still be acceptable if the boundary that contains them is clear. What matters is that startup exposes one coherent contract to the top level.

## Request Handling Should Treat Borrowed Data as Short-Lived

Small services often fail by turning temporary request data into longer-lived internal state. Headers become `std::string_view` members in async jobs. Parsed payload views are retained in caches. Callbacks capture references to objects whose lifetime ended with the request. The service works until a slow path, retry path, or queue delay makes the mistake observable.

The rule is simple: borrowed views are excellent for synchronous inspection and terrible as implicit storage. Use `std::string_view`, `std::span`, and range views aggressively inside a request path when the lifetime is local and obvious. Convert to owning representations before the data crosses time, threads, queues, or retry boundaries.

That decision is one of the main reasons service code benefits from explicit request models. Parse and validate into a value type that owns what background work must retain. Keep that model small enough that copying it is a conscious cost, then move it into async work when the design requires time decoupling.

This is where many C++ service codebases overuse `std::shared_ptr<request_context>`. Shared ownership looks like a convenient escape hatch for async lifetimes, but it often hides the actual design choice: which parts of the request need to survive, who owns them, and when they may be discarded. In a small service, it is usually better to extract an owned work item and move it into the queue than to extend the lifetime of an entire request graph.

## Concurrency Should Be Bounded, Owned, and Cancelable

The service concurrency model matters more than the individual primitive names. A small service rarely needs a large custom scheduler. It does need three things.

First, the amount of concurrent work must be bounded. If overload can translate directly into unbounded queue growth, you have not designed a service; you have delayed an outage. Bounded executors, semaphores, admission control, and per-request time budgets are more valuable than clever thread-pool internals.

Second, work must be owned. Detached threads and fire-and-forget tasks are attractive because they make local code short. They also destroy shutdown semantics. If the service can enqueue work, the service should know when that work starts, when it finishes, and how cancellation reaches it.

Third, cancellation must be part of the normal model rather than an afterthought. `std::jthread` and `std::stop_token` help here because they make stop propagation part of the type-level contract. They do not solve everything. You still need work units that check the token at sensible boundaries and storage or network operations that map cancellation into consistent errors. But they force the question into the code instead of leaving it in comments.

### Anti-pattern: blocking the event loop

One of the most common service failures is performing synchronous blocking work on a thread that should be driving I/O or dispatching requests. The service appears healthy under light load, then collapses under traffic because the event loop is stuck in a database call, a DNS resolution, or a file read.

```cpp
// BAD: synchronous blocking on the listener thread
void on_request(http_request& req) {
    auto record = db_.query_sync(req.key());   // blocks for 5-200ms
    auto enriched = enrich(record);             // CPU work, fine
    auto blob = fs::read_file(enriched.path()); // blocks again
    req.respond(200, serialize(blob));
}
// Under 50 concurrent requests, the listener thread is blocked
// for the entire duration of each request. Tail latency explodes.
// New connections queue at the OS level with no backpressure signal.
```

The fix is to dispatch blocking work to a bounded executor and keep the listener thread non-blocking.

```cpp
// BETTER: dispatch blocking work off the listener thread
void on_request(http_request& req) {
    auto work = request_work{req.key(), req.take_response_sink()};
    if (!executor_.try_submit(std::move(work))) {
        req.respond(503, "overloaded");  // explicit rejection
        metrics_.increment("request.rejected.overload");
    }
    // listener thread returns immediately, ready for next connection
}

// In the executor's worker threads:
void process(request_work work) {
    auto record = db_.query_sync(work.key);
    auto enriched = enrich(record);
    auto blob = fs::read_file(enriched.path());
    work.sink.respond(200, serialize(blob));
}
```

### Anti-pattern: no graceful shutdown

Services that lack explicit shutdown logic produce use-after-free bugs, partial writes, orphaned connections, and hung processes that must be `SIGKILL`-ed by the orchestrator. The failure is rarely visible in development because the process exits quickly. In production, in-flight work and background timers create real races.

```cpp
// BAD: shutdown by destruction order and hope
class service {
    http_listener listener_;
    database_pool db_;
    std::vector<std::jthread> workers_;
public:
    ~service() {
        // listener_ destructor closes the socket (maybe)
        // workers_ destructors request stop and join (maybe)
        // db_ destructor closes connections (maybe)
        // but workers_ may still be using db_ when db_ destructs
        // destruction order is reverse-of-declaration, so db_
        // is destroyed BEFORE workers_ — use-after-free
    }
};
```

The fix is to make shutdown an explicit, ordered operation that drains work before destroying resources.

```cpp
// BETTER: explicit drain-then-destroy shutdown
class service {
    database_pool db_;           // destroyed last
    http_listener listener_;
    bounded_executor executor_;  // owns worker threads
    std::atomic<bool> stopping_{false};
public:
    void shutdown() noexcept {
        stopping_.store(true, std::memory_order_relaxed);
        listener_.stop_accepting();               // 1. stop new work
        executor_.drain(std::chrono::seconds{5});  // 2. finish in-flight
        db_.close();                               // 3. release deps
        metrics_.flush();                          // 4. final telemetry
    }
    // destructor now only releases already-drained resources
};
```

The key insight is that destruction order is a language mechanism, not a shutdown policy. The two must be designed together, and explicit drain logic should precede any resource teardown that in-flight work might depend on.

Coroutines can improve structure if the service already benefits from asynchronous composition, especially around I/O-heavy request paths. They are a bad bargain when used only to avoid writing callbacks while the lifetime model remains vague. If a coroutine frame captures borrowed request data, executor references, and cancellation state without a clear owner, you have compressed the bug, not removed it. Use coroutines when they simplify a design whose ownership model is already sound.

## Backpressure Is a Product Decision, Not a Queue Detail

In a small service, backpressure is where local technical choices become user-visible policy. When the system is saturated, what happens? Do requests block, fail fast, shed optional work, degrade to stale data, or time out after bounded waiting? If the answer is "the queue grows," the service is still missing an operational decision.

Modern C++ helps you implement these decisions, but it does not choose them. `std::expected` can represent overload as a stable error. Value-typed work items make queue costs visible. `std::chrono`-based deadlines can be threaded through the call graph explicitly. Structured cancellation lets a request abandon subwork once the caller no longer benefits from it. None of these replace the need to decide the overload behavior.

For small services, the usual recommendation is to prefer explicit rejection over silent latency inflation. A bounded queue with clear rejection metrics is easier to operate than a "helpful" queue that absorbs bursts until memory and tail latency become somebody else's incident. The tradeoff is harsher user-visible failure under load. That is still usually the correct tradeoff because it preserves system shape and makes capacity problems measurable.

## Keep Dependency Boundaries Narrow and Translating

The code inside a small service often depends on databases, RPC clients, filesystems, clocks, and telemetry vendors. The mistake is either to abstract all of them immediately or to let vendor types flow through the entire codebase. Both approaches age badly.

Narrow boundary adapters are the practical middle ground. The service layer should depend on contracts expressed in the language of the service: persist this record, fetch this snapshot, emit this metric, publish this event. The adapter translates to and from the external API, error model, and allocation behavior.

This gives the service a place to normalize timeouts, classify failures, add observability fields, and control allocation or copying decisions. It also stops transport-specific details from leaking into application logic. A handler should receive a domain-relevant failure class that the service can act on consistently.

Do not overgeneralize these interfaces. A small service usually needs thin ports, not enterprise-wide universes of abstractions. The adapter exists to preserve ownership and failure boundaries, not to simulate a platform team.

## Observability Should Follow the Service Shape

If the service root, request model, and concurrency model are explicit, observability gets easier. Request identifiers, queue depth, active work count, dependency latency, cancellation counts, startup failures, and shutdown duration all map naturally to named boundaries. If the codebase is built from hidden globals and detached work, telemetry also becomes vague because no one can say where work begins or ends.

A small service should usually expose at least these signals.

- Startup success or failure by category.
- Request rate, latency histogram, and failure categories.
- Queue depth and rejection count for bounded work.
- Downstream dependency latency and timeout counts.
- Shutdown duration and count of canceled in-flight operations.

Anything beyond that should be justified by an operating question, not by fear. The goal is not maximum telemetry volume. The goal is fast diagnosis when the service is saturated, misconfigured, or stuck in shutdown.

## Verification Should Target Lifecycle, Not Just Behavior

The most valuable tests for a small service are rarely "returns 200 for valid input." They are tests that prove lifecycle behavior under pressure: invalid configuration blocks readiness, overload produces explicit rejection, canceled work does not commit partial state, shutdown drains owned tasks without use-after-free risk, and dependency failures remain classified correctly.

That test mix usually includes focused unit tests around adapters and translation, integration tests around startup and shutdown stories, sanitizer-backed runs for memory and concurrency hazards, and observability assertions where operational contracts matter. For example, if overload rejection is the chosen policy, the service should expose a metric or structured event that proves the policy is happening in the field.

Notice what this chapter is not repeating. It is not re-explaining test taxonomies, sanitizers, or telemetry pipelines. Those were earlier chapters. The synthesis point here is that service shape determines whether those tools can produce useful evidence.

## Where This Shape Stops Being Enough

The recommendations in this chapter are for a genuinely small service: one process, a modest number of long-lived dependencies, bounded background work, and a codebase where one team can still hold the whole runtime model in its head. Beyond that size, you may need more explicit subsystem ownership, stronger component isolation, service-level admission control, or a dedicated async framework whose lifecycle model is already opinionated.

You should also choose a different shape when the domain is dominated by one constraint this chapter only touches lightly: extreme low-latency trading, hard real-time behavior, plugin-hosting with hostile extensions, or public network servers with specialized protocol stacks. The same principles still apply, but the engineering center of gravity shifts.

## Takeaways

A good small C++23 service is built around owned resources, explicit startup and shutdown, bounded concurrency, short-lived borrowing, narrow dependency adapters, and observability tied to real lifecycle boundaries. The code should make it obvious what the process owns, how work is admitted, how cancellation propagates, and what state remains valid during failure.

The tradeoffs are deliberate. Explicit boundaries add boilerplate. Bounded queues reject work sooner. Value-typed work items may copy more than view-heavy designs. Narrow adapters add translation code. Those costs are usually cheap compared with debugging a service whose lifetime and overload behavior are implicit.

Review questions:

- What is the single ownership root for long-lived service resources?
- Which request data crosses time or thread boundaries, and does it become owned before it does?
- Where is concurrent work bounded, and what explicit overload policy follows from that bound?
- How does cancellation reach in-flight work, and what state is guaranteed after shutdown completes?
- Which dependency failures are translated into stable service-level categories rather than leaked as vendor detail?