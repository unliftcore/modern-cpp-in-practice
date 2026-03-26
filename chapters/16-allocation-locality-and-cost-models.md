# Allocation, Locality, and Cost Models

## Production Problem

Once the data shape is reasonable, the next performance question is where the bytes come from and how often they move. Many teams jump from “this path is slow” straight to allocator tweaks or pool designs without first stating the cost model. That is backwards. Before changing allocation strategy, you need to know which costs dominate: allocation latency, synchronization in the allocator, page faults, cache and TLB misses from scattered objects, copy cost from oversized values, or instruction overhead from abstraction layers.

This chapter builds that model. The point is not to memorize allocator APIs. It is to reason concretely about what a design forces the machine to do. Claims such as “`std::function` is zero cost,” “arenas are always faster,” or “small allocations are cheap now” are not engineering arguments. The argument starts when you can identify the actual object graph, the number and timing of allocations, the ownership horizon of the objects involved, and the locality consequences of each layer of indirection.

Keep the boundary with the previous chapter sharp. Chapter 15 asked, “What should the representation be?” This chapter asks, “Given a representation, what costs does it impose over time?” Containers still appear here, but the emphasis is allocation frequency, lifetime clustering, object graph depth, and locality, not container semantics.

## Start With an Allocation Inventory

The first useful performance model is embarrassingly simple: list what allocates on the path you care about.

Most codebases are worse at this than they think. A request parser allocates strings for every header value, a routing layer stores callbacks in type-erased wrappers, a JSON transformation materializes intermediate objects, and a logging path formats into temporary buffers. Each decision may be locally reasonable. Together they create a request that performs dozens or hundreds of allocations before any real business work begins.

The example project's `parse_request()` in `examples/web-api/src/modules/http.cppm` illustrates this pattern concretely. Each call allocates a `std::string` for every header name and value (line 191: `headers.emplace_back(std::string(name), std::string(value))`), plus a `std::string` for the path and the body. For a request with ten headers, that is at least twelve heap allocations before any handler runs. This is a natural candidate for PMR optimization: a `std::pmr::monotonic_buffer_resource` backed by a stack buffer could supply all of those strings from a single arena, eliminating per-header allocator calls entirely and making teardown a bulk operation when the request scope ends.

Inventory work should separate three questions:

- Which operations allocate on the steady-state hot path?
- Which allocations are one-time setup or batch rebuild costs?
- Which allocations are avoidable with different ownership or data flow, rather than with a better allocator?

That last question matters most. If a system allocates because it insistently decomposes dense processing into many short-lived heap objects, changing the allocator may reduce pain without fixing the design. The highest-leverage change is often to stop needing the allocations.

Here is a concrete example of what allocation-heavy code looks like on a hot path, and what the alternative can be:

```cpp
// Allocation-heavy: every event creates a temporary string,
// a vector, and a map entry.  Under load this path may perform
// 5-10 heap allocations per event.
struct Event {
    std::string type;
    std::string payload;
    std::vector<std::string> tags;
    std::unordered_map<std::string, std::string> metadata;
};

void process_batch_heavy(std::span<const RawEvent> raw,
                         std::vector<Event>& out) {
    for (const auto& r : raw) {
        Event e;
        e.type = parse_type(r);         // allocates
        e.payload = parse_payload(r);   // allocates
        e.tags = parse_tags(r);         // allocates vector + each string
        e.metadata = parse_meta(r);     // allocates map buckets + nodes
        out.push_back(std::move(e));    // may reallocate out's buffer
    }
}
```

```cpp
// Allocation-light: pre-sized arena, string views into stable
// input buffer, fixed-capacity inline storage.
struct EventView {
    std::string_view type;
    std::string_view payload;
    // Use a small fixed-capacity container for tags.
    // boost::static_vector or a similar stack-allocated small vector.
    std::array<std::string_view, 8> tags;
    std::uint8_t tag_count = 0;
};

void process_batch_light(std::string_view input_buffer,
                         std::span<const RawEvent> raw,
                         std::vector<EventView>& out) {
    out.clear();
    out.reserve(raw.size());  // one allocation, amortized
    for (const auto& r : raw) {
        EventView e;
        e.type = parse_type_view(r, input_buffer);
        e.payload = parse_payload_view(r, input_buffer);
        e.tag_count = parse_tags_view(r, input_buffer, e.tags);
        out.push_back(e);
    }
    // Zero heap allocations per event if input_buffer is stable
    // and out has sufficient capacity.
}
```

The light version imposes constraints: the input buffer must outlive the views, tags are bounded, and metadata is handled differently. Those constraints are the cost of avoiding allocations. Whether that cost is acceptable depends on the workload, but making it visible is the point.

