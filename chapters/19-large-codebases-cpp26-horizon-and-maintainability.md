# Chapter 19: Large Codebases, C++26 Horizon, and Maintainability

> **Prerequisites:** All earlier chapters (Chapters 1--18). This chapter synthesizes the book's technical material into review, maintenance, and forward-looking design practices. It references ownership models (Chapter 1), failure boundaries (Chapter 2), value semantics (Chapter 3), generic code costs (Chapter 4), API shape (Chapter 5), binary boundaries (Chapter 6), concurrency contracts (Chapters 7--9), data layout (Chapters 10--11), measurement discipline (Chapter 12), build and tooling infrastructure (Chapter 13), verification strategy (Chapter 14), migration sequencing (Chapter 15), vocabulary types and standard library evolution (Chapter 16), compile-time programming (Chapter 17), and coroutines and asynchronous design (Chapter 18). If a concept below feels unfamiliar, the prerequisite chapter is the right place to look.

---

## 19.1 The Production Problem

Small codebases forgive inconsistency. A team of three can hold the ownership model, error policy, and threading rules in working memory. Reviews are fast because every reviewer has seen every file. Build times are tolerable. Abstraction drift stays contained because the same people who designed the abstraction are still writing the callers.

None of that survives scale.

At several hundred thousand lines, different subsystems start adopting incompatible conventions. One team wraps error results in `std::expected`, another throws, a third returns raw status codes and logs on failure. Smart pointer usage drifts: `shared_ptr` appears where unique ownership was sufficient, or raw pointers cross boundaries with no documented lifetime contract. Template-heavy utilities accumulate in `util/` directories with no owner and no tests. Review turnaround slows because reviewers cannot tell whether a change follows subsystem conventions or accidentally violates them.

These problems are not caused by ignorance. They are caused by the absence of reviewable, enforceable structure at the points where humans make decisions: code review, API approval, dependency introduction, and release gating. The technical material in earlier chapters gives teams the vocabulary to describe ownership, failure, concurrency, and performance contracts. This chapter is about making that vocabulary operational across a codebase that no single person can hold in their head.

---

## 19.2 The Naive Approach: Style Guides and Good Intentions

Most teams begin with a style guide. The guide says "prefer `unique_ptr` over `shared_ptr`," "use RAII for resource management," and "avoid raw `new`." These are fine sentences. They do not prevent the problems described above.

Style guides fail at scale for three reasons:

1. **They describe syntax, not design contracts.** A rule like "use `std::expected` for recoverable errors" says nothing about whether a given failure is recoverable, where the failure boundary lives, or what the caller is expected to do with the error. Two developers can both follow the rule and produce incompatible error-handling strategies.

2. **They are not scoped to boundaries.** A global rule applies identically to a leaf utility and a service entry point. But the cost of a wrong choice is vastly different at those two locations. Ownership policy at a public API boundary has system-wide consequences; ownership policy inside a three-line helper does not.

3. **They lack enforcement teeth.** A rule that depends on reviewer memory will be followed inconsistently. Reviewers have limited attention. If the guide has 200 rules, each review checks a biased subset — usually the rules the reviewer learned most recently or was burned by most painfully.

The result is a codebase where the guide exists, everyone claims to follow it, and the actual code reflects dozens of local dialects.

```cpp
// Anti-pattern: three modules in the same codebase, three error contracts.

// Module A: exceptions
// BUG: caller in Module C catches std::exception, never sees this type.
class ParseError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Module B: expected
// RISK: error type is std::string — no structured diagnosis, no mapping to
// Module A's exception hierarchy.
auto load_config(std::string_view path) -> std::expected<Config, std::string>;

// Module C: errno-style
// BUG: caller must check return value AND errno. Omitting either check
// compiles cleanly and fails silently.
int connect_to_peer(const Endpoint& ep);
```

No amount of local correctness fixes this. The problem is that no one decided — at the boundary level — what the error contract is for code that crosses module lines.

---

## 19.3 The Modern C++ Approach: Review Heuristics Tied to Design Boundaries

The alternative to a flat style guide is a set of **review heuristics** organized around the boundaries where design decisions have the highest cost of future change. A heuristic is not a rule. It is a question that a reviewer asks when examining code at a specific kind of boundary. The answer determines whether the change is locally acceptable or needs broader discussion.

### 19.3.1 Boundary Classification

Not all code deserves the same review intensity. Classify locations in the codebase by their blast radius:

| Boundary type | Examples | Cost of wrong choice |
|---|---|---|
| **Public API** | Library headers, service endpoints, plugin interfaces | Breaking change affects all consumers. May require ABI-compatible fix or deprecation cycle. |
| **Module boundary** | Internal header exposed to sibling modules | Inconsistency propagates across teams. Fixing requires coordinated migration. |
| **Subsystem internal** | Implementation files, private headers | Local cost only. Refactoring is contained. |

Review effort should be proportional to boundary type. A `shared_ptr` in a private implementation detail is a style preference. A `shared_ptr` in a public API is an ownership contract that every consumer inherits.

### 19.3.2 Ownership and Lifetime Heuristics

At every boundary that passes a pointer, reference, or handle:

- **Can the reviewer state the lifetime contract in one sentence?** If not, the interface is underspecified. "Caller retains ownership" or "callee takes unique ownership via `unique_ptr`" are sentences. "It depends on the context" is not.
- **Does the transfer mechanism match the contract?** A function that takes `unique_ptr<T>` by value documents ownership transfer in the type system. A function that takes `T*` documents nothing — the reviewer must find a comment or read the implementation.
- **Is `shared_ptr` justified by genuinely shared, non-hierarchical lifetime?** In practice, most `shared_ptr` usage in large codebases is not shared ownership. It is deferred ownership design. The reviewer should ask: "Who is the last owner, and can we make that explicit?"

```cpp
// A boundary where the reviewer can state the contract immediately.
class SessionManager {
public:
    // Takes ownership. Session lives until SessionManager decides to expire it.
    void register_session(std::unique_ptr<Session> session);

    // Borrows. Caller must not store the pointer beyond the call.
    // Returns nullptr if session has expired.
    Session* find_session(SessionId id) const;
};
```

```cpp
// Anti-pattern: ownership is invisible at the boundary.
class SessionManager {
public:
    // BUG: does the manager own this? Does the caller? Who deletes?
    void register_session(Session* session);

    // RISK: is this a borrowed pointer or a transferred one?
    // Reviewer cannot tell without reading the implementation.
    Session* find_session(SessionId id) const;
};
```

### 19.3.3 Error Contract Heuristics

At every module boundary:

- **Is the failure domain documented?** A function that returns `std::expected<T, E>` with a well-typed `E` communicates more than one that throws an unspecified exception. But either is acceptable if the contract is stated and consistent across the module boundary.
- **Can the caller distinguish transient from permanent failure?** If the caller cannot, retry logic and circuit-breaker patterns become guesswork.
- **Does the error cross a failure boundary?** If so, is it translated at the boundary, or does it leak implementation detail? A database driver error surfacing in an HTTP response is a failure boundary violation (Chapter 2).

### 19.3.4 Concurrency Contract Heuristics

At every boundary where data may be accessed from multiple threads:

- **Is thread safety documented per type, not per function?** A type that is "thread-safe for reads, not writes" is a contract. A function that "should be called under a lock" is a hope.
- **Does the API force correct synchronization, or merely permit it?** An API that hands out a raw reference to internally synchronized state is one refactor away from a data race. An API that returns a copy, a snapshot, or a guard object encodes the contract in the type system.

```cpp
// The guard pattern: synchronization is structural, not manual.
class MetricsRegistry {
public:
    // Returns a locked view. The caller cannot forget to synchronize.
    class LockedView {
        friend class MetricsRegistry;
        explicit LockedView(std::mutex& mu, std::unordered_map<std::string, Counter>& data)
            : lock_(mu), data_(data) {}
        std::unique_lock<std::mutex> lock_;
        std::unordered_map<std::string, Counter>& data_;
    public:
        Counter& operator[](const std::string& name) { return data_[name]; }
        auto begin() { return data_.begin(); }
        auto end()   { return data_.end(); }
    };

    LockedView lock() { return LockedView{mu_, counters_}; }

private:
    std::mutex mu_;
    std::unordered_map<std::string, Counter> counters_;
};
```

```cpp
// Anti-pattern: synchronization depends on caller discipline.
class MetricsRegistry {
public:
    std::mutex mu;  // BUG: public mutex. Callers may forget to lock.
    std::unordered_map<std::string, Counter> counters;  // RISK: direct access.
};
```

### 19.3.5 Dependency Introduction Heuristics

When a change adds a new dependency — a third-party library, a new internal module import, or a new template instantiation chain:

