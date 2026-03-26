# Interface Design and Dependency Direction

This chapter assumes you already read function signature design, ownership, invariants, and failure boundaries. The question here is not how to write a function, but how to shape a boundary that survives team growth and system pressure.

## The Production Problem

Most interface damage does not come from obviously bad code. It comes from locally reasonable choices that harden into system-wide coupling. A storage layer returns database row types because that is what it already has. A service boundary takes a giant configuration object because future options might matter. A library accepts callbacks as `std::function` everywhere because it seems flexible. Six months later, tests require half the dependency graph, call sites leak transport concerns, and changing an implementation detail becomes a breaking change.

This chapter is about source-level interface design: what a boundary exposes, which direction dependencies should point, and how to keep policy, ownership, and representation from leaking across layers. It is not a chapter about runtime dispatch mechanics; that belongs in the next chapter. It is not a chapter about binary compatibility or distribution; that belongs in the chapter after that. The focus here is narrower and more important: deciding what one part of the program is allowed to know about another.

The core rule is simple: dependencies should point toward stable policy and domain meaning, not toward volatile implementation detail. In practice, that means interfaces should be built from concepts the caller already understands, not from the callee's storage, transport, framework, or logging choices.

## Why This Gets Expensive

Bad dependency direction multiplies cost in ways that code review often misses.

If domain logic depends directly on SQL row types, protobuf-generated classes, HTTP request wrappers, or filesystem traversal state, then every test, benchmark, and refactor must drag those details with it. The dependency graph becomes wider than the design requires. Build times increase because transitive includes and templates spread implementation detail everywhere. Review quality drops because boundary violations become normal. Most importantly, design choices that should have been local stop being local.

The cost is not only compile time. It is also conceptual stability. A good interface can survive a database change, a queue replacement, or a logging rewrite. A bad one requires the rest of the codebase to relearn internal facts that were never their concern.

## Start From the Boundary Question

Before writing an interface, force the production question into one sentence.

In a native service, the question is usually not "how do I expose the repository?" It is "how does the order workflow ask for customer credit state?" In a shared library, it is not "how do I surface the parser internals?" It is "what contract does the caller need in order to validate and transform incoming records?"

That shift matters because it changes the shape of the types. Interfaces designed around implementation nouns tend to leak mechanisms. Interfaces designed around work and invariants tend to stay narrow.

An interface should answer four questions clearly:

1. What capability does the caller need?
2. Which side owns data and lifetime?
3. Where does failure get translated into the chapter-3 error model?
4. Which policies are fixed here, and which remain caller choices?

If those answers are vague, the interface is probably mixing layers.

## Dependency Direction Means Policy Direction

Dependency inversion is often explained mechanically: depend on abstractions, not concretions. That is correct and not sufficient. The useful test is whether the dependency arrow follows stable policy.

In a service, business rules change slower than transport glue. Fraud policy should not depend on HTTP handlers. Order validation should not depend on SQL record wrappers. Domain logic can define the port it needs, and the database or network adapter can implement that port.

That does not mean every boundary needs an abstract base class. Many do not. Sometimes the right boundary is a free function taking domain data. Sometimes it is a concept-constrained template in an internal library. Sometimes it is a value-type request and result object with no virtual dispatch anywhere. The design decision is not "where do I place an interface type?" It is "which side gets to name the contract?"

The side with the more stable vocabulary should usually name it.

## Anti-pattern: Interface Defined by the Dependency

When implementation detail names the contract, the dependency arrow is already wrong.

```cpp
// Anti-pattern: domain code now depends on storage representation.
struct AccountRow {
	std::string id;
	std::int64_t cents_available;
	bool is_frozen;
	std::string fraud_flag;
};

class AccountsTable {
public:
	virtual std::expected<AccountRow, DbError>
	fetch_by_id(std::string_view id) = 0;
	virtual ~AccountsTable() = default;
};

std::expected<PaymentDecision, PaymentError>
authorize_payment(AccountsTable& table, const PaymentRequest& request);
```

This looks testable because it uses an abstract base class. It is still the wrong seam. The payment workflow should not know that available credit is stored as cents next to a fraud flag string loaded from a table row. The abstraction preserved the dependency but did not improve its direction.

A better port is named by the workflow and returns the minimum stable facts the workflow needs.

```cpp
struct CreditState {
	Money available;
	bool frozen;
	RiskLevel risk;
};

class CreditPolicyPort {
public:
	virtual std::expected<CreditState, PaymentError>
	load_credit_state(AccountId account) = 0;
	virtual ~CreditPolicyPort() = default;
};

std::expected<PaymentDecision, PaymentError>
authorize_payment(CreditPolicyPort& credit, const PaymentRequest& request);
```

Now the workflow depends on domain meaning rather than storage shape. The adapter that talks to SQL does the translation. That translation is work, but it is the right work: contain volatility near the volatile thing.

## Anti-pattern: Fat Interfaces That Attract Everything Nearby

A bloated interface does not just violate aesthetics. It creates coupling gravity: every new feature gets bolted onto the existing surface because adding a method is easier than rethinking the boundary.

```cpp
// Anti-pattern: a "god interface" that mixes query, mutation, lifecycle,
// metrics, and configuration concerns in one surface.
class UserService {
public:
	virtual std::expected<UserProfile, ServiceError>
	get_profile(UserId id) = 0;

	virtual void update_profile(UserId id, const ProfilePatch& patch) = 0;

	virtual void ban_user(UserId id, std::string_view reason) = 0;

	virtual std::vector<AuditEntry>
	get_audit_log(UserId id, TimeRange range) = 0;

	virtual void flush_cache() = 0;

	virtual MetricsSnapshot get_metrics() const = 0;

	virtual void set_rate_limit(RateLimitConfig config) = 0;

	virtual ~UserService() = default;
};
```

This interface has at least four unrelated axes of change: user data access, moderation policy, operational observability, and runtime configuration. A caller who only needs to read a profile now transitively depends on audit, caching, metrics, and rate-limiting types. Test doubles must implement seven methods to fake one. Adding a new moderation action forces recompilation of read-only consumers. The interface is not flexible. It is a dependency sink that makes every change expensive and every test fragile.

The fix is to split along responsibility boundaries:

```cpp
class UserProfileQuery {
public:
	virtual std::expected<UserProfile, ServiceError>
	get_profile(UserId id) = 0;
	virtual ~UserProfileQuery() = default;
};

class ModerationActions {
public:
	virtual void ban_user(UserId id, std::string_view reason) = 0;
	virtual std::vector<AuditEntry>
	get_audit_log(UserId id, TimeRange range) = 0;
	virtual ~ModerationActions() = default;
};
```

Now read-only consumers depend only on `UserProfileQuery`, moderation tools depend on `ModerationActions`, and operational concerns live in yet another interface. Each can evolve independently. Test doubles are trivial.

## Anti-pattern: Leaking Implementation Details Through the Interface

Even a small interface can damage a system if it exposes the wrong types.

```cpp
// Anti-pattern: interface leaks the JSON library into every consumer.
#include <nlohmann/json.hpp>

class RetryConfigProvider {
public:
	virtual nlohmann::json load_retry_config() = 0;
	virtual ~RetryConfigProvider() = default;
};
```

Every translation unit that includes this header now depends on the JSON library, whether or not it cares about JSON. Changing to TOML, YAML, or a binary config format becomes a breaking change across the entire codebase. The JSON library's compile time, macro definitions, and transitive includes spread into unrelated components. Worse, callers must navigate a JSON tree to extract retry parameters—initial backoff, max backoff, max attempts—scattering implicit schema knowledge throughout the codebase.

The fix is to return domain-meaningful types:

```cpp
struct RetryConfig {
	std::chrono::milliseconds initial_backoff;
	std::chrono::milliseconds max_backoff;
	std::uint32_t max_attempts;
};

class RetryConfigProvider {
public:
	virtual std::expected<RetryConfig, ConfigError>
	load_retry_config() = 0;
	virtual ~RetryConfigProvider() = default;
};
```

Now the JSON dependency stays inside the adapter implementation. Consumers work with typed, validated values. The interface communicates domain meaning, not serialization format.

## Anti-pattern: Wrong Abstraction Level

Interfaces pitched at the wrong level of abstraction force callers to do work that should be encapsulated, or prevent them from doing work they need.

```cpp
// Anti-pattern: too low-level. Caller must assemble SQL semantics
// even though this is supposed to abstract away storage.
class DataStore {
public:
	virtual std::expected<RowSet, DbError>
	execute_query(std::string_view sql) = 0;

	virtual std::expected<std::size_t, DbError>
	execute_update(std::string_view sql) = 0;

	virtual ~DataStore() = default;
};
```

This interface claims to abstract storage, but it exposes SQL as a string protocol. Callers must know the schema, construct correct SQL, and parse `RowSet` results. The abstraction prevents neither SQL injection nor schema coupling. It is a pass-through that adds indirection without reducing dependency.

Conversely, an interface can be too high-level and prevent legitimate use:

```cpp
// Anti-pattern: too high-level. No way to paginate, filter,
// or control what gets loaded.
class OrderRepository {
public:
	virtual std::vector<Order> get_all_orders() = 0;
	virtual ~OrderRepository() = default;
};
```

The right abstraction level matches the operations the caller actually performs, using domain vocabulary and offering enough control to be efficient.

## Keep Interfaces Small by Separating Commands From Queries

Bloated interfaces usually come from mixing unrelated reasons to change. A boundary that both retrieves state, mutates state, emits audit events, opens transactions, and exposes metrics snapshots is not flexible. It is a dependency sink.

Splitting commands from queries is often enough to recover clarity. Query paths usually want value-oriented request and result types, predictable cost, and no hidden mutation. Command paths usually want explicit ownership transfer, clear side effects, and strong failure semantics. Treating them as one interface encourages accidental coupling because callers start depending on whatever was convenient to put there last quarter.

Smaller interfaces also help review quality. A reviewer can ask whether each function belongs to the same boundary at all. Once an interface becomes a bag of nearby operations, that question stops being easy.

The `TaskRepository` in `examples/web-api/src/modules/repository.cppm` illustrates a narrow interface that stays focused. Its public surface is only CRUD: `create`, `find_by_id`, `find_all`, `find_completed`, `update`, `remove`, and `size`. There is no logging method, no configuration knob, no metric snapshot, no cache flush. Locking strategy (`std::shared_mutex`), storage representation (`std::vector<Task>`), and ID generation (`std::atomic<TaskId>`) are private. Callers depend on domain operations, not on how the repository happens to implement them.

## Data Shapes: Accept Stable Views, Return Owned Meaning

Chapter 4 covers local signature choices. At interface boundaries, the same rules become architectural.

Inputs should usually accept non-owning views when the callee does not need to retain data: `std::string_view`, `std::span<const std::byte>`, spans of domain objects, or lightweight request structs referencing caller-owned data. That keeps call sites cheap and honest.

Outputs should usually return owned values or domain objects with clear lifetimes. Returning a view into adapter-owned storage, a borrowed pointer into a cache line, or an iterator into internal state turns a boundary into a lifetime puzzle. That is rarely worth it.

The asymmetry is deliberate. Borrow from callers when cost matters and retention does not. Return ownership when crossing back out, because the callee controls its internals and should not force callers to care how long those internals stay alive.

There are exceptions. Hot-path parsers, zero-copy data pipelines, and memory-mapped processing stages may intentionally return views. When they do, the lifetime boundary must be part of the interface contract, not tribal knowledge. A type like `ParsedFrameView` tied to a specific buffer owner is much safer than leaking naked `std::string_view` or raw pointers and hoping reviewers notice the coupling.

## Do Not Smuggle Policy Through Optional Parameters

One of the fastest ways to make an interface unclear is to use configuration objects or default parameters to push policy decisions into places where callers cannot reason about them.

If a function has flags such as `skip_cache`, `best_effort`, `emit_audit`, `allow_stale`, and `retry_count`, the function is probably doing too many jobs. The problem is not aesthetics. The problem is that callers can now form combinations whose semantics are unclear, untested, or operationally dangerous.

Prefer one of three alternatives:

1. Split the capability into separate operations with clearer names.
2. Promote policy to an explicit type whose invalid states are impossible or obvious.
3. Move policy selection up a layer so the lower-level interface stays deterministic.

An interface is easier to evolve when policy is named explicitly instead of hidden in parameter soup.

## Testability Is a Consequence, Not the Goal

Teams often justify an interface by saying it improves testing. That is backward. The primary question is whether the boundary reflects the real design. If it does, testing usually gets easier. If it does not, test doubles only preserve the mistake.

For example, introducing a repository interface solely so unit tests can fake database access is weak reasoning if the domain still depends on table-shaped data and transport-shaped errors. The tests may become easier to write while the design stays wrong.

Good boundaries produce better tests because they isolate policy from mechanism. You can test business logic against simple fakes because the business logic asks for domain facts, not framework objects. You can integration-test the adapter separately because translation is contained in one place. This is a stronger outcome than "we can mock it now."

## Use Concepts and Templates Internally, Not as Public Escape Hatches

Modern C++ makes it easy to encode interfaces as constraints rather than virtual classes. This is often the right choice inside a component or within a tightly controlled codebase. A constrained template can keep code allocation-free, inlineable, and more expressive than a deep hierarchy.

But a public interface that tries to be everything through templates often stops being an interface at all. It becomes a policy surface, a compile-time integration mechanism, and a documentation burden at the same time. Error messages get worse, build dependencies widen, and call-site expectations become murky.

Use concept-constrained interfaces when all of the following are true:

1. The caller and callee are built together.
2. The customization point is central to performance or representation.
3. You can state the semantic contract clearly, not just the syntax contract.

If those conditions do not hold, a smaller value-oriented API or a runtime boundary is often better.

## Failure Translation Belongs at the Boundary

An interface is also where failure semantics become explicit. If the adapter speaks SQL exceptions, gRPC status codes, or platform error values, that does not mean the rest of the system should.

Translate failures as close to the volatile dependency as practical. The domain-facing interface should expose failure categories the caller can actually act on. This keeps business logic from depending on transport or vendor error taxonomies, and it makes logging and retry behavior much easier to reason about.

Do not over-normalize into useless generic errors. "Operation failed" is not a boundary model. The point is to expose stable decision-relevant categories while containing unstable backend detail.

The example project in `examples/web-api/` shows this pattern concretely. The `result_to_response()` function in `handlers.cppm` sits at the boundary between domain logic and HTTP transport:

```cpp
// examples/web-api/src/modules/handlers.cppm
template <json::JsonSerializable T>
[[nodiscard]] http::Response
result_to_response(const Result<T>& result, int success_status = 200) {
    if (result) {
        return {.status = success_status, .body = result->to_json()};
    }
    return http::Response::error(result.error().http_status(),
                                 result.error().to_json());
}
```

Domain code works exclusively with `Result<T>` and `ErrorCode` from the error module. The HTTP status code mapping is defined once in `error.cppm` via `to_http_status()`, and the translation into an HTTP response happens at the handler layer. No domain type knows what an HTTP response looks like. No handler leaks domain error internals into the transport. The boundary translates, and each side speaks its own vocabulary.

## When Not to Abstract

Some code should depend directly on a concrete type. Over-abstraction creates indirection, hides cost, and makes simple paths harder to read.

If a type is local to one subsystem, has one obvious implementation, and changing it would not produce a different deployment or test strategy, a direct dependency is often correct. Internal helper types, parsers, allocators scoped to a component, and single-backend pipeline stages do not become better because they grew ports.

The test is not whether abstraction is theoretically possible. The test is whether a boundary isolates a real axis of change or policy. If not, keep the dependency concrete and local.

## Verification and Review Questions

Interface design should be reviewed with the same discipline as performance or concurrency.

Ask these questions:

1. Does the interface expose domain meaning or implementation detail?
2. Are ownership and lifetime obvious at the boundary?
3. Are failure types translated into something decision-relevant?
4. Could a caller use this API correctly without knowing storage, transport, or framework internals?
5. Does the dependency arrow point toward the more stable policy vocabulary?
6. Is any abstraction here justified by a real axis of change, or only by a desire to mock?

Verification is not only code review. Integration tests should exercise adapters at the actual boundary where translation happens. Build profiling is also useful: if a supposedly clean interface still drags large transitive dependencies everywhere, the design may be source-level coupling in disguise.

## Takeaways

Interface design is mostly about deciding what must not leak.

Keep dependency direction aligned with stable policy, not convenient implementation. Accept cheap borrowed inputs when retention is unnecessary, but return owned meaning when crossing a boundary. Split interfaces by responsibility instead of building bags of operations. Translate failures where volatile dependencies enter the system. Abstract only where there is a real design seam.

If a caller must understand your database schema, transport wrapper, framework handle, or internal storage lifetime in order to use your API correctly, the boundary is already doing too much. That is the signal to redesign before the coupling becomes normal.