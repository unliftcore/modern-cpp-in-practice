# Allocation, Locality, and Cost Models

## Production Problem

Once the data shape is reasonable, the next performance question is where the bytes come from and how often they move. Many teams jump from “this path is slow” straight to allocator tweaks or pool designs without first stating the cost model. That is backwards. Before changing allocation strategy, you need to know which costs dominate: allocation latency, synchronization in the allocator, page faults, cache and TLB misses from scattered objects, copy cost from oversized values, or instruction overhead from abstraction layers.

This chapter builds that model. The point is not to memorize allocator APIs. It is to reason concretely about what a design forces the machine to do. Claims such as “`std::function` is zero cost,” “arenas are always faster,” or “small allocations are cheap now” are not engineering arguments. The argument starts when you can identify the actual object graph, the number and timing of allocations, the ownership horizon of the objects involved, and the locality consequences of each layer of indirection.

Keep the boundary with the previous chapter sharp. Chapter 15 asked, “What should the representation be?” This chapter asks, “Given a representation, what costs does it impose over time?” Containers still appear here, but the emphasis is allocation frequency, lifetime clustering, object graph depth, and locality, not container semantics.

## Start With an Allocation Inventory

The first useful performance model is embarrassingly simple: list what allocates on the path you care about.

Most codebases are worse at this than they think. A request parser allocates strings for every header value, a routing layer stores callbacks in type-erased wrappers, a JSON transformation materializes intermediate objects, and a logging path formats into temporary buffers. Each decision may be locally reasonable. Together they create a request that performs dozens or hundreds of allocations before any real business work begins.

Inventory work should separate three questions:

- Which operations allocate on the steady-state hot path?
- Which allocations are one-time setup or batch rebuild costs?
- Which allocations are avoidable with different ownership or data flow, rather than with a better allocator?

That last question matters most. If a system allocates because it insistently decomposes dense processing into many short-lived heap objects, changing the allocator may reduce pain without fixing the design. The highest-leverage change is often to stop needing the allocations.

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

But monotonic allocation is not a universal upgrade. It is a bad fit when objects need selective deallocation, when memory spikes from one pathological request must not bloat the steady-state footprint, or when accidentally retaining a single object would retain an entire arena. Regional allocation sharpens lifetime assumptions. If the assumptions are wrong, the failure is bigger than with individual ownership.

## Locality Is About Graph Shape, Not Just Raw Bytes

It is possible to have a low allocation count and still have terrible locality. A handful of large allocations containing arrays of pointers to separately allocated nodes can be worse than many small allocations if traversal constantly bounces between pages. The cost model therefore needs one more question: when the hot code walks this structure, how many pointer dereferences does it perform before it reaches useful payload?

Pointer-rich designs are often semantically attractive because they mirror domain relationships directly. Trees point to children. Polymorphic objects point to implementations. Pipelines store chains of heap-allocated stages. Sometimes that is unavoidable. Often it is laziness disguised as modeling.

The cure is not “never use pointers.” The cure is to distinguish identity and topology from storage. A graph can be stored in contiguous node arrays with index-based adjacency. A polymorphic pipeline can often be represented as a small closed `std::variant` of step types when the set of operations is known. A string-heavy parser can intern repeated tokens or keep slices into a stable input buffer rather than allocating owned strings for every field.

Those are not language-trick optimizations. They are graph-shape decisions. They reduce the amount of memory chasing required before useful work begins.

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