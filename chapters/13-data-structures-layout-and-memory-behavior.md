# Chapter 13: Data Structures, Layout, and Memory Behavior

*Prerequisites: Chapter 1 (ownership-aware layout), Chapter 3 (value semantics determine copy and move cost), and Chapter 6 (spans and views as non-owning access).*

High-level design decisions often hide low-level costs until the system reaches real scale. A `std::map` chosen for convenience in a prototype survives into production, where it becomes millions of scattered heap nodes touched on every request. A flat array of polymorphic pointers seems cache-friendly until the virtual dispatch at each element destroys the branch predictor. These failures share a root cause: the data structure was chosen to match the API shape rather than the workload shape.

This chapter covers container choice, representation tradeoffs, ownership-aware data layout, and how memory behavior interacts with cache locality, traversal patterns, and update frequency. The focus is on selecting structures that fit the workload — the "what and why" of data representation. When a container choice is also an API contract (exposing iterators, constraining element lifetime, or promising contiguous storage), that boundary implication is part of the analysis. Chapter 14 picks up where this one ends: measuring whether the choices actually deliver.

---

> **Prerequisites:** You should already understand **ownership and RAII** (Chapter 1): who owns the memory backing a data structure determines when and how it is released, and constrains where the structure can live. **Value semantics, copy, and move cost** (Chapter 3): the cost of populating, growing, and reorganizing a container depends directly on the copy and move behavior of its elements. **Basic memory model**: what a cache line is (~64 bytes on modern x86/ARM), what a TLB miss costs, and the general hierarchy of L1/L2/L3/main memory latencies. You do not need to be a hardware specialist, but you need to know that pointer chasing is expensive and spatial locality is cheap.

---

## 13.1 The Production Problem

A trading analytics service maintains an order book per instrument. The initial implementation used `std::map<OrderId, Order>` for fast lookup and `std::list<Order*>` for price-level iteration. Under light load, latency was acceptable. At 50,000 instruments with frequent updates, the service missed SLA targets by 3x. Profiling showed the dominant cost was not computation — it was memory access. Every map lookup chased a tree of heap-allocated nodes. Every price-level traversal walked a linked list whose nodes were scattered across the heap. The CPU spent more time waiting for cache lines than doing useful work.

The fix was not algorithmic. The asymptotic complexity was fine. The fix was structural: replacing the node-based containers with contiguous storage, sorting by access pattern, and co-locating data that was read together. Latency dropped below SLA with no change to the logical operations.

This pattern recurs across domains. The container that matches the abstract data type (map for key-value, list for ordered sequence) is often the wrong container for the workload. Production data structure selection is about memory behavior first and abstract interface second.

---

## 13.2 The Naive Approach: Choosing Containers by Abstract Interface

The standard library names its containers after abstract data types: `map`, `set`, `list`, `deque`. This naming encourages a selection process that starts from the wrong question — "what abstract operations do I need?" — instead of "what does my workload actually do to this data?"

### 13.2.1 Anti-pattern: Node-based containers as default

```cpp
// Anti-pattern: choosing std::map because the problem is "key-value lookup"
struct SessionCache {
    std::map<SessionId, SessionData> sessions_;  // RISK: each entry is a separate
                                                  // heap allocation; tree traversal
                                                  // on every lookup

    void expire_old_sessions(Timestamp cutoff) {
        // RISK: full tree walk, pointer-chasing through scattered nodes
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            if (it->second.last_active < cutoff) {
                it = sessions_.erase(it);  // RISK: per-node deallocation
            } else {
                ++it;
            }
        }
    }
};
```

This code is correct. It has logarithmic lookup, ordered iteration, and safe erasure during traversal. On a machine with 200ns main-memory latency and 1ns L1 latency, the constant factor hiding behind O(log n) is the dominant cost once n exceeds a few thousand entries.

### 13.2.2 Anti-pattern: `std::list` for "I need stable iterators"

```cpp
// Anti-pattern: using std::list because "elements must not move"
struct TaskQueue {
    std::list<Task> pending_;  // RISK: every node is a separate allocation;
                               // iteration touches a new cache line per element

    void enqueue(Task t) {
        pending_.push_back(std::move(t));  // BUG: heap allocation per enqueue
                                           // under load, allocator contention
    }
};
```