## Allocation Cost Is More Than the Call to `new`

Engineers sometimes talk about allocation as though the only cost were the allocator function call. In production, that is usually a minority of the bill. Allocation also affects cache locality, synchronization behavior, fragmentation, page working set, and destruction cost later. If an object graph spreads logically adjacent data across unrelated heap locations, every later traversal pays for that decision. If per-request allocations hit a shared global allocator from many threads, allocator contention becomes part of latency variance. If many short-lived objects are destroyed individually, cleanup traffic can dominate tail latency during bursts.

This is why “we pooled it, so the problem is solved” is often false. A pool may reduce allocator call overhead and even reduce contention, but if the resulting object graph is still pointer-heavy and scattered, traversal remains expensive. Conversely, a design that stores request-local state in contiguous buffers may perform very few allocations and enjoy better locality even with the default allocator.

## Lifetime Clustering Usually Beats Clever Reuse

Objects that die together should often be allocated together. This is the core intuition behind arenas and monotonic resources: if a batch of data shares a lifetime boundary, paying for individual deallocation is wasted work. Request-local parse trees, temporary token buffers, graph search scratch state, and one-shot compilation metadata are classic examples.

C++23 still relies on `std::pmr` for the standard vocabulary here. The value is not stylistic. It is architectural. Memory resources let you express that a family of objects belongs to a shared lifetime region without hard-wiring a custom allocator type through every template instantiation.

```cpp
struct RequestScratch {
    std::pmr::monotonic_buffer_resource arena;
    std::pmr::vector<std::pmr::string> tokens{&arena};
    std::pmr::unordered_map<std::pmr::string, std::pmr::string> headers{&arena};

    explicit RequestScratch(std::span<std::byte> buffer)
        : arena(buffer.data(), buffer.size()) {}
};
```

This design says something important: the strings and containers are not independent heap citizens. They are request-scoped scratch. That reduces allocation overhead and makes teardown a bulk operation.

A more complete example shows the difference in practice. Compare standard allocation versus `pmr` with a stack-local buffer for a request-processing path:

```cpp
#include <memory_resource>
#include <vector>
#include <string>
#include <array>

// Standard allocation: every string, every vector growth, and the
// map internals go through the global allocator.  Under contention
// from many threads, this serializes on allocator locks.
void handle_request_standard(std::span<const std::byte> input) {
    std::vector<std::string> tokens;
    std::unordered_map<std::string, std::string> headers;
    parse(input, tokens, headers);  // many small allocations
    route(tokens, headers);
    // Destruction: each string freed individually, each map node freed.
}

// PMR with stack buffer: small requests never touch the heap.
// The monotonic_buffer_resource first allocates from the stack buffer.
// If the request is large enough to exhaust it, it falls back to
// the upstream resource (default: new/delete).
void handle_request_pmr(std::span<const std::byte> input) {
    std::array<std::byte, 4096> stack_buf;
    std::pmr::monotonic_buffer_resource arena{
        stack_buf.data(), stack_buf.size(),
        std::pmr::null_memory_resource()
        // null_memory_resource: fail loudly if buffer is exceeded.
        // Replace with std::pmr::new_delete_resource() to allow
        // fallback to heap for oversized requests.
    };

    std::pmr::vector<std::pmr::string> tokens{&arena};
    std::pmr::unordered_map<std::pmr::string, std::pmr::string>
        headers{&arena};
    parse_pmr(input, tokens, headers);
    route_pmr(tokens, headers);
    // Destruction: arena destructor releases everything in one shot.
    // No per-string, per-node deallocation calls.
}
```

The pmr version eliminates all per-object deallocation calls and avoids global allocator contention entirely for requests that fit within the stack buffer. On a high-throughput server handling small requests, this can reduce allocator overhead by an order of magnitude. The tradeoff is that `std::pmr` containers carry an extra pointer to the memory resource (increasing `sizeof` slightly) and that the monotonic resource does not reclaim memory from individual deallocations -- it only grows until the resource itself is destroyed. This is fine for request-scoped scratch; it is wrong for long-lived containers that grow and shrink over time.

But monotonic allocation is not a universal upgrade. It is a bad fit when objects need selective deallocation, when memory spikes from one pathological request must not bloat the steady-state footprint, or when accidentally retaining a single object would retain an entire arena. Regional allocation sharpens lifetime assumptions. If the assumptions are wrong, the failure is bigger than with individual ownership.

## Locality Is About Graph Shape, Not Just Raw Bytes

