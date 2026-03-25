# Chapter 3: Value Semantics, Identity, and Invariants

> **Prerequisites:** This chapter assumes you understand RAII, move semantics, and smart-pointer ownership from Chapter 1. You should be comfortable reasoning about when a destructor runs and who is responsible for a resource. The failure-handling material from Chapter 2 is helpful but not required here.

## 3.1 Production Problem

A price feed ingestion service maintains an in-memory book of instrument quotes. Each quote has a symbol, a bid, an ask, a timestamp, and some derived fields. Early in the project, the team models quotes as plain structs with public fields, copies them freely, and compares them with member-wise equality. The system works until three problems compound:

1. Two threads hold copies of the same quote. One thread updates the bid; the other still sees the stale copy. A downstream aggregation produces a crossed market (bid > ask) because the two copies diverged silently.
2. A cache keyed on `Quote` uses default `operator==`. Someone adds a `last_seen` diagnostic field. Now two quotes for the same instrument at the same price compare unequal, and the cache hit rate collapses.
3. A serialization layer round-trips a quote to JSON and back. The reconstructed object compares equal to the original, but the original carried a non-serialized internal sequence number used for deduplication. The deduplication logic breaks because the sequence number is zero after deserialization.

None of these bugs is a language error. They are design errors rooted in the same cause: the team never decided what "sameness" means for a quote, which fields are part of its identity, which are part of its value, and which are incidental metadata. The language gave them memberwise copy and comparison by default, and the defaults were wrong.

This class of problem shows up wherever objects carry mixed concerns — identifiers, measured values, cached derived state, diagnostic metadata — and the code treats them uniformly. The cost is subtle data corruption, broken caching, flaky tests, and thread-safety assumptions that hold only by accident.

## 3.2 Naive or Legacy Approach

The common starting point is a struct where every field is public and no semantic distinction exists between fields:

```cpp
// Anti-pattern: undifferentiated bag of fields
struct Quote {
    std::string symbol;
    double bid;
    double ask;
    std::chrono::system_clock::time_point timestamp;
    std::chrono::steady_clock::time_point last_seen; // diagnostic
    uint64_t sequence_number;                         // dedup key

    // BUG: defaulted == compares all fields, including diagnostic metadata
    bool operator==(const Quote&) const = default;
};
```

This design has several failure modes that compound over time.

**Unrestricted mutation.** Any caller can modify any field at any time. There is no way to enforce that `bid <= ask` or that `sequence_number` increases monotonically. Every consumer must defensively validate, and most will not.

**Equality means everything and nothing.** Defaulted comparison includes `last_seen`, which changes every time the quote is observed, and `sequence_number`, which is an internal dedup mechanism. Equality is too strict for caching (where you want price equality) and too loose for deduplication (where you need sequence identity).

**Copies proliferate without intent.** Because the struct is cheap to copy and has no restrictions, code passes quotes by value across threads, into containers, and through queues. Each copy is an independent snapshot, but nothing in the type communicates whether the caller wanted a snapshot or a reference to a shared, mutable state. When a bug appears, the question "which copy diverged?" is unanswerable without tracing every call site.

**Identity is implicit.** The symbol string is the business identity of the instrument, but the type does not express this. A function that deduplicates quotes must know to compare only `symbol` — knowledge that lives in convention, not in the type system.

Teams patch these problems with comments, coding guidelines, wrapper functions, and increasingly baroque equality overloads. The patches do not compose and they do not survive refactoring.

## 3.3 Modern C++ Approach

The fix is to model the distinctions explicitly: separate value from identity, enforce invariants at construction, and make the type system carry the semantic intent.

### 3.3.1 Value Types

A value type is defined by its content, not by its address in memory. Two values with the same content are interchangeable. Copying a value produces a new object that is equal to the original, and the original is unaffected by mutations to the copy. `int`, `std::string`, `std::chrono::system_clock::time_point`, and `std::optional` are all value types.

A well-behaved value type in C++ satisfies *regular* semantics (borrowing Alexander Stepanov's term): it is default-constructible, copyable, movable, equality-comparable, and destruction is benign. The C++20 concept `std::regular` captures this mechanically, but the design intent matters more than the concept check. A type that satisfies `std::regular` syntactically but whose equality operator lies about equivalence is worse than a type that fails the concept honestly.

For the price feed, the pure price data is a value:

```cpp
class PricePoint {
public:
    constexpr PricePoint(double bid, double ask)
        : bid_{bid}, ask_{ask}
    {
        if (bid_ > ask_)
            throw std::invalid_argument{"crossed market: bid > ask"};
        if (!std::isfinite(bid_) || !std::isfinite(ask_))
            throw std::invalid_argument{"non-finite price"};
    }

    constexpr double bid() const noexcept { return bid_; }
    constexpr double ask() const noexcept { return ask_; }
    constexpr double mid() const noexcept { return (bid_ + ask_) / 2.0; }
    constexpr double spread() const noexcept { return ask_ - bid_; }

    constexpr bool operator==(const PricePoint&) const = default;
    constexpr auto operator<=>(const PricePoint&) const = default;

private:
    double bid_;
    double ask_;
};
```

Key properties:

- **Invariant at construction.** A `PricePoint` cannot exist in a crossed or non-finite state. Every consumer can rely on `bid() <= ask()` without checking.
- **Immutable after construction.** There are no setters. To change a price, you construct a new `PricePoint`. This eliminates aliasing hazards: if two threads hold copies, neither can corrupt the other.
- **Equality is meaningful.** Defaulted `==` compares bid and ask — exactly the fields that define the value. No diagnostic metadata leaks into comparison.
- **Cheap to copy.** Two doubles. Pass by value without hesitation.

The spaceship operator (`<=>`) gives you a total order, which makes `PricePoint` usable as a map key or in sorted containers without writing custom comparators.

### 3.3.2 Identity Types (Entities)

An identity type represents something with a persistent, unique existence that is not defined by its current field values. Two customers with the same name and address are still two different customers. An instrument in the trading system is identified by its symbol, not by its current price.

Identity types should typically be non-copyable. Copying an entity creates a second object that claims to be the same thing, which is almost always a bug. Move may or may not make sense depending on whether the entity's identity is tied to its address (e.g., a registered callback) or is portable (e.g., a session token).

```cpp
class Instrument {
public:
    explicit Instrument(std::string symbol)
        : symbol_{std::move(symbol)}
    {}

    // Non-copyable: two instruments with the same symbol are not
    // "the same instrument" — they are a data integrity problem.
    Instrument(const Instrument&) = delete;
    Instrument& operator=(const Instrument&) = delete;

    // Movable: ownership can transfer, identity travels with the object.
    Instrument(Instrument&&) noexcept = default;
    Instrument& operator=(Instrument&&) noexcept = default;

    const std::string& symbol() const noexcept { return symbol_; }

    void update_price(PricePoint price) {
        last_price_ = price;
        last_update_ = std::chrono::system_clock::now();
    }

    std::optional<PricePoint> last_price() const noexcept {
        return last_price_;
    }

private:
    std::string symbol_;
    std::optional<PricePoint> last_price_;
    std::chrono::system_clock::time_point last_update_{};
};
```

The instrument is mutable — its price changes — but its identity (the symbol) does not. Deleting copy prevents accidental duplication. If code needs to reference the same instrument from multiple places, it should hold a pointer or reference to a single canonical instance, not a copy. This makes the sharing explicit and auditable.

Equality for identity types, when needed, should compare identity, not state:

```cpp
bool operator==(const Instrument& a, const Instrument& b) noexcept {
    return a.symbol() == b.symbol();
}
```

This is identity equality, not value equality. Two instruments are "the same" if they represent the same symbol, regardless of their current price. Whether to provide this operator at all depends on the use case. If instruments are always accessed through a registry that guarantees uniqueness, pointer equality may be sufficient and less misleading.

### 3.3.3 Separating Concerns: Composed Types

The original `Quote` conflated value, identity, and metadata. The modern design decomposes it:

```cpp
struct QuoteSnapshot {
    std::string symbol;                              // identity key
    PricePoint price;                                // value
    std::chrono::system_clock::time_point timestamp; // value (part of the observation)

    // Equality: same instrument, same price, same time.
    bool operator==(const QuoteSnapshot&) const = default;
};
```

Diagnostic metadata (`last_seen`) and internal mechanisms (`sequence_number`) live elsewhere — in the ingestion pipeline's bookkeeping, not in the domain type. The `QuoteSnapshot` is a value: copyable, comparable, serializable. Its equality is honest because it contains only the fields that define the observation.

For deduplication, the pipeline uses the sequence number as a separate key:

```cpp
class QuoteDeduplicator {
public:
    // Returns true if this is a new quote (not a duplicate).
    bool accept(uint64_t sequence_number, const QuoteSnapshot& quote) {
        auto [it, inserted] = seen_.emplace(sequence_number, quote);
        return inserted;
    }

private:
    std::unordered_map<uint64_t, QuoteSnapshot> seen_;
};
```

The dedup key is explicit. It does not leak into the quote's equality or serialization. The `last_seen` timestamp belongs to the monitoring layer, not the domain model.

### 3.3.4 Enforcing Invariants Beyond Construction

Some types have invariants that span mutation — a sorted container must remain sorted after insertion, a ring buffer's head and tail indices must stay consistent. For these types, the invariant is not just a construction check; it is a class-wide contract.

The standard approach: make all mutation go through member functions that preserve the invariant, and keep the representation private.

```cpp
class BoundedTimeSeries {
public:
    explicit BoundedTimeSeries(std::size_t max_points)
        : max_points_{max_points}
    {
        if (max_points_ == 0)
            throw std::invalid_argument{"max_points must be positive"};
        points_.reserve(max_points_);
    }

    // Invariant: points are in strictly ascending time order,
    // and size never exceeds max_points_.
    void append(std::chrono::system_clock::time_point t, double value) {
        if (!points_.empty() && t <= points_.back().first)
            throw std::invalid_argument{"time must be strictly increasing"};
        if (points_.size() == max_points_)
            points_.erase(points_.begin()); // evict oldest
        points_.emplace_back(t, value);
    }

    std::span<const std::pair<std::chrono::system_clock::time_point, double>>
    points() const noexcept {
        return points_;
    }

    std::size_t size() const noexcept { return points_.size(); }
    std::size_t capacity() const noexcept { return max_points_; }

private:
    std::size_t max_points_;
    std::vector<std::pair<std::chrono::system_clock::time_point, double>> points_;
};
```

Returning `std::span<const ...>` from `points()` gives callers read access without exposing the vector for mutation. The invariant (ascending time, bounded size) is maintained by `append` and cannot be violated from outside the class.

A common temptation is to return a `const std::vector<...>&` instead. This is safe as long as the reference does not outlive the container or survive a mutation that invalidates iterators. `std::span` has the same lifetime hazard but carries slightly less API surface — the caller cannot accidentally call `size()` on the wrong overload or take the address of the vector itself. Neither choice eliminates the need for lifetime discipline; Chapter 1's ownership model applies here.

### 3.3.5 When to Use `const` and When It Lies

`const` in C++ is a shallow, syntactic check. It prevents direct mutation through a particular access path, but it does not prevent mutation through aliased pointers, mutable members, or external state. Relying on `const` as a semantic guarantee of immutability is a design error.

```cpp
// Anti-pattern: const does not mean immutable
class Config {
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> entries_;
    mutable uint64_t access_count_ = 0; // RISK: const methods mutate shared state

public:
    std::optional<std::string> get(const std::string& key) const {
        std::lock_guard lock{mu_};
        ++access_count_; // mutation inside a const method
        auto it = entries_.find(key);
        if (it != entries_.end()) return it->second;
        return std::nullopt;
    }
};
```

The `mutable` keyword is sometimes necessary (mutexes are the canonical case), but `mutable` on observable state like `access_count_` breaks the contract that `const` methods do not change the object's visible behavior. Callers who assume `const` methods are safe to call concurrently without external synchronization will be wrong if the mutable state is not itself properly synchronized.

The rule: `const` means "does not modify the object's abstract value." The mutex is not part of the abstract value; the access count arguably is. If you need observable mutation from a `const` method, document it, synchronize it, and question whether it belongs in this class at all.

### 3.3.6 Strong Types and Phantom Distinctions

Primitive types carry no semantic intent. A function that takes `(double, double, int)` tells you nothing about which double is the bid and which is the ask. In production, swapped arguments cause silent data corruption, not compiler errors.

Strong typedefs fix this. C++23 does not provide a standard strong typedef, but a lightweight wrapper is sufficient:

```cpp
template <typename Tag, typename T>
class StrongType {
public:
    constexpr explicit StrongType(T value) : value_{std::move(value)} {}
    constexpr const T& get() const noexcept { return value_; }

    constexpr bool operator==(const StrongType&) const = default;
    constexpr auto operator<=>(const StrongType&) const = default;

private:
    T value_;
};

using Symbol   = StrongType<struct SymbolTag, std::string>;
using Quantity = StrongType<struct QuantityTag, int64_t>;
using Price    = StrongType<struct PriceTag, double>;
```

Now `place_order(Symbol, Price, Quantity)` is unambiguous at call sites:

```cpp
place_order(Symbol{"AAPL"}, Price{142.50}, Quantity{100});
```

Swapping `Price` and `Quantity` is a compile error, not a runtime surprise. The cost is explicit construction at every boundary, which is the point — it forces the caller to label the intent.

Avoid making strong types implicitly convertible to their underlying type. The entire value of the wrapper is that it resists accidental mixing. Provide `.get()` for the rare cases where the raw value is needed, and let the friction be deliberate.

### 3.3.7 Moved-From State: The Contract You Owe

After a move, the source object must be in a valid but unspecified state. "Valid" means the destructor will not crash and assignment will work. "Unspecified" means you should not read the value.

In practice, this contract is frequently violated in two directions:

1. **Too weak.** A moved-from object is left in a state that causes a crash if anyone touches it, not just reads a value. This is a bug in the move constructor.
2. **Too strong.** Code assumes that moved-from standard containers are empty, and builds logic around that assumption. This works today for all major standard library implementations, but the standard only guarantees "valid but unspecified."

For your own types, document the moved-from state if it matters for your API. If a type manages a resource handle, a moved-from instance should hold a null or sentinel handle, not a dangling one:

```cpp
class Connection {
public:
    Connection(Connection&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)}
    {}

    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ~Connection() { close(); }

    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    void close() noexcept {
        if (handle_) { /* release resource */ }
    }

    void* handle_;
};
```

The `std::exchange` pattern is the standard idiom for move: take the resource, leave the source in a null state. The destructor and `operator bool` both handle the null state correctly. This is the minimum contract.

## 3.4 Tradeoffs and Boundaries

**Immutable values are not free.** If a value type is large (contains a `std::vector` or `std::string`), constructing a new instance for every mutation means allocation. For hot paths, consider a builder pattern or a mutable internal representation with an immutable published snapshot. The immutability is at the API boundary, not necessarily in the implementation.

**Non-copyable entities complicate containers.** You cannot put a non-copyable type in a `std::vector` that needs to reallocate (pre-C++11 this was a hard requirement; now move suffices, but some algorithms still require copyability). Use `std::unique_ptr<Entity>` in containers when you need pointer stability, or use `std::deque` or `std::list` if you need stable addresses without indirection overhead.

**Strong types impose friction at boundaries.** Serialization, logging, and foreign-function interfaces often need raw primitives. Strong types must provide explicit unwrapping, and the conversion code concentrates at the boundary rather than spreading through the domain. This is a feature, not a defect — it makes the boundary visible.

**Defaulted comparison operators interact with inheritance.** If a base class provides `operator==` via `= default`, and a derived class adds fields, the derived class's comparison will only compare the derived fields plus whatever the base class compares. This is correct if the base class comparison is part of the semantic contract, but surprising if the base class is a mixin or an implementation detail. Prefer composition over inheritance for value types to avoid this ambiguity.

**`std::regular` is necessary but not sufficient.** A type can satisfy `std::regular` mechanically (default-constructible, copyable, equality-comparable) while violating the semantic contract — for example, a type whose `operator==` uses pointer identity instead of value comparison. Concept checks catch syntax. Code review catches semantics.

## 3.5 Testing and Tooling Implications

**Test value types with round-trip checks.** For any value type, verify: construct, copy, compare equal, mutate the copy, compare unequal. If the type is serializable, verify that deserialized values compare equal to originals. If the type supports ordering, verify that the ordering is consistent with equality (i.e., `a == b` implies `!(a < b) && !(b < a)`).

```cpp
void test_price_point_round_trip() {
    PricePoint original{100.0, 101.5};
    PricePoint copy = original;
    assert(copy == original);

    PricePoint different{100.0, 102.0};
    assert(different != original);

    // Ordering consistency
    assert(!(original < copy));
    assert(!(copy < original));
}
```

**Test invariant types with boundary values.** For types that enforce invariants at construction, test the rejection path: crossed prices, zero-length buffers, empty strings where identifiers are required. Use death tests or exception assertions to verify that invalid states are unreachable.

**Fuzz composite values.** If a value type participates in hashing, serialization, or comparison, fuzz it. Generate random field combinations, round-trip through serialization, and verify that equality is preserved. This catches fields that were accidentally excluded from `operator==` or the hash function.

**Sanitizer notes.** AddressSanitizer will catch use-after-move bugs where a moved-from object's dangling pointer is dereferenced. MemorySanitizer will flag reads of uninitialized moved-from fields. Neither will catch the higher-level design error of reading a moved-from value and getting a "valid but useless" answer. For that, Clang's `-Wuse-after-move` warning (enabled in `-Wall`) provides static detection of the most common patterns.

**Static analysis.** Clang-tidy checks worth enabling for this chapter's concerns:

- `bugprone-use-after-move` — flags reads from moved-from objects.
- `cppcoreguidelines-special-member-functions` — warns when a class defines some special members but not others (Rule of Five violations).
- `misc-unconventional-assign-operator` — catches assignment operators that return the wrong type or lack self-assignment protection.
- `performance-unnecessary-copy-initialization` — identifies copies that could be const references, relevant when value types get large.

## 3.6 Review Checklist

When reviewing code that introduces or modifies a type, apply these checks:

- [ ] **Value or entity?** Does the type represent a value (defined by its content, freely copyable) or an entity (defined by identity, non-copyable)? Is the choice explicit in the class definition?
- [ ] **Equality semantics.** If `operator==` is provided, does it compare exactly the fields that define sameness for this type? Are diagnostic, caching, or bookkeeping fields excluded?
- [ ] **Invariant enforcement.** Can the type exist in an invalid state? Are invariants checked at construction and preserved by every mutating member function? Are fields that should not change after construction actually immutable?
- [ ] **Copy and move.** Are copy and move explicitly defaulted, deleted, or implemented? If custom, does the move constructor leave the source in a safe state? Does the class follow the Rule of Five (or Rule of Zero)?
- [ ] **Moved-from safety.** Is the moved-from state documented? Can the destructor, assignment operator, and any "is-valid" check handle it?
- [ ] **Strong typing at boundaries.** Are primitive parameters that could be swapped distinguished by type? Are implicit conversions minimized?
- [ ] **`const` honesty.** Do `const` methods actually preserve the abstract value of the object? Is `mutable` used only for non-observable state (e.g., mutexes, caches with identical observable behavior)?
- [ ] **Container compatibility.** If the type goes into a standard container, does it satisfy the container's requirements (movable for `std::vector`, hashable for `std::unordered_map`, ordered for `std::map`)?
- [ ] **Serialization round-trip.** If the type is serialized, does the round-trip preserve equality? Are non-serialized fields handled correctly on deserialization?
- [ ] **Thread safety.** Is the type safe to use from multiple threads? If it is a value type passed by copy, it is inherently safe. If it is an entity with shared state, what synchronization protects it?
