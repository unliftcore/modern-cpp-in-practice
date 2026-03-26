# Values, Identity, and Invariants

Once ownership is clear, the next production question is semantic rather than mechanical: what kind of thing is this object supposed to be?

Modern C++ code gets harder than it needs to be when every type is treated as mutable state with methods attached. Some objects are values. They represent a self-contained piece of meaning, can often be copied or replaced wholesale, and should be easy to compare and test. Some objects carry identity. They represent a specific session, account, worker, or connection whose continuity matters across time even as its fields change. Mixing those roles inside one vague type creates bugs that look unrelated: broken equality, unstable cache keys, accidental aliasing, poor concurrency behavior, and APIs that cannot say whether mutation changes "the same thing" or creates a new one.

This chapter is about keeping those categories sharp and enforcing invariants so the types remain trustworthy under pressure. It is not a chapter about parameter passing or ownership transfer mechanics. Those belong elsewhere. The focus here is modeling: deciding when a type should behave like a value, when identity must remain explicit, and how invariants stop the object graph from becoming a bag of fields.

## Values and Entities Solve Different Problems

A value is defined by what it contains, not by where it came from. Two values that represent the same configuration, time window, or money amount should usually be interchangeable. You can copy them, compare them, and move them between threads without inventing a story about which one is the "real" instance.

An entity or identity-bearing object is different. A live client session is not interchangeable with another session that happens to have the same fields at one moment. A connection object may reconnect, accumulate statistics, and hold synchronization state while still remaining the same connection from the system's point of view. Identity exists so the program can talk about continuity over time.

This sounds obvious. The design damage appears when teams fail to decide which category a type belongs to.

If an `Order` type is mutable, shared, equality-comparable by every field, and also used as a cache key, the program now has several incompatible stories about what that object means. If a configuration snapshot is wrapped in a reference-counted mutable object even though callers only need an immutable set of values, the code has paid for aliasing and lifetime complexity without gaining semantic power.

The useful default is stronger than many codebases realize: if a type does not need continuity across time, design it as a value first.

## Value Types Reduce Accidental Coupling

Value semantics are powerful because they reduce invisible sharing. A caller gets its own copy or moved instance. Mutation is local. Equality can often be structural. Tests can build small examples without allocating object graphs or mocking infrastructure.

Configuration is a good example. Many systems model configuration as a globally shared mutable object because updates exist somewhere in the product. That choice infects code that only needs a stable snapshot.

Usually the better design is this:

- Parse raw configuration into a validated value object.
- Publish a new snapshot when configuration changes.
- Let consumers hold the snapshot they were given.

That design makes each reader's world explicit. Code processing one request can reason against one configuration value. There is no half-updated object graph, no lock required merely to read a timeout value, and no mystery about whether two callers are looking at the same mutable instance.

### What Goes Wrong Without Value Semantics

When configuration is modeled as a shared mutable object rather than a value snapshot, aliasing bugs appear:

```cpp
// Anti-pattern: shared mutable configuration.
struct AppConfig {
	std::string db_host;
	int db_port;
	std::chrono::seconds timeout;
};

// A single global mutable instance, shared by reference.
AppConfig g_config;

void handle_request(RequestContext& ctx) {
	auto conn = connect(g_config.db_host, g_config.db_port);
	// ... long operation ...
	// BUG: another thread calls reload_config(), mutating g_config
	// mid-request. conn was opened with the old host, but now
	// ctx uses the new timeout. The request operates against
	// an incoherent mix of old and new configuration.
	conn.set_timeout(g_config.timeout);
}
```

With value semantics, each request captures its own immutable snapshot. No lock is needed to read fields, and no mid-flight mutation can create incoherent state:

```cpp
void handle_request(RequestContext& ctx, const ServiceConfig& config) {
	// config is a value -- it cannot change during this call.
	auto conn = connect(config.db_host(), config.db_port());
	conn.set_timeout(config.timeout());
	// Entire request sees a single consistent configuration.
}
```

The deeper point is that values compose well. They can sit inside containers, cross threads, participate in deterministic tests, and form stable inputs to hashing or comparison. Identity-bearing objects can do those things too, but they require more rules and more caution. Use that complexity only when the model truly needs it.

## Invariants Are the Reason to Have Types at All

A type that permits invalid combinations of state is often just a struct-shaped bug carrier.

An invariant is a condition that should hold whenever an object is observable by the rest of the program. A time window may require `start <= end`. A money amount may require a currency and a bounded integer representation. A batching policy may require `max_items > 0` and `flush_interval > 0ms`. A connection state object may forbid "authenticated but not connected."

The point of an invariant is not to make the constructor fancier. It is to shrink the number of invalid states that later code must defend against.

Consider a scheduling subsystem.

```cpp
class RetryPolicy {
public:
	static auto create(std::chrono::milliseconds base_delay,
					   std::chrono::milliseconds max_delay,
					   std::uint32_t max_attempts)
		-> std::expected<RetryPolicy, ConfigError>;

	auto base_delay() const noexcept -> std::chrono::milliseconds {
		return base_delay_;
	}

	auto max_delay() const noexcept -> std::chrono::milliseconds {
		return max_delay_;
	}

	auto max_attempts() const noexcept -> std::uint32_t {
		return max_attempts_;
	}

private:
	RetryPolicy(std::chrono::milliseconds base_delay,
				std::chrono::milliseconds max_delay,
				std::uint32_t max_attempts) noexcept
		: base_delay_(base_delay),
		  max_delay_(max_delay),
		  max_attempts_(max_attempts) {}

	std::chrono::milliseconds base_delay_;
	std::chrono::milliseconds max_delay_;
	std::uint32_t max_attempts_;
};
```

The details of error transport belong more fully in the next chapter, but the modeling point is already clear: a `RetryPolicy` should not exist in a nonsense state. Once created, code using it should not have to ask whether the delays are inverted or the attempt count is zero unless those are valid meanings the domain actually wants.

If a type does not enforce its invariants, the burden moves outward into every caller and every code review.

### What Happens When Invariants Are Not Enforced

Compare the factory-validated `RetryPolicy` above with a plain aggregate that leaves validation to callers:

```cpp
// Anti-pattern: invariants left to the caller.
struct RetryPolicy {
	std::chrono::milliseconds base_delay;
	std::chrono::milliseconds max_delay;
	std::uint32_t max_attempts;
};

void schedule_retries(const RetryPolicy& policy) {
	// Caller forgot to validate. base_delay is negative, max_attempts is 0.
	// This loop does nothing, silently dropping work.
	for (std::uint32_t i = 0; i < policy.max_attempts; ++i) {
		auto delay = std::min(policy.base_delay * (1 << i), policy.max_delay);
		enqueue_after(delay); // never executes when max_attempts == 0
	}
}
```

Every function that receives `RetryPolicy` must now independently check for nonsense values, or assume some earlier layer already did. In practice, some callers check and some do not, producing inconsistent behavior depending on the call path. The factory approach shown earlier makes this class of bug structurally impossible: if you have a `RetryPolicy`, it is valid.

The companion web-api project applies the same pattern to its domain model. `Task::validate()` is a static factory that returns `Result<Task>`, rejecting empty or oversized titles at the boundary:

```cpp
// examples/web-api/src/modules/task.cppm
[[nodiscard]] static Result<Task> validate(Task t) {
    if (t.title.empty()) {
        return make_error(ErrorCode::bad_request, "title must not be empty");
    }
    if (t.title.size() > 256) {
        return make_error(ErrorCode::bad_request, "title exceeds 256 characters");
    }
    return t;
}
```

Every path that stores a `Task` goes through `validate()` first, including updates — the repository re-validates after mutation. The invariant is owned by the type, not by individual callers.

## Anti-pattern: Entity Semantics Smuggled Into a Value Type

One recurring failure is a type that looks like a value because it is copied and compared, but actually carries identity-bearing mutation.

```cpp
// Anti-pattern: one type tries to be both a value and a live entity.
struct Job {
	std::string id;
	std::string owner;
	std::vector<Task> tasks;
	std::mutex mutex; // RISK: identity-bearing synchronization hidden inside data model
	bool cancelled = false;
};
```

This object cannot honestly behave like a value because copying a mutex and a live cancellation flag has no sensible meaning. It also cannot honestly behave like a narrow entity model because the entire mutable representation is public. The type will infect the rest of the codebase with ambiguity.

The cleaner split is usually:

- a value type such as `JobSpec` or `JobSnapshot` for the stable domain data,
- and an identity-bearing runtime object such as `JobExecution` that owns synchronization, progress, and cancellation state.

That split clarifies which parts are serializable, comparable, cacheable, and safe to move across threads, and which parts model a live process in the system.

The companion web-api project demonstrates this separation cleanly. `Task` is a pure value type — copyable, comparable, serializable — while `TaskRepository` is the identity-bearing entity that owns a `shared_mutex`, an ID generator, and the mutable collection. The value carries domain data; the entity manages lifecycle and synchronization. Neither tries to do the other's job.

## Equality Should Match Meaning

One of the best tests for whether a type has a coherent semantic role is whether equality is obvious.

For many value types, equality should be structural. Two validated endpoint configurations with the same host, port, and TLS mode are the same value. Two money amounts with the same currency and minor units are the same value. Two time ranges with the same endpoints are the same value.

For identity-bearing objects, structural equality is often actively misleading. Two live sessions with the same user id and remote address are not the same session. Two connections pointed at the same shard are not interchangeable if each carries different lifecycle state and pending work.

If a team cannot answer what equality should mean for a type, the type is probably mixing value data with identity-bearing runtime concerns.

The companion web-api project keeps this straightforward. `Task` declares a defaulted three-way comparison, making equality purely structural:

```cpp
// examples/web-api/src/modules/task.cppm
[[nodiscard]] auto operator<=>(const Task&) const = default;
```

Because `Task` is a value, structural equality is the right answer. The identity-bearing `TaskRepository` has no equality operator at all — comparing two repositories would be meaningless.

This matters operationally. Equality influences cache keys, deduplication logic, diff generation, test assertions, and change detection. A semantically vague type tends to produce semantically vague equality, which then breaks several systems at once.

### Shallow Copies and Aliasing: A Concrete Trap

When a type looks like a value but shares internal state through pointers or references, copies become aliases rather than independent values:

```cpp
// Anti-pattern: shallow copy creates aliasing bugs.
struct Route {
	std::string name;
	std::shared_ptr<std::vector<Endpoint>> endpoints; // shared, not owned
};

void reconfigure(Route primary) {
	Route backup = primary; // looks like a copy, but endpoints are shared

	backup.name = "backup-" + primary.name;
	backup.endpoints->push_back(fallback_endpoint()); // BUG: mutates primary too

	// primary.endpoints and backup.endpoints point to the same vector.
	// The caller who passed primary now sees an endpoint they never added.
}
```

The fix is to give the type genuine value semantics. Either store the vector directly as a member (so copies are deep), or use a copy-on-write strategy, or make the type immutable so sharing is safe:

```cpp
struct Route {
	std::string name;
	std::vector<Endpoint> endpoints; // owned, copied on assignment

	auto with_endpoint(Endpoint ep) const -> Route {
		Route copy = *this;
		copy.endpoints.push_back(std::move(ep));
		return copy;
	}
};
```

Now `Route` behaves as a value. Copies are independent. Mutation through `with_endpoint` produces a new value without disturbing the original. No aliasing surprise is possible.

## Mutation Should Respect the Modeling Choice

Values and entities tolerate mutation differently.

For value types, the cleanest design is often immutability after validation or at least mutation through narrow operations that preserve invariants. Replacing a configuration snapshot or producing a new routing table is frequently easier to reason about than mutating one shared instance in place.

For entities, mutation is natural because the object models continuity over time. But that does not justify public writable fields or unconstrained setters. An entity still needs a controlled state machine. A `Connection` may transition from `connecting` to `ready` to `draining` to `closed`; it should not permit arbitrary combinations just because the fields are individually legal.

The real design question is not whether mutation is allowed. It is where mutation is allowed and what guarantees survive it.

If mutation can break invariants between two field assignments, the type likely needs a stronger operation boundary. If callers must lock, update three fields, and remember to recompute a derived flag, the invariant was never really owned by the type.

```cpp
// Anti-pattern: public fields allow invariant-breaking mutation.
struct TimeWindow {
	std::chrono::system_clock::time_point start;
	std::chrono::system_clock::time_point end;
};

void extend_deadline(TimeWindow& window, std::chrono::hours extra) {
	window.end += extra; // fine
}

void shift_start(TimeWindow& window, std::chrono::hours shift) {
	window.start += shift;
	// BUG: if shift is large enough, start > end.
	// Every consumer of TimeWindow must now defend against this.
}
```

An encapsulated type eliminates this class of bug by making the invariant un-breakable from outside:

```cpp
class TimeWindow {
public:
	static auto create(system_clock::time_point start,
					   system_clock::time_point end)
		-> std::optional<TimeWindow>
	{
		if (start > end) return std::nullopt;
		return TimeWindow{start, end};
	}

	auto start() const noexcept { return start_; }
	auto end() const noexcept { return end_; }

	auto with_extended_end(std::chrono::hours extra) const -> TimeWindow {
		return TimeWindow{start_, end_ + extra}; // always valid: end moves forward
	}

private:
	TimeWindow(system_clock::time_point s, system_clock::time_point e)
		: start_(s), end_(e) {}

	system_clock::time_point start_;
	system_clock::time_point end_;
};
```

Callers cannot produce an invalid `TimeWindow`. The invariant `start <= end` is enforced once, in the type, rather than diffusely across every mutation site.

## Small Domain Types Are Worth the Ceremony

Experienced programmers sometimes resist tiny wrapper types because they look like ceremony compared with plain integers or strings. In production C++, these types often pay for themselves quickly.

An `AccountId`, `ShardId`, `TenantName`, `BytesPerSecond`, or `Deadline` type can eliminate argument swaps, clarify logs, and make invalid combinations harder to express. Just as importantly, these types can carry invariants and conversions locally instead of distributing them across parsing, storage, and formatting code.

The warning is that a wrapper type is only useful if it actually sharpens meaning. A thin shell around `std::string` that preserves all invalid states and adds no semantic operations is mostly noise. The right question is whether the type enforces or communicates a real distinction the system cares about.

## Concurrency Gets Easier When Values Stay Values

Many concurrency problems are secretly modeling problems. Shared mutable state is hard largely because the program uses identity-bearing objects where immutable values would have been enough.

Threading a validated snapshot through a pipeline is easy to reason about. Sharing a mutable configuration service object with interior locking across the same pipeline is much harder. Passing a value-oriented request descriptor into work queues is easier than passing a live session object with hidden aliasing and synchronization.

This does not mean every concurrent system can eliminate entities. It means value semantics are one of the most effective ways to reduce the amount of state that must be shared and synchronized. When the code can replace mutation with snapshot publication or message passing of values, both correctness and reviewability improve.

## Verification and Review

Types that claim semantic roles should be reviewed against those roles directly.

Useful review questions:

1. Is this type primarily a value or primarily an identity-bearing object?
2. Do its equality, copying, and mutation rules match that choice?
3. Which invariants does the type enforce itself?
4. Would splitting stable domain data from live runtime state simplify the design?
5. Is shared mutable state present because the model truly requires identity, or because value semantics were never attempted?

Testing follows the same logic. Value types deserve property-style tests for invariant preservation, equality, and serialization stability where relevant. Identity-bearing types deserve lifecycle and state-machine tests that verify legal transitions and reject illegal ones.

## Takeaways

- Default to value semantics when continuity across time is not part of the domain meaning.
- Make identity explicit when the object represents a specific live thing rather than interchangeable data.
- Enforce invariants inside types so callers do not have to rediscover them defensively.
- Let equality, copying, and mutation rules follow the semantic role of the type.
- Split stable domain values from runtime control state when one object is trying to do both jobs.

When a type has a clear answer to "what kind of thing is this," the rest of the design gets easier: ownership is more obvious, APIs get narrower, tests become simpler, and concurrency stops fighting hidden aliasing. That is why semantic clarity belongs this early in the book.