It is possible to have a low allocation count and still have terrible locality. A handful of large allocations containing arrays of pointers to separately allocated nodes can be worse than many small allocations if traversal constantly bounces between pages. The cost model therefore needs one more question: when the hot code walks this structure, how many pointer dereferences does it perform before it reaches useful payload?

Pointer-rich designs are often semantically attractive because they mirror domain relationships directly. Trees point to children. Polymorphic objects point to implementations. Pipelines store chains of heap-allocated stages. Sometimes that is unavoidable. Often it is laziness disguised as modeling.

The cure is not “never use pointers.” The cure is to distinguish identity and topology from storage. A graph can be stored in contiguous node arrays with index-based adjacency. A polymorphic pipeline can often be represented as a small closed `std::variant` of step types when the set of operations is known. A string-heavy parser can intern repeated tokens or keep slices into a stable input buffer rather than allocating owned strings for every field.

Those are not language-trick optimizations. They are graph-shape decisions. They reduce the amount of memory chasing required before useful work begins.

## The Hidden Cost of `std::shared_ptr`

`std::shared_ptr` deserves special attention because its costs are frequently underestimated. The allocation cost is the most visible: `std::make_shared` performs one allocation for the control block and the managed object together, while constructing from a raw pointer performs two. But allocation is only the beginning.

The deeper cost is reference counting. Every copy of a `std::shared_ptr` performs an atomic increment; every destruction performs an atomic decrement with acquire-release semantics. On x86, an atomic increment is relatively cheap (a locked instruction, roughly 10-20 ns under no contention), but under cross-core sharing, the cache line holding the control block bounces between cores. Under heavy contention, this serializes otherwise parallel work.

```cpp
// Looks innocent: passing shared_ptr by value into a thread pool.
// Each enqueue copies the shared_ptr (atomic increment), and each
// task completion destroys it (atomic decrement + potential dealloc).
void submit_work(std::shared_ptr<Config> cfg,
                 ThreadPool& pool,
                 std::span<const Request> requests) {
    for (const auto& req : requests) {
        // Copies cfg: atomic ref-count increment per task.
        pool.enqueue([cfg, &req] {
            handle(req, *cfg);
        });
    }
    // If 10,000 requests are enqueued, that is 10,000 atomic
    // increments on submission and 10,000 atomic decrements
    // on completion, all contending on the same cache line.
}
```

```cpp
// Fix: cfg outlives all tasks, so pass a raw pointer or reference.
void submit_work_fixed(const Config& cfg,
                       ThreadPool& pool,
                       std::span<const Request> requests) {
    for (const auto& req : requests) {
        pool.enqueue([&cfg, &req] {
            handle(req, cfg);
        });
    }
    // Zero reference-counting overhead.  Caller guarantees
    // cfg lives until all tasks complete.
}
```

The rule is not "never use `std::shared_ptr`." It is: do not use shared ownership to avoid thinking about lifetime. When an object has a clear owner and borrowers, express that with a unique owner and references or views. Reserve `std::shared_ptr` for genuinely shared, non-deterministic lifetimes. And never pass `std::shared_ptr` by value when a `const&` or raw reference suffices -- each copy is an atomic round-trip you are paying for nothing.

The example project demonstrates this principle in practice. In `examples/web-api/src/modules/handlers.cppm`, every handler factory (e.g., `list_tasks()`, `get_task()`, `create_task()`) takes `TaskRepository&` by reference and captures it by reference in the returned lambda. The repository is owned by `main()` and outlives all handlers, so there is no need for `std::shared_ptr<TaskRepository>`. This avoids atomic reference-count traffic on every request and keeps the handler's capture small -- a single pointer rather than a two-pointer-wide `shared_ptr` plus its control block.

Additional costs that accumulate: `std::shared_ptr` is two pointers wide (pointer to object + pointer to control block), doubling the size of a raw pointer. Containers of `std::shared_ptr` therefore have worse cache density. The weak reference count adds another atomic variable. And custom deleters stored in the control block add type-erased indirection at destruction time.

## Hidden Allocation Is a Design Smell

Modern C++ provides abstractions that are appropriate only when their cost model remains visible enough for the code under review. The problem is not abstraction itself. The problem is abstraction whose allocation behavior is implicit, workload-dependent, or implementation-defined in ways the team ignores.

`std::string` may allocate or may fit in a small-string buffer. `std::function` may allocate for larger callables and may not for smaller ones. Type-erased wrappers, coroutine frames, regex engines, locale-aware formatting, and stream-based composition can all allocate in ways that disappear from the immediate call site.

None of these types are forbidden. They become dangerous when used on hot paths without explicit evidence. If a service constructs a `std::function` per message, or repeatedly turns stable string slices into owned `std::string` objects because downstream APIs demand ownership by default, the real issue is not just “too many allocations.” It is that the API surface obscures where cost enters.

