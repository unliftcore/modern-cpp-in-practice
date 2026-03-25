# Chapter 9: Shared State, Synchronization, and Contention

> **Prerequisites:** This chapter requires the ownership model from Chapter 1 and the value-semantics discussion from Chapter 3. Sharing state across threads is an ownership decision: if you cannot articulate who owns a piece of data, adding a mutex does not fix the problem — it hides it behind a lock. Value semantics determine what can be copied into thread-local scope and what must remain shared. Readers should also be comfortable with the interface boundary ideas from Chapter 7, since the synchronization surface of a component is part of its public contract.

## 9.1 The Production Problem

Most concurrency bugs in shipped C++ are not exotic. They fall into a small number of categories:

1. **Unprotected shared mutation.** Two threads write the same field without synchronization. The program has a data race, which is undefined behavior — not merely a logic error.
2. **Lock-order inversion.** Two mutexes are acquired in inconsistent order across call sites, producing a deadlock that surfaces only under specific scheduling.
3. **Contention collapse.** A mutex serializes so much work that adding threads makes throughput worse, not better.
4. **Stale or torn reads.** An atomic variable is read with `memory_order_relaxed` without understanding the ordering implications, and dependent state is observed in an inconsistent combination.
5. **Lifetime races.** A thread holds a reference to an object whose owner has already destroyed it. The synchronization primitive itself was fine; the lifetime contract was not.

These failures share a common root: the code does not state, in a way reviewable by another engineer, which data is shared, what invariants protect it, and what ordering constraints apply. The primitives are available. The design discipline is not.

---

## 9.2 The Naive Approach: Mutexes Everywhere, Reasoning Nowhere

The reflexive response to a data race report is to wrap the offending access in a `std::mutex`. This works locally and scales poorly.

### 9.2.1 Anti-pattern: The God Mutex

```cpp
// Anti-pattern: single mutex guarding unrelated state
class SessionManager {
    std::mutex mtx_;
    std::unordered_map<SessionId, Session> sessions_;   // hot path
    std::vector<std::string> audit_log_;                 // cold path
    MetricsSnapshot last_snapshot_;                      // periodic

public:
    void add_session(SessionId id, Session s) {
        std::lock_guard lock(mtx_);
        sessions_.emplace(id, std::move(s));
        audit_log_.push_back("added " + to_string(id)); // BUG: audit I/O under session lock
    }

    MetricsSnapshot snapshot() {
        std::lock_guard lock(mtx_); // RISK: contention from unrelated field
        return last_snapshot_;
    }
};
```

Three unrelated data fields share one mutex. The hot path (session lookup and insertion) contends with the cold path (audit logging) and a periodic read (metrics snapshot). Under load, every thread stalls on a single lock even when the operations are logically independent.

The deeper issue: this design encodes no information about which invariants the mutex protects. The lock "protects everything," which means a reviewer cannot tell whether a new field added later needs the same lock, a different lock, or no lock at all.

### 9.2.2 Anti-pattern: Lock-and-Forget Composition

```cpp
// Anti-pattern: holding a lock while calling user-provided code
class Registry {
    std::mutex mtx_;
    std::map<std::string, Callback> handlers_;

public:
    void dispatch(const std::string& event, const Payload& p) {
        std::lock_guard lock(mtx_);
        if (auto it = handlers_.find(event); it != handlers_.end()) {
            it->second(p); // BUG: callback may re-enter Registry or acquire another lock
        }
    }
};
```

Calling unknown code under a lock invites re-entrance and lock-order violations. The fix is not "use a recursive mutex." It is to copy or move the data you need, release the lock, and then call outward.

---

## 9.3 Modern C++ Approach: Bounded Sharing with Explicit Invariants

The goal is a design where shared state is visible, bounded, and reviewable. Three principles guide the approach.

### 9.3.1 Principle 1: Minimize What Is Shared

