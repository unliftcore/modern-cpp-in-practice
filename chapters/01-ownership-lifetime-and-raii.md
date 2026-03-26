# Ownership, Lifetime, and RAII

The first production question in modern C++ is not "should this be a class" or "can this be zero-copy." It is simpler and more dangerous: who owns this resource, how long does it stay valid, and what guarantees cleanup when the happy path stops being happy.

That question applies to memory, but memory is only part of the story. Real systems own sockets, file descriptors, mutexes, thread joins, temporary directories, telemetry registrations, process handles, mapped files, transaction scopes, and shutdown hooks. The language gives you enough rope to represent all of them badly. The reason ownership deserves the first chapter is not that it is foundational in an academic sense. It is that unclear ownership makes the rest of the design impossible to review with confidence.

In production, the expensive failures are rarely dramatic at the call site. A service starts a background flush, captures a raw pointer to request state, and occasionally crashes during deploy. A connection pool closes on the wrong thread because the last `shared_ptr` release happened in a callback nobody considered part of shutdown. An initialization path half-builds three resources and leaks the second one when the fourth throws. These are not syntax problems. They are ownership problems that became operational incidents.

RAII remains the main reason modern C++ can manage these situations cleanly. It is not an old idiom that survived by habit. It is the mechanism that lets resource lifetime compose with scope, exceptions, early returns, and partial construction. Used well, RAII makes cleanup boring. That is exactly what you want.

## Ownership Must Be Legible

Ownership is a contract, not an implementation detail. A reviewer should be able to point at a type or member and answer three questions quickly.

1. What does this object own?
2. What may it borrow temporarily?
3. What event ends the lifetime of the owned resource?

If the answers require reading several helper functions, the design is already too implicit.

This is why modern C++ favors types whose ownership behavior is obvious. `std::unique_ptr<T>` means exclusive ownership. `std::shared_ptr<T>` means shared ownership with reference-counted lifetime. A plain object member means the containing object owns that subobject directly. A `std::span<T>` or `std::string_view` means borrowing, not retention. These are not stylistic preferences. They are part of how the program communicates lifetime.

The opposite style is familiar and expensive: a raw pointer member that might own, might observe, and might occasionally be null because shutdown is in progress. That design is cheap to type and expensive to reason about.

## RAII Is About Resources, Not About `new`

Many programmers first encounter RAII as "use smart pointers instead of manual `delete`." That is directionally correct and far too small.

RAII means tying a resource to the lifetime of an object whose destructor releases it. The resource might be memory. It might just as easily be a file descriptor, a kernel event, a transaction lock, or a metrics registration that must be unregistered before shutdown completes.

### What Happens Without RAII

Before illustrating the RAII pattern, it is worth seeing the manual approach in full, because production codebases still contain code that looks exactly like this.

```cpp
// Manual resource management: C-style socket handling.
void serve_request(const Config& config) {
	socket_t sock = ::open_socket(config.port());
	if (sock == invalid_socket) {
		throw NetworkError{"bind failed"};
	}

	char* buffer = new char[config.buffer_size()]; // second resource

	auto* sub = event_bus::subscribe("health_check"); // third resource

	// -- any throw between here and the cleanup block leaks all three --

	process(sock, buffer, sub); // may throw

	event_bus::unsubscribe(sub);
	delete[] buffer;
	::close_socket(sock);
}
```

The problems compound quickly:

1. **Exception unsafety.** If `process` throws, or if `event_bus::subscribe` throws after the buffer is allocated, the preceding resources leak silently. Adding `try`/`catch` blocks for each acquisition creates deeply nested cleanup ladders that are painful to write and easy to get wrong.

2. **Double-free risk.** If a maintenance change adds an early return or a second call path, a developer may call `delete[] buffer` twice or call `::close_socket` on an already-closed handle. Neither failure is caught at compile time.

3. **Order-dependent teardown.** Every new resource added to the function forces the developer to update every exit path. With three resources there are already multiple combinations of partially-acquired state. With five or six, the cleanup logic dominates the function.

4. **Fragility under maintenance.** Code review cannot verify cleanup correctness locally. The reviewer must trace every path through the function to confirm that every resource is released exactly once. This does not scale.

The RAII alternative eliminates these problems by construction. Each resource is held by an owning object whose destructor performs the release. Stack unwinding does the rest.

Consider a service component that needs a socket and a temporary subscription to an internal event stream.

```cpp
class SocketHandle {
public:
	SocketHandle() = default;
	explicit SocketHandle(socket_t fd) noexcept : fd_(fd) {}

	SocketHandle(const SocketHandle&) = delete;
	auto operator=(const SocketHandle&) -> SocketHandle& = delete;

	SocketHandle(SocketHandle&& other) noexcept
		: fd_(std::exchange(other.fd_, invalid_socket)) {}

	auto operator=(SocketHandle&& other) noexcept -> SocketHandle& {
		if (this != &other) {
			reset();
			fd_ = std::exchange(other.fd_, invalid_socket);
		}
		return *this;
	}

	~SocketHandle() { reset(); }

	auto get() const noexcept -> socket_t { return fd_; }

	void reset() noexcept {
		if (fd_ != invalid_socket) {
			::close_socket(fd_);
			fd_ = invalid_socket;
		}
	}

private:
	socket_t fd_ = invalid_socket;
};
```

This type does not exist to show move semantics. It exists so ownership of the socket becomes single, explicit, and exception-safe. The destructor is the cleanup policy. Move operations define transfer. Copy is forbidden because duplicating ownership would be wrong.

The companion web-api project demonstrates this exact pattern in production form. Its `Socket` class in `http.cppm` wraps a POSIX file descriptor with the same structure -- deleted copy, move via `std::exchange`, and a destructor that calls `::close`:

```cpp
// From examples/web-api/src/modules/http.cppm
class Socket {
public:
    explicit Socket(int fd) noexcept : fd_{fd} {}
    Socket(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
    ~Socket() { close(); }

    void close() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
private:
    int fd_{-1};
};
```

This `Socket` is then used throughout the server -- for instance, `create_server_socket()` performs a multi-step setup (create, `setsockopt`, `bind`, `listen`) and returns a `Socket` by value. If any step fails, the already-acquired file descriptor is cleaned up automatically when the local `Socket` is destroyed.

The same pattern applies to many non-memory resources. A scoped registration token unregisters in its destructor. A transaction object rolls back unless explicitly committed. A joined-thread wrapper joins during destruction or refuses to be destroyed while still joinable. Once a codebase thinks this way, cleanup paths become local again instead of scattered through error handling.

## Anti-pattern: Cleanup by Convention

The alternative to RAII is usually not explicit manual cleanup done perfectly. It is cleanup by convention, which means cleanup gets skipped under stress.

```cpp
// Anti-pattern: ownership and cleanup are split across control flow.
void publish_snapshot(Publisher& publisher, std::string_view path) {
	auto* file = ::open_config(path.data());
	if (file == nullptr) {
		throw ConfigError{"open failed"};
	}

	auto payload = read_payload(file);
	if (!payload) {
		::close_config(file); // BUG: one exit path remembered cleanup
		throw ConfigError{"parse failed"};
	}

	publisher.send(*payload); // BUG: if this throws, file leaks
	::close_config(file);
}
```

This is not controversial because manual cleanup is ugly. It is wrong because cleanup policy is now interleaved with every exit path. Once the function acquires a second or third resource, the control flow becomes harder to audit than the work the function actually performs.

The RAII version eliminates every manual release and every conditional cleanup path:

```cpp
void publish_snapshot(Publisher& publisher, std::string_view path) {
	auto file = ConfigFile::open(path); // RAII: destructor calls ::close_config
	if (!file) {
		throw ConfigError{"open failed"};
	}

	auto payload = read_payload(*file);
	if (!payload) {
		throw ConfigError{"parse failed"};
		// file releases automatically -- no manual cleanup needed
	}

	publisher.send(*payload);
	// file releases automatically at scope exit, whether normal or exceptional
}
```

The function now has one concern: its actual logic. Cleanup is invisible because it is guaranteed. Adding a third, fourth, or tenth exit path changes nothing about resource safety. That composability is the real payoff of RAII -- not prettier code, but correct code under maintenance pressure.

RAII fixes cleanup-by-convention by moving the release policy into the owning object. Error paths then recover their main job: describing failure rather than describing teardown.

## Exclusive Ownership Should Be the Default

Most resources in well-designed systems have one obvious owner at any given time. A request object owns its parsed payload. A connection object owns its socket. A batch owns its buffers. That is why exclusive ownership is the right default mental model.

In practice this means preferring plain object members or `std::unique_ptr` when direct containment is not possible. `unique_ptr` is not a signal that the design is sophisticated. It is a signal that ownership transfers and destruction are explicit. It also composes well with containers, factories, and failure paths because moved-from state is defined and single ownership stays single.

Shared ownership should be treated as a deliberate exception. There are valid cases: asynchronous fan-out where several components must keep the same immutable state alive, graph-like structures with genuine shared lifetime, caches whose entries remain valid while multiple users still hold them. But `shared_ptr` is not a generic safety blanket. It changes destruction timing, adds atomic reference-count traffic in many implementations, and often hides the real question: why can no component name the owner?

If a review finds `shared_ptr` at a boundary, the follow-up question should be concrete: what lifetime relationship made exclusive ownership impossible here? If the answer is vague, the shared ownership is probably compensating for a design that never decided where the resource belongs.

A common symptom is shutdown non-determinism. When the last `shared_ptr` to a resource is released from an unpredictable callback or thread, the destructor runs at an unpredictable time and place:

```cpp
// Risky: destruction timing depends on which callback finishes last.
void start_fanout(std::shared_ptr<Connection> conn) {
	for (auto& shard : shards_) {
		shard.post([conn] {           // each lambda extends lifetime
			conn->send(shard_ping()); // last lambda to finish destroys conn
		});
	}
	// conn may already be destroyed here, or may live much longer --
	// depends on thread scheduling. Destructor side effects (logging,
	// metric flush, socket close) now happen at an uncontrolled point.
}
```

When destruction order matters -- and in production it almost always does -- prefer `unique_ptr` with explicit lifetime scoping, and pass non-owning raw pointers or references to work that is guaranteed to complete within the owner's lifetime.

## Borrowing Needs Tighter Discipline Than Owning

A system with clear ownership still needs non-owning access. Algorithms inspect caller-owned buffers. Validation reads request metadata. Iterators and views traverse storage without copying it. Borrowing is normal. The mistake is letting borrowed state outlive the owner or making the borrow invisible.

Modern C++ gives you useful borrowing vocabulary: references, pointers used explicitly as observers, `std::span`, and `std::string_view`. These types help, but they do not enforce a good design by themselves. A view member inside a long-lived object is still a lifetime risk if the owner is elsewhere. A callback that captures a reference to stack state is still wrong if the callback runs later.

That risk becomes more severe under concurrency. A raw pointer or `string_view` captured into background work is not a small local shortcut. It is a cross-time borrow whose validity now depends on scheduling and shutdown order.

This is why a useful ownership rule is simple: owning types may cross time freely; borrowed types should cross time only when the owner is visibly stronger and longer-lived than the work using the borrow. If you cannot make that argument quickly, copy or transfer ownership instead.

## Move Semantics Define Transfer, Not Mere Optimization

Move semantics are often introduced as a performance topic. In practice they matter first as an ownership topic.

Moving an object states that the resource changes owners while the source remains valid but no longer responsible for the old resource. That is what makes factories, containers, and pipeline stages composable without inventing bespoke transfer APIs for every type.

For resource-owning types, good move behavior is part of the type's correctness story.

- The moved-to object becomes the owner.
- The moved-from object remains destructible and assignable.
- Double-release cannot occur.

This is one reason direct resource wrappers are worth the small amount of code they require. Once the ownership transfer rules live in the type, callers stop hand-transferring raw handles and hoping conventions line up.

Not every type should be movable, and not every move is cheap. A mutex is typically neither copyable nor movable because moving it would complicate invariants and platform semantics. A large aggregate with direct-buffer ownership may be movable but still not cheap in a hot path. The design question is not "can I default the move operations." It is "what ownership story should this type allow."

## Lifetime Bugs Often Hide in Shutdown and Partial Construction

Programmers tend to think about lifetime during the main work path. Production bugs often show up during startup failure and shutdown instead.

Partial construction is one example. If an object acquires three resources and the second acquisition throws, the first one must still release correctly. RAII handles this automatically when ownership is layered into members rather than performed manually in constructor bodies with cleanup flags.

The manual approach is fragile:

```cpp
// Anti-pattern: manual multi-resource construction with cleanup flags.
class Pipeline {
public:
	Pipeline(const Config& cfg) {
		db_ = ::open_db(cfg.db_path().c_str());
		if (!db_) throw InitError{"db open failed"};

		cache_ = ::create_cache(cfg.cache_size());
		if (!cache_) {
			::close_db(db_); // must remember to clean up db_
			throw InitError{"cache alloc failed"};
		}

		listener_ = ::bind_listener(cfg.port());
		if (listener_ == invalid_socket) {
			::destroy_cache(cache_); // must remember both prior resources
			::close_db(db_);
			throw InitError{"bind failed"};
		}
	}

	~Pipeline() {
		::close_listener(listener_);
		::destroy_cache(cache_);
		::close_db(db_);
	}

private:
	db_handle_t db_ = nullptr;
	cache_handle_t cache_ = nullptr;
	socket_t listener_ = invalid_socket;
};
```

Every new resource added to this constructor requires updating every prior failure branch. A maintenance change that reorders acquisitions silently breaks the cleanup logic.

The RAII version uses member wrappers and relies on the language rule that already-constructed members are destroyed when a constructor throws:

```cpp
class Pipeline {
public:
	Pipeline(const Config& cfg)
		: db_(DbHandle::open(cfg.db_path()))       // destroyed automatically if
		, cache_(Cache::create(cfg.cache_size()))   // a later member throws
		, listener_(Listener::bind(cfg.port())) {}

private:
	DbHandle db_;
	Cache cache_;
	Listener listener_;
};
```

No cleanup flags, no cascading `if` blocks, no order-sensitive manual teardown. The language does the work.

In the book's example project, `main.cpp` shows this principle applied to a complete service startup. Each layer is constructed as a scoped local in `main()`, and the stack's natural destruction order handles teardown:

```cpp
// From examples/web-api/src/main.cpp (simplified)
int main() {
    webapi::TaskRepository repo;                       // 1. domain object
    webapi::Router router;                             // 2. route table
    router.get("/tasks", webapi::handlers::list_tasks(repo));

    auto handler = webapi::middleware::chain(           // 3. middleware
        pipeline, router.to_handler());

    webapi::http::Server server{port, std::move(handler)}; // 4. server
    server.run_until(shutdown_requested);
    // destruction unwinds in reverse: server, handler, router, repo
}
```

No explicit teardown code appears anywhere. If any construction step throws, every previously constructed object is destroyed automatically in reverse order -- exactly the guarantee the RAII `Pipeline` pattern relies on.

Shutdown is the other major pressure point. Destructors run when the system is already under state transition. Background work may still hold references. Logging infrastructure may be partially torn down. A destructor that blocks indefinitely, calls back into unstable subsystems, or depends on thread affinity that the type never documented can turn a tidy ownership model into a deploy-time failure.

The lesson is not to fear destructors. The lesson is to keep destructor work narrow and explicit. Release the resource you own. Avoid surprising cross-subsystem behavior. If teardown requires a richer protocol than destruction alone can safely provide, expose an explicit stop or close operation and use the destructor as a final safety net rather than the only cleanup path.

## Verification and Review

Ownership design needs explicit review because many lifetime bugs are structurally visible before any tool runs.

Useful review questions:

1. Is there a single obvious owner for each resource?
2. Are borrowed references and views visibly shorter-lived than the owner?
3. Is `shared_ptr` solving a real shared-lifetime problem or avoiding an ownership decision?
4. Do move operations preserve single ownership and safe destruction?
5. Does shutdown rely on destructor side effects that are broader than resource release?

Dynamic tools still matter. AddressSanitizer catches many use-after-free bugs. Leak sanitizers and platform diagnostics catch forgotten release paths. ThreadSanitizer helps when lifetime errors are exposed by races during shutdown. But tools are strongest when the type system already makes ownership legible.

## Takeaways

- Treat ownership as a contract that must be visible in types and object structure.
- Use RAII for every meaningful resource, not only heap memory.
- Prefer exclusive ownership by default and justify shared ownership explicitly.
- Think of move semantics as ownership transfer rules before thinking of them as performance features.
- Review shutdown and partial-construction paths as seriously as the steady-state path.

If a resource can be leaked, double-released, used after destruction, or destroyed on the wrong thread, the problem usually started earlier than the crash. It started when ownership was left implicit.