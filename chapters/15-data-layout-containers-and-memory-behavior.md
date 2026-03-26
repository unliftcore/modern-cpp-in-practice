# Data Layout, Containers, and Memory Behavior

## Production Problem

Many C++ performance failures are representation failures disguised as algorithm discussions. A team debates whether a lookup should be $O(1)$ or $O(\log n)$ while the real problem is that the hot path walks heap nodes, touches too many cache lines, or drags cold fields through every iteration. The compiler cannot optimize around a bad data shape. It can only make the chosen shape slightly less expensive.

This chapter is about the decisions that determine how data lives in memory: container selection, element layout, iteration order, invalidation behavior, and how views expose stored data without obscuring lifetime. The focus is not “which container is fastest” in the abstract. The question is narrower and more useful: what representation makes the dominant operations cheap in the system you are actually building?

Use realistic pressure when answering that question. Think about a request router with millions of route entries, a telemetry pipeline ingesting dense time-series samples, a game or simulation update loop scanning component state every frame, or an analytics job walking event batches. In those systems, container choice is not an implementation detail. It is part of the performance contract.

## When Big-O Stops Explaining Runtime

Asymptotic complexity is necessary and insufficient. It filters out obviously bad designs, but it does not describe memory traffic, branch predictability, prefetch behavior, false sharing, or the price of indirection. A `std::list` insert is constant time, but that fact is almost irrelevant if every useful operation also requires pointer chasing through cold memory. A sorted `std::vector` can beat a hash table for moderate sizes because contiguous binary search and cheap iteration often dominate nominal lookup complexity.

That mismatch matters because production workloads are rarely uniform. Some operations dominate the total cost. Others are latency-sensitive enough that a few extra cache misses matter more than one extra comparison. Before choosing a container, state the actual workload shape in plain language:

- Is the dominant operation full traversal, point lookup, append, erase in the middle, or batched rebuild?
- Is the data mostly read-only after construction, or constantly mutated?
- Do you care about stable addresses, stable iteration order, or predictable invalidation rules?
- Is the data set small enough to fit in private cache, or large enough that TLB and memory bandwidth behavior dominate?

If those answers are missing, container selection will drift toward folklore.

## Default Toward Contiguous Storage

For data that is traversed often, contiguous storage should be the default starting point. `std::vector`, `std::array`, `std::span`, `std::mdspan`, flat buffers, and columnar arrays win repeatedly because hardware rewards predictable access. Sequential scans let the processor prefetch effectively, amortize TLB work, and keep branch behavior simple. That advantage is often larger than the algorithmic edge of a theoretically “more advanced” structure.

The difference is not subtle. Consider summing values stored in a `std::vector<int>` versus a `std::list<int>` with the same element count:

```cpp
#include <list>
#include <vector>
#include <numeric>
#include <cstdint>

// Contiguous: hardware prefetcher has a good day.
std::int64_t sum_vector(const std::vector<int>& v) {
    return std::accumulate(v.begin(), v.end(), std::int64_t{0});
}

// Scattered: every node is a pointer chase.  Each dereference is
// a potential cache miss if nodes were allocated at different times
// and landed on different cache lines or pages.
std::int64_t sum_list(const std::list<int>& l) {
    return std::accumulate(l.begin(), l.end(), std::int64_t{0});
}
```

On typical hardware with one million elements, the vector version runs 10-50x faster than the list version for pure traversal. The list carries per-node overhead (two pointers per element on most implementations, plus allocator metadata), but the dominant cost is not space -- it is that advancing to the next node requires loading a pointer whose target address the prefetcher cannot predict. Every step risks a last-level-cache miss at 50-100 ns each, while the vector scan hits L1 or L2 almost every time because the hardware prefetcher recognizes the sequential pattern.

This is not a contrived worst case. It is the default behavior of `std::list` when nodes are allocated over time through a general-purpose allocator. Even `std::list` with a pool allocator that places nodes contiguously only partially recovers, because the next-pointer indirection and per-node overhead remain.

This is why many high-performance designs look boring at first glance. They store records in `std::vector`, sort once, then answer queries with binary search or batched scans. They keep hot data compact. They rebuild indexes in coarse batches instead of maintaining pointer-rich structures incrementally. They move work from random access toward regular access.

That does not mean `std::vector` is universally right. It means the burden of proof usually falls on the non-contiguous alternative. If a node-based or hash-based structure is required, the reason should be concrete: stable iterators across mutation, truly heavy mid-sequence insertion, concurrent ownership patterns, external handles that must remain valid, or lookup patterns that stay large and sparse enough for hashing to pay off.

## Containers Encode Tradeoffs, Not Just Operations

Experienced reviewers should read a container choice as a set of promises and costs.

`std::vector` promises contiguous storage, cheap append at the end, and efficient traversal. It charges you with occasional reallocation, iterator invalidation on growth, and expensive mid-sequence erase. It is often the right answer for batches, indexes, dense state, and tables that can be rebuilt or compacted.

`std::deque` relaxes contiguity to preserve cheaper growth at both ends and to avoid whole-buffer relocation. That can be valuable for queue-like workloads, but traversal locality is weaker than `std::vector`, and treating it as “basically a vector” is a mistake when the code is scan-heavy.

Ordered associative containers such as `std::map` and `std::set` buy stable ordering and stable references under many mutations, but they pay with node allocation, indirection, and branch-heavy traversal. They are justified when ordering is semantically required or when mutation patterns make rebuild-once strategies impossible. They are bad defaults for hot lookups on read-mostly data.

`std::unordered_map` and `std::unordered_set` trade ordering for average-case lookup speed. But they still carry a real memory cost: buckets, load factors, node storage in many implementations, and unpredictable probe behavior. They are valuable for large key spaces with frequent lookup by exact key. They are less compelling when iteration dominates, when memory footprint matters, or when the working set is small enough that sorted contiguous data stays in cache.

C++23 adds `std::flat_map` and `std::flat_set` (in `<flat_map>` and `<flat_set>`), which formalize what production codebases have done for years: a sorted contiguous key-value array that is frequently better for read-mostly indexes. Prior to C++23, teams relied on Boost.Container, Abseil, or hand-rolled equivalents. The standard versions accept underlying container template parameters, so you can back them with `std::vector` (the default), `std::deque`, or `std::pmr::vector` as locality and allocation needs dictate. Note that `std::flat_map` invalidates iterators on mutation just like `std::vector` does, and insertion into the middle is O(n) due to element shifting. It is a read-optimized structure, not a write-optimized one.

## Layout Inside the Element Matters As Much As the Container

Choosing `std::vector<Order>` is not enough. The shape of `Order` can still waste bandwidth. If every iteration reads `price`, `quantity`, and `timestamp`, but each object also carries a large symbol string, audit metadata, retry policy, and debugging state, then a scan over the vector is still dragging cold bytes through cache.

This is where hot/cold splitting matters. Keep the fields touched together in time physically close together. Move infrequently used state behind a separate table, side structure, or handle if it materially reduces scan cost. Do not over-abstract this into a generalized “entity system” unless the codebase really needs one. Often the right move is simpler: a compact hot record plus an auxiliary store for cold metadata.

The same pressure drives the array-of-structures versus structure-of-arrays decision. Array-of-structures is easier to reason about when objects move through the system as units. Structure-of-arrays wins when processing is columnar: filter all timestamps, compute on all prices, aggregate all counters, or feed vectorized kernels. The representation should match the dominant access pattern, not an imagined object model.

To see the tradeoff concretely, compare these two representations of the same data:

```cpp
// Array of Structures (AoS): natural object-oriented layout.
// Each Tick is self-contained.  Good when you routinely need all
// fields of a single tick (e.g., serializing one record, looking
// up a specific event).
struct Tick {
    std::int64_t timestamp_ns;
    std::int32_t instrument_id;
    double bid;
    double ask;
    char exchange[8];       // cold: rarely used in hot aggregation
    std::uint32_t seq_no;   // cold
    std::uint16_t flags;    // cold
    // sizeof(Tick) ~ 48 bytes with padding on most ABIs
};

double mid_price_sum_aos(std::span<const Tick> ticks) {
    double total = 0.0;
    for (const auto& t : ticks) {
        // Each iteration loads a full 48-byte Tick, but only
        // reads bid and ask (16 bytes).  The remaining 32 bytes
        // pollute cache lines and reduce effective bandwidth.
        total += (t.bid + t.ask) * 0.5;
    }
    return total;
}
```

```cpp
// Structure of Arrays (SoA): columnar layout.
// Each field lives in its own contiguous array.
struct TickColumns {
    std::vector<std::int64_t> timestamp_ns;
    std::vector<std::int32_t> instrument_id;
    std::vector<double> bid;
    std::vector<double> ask;
    std::vector<std::array<char, 8>> exchange;
    std::vector<std::uint32_t> seq_no;
    std::vector<std::uint16_t> flags;

    void append(std::int64_t ts, std::int32_t id, double b, double a) {
        timestamp_ns.push_back(ts);
        instrument_id.push_back(id);
        bid.push_back(b);
        ask.push_back(a);
        // ...other columns omitted for brevity
    }
};

double mid_price_sum_soa(const TickColumns& ticks) {
    double total = 0.0;
    // Only bid[] and ask[] are touched.  Each cache line is 100%
    // useful payload.  The compiler can auto-vectorize this loop,
    // and the prefetcher has two clean sequential streams.
    for (std::size_t i = 0; i != ticks.bid.size(); ++i) {
        total += (ticks.bid[i] + ticks.ask[i]) * 0.5;
    }
    return total;
}
```

With one million ticks, the SoA version typically runs 2-4x faster for columnar aggregation on modern x86 hardware. The reason is bandwidth efficiency: the AoS loop loads roughly 48 bytes per element but uses only 16, wasting two thirds of every cache line fetch. The SoA loop touches only the two 8-byte arrays it needs, and both are perfectly sequential. The compiler is also far more likely to emit SIMD instructions for the SoA version because there is no interleaving stride to complicate vectorization.

The cost of SoA appears elsewhere. Adding a new tick requires appending to every column vector, which is awkward and error-prone. Passing “one tick” to a function requires either an index plus a reference to the whole table, or a temporary struct assembled from columns. If the dominant operation is per-record processing that touches most fields, the AoS layout avoids that assembly cost and keeps related data together.

A practical middle ground is hot/cold splitting without full SoA: keep a compact “hot” struct with only the fields the hot path needs, and store cold fields in a parallel side table indexed by the same position.

This representation is not “more modern” by itself. It is better only if the workload repeatedly processes columns independently or if compact numeric columns materially improve memory behavior. If downstream logic constantly needs a full logical tick object with many correlated fields, the conversion cost or loss of clarity may erase the win.

## Stable Handles Are Expensive; Ask Whether You Really Need Them

Many poor representations originate in an unstated requirement for address stability. Code stores raw pointers or references into container elements, so the container choice becomes constrained by lifetime and invalidation concerns rather than by access cost. That often leads to node-based structures that preserve addresses at the price of worse locality everywhere else.

Sometimes that tradeoff is correct. More often the deeper problem is that the system has coupled identity to physical address. If components need durable external references, use explicit handles, indices with generation counters, or keys into a stable indirection layer. That lets the underlying storage stay compact and movable while keeping external references safe.

This is not free. Handles add lookup steps, validation work, and failure modes around stale references. But the cost is explicit and localized, which is usually preferable to poisoning the entire representation with pointer-stable containers just to satisfy a few long-lived aliases.

## Invalidation Rules Are Part of API Design

A container is not only a storage mechanism. It creates or destroys API guarantees. Returning `std::span<T>` into a `std::vector<T>` tells callers that the view is valid only while the underlying storage remains alive and unmodified in invalidating ways. Returning iterators into a hash table exposes rehash sensitivity. Returning references into a node container exposes object lifetime and synchronization assumptions.

That is why representation and interface design cannot be fully separated. If a module wants freedom to compact, sort, rebuild, or reallocate internally, it should not leak raw iterators or long-lived references casually. Prefer value results, short-lived callback-based access, copied summaries, or opaque handles when the implementation needs movement freedom.

Ranges make this cleaner but also easier to misuse. A view pipeline can look purely functional while still borrowing storage whose lifetime is narrower than the pipeline’s use. If the underlying data lives in a transient buffer, query object, or request-local arena, a beautifully composed range expression can still be a lifetime bug. The storage model remains primary.

## Dense Data Beats Clever Object Models on Hot Paths

Data-intensive systems often degrade when teams model the problem domain too literally. A log-processing stage becomes a graph of objects with virtual methods and scattered ownership because the domain sounds object-oriented. Under load, the profiler reports cache misses and allocator churn rather than expensive arithmetic.

For hot paths, prefer representations that make the dominant walk simple and dense. A packet classifier might store parsed header fields in packed arrays and keep only rare extension data elsewhere. A recommendation engine might separate immutable item features from request-local scoring buffers. An order book might keep price levels in contiguous arrays indexed by normalized tick offsets instead of trees of heap nodes if the price band is bounded enough.

These designs can look less elegant than an object graph. They are often more honest. Hardware executes memory traffic, not class diagrams.

## Cache-Hostile Structures in Practice

It is worth seeing exactly what a cache-hostile structure looks like and why it hurts, because the failure mode is common and invisible in code review.

```cpp
// A naive priority queue built from scattered heap nodes.
// Each node is individually allocated and linked by pointer.
struct Job {
    int priority;
    std::string description;  // may allocate on heap
    Job* next;
};

class NaivePriorityQueue {
    Job* head_ = nullptr;
public:
    void insert(int priority, std::string desc) {
        auto* node = new Job{priority, std::move(desc), nullptr};
        // Sorted insert: walk the list to find position.
        // Each step dereferences a pointer to a random heap location.
        Job** pos = &head_;
        while (*pos && (*pos)->priority <= priority)
            pos = &(*pos)->next;
        node->next = *pos;
        *pos = node;
    }

    // Find highest-priority job.  Cheap -- it is at the head.
    // But any operation that scans (e.g., "remove by description",
    // "count jobs above threshold") pointer-chases through
    // potentially thousands of cold cache lines.
    Job* top() const { return head_; }

    ~NaivePriorityQueue() {
        while (head_) { auto* n = head_; head_ = head_->next; delete n; }
    }
};
```

Compare with the cache-friendly alternative that stores the same data contiguously:

```cpp
struct Job {
    int priority;
    std::string description;
};

class FlatPriorityQueue {
    std::vector<Job> jobs_;
public:
    void insert(int priority, std::string desc) {
        jobs_.push_back({priority, std::move(desc)});
        // Could maintain sorted order with std::lower_bound + insert,
        // or just push_back and sort/partial_sort when needed.
    }

    // Rebuild top in O(n) but with contiguous memory access.
    // For scan-heavy workloads this dominates pointer-chasing.
    const Job& top() const {
        return *std::min_element(jobs_.begin(), jobs_.end(),
            [](const auto& a, const auto& b) {
                return a.priority < b.priority;
            });
    }
};
```

The flat version may perform more comparisons for `top()`, but it does so while streaming through contiguous memory. In practice, for collections under a few thousand elements, the flat scan often beats the linked version's O(1) head access combined with O(n) insert, because the insert in the linked version pays a cache miss per node visited. For larger collections, `std::priority_queue` (which wraps a contiguous heap in a `std::vector`) is the standard tool for exactly this reason.

The general lesson: pointer-linked structures pay a per-node tax on every traversal step that is invisible in algorithmic analysis but dominant in wall-clock time.

## Common Failure Modes

Several recurring mistakes are worth naming explicitly.

The first is choosing a container by operation cheat sheet rather than by measured workload shape. “Need fast lookup” is too vague. How many elements? What key distribution? How often do you iterate? How often do you rebuild? Without those numbers, “fast” has no content.

The second is mixing hot and cold fields because a single struct feels tidier. Compact layout is not premature optimization when the code traverses millions of elements per second.

The third is allowing incidental aliases to dictate representation. A few long-lived pointers should not force the entire system onto node-based storage if a handle layer would isolate the requirement.

The fourth is treating views as a lifetime abstraction. They are not. `std::span` and ranges make non-owning access explicit; they do not make non-owning access safe by themselves.

The fifth is overfitting to one microbenchmark. A representation that wins isolated lookup tests may lose badly in full pipelines where decoding, filtering, aggregation, and serialization interact.

## What to Verify in Real Code

Representation work should show up in code review and measurement plans, not just in implementation.

Reviewers should ask:

- Which operations dominate time and memory traffic?
- Does the chosen container optimize those operations or merely make one local call site convenient?
- Are stable addresses truly required, or is the code leaking representation constraints through aliases?
- Which invalidation rules are now part of the API contract?
- Are hot fields physically grouped together, or are scans dragging cold state through cache?
- If views are returned, what storage and mutation conditions bound their lifetime?

The next chapter turns those questions into a cost model. For now, the central point is simpler: representation decisions are often the first-order performance decision. Before discussing allocators, inlining, or benchmarking methodology, get the shape of the data right.

## Takeaways

- Start with the dominant access pattern, not with container folklore.
- Prefer contiguous storage by default for scan-heavy and read-mostly workloads.
- Treat stable addresses and stable iterators as expensive requirements that need justification.
- Separate hot and cold data when repeated traversal makes bandwidth the bottleneck.
- Use handles or indirection layers deliberately when identity must survive storage movement.
- Treat invalidation and view lifetime as API-level consequences of representation.