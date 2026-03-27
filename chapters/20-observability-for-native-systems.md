# Observability for Native Systems

Testing and sanitizers reduce how often bugs escape. They do not eliminate production failures, and they do not explain live behavior under real load, real data, real dependency failures, and real rollout conditions. That is what observability is for.

In native systems, poor observability is unusually expensive. When a managed service stalls, you may still have a runtime with rich exceptions, heap snapshots, and standardized tracing hooks. In C++ you may instead have a partially symbolized crash, a thread pool stuck behind one blocked dependency, RSS growth that does not cleanly map to logical ownership, and operators who only know that latency tripled after a deployment. If logs, metrics, traces, and dump artifacts were not designed into the system before the incident, the investigation starts from guesswork.

This chapter is about runtime evidence. Keep the boundary sharp. Tests ask whether the code meets a contract before shipping. Sanitizers and static analysis mechanically search for bug classes in development and CI. Observability answers a different question: when a native system is running for real, what signals let engineers explain failure, overload, or degradation fast enough to act?

## Start from operating questions

Observability is weak when it begins as "add some logs." It gets strong when it begins with operating questions.

For a service, those questions may be:

- Why did request latency rise even though CPU stayed moderate?
- Which dependency failures are causing retries, queue growth, or partial work?
- Did shutdown hang because work ignored cancellation or because a downstream dependency never drained?
- Is memory growth caused by leaks, fragmentation, caching, or backlog?

For a library, the questions shift:

- Which host operation triggered the failure and with what inputs or version context?
- Is the library spending time on parsing, waiting, locking, allocation, or I/O?
- Can the host correlate library failures with its own request or job identifiers?

Those questions determine which fields, metrics, and spans are worth recording. Without them, teams default to verbose but low-value telemetry: string-heavy logs, counters with no dimensions, or traces that show everything except queueing, retries, and cancellation.

### What debugging looks like without observability

To make the value concrete, consider a service that processes file uploads. A user reports that uploads are timing out. Here is the handler with no structured observability:

```cpp
void handle_upload(const http_request& req) {
    std::println("INFO: Processing upload");

    std::println("INFO: Starting validation");
    auto validation = validate(req);
    if (!validation) {
        std::println(stderr, "ERROR: Validation failed: {}",
                     validation.error().message());
        return;
    }

    std::println("INFO: Storing file");
    auto store_result = store(req);
    if (!store_result) {
        std::println(stderr, "ERROR: Store failed: {}",
                     store_result.error().message());
        return;
    }

    notify(req);
    std::println("INFO: Upload complete");
}
```

Which upload failed? Was it the same user or a different one? What was it waiting on -- disk I/O, a downstream service, a lock? How long did the store call take? Was the failure retried? None of these questions are answerable from the logs this function produces. The on-call engineer resorts to grepping logs by timestamp ranges, guessing at correlation, and asking the user to reproduce.

Now the same function with structured logging, correlation IDs, and dimensional metrics:

```cpp
void handle_upload(upload_context& ctx) {
    auto span = ctx.tracer().start_span("handle_upload", {
        {"request_id", ctx.request_id()},
        {"user_id",    ctx.user_id()},
        {"file_size",  ctx.file_size()},
        {"shard",      ctx.shard_id()},
    });

    ctx.log(severity::info, "upload_started", {
        {"request_id", ctx.request_id()},
        {"file_name",  ctx.file_name()},
        {"file_size",  std::to_string(ctx.file_size())},
    });

    auto validation = validate(ctx);
    if (!validation) {
        ctx.log(severity::warning, "validation_failed", {
            {"request_id", ctx.request_id()},
            {"reason",     validation.error().category()},
        });
        ctx.metrics().increment("upload_failures", 1,
            {{"reason", "validation"}, {"shard", ctx.shard_id()}});
        return;
    }

    auto store_result = store(ctx);
    if (!store_result) {
        ctx.log(severity::error, "store_failed", {
            {"request_id",  ctx.request_id()},
            {"dependency",  "blob_store"},
            {"error_class", store_result.error().category()},
            {"latency_ms",  std::to_string(store_result.elapsed_ms())},
        });
        ctx.metrics().increment("upload_failures", 1,
            {{"reason", "store"}, {"shard", ctx.shard_id()}});
        return;
    }

    ctx.metrics().observe_latency("upload_duration_ms", span.elapsed_ms(),
        {{"shard", ctx.shard_id()}});
    ctx.log(severity::info, "upload_complete", {
        {"request_id", ctx.request_id()},
        {"latency_ms", std::to_string(span.elapsed_ms())},
    });
}
```

