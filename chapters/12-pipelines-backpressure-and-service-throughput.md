# Chapter 12: Pipelines, Backpressure, and Service Throughput

> **Prerequisites:** Chapters 9–11. Pipeline stages share mutable state across threads (Chapter 9's synchronization primitives), and the task and coroutine infrastructure from Chapter 11 provides the execution model that stages run on. Familiarity with bounded queues, condition variables, and cooperative cancellation is assumed throughout.

## 12.1 The Production Problem

A service accepts requests, transforms them through several processing stages, and emits responses. Each stage does different work: parsing, validation, enrichment from a cache or external call, serialization, I/O. The team writes each stage as a concurrent task, connects them with queues, and the system works in testing.

Under production load, the system fails in ways that unit tests never exercised:

- A slow downstream dependency causes one stage to stall. The upstream stage keeps producing. The intervening queue grows without bound, consuming memory until the process is killed by the OOM reaper.
- A burst of requests fills all queues simultaneously. Recovery requires draining every stage, but the stages are coupled: the downstream stall prevents upstream drain. The system enters a state where all threads are blocked, all queues are full, and forward progress requires capacity that no longer exists.
- Retry logic in a middle stage amplifies load. One failed enrichment call becomes three, each occupying a thread and a queue slot, creating artificial fan-out that the downstream stages never sized for.
- Tail latency climbs because items that entered the pipeline during a burst sit in queues long after the burst ends. The item's total latency is dominated by queueing delay, not processing time, but no metric distinguishes the two.

These are not concurrency bugs in the traditional sense. The locks are correct. The atomics are sound. The failure is architectural: the pipeline has no flow control. It processes work as fast as each stage can go, with no mechanism for a slow stage to signal upstream that it cannot keep up.

Flow control in pipelines is the same problem as flow control in networks. TCP solved it decades ago with window-based backpressure. The lesson applies directly: producers must not outrun consumers, and the mechanism for throttling must be explicit, bounded, and observable.

## 12.2 The Naive Approach: Unbounded Queues and Hope

The most common first design connects stages with `std::queue` behind a mutex, or a lock-free queue with no capacity limit.

```cpp
// Anti-pattern: unbounded inter-stage queue
template <typename T>
class UnboundedChannel {
    std::queue<T> queue_;
    mutable std::mutex mu_;
    std::condition_variable not_empty_;

public:
    void send(T item) {
        {
            std::lock_guard lock(mu_);
            queue_.push(std::move(item));  // BUG: no upper bound on queue size
        }
        not_empty_.notify_one();
    }

    T recv() {
        std::unique_lock lock(mu_);
        not_empty_.wait(lock, [&] { return !queue_.empty(); });
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }
};
```

This works under light load. Under heavy load, `send` never blocks, so a fast producer fills memory without any feedback signal. The queue becomes a buffer that hides the problem until it becomes catastrophic.

A second common mistake is to add a capacity limit but respond to a full queue by dropping items silently:

```cpp
// Anti-pattern: silent drop on overflow
void send(T item) {
    std::lock_guard lock(mu_);
    if (queue_.size() >= max_size_) {
        return;  // RISK: caller has no idea the item was lost
    }
    queue_.push(std::move(item));
}
```

Silent drops turn a throughput problem into a correctness problem. The caller believes work was accepted. Downstream stages never see it. The failure is invisible until an audit discovers missing records, or a customer reports that one in a thousand requests vanishes.

Both designs share the same flaw: the producer has no way to learn that the consumer is falling behind, and no mechanism to slow down in response.

## 12.3 Bounded Channels with Backpressure

A bounded channel is a queue with a fixed capacity where `send` blocks (or returns a failure indicator) when the queue is full. This is the fundamental building block of pipeline backpressure.

```cpp
#include <deque>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <stop_token>

template <typename T>
class BoundedChannel {
    std::deque<T> buf_;
    std::size_t cap_;
    mutable std::mutex mu_;
    std::condition_variable_any not_full_;
    std::condition_variable_any not_empty_;
    bool closed_ = false;

public:
    explicit BoundedChannel(std::size_t capacity) : cap_(capacity) {}

    // Returns false if the channel was closed while waiting.
    bool send(T item, std::stop_token st = {}) {
        std::unique_lock lock(mu_);
        auto can_send = [&] { return closed_ || buf_.size() < cap_; };
        if (st.stop_possible()) {
            if (!not_full_.wait(lock, st, can_send)) return false;
        } else {
            not_full_.wait(lock, can_send);
        }
        if (closed_) return false;
        buf_.push_back(std::move(item));
        not_empty_.notify_one();
        return true;
    }

    // Returns nullopt when the channel is closed and drained.
    std::optional<T> recv(std::stop_token st = {}) {
        std::unique_lock lock(mu_);
        auto can_recv = [&] { return closed_ || !buf_.empty(); };
        if (st.stop_possible()) {
            if (!not_empty_.wait(lock, st, can_recv)) return std::nullopt;
        } else {
            not_empty_.wait(lock, can_recv);
        }
        if (buf_.empty()) return std::nullopt;  // closed and drained
        T item = std::move(buf_.front());
        buf_.pop_front();
        not_full_.notify_one();
        return item;
    }

    void close() {
        {
            std::lock_guard lock(mu_);
            closed_ = true;
        }
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard lock(mu_);
        return buf_.size();
    }
};
```

Intentional partial: later examples assume an obvious non-blocking `try_recv()` convenience wrapper around the same queue. The backpressure contract lives in `send`, `recv`, and `close`, so the helper is omitted here.

Key design decisions:

**Blocking send.** When the buffer is full, the producer waits. This is the backpressure signal: the producer physically cannot outrun the consumer. Queue depth is bounded by construction, not by hope.

**Stop token integration.** `std::stop_token` from C++20 (universally available in C++23) provides cooperative cancellation. A stage that is shutting down can interrupt blocked senders and receivers without closing the channel or introducing a separate cancellation flag. This dovetails with the cancellation model from Chapter 11.

**Close semantics.** Closing the channel wakes all waiters. Receivers drain remaining items before seeing the closed state. This avoids the common bug where closing a channel loses in-flight work.

**`std::deque` over `std::queue`.** `std::deque` gives the same FIFO behavior but avoids the wrapper overhead and allows `size()` without walking the container. For hot paths, a ring buffer on a contiguous allocation is better; the interface stays the same.

## 12.4 Composing Pipeline Stages

A pipeline stage is a function that reads from an input channel, processes each item, and writes to an output channel. The type signature captures the contract:

```cpp
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

template <typename In, typename Out>
using StageFunc = std::function<std::optional<Out>(const In&)>;

template <typename In, typename Out>
std::jthread run_stage(
    BoundedChannel<In>& input,
    BoundedChannel<Out>& output,
    StageFunc<In, Out> process,
    std::size_t concurrency = 1)
{
    // Returns a jthread managing `concurrency` workers for this stage.
    // For concurrency > 1, internal fan-out is needed.
    return std::jthread([&input, &output, process, concurrency](std::stop_token st) {
        auto remaining_workers = std::make_shared<std::atomic<std::size_t>>(concurrency);
        auto worker = [&](std::stop_token st) {
            while (auto item = input.recv(st)) {
                auto result = process(*item);
                if (result) {
                    if (!output.send(std::move(*result), st)) return;
                }
                // If process returns nullopt, the item is filtered out.
            }
            // Input channel closed and drained. Only the last worker closes the
            // output; earlier closure would drop items still being processed by
            // sibling workers.
            if (remaining_workers->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                output.close();
            }
        };

        if (concurrency <= 1) {
            worker(st);
            return;
        }

        std::vector<std::jthread> workers;
        workers.reserve(concurrency);
        for (std::size_t i = 0; i < concurrency; ++i) {
            workers.emplace_back(worker);
        }
        // jthread destructors join on scope exit.
    });
}
```

A three-stage pipeline for an HTTP-style service might look like:

```cpp
// Realistic domain: request processing pipeline
struct RawRequest { std::string payload; uint64_t id; };
struct ValidatedRequest { ParsedPayload data; uint64_t id; };
struct EnrichedRequest { ParsedPayload data; UserProfile profile; uint64_t id; };
struct Response { uint64_t id; std::string body; int status; };

void run_pipeline(std::stop_token shutdown) {
    BoundedChannel<RawRequest>       parsed(256);
    BoundedChannel<ValidatedRequest> validated(128);
    BoundedChannel<EnrichedRequest>  enriched(64);
    BoundedChannel<Response>         responses(256);

    // Stage 1: Parse and validate. CPU-bound, scales with cores.
    auto parse = run_stage<RawRequest, ValidatedRequest>(
        parsed, validated,
        [](const RawRequest& r) -> std::optional<ValidatedRequest> {
            auto result = parse_and_validate(r.payload);
            if (!result) return std::nullopt;  // drop malformed
            return ValidatedRequest{std::move(*result), r.id};
        },
        /*concurrency=*/4);

    // Stage 2: Enrich from cache/DB. IO-bound, needs more concurrency
    // to hide latency.
    auto enrich = run_stage<ValidatedRequest, EnrichedRequest>(
        validated, enriched,
        [](const ValidatedRequest& v) -> std::optional<EnrichedRequest> {
            auto profile = fetch_user_profile(v.data.user_id);
            if (!profile) return std::nullopt;  // enrichment failed
            return EnrichedRequest{v.data, std::move(*profile), v.id};
        },
        /*concurrency=*/16);

    // Stage 3: Serialize response. CPU-bound again.
    auto serialize = run_stage<EnrichedRequest, Response>(
        enriched, responses,
        [](const EnrichedRequest& e) -> std::optional<Response> {
            return Response{e.id, render_response(e), 200};
        },
        /*concurrency=*/4);

    // Response writer runs in this thread.
    while (auto resp = responses.recv(shutdown)) {
        send_to_client(resp->id, resp->body, resp->status);
    }
}
```

Notice the channel capacities taper: 256, 128, 64. This is deliberate. The enrichment stage is the bottleneck (IO-bound with external calls). Giving it a smaller input buffer means backpressure reaches the parse stage faster, which reduces the amount of work that has already been validated but cannot yet be enriched. Work sitting in a queue is wasted memory and latency.

## 12.5 Choosing Queue Capacity

Queue capacity is not a tuning knob to maximize. It is a latency budget.

If an item takes `P` milliseconds to process and the queue holds `N` items, the worst-case queueing delay is `N * P / concurrency`. A queue of 10,000 items in front of a stage that takes 1ms per item with 4 workers means up to 2.5 seconds of queueing delay. That delay is invisible in throughput metrics — the stage is processing at full speed — but it destroys tail latency.

Rules of thumb:

- **Start small.** A capacity of 1-2x the stage's concurrency level is a reasonable starting point. It provides enough buffering to absorb scheduling jitter without hiding a throughput mismatch.
- **Measure queueing delay, not just throughput.** Instrument `send` and `recv` with timestamps. The difference between an item's enqueue time and its dequeue time is the queueing delay. If this grows under load, the downstream stage is the bottleneck.
- **Do not size queues to absorb bursts.** Large queues turn bursts into sustained latency increases. If you need burst absorption, do it at the ingress point with an explicit admission control policy (see below), not by making internal queues deep.

## 12.6 Admission Control and Overload

Backpressure propagates from the slowest stage back to the ingress point. At that point, the system has a choice: block the caller, reject the request, or shed load. This is admission control.

```cpp
class IngressController {
    BoundedChannel<RawRequest>& input_;
    std::atomic<uint64_t> in_flight_{0};
    uint64_t max_in_flight_;
    std::atomic<uint64_t> rejected_{0};

public:
    IngressController(BoundedChannel<RawRequest>& input,
                      uint64_t max_in_flight)
        : input_(input), max_in_flight_(max_in_flight) {}

    enum class AdmitResult { accepted, rejected, shutting_down };

    AdmitResult try_admit(RawRequest req, std::stop_token st) {
        uint64_t current = in_flight_.load(std::memory_order_relaxed);
        while (current < max_in_flight_) {
            if (in_flight_.compare_exchange_weak(current, current + 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                if (!input_.send(std::move(req), st)) {
                    in_flight_.fetch_sub(1, std::memory_order_relaxed);
                    return AdmitResult::shutting_down;
                }
                return AdmitResult::accepted;
            }
        }
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return AdmitResult::rejected;
    }

    void complete_one() {
        in_flight_.fetch_sub(1, std::memory_order_relaxed);
    }

    uint64_t rejected_count() const {
        return rejected_.load(std::memory_order_relaxed);
    }
};
```

The `in_flight_` counter tracks how many requests are anywhere in the pipeline. When it reaches the limit, new requests are rejected immediately with an appropriate status (HTTP 503, gRPC RESOURCE_EXHAUSTED). This keeps the pipeline from accumulating unbounded work.

The maximum in-flight count should be derived from measurement, not guessed. Profile the pipeline under steady-state load. Find the throughput ceiling — the point where adding more in-flight requests no longer increases throughput but does increase latency. Set the limit slightly above that point to absorb jitter.

### 12.6.1 Why Not Just Use the Queue Capacity?

Queue capacity and admission control solve different problems. Queue capacity bounds memory between two specific stages. Admission control bounds total work in the system. A pipeline with five stages, each with a queue of 64, can have 320 items in flight — far more than the system can process promptly. The admission controller sets a single, system-wide limit that reflects actual processing capacity.

## 12.7 Head-of-Line Blocking and Priority

When all items share a single queue, a slow item blocks everything behind it. This is head-of-line blocking, and it is the most common source of latency variance in pipeline systems.

Two structural approaches address this:

**Multiple priority lanes.** Separate channels for different priority classes, each with its own capacity. Workers poll the high-priority channel first.

```cpp
template <typename T>
class PriorityDrain {
    BoundedChannel<T>& high_;
    BoundedChannel<T>& normal_;

public:
    PriorityDrain(BoundedChannel<T>& high, BoundedChannel<T>& normal)
        : high_(high), normal_(normal) {}

    std::optional<T> recv(std::stop_token st) {
        // Non-blocking try of high-priority first.
        // Falls back to blocking wait on both.
        // Simplified: real implementation uses platform-specific
        // multi-channel wait or a polling loop with short timeout.
        if (auto item = high_.try_recv()) return item;
        return normal_.recv(st);
    }
};
```

This works when the classification is known at ingress. It does not help when slowness is discovered mid-pipeline (for example, a cache miss that turns a fast path into a slow one).

**Work-stealing with item deadlines.** Attach a deadline to each item. When a worker dequeues an item whose deadline has already passed, it skips processing and emits a timeout error. This prevents stale work from consuming resources that should serve fresh requests.

```cpp
struct TimedItem {
    RawRequest request;
    std::chrono::steady_clock::time_point deadline;
};

auto process = [](const TimedItem& item) -> std::optional<ValidatedRequest> {
    if (std::chrono::steady_clock::now() > item.deadline) {
        record_metric("pipeline.expired_in_queue", 1);
        return std::nullopt;  // drop expired work
    }
    return parse_and_validate(item.request.payload);
};
```

Dropping expired work is counterintuitive but correct under overload. The client has already timed out. Processing the request wastes resources and produces a response nobody will read. The metric makes the expiration rate visible so the team can adjust capacity or shedding thresholds.

## 12.8 Fan-Out, Fan-In, and Retry Amplification

Some pipelines fan out: one input item produces N downstream items (e.g., a batch request, a scatter query). The downstream channel must be sized for the amplified rate, or the fan-out stage will block on send, stalling the entire upstream.

More subtly, retries inside a stage act as implicit fan-out. If a stage retries a failed enrichment call three times, it triples its demand on the downstream dependency and triples the time it occupies a thread. Under load, this creates a feedback loop: the dependency is slow because it is overloaded, the retries add more load, the dependency gets slower, more retries fire.

The fix is bounded, budgeted retry:

```cpp
struct RetryPolicy {
    int max_attempts = 3;
    std::chrono::milliseconds base_delay{10};
    std::chrono::milliseconds max_delay{500};
    std::chrono::steady_clock::time_point deadline;
};

template <typename F>
auto with_retry(F&& fn, RetryPolicy policy, std::stop_token st)
    -> std::optional<std::invoke_result_t<F>>
{
    using namespace std::chrono;
    auto delay = policy.base_delay;

    for (int attempt = 0; attempt < policy.max_attempts; ++attempt) {
        if (st.stop_requested()) return std::nullopt;
        if (steady_clock::now() > policy.deadline) return std::nullopt;

        auto result = fn();
        if (result) return result;

        if (attempt + 1 < policy.max_attempts) {
            std::this_thread::sleep_for(
                std::min(delay, duration_cast<milliseconds>(
                    policy.deadline - steady_clock::now())));
            delay = std::min(delay * 2, policy.max_delay);
        }
    }
    return std::nullopt;
}
```

The deadline check is critical. Without it, retries continue long after the original request has timed out, wasting capacity. The exponential backoff with a cap limits the rate at which retries hit the dependency. Together, these ensure that retry behavior degrades gracefully under sustained failure rather than amplifying it.

A circuit breaker (tracking the error rate over a rolling window and short-circuiting calls when it exceeds a threshold) provides the system-level complement. Individual retry policies limit per-request cost; the circuit breaker limits aggregate cost.

## 12.9 Observability

A pipeline that you cannot observe is a pipeline that you cannot operate. The minimum instrumentation for a production pipeline:

- **Queue depth per channel** — sampled periodically or emitted as a gauge. Rising queue depth is the first signal that a stage is falling behind.
- **Queueing delay per item** — the time between enqueue and dequeue. This is the most direct measure of backpressure impact on latency.
- **Stage processing time** — wall-clock time per item, per stage. Identifies which stage is the bottleneck.
- **Rejection and expiration counts** — from admission control and deadline checks. These track how much load the system is shedding.
- **In-flight count** — the total number of items in the pipeline. Correlated with latency, this shows whether the system is operating in its efficient region.

```cpp
struct StageMetrics {
    std::atomic<uint64_t> items_processed{0};
    std::atomic<uint64_t> items_filtered{0};
    std::atomic<uint64_t> processing_ns_total{0};

    void record(std::chrono::nanoseconds processing_time, bool filtered) {
        if (filtered) {
            items_filtered.fetch_add(1, std::memory_order_relaxed);
        } else {
            items_processed.fetch_add(1, std::memory_order_relaxed);
        }
        processing_ns_total.fetch_add(
            processing_time.count(), std::memory_order_relaxed);
    }

    double avg_processing_ms() const {
        auto total = processing_ns_total.load(std::memory_order_relaxed);
        auto count = items_processed.load(std::memory_order_relaxed)
                   + items_filtered.load(std::memory_order_relaxed);
        if (count == 0) return 0.0;
        return static_cast<double>(total) / static_cast<double>(count) / 1e6;
    }
};
```

These metrics should feed into whatever observability system the service already uses (Prometheus, OpenTelemetry, internal counters). The point is not the export mechanism; it is that the information exists.

## 12.10 Tradeoffs and Boundaries

**Bounded queues trade throughput for latency predictability.** An unbounded queue maximizes throughput at the cost of arbitrary latency under load. A bounded queue with admission control sacrifices some peak throughput (rejected requests) to keep latency bounded. The right tradeoff depends on the service's SLO. Most services care more about p99 latency than peak throughput.

**Backpressure requires end-to-end design.** A bounded channel between two stages is only useful if the backpressure signal eventually reaches a point that can do something about it — reject, shed, or throttle. If the ingress point is a push-based network socket with no flow control, the backpressure has nowhere to go, and the system accumulates work in the network stack or OS buffers instead.

**Concurrency per stage is not free.** More workers on a stage reduce queueing delay for that stage but increase contention on the shared channel, increase context-switch overhead, and may increase contention on downstream resources (database connections, external APIs). Measure; do not guess.

**Coroutine-based stages change the cost model.** If stages are coroutines on an executor (Chapter 11), a "blocked" send does not consume an OS thread — it suspends, freeing the thread for other work. This changes the concurrency arithmetic: you can have thousands of in-flight items without thousands of threads. But it also means backpressure is less visible; a coroutine waiting on a full channel does not show up as a blocked thread in `top`. Instrumentation becomes more important, not less.

**Fan-out and fan-in require explicit capacity planning.** If a stage can produce multiple output items per input item, the downstream channel's capacity and the downstream stage's concurrency must account for the amplification factor. This is easy to overlook when the fan-out ratio varies per item.

**This chapter does not cover distributed pipelines.** The patterns here apply within a single process or a tightly coupled set of threads. Distributed stream processing (Kafka, Flink, etc.) shares the same concepts — bounded buffers, backpressure, admission control — but adds network partitions, exactly-once semantics, and rebalancing, which are outside this book's scope.

## 12.11 Testing and Tooling Implications

### 12.11.1 Testing Pipeline Behavior

Unit tests that process one item at a time through a stage are necessary but insufficient. The failure modes are about queueing, timing, and load interaction. You need:

**Saturation tests.** Push items faster than the pipeline can process them. Verify that admission control rejects excess work, queue depths stay bounded, and the system recovers when load drops. This requires either real concurrency or a test harness that simulates fast producers and slow consumers.

```cpp
// Test: verify backpressure prevents unbounded queue growth
void test_backpressure_under_overload() {
    BoundedChannel<int> ch(8);
    std::atomic<int> sent{0};
    std::atomic<int> received{0};

    // Slow consumer: 10ms per item
    std::jthread consumer([&](std::stop_token st) {
        while (auto item = ch.recv(st)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Fast producer: no delay
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        ch.send(i);
        sent.fetch_add(1, std::memory_order_relaxed);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    ch.close();
    consumer.join();

    // Producer must have been slowed by backpressure.
    // 100 items, consumer takes 10ms each, channel capacity 8,
    // so producer cannot stay far ahead.
    assert(elapsed > std::chrono::milliseconds(500));
    assert(received.load() == 100);
}
```

**Shutdown ordering tests.** Close the ingress channel and verify that every item already in the pipeline is processed and drained. Verify that stages shut down in order, that no thread hangs on a blocked send to a closed channel, and that destructors run cleanly.

**Deadline expiration tests.** Inject items with short deadlines and a slow processing stage. Verify that expired items are dropped and counted, not processed.

### 12.11.2 Tooling

**ThreadSanitizer (TSan).** Run saturation tests under TSan. Pipeline code is heavily concurrent, and the interaction between condition variables, atomics, and shared state is where data races hide. TSan will catch races on the `closed_` flag, unsynchronized access to metrics counters, and incorrect memory ordering on the in-flight counter.

**AddressSanitizer (ASan).** Catches use-after-move, buffer overflows in queue storage, and lifetime bugs where a stage holds a reference to a channel that has been destroyed.

**Profiling under load.** `perf` or VTune on a saturated pipeline reveals where time goes: mutex contention in channels, cache misses from deque node allocation, or system call overhead from condition variable wakes. This is where you decide whether to replace `std::deque` with a ring buffer or switch from condition variables to a futex-based channel.

**Latency histograms.** Log per-item latency (ingress timestamp to egress timestamp) and plot the distribution. Under healthy load, the histogram is tight. Under overload without backpressure, it develops a long tail. Under overload with backpressure and admission control, the tail is bounded but some items are rejected. The histogram shape tells you which regime you are in.

## 12.12 Review Checklist

When reviewing pipeline code, verify:

1. **Every inter-stage queue has a bounded capacity.** No `std::queue` without a size limit. No "we'll add a limit later."

2. **Backpressure reaches a point that can act on it.** Trace the path from the slowest stage back to the ingress. At the ingress, there must be a rejection, throttling, or shedding mechanism. If the ingress is a blocking `accept()` on a socket, backpressure stalls the network stack, which may be acceptable or catastrophic depending on the protocol and load balancer behavior.

3. **Admission control has a measured basis.** The maximum in-flight count or ingress rate limit should come from load testing, not from a round number someone picked during design review.

4. **Retry policies have deadlines and backoff.** No unbounded retry loops. Every retry loop checks a deadline and uses exponential backoff or similar. The total retry budget per request is bounded.

5. **Fan-out ratios are accounted for in downstream capacity.** If a stage can produce N items per input, the downstream channel and stage concurrency must handle N times the input rate.

6. **Expired work is dropped, not processed.** Items that have exceeded their deadline should be discarded with a metric increment, not processed to produce a response that nobody will read.

7. **Shutdown drains cleanly.** Closing the ingress channel propagates through the pipeline. Each stage drains its input before closing its output. No items are lost, and no threads hang.

8. **Queue depth, queueing delay, and rejection rate are instrumented.** If you cannot observe it, you cannot operate it. These metrics must exist before the system reaches production, not after the first incident.

9. **Stop tokens propagate through all blocking waits.** Every `wait`, `send`, and `recv` call that can block accepts a `std::stop_token`. Shutdown must not depend on closing channels in exactly the right order.

10. **Concurrency per stage has a justification.** CPU-bound stages scale with available cores. IO-bound stages need enough concurrency to hide latency, but not so much that they overwhelm downstream resources. The numbers should be documented and revisited when workload characteristics change.