The cheapest synchronization is the one you eliminate. When a value can be copied into a thread's local scope, do that instead of sharing.

```cpp
struct PricingConfig {
    double spread_bps;
    double max_notional;
    std::chrono::milliseconds staleness_limit;
};

// Thread-local copy, no synchronization needed
void run_pricing_loop(PricingConfig config, /* ... */) {
    // config is owned entirely by this thread.
    // Updates are published by replacing the config atomically (see Principle 3).
    while (running_.load(std::memory_order_acquire)) {
        apply_spread(config.spread_bps);
        // ...
    }
}
```

Value semantics (Chapter 3) make this viable. If `PricingConfig` were a polymorphic base class held by pointer, copying it safely would require a clone protocol and careful lifetime management. Flat value types copy trivially.

### 9.3.2 Principle 2: Protect Invariants, Not Fields

A mutex should guard a named invariant, not "all the members." Document the invariant in the code, not in a wiki that drifts.

```cpp
class OrderBook {
    // Invariant: bids_ and asks_ are consistent snapshots.
    // Any mutation must update both under book_mtx_.
    mutable std::mutex book_mtx_;
    SortedLevels bids_;
    SortedLevels asks_;

    // Independent of the book invariant; uses its own lock.
    mutable std::mutex stats_mtx_;
    TradeStats stats_;

public:
    Spread top_of_book() const {
        std::lock_guard lock(book_mtx_);
        return {bids_.best(), asks_.best()};
    }

    void record_fill(const Fill& f) {
        // Only stats lock needed — no interaction with book invariant.
        std::lock_guard lock(stats_mtx_);
        stats_.record(f);
    }
};
```

Two mutexes, two invariants, zero interaction between them. A reviewer can verify each invariant in isolation. If a new method needs both locks, that is a design signal — either the invariants are coupled and should merge, or the method is doing too much.

### 9.3.3 Principle 3: Publish Immutable Snapshots

For data that is read far more often than it is written, the read-copy-update pattern avoids holding a lock on the read path entirely.

```cpp
class ConfigStore {
    std::atomic<std::shared_ptr<const AppConfig>> current_;

public:
    std::shared_ptr<const AppConfig> load() const {
        return current_.load(std::memory_order_acquire);
    }

    void publish(AppConfig new_config) {
        current_.store(
            std::make_shared<const AppConfig>(std::move(new_config)),
            std::memory_order_release
        );
    }
};
```

`std::atomic<std::shared_ptr<T>>` (C++20) lets readers load a snapshot without a mutex. On common contemporary platforms this is often efficient enough for configuration publication, but the standard does not guarantee lock-free behavior. The writer allocates a new immutable config and publishes it atomically. Readers that loaded the old pointer keep it alive via `shared_ptr` reference counting until they are done.

Tradeoff: each update allocates. If updates are infrequent (configuration changes, periodic recalculation), the allocation is negligible. If updates are per-message, the allocation pressure can dominate, and a different strategy — such as a seqlock or epoch-based reclamation — becomes necessary.

### 9.3.4 Scoped Locking and `std::scoped_lock`

When multiple locks must be held simultaneously, `std::scoped_lock` uses a deadlock-avoidance algorithm internally (equivalent to `std::lock`):

```cpp
void transfer(Account& from, Account& to, Amount amt) {
    std::scoped_lock lock(from.mtx_, to.mtx_); // deadlock-free regardless of argument order
    from.debit(amt);
    to.credit(amt);
}
```

This eliminates manual lock ordering for the specific case of acquiring multiple locks at once. It does not help when locks are acquired at different points in a call chain — that requires a documented lock hierarchy.

### 9.3.5 `std::shared_mutex` for Read-Heavy Workloads

When most accesses are reads and writes are infrequent, `std::shared_mutex` allows concurrent readers:

```cpp
class SymbolTable {
    mutable std::shared_mutex mtx_;
    std::unordered_map<std::string, SymbolInfo> table_;

public:
    std::optional<SymbolInfo> lookup(const std::string& name) const {
        std::shared_lock lock(mtx_);
        if (auto it = table_.find(name); it != table_.end())
            return it->second;
        return std::nullopt;
    }

    void update(const std::string& name, SymbolInfo info) {
        std::unique_lock lock(mtx_);
        table_.insert_or_assign(name, std::move(info));
    }
};
```

`std::shared_mutex` is not free. On most implementations it is heavier than `std::mutex` even for uncontended unique locks. Use it when profiling shows that reader contention on a plain mutex is the bottleneck, not as a default.

---

## 9.4 Atomics and Memory Ordering

Atomics are not a replacement for mutexes. They protect individual variables; mutexes protect invariants spanning multiple variables. Reaching for atomics to avoid "the cost of a mutex" without understanding the memory-ordering contract is a common source of subtle bugs that pass every test and fail in production under high core counts.

### 9.4.1 The Ordering Spectrum

| Order | Guarantees | Typical Use |
|---|---|---|
| `memory_order_relaxed` | Atomicity only. No ordering relative to other memory operations. | Counters, statistics that tolerate stale reads. |
| `memory_order_acquire` / `release` | A release-store is visible to a subsequent acquire-load, and all writes before the release are visible after the acquire. | Flag-based signaling, publication of initialized data. |
| `memory_order_seq_cst` | Total order across all `seq_cst` operations on all threads. | Default. Use when reasoning about weaker orders is not worth the risk. |

### 9.4.2 Anti-pattern: Relaxed Ordering on a Flag

```cpp
// Anti-pattern: relaxed flag guarding non-atomic data
struct SharedWork {
    std::atomic<bool> ready{false};
    Payload payload; // non-atomic

    void publish(Payload p) {
        payload = std::move(p);
        ready.store(true, std::memory_order_relaxed); // BUG: payload write may not be visible
    }

    Payload consume() {
        while (!ready.load(std::memory_order_relaxed)) {}
        return payload; // BUG: may read uninitialized or partially written payload
    }
};
```

`relaxed` provides no ordering. The reader can observe `ready == true` before the write to `payload` is visible. The fix is `release` on the store and `acquire` on the load:

```cpp
struct SharedWork {
    std::atomic<bool> ready{false};
    Payload payload;

    void publish(Payload p) {
        payload = std::move(p);
        ready.store(true, std::memory_order_release);
    }

    Payload consume() {
        while (!ready.load(std::memory_order_acquire)) {
            // Spin — acceptable only for short waits; prefer condition_variable otherwise.
        }
        return payload;
    }
};
```

The acquire-release pair guarantees that all writes before the `release` store are visible to the thread that observes `true` via the `acquire` load.

### 9.4.3 When `seq_cst` Is the Right Default

`memory_order_seq_cst` is the default for `std::atomic` operations because it provides the strongest guarantees. Weakening the order is an optimization. Like all optimizations, it requires evidence that the stronger order is actually a bottleneck and that the weaker order is correct. In practice, `seq_cst` is rarely the bottleneck; the bottleneck is usually contention on the cache line itself, which no memory order eliminates.

Use `relaxed` for statistics counters and approximate metrics. Use `acquire`/`release` for flag-and-data publication patterns. Use `seq_cst` everywhere else until a profiler says otherwise.

---

## 9.5 Condition Variables and Waiting

Raw spin loops waste CPU. `std::condition_variable` is the standard mechanism for blocking until a predicate holds.

```cpp
template <typename T, std::size_t Capacity>
class BoundedQueue {
    std::mutex mtx_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::array<T, Capacity> buf_;
    std::size_t head_ = 0, tail_ = 0, count_ = 0;

public:
    void push(T item) {
        std::unique_lock lock(mtx_);
        not_full_.wait(lock, [&] { return count_ < Capacity; });
        buf_[tail_] = std::move(item);
        tail_ = (tail_ + 1) % Capacity;
        ++count_;
        not_empty_.notify_one();
    }

    T pop() {
        std::unique_lock lock(mtx_);
        not_empty_.wait(lock, [&] { return count_ > 0; });
        T item = std::move(buf_[head_]);
        head_ = (head_ + 1) % Capacity;
        --count_;
        not_full_.notify_one();
        return item;
    }
};
```

Key discipline: always pass a predicate to `wait()`. The two-argument form (`wait(lock, predicate)`) re-checks the condition on spurious wakeups. The one-argument form without a predicate is a bug waiting to happen.

### 9.5.1 `std::stop_token` Integration (C++20)

For cancellable waits, `std::condition_variable_any` accepts a `std::stop_token`:

```cpp
T pop_or_cancel(std::stop_token token) {
    std::unique_lock lock(mtx_);
    if (!not_empty_.wait(lock, token, [&] { return count_ > 0; })) {
        throw operation_cancelled{}; // stop was requested
    }
    // ... dequeue as before
}
```

This avoids the pattern of polling a separate cancellation flag inside a loop and composes cleanly with `std::jthread`, which provides cooperative cancellation via `stop_token`.

---

## 9.6 Thread Lifecycle and `std::jthread`

`std::thread` has a destructive edge: if a `std::thread` object is destroyed while still joinable, `std::terminate` is called. In exception paths and early returns, this creates landmines.

`std::jthread` (C++20) joins automatically on destruction and carries a `std::stop_source` for cooperative cancellation:

```cpp
class BackgroundProcessor {
    std::jthread worker_;

public:
    explicit BackgroundProcessor(BoundedQueue<Task>& queue)
        : worker_([&queue](std::stop_token st) {
              while (!st.stop_requested()) {
                  auto task = queue.pop_or_cancel(st);
                  process(task);
              }
          })
    {}

    // Destructor requests stop and joins automatically.
    // No manual join(), no risk of std::terminate.
};
```

Prefer `std::jthread` over `std::thread` in all new code. The only reason to use `std::thread` is interoperability with APIs that require it.

---

## 9.7 Tradeoffs and Boundaries

### 9.7.1 Mutex vs. Atomic vs. Lock-Free

The decision is not "mutexes are slow, atomics are fast." The decision depends on what you are protecting:

- **Single scalar, independent of other state:** `std::atomic` is appropriate. Example: a monotonic counter, a boolean flag.
- **Multiple fields that form a single invariant:** A mutex is appropriate. Attempting to protect a multi-field invariant with multiple atomics introduces ordering bugs that are nearly impossible to test.
- **Lock-free data structures:** Appropriate when you have measured contention, rejected simpler alternatives, and are prepared to invest heavily in verification. Writing a correct lock-free queue is harder than writing a correct mutex-based queue and harder to maintain. Use a proven library (such as Folly's `MPMCQueue`, Boost.Lockfree, or libcds) rather than writing your own.

### 9.7.2 `std::shared_mutex` vs. `std::mutex`

`std::shared_mutex` wins only when the reader-to-writer ratio is high (roughly 10:1 or more) and the critical section is long enough that reader serialization is measurable. For short critical sections — a handful of map lookups — an uncontended `std::mutex` is often faster because `std::shared_mutex` has higher per-operation overhead on most implementations.

### 9.7.3 False Sharing

Atomics and mutexes that live on the same cache line contend even when they protect unrelated state:

```cpp
// Anti-pattern: two independent atomics on the same cache line
struct Counters {
    std::atomic<uint64_t> requests{0}; // RISK: false sharing
    std::atomic<uint64_t> errors{0};
};
```

Fix with alignment:

```cpp
struct Counters {
    alignas(std::hardware_destructive_interference_size)
    std::atomic<uint64_t> requests{0};

    alignas(std::hardware_destructive_interference_size)
    std::atomic<uint64_t> errors{0};
};
```

`std::hardware_destructive_interference_size` (C++17) gives the L1 cache line size. On most x86 platforms this is 64 bytes; on Apple Silicon it is 128. If your implementation does not provide it, use a platform-specific constant.

### 9.7.4 Contention Under Skew

Real workloads are rarely uniform. A single hot key in a sharded map, a burst of writes to a usually-read-heavy config, a shutdown sequence that takes a lock held by every in-flight request — skewed access patterns are where synchronization designs actually fail.

Design for the worst observed contention, not the average. This means:

- Profile under realistic load, including startup, shutdown, and failure injection.
- Separate hot-path and cold-path state into distinct synchronization domains.
- Treat lock hold time as a metric. Instrument it in debug builds.

---

## 9.8 Testing and Tooling Implications

### 9.8.1 ThreadSanitizer (TSan)

TSan detects data races at runtime. It is the single most effective tool for finding concurrency bugs in C++ and should run in CI on every commit.

```bash
# GCC / Clang
g++ -fsanitize=thread -g -O1 -o my_test my_test.cpp
./my_test
```

TSan reports races with stack traces for both accesses. Its overhead is roughly 5–15x slower execution and 5–10x more memory. This means tests must be structured to exercise concurrent paths within a reasonable wall-clock budget.

Caveats:

- TSan cannot detect all logic-level races (e.g., TOCTOU bugs where each access is individually synchronized but the compound operation is not).
- It does not prove the absence of races — only the absence of races on the paths that were exercised.
- Mixing TSan with other sanitizers (particularly MSan) in the same binary is not supported.

### 9.8.2 Deadlock Detection

TSan includes a lock-order inversion detector. Clang's `-fsanitize=thread` reports potential deadlocks even when they do not manifest during the test run, based on observed lock-acquisition graphs.

For runtime deadlock detection in production, some teams instrument `std::mutex` wrappers that record acquisition order and assert consistency. This is worth the effort in systems with more than a handful of mutexes.

### 9.8.3 Stress Testing and Nondeterminism

Concurrency bugs depend on scheduling. A test that passes 99 times and fails on the 100th is not flaky — it is detecting a real race with low probability.

Strategies:

- **Run tests with varying thread counts.** A two-thread test may never trigger a race that a 16-thread test exposes.
- **Insert artificial delays.** `std::this_thread::yield()` or short sleeps at synchronization boundaries increase the chance of interleaving. Remove these before production builds.
- **Use `CHESS`-style model checkers** (such as CDSChecker or Relacy) for critical lock-free algorithms. These systematically explore interleavings rather than relying on scheduling luck.
- **Run TSan tests repeatedly in CI.** A single pass provides moderate confidence; 10–50 repetitions under TSan provide substantially more.

### 9.8.4 Observability

Concurrency problems are diagnosed after the fact when they cannot be reproduced. Build observability into the synchronization layer:

- Log lock contention metrics (wait time, hold time) in debug and staging builds.
- Tag thread names using `pthread_setname_np` (POSIX) or the platform equivalent. This makes profiler output and core dumps readable.
- Record the thread ID and timestamp in structured log entries so that interleaving can be reconstructed from logs.

---

## 9.9 Putting It Together: A Concurrent Cache

The following example applies the principles from this chapter to a realistic component: a thread-safe LRU cache for a service that resolves user profiles.

```cpp
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <list>
#include <optional>
#include <string>

struct UserProfile {
    std::string display_name;
    std::string email;
    int permission_level;
};

class ProfileCache {
    // Invariant: lru_list_ and map_ are consistent.
    // map_[key].iterator points to the corresponding lru_list_ node.
    // lru_list_ is ordered from most-recently-used (front) to least-recently-used (back).
    //
    // All mutations require exclusive lock. Reads *could* use shared_mutex,
    // but lookup promotes the entry (mutation), so exclusive lock is needed here too.
    struct Entry {
        std::string key;
        UserProfile profile;
    };

    mutable std::mutex mtx_;
    std::size_t capacity_;
    std::list<Entry> lru_list_;
    std::unordered_map<std::string,
                       std::list<Entry>::iterator> map_;

public:
    explicit ProfileCache(std::size_t capacity)
        : capacity_(capacity) {}

    std::optional<UserProfile> get(const std::string& user_id) {
        std::lock_guard lock(mtx_);
        auto it = map_.find(user_id);
        if (it == map_.end()) return std::nullopt;

        // Promote to front of LRU list.
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return it->second->profile;
    }

    void put(std::string user_id, UserProfile profile) {
        std::lock_guard lock(mtx_);
        if (auto it = map_.find(user_id); it != map_.end()) {
            it->second->profile = std::move(profile);
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return;
        }

        if (map_.size() >= capacity_) {
            auto& evict = lru_list_.back();
            map_.erase(evict.key);
            lru_list_.pop_back();
        }

        lru_list_.push_front(Entry{user_id, std::move(profile)});
        map_.emplace(user_id, lru_list_.begin());
    }
};
```

Design notes:

- **Single mutex, one invariant.** The LRU list and the map are coupled — they must stay consistent. One mutex is correct here. Using `shared_mutex` would be wrong because reads promote entries, which mutates the list.
- **No external calls under the lock.** `get()` and `put()` do only in-memory work. If a cache miss needs to call a downstream service, the caller does that outside the cache's lock and then calls `put()`.
- **Value copies out.** `get()` returns `std::optional<UserProfile>` by value. The caller owns the copy. There is no dangling reference risk if the entry is later evicted.
- **Capacity is a constructor argument.** No runtime reconfiguration of the eviction policy under a lock, avoiding a class of subtle bugs.

For higher throughput under heavy read load, a sharded design (multiple `ProfileCache` instances, keyed by a hash of the user ID) reduces contention by a factor proportional to the shard count, without changing the per-shard logic.

---

## 9.10 Review Checklist

Use this checklist during code review for any change that introduces or modifies shared mutable state.

- [ ] **Shared state is identified.** Every field accessed by more than one thread is explicitly marked (by comment, naming convention, or annotation).
- [ ] **Each mutex has a documented invariant.** The code states what the mutex protects, not just that it exists.
- [ ] **Lock scope is minimal.** No I/O, allocation, or callback invocation occurs while holding a lock unless there is a documented reason.
- [ ] **No lock-order ambiguity.** If more than one mutex can be held simultaneously, the acquisition order is documented and consistent across all call sites. Prefer `std::scoped_lock` for multi-lock acquisition.
- [ ] **Atomic operations use correct memory ordering.** Any use of `memory_order_relaxed` or `memory_order_acquire`/`release` has a comment explaining why `seq_cst` is not needed and what ordering guarantee the code relies on.
- [ ] **No data shared by raw pointer or reference without synchronization.** If a pointer to mutable state crosses a thread boundary, the synchronization mechanism is visible at the point of sharing.
- [ ] **Thread lifecycle is managed by RAII.** `std::jthread` is preferred. If `std::thread` is used, the join or detach point is obvious and exception-safe.
- [ ] **Cancellation is cooperative.** Threads check `std::stop_token` or an equivalent flag. No thread is killed or abandoned with live resources.
- [ ] **False sharing is addressed for hot atomics.** Independent atomics updated on different threads are separated by at least one cache line.
- [ ] **ThreadSanitizer passes.** The change has been tested under TSan with concurrent workloads that exercise the shared paths.
- [ ] **Contention is measurable.** Lock hold time and wait time can be observed in debug or staging builds. There is a plan to detect contention regression.
- [ ] **Lifetime outlives all accessors.** The shared object is guaranteed to outlive every thread or task that references it. If `shared_ptr` is used, the reason it cannot be replaced by a simpler ownership model is documented.