- **What is the transitive cost?** A header-only library with 50,000 lines of templates affects every translation unit that includes it. A compiled library with a stable ABI affects link time but not compile time. The reviewer should ask about build cost, not just functionality.
- **Who owns the dependency going forward?** If the upstream is unmaintained, the team inherits its bugs. If the dependency has an incompatible license, legal exposure follows. If the dependency is internal but unowned, it will rot.
- **Does the dependency pull in policy?** A logging library that installs a global handler, an allocator library that replaces `operator new`, or a serialization library that requires macro registration — each imposes policy on the entire codebase. That is a different category of dependency than a leaf utility.

```cpp
// A dependency audit comment. Cheap to write, valuable in review.
// Dependency: absl::flat_hash_map (abseil-cpp 20240722)
// Justification: 40% lower memory overhead than std::unordered_map for
//   our key/value profile (measured in bench/hash_map_bench.cc).
// Transitive cost: header-only for this container; adds ~2s to
//   affected TUs on full rebuild (measured).
// Owner: platform-libs team.
#include "absl/container/flat_hash_map.h"
```

This is not bureaucracy. It is a reviewable record of why the dependency exists and who is responsible when it breaks.

---

## 19.4 Tradeoffs and Boundaries

### 19.4.1 Heuristic Review vs. Mechanical Enforcement

Heuristics require human judgment. That is their strength — they handle ambiguity that linters cannot parse — and their weakness — they depend on reviewer expertise and attention. The practical approach is a layered defense:

| Layer | Mechanism | What it catches |
|---|---|---|
| **Automated** | Clang-Tidy checks, compiler warnings, custom lint rules | Syntax-level violations: raw `new`, missing `[[nodiscard]]`, known anti-patterns. |
| **Structural** | CODEOWNERS files, module boundary annotations, build visibility rules | Boundary violations: unauthorized dependencies, unreviewed API changes. |
| **Heuristic** | Human review guided by the questions above | Design violations: wrong ownership model, leaked failure detail, unguarded concurrency. |

Automated checks should be exhaustive for the rules they cover. Structural controls should prevent the highest-cost mistakes (unapproved public API changes, unvetted dependency additions). Heuristic review should focus on the questions that require design judgment.

### 19.4.2 The Cost of Abstraction Layers

Large codebases develop abstraction layers to contain complexity. Each layer has a maintenance cost:

- **A wrapper that adds nothing.** A `StringHelper` class that wraps `std::string` with identical semantics is pure cost. It obscures the underlying type, breaks generic code expectations, and requires maintenance when the standard evolves.
- **A wrapper that enforces an invariant.** A `NonEmptyString` type that validates on construction adds real value if the invariant matters at the boundary. The cost is the wrapper's API surface and the conversion overhead at entry points.
- **An abstraction that hides a volatile dependency.** A `DatabaseConnection` interface that shields callers from the specific driver is valuable if the driver may change and expensive if it never will. The reviewer should ask: "Has this dependency ever changed, or is the abstraction speculative?"

The heuristic is straightforward: abstractions earn their keep when they protect a boundary that has actually shifted, or when they enforce an invariant that has actually been violated. Speculative abstraction is technical debt with a respectable name.

### 19.4.3 Knowledge Distribution

In a small team, knowledge lives in people. In a large codebase, knowledge must live in artifacts: documentation next to the code it describes, test cases that encode invariants, and commit messages that explain why a decision was made rather than what changed.

The most effective knowledge artifact is a **design comment at the boundary**:

```cpp
// Connection pool design:
// - Pool owns all connections via unique_ptr.
// - Callers borrow via raw pointer from checkout(); must return via checkin().
// - Pool is thread-safe. Individual connections are not.
// - Shutdown drains in-flight checkouts with a 5-second timeout,
//   then force-closes. See shutdown_test.cc for the race-condition
//   regression test.
class ConnectionPool { /* ... */ };
```

This comment is worth more than a wiki page because it lives where the decision is enforced and is visible in every review that touches the class.

---

## 19.5 Testing and Tooling Implications

### 19.5.1 Enforcing Boundaries with Build Systems

Build visibility rules are the most reliable structural enforcement mechanism. In Bazel, CMake (with appropriate discipline), or build systems that support visibility:

```python
# Bazel BUILD: only approved consumers can depend on the connection pool.
cc_library(
    name = "connection_pool",
    hdrs = ["connection_pool.h"],
    srcs = ["connection_pool.cc"],
    visibility = [
        "//services/auth:__pkg__",
        "//services/session:__pkg__",
        # New consumers require review from platform-libs.
    ],
)
```

Without visibility rules, dependency sprawl is governed by the filesystem and the honor system. In a codebase with thousands of targets, the honor system fails.

### 19.5.2 Static Analysis as Review Automation

Clang-Tidy and custom checks can encode team-specific heuristics:

```yaml
# .clang-tidy: project-specific checks layered on top of defaults.
Checks: >
  -*,
  bugprone-*,
  cppcoreguidelines-avoid-non-const-global-variables,
  cppcoreguidelines-owning-memory,
  modernize-use-override,
  readability-identifier-naming,
  performance-unnecessary-copy-initialization,
  # Custom: flag shared_ptr in public API headers.
  # Requires team-written check; see tools/clang-tidy-checks/.
  my-project-no-shared-ptr-in-public-api
```

Custom checks are worth writing for rules that are both high-value and high-frequency. A rule that fires once a year is not worth the tooling investment. A rule that prevents a class of bug that has caused three incidents is.

### 19.5.3 Architectural Tests

Some invariants are structural rather than behavioral: "module A never depends on module B," "no header in `public/` includes a header from `internal/`," "every class in `api/` has a `[[nodiscard]]` on its factory function." These are testable assertions about the codebase itself.

```cpp
// Architectural test: verify no public header includes an internal header.
// Runs as part of CI, not at runtime.
#include <filesystem>
#include <fstream>
#include <string>
#include <cassert>
#include <print>

auto contains_internal_include(const std::filesystem::path& header) -> bool {
    std::ifstream in(header);
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("#include") && line.contains("/internal/")) {
            std::println(stderr, "Violation: {} includes internal header: {}",
                         header.string(), line);
            return true;
        }
    }
    return false;
}

int main() {
    bool violation = false;
    for (auto& entry : std::filesystem::recursive_directory_iterator("include/public")) {
        if (entry.path().extension() == ".h") {
            violation |= contains_internal_include(entry.path());
        }
    }
    return violation ? 1 : 0;
}
```

This test is crude. It does not understand the preprocessor, conditional includes, or transitive closure. For most teams, crude is sufficient — it catches 95% of violations, costs an hour to write, and runs in milliseconds. If the remaining 5% matters, invest in a proper include-graph analysis tool.

### 19.5.4 Code Ownership and Review Routing

CODEOWNERS files (or their equivalent in your review system) should reflect boundary classification:

```
# CODEOWNERS: route reviews based on boundary type.

# Public API: requires platform-libs approval.
/include/public/           @platform-libs-team

# Module boundaries: requires owning team + one platform-libs member.
/src/services/auth/api.h   @auth-team @platform-libs-team

# Internal implementation: owning team only.
/src/services/auth/         @auth-team
```

The goal is that changes to high-cost boundaries are reviewed by people who understand the system-wide implications, while changes to internal implementation move at team speed.

---

## 19.6 Maintaining Maintainability Over Time

### 19.6.1 Abstraction Drift Detection

Abstractions rot when their usage diverges from their original intent. A `Cache<K, V>` designed for read-heavy workloads accumulates write-heavy callers. A `TaskQueue` designed for CPU-bound work starts hosting I/O-bound tasks with unbounded latency.

Detection is manual but can be assisted:

- **Usage audits.** Periodically grep for all instantiations of a generic type and verify that usage patterns still match the design assumptions documented in the type's boundary comment.
- **Performance regression tests.** If a type was chosen for its performance characteristics, a benchmark that exercises the expected workload detects drift when new callers change the profile (Chapter 12).
- **Deprecation as a forcing function.** When an abstraction has drifted beyond repair, deprecate it with a clear migration target and a timeline. Leaving a broken abstraction in place because "someone might be using it" guarantees that someone will keep using it.

### 19.6.2 The Cost of Inconsistency

Inconsistency in a large codebase is not merely an aesthetic problem. It has measurable costs:

- **Review latency.** Reviewers must determine which local convention applies before they can evaluate correctness. In a consistent codebase, convention is implicit.
- **Bug surface.** At the boundary between two inconsistent conventions, translation code is required. Translation code is where bugs live — off-by-one conversions, exception-to-error-code mismatches, lifetime confusion between owning and borrowing conventions.
- **Onboarding time.** New team members learn patterns by reading code. If the codebase teaches three different error-handling patterns, the new developer will use whichever they encountered first, propagating randomness.

The practical response is not to rewrite for consistency, but to establish a **preferred convention per boundary type** and enforce it only at boundaries. Internal implementation can be inconsistent as long as it does not leak across module lines. This gives teams freedom to migrate incrementally without a codebase-wide flag day.

### 19.6.3 Commit Messages and Decision Records

A commit that says "refactored error handling" tells the reviewer nothing about why the change was made, what alternatives were considered, or what constraints shaped the decision. A commit that says "switch auth module from exceptions to std::expected at the service boundary; exceptions were leaking across the gRPC serialization layer (see incident #4127)" gives future maintainers a trail they can follow.

For decisions that affect multiple modules or establish precedent, a lightweight Architecture Decision Record (ADR) — a short document in the repository that states the context, decision, and consequences — is worth the ten minutes it takes to write. The alternative is that the decision lives in a Slack thread that no one will find in six months.

---

## 19.7 C++26 Horizon: Features That Change Design Decisions

> **[C++26]** The features in this section target C++26. As of this writing, the core proposals have been adopted into the C++26 working draft or are on track for inclusion. Compiler support is partial and evolving. The design guidance here is based on the proposals as accepted; details may shift before the standard is finalized. Each subsection ends with concrete steps teams can take now, in C++23, to prepare.

C++26 is not a minor maintenance release. Three features — contracts, pattern matching, and reflection — change the vocabulary available for expressing design intent at boundaries. Each one directly affects the review heuristics, enforcement mechanisms, and abstraction trade-offs discussed earlier in this chapter. Teams that understand these features now can make C++23 design decisions that align with the migration path rather than fight it.

### 19.7.1 Contracts (P2900): Enforceable Interface Specifications

**The production problem contracts solve.** Large codebases accumulate preconditions and postconditions as comments, `assert()` macros, or ad-hoc validation at function entry. These mechanisms share a deficiency: they are invisible to the type system, inconsistently enforced across build configurations, and unreadable to tooling. A comment that says "index must be less than size()" is a wish. An `assert(index < size())` is slightly better — it crashes in debug builds — but vanishes in release, exactly when the invariant matters most in production.

The deeper problem is that preconditions and postconditions are *interface* properties, but `assert` is an *implementation* mechanism. Callers cannot see assertions inside function bodies. Reviewers must open the implementation to discover the contract. Static analysis tools cannot reliably extract preconditions from scattered `assert` calls.

**What C++26 contracts provide.** The contracts facility (P2900) adds `pre`, `post`, and `contract_assert` as first-class language constructs:

```cpp
// [C++26] Contracts: preconditions and postconditions are part of the declaration.
class RingBuffer {
public:
    auto push(int value) -> void
        pre(not full())          // caller's obligation: buffer has space
        post(not empty());       // implementation's guarantee: buffer is non-empty after push

    auto pop() -> int
        pre(not empty())         // caller's obligation: buffer is non-empty
        post(not full());        // implementation's guarantee: buffer has space after pop

    [[nodiscard]] auto size() const noexcept -> std::size_t
        post(r: r <= capacity()); // named postcondition: result is bounded

    [[nodiscard]] auto empty() const noexcept -> bool;
    [[nodiscard]] auto full() const noexcept -> bool;
    [[nodiscard]] auto capacity() const noexcept -> std::size_t;
};
```

Contracts live in the declaration, not the body. They are visible to callers, tools, and reviewers without opening the implementation. The standard defines semantic levels — `default`, `audit`, and `axiom` — that control evaluation cost, and a build-mode mechanism that determines whether violations terminate, continue, or are assumed never to occur.

**How contracts change API design and review heuristics.** Contracts shift precondition documentation from prose to checkable specification. The review heuristic at a public API boundary changes from "is the precondition documented in a comment?" to "is the precondition expressed as a `pre` clause?" This is a stronger question because the answer is mechanically verifiable.

Contracts also change testing strategy. Instead of writing separate unit tests that exercise every precondition violation path, teams can rely on contract-checking builds to catch violations during integration testing. The test suite still needs to exercise boundary conditions, but the assertion that a violated precondition is caught no longer requires a hand-written test — the contract facility does it.

For teams that currently use GSL's `Expects()` and `Ensures()` or custom precondition macros, the migration is straightforward in structure:

```cpp
// C++23: GSL-style precondition — implementation detail, not visible in the declaration.
auto element_at(std::size_t index) -> T& {
    Expects(index < size());  // hidden inside the body
    return data_[index];
}

// [C++26] Contract-style precondition — visible in the declaration.
auto element_at(std::size_t index) -> T&
    pre(index < size())        // part of the interface
{
    return data_[index];
}
```

**Interaction with existing assert/expects patterns.** Contracts do not replace all uses of `assert`. Implementation-internal invariants — sanity checks within an algorithm, loop invariant assertions, debug-only consistency checks — remain as `contract_assert` (the C++26 replacement for `assert` within function bodies) or as traditional assertions. The distinction matters: a `pre` clause is an interface contract between caller and callee; a `contract_assert` is an implementation-internal check.

Teams using custom assertion macros with logging, stack traces, or crash reporting will need an adaptation layer. The contract violation handler in C++26 is customizable, so existing crash-reporting infrastructure can be wired in.

**What to do now in C++23 to prepare.**

- Adopt a precondition/postcondition convention that separates interface contracts from implementation assertions. `Expects()` at function entry for preconditions, `Ensures()` before return for postconditions, and `assert()` for internal invariants. This maps directly to `pre`, `post`, and `contract_assert`.
- Write preconditions on declarations (in comments or macro annotations), not only in bodies. This habit makes the migration to `pre` clauses mechanical.
- Audit public API boundaries for undocumented preconditions. If a function silently assumes non-null, non-empty, or in-range, document it now. C++26 contracts will give those assumptions teeth, but only if you know what they are.

### 19.7.2 Pattern Matching (P2688): Structured Value Inspection

**The production problem pattern matching solves.** C++ codebases that use `std::variant`, tagged unions, or type-erased value hierarchies develop a recurring pattern: cascaded `if`/`else` chains or `std::visit` with overloaded lambdas to inspect and dispatch on value shape. Both approaches are verbose, error-prone, and hostile to reviewers.

The `std::visit` approach with `overloaded` is the current best practice, but it has real costs:

```cpp
// C++23: variant visitation via overloaded lambdas.
// Works, but the boilerplate obscures the logic.
auto describe(const Shape& shape) -> std::string {
    return std::visit(overloaded{
        [](const Circle& c) {
            return std::format("circle r={}", c.radius);
        },
        [](const Rectangle& r) {
            return std::format("rect {}x{}", r.width, r.height);
        },
        [](const Triangle& t) {
            return std::format("triangle base={} height={}", t.base, t.height);
        }
    }, shape);
}
```

This is not terrible. But it requires the `overloaded` helper (a boilerplate struct that every project defines slightly differently), it buries the matched type inside lambda parameter lists, and adding a new alternative requires touching every visit site. In a codebase with 50 visit sites for a 10-alternative variant, adding an 11th alternative means editing 50 files — and the compiler only catches missing cases if every site uses `std::visit` (not if some use `std::get_if` chains).

**What C++26 pattern matching provides.** Pattern matching (P2688) introduces an `inspect` expression that combines value decomposition, guard conditions, and exhaustiveness checking:

```cpp
// [C++26] Pattern matching: structured, exhaustive value inspection.
auto describe(const Shape& shape) -> std::string {
    return inspect(shape) {
        <Circle>    [.radius: r]            => std::format("circle r={}", r);
        <Rectangle> [.width: w, .height: h] => std::format("rect {}x{}", w, h);
        <Triangle>  [.base: b, .height: h]  => std::format("triangle base={} height={}", b, h);
    };
}
```

The `inspect` expression is exhaustive: if a new alternative is added to the variant and a match arm is missing, the program is ill-formed. Binding is structural — members are extracted by name, not positional index. Guard clauses (`if` after a pattern) allow conditional matching without nesting.

**How pattern matching changes value-semantic type design.** Pattern matching rewards types with public, named members and discourages opaque accessor-only interfaces for value types. A `struct Point { double x; double y; }` is directly matchable. A `class Point` with only `getX()` and `getY()` requires adaptation. This aligns with the value semantics guidance from Chapter 3: if a type is a value, its representation *is* its identity, and hiding it behind accessors adds cost without invariant protection.

For variant handling, pattern matching makes exhaustiveness a compiler-checked property rather than a discipline-dependent one. The review heuristic changes from "did the developer handle all cases?" to "does the code compile?" — a dramatically lower review burden.

**Impact on code review clarity.** Pattern matching replaces control-flow spaghetti with declarative case enumeration. A reviewer can scan a `inspect` block vertically and verify coverage. With `if`/`else` chains or nested `std::get_if` calls, the reviewer must trace control flow to determine whether all cases are handled and whether fall-through is intentional.

**What to do now in C++23 to prepare.**

- Use `std::visit` with the `overloaded` pattern consistently rather than `std::get_if` chains or `index()`-based switching. `std::visit` already provides exhaustiveness checking, which makes the migration to `inspect` mechanical.
- Prefer `std::variant` over hand-rolled tagged unions or type-code enums. Pattern matching works natively with `variant`; hand-rolled dispatch mechanisms will need adapters.
- For value types that will be inspected, prefer aggregate-style structs with named public members over accessor-heavy classes. This is already good practice for value semantics; pattern matching adds another reason.
- Centralize variant type aliases. If `using Shape = std::variant<Circle, Rectangle, Triangle>` is defined in one header, adding a new alternative and finding all `inspect`/`visit` sites is straightforward. If the variant is duplicated or aliased inconsistently, the migration is painful.

### 19.7.3 Reflection (P2996/P3394): Compile-Time Introspection

**The production problem reflection solves.** Large C++ codebases accumulate boilerplate that exists solely because the language cannot inspect its own types at compile time. Enum-to-string conversion, serialization, structured logging, ORM mapping, command-line argument parsing, and type-erased interface generation all follow the same pattern: a developer manually writes code that mirrors the structure of a type, and that mirror drifts out of sync when the type changes.

The standard workarounds are all unsatisfying:

- **Macros** (`X_MACRO`, `NLOHMANN_DEFINE_TYPE_INTRUSIVE`, `BOOST_DESCRIBE_STRUCT`) are fragile, opaque to tooling, and produce incomprehensible error messages.
- **Code generation** (protobuf, flatbuffers, custom Python scripts) adds build complexity, creates generated files that confuse reviewers, and forces types to live in a schema language rather than C++.
- **Manual registration** (`std::map<std::string, std::function<...>>` populated by hand) is error-prone and silently breaks when members are added or renamed.

In a codebase with 200 enum types and 500 serializable structs, these workarounds represent thousands of lines of maintenance-heavy boilerplate.

**What C++26 reflection provides.** Compile-time reflection (P2996, with the metafunction API refined by P3394) allows code to inspect the members, bases, enumerators, and attributes of types at compile time using standard library facilities:

```cpp
// [C++26] Reflection: compile-time enum-to-string without macros.
template <typename E>
    requires std::is_enum_v<E>
constexpr auto enum_to_string(E value) -> std::string_view {
    // reflect on the enum type, iterate its enumerators at compile time
    template for (constexpr auto e : std::meta::enumerators_of(^^E)) {
        if (value == [:e:]) {
            return std::meta::identifier_of(e);
        }
    }
    return "<unknown>";
}

enum class Color { Red, Green, Blue };
static_assert(enum_to_string(Color::Green) == "Green");  // no macros, no tables
```

```cpp
// [C++26] Reflection: automatic serialization for aggregate types.
template <typename T>
auto to_json(const T& obj) -> json::object {
    json::object result;
    template for (constexpr auto member : std::meta::nonstatic_data_members_of(^^T)) {
        result[std::meta::identifier_of(member)] =
            to_json_value(obj.[:member:]);  // splice: access the member by reflection handle
    }
    return result;
}

struct Employee {
    std::string name;
    int         department_id;
    double      salary;
};

// to_json(employee) produces {"name": "...", "department_id": 42, "salary": 85000.0}
// No macros. No registration. Adding a field to Employee automatically updates serialization.
```

The `^^` operator yields a reflection value (a `std::meta::info`) representing a type, namespace, or expression. The `[: :]` splice operator converts a reflection value back into a language construct. `template for` (expansion statements) iterates over compile-time sequences of reflections. Standard library metafunctions like `std::meta::nonstatic_data_members_of`, `std::meta::enumerators_of`, and `std::meta::identifier_of` provide structured access to type properties.

**Reducing boilerplate that currently requires macros or code generation.** The impact on large codebases is substantial. Consider what reflection replaces:

| Boilerplate pattern | C++23 workaround | C++26 reflection replacement |
|---|---|---|
| Enum-to-string | `X_MACRO` or manual `switch` | Generic `enum_to_string<E>()` template |
| JSON/XML serialization | Code generator or `NLOHMANN_DEFINE_TYPE_INTRUSIVE` macro | Generic `to_json<T>()` / `from_json<T>()` template |
| Structured logging | Manual field enumeration per type | Generic `log_fields<T>()` that discovers member names and values |
| Type-erased interfaces | Virtual base class + manual forwarding | Reflection-generated forwarding from concept to type-erased wrapper |
| ORM mapping | Schema DSL + code generator | Reflect on struct members, generate SQL column mappings |
| CLI argument parsing | Manual option registration per field | Reflect on a config struct, generate `--flag` options from member names |

Each of these currently consumes dozens to hundreds of lines of boilerplate per type. With reflection, they become generic templates written once and instantiated for any conforming type.

**Impact on maintainability and review.** Reflection eliminates a category of bug: the "added a field but forgot to update the serializer/logger/test-helper" problem. When serialization is generated from the type definition, adding a field to the struct is the only change required. No secondary file to update, no macro to extend, no code generator to rerun.

For code review, this is transformative. Instead of reviewing 50 lines of hand-written serialization for each new type, the reviewer verifies that the type conforms to the generic serialization template's requirements (e.g., all members are themselves serializable). The review question shifts from "did you mirror the struct correctly?" to "is the struct well-designed?" — a more valuable use of reviewer attention.

The risk is that reflection-heavy generic code can be difficult to debug. A `template for` loop over reflected members produces code whose behavior depends on type structure in ways that are not visible in the source. Teams should invest in clear error messages (using `static_assert` with reflection-generated diagnostics) and in testing reflection-based utilities thoroughly against representative types.

**What to do now in C++23 to prepare.**

- Identify the boilerplate hotspots in the codebase: enum-to-string functions, serialization code, logging helpers, and type registration. These are the first candidates for reflection-based replacement.
- Ensure that types destined for reflection are aggregates or have public members where possible. Reflection can inspect private members, but generic utilities are simpler and more maintainable when they operate on public structure.
- Isolate macro-based registration behind a single header per concern (e.g., `serialization_macros.h`, `enum_strings.h`). When reflection arrives, the migration is one header replacement rather than a codebase-wide macro hunt.
- Avoid introducing new code generators for problems that reflection will solve. If a new type needs JSON serialization today, use an existing macro-based solution rather than building a new custom generator that will be obsolete in two years.
- Structure enum types cleanly — avoid sentinel values, bitfield enums mixed with sequential enums in the same type, or enumerator values that do not match their names. Reflection-based enum utilities work best with straightforward enumerations.

---

## 19.8 Review Checklist

Use during code review. Not every item applies to every change; focus on the items that match the boundary type of the code under review.

### 19.8.1 Ownership and Lifetime
- [ ] Can you state the lifetime contract of every pointer/reference that crosses a function or module boundary?
- [ ] Is ownership transfer expressed in the type system (`unique_ptr`, value types) rather than by convention?
- [ ] Is `shared_ptr` justified by genuinely shared, non-hierarchical lifetime rather than deferred design?
- [ ] Are borrowed references (`T&`, `T*`, `span<T>`) annotated or documented with their validity scope?

### 19.8.2 Error Handling
- [ ] Is the failure domain consistent across the module boundary (all exceptions, all `expected`, all error codes — not a mix)?
- [ ] Can the caller distinguish transient from permanent failure?
- [ ] Are implementation-specific errors translated at the failure boundary rather than leaked to callers?
- [ ] Is `[[nodiscard]]` applied to functions returning error-bearing types?

### 19.8.3 Concurrency
- [ ] Is thread safety documented per type, not per function?
- [ ] Does the API force correct synchronization (guard objects, copies, snapshots) rather than relying on caller discipline?
- [ ] Are mutable shared data structures accessed through a synchronization mechanism that the type system enforces?
- [ ] Is the shutdown/cancellation path tested, including the race between cleanup and in-flight operations?

### 19.8.4 Dependencies
- [ ] Is the justification for a new dependency documented (performance data, functionality gap, ownership)?
- [ ] Is the transitive build cost measured or estimated?
- [ ] Does the dependency introduce global policy (allocators, logging handlers, signal handlers)?
- [ ] Is there an identified owner for the dependency going forward?

### 19.8.5 Abstraction and Design
- [ ] Does a new abstraction protect a boundary that has actually shifted or an invariant that has actually been violated?
- [ ] Is speculative generality avoided? (A template that is instantiated for one type is not generic code — it is obfuscated concrete code.)
- [ ] Are wrapper types adding value (invariant enforcement, boundary protection) or just indirection?
- [ ] Is the change consistent with the established convention at this boundary type, or does it introduce a new local dialect?

### 19.8.6 Build and Tooling
- [ ] Does the change compile cleanly under the project's warning level (`-Wall -Wextra -Werror` or equivalent)?
- [ ] Do new public headers pass the project's include-what-you-use or include-graph checks?
- [ ] Are build visibility rules updated to reflect new module dependencies?
- [ ] Does CI run sanitizers (ASan, TSan, UBSan) on the affected test targets?

### 19.8.7 Knowledge and Maintainability
- [ ] Do boundary types (public APIs, module interfaces) have design comments explaining the ownership, failure, and concurrency contracts?
- [ ] Does the commit message explain *why* the change was made, not just *what* changed?
- [ ] For precedent-setting decisions, is there an ADR or equivalent decision record in the repository?
- [ ] Is the CODEOWNERS file updated if the change introduces a new boundary or module?