Now the on-call engineer filters by `request_id`, sees that the timeout happened during `store` with `dependency=blob_store`, checks the `upload_duration_ms` histogram by shard, and discovers that shard-3 latency spiked at 09:40. The blob store dashboard confirms the dependency was degraded. Total investigation time drops from hours to minutes.

Both versions of `handle_upload` do the same work: validate, store, notify. The difference is not more code. It is code that was written with operating questions in mind from the start.

## Logs should explain decisions and state transitions

Logs are most useful when they capture decisions the system made and the state that mattered at the time, not when they narrate every function call. In native systems, this discipline matters even more because the volume and overhead of logging can become a performance problem quickly.

Good production logs are structured, sparse, and stable.

- Structured means the important fields are emitted as machine-readable key-value data, not hidden in prose.
- Sparse means the default path is quiet and the exceptional path is informative.
- Stable means field names and meanings do not drift every sprint.

When an operation fails, the log record should usually capture identity, classification, and local operating context.

- Request or job identifier.
- Operation or route name.
- Failure category, not just a formatted message.
- Retryability or permanence if the code knows it.
- Resource indicators that change diagnosis, such as queue depth, shard, peer, or attempt number.
- Version or build metadata when rollout state matters.

Avoid two common mistakes.

First, do not make logs the only source of truth for metrics-like questions. If you need to know retry rate or queue depth, emit those as metrics instead of forcing operators to reconstruct them from text. Second, do not log high-cardinality payloads or sensitive blobs just because an incident once needed them. Put those behind deliberate sampling or debug paths.

`std::source_location` can be useful in low-volume internal diagnostics or infrastructure code, especially when you need a stable call-site tag without hand-maintaining strings. It is not a substitute for a meaningful operation name. A log saying `source=foo.cpp:412` is weaker than one saying `operation=manifest_reload phase=commit`.

The example project's `request_logger()` middleware (in `examples/web-api/src/modules/middleware.cppm`) shows a minimal but structured starting point for per-request logging:

```cpp
// examples/web-api/src/modules/middleware.cppm — request_logger()
return [](const http::Request& req, const http::Handler& next) -> http::Response {
    auto start = std::chrono::steady_clock::now();
    auto resp = next(req);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    std::println("[{}] {} {} → {} ({} μs)",
        "LOG",
        http::method_to_string(req.method),
        req.path,
        resp.status,
        ms
    );
    return resp;
};
```

Every response carries method, path, status code, and latency. That is not yet production-grade structured logging -- the fields are embedded in formatted text rather than emitted as machine-queryable key-value pairs -- but it demonstrates the right instinct: capture identity (route), outcome (status), and timing (latency) at a single cross-cutting point. Moving to a structured JSON or key-value emitter is a natural evolution.

### Unstructured versus structured logging in practice

The difference between unstructured and structured logs matters most during incidents, when the person reading logs is under time pressure and may not have written the code.

```cpp
// Unstructured: human-readable but machine-hostile.
log("Failed to connect to database server db-prod-3 after 3 retries "
    "(last error: connection refused), request will be dropped");
```

This line contains useful information, but extracting it requires parsing English. You cannot filter by retry count, dependency name, or error class without fragile regex. Across a fleet of instances, aggregating failure patterns from lines like this is expensive and error-prone.

```cpp
// Structured: same information, machine-queryable.
ctx.log(severity::error, "dependency_connect_failed", {
    {"dependency",  "db-prod-3"},
    {"attempts",    "3"},
    {"last_error",  "connection_refused"},
    {"action",      "request_dropped"},
    {"request_id",  ctx.request_id()},
});
```

Now `dependency_connect_failed` events can be counted, filtered by dependency name, and correlated with specific requests. The field names are stable across code changes, so dashboards and alerts do not break when someone rewords a log message.

## Metrics should track throughput, saturation, and failure shape

Metrics answer questions that logs answer poorly: rates, distributions, long-term drift, and comparative behavior across instances. For native systems, the most useful metrics usually fall into three families.

The first is throughput and latency: request rate, task completion rate, retry rate, and latency histograms for important stages. Use histograms for latency, not just averages. Native performance failures are often tail problems.

The second is saturation: queue depth, worker utilization, open file descriptors, connection pool occupancy, allocator pressure, pending timers, and outstanding background tasks. These tell you whether the system is busy in a healthy way or accumulating work it cannot retire.

The third is failure shape: counts by error category, timeout counts, cancellation counts, parse failures, dropped work, crash-loop restarts, and degraded-mode activations. These reveal whether the system is failing because dependencies are slow, because input quality changed, or because internal backpressure kicked in.

The example project's `ErrorCode` enum (in `examples/web-api/src/modules/error.cppm`) illustrates the foundation for failure-shape metrics. The closed set -- `not_found`, `bad_request`, `conflict`, `internal_error` -- with its `constexpr to_http_status()` mapping gives every failure a stable category. In a production evolution, you would increment a counter dimensioned by `ErrorCode` at the handler boundary, turning the type system's classification into queryable metric labels without inventing ad-hoc string tags.

Use labels conservatively. High-cardinality labels are one of the fastest ways to turn a metrics system into an expensive liability. Request ID, user ID, file path, arbitrary exception text, and raw peer address usually do not belong as labels. Region, route, dependency name, result category, and bounded shard identifiers often do.

Gauges also deserve suspicion. They are easy to add and easy to misread. If a queue depth gauge jumps, is that a brief burst or a persistent trend? Pair gauges with rates or histograms when possible so operators can tell whether the state is draining.

## Traces need to follow asynchronous ownership, not just synchronous calls

In distributed services and async native systems, traces are the only practical way to see where end-to-end time went. But C++ code often loses trace value by failing to preserve context across executors, callbacks, thread hops, and coroutine suspension.

If a request enters a service, enqueues background work, awaits a downstream call, and later resumes on another worker, the trace should still describe one coherent operation. That requires explicit propagation of trace context at the boundaries where ownership of work crosses time.

This is where earlier design decisions matter. Structured concurrency and explicit cancellation scopes make tracing cleaner because the parent-child relationships are already meaningful. Detached work and ad hoc thread spawning make traces fragment into unrelated spans.

Record spans for stages that correspond to actual waiting or service boundaries.

- Time spent queued before work starts.
- Time spent executing local CPU work.
- Time spent waiting on downstream I/O.
- Time spent retrying or backing off.
- Time lost to cancellation, shutdown, or overload shedding.

Do not create spans for every helper function. That produces trace noise without causal value. The purpose of tracing is to explain latency structure and dependency shape, not to restate the call graph.

The middleware pipeline in the example project (see `middleware::chain()` in `examples/web-api/src/modules/middleware.cppm`) is a natural carrier for trace context propagation. Each middleware wraps the next handler and has access to both the request and the response. A tracing middleware inserted into the pipeline can start a span before calling `next`, attach it to the request context, and close the span after the response returns. Because the pipeline already composes as a chain of `std::function` wrappers, adding a tracing stage requires no changes to handler signatures -- a clean cross-cutting insertion point for trace propagation.

### Without trace context: the invisible queue

A common failure mode in async native services is latency that lives in queueing, not in execution. Without trace propagation across executor boundaries, this is invisible:

```cpp
// No trace context propagation. The span only covers execution, not waiting.
void enqueue_work(thread_pool& pool, request req) {
    pool.submit([req = std::move(req)] {
        auto span = tracer::start_span("process_request");  // Starts when work RUNS.
        process(req);
    });
    // Time between submit() and when the lambda actually executes is lost.
    // If the pool is saturated, requests wait 500ms in the queue,
    // but traces show 2ms of execution time. Operators see low latency
    // in traces while users experience high latency. The queue time is a
    // blind spot.
}
```

With proper context propagation, the full picture is visible:

```cpp
void enqueue_work(thread_pool& pool, request req, trace_context ctx) {
    auto enqueue_time = steady_clock::now();
    pool.submit([req = std::move(req), ctx = std::move(ctx), enqueue_time] {
        auto queue_span = ctx.start_span("queued", {
            {"queue_ms", std::to_string(duration_cast<milliseconds>(
                steady_clock::now() - enqueue_time).count())},
        });
        queue_span.end();

        auto exec_span = ctx.start_span("process_request");
        process(req);
    });
}
```

Now the trace shows two spans -- queueing and execution -- both linked to the parent request. When queue time dominates, it is immediately visible in the trace waterfall. This is the kind of latency that metrics alone (average processing time) systematically hide.

## Crash diagnostics are part of observability, not a separate emergency hobby

Native services and tools need a crash story before the first crash. That story includes more than "enable dumps." You need to know where dumps go, how they are symbolized, how they map to exact builds, how operators correlate them with logs and traces, and which process metadata is attached.

At minimum, a crash event should be linkable to:

- Exact binary or build ID.
- Symbol files produced by the matching build.
- Deployment metadata such as version, environment, and rollout ring.
- Recent structured breadcrumbs around the failing operation.
- Thread identities and, where possible, stack traces for relevant threads.

The build work from the previous chapter is what makes this viable. Symbol servers, build IDs, and release metadata are build concerns. Their operational payoff shows up here when an incident happens at 03:00 and the on-call engineer needs a useful stack instead of raw addresses.

Crash reporting also needs policy. Some components should fail fast because continuing risks data corruption. Others can isolate a failing request or plugin and keep the host alive. Observability should make that decision legible after the fact. If the process aborts intentionally on invariant violation, emit enough context before termination that the crash is distinguishable from an arbitrary segfault.

## Resource visibility matters more in native systems

A recurring operational problem in C++ services is confusing logical work growth with memory bugs. RSS climbs, latency rises, and everyone asks whether there is a leak. Sometimes there is. Often the explanation is less clean: allocator retention, oversized caches, unbounded queues, stalled consumers, mmap growth, file descriptor leaks, or fragmentation under a new traffic pattern.

You cannot solve that with one metric. You need a set of resource signals that connect runtime behavior to likely causes.

- RSS and virtual memory for coarse process shape.
- Allocator-specific statistics where available.
- Queue depth and backlog age for in-memory work accumulation.
- Open file descriptor or handle counts.
- Active thread count and blocked-thread indicators.
- Connection pool occupancy and timeout counts.
- Cache size and eviction metrics for components that intentionally retain memory.

The point is not to expose every allocator bin or kernel counter. The point is to make the likely failure modes distinguishable. If memory climbs while queue depth and backlog age also climb, overload is a stronger hypothesis than a pure leak. If memory climbs while queue depth stays flat and handle counts rise, a resource leak becomes more plausible. Observability should narrow the search space.

## Libraries need host-owned telemetry boundaries

A reusable library should not assume a global logging framework, metrics backend, or tracing SDK. That creates the same dependency inversion problems discussed earlier in the book, now in operational form. Libraries should instead expose a narrow diagnostics boundary that the host can implement.

### Intentional partial: a library-facing diagnostics sink

```cpp
enum class severity { debug, info, warning, error };

struct diagnostic_field {
    std::string_view key;
    std::string_view value;
};

struct diagnostics_sink {
    virtual ~diagnostics_sink() = default;

    virtual void record_event(severity level,
                              std::string_view event_name,
                              std::span<diagnostic_field const> fields) noexcept = 0;

    virtual void increment_counter(std::string_view name,
                                   std::int64_t delta,
                                   std::span<diagnostic_field const> dimensions) noexcept = 0;
};
```

This kind of interface keeps the library honest. It can report parse failures, retries, cache evictions, or reload timings without hard-coding a vendor SDK into every binary that uses it. The host decides how to attach request IDs, export metrics, or bridge into traces.

The tradeoff is deliberate abstraction work. For a small internal-only component, direct integration may be acceptable. For a reusable library, host-owned telemetry is usually the cleaner long-term design.

## What to avoid

Native observability goes wrong in familiar ways.

- Logging every allocation, lock acquisition, or function entry in the hope that more data is always safer.
- Using high-cardinality identifiers as metric dimensions.
- Emitting traces that follow synchronous helper calls but drop async context on executor or coroutine boundaries.
- Shipping binaries with weak symbolization and expecting crash analysis to work later.
- Treating logs as the only operational interface instead of combining logs, metrics, traces, and dumps.
- Making library telemetry depend on a specific service logging stack.

All of these create cost without proportional diagnostic value.

## Takeaways

Observability in native systems is runtime evidence design. Start from concrete operating questions. Use logs for decisions and state transitions, metrics for rates and saturation, traces for end-to-end latency structure, and crash artifacts for postmortem debugging. Preserve the async and ownership boundaries that make these signals meaningful. Expose resource signals that let operators distinguish leaks, backlog, fragmentation, and dependency stalls.

The main tradeoff is overhead versus explanation quality. Rich telemetry adds CPU, memory, storage, and design complexity. Sparse telemetry lengthens incidents and leaves native failures ambiguous. Choose the smallest signal set that answers your real operating questions, then make it stable.

Review questions:

- Which operating questions can this service or library answer today without guessing?
- Which log fields are stable and structured enough to support automation, not just humans reading text?
- Which metrics distinguish throughput, saturation, and failure shape rather than mixing them together?
- Does trace context survive executor hops, callbacks, and coroutine suspension boundaries?
- Can a production crash be correlated with exact build identity, symbols, and nearby operational context?
