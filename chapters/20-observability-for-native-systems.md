# Observability for Native Systems

Testing and sanitizers reduce how often bugs escape. They do not eliminate production failures, and they do not explain live behavior under real load, real data, real dependency failures, and real rollout conditions. That is what observability is for.

In native systems, poor observability is unusually expensive. When a managed service stalls, you may still have a runtime with rich exceptions, heap snapshots, and standardized tracing hooks. In C++ you may instead have a partially symbolized crash, a thread pool stuck behind one blocked dependency, RSS growth that does not cleanly map to logical ownership, and operators who only know that latency tripled after a deployment. If logs, metrics, traces, and dump artifacts were not designed into the system before the incident, the investigation starts from guesswork.

This chapter is about runtime evidence. Keep the boundary sharp. Tests ask whether the code meets a contract before shipping. Sanitizers and static analysis mechanically search for bug classes in development and CI. Observability answers a different question: when a native system is running for real, what signals let engineers explain failure, overload, or degradation fast enough to act?

## Start From Operating Questions

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

## Logs Should Explain Decisions and State Transitions

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

## Metrics Should Track Throughput, Saturation, and Failure Shape

Metrics answer questions that logs answer poorly: rates, distributions, long-term drift, and comparative behavior across instances. For native systems, the most useful metrics usually fall into three families.

The first is throughput and latency: request rate, task completion rate, retry rate, and latency histograms for important stages. Use histograms for latency, not just averages. Native performance failures are often tail problems.

The second is saturation: queue depth, worker utilization, open file descriptors, connection pool occupancy, allocator pressure, pending timers, and outstanding background tasks. These tell you whether the system is busy in a healthy way or accumulating work it cannot retire.

The third is failure shape: counts by error category, timeout counts, cancellation counts, parse failures, dropped work, crash-loop restarts, and degraded-mode activations. These reveal whether the system is failing because dependencies are slow, because input quality changed, or because internal backpressure kicked in.

Use labels conservatively. High-cardinality labels are one of the fastest ways to turn a metrics system into an expensive liability. Request ID, user ID, file path, arbitrary exception text, and raw peer address usually do not belong as labels. Region, route, dependency name, result category, and bounded shard identifiers often do.

Gauges also deserve suspicion. They are easy to add and easy to misread. If a queue depth gauge jumps, is that a brief burst or a persistent trend? Pair gauges with rates or histograms when possible so operators can tell whether the state is draining.

## Traces Need To Follow Asynchronous Ownership, Not Just Synchronous Calls

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

## Crash Diagnostics Are Part of Observability, Not a Separate Emergency Hobby

Native services and tools need a crash story before the first crash. That story includes more than "enable dumps." You need to know where dumps go, how they are symbolized, how they map to exact builds, how operators correlate them with logs and traces, and which process metadata is attached.

At minimum, a crash event should be linkable to:

- Exact binary or build ID.
- Symbol files produced by the matching build.
- Deployment metadata such as version, environment, and rollout ring.
- Recent structured breadcrumbs around the failing operation.
- Thread identities and, where possible, stack traces for relevant threads.

The build work from the previous chapter is what makes this viable. Symbol servers, build IDs, and release metadata are build concerns. Their operational payoff shows up here when an incident happens at 03:00 and the on-call engineer needs a useful stack instead of raw addresses.

Crash reporting also needs policy. Some components should fail fast because continuing risks data corruption. Others can isolate a failing request or plugin and keep the host alive. Observability should make that decision legible after the fact. If the process aborts intentionally on invariant violation, emit enough context before termination that the crash is distinguishable from an arbitrary segfault.

## Resource Visibility Matters More in Native Systems

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

## Libraries Need Host-Owned Telemetry Boundaries

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

## What To Avoid

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