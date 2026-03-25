# Chapter 14: Cost Models, Allocation, and Locality

*Prerequisites: Chapter 13 (the data layout choices whose costs this chapter quantifies).*

Performance work in C++ is often derailed by vague claims — "zero-cost abstraction," "fast enough," "it's just a pointer dereference" — that never become a concrete model of where time and memory actually go. This chapter builds that model. Where Chapter 13 asks which data structure fits the workload, this chapter asks how to verify the assumption: what does the allocator actually do, where does the cache miss, and which abstraction is paying for itself versus pushing cost into hotter paths.

The goal is not optimization. It is disciplined reasoning *before* optimization and clearer tradeoffs when optimization becomes necessary.

> **Prerequisites:** You should be comfortable with the data layout and container selection material from Chapter 13. Familiarity with basic computer architecture — cache hierarchies, virtual memory pages, branch prediction — is helpful but not required; this chapter introduces those concepts at the level needed for cost reasoning. Chapter 15 covers measurement in depth; this chapter focuses on building the mental model that tells you *what* to measure and *why*.

## 14.1 The Production Problem

A trading system processes market-data ticks through a pipeline of normalizers, risk checks, and order generators. Under synthetic benchmarks the pipeline handles 800,000 messages per second. In production, throughput collapses to 200,000 during opening auction when message diversity spikes. Profiling reveals no single hot function — the flamegraph is flat. The team suspects "memory," but cannot articulate what that means.

The problem is not a missing optimization. It is a missing cost model. Nobody can answer three questions:

1. How many allocations occur per message, and what sizes?
2. How many cache lines does processing one message touch, and are they likely to be resident?
3. Which indirections are structural (necessary for the design) and which are accidental (artifacts of convenience types)?

Without those answers, every "optimization" is guesswork. This chapter provides the vocabulary and techniques to answer them.

## 14.2 Naive and Legacy Approaches

### 14.2.1 Optimization by folklore

Teams without a cost model fall back on received wisdom: "avoid `std::map`," "use `reserve()`," "pass by `const&`." These heuristics are not wrong, but they are imprecise. Calling `reserve()` on a vector that already fits in the small-buffer range wastes a branch. Avoiding `std::map` for 12 elements may cost more in code complexity than it saves in cache misses. Folklore substitutes pattern-matching for analysis.

### 14.2.2 Premature micro-optimization

The inverse failure: rewriting inner loops in SIMD intrinsics while the outer loop allocates a `std::string` per iteration through `std::format`. The optimization is locally impressive and globally irrelevant. Without a cost model that accounts for the full path, effort lands in the wrong place.

### 14.2.3 Treating the profiler as an oracle

A profiler tells you where time *was spent*, not where time *was wasted*. Flat profiles — where no single function dominates — are common in allocation-heavy or cache-hostile code because the cost is spread across thousands of call sites. A cost model lets you interpret the profiler output; it does not replace it, but without it the profiler output is noise.

## 14.3 Building a Cost Model

A cost model is a set of approximate answers to the question: "what does this operation actually cost on this hardware, at this scale, under this access pattern?" It does not need to be precise. It needs to be *directional* — accurate enough to distinguish a 10ns operation from a 100ns one and a 100ns one from a 10μs one.

### 14.3.1 The Memory Hierarchy as a Cost Table

Rough order-of-magnitude latencies on contemporary server hardware (2024-era x86-64):

| Operation | Approximate latency |
|---|---|
| L1 cache hit | 1–2 ns |
| L2 cache hit | 4–7 ns |
| L3 cache hit | 10–30 ns |
| DRAM access (cache miss) | 50–100 ns (nominal; effective latency depends on CPU frequency and memory topology) |
| Page fault (minor, page in RAM) | 1–10 μs |
| Page fault (major, page on disk/swap) | 1–10 ms |
| `malloc` (tcmalloc/jemalloc, hot path) | 15–30 ns |
| `malloc` (cold path, mmap) | 1–10 μs |
| System call (minimal, e.g., `clock_gettime` vDSO) | 10–20 ns |
| System call (context switch) | 1–5 μs |

These numbers are not constants — they depend on hardware, contention, and state. Their value is relative: a cache miss costs 50–100× an L1 hit. An allocation that falls off the fast path costs 100× one that stays on it. A cost model anchored to these ratios catches design mistakes that micro-benchmarks miss.

### 14.3.2 Allocation Costs

Every `new`, every `std::make_unique`, every `std::string` that exceeds SSO, every `std::vector::push_back` that triggers growth — all of these invoke the allocator. The cost is not the bookkeeping alone. It is the combination of:

1. **Allocator work**: lock acquisition (in older allocators), free-list traversal, size-class lookup, possible `mmap`/`sbrk`.
2. **TLB pressure**: new pages mean new TLB entries; running out of TLB slots causes page-table walks.
3. **Fragmentation over time**: small allocations scattered across pages destroy spatial locality for later traversal.
4. **Deallocation cost**: `free` is not free — it may coalesce, unmap, or cross thread boundaries (returning memory to another thread's cache in tcmalloc).

#### Counting allocations

Before reasoning about allocation cost, count them. A lightweight approach uses allocator interposition:

```cpp
// allocation_counter.cpp — LD_PRELOAD or link-time interposition
#include <atomic>
#include <cstdlib>
#include <cstdio>

namespace {
    std::atomic<std::size_t> alloc_count{0};
    std::atomic<std::size_t> alloc_bytes{0};
    std::atomic<std::size_t> free_count{0};
}

extern "C" void* malloc(std::size_t size) {
    static auto* real_malloc =
        reinterpret_cast<void*(*)(std::size_t)>(dlsym(RTLD_NEXT, "malloc"));
    alloc_count.fetch_add(1, std::memory_order_relaxed);
    alloc_bytes.fetch_add(size, std::memory_order_relaxed);
    return real_malloc(size);
}

extern "C" void free(void* ptr) {
    static auto* real_free =
        reinterpret_cast<void(*)(void*)>(dlsym(RTLD_NEXT, "free"));
    if (ptr) free_count.fetch_add(1, std::memory_order_relaxed);
    real_free(ptr);
}

void print_allocation_stats() {
    std::fprintf(stderr, "allocations: %zu  bytes: %zu  frees: %zu\n",
                 alloc_count.load(), alloc_bytes.load(), free_count.load());
}
```

On Linux, `LD_PRELOAD=./liballoc_counter.so ./my_service` gives you per-run totals without recompilation. On MSVC, equivalent instrumentation uses the CRT debug heap (`_CrtSetAllocHook`) or ETW tracing. The goal is a single number: allocations per logical operation. If processing one market-data tick requires 47 allocations, you know where to look before you profile.

#### Anti-pattern: allocation in a hot loop

```cpp
// Anti-pattern: hidden allocation per element
std::vector<std::string> normalize_ticks(std::span<const RawTick> ticks) {
    std::vector<std::string> results;
    for (auto const& tick : ticks) {
        // BUG: std::format returns std::string — allocates if result > SSO
        results.push_back(std::format("{}/{}/{:.4f}",
                                       tick.exchange, tick.symbol, tick.price));
    }
    return results;
}
```

Each `std::format` call may allocate. Each `push_back` may reallocate the vector. For 10,000 ticks, this is potentially 20,000 allocations. The fix depends on what the caller needs — if the strings are consumed immediately, a single pre-sized `std::string` buffer rewritten per iteration eliminates all of them:

```cpp
void process_ticks(std::span<const RawTick> ticks,
                   std::function<void(std::string_view)> sink) {
    std::string buf;
    buf.reserve(64);  // typical max length
    for (auto const& tick : ticks) {
        buf.clear();  // does not deallocate
        std::format_to(std::back_inserter(buf), "{}/{}/{:.4f}",
                       tick.exchange, tick.symbol, tick.price);
        sink(buf);
    }
}
```

Allocations per tick: zero (amortized), assuming the reserve covers the typical case. The cost model makes the improvement obvious; the profiler alone would show time spread across `malloc`, `free`, and `memcpy` with no single dominant site.

### 14.3.3 Indirection and Pointer-Chasing

Every pointer dereference is a potential cache miss. A single cache miss costs 50–100 ns; a chain of dependent pointer dereferences — where the address of the next load depends on the value of the previous load — serializes those misses because the CPU cannot prefetch what it cannot predict.

The classic example is a linked list, but production code produces the same pattern through less obvious means:

- `std::map<K, V>` — each lookup follows a chain of node pointers
- `std::vector<std::unique_ptr<Base>>` — the vector body is contiguous, but each element requires a dereference to reach the object
- `std::unordered_map<K, V>` — bucket chains are pointer-linked in most implementations
- `std::function<R(Args...)>` — heap-allocated callable if it exceeds the small-buffer optimization

#### Quantifying indirection

Count the number of dependent dereferences between the start of an operation and the completion of useful work. This is the **pointer-chase depth**. For a `std::map` lookup with N elements, the depth is O(log N) — around 20 dereferences for a million elements, each potentially a cache miss. For a sorted `std::vector` with binary search, the depth is also O(log N), but the data is contiguous, so prefetching covers most of it. The algorithmic complexity is the same; the constant factor differs by 10–50×.

#### Measuring cache behavior

Hardware performance counters expose cache miss rates directly. On Linux:

```bash
perf stat -e cache-references,cache-misses,L1-dcache-load-misses,LLC-load-misses \
    ./my_service --benchmark-mode
```

On Windows, use `xperf` (Windows Performance Toolkit) or Intel VTune. The metrics that matter:

- **L1-dcache-load-misses**: how often the innermost data cache fails. High rates indicate poor spatial locality.
- **LLC-load-misses**: last-level cache misses — these go to DRAM and are the most expensive.
- **Instructions per cycle (IPC)**: low IPC (below 1.0 on modern hardware) often indicates memory stalls.

A cost model predicts which operations should be cache-friendly and which should not. The counters validate or refute the prediction.

### 14.3.4 Copying and Move Costs

`std::move` does not move anything. It casts to an rvalue reference. The actual work depends on the type's move constructor. For types that own heap memory (`std::vector`, `std::string`, `std::unique_ptr`), a move is a pointer swap — essentially free. For types that carry inline data (`std::array<double, 1024>`, a struct with 200 bytes of value fields), a move is a copy. The cost model must distinguish these cases.

```cpp
struct SmallConfig {
    std::array<char, 32> name;
    std::uint32_t flags;
    double threshold;
};
// sizeof(SmallConfig) ≈ 48 bytes — move == copy, and that's fine

struct LargeState {
    std::vector<double> prices;        // move: pointer swap
    std::unordered_map<int, Order> orders; // move: pointer swap
    std::string description;           // move: pointer swap (or SSO copy)
};
// Move of LargeState: ~3 pointer swaps, regardless of element count
```

#### Anti-pattern: accidental copies in range-for

```cpp
// Anti-pattern: copies every element
void audit(std::vector<LargeState> const& states) {
    for (auto state : states) {  // RISK: copies each LargeState
        log_audit(state.description);
    }
}
```

The fix (`auto const&` or `auto const&` with structured bindings) is well-known. The cost model makes the mistake quantifiable: if `states` has 10,000 elements and each `LargeState` owns 1 MB of heap data, the loop copies 10 GB. The compiler will not warn. The profiler will show time in copy constructors and `malloc`, but the root cause is a missing `&`.

### 14.3.5 Branch Behavior and Representation Choices

Modern CPUs predict branches with >95% accuracy on well-behaved patterns. The cost of a mispredicted branch is 10–20 ns (pipeline flush). This matters when:

- Virtual dispatch on a polymorphic collection with many concrete types defeats branch prediction because the indirect-call target is unpredictable.
- `std::variant` visitation with `std::visit` compiles to a jump table — predictable if the active alternative is stable, unpredictable if it rotates.
- Conditional branches inside tight loops that depend on data values (e.g., filtering) can stall if the data is not sorted or partitioned.

#### Quantifying branch cost

```bash
perf stat -e branch-instructions,branch-misses ./my_service --benchmark-mode
```

A branch miss rate above 2–3% on a hot path is worth investigating. If `std::visit` on a variant alternates between 8 types uniformly, expect ~87% miss rate on the indirect jump (7 of 8 predictions wrong). If the variant almost always holds one type, the miss rate drops to near zero. The representation choice — variant versus type-erased pointer versus separate containers per type — depends on the distribution, not the syntax.

### 14.3.6 The `pmr` Allocator Model

`std::pmr` (polymorphic memory resources) gives you allocator control without infecting every template parameter. The key resources for cost modeling:

- `std::pmr::monotonic_buffer_resource` — bump-pointer allocation from a pre-sized buffer. Allocation cost: ~2 ns (pointer increment). Deallocation cost: zero (no individual frees; the entire buffer is released at once). Useful for request-scoped or frame-scoped work where all allocations share a lifetime.
- `std::pmr::unsynchronized_pool_resource` — pool allocator without thread safety. Faster than `malloc` for small allocations in single-threaded contexts.
- `std::pmr::synchronized_pool_resource` — pool allocator with internal synchronization. Appropriate for shared contexts but slower than the unsynchronized variant.

```cpp
#include <memory_resource>
#include <vector>

void process_batch(std::span<const RawTick> ticks) {
    // Stack buffer for small batches; falls back to default for large ones
    std::array<std::byte, 16384> buf;
    std::pmr::monotonic_buffer_resource arena(buf.data(), buf.size());

    std::pmr::vector<NormalizedTick> normalized(&arena);
    normalized.reserve(ticks.size());

    for (auto const& tick : ticks) {
        normalized.push_back(normalize(tick));
    }

    // All allocations freed when arena goes out of scope — no per-element free
    dispatch(normalized);
}
```

The cost model comparison:

| Approach | Allocs per batch (N ticks) | Approx. cost |
|---|---|---|
| `std::vector` + default allocator | 1–O(log N) reallocations | 15–30 ns each |
| `std::pmr::vector` + monotonic arena | 0 (pre-reserved) | ~0 ns marginal |

The tradeoff: monotonic arenas cannot reclaim memory until the entire resource is destroyed. If the batch processing holds the arena open while waiting on I/O, memory usage grows without bound. The cost model must include the lifetime of the resource, not just the allocation speed.

### 14.3.7 Putting the Model Together

A complete cost model for a hot path answers:

1. **Allocations**: how many, what sizes, which allocator, what contention?
2. **Cache footprint**: how many cache lines touched, what is the access pattern (sequential, random, pointer-chase)?
3. **Indirection depth**: how many dependent dereferences between input and output?
4. **Copy volume**: how many bytes copied per operation, and are any of them unnecessary?
5. **Branch predictability**: are there data-dependent branches or indirect calls in the hot path, and how predictable are they?

Write the answers down. Literally. A one-paragraph cost note in the design document or code comment pays for itself the first time someone proposes "optimizing" the wrong thing.

```cpp
// Cost model for tick normalization (per tick):
//   - 0 heap allocations (arena-backed, pre-reserved)
//   - 2 cache lines touched (RawTick is 96 bytes = 1.5 cache lines)
//   - 0 pointer chases (all data inline)
//   - 1 branch (exchange ID dispatch, >99% predictable in practice)
//   - ~40 ns measured on Xeon 8380 @ 2.3 GHz
```

## 14.4 Tradeoffs and Boundaries

### 14.4.1 Arena allocators are not universally better

Arenas trade deallocation granularity for speed. If your workload requires freeing individual objects (e.g., a long-lived cache with eviction), an arena is the wrong tool. Pool allocators (`std::pmr::unsynchronized_pool_resource`) or size-class allocators (tcmalloc, mimalloc) are more appropriate.

### 14.4.2 Contiguous layout is not always faster

A `std::vector<std::unique_ptr<Widget>>` is worse than `std::vector<Widget>` for traversal — the indirection defeats prefetching. But if `Widget` is 4 KB and you only access 1% of elements per pass, the indirection *saves* memory bandwidth by avoiding loading 99% of the data. The cost model must account for the *access pattern*, not just the *layout*.

### 14.4.3 Small-buffer optimization has a size cost

`std::string`, `std::function`, `std::any`, and many `std::pmr` containers use small-buffer optimization (SBO) to avoid heap allocation for small payloads. The buffer increases the `sizeof` of the object. A `std::function` is typically 32–64 bytes even when empty. If you store millions of them, the inline buffer may waste more memory than the heap allocation it avoids.

### 14.4.4 Abstractions have real costs — and real benefits

A `std::shared_ptr` costs ~20 ns per copy (atomic increment) and ~20 ns per destruction (atomic decrement + potential deallocation). In a hot loop processing millions of elements, this matters. In a configuration object accessed once at startup, it does not. The cost model prevents both premature optimization ("never use `shared_ptr`!") and premature dismissal ("the overhead is negligible"). The answer is always: negligible *relative to what?*

### 14.4.5 Thread-local allocation changes the tradeoff

Modern allocators (tcmalloc, mimalloc, jemalloc) use thread-local caches to eliminate lock contention on the fast path. This means allocation cost in a multi-threaded program is often *lower* than naive analysis suggests — but only if threads are long-lived and allocations are balanced. Short-lived threads or heavily skewed allocation patterns (one thread allocates, another frees) can force cross-thread returns that are 5–10× slower than the fast path.

## 14.5 Testing and Tooling Implications

### 14.5.1 Allocation-aware testing

Write tests that assert allocation counts for critical paths. This catches regressions that are invisible to functional tests:

```cpp
#include <cassert>

// Assuming an allocation-counting facility (see Section 3.2)
TEST(TickNormalization, ZeroAllocationsPerTick) {
    AllocationCounter counter;  // RAII — starts counting on construction
    auto arena = make_tick_arena(/*capacity=*/1024);

    process_ticks(sample_ticks(1000), arena);

    // Only the initial arena setup should allocate
    assert(counter.allocations_since_start() <= 1);
}
```

This kind of test fails fast when someone adds a `std::string` concatenation or an `std::format` call inside the hot path. It is more useful than a benchmark for catching allocation regressions in CI, because it is deterministic and has zero noise.

### 14.5.2 Sanitizer integration

AddressSanitizer (ASan) tracks every allocation and deallocation. Its `__asan_get_alloc_stack` API can identify where each allocation originated. In combination with a custom allocator that tags allocations by subsystem, you can build a per-subsystem allocation profile without full profiling overhead.

HeapSanitizer (not a standard sanitizer, but tools like `heaptrack` on Linux or `DHAT` from Valgrind) provide allocation profiles: number of allocations, total bytes, allocation lifetime histograms. These answer the question "are my allocations short-lived (suitable for arenas) or long-lived (need a pool)?"

### 14.5.3 Compiler Explorer for cost intuition

Use [Compiler Explorer](https://godbolt.org) to check whether an abstraction compiles away. A `std::optional<int>` should compile to the same code as an `int` + `bool` flag. A `std::span` should compile away entirely in optimized builds. When it does not, the cost model needs to account for the residual overhead.

```cpp
// Check: does std::span compile away?
int sum_span(std::span<int const> s) {
    int total = 0;
    for (auto v : s) total += v;
    return total;
}

int sum_raw(int const* data, std::size_t n) {
    int total = 0;
    for (std::size_t i = 0; i < n; ++i) total += data[i];
    return total;
}
// At -O2, GCC 14 and Clang 18 produce identical assembly for both.
```

When the assembly differs, you have found a cost that the abstraction is not hiding. That cost may be acceptable — but it should be known, not assumed away.

### 14.5.4 `perf` and hardware counters

The cost model makes predictions; hardware counters validate them. Key events to monitor:

| Counter | What it reveals |
|---|---|
| `cache-misses` / `cache-references` | Overall cache effectiveness |
| `L1-dcache-load-misses` | Spatial locality problems |
| `LLC-load-misses` | Working set exceeds last-level cache |
| `dTLB-load-misses` | Too many distinct memory pages touched |
| `branch-misses` | Unpredictable control flow |
| `instructions` / `cycles` | IPC — low values suggest memory stalls |

If your cost model predicts zero allocations and high spatial locality, but `LLC-load-misses` is high, the model is wrong. Investigate. Common causes: unexpected copies, iterator invalidation forcing reallocation, or the working set simply being larger than estimated.

## 14.6 Review Checklist

Use this checklist during code review for performance-sensitive paths:

- [ ] **Allocation count**: can you state the number of heap allocations per logical operation? If not, instrument and measure.
- [ ] **Allocation lifetime**: are allocations short-lived (candidates for arena/pool) or long-lived (need stable addresses)?
- [ ] **Indirection depth**: how many dependent pointer dereferences does the hot path require? Can any be eliminated by inlining data or using indices instead of pointers?
- [ ] **Copy audit**: are there any implicit copies in range-for loops, lambda captures, function arguments, or return values? Mark intentional copies explicitly.
- [ ] **Cache footprint**: how many cache lines does processing one logical unit touch? Is the access pattern sequential, strided, or random?
- [ ] **Branch predictability**: does the hot path contain data-dependent branches, virtual calls, or variant visitation? What is the expected prediction rate?
- [ ] **Allocator choice**: is the default allocator appropriate, or would `pmr`, a pool, or a thread-local cache improve the cost profile?
- [ ] **Cost model documentation**: is there a cost note (in code or design doc) that states the expected allocation, cache, and branch behavior? Will the next person to touch this code know what assumptions they must preserve?
- [ ] **Regression test**: is there a test or assertion that will fail if the allocation or copy behavior regresses?
- [ ] **Measurement validation**: have hardware counters or allocation profiling confirmed the cost model's predictions? If the model and measurements disagree, which is wrong?