The stated requirement — stable element addresses — is real, but `std::list` is almost never the right way to satisfy it. The per-element allocation cost and cache-hostile iteration pattern make it one of the most expensive containers in the standard library for any workload that iterates.

---

## 13.3 Modern Approach: Workload-Driven Container Selection

### 13.3.1 Start from the access pattern

Before choosing a container, answer these questions about the workload:

1. **Read/write ratio.** Is the data mostly read, mostly written, or balanced?
2. **Traversal pattern.** Do you scan all elements, access by key, or access by position?
3. **Mutation frequency.** How often are elements inserted, removed, or modified?
4. **Element size.** Are elements small values (< 64 bytes) or large objects?
5. **Lifetime constraint.** Must element addresses remain stable across mutations?
6. **Concurrency.** Is the container accessed from multiple threads? (Chapter 7 covers synchronization; here we care about the layout implications.)

These questions determine the container, not the abstract data type.

### 13.3.2 Contiguous storage as the default

For most workloads, `std::vector` is the right starting point. Contiguous storage means sequential reads are prefilled by the hardware prefetcher, iteration touches minimal cache lines, and the allocator is invoked infrequently (amortized O(1) push_back). Even for "lookup by key" workloads, a sorted vector with binary search often outperforms `std::map` for collections under ~100,000 elements, because the cache behavior dominates the log-factor difference.

```cpp
// Sorted vector as an associative container
struct SessionCache {
    struct Entry {
        SessionId id;
        SessionData data;
    };

    std::vector<Entry> sessions_;  // contiguous, cache-friendly

    auto find(SessionId id) -> SessionData* {
        auto it = std::ranges::lower_bound(sessions_, id, {}, &Entry::id);
        if (it != sessions_.end() && it->id == id) return &it->data;
        return nullptr;
    }

    void insert(SessionId id, SessionData data) {
        auto it = std::ranges::lower_bound(sessions_, id, {}, &Entry::id);
        sessions_.insert(it, Entry{id, std::move(data)});
    }

    void expire_old_sessions(Timestamp cutoff) {
        std::erase_if(sessions_, [&](const Entry& e) {
            return e.data.last_active < cutoff;
        });
    }
};
```

The insert is O(n) due to element shifting. For a read-heavy workload with infrequent mutations, this is a net win over `std::map` because every lookup and every scan runs at memory-bus speed instead of pointer-chasing speed.

### 13.3.3 `std::flat_map` and `std::flat_set` (C++23)

C++23 codifies the sorted-vector pattern with `std::flat_map` and `std::flat_set`. These adaptors store keys and values in separate contiguous sequences, giving you associative-container semantics with contiguous-storage performance.

```cpp
#include <flat_map>

// C++23: associative interface, contiguous storage
std::flat_map<SessionId, SessionData> sessions;

sessions.insert({id, std::move(data)});
auto it = sessions.find(id);
```

Key properties of `std::flat_map`:

- **Lookup** is binary search over a contiguous key array — cache-friendly.
- **Insertion and erasure** are O(n) due to shifting, same as a sorted vector.
- **Iteration** is contiguous in both keys and values, independently.
- **Iterator invalidation** follows vector rules: any insertion or erasure can invalidate all iterators.

Use `std::flat_map` when the read-to-write ratio is high, the collection fits comfortably in cache (tens of thousands of entries or fewer with small keys), and you do not need stable iterators or references.

### 13.3.4 When you actually need node-based containers

Node-based containers (`std::map`, `std::set`, `std::unordered_map`, `std::unordered_set`, `std::list`) are the right choice under specific constraints:

- **Stable references required by contract.** If your API returns references or pointers to elements and callers hold them across mutations, elements must not move. Node-based containers guarantee this. Note: this is an API design question. If you control the interface, consider returning handles (indices, IDs) instead of pointers, which lets you use contiguous storage.
- **Frequent insertion and deletion in the middle.** If the workload is dominated by inserts and erases at arbitrary positions and the collection is large, the O(n) shifting cost of a vector becomes dominant. The crossover point depends on element size — for 8-byte elements, vectors often win past 100,000 entries; for 4KB elements, the crossover is much lower.
- **Interleaved mutation and iteration.** If you must erase elements during iteration and the erase pattern is sparse, `std::map::erase` returning the next iterator is ergonomic and correct.
- **Hash-table performance.** `std::unordered_map` is node-based in most implementations, but its O(1) average lookup is worth the scattered memory when lookup dominates and elements are expensive to move. For small-element, lookup-heavy workloads, consider open-addressing alternatives (Abseil's `flat_hash_map`, Boost's `unordered_flat_map` in Boost 1.81+).

### 13.3.5 Separation of hot and cold data

A common mistake is storing all fields of an object in a single structure, then putting those structures in a vector. If the hot path reads only two fields out of twelve, every cache line loaded carries ten fields of waste.

```cpp
// Anti-pattern: all fields in one struct, traversed for a hot-path field
struct Instrument {
    InstrumentId id;                // hot: read on every tick
    double last_price;              // hot: read on every tick
    double bid, ask;                // hot: read on every tick
    std::string name;               // cold: read only on display
    std::string exchange;           // cold: read only on display
    std::vector<Tag> tags;          // cold: read only on admin queries
    AuditLog audit;                 // cold: read only on compliance queries
    // ... total size: ~280 bytes
};

std::vector<Instrument> instruments;  // RISK: iterating for price updates
                                      // loads 280 bytes per element,
                                      // uses ~24 bytes
```

Split hot and cold data into parallel arrays or a struct-of-arrays layout:

```cpp
// Hot data: fits more elements per cache line
struct InstrumentPricing {
    InstrumentId id;
    double last_price;
    double bid;
    double ask;
};
static_assert(sizeof(InstrumentPricing) <= 64);  // fits in one cache line

// Cold data: loaded only when needed
struct InstrumentMetadata {
    std::string name;
    std::string exchange;
    std::vector<Tag> tags;
    AuditLog audit;
};

struct InstrumentStore {
    std::vector<InstrumentPricing> pricing;   // hot path iterates this
    std::vector<InstrumentMetadata> metadata;  // cold path indexes into this

    // Invariant: pricing.size() == metadata.size()
    // pricing[i] and metadata[i] refer to the same instrument
};
```

This is a data-oriented design pattern sometimes called "struct of arrays" (SoA). The tradeoff is coupling: you must maintain the invariant that both vectors are the same size and indexed consistently. The benefit is that hot-path iteration over pricing touches only pricing cache lines, fitting 2–3x more elements per line than the combined struct. The cost is that SoA only helps when the hot path touches a subset of fields. If the hot path usually reads all fields of one logical element together, array-of-structs is often faster because related data already lands in the same cache line.

### 13.3.6 Object pools and arena patterns

When the workload creates and destroys many short-lived objects of the same type, per-object allocation becomes a bottleneck. An object pool or arena pre-allocates a block and hands out slots.

Intentional partial: the sketch below returns uninitialized storage. Real code must construct objects with `std::construct_at()` or placement `new`, and must pair `deallocate()` with destruction.

```cpp

template <typename T, std::size_t BlockSize = 4096>
class ObjectPool {
    struct Block {
        alignas(T) std::byte storage[BlockSize * sizeof(T)];
        std::bitset<BlockSize> occupied;
        std::size_t next_free = 0;
    };

    std::vector<std::unique_ptr<Block>> blocks_;

public:
    struct Handle {
        std::uint32_t block_index;
        std::uint32_t slot_index;
    };

    [[nodiscard]] auto allocate() -> std::pair<Handle, T*> {
        for (std::uint32_t bi = 0; bi < blocks_.size(); ++bi) {
            auto& blk = *blocks_[bi];
            if (blk.next_free < BlockSize) {
                auto slot = blk.next_free++;
                blk.occupied.set(slot);
                auto* ptr = std::launder(
                    reinterpret_cast<T*>(blk.storage + slot * sizeof(T)));
                return {{bi, static_cast<std::uint32_t>(slot)}, ptr};
            }
        }
        blocks_.push_back(std::make_unique<Block>());
        auto& blk = *blocks_.back();
        blk.occupied.set(0);
        blk.next_free = 1;
        auto* ptr = std::launder(
            reinterpret_cast<T*>(blk.storage));
        return {{static_cast<std::uint32_t>(blocks_.size() - 1), 0}, ptr};
    }

    void deallocate(Handle h) {
        blocks_[h.block_index]->occupied.reset(h.slot_index);
    }

    auto get(Handle h) -> T* {
        auto& blk = *blocks_[h.block_index];
        return std::launder(
            reinterpret_cast<T*>(blk.storage + h.slot_index * sizeof(T)));
    }
};
```

The pool returns handles rather than raw pointers. Handles are stable identifiers that survive pool growth (new blocks do not move existing blocks), and they can be validated — a freed slot can be detected by checking the occupancy bitset. This is the pattern referenced in Section 3.4: returning handles instead of pointers lets you decouple element identity from element address.

Pools are not free. They add a layer of indirection (handle → pointer), they require manual lifetime discipline (the pool does not call destructors automatically in this minimal sketch), and they complicate debugging. Use them when allocation profiling (Chapter 14) shows that per-object `new`/`delete` is a measurable cost.

### 13.3.7 `std::pmr` and memory resources

C++17 introduced `std::pmr::polymorphic_allocator` and a set of memory resource classes. C++23 codebases should consider `std::pmr` containers when:

- You want arena-style allocation without writing a custom pool.
- The container's lifetime is scoped to a request, transaction, or frame.
- You want to measure allocation behavior by swapping in a tracking resource.

```cpp
#include <memory_resource>
#include <vector>

void process_request(std::span<const std::byte> payload) {
    // Stack-backed buffer for small requests; falls back to heap
    std::array<std::byte, 8192> buffer;
    std::pmr::monotonic_buffer_resource arena(
        buffer.data(), buffer.size(), std::pmr::null_memory_resource());

    // All allocations from this vector come from the arena
    std::pmr::vector<ParsedField> fields(&arena);
    fields.reserve(64);

    parse_into(payload, fields);
    validate(fields);
    // arena memory is released when `arena` goes out of scope —
    // no per-element deallocation
}
```

`std::pmr::monotonic_buffer_resource` is particularly effective for parse-process-discard workloads: it never frees individual allocations, so deallocation is a no-op, and the entire buffer is released at scope exit. The tradeoff is that you must ensure nothing retains a pointer into the arena after the resource is destroyed — an ownership constraint that Chapter 1's principles directly address.

### 13.3.8 Small-buffer optimization and inline storage

Many standard library implementations apply small-buffer optimization (SBO) to `std::string` and `std::function`. The principle generalizes: if most instances are small, store them inline to avoid heap allocation entirely.

```cpp
// A variant that avoids heap allocation for common message types
using Message = std::variant<
    Heartbeat,          // 8 bytes
    PriceUpdate,        // 32 bytes
    OrderAck,           // 24 bytes
    std::unique_ptr<LargeReport>  // 8 bytes (pointer), heap-allocated payload
>;
// sizeof(Message) ≈ 40 bytes (variant overhead + largest inline member)
// Common messages are inline; rare large messages go to heap
```

`std::variant` gives you tagged-union semantics with inline storage for all alternatives. If one alternative is disproportionately large, wrap it in `std::unique_ptr` to keep the variant small. This preserves value semantics for the common case while paying heap cost only for the uncommon case.

### 13.3.9 `std::mdspan`: Zero-overhead multidimensional views (C++23)

Contiguous storage is the foundation of cache-friendly data access, but many production workloads operate on logically multidimensional data — images (rows x columns x channels), matrices, sensor grids, simulation meshes. Before C++23, accessing such data meant either manual index arithmetic (error-prone, hard to read) or wrapper classes that each project reinvented. `std::mdspan` solves this: it is a non-owning, zero-overhead multidimensional view over contiguous storage, analogous to how `std::span` provides a one-dimensional view.

```cpp
#include <mdspan>
#include <vector>

// A 1080p RGB image stored as a flat vector
std::vector<std::uint8_t> pixels(1920 * 1080 * 3);

// View the same memory as a 3D array: [row][col][channel]
auto image = std::mdspan(pixels.data(),
    std::extents<std::size_t, 1080, 1920, 3>{});

// Access is bounds-checked in debug, zero-overhead in release
image[540, 960, 0] = 255;  // set red channel of center pixel
image[540, 960, 1] = 0;    // green
image[540, 960, 2] = 0;    // blue
```

`std::mdspan` does not own memory. It is a view type — a pointer plus extents plus a layout mapping. The owning container (`std::vector`, a `std::pmr` container, a raw allocation, or mapped memory) lives elsewhere. This separation of storage ownership from access shape is the key design insight.

**Layout policies** control how multidimensional indices map to linear offsets:

- **`std::layout_right`** (the default): row-major order. The rightmost index varies fastest. This is the C/C++ array convention and is optimal when the inner loop iterates over the last dimension (e.g., iterating across columns within a row, or across channels within a pixel).
- **`std::layout_left`**: column-major order. The leftmost index varies fastest. This is the Fortran/MATLAB convention and is optimal for column-oriented access patterns or interoperating with Fortran libraries.
- **`std::layout_stride`**: each dimension has an explicit stride. Use this when the view represents a subregion of a larger allocation (e.g., a tile within an image), when rows are padded for SIMD alignment, or when interoperating with external libraries that use non-standard strides.

```cpp
// Row-major (default) — iterating across columns is fast
auto row_major = std::mdspan<float, std::dextents<std::size_t, 2>,
                             std::layout_right>(data, rows, cols);

// Column-major — iterating down rows is fast
auto col_major = std::mdspan<float, std::dextents<std::size_t, 2>,
                             std::layout_left>(data, rows, cols);

// Strided — a 100x100 subregion of a 1920-wide image
std::array<std::size_t, 2> strides{1920, 1};  // row stride, col stride
auto tile = std::mdspan<float, std::dextents<std::size_t, 2>,
                        std::layout_stride>(
    data + start_row * 1920 + start_col,
    std::layout_stride::mapping<std::dextents<std::size_t, 2>>(
        std::dextents<std::size_t, 2>(100, 100), strides));
```

**Custom accessors** let you transform element access without modifying storage. The accessor policy controls how the pointer-plus-offset is converted to a reference. The default (`std::default_accessor`) simply dereferences the pointer. A custom accessor can apply scaling, unit conversion, or atomic access:

```cpp
// Accessor that scales every read by a constant factor
template <typename T>
struct ScaledAccessor {
    using element_type = T;
    using reference = T;              // returns by value (the scaled result)
    using data_handle_type = const T*;
    using offset_policy = ScaledAccessor;

    T scale_factor;

    constexpr reference access(data_handle_type p, std::size_t i) const noexcept {
        return p[i] * scale_factor;
    }
    constexpr data_handle_type offset(data_handle_type p, std::size_t i) const noexcept {
        return p + i;
    }
};

// View raw sensor data as already-calibrated values
std::vector<float> raw_adc(sensor_rows * sensor_cols);
auto calibrated = std::mdspan<const float, std::dextents<std::size_t, 2>,
                              std::layout_right, ScaledAccessor<float>>(
    raw_adc.data(), sensor_rows, sensor_cols,
    ScaledAccessor<float>{.scale_factor = 0.00489f});  // ADC to volts

float voltage = calibrated[row, col];  // scaled on read, no copy of data
```

**Production use cases.** `std::mdspan` is not an academic curiosity; it addresses real production patterns:

- **Image processing.** View pixel buffers as `[height][width][channels]` without index arithmetic. Switch between planar (channels as outermost dimension) and interleaved (channels as innermost) layouts by changing the layout policy, not the processing code.
- **Matrix operations.** Dense linear algebra on row-major or column-major storage, interoperating with BLAS/LAPACK libraries that expect specific layouts.
- **Scientific and simulation data.** 3D grids, time-series of 2D fields, multi-variable datasets — all map naturally to `mdspan` with appropriate extents.
- **Sensor arrays.** Hardware sensor grids (LiDAR, camera arrays, antenna arrays) produce data in fixed-size multidimensional layouts. `mdspan` provides type-safe access with zero runtime cost.

**`mdspan` as an API boundary type.** One of the strongest uses of `mdspan` is at function boundaries. A function that operates on 2D data can accept an `mdspan` instead of a raw pointer plus dimensions:

```cpp
// Before: raw pointer, manual dimensions, no layout safety
void blur(const float* input, float* output,
          std::size_t rows, std::size_t cols, std::size_t stride);

// After: self-describing, layout-aware, zero-copy
void blur(std::mdspan<const float, std::dextents<std::size_t, 2>> input,
          std::mdspan<float, std::dextents<std::size_t, 2>> output);
```

The `mdspan` version carries its dimensions and layout intrinsically. Callers cannot accidentally swap rows and columns, pass the wrong stride, or forget a dimension. The function can query `input.extent(0)` and `input.extent(1)` instead of relying on separately-passed integers. If both row-major and column-major callers exist, the function can be templated on the layout policy:

```cpp
template <typename Layout>
void blur(std::mdspan<const float, std::dextents<std::size_t, 2>, Layout> input,
          std::mdspan<float, std::dextents<std::size_t, 2>, Layout> output);
```

**Interaction with owning containers.** `mdspan` is always non-owning. The backing storage must outlive the `mdspan`, just as with `std::span`. A common pattern is a `std::vector` as the owning store with `mdspan` as the access interface:

```cpp
class Matrix {
    std::vector<double> storage_;
    std::size_t rows_;
    std::size_t cols_;

public:
    Matrix(std::size_t r, std::size_t c)
        : storage_(r * c), rows_(r), cols_(c) {}

    auto view() {
        return std::mdspan(storage_.data(),
            std::extents<std::size_t, std::dynamic_extent,
                         std::dynamic_extent>(rows_, cols_));
    }

    auto view() const {
        return std::mdspan(std::as_const(storage_).data(),
            std::extents<std::size_t, std::dynamic_extent,
                         std::dynamic_extent>(rows_, cols_));
    }
};
```

This preserves the ownership model from Chapter 1 — the `Matrix` owns the storage, and `mdspan` views are borrowed references that must not outlive it. The same pattern works with `std::pmr::vector` for arena-backed multidimensional data, or with `std::unique_ptr<T[]>` for fixed-size allocations.

### 13.3.10 Container choice as API contract

When a container type appears in a public interface, it becomes a contract. Callers may depend on:

- **Iterator stability.** `std::map` guarantees iterators survive insertion; `std::vector` does not.
- **Contiguous storage.** A function taking `std::span<const T>` promises contiguous layout. Returning `std::vector<T>` implies the caller can take a span over it.
- **Element lifetime.** A reference into a `std::deque` survives push_back at either end; a reference into a `std::vector` does not survive any reallocation.

These are not implementation details. They are semantic promises. Changing from `std::map` to `std::flat_map` in a public API is a breaking change if any caller holds iterators across mutations.

The defensive approach: expose the narrowest contract that supports the workload. Return `std::span` instead of `const std::vector<T>&` when callers need only to read. Accept ranges or iterator pairs instead of concrete container types. Keep the container type private and expose access through functions that do not leak stability guarantees.

```cpp
class PriceBook {
public:
    // Good: narrow contract, no container type leaked
    auto best_bid() const -> std::optional<Price>;
    auto levels(Side side) const -> std::span<const PriceLevel>;

private:
    // Implementation detail: can change without breaking callers
    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
};
```

---

## 13.4 Tradeoffs and Boundaries

### 13.4.1 Contiguous vs. node-based

| Property | `std::vector` / `std::flat_map` | `std::map` / `std::list` |
|---|---|---|
| Cache behavior on iteration | Excellent (prefetcher-friendly) | Poor (pointer chasing) |
| Insertion in the middle | O(n) shift | O(log n) or O(1) |
| Iterator/reference stability | None across mutations | Guaranteed |
| Memory overhead per element | Zero (plus amortized slack) | 2–3 pointers per node |
| Allocator pressure | Rare, bulk | Frequent, per-node |

### 13.4.2 Sorted vector vs. `std::flat_map`

`std::flat_map` is a standardized sorted-vector adaptor. It is not always superior to a hand-rolled sorted vector:

- `flat_map` stores keys and values in **separate** vectors. If your lookup pattern reads the value immediately after finding the key, this means two cache misses (one for the key array, one for the value array at the same index) instead of one (key and value adjacent in a single vector of pairs).
- If your values are large, the separation is a benefit: key-only binary search touches fewer cache lines.
- If your values are small and always accessed with their keys, a single `std::vector<std::pair<K,V>>` may be faster.

Profile before committing to either layout. Chapter 14 covers how.

### 13.4.3 SoA vs. AoS

Struct-of-arrays improves cache utilization when the hot path touches a subset of fields. It degrades when the hot path touches many fields of the same element, because each field access hits a different array (and potentially a different cache line). Measure the actual access pattern before splitting.

### 13.4.4 Pool allocation vs. general allocator

Object pools reduce allocation latency and fragmentation, but they introduce handle-based indirection, complicate debugging (addresses are less meaningful), and require careful destruction discipline. Reserve pools for workloads where profiling confirms that allocation is a top-five cost.

---

## 13.5 Testing and Tooling Implications

### 13.5.1 Correctness testing with address sanitizer

Any container migration — especially from node-based to contiguous — risks introducing use-after-move, dangling reference, or iterator invalidation bugs. Compile and run tests with AddressSanitizer (`-fsanitize=address`) to catch:

- Access to a `std::vector` element after reallocation.
- Use of a reference into a `std::flat_map` after insertion.
- Reads from a `std::pmr` container after the memory resource is destroyed.

### 13.5.2 Verifying layout assumptions

Use `static_assert` to enforce size and alignment expectations:

```cpp
static_assert(sizeof(InstrumentPricing) <= 64,
    "InstrumentPricing must fit in a single cache line");

static_assert(std::is_trivially_copyable_v<InstrumentPricing>,
    "InstrumentPricing must be trivially copyable for memcpy-based operations");

static_assert(alignof(InstrumentPricing) <= alignof(std::max_align_t),
    "InstrumentPricing must not require over-aligned allocation");
```

These assertions catch silent regressions when someone adds a field or changes a type.

### 13.5.3 Allocation-aware testing

Wrap tests in a custom `std::pmr::memory_resource` that counts allocations:

```cpp
class CountingResource : public std::pmr::memory_resource {
    std::size_t allocation_count_ = 0;
    std::pmr::memory_resource* upstream_;

protected:
    void* do_allocate(std::size_t bytes, std::size_t align) override {
        ++allocation_count_;
        return upstream_->allocate(bytes, align);
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
        upstream_->deallocate(p, bytes, align);
    }
    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }

public:
    explicit CountingResource(std::pmr::memory_resource* up)
        : upstream_(up) {}

    auto count() const -> std::size_t { return allocation_count_; }
};

// In a test:
CountingResource counter(std::pmr::get_default_resource());
{
    std::pmr::vector<int> v(&counter);
    v.reserve(1000);
    for (int i = 0; i < 1000; ++i) v.push_back(i);
}
EXPECT_EQ(counter.count(), 1u);  // single allocation from reserve()
```

This technique makes allocation behavior a testable property, not an assumption. If a refactor introduces unexpected allocations, the test fails.

### 13.5.4 Iterator invalidation as a test category

For any container exposed through an interface, write explicit tests for the invalidation contract:

```cpp
// Test: verify that span over PriceBook::levels() remains valid
// after a read-only operation
auto book = make_test_price_book();
auto levels = book.levels(Side::Bid);
auto first_price = levels[0].price;

book.update_last_trade(some_trade);  // does not modify bid levels

EXPECT_EQ(levels[0].price, first_price);  // span still valid
```

These tests encode the stability promises your API makes. When you change the underlying container, they tell you whether you broke a contract.

---

## 13.6 Review Checklist

Use this checklist when reviewing code that introduces or modifies a data structure:

**Container selection**

- [ ] Is the container chosen based on the actual access pattern (read/write ratio, traversal, mutation frequency), not just the abstract data type?
- [ ] If a node-based container is used (`std::map`, `std::list`, `std::unordered_map`), is there a documented reason why contiguous storage is insufficient?
- [ ] If the collection is small (< 1,000 elements) and read-heavy, has a sorted `std::vector` or `std::flat_map` been considered?
- [ ] For hash maps with small, trivially-copyable keys and values, has an open-addressing alternative been considered?

**Layout and locality**

- [ ] Are hot and cold fields separated when the hot path touches a small subset of each element?
- [ ] Do `static_assert` checks enforce size, alignment, or trivial-copyability assumptions?
- [ ] If struct-of-arrays layout is used, is the parallel-array invariant (same size, consistent indexing) enforced in the API?

**Ownership and lifetime**

- [ ] If the container uses arena or pool allocation, is the lifetime of the resource clearly scoped and documented?
- [ ] Are handles used instead of raw pointers when element addresses may change?
- [ ] If `std::pmr` containers are used, is the memory resource lifetime guaranteed to outlive all references into the container?

**API contract**

- [ ] Does the public interface avoid leaking the concrete container type when a narrower type (`std::span`, range, iterator pair) would suffice?
- [ ] Are iterator and reference stability guarantees documented for any container exposed in the API?
- [ ] If the container type changed (e.g., `std::map` to `std::flat_map`), have all callers been audited for reliance on the old stability guarantees?

**Testing and verification**

- [ ] Are tests run under AddressSanitizer to catch invalidation and use-after-free?
- [ ] Is allocation count tested for performance-critical paths?
- [ ] Do tests cover the iterator/reference stability contract of any container exposed through the API?
