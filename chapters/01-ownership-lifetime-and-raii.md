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

RAII fixes this by moving the release policy into the owning object. Error paths then recover their main job: describing failure rather than describing teardown.

## Exclusive Ownership Should Be the Default

Most resources in well-designed systems have one obvious owner at any given time. A request object owns its parsed payload. A connection object owns its socket. A batch owns its buffers. That is why exclusive ownership is the right default mental model.

In practice this means preferring plain object members or `std::unique_ptr` when direct containment is not possible. `unique_ptr` is not a signal that the design is sophisticated. It is a signal that ownership transfers and destruction are explicit. It also composes well with containers, factories, and failure paths because moved-from state is defined and single ownership stays single.

Shared ownership should be treated as a deliberate exception. There are valid cases: asynchronous fan-out where several components must keep the same immutable state alive, graph-like structures with genuine shared lifetime, caches whose entries remain valid while multiple users still hold them. But `shared_ptr` is not a generic safety blanket. It changes destruction timing, adds atomic reference-count traffic in many implementations, and often hides the real question: why can no component name the owner?

If a review finds `shared_ptr` at a boundary, the follow-up question should be concrete: what lifetime relationship made exclusive ownership impossible here? If the answer is vague, the shared ownership is probably compensating for a design that never decided where the resource belongs.

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