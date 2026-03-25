# Chapter 5: Concepts, Constraints, and Generic Interfaces

> **Prerequisites:** Chapters 1–4. Generic code must respect the ownership contracts (Chapter 1), failure boundaries (Chapter 2), and value-semantic invariants (Chapter 3) of the types it operates on. Chapter 4's compile-time programming techniques (`constexpr`, `consteval`, template metaprogramming) underpin much of what concepts constrain. A template that silently copies a move-only handle, swallows an error result, or breaks a value type's comparison contract is not generic — it is broken in a way the compiler will not catch until the wrong instantiation reaches production.

## 5.1 The Production Problem

Templates let you write an algorithm once and instantiate it for every type that satisfies the algorithm's real requirements. When this works, it removes duplication that would otherwise drift across hand-specialized overloads. When it does not work, you get a different set of costs:

- **Build time.** Every translation unit that includes a template header pays for parsing, instantiation, and optimization of every reachable specialization. In large codebases, template-heavy headers become the dominant contributor to incremental build latency.
- **Diagnostic quality.** An unconstrained template that fails deep inside a nested instantiation produces error messages that reference internal implementation details the caller never intended to interact with. A single missing `operator<` on a user type can produce pages of template backtrace.
- **Accidental coupling.** A template that works "for any T" quietly depends on whatever operations it happens to call. Change an internal helper, and a downstream instantiation breaks — with no explicit interface to blame.
- **Maintenance burden.** Policy-based designs and deeply parameterized class templates create APIs that only the original author can safely extend. New team members read the code and see a language inside the language.

The decision is not whether to use templates. It is how much genericity a component actually needs, and whether the compile-time and cognitive cost is justified by real reuse or a real abstraction boundary.

---

## 5.2 The Legacy Approach: Unconstrained Templates and SFINAE

Before C++20, the only way to restrict template parameters was through SFINAE (Substitution Failure Is Not An Error) and `std::enable_if`. The technique works, but it encodes requirements as arcane type-trait expressions in template parameter lists or return types, and it fails with notoriously unhelpful diagnostics.

### 5.2.1 Anti-pattern: SFINAE soup

```cpp
// Anti-pattern: SFINAE-based constraint
template <typename T,
          typename = std::enable_if_t<
              std::is_default_constructible_v<T> &&
              std::is_copy_assignable_v<T>>,
          typename = std::enable_if_t<
              std::is_invocable_r_v<bool, std::less<>, T const&, T const&>>>
class SortedBuffer {
    std::vector<T> data_;
public:
    void insert(T const& value);
    // BUG: nothing here states that T must be equality-comparable,
    // but remove() calls operator==. The constraint is incomplete,
    // and the compiler error appears inside remove()'s body,
    // not at the point of instantiation.
    void remove(T const& value);
};
```

Problems with this pattern in production:

1. The constraints are partial. The class compiles until someone instantiates `remove()` with a type that lacks `operator==`, at which point the error points into the implementation, not the interface.
2. Adding a new member function that requires a new operation on `T` silently widens the implicit contract. No existing instantiation breaks, but the next one might — and the error will not mention the contract.
3. SFINAE conditions interact badly with overload sets. Two overloads guarded by complementary `enable_if` conditions are fragile under refactoring; reorder or restructure the conditions and you get ambiguity or silent misrouting.
4. The syntax is hostile to review. A reviewer must mentally evaluate the type-trait expressions to understand what types are accepted.

This approach was the best available tool for fifteen years. It is no longer necessary for most use cases.

---

## 5.3 The Modern Approach: Concepts and Constrained Templates

C++20 concepts let you name a set of requirements on a template parameter and enforce them at the point of use, not deep inside the implementation. The compiler checks the constraints before attempting instantiation and reports failures in terms of the concept, not the internal call site.

### 5.3.1 Defining concepts that reflect real contracts

A concept should express the semantic contract the algorithm depends on, not just syntactic validity. The standard library provides building blocks (`std::regular`, `std::totally_ordered`, `std::movable`, etc.), but production code often needs domain-specific concepts that compose these with additional requirements.

```cpp
#include <concepts>
#include <compare>

// A concept for types that can live in a sorted, deduplicating container.
// Requires: regular (copyable, equality-comparable, default-constructible),
// totally ordered (consistent <, >, <=, >=, <=>), and nothrow-movable
// (so the container can provide strong exception safety on reallocation).
template <typename T>
concept SortableElement =
    std::regular<T> &&
    std::totally_ordered<T> &&
    std::is_nothrow_move_constructible_v<T>;
```

This concept is a single, reviewable statement of what `SortedBuffer` requires. It can be tested independently. It appears in diagnostics by name.

### 5.3.2 Using concepts to constrain a class template

```cpp
template <SortableElement T>
class SortedBuffer {
    std::vector<T> data_;
public:
    void insert(T const& value) {
        auto it = std::ranges::lower_bound(data_, value);
        if (it == data_.end() || *it != value) {
            data_.insert(it, value);
        }
    }

    void remove(T const& value) {
        auto it = std::ranges::lower_bound(data_, value);
        if (it != data_.end() && *it == value) {
            data_.erase(it);
        }
    }

    [[nodiscard]] bool contains(T const& value) const {
        return std::ranges::binary_search(data_, value);
    }

    [[nodiscard]] std::span<T const> elements() const noexcept {
        return data_;
    }
};
```

If a user instantiates `SortedBuffer<SomeType>` where `SomeType` lacks `operator==`, the error message names the `SortableElement` concept and the specific sub-requirement that failed. The error appears at the declaration site, not inside `insert()` or `remove()`.

### 5.3.3 Constraining free functions and overload sets

Concepts replace SFINAE in overload resolution cleanly. The compiler picks the most constrained viable overload.

```cpp
// Serialize anything that has a .serialize(Archive&) member.
template <typename T>
concept MemberSerializable = requires(T const& obj, Archive& ar) {
    { obj.serialize(ar) } -> std::same_as<void>;
};

// Serialize anything that has a free-function serialize(Archive&, T const&).
template <typename T>
concept FreeSerializable = requires(T const& obj, Archive& ar) {
    { serialize(ar, obj) } -> std::same_as<void>;
};

template <MemberSerializable T>
void write(Archive& ar, T const& obj) {
    obj.serialize(ar);
}

template <FreeSerializable T>
void write(Archive& ar, T const& obj) {
    serialize(ar, obj);
}

// If a type satisfies both, this is ambiguous — and that ambiguity
// surfaces at the call site as a clear overload-resolution error,
// not a silent priority decision.
```

This is a deliberate design choice. If you want a priority order (prefer the member function when both exist), you add a third overload constrained on the conjunction and delegate:

```cpp
template <typename T>
    requires MemberSerializable<T> && FreeSerializable<T>
void write(Archive& ar, T const& obj) {
    // Policy decision: prefer member serialization.
    obj.serialize(ar);
}
```

The conjunction is more constrained than either individual concept, so overload resolution selects it when both are satisfied.

### 5.3.4 Deducing `this` (C++23): eliminating CRTP and simplifying forwarding

C++23 introduces explicit object parameters — commonly called "deducing `this`" — which let a member function deduce the type and value category of the object it is called on. This eliminates several patterns that previously required CRTP, manual overload sets, or casts.

**Replacing CRTP for mixin interfaces.**

The Curiously Recurring Template Pattern exists primarily to give a base class access to the derived type at compile time. Deducing `this` makes this unnecessary:

```cpp
// Legacy: CRTP mixin
template <typename Derived>
struct Comparable {
    friend bool operator>(Derived const& a, Derived const& b) {
        return b < a;
    }
    friend bool operator<=(Derived const& a, Derived const& b) {
        return !(b < a);
    }
    // ... more boilerplate for >=, etc.
};

struct Widget : Comparable<Widget> {
    int id;
    bool operator<(Widget const& other) const { return id < other.id; }
};

// Modern: deducing this, no CRTP
struct Comparable2 {
    template <typename Self>
    bool operator>(this Self const& self, Self const& other) {
        return other < self;
    }
    template <typename Self>
    bool operator<=(this Self const& self, Self const& other) {
        return !(other < self);
    }
};

struct Widget2 : Comparable2 {
    int id;
    bool operator<(Widget2 const& other) const { return id < other.id; }
};
```

The CRTP version requires the derived class to name itself in the base class template argument — a fragile coupling. The deducing-`this` version is a plain base class. It composes, inherits, and refactors like any other non-template class.

**Forwarding member functions without overload duplication.**

Before C++23, providing both `const&` and `&&` qualified accessors required writing the same function body twice (or using a private template with `static_cast`):

```cpp
// Legacy: duplicated accessors
class Container {
    std::vector<Item> items_;
public:
    std::vector<Item> const& items() const& { return items_; }
    std::vector<Item>&&      items() &&     { return std::move(items_); }
};

// Modern: single function, deducing this
class Container2 {
    std::vector<Item> items_;
public:
    template <typename Self>
    auto&& items(this Self&& self) {
        return std::forward<Self>(self).items_;
    }
};
```

The single deducing-`this` overload handles `const&`, `&`, and `&&` calls correctly, deducing the appropriate reference qualification from the call site.

**Recursive lambdas.**

Before deducing `this`, a lambda could not call itself without capturing a `std::function` or using a Y-combinator wrapper. Explicit object parameters make recursion direct:

```cpp
auto traverse = [](this auto const& self, TreeNode const& node) -> void {
    process(node);
    for (auto const& child : node.children()) {
        self(child);  // direct recursion — no std::function, no wrapper
    }
};

traverse(root);
```

This avoids the heap allocation and type-erasure cost of `std::function` while keeping the lambda's capture and locality benefits.

**When to prefer deducing `this` over CRTP.** Any new code that would reach for CRTP purely to inject interface members should use deducing `this` instead. Existing CRTP that works and is not being actively modified does not need migration — the benefit is in new code clarity, not in chasing existing patterns.

---

## 5.4 When Not to Write Generic Code

Concepts improve the experience of writing and consuming templates. They do not change the fundamental question: does this component need to be generic at all?

### 5.4.1 The one-instantiation test

If a template is instantiated with exactly one type in your codebase, it is not generic code. It is a regular function or class with extra compile-time cost. This is common in application-level code where someone writes `template <typename Logger>` and every call site passes `ProductionLogger`. The template bought nothing. It added a header dependency, increased build time, and made the function harder to find in a debugger.

The correct response is to use the concrete type until a second, genuinely different type appears. Premature generalization is not cheaper in C++ than in any other language; it is more expensive because the cost is paid in every translation unit that sees the header.

### 5.4.2 The abstraction test

Generic code is justified when it provides an abstraction that concrete code cannot: algorithms over arbitrary ranges, containers parameterized by allocator policy, serialization frameworks that operate on user-defined types. In each case, the template exists because the set of types it operates on is open-ended and controlled by the caller.

If the set of types is closed and known at design time, a `std::variant` or a simple class hierarchy is often clearer, cheaper to compile, and easier to debug.

### 5.4.3 The header cost test

Every template that lives in a header is parsed by every translation unit that includes it, directly or transitively. A 200-line template header included by 400 translation units costs 80,000 lines of parsing — before instantiation. If the template is only used in a few of those units, the rest are paying for nothing.

Mitigation strategies:

- **Explicit instantiation.** Define the template in a `.cpp` file, explicitly instantiate it for the known types, and expose only declarations in the header. This eliminates redundant instantiation across translation units.
- **Extern template declarations.** Declare `extern template class SortedBuffer<int>;` in the header to suppress implicit instantiation in every translation unit, then provide the instantiation in one `.cpp` file.
- **Forward declarations and type erasure.** If the template is an implementation detail behind a stable interface, erase the type at the boundary. The public API takes a concrete or type-erased handle; the template lives in the implementation file.

```cpp
// sorted_buffer.h — public header, no template exposure
#include <span>
#include <cstdint>

class IntBuffer {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    IntBuffer();
    ~IntBuffer();
    IntBuffer(IntBuffer&&) noexcept;
    IntBuffer& operator=(IntBuffer&&) noexcept;

    void insert(int value);
    void remove(int value);
    [[nodiscard]] bool contains(int value) const;
    [[nodiscard]] std::span<int const> elements() const noexcept;
};

// sorted_buffer.cpp — template instantiated once, behind the wall
#include "sorted_buffer.h"
#include "sorted_buffer_impl.h"  // contains SortedBuffer<T>

struct IntBuffer::Impl {
    SortedBuffer<int> buf;
};

IntBuffer::IntBuffer() : impl_(std::make_unique<Impl>()) {}
IntBuffer::~IntBuffer() = default;
IntBuffer::IntBuffer(IntBuffer&&) noexcept = default;
IntBuffer& IntBuffer::operator=(IntBuffer&&) noexcept = default;

void IntBuffer::insert(int value) { impl_->buf.insert(value); }
void IntBuffer::remove(int value) { impl_->buf.remove(value); }
bool IntBuffer::contains(int value) const { return impl_->buf.contains(value); }
std::span<int const> IntBuffer::elements() const noexcept { return impl_->buf.elements(); }
```

This costs one virtual-dispatch-equivalent indirection (the pointer chase through `Impl`), but it eliminates the template from every consumer's compile path. For application-level code where `SortedBuffer<int>` is the only instantiation, this is almost always the right trade.

---

## 5.5 Policy-Based Design: Power and Cost

Policy-based design parameterizes a class on behavioral strategies — allocation policy, thread-safety policy, logging policy — as template parameters. The technique produces highly configurable components. It also produces components that are difficult to use, test, and maintain when the number of policies grows.

### 5.5.1 A measured example

```cpp
template <typename T,
          typename Allocator = std::allocator<T>,
          typename MutexPolicy = NullMutex>
class ObjectPool {
    std::vector<T, Allocator> storage_;
    std::vector<bool> available_;
    [[no_unique_address]] MutexPolicy mutex_;

public:
    explicit ObjectPool(std::size_t capacity, Allocator alloc = {})
        : storage_(capacity, alloc), available_(capacity, true) {}

    [[nodiscard]] T* acquire() {
        auto lock = mutex_.lock();
        for (std::size_t i = 0; i < available_.size(); ++i) {
            if (available_[i]) {
                available_[i] = false;
                return &storage_[i];
            }
        }
        return nullptr;
    }

    void release(T* obj) {
        auto lock = mutex_.lock();
        auto idx = static_cast<std::size_t>(obj - storage_.data());
        available_[idx] = true;
    }
};
```

Two policies is manageable. The `NullMutex` default means single-threaded code pays nothing. A `SpinMutex` or `std::mutex`-wrapping policy enables thread-safe use. The allocator policy integrates with standard container conventions.

### 5.5.2 Anti-pattern: policy explosion

```cpp
// Anti-pattern: too many policies
template <typename T,
          typename Allocator,
          typename MutexPolicy,
          typename GrowthPolicy,
          typename ExpirationPolicy,
          typename MetricsPolicy,
          typename ValidationPolicy>
class ObjectPool {
    // RISK: each combination of policies is a distinct type.
    // Testing the cross-product is impractical.
    // Users cannot store different ObjectPool instantiations
    // in a homogeneous container or pass them through a common interface
    // without type erasure.
};
```

Every template parameter multiplies the number of distinct types the system must compile, link, test, and reason about. Three policies with two implementations each produce eight instantiations. Six policies with three implementations each produce 729.

The guideline: if you need more than two or three policy parameters, consider whether runtime polymorphism (a `std::function`, a virtual interface, or a type-erased wrapper) would serve the same purpose with less compile-time and cognitive cost. Policies that are performance-critical (allocators, mutex strategies) earn their place. Policies that are configuration choices (logging, metrics) rarely do.

---

## 5.6 Template and Concept Compilation Cost

> For general compile-time measurement tools and techniques (`-ftime-trace`, ClangBuildAnalyzer, precompiled headers, etc.), see Chapter 4. This section covers compilation costs specific to templates and concepts as a design tradeoff.

### 5.6.1 Concept-check cost

Concepts are checked at every constrained declaration, not just at instantiation. A concept that requires evaluating complex `requires` expressions with nested substitutions can become expensive when it appears in heavily-used overload sets.

Keep concept definitions shallow. Compose from standard concepts and simple type traits. Avoid `requires` expressions that trigger deep template instantiation as part of the check itself.

### 5.6.2 Controlling instantiation spread

Every template that lives in a header is parsed and potentially instantiated in every translation unit that includes it. The two most effective mitigations are specific to template design:

**Explicit instantiation definitions.**

```cpp
// widget_serializer.h
template <typename T>
void serialize_widget(Archive& ar, T const& widget);

// Suppress implicit instantiation in every includer.
extern template void serialize_widget<Widget>(Archive&, Widget const&);
extern template void serialize_widget<GadgetWidget>(Archive&, GadgetWidget const&);

// widget_serializer.cpp
#include "widget_serializer.h"
// Provide the definitions.
template void serialize_widget<Widget>(Archive&, Widget const&);
template void serialize_widget<GadgetWidget>(Archive&, GadgetWidget const&);
```

This moves instantiation cost from N translation units to one. For templates with a known, stable set of instantiations, this is the single most effective template-specific compile-time optimization.

**Avoid gratuitous `auto` in return types.**

A function declared `auto f(auto x)` is an unconstrained template. Every call site triggers a new instantiation. In a header, this multiplies across every translation unit. Use concrete return types or constrained templates when the function's interface is stable.

```cpp
// Anti-pattern: unconstrained abbreviated template in a header
// BUG: every distinct argument type produces a new instantiation.
inline auto compute(auto const& input) {
    return input.process();
}

// Prefer: constrained and explicit
template <Processable T>
ProcessResult compute(T const& input) {
    return input.process();
}
```

---

## 5.7 Testing and Tooling Implications

### 5.7.1 Testing concept conformance

A concept is a checkable predicate. Test it with `static_assert`:

```cpp
// test_concepts.cpp
#include "sorted_buffer.h"
#include "our_types.h"

// Positive: types that should satisfy the concept.
static_assert(SortableElement<int>);
static_assert(SortableElement<std::string>);
static_assert(SortableElement<CustomerId>);

// Negative: types that should not.
static_assert(!SortableElement<std::unique_ptr<int>>);  // not copyable
static_assert(!SortableElement<NonComparable>);          // no ordering
static_assert(!SortableElement<ThrowingMover>);          // move may throw
```

These assertions cost nothing at runtime and fail at compile time with a clear message. They serve as regression tests for the concept definition itself: if someone widens or narrows a concept, the static assertions catch the change immediately.

### 5.7.2 Testing generic code with archetype types

To verify that a template uses only the operations guaranteed by its concept, instantiate it with a minimal archetype — a type that satisfies the concept and nothing more.

```cpp
// A minimal type satisfying SortableElement — and nothing else.
struct Archetype {
    int value;

    Archetype() = default;
    Archetype(Archetype const&) = default;
    Archetype& operator=(Archetype const&) = default;
    Archetype(Archetype&&) noexcept = default;
    Archetype& operator=(Archetype&&) noexcept = default;

    auto operator<=>(Archetype const&) const = default;
    bool operator==(Archetype const&) const = default;
};

static_assert(SortableElement<Archetype>);

// Force full instantiation.
template class SortedBuffer<Archetype>;
```

If `SortedBuffer` accidentally calls `std::hash<Archetype>` or `operator<<`, the archetype instantiation fails — revealing an undocumented requirement that the concept did not capture.

### 5.7.3 Tooling for template diagnostics

- **`-fconcepts-diagnostics-depth=N` (GCC).** Controls how many levels of concept-check failure the compiler reports. Increase it when a nested concept failure is unclear; decrease it when the output is overwhelming.
- **`-ftemplate-backtrace-limit=N` (Clang/GCC).** Limits the depth of template instantiation backtraces. In production builds, a limit of 10–20 is usually sufficient. Set it higher only when debugging a specific instantiation failure.
- **`/diagnostics:caret` (MSVC).** Provides caret-based error locations that are more readable for concept failures than the default format.

---

## 5.8 Tradeoffs and Boundaries

### 5.8.1 Concepts vs. virtual interfaces

Concepts provide compile-time polymorphism: zero runtime overhead, full inlining, but separate binary code for each instantiation. Virtual interfaces provide runtime polymorphism: one binary code path, indirect-call overhead, and no instantiation cost.

The decision matrix:

| Criterion | Concepts | Virtual interface |
|---|---|---|
| Set of types | Open, controlled by downstream code | Open, but requires inheritance |
| Runtime cost | None (inlined) | Virtual dispatch, potential cache miss |
| Compile cost | Per-instantiation | Once |
| Binary size | Grows with distinct instantiations | Fixed |
| Debugging | Each instantiation is a separate function | One function, indirect call |
| ABI stability | None (templates are source-level) | Stable across shared library boundaries |

For hot paths with a small, known set of types, concepts win on performance. For plugin architectures, shared library boundaries, and designs where binary size matters, virtual interfaces win on deployment cost.

### 5.8.2 Concepts do not check semantics

A concept verifies syntax: "this expression compiles." It does not verify semantics: "this expression does what the algorithm expects." A type can satisfy `std::totally_ordered` while implementing `operator<=>` incorrectly (violating transitivity, for instance). The concept will pass. The algorithm will produce wrong results.

This means concepts reduce diagnostic noise and document intent, but they do not replace runtime testing. A `SortedBuffer` instantiated with a type whose ordering is inconsistent will corrupt its internal invariant silently. Tests with concrete types remain necessary.

### 5.8.3 `if constexpr` vs. concept overloads

When a function needs to handle two cases — say, types with a `.size()` member and types without — there are two mechanisms:

```cpp
// Option A: if constexpr
template <typename T>
void log_info(T const& obj) {
    if constexpr (requires { obj.size(); }) {
        std::println("size={}", obj.size());
    } else {
        std::println("(no size)");
    }
}

// Option B: concept overloads
template <typename T> requires requires { std::declval<T const&>().size(); }
void log_info(T const& obj) {
    std::println("size={}", obj.size());
}

template <typename T>
void log_info(T const& obj) {
    std::println("(no size)");
}
```

`if constexpr` keeps both branches in one function, which is easier to read when the branches are small and share context. Concept overloads are preferable when the branches are large, independently testable, or part of an extensible overload set that third-party code may add to.

The tradeoff is readability vs. extensibility. Pick based on which pressure is real in your codebase.

---

## 5.9 Review Checklist

Use these questions during code review for any change that introduces or modifies template code.

- [ ] **Is the template instantiated with more than one type?** If not, replace it with a concrete implementation until a second type appears.
- [ ] **Are all requirements on template parameters expressed as named concepts?** Unconstrained `typename T` or `auto` parameters should have a documented justification (e.g., forwarding reference in a thin wrapper).
- [ ] **Does the concept match the actual requirements?** Instantiate with an archetype type that satisfies the concept and nothing more. If the instantiation fails, the concept is too narrow or the implementation uses undocumented operations.
- [ ] **Are concept definitions shallow?** Concepts that trigger deep template instantiation during checking add compile-time cost at every use.
- [ ] **Is the template in a header?** If so, is that necessary? Can it be moved behind an explicit instantiation or a type-erased interface?
- [ ] **Have you measured compile-time impact?** Use `-ftime-trace` or equivalent before and after the change. A template that adds 100ms per translation unit across 200 units adds 20 seconds to the build.
- [ ] **Are `extern template` declarations used for known instantiations?** Every template instantiated in more than a few translation units should have an explicit instantiation in one `.cpp` file and `extern template` declarations in the header.
- [ ] **Does the policy-based design have three or fewer policy parameters?** If more, justify why runtime polymorphism or a simpler design is insufficient.
- [ ] **Are negative concept checks tested?** `static_assert(!MyConcept<BadType>)` prevents accidental concept widening during refactoring.
- [ ] **Does the generic code respect ownership and failure contracts?** A template that copies, moves, or destroys its parameter type must do so in a way consistent with the contracts established in Chapters 1–4. Verify that `noexcept` specifications, move semantics, and error propagation are correct for all constrained types.