Review hot-path abstractions with the same seriousness you apply to thread synchronization. Ask:

- Can this wrapper allocate?
- Can it force an extra indirection?
- Can it enlarge object size enough to reduce packing density?
- Can the same behavior be expressed with a closed alternative such as `std::variant`, a template boundary, or a borrowed view?

The right answer depends on code size, ABI, compile times, and substitution flexibility. The point is to make the trade explicit.

## Pools, Freelist Reuse, and Their Failure Modes

Pooling is attractive because it offers a story of reuse and predictability. Sometimes that story is true. Fixed-size object pools can help when allocation size is uniform, object lifetime is short, reuse is heavy, and allocator contention matters. Slab-like designs can also improve spatial locality relative to fully general heap allocation.

But pools fail in repeatable ways.

They fail when object sizes vary enough that multiple pools or internal fragmentation erase the gain. They fail when a pool hides unbounded retention because objects are “reused later” but rarely enough that memory never returns to the system. They fail when per-thread pools complicate balancing under skewed workloads. They fail when code starts encoding lifetime around pool availability instead of around domain ownership. And they fail when developers stop measuring because the existence of a pool feels like optimization.

The operational rule is blunt: use pooling to support a known workload shape, not as a generic performance gesture. If you cannot describe the allocation distribution and reuse pattern, you are not ready to design the pool.

## Value Size and Parameter Surfaces Still Matter

Allocation is only part of the cost model. Large value types copied casually through APIs can be just as destructive. A “convenient” record type that embeds several `std::string` members, optional blobs, and vectors may avoid some heap traffic by using move semantics, but it still enlarges working-set size, increases cache pressure, and makes pass-by-value choices more expensive.

This is where Chapter 4’s API guidance re-enters the picture. Passing by value is excellent when ownership transfer or cheap move is the actual contract. It is poor when a path repeatedly copies or moves large aggregate objects merely to satisfy generic convenience. A cost model must include object size, move cost, and how often data crosses layer boundaries.

Small values are easier to move through the system. Large values often want stable storage plus borrowed access, summary extraction, or decomposition into hot and cold parts. If those options complicate the API, that complication may be justified. Performance design is full of cases where cleaner interfaces at one layer create avoidable cost everywhere else.

## A Practical Cost Model for Review

For production work, an informal but explicit model is usually enough. For the path under review, write down:

- Number of steady-state allocations per operation.
- Whether those allocations are thread-local, globally contended, or hidden behind abstractions.
- The lifetime grouping of allocated objects.
- Number of pointer indirections on the hot traversal path.
- Approximate hot working-set size.
- Whether traversal is contiguous, strided, hashed, or graph-like.
- Whether destruction is individual, batched, or region-based.

That list will not yield a cycle-accurate prediction. It will stop hand-waving. It lets reviewers distinguish “this feels costly” from “this design guarantees allocator traffic, scattered reads, and poor teardown behavior under burst load.”

## Boundary Conditions

A cost model is not a license to over-specialize everything. Sometimes a heap allocation is correct because the object truly outlives local scopes and participates in shared ownership. Sometimes type erasure is the right trade for substitution across library boundaries. Sometimes arena allocation is inappropriate because retention risk or debugging complexity outweighs the throughput gain.

The goal is not maximal local speed. The goal is predictable, explainable cost under real system pressure. If a design slightly increases steady-state cost while dramatically improving correctness or evolvability at a non-hot boundary, that can be the right call. Cost models exist to support tradeoffs, not to abolish them.

## What to Verify Before You Tune

Before introducing custom allocators, pooling, or broad `pmr` plumbing, verify four things.

First, confirm the path is hot enough that allocation and locality matter materially. Second, confirm the current design actually allocates or scatters data in the way you think it does. Third, confirm the objects involved share the lifetime shape your proposed allocator strategy assumes. Fourth, confirm the new design does not simply move cost elsewhere, such as larger retained memory, worse debugging ergonomics, or more complex ownership boundaries.

The next chapter addresses the evidence side directly. Cost models are hypotheses. They become engineering only when benchmarking and profiling test the right hypothesis.

## Takeaways

- Start with an allocation inventory before reaching for allocator techniques.
- Treat allocation cost as latency, locality, contention, retention, and teardown cost, not just the price of `new`.
- Cluster objects by lifetime when their destruction boundary is genuinely shared.
- Use `std::pmr` to express regional memory strategy when it matches ownership, not as decorative modernity.
- Be suspicious of abstractions whose allocation and indirection behavior is hidden on hot paths.
- Design pools for a measured workload shape or not at all.