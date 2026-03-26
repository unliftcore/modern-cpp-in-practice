# Shared State, Synchronization, and Contention

This chapter assumes you already reason precisely about ownership and invariants in single-threaded code. The question now is what survives once multiple threads can observe and mutate the same state.

## The Production Problem

Most concurrency failures are not caused by missing primitives. They are caused by vague sharing policy.

A cache is "mostly reads." A connection pool has "just one mutex." A metrics registry uses atomics "for speed." A request coordinator stores a few counters and a queue behind a lock and appears correct in code review. Then production load arrives. Lock hold time couples unrelated work. A reader observes half-updated state because the invariant spans two fields. A background cleanup path waits under the same mutex as the hot path. Throughput collapses before anyone sees a crash, and once crashes do appear they are often data-race UB, deadlock, or starvation rather than a clean failure.

This chapter is about the shape of shared mutable state in production C++23 systems. The core question is not which mutex type to memorize. It is how much state should be shared at all, which invariants require synchronization, how contention appears under real traffic, and what reviewable designs look like when sharing is unavoidable.

Keep the boundary with the next chapters clear. This chapter is about simultaneous access to shared mutable state. Chapter 13 is about coroutine lifetime across suspension. Chapter 14 is about orchestrating groups of tasks, cancellation, and backpressure. Those topics interact, but they are not interchangeable.

## Shared Mutable State Is a Cost Center

Shared state buys coordination convenience at the price of coupling.

Once two threads can mutate the same object graph, local reasoning stops being enough. Every access now depends on synchronization policy, lock ordering, memory ordering, wakeup behavior, and destruction timing. Reviewers must ask not only whether an operation is correct in isolation, but whether the state can be observed between two steps, whether a callback re-enters under a lock, whether waiting code holds resources needed for progress, and whether contention turns a correct design into a slow one.

That cost means the first concurrency decision should be structural:

1. Can this state become thread-confined instead?
2. Can updates be batched, sharded, snapshotted, or message-passed?
3. If sharing is unavoidable, what invariant must be preserved atomically?

Teams often skip directly to lock selection. That is backward. The expensive choice is the sharing topology, not the spelling of `std::mutex`.

## Start by Narrowing the Sharing Surface

The safest shared state is the state you never share.

In service code, many apparently shared structures can be split by traffic key, request lifetime, or ownership role. Metrics ingestion can aggregate per thread and merge periodically. A session table can be sharded by session identifier. A cache can separate immutable value blobs from a small mutable index. A queue consumer can own its local work buffer and publish only completed snapshots.

These moves matter more than switching from one primitive to another because they reduce the number of places where correctness depends on interleaving.

Three reductions are especially common in production systems:

### Thread confinement

Let one thread or one executor own mutation. Everyone else communicates by message, snapshot, or handoff. This is often the simplest answer for request schedulers, connection managers, and event loops. The benefit is not merely fewer locks. It is that the invariants stay local.

### Sharding

Partition state by a stable key so that contention is proportional to hot-key concentration rather than total traffic. Sharding does not remove synchronization, but it narrows the blast radius of each critical section.

### Snapshotting

If readers dominate and they can tolerate slightly stale data, publish immutable snapshots and update them off to the side. Readers get cheap, stable access. Writers pay the complexity cost once.

None of these is free. Confinement can create bottlenecks. Sharding complicates cross-shard operations. Snapshotting increases allocation and copy cost. But those are explicit costs, which is better than paying accidental contention everywhere.

## What Happens Without Synchronization

Before discussing which primitives to use, it is worth seeing what happens when they are absent. The following code has a data race, which is undefined behavior in C++.

```cpp
// BUG: data race — two threads read and write counter without synchronization.
struct metrics {
	int request_count = 0;
	int error_count = 0;
};

metrics g_metrics;

void record_request(bool success) {
	++g_metrics.request_count;            // unsynchronized read-modify-write
	if (!success) ++g_metrics.error_count; // same
}
```

This is not merely a correctness risk; it is undefined behavior per the standard. The compiler and hardware are free to reorder, tear, or elide these operations. In practice, counters may lose updates, report impossible values, or corrupt adjacent memory on architectures with non-atomic word stores. Sanitizers will flag this immediately, but sanitizers are not always running in production.

A subtler variant involves multi-field invariants:

```cpp
// BUG: readers can observe state_ == READY while payload_ is half-written.
struct shared_result {
	std::string payload_;
	enum { EMPTY, READY } state_ = EMPTY;
};

// Writer thread:
result.payload_ = build_payload();   // not yet visible to readers
result.state_ = READY;              // may be reordered before payload_ write

// Reader thread:
if (result.state_ == READY)
	process(result.payload_);        // may see partially constructed string
```

Even if `state_` were atomic, the write to `payload_` could be reordered past it without an appropriate memory order. The lesson: data races are not just about single variables. They are about the visibility ordering of related mutations.

## Raw Mutex Misuse vs. Scoped Guards

Manual lock/unlock is the oldest source of mutex bugs. Consider:

```cpp
// BUG: exception between lock and unlock leaks the lock.
std::mutex mtx;
std::vector<int> data;

void push(int value) {
	mtx.lock();
	data.push_back(value); // may throw (allocation failure)
	mtx.unlock();          // never reached if push_back throws — deadlock on next access
}
```

If `push_back` throws, `unlock()` is skipped. Every subsequent thread that tries to acquire `mtx` will block forever. This is not hypothetical; allocation failure under memory pressure or a throwing copy constructor will trigger it.

The fix is mechanical: use RAII guards.

```cpp
void push(int value) {
	std::scoped_lock lock(mtx);
	data.push_back(value); // if this throws, ~scoped_lock releases the mutex
}
```

`std::scoped_lock` handles single and multiple mutexes with deadlock avoidance. `std::unique_lock` adds the ability to defer locking, transfer ownership, and use condition variables. Prefer `scoped_lock` unless you need the extra flexibility.

```cpp
// unique_lock: needed when the lock must be released before scope exit.
void transfer_expired(registry& reg, std::vector<session>& out) {
	std::unique_lock lock(reg.mutex_);
	auto expired = reg.extract_expired(); // modifies registry under lock
	lock.unlock();                        // release before expensive cleanup
	for (auto& s : expired)
		s.close_socket();                 // no lock held — safe to block
	// out is caller-owned, no synchronization needed
	out.insert(out.end(),
		std::make_move_iterator(expired.begin()),
		std::make_move_iterator(expired.end()));
}
```

## Deadlock from Inconsistent Lock Ordering

When code acquires multiple mutexes, inconsistent ordering is the classic deadlock source.

```cpp
// BUG: deadlock if thread 1 calls transfer(a, b) while thread 2 calls transfer(b, a).
struct account {
	std::mutex mtx;
	int balance = 0;
};

void transfer(account& from, account& to, int amount) {
	std::lock_guard lock_from(from.mtx); // locks 'from' first
	std::lock_guard lock_to(to.mtx);     // then 'to' — opposite order on another thread
	from.balance -= amount;
	to.balance += amount;
}
```

Thread 1 locks `a.mtx` and waits for `b.mtx`. Thread 2 locks `b.mtx` and waits for `a.mtx`. Neither can proceed. `std::scoped_lock` solves this by using `std::lock` internally to acquire both mutexes with deadlock avoidance:

```cpp
void transfer(account& from, account& to, int amount) {
	std::scoped_lock lock(from.mtx, to.mtx); // deadlock-free acquisition
	from.balance -= amount;
	to.balance += amount;
}
```

This is not just a convenience. It is a correctness boundary. Any design requiring multiple mutexes should either use `std::scoped_lock` for simultaneous acquisition or enforce a documented total ordering on locks. Ad hoc ordering disciplines rarely survive refactoring.

## The Performance Cost of Over-Synchronization

Contention is not just about correctness. Excessive locking serializes work that could run in parallel.

```cpp
// Over-synchronized: every stat update contends on one lock.
class request_stats {
	std::mutex mtx_;
	uint64_t total_requests_ = 0;
	uint64_t total_bytes_ = 0;
	uint64_t error_count_ = 0;
public:
	void record(uint64_t bytes, bool error) {
		std::scoped_lock lock(mtx_);
		++total_requests_;
		total_bytes_ += bytes;
		if (error) ++error_count_;
	}
};
```

On a 64-core machine handling millions of requests per second, every thread serializes on one cache line. Lock acquisition, cache-line bouncing, and scheduler wakeups dominate. The better design depends on tolerances:

- If exact consistency between fields is unnecessary, use per-thread counters and merge periodically.
- If only approximate totals are needed, use `std::atomic<uint64_t>` with `memory_order_relaxed` for each counter independently.
- If cross-field consistency is required (e.g., error rate = errors / total), keep the mutex but shard by thread or request key.

The point is not that mutexes are slow. It is that one mutex shared across all cores turns a parallel workload into a serial bottleneck. Measure lock hold time and wait time separately; high wait time with low hold time is the signature of over-synchronization.

## Design Around Invariants, Not Fields

Locks do not protect variables. They protect invariants.

That distinction matters because production objects rarely fail at the field level. They fail when multiple fields must change together and one thread can observe the state between those changes.

A connection pool is not correct because `available_count` is atomic. It is correct if the following relationship always holds under concurrent access: checked-out connections are not also on the idle list, closed connections are not reissued, and waiters are woken when progress becomes possible. Those are invariant statements. If the design does not name them explicitly, the synchronization boundary is already underspecified.

This is where coarse-grained locking sometimes wins. If one mutex cleanly covers one invariant domain, that may be a better design than several finer locks that allow impossible intermediate states or require fragile lock ordering. Fine-grained locking is not advanced by default. It is often just harder to review.

## Anti-pattern: One Lock Around a Growing Service Object

The most common failure shape is not "no synchronization." It is "one reasonable lock that gradually became the service boundary."

```cpp
// Anti-pattern: one mutex protects unrelated invariants and long operations.
class session_registry {
public:
	std::optional<session_info> find(session_id id) {
		std::scoped_lock lock(mutex_);
		auto it = sessions_.find(id);
		if (it == sessions_.end()) {
			return std::nullopt;
		}
		return it->second;
	}

	void expire_idle_sessions(std::chrono::steady_clock::time_point now) {
		std::scoped_lock lock(mutex_);
		for (auto it = sessions_.begin(); it != sessions_.end();) {
			if (it->second.expires_at <= now) {
				close_socket(it->second.socket); // RISK: blocking work under the lock.
				it = sessions_.erase(it);
			} else {
				++it;
			}
		}
	}

private:
	std::mutex mutex_;
	std::unordered_map<session_id, session_info> sessions_;
};
```

This object may survive early testing because it is locally simple. It fails later because unrelated work now shares one queueing point. A read blocks on cleanup. Cleanup holds the mutex while doing I/O. Future features will add metrics, callbacks, and logging inside the same critical section because the lock already exists.

The problem is not just duration of the critical section. It is that the object has no explicit invariant boundaries. Lifetime management, lookup, expiration, and side effects have been collapsed into one synchronization domain.

The better direction is usually to separate state transition from external action: identify which sessions should expire under the lock, move them out, release the lock, then close sockets afterward. That shortens lock scope and makes the invariant easier to state: the protected region updates the registry; external cleanup happens after ownership has been transferred out of the shared structure.

## Minimize Lock Scope, But Do Not Split Logic Blindly

"Keep lock scope small" is correct and incomplete.

A critical section should contain exactly the work required to preserve the invariant, no more and no less. That means:

1. No blocking I/O under the lock.
2. No callbacks into foreign code under the lock.
3. No allocation-heavy or logging-heavy slow paths under the lock if they can be moved out.
4. No splitting of logically atomic state updates merely to make the scope visually shorter.

The last point is where teams get into trouble. A lock that protects a multi-step invariant may need to span several operations. If you release it between steps to look "faster," you may create impossible states. Optimize after you can state what must remain atomic.

## Atomics Are for Narrow Facts, Not Complex Ownership

Atomics are essential and easy to misuse.

Use atomics when the shared fact is truly narrow: a stop flag, a generation counter, an index into a ring buffer, a reference count in an already-sound ownership model, or a statistics counter where relaxed ordering is sufficient. Avoid using atomics as a substitute for structured ownership or for multi-field invariants.

The example project's `TaskRepository` (`examples/web-api/src/modules/repository.cppm`) illustrates the distinction. The ID generator is a single monotonic counter — a textbook narrow fact — so it uses `std::atomic<TaskId>` with `memory_order_relaxed`. The task collection, on the other hand, is a multi-field invariant (the vector contents must be consistent with the IDs issued), so it is protected by a `shared_mutex`. Mixing those two strategies in one class is perfectly sound because the scopes do not overlap: the atomic handles one independent fact, the mutex handles everything else.

An atomic counter does not make a queue safe. An atomic pointer does not make object lifetime trivial. A handful of `memory_order` arguments does not fix a design that lets one thread observe partially published state.

C++23 gives useful tools here, including `std::atomic::wait` and `notify_one` or `notify_all`. They can remove some condition-variable boilerplate for narrow state transitions. They do not change the need to design the state machine first.

If a reviewer cannot explain which value transitions are legal and why the chosen ordering is sufficient, the atomic code is not done.

## Reader-Heavy Data Wants Different Shapes

Contention is often caused less by write frequency than by read design.

A configuration table, routing map, or feature-policy snapshot may be read on every request and updated rarely. Protecting that with a central mutex works functionally and creates avoidable tail latency. In these cases, immutable snapshots or copy-on-write style publication often produce a better system than finer locking.

The tradeoff is explicit:

1. Readers get stable, low-contention access.
2. Writers pay copy and publication cost.
3. Memory pressure may increase due to overlapping generations.
4. Staleness must be acceptable for the domain.

That is often the right trade in request routing, authorization policy, and read-mostly metadata. It is the wrong trade for highly write-heavy order books or frequently mutating shared indexes.

When neither extreme applies — reads are frequent but writes happen on every create, update, or delete request — `std::shared_mutex` with `std::shared_lock` for readers and `std::scoped_lock` for writers is the pragmatic middle ground. The example project's `TaskRepository` (`examples/web-api/src/modules/repository.cppm`) follows exactly this pattern:

```cpp
// repository.cppm — reader-writer locking in practice
class TaskRepository {
    mutable std::shared_mutex mutex_;
    std::vector<Task>         tasks_;
    std::atomic<TaskId>       next_id_{1};
public:
    // Reads take shared_lock — multiple readers proceed in parallel.
    [[nodiscard]] std::optional<Task> find_by_id(TaskId id) const {
        std::shared_lock lock{mutex_};
        auto it = std::ranges::find(tasks_, id, &Task::id);
        if (it == tasks_.end()) return std::nullopt;
        return *it;
    }

    // Writes take scoped_lock — exclusive access preserves invariants.
    [[nodiscard]] Result<Task> create(Task task) {
        std::scoped_lock lock{mutex_};
        // validate, assign id, store ...
    }
};
```

Every read path (`find_by_id`, `find_all`, `find_completed`, `size`) acquires a `shared_lock`, allowing concurrent readers. Every write path (`create`, `update`, `remove`) acquires a `scoped_lock` for exclusive access. The mutex protects the invariant domain — the relationship between `tasks_` and `next_id_` — not individual fields.

The corresponding stress test (`examples/web-api/tests/test_repository.cpp`) validates this design under concurrent load:

```cpp
void test_concurrent_access() {
    webapi::TaskRepository repo;
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 100;

    std::vector<std::jthread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&repo, i]() {
            for (int j = 0; j < ops_per_thread; ++j) {
                auto title = std::format("Task-{}-{}", i, j);
                auto result = repo.create(webapi::Task{.title = std::move(title)});
                assert(result.has_value());
            }
        });
    }
    threads.clear(); // jthreads auto-join
    assert(repo.size() == num_threads * ops_per_thread);
}
```

Eight threads hammering `create` concurrently, then verifying the total count matches expectations. This is a baseline correctness test, not a contention benchmark — but it catches data races and lost updates that would surface under ThreadSanitizer.

## Condition Variables and Wakeup Discipline

Condition variables are where many otherwise careful designs become hand-wavy.

The rule is simple: the wait predicate is part of the invariant, not a convenience expression. A waiting thread must re-check a predicate that is protected by the same synchronization domain that makes the predicate meaningful. Notifications are signals to re-check, not proofs that progress is guaranteed.

In practical terms:

1. Name the predicate precisely: queue not empty, shutdown requested, capacity available, generation changed.
2. Update the predicate state before notifying.
3. Keep waiting code robust against spurious wakeups and shutdown races.
4. Decide whether waking one waiter or all waiters matches the progress model.

Most broken condition-variable code is not broken because the author forgot the loop. It is broken because the predicate is underspecified or split across state that different code paths update inconsistently.

## Hidden Shared State Is Still Shared State

Concurrency bugs often hide in objects that do not look shared from the call site.

Examples include:

1. Allocators or memory resources used by many threads.
2. Logging sinks with internal buffers.
3. Reference-counted handles with shared control blocks.
4. Caches behind seemingly pure helper APIs.
5. Global registries used for plugin discovery, metrics, or tracing.

These deserve the same scrutiny as explicit shared maps and queues. "This helper is thread-safe" is not enough. Ask whether it serializes all callers, whether it allocates under contention, whether it can call user code while holding internal locks, and whether it introduces contention in the hot path without making that cost visible in the API.

## Measuring Contention Changes the Design

Correctness is only the first gate. After that, shared-state design is a measurement problem.

Contention rarely appears as a clear source-level smell. It shows up as queueing time, lock hold distributions, convoy behavior, cache-line bouncing, and scheduler-visible stalls. That means verification must include operational evidence:

1. Measure lock hold time and wait time on hot paths.
2. Track tail latency, not only throughput averages.
3. Observe hot-key skew when using sharding.
4. Profile allocation inside critical sections.
5. Use ThreadSanitizer for race detection and targeted stress tests for deadlock and starvation patterns.

A design that is logically correct but collapses at the ninety-ninth percentile is still a bad concurrency design.

## Review Questions for Shared State

Before approving concurrent shared-state code, ask:

1. What exact invariant does each lock or atomic protect?
2. Could this state be confined, sharded, or snapshotted instead of shared?
3. Does any critical section perform I/O, allocation-heavy work, logging, or callbacks?
4. Are cross-field updates truly atomic with respect to observers?
5. Are condition-variable predicates precise and updated under the right synchronization domain?
6. Where will contention appear under burst load or hot-key skew?
7. What evidence do we have beyond "it passed tests"?

If those questions do not have crisp answers, the design is not ready for production load.

## Takeaways

Shared mutable state is not the default shape of concurrent design. It is the expensive shape.

When sharing is unavoidable, define invariants before choosing primitives. Prefer confinement, sharding, and snapshots over ever more clever locking. Use mutexes to protect invariant domains, atomics for narrow facts, and condition variables only with clearly stated predicates. Then measure the result under realistic load, because correct synchronization can still produce the wrong system if contention dominates behavior.