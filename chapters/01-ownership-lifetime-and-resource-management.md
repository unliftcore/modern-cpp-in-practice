# Chapter 1: Ownership, Lifetime, and Resource Management

## 1.1 The Production Problem

The most expensive recurring defect class in C++ systems is not algorithmic — it is ownership confusion. A resource is acquired, passed through several layers, and eventually must be released. When the code does not make it structurally obvious who is responsible for that release, when the release happens, and what invariants protect the path between acquisition and release, bugs follow. They are familiar: memory leaks under error paths, use-after-free on shutdown, double-close of file descriptors, destructor ordering surprises across compilation units, and APIs whose ownership contracts exist only in comments that drift from reality.

These bugs share a pattern. The code works on the success path. It fails on an exceptional, racy, or shutdown path that nobody tested because nobody could see, from reading the code, that the path existed. Ownership confusion is not a knowledge gap about smart pointers. It is a structural failure: the code's type system and API contracts do not carry enough information for a reviewer to verify correctness by reading.

The cost compounds. When ownership is unclear, every downstream decision becomes harder. Can this pointer be cached? Can it cross a thread boundary? What happens if the caller dies first? What happens during shutdown? Teams that cannot answer these questions from the code end up answering them from crash dumps.

This chapter treats ownership, lifetime, and resource management as a single interrelated design concern. The tools — RAII, move semantics, smart pointers, and handle types — are presented as ways to encode ownership decisions into the type system so that correctness becomes reviewable.

## 1.2 The Naive and Legacy Approach

### 1.2.1 Raw pointers with manual lifetime management

The classic C approach, and the approach inherited by much pre-C++11 code, is to pass raw pointers and document the ownership contract in comments or conventions.

**Anti-pattern: Raw-pointer ownership ambiguity**

```cpp
// A connection cache used by a service backend.
class ConnectionPool {
public:
    // Returns a connection. Caller must call release() when done.
    Connection* acquire(const Endpoint& ep);  // BUG: ownership contract is only in this comment

    void release(Connection* conn);

    ~ConnectionPool();  // Cleans up "orphaned" connections — but what counts as orphaned?

private:
    std::vector<Connection*> connections_;
};

void handle_request(ConnectionPool& pool, const Request& req) {
    Connection* conn = pool.acquire(req.endpoint());
    if (!validate(req)) {
        return;  // BUG: conn is leaked — release() never called
    }
    auto resp = conn->execute(req.query());
    pool.release(conn);
}
```

This code has two independent problems. First, the early return leaks the connection. Second, there is no type-level distinction between "pointer you own and must release" and "pointer you are borrowing." Every caller must remember the protocol. Every reviewer must verify it manually on every path, including exception paths, early returns, and paths added six months later by someone who never read the original comment.

### 1.2.2 The `new`/`delete` ceremony

A step up from the pool example is direct heap allocation with `new` and `delete`. The failure mode is identical — every control-flow path must reach the `delete` — but the damage is worse because the resource is memory, and leaks accumulate silently until a service exhausts its address space at 3 AM.

**Anti-pattern: Manual delete on every path**

```cpp
void process_batch(std::span<const Record> records) {
    auto* buffer = new TransformBuffer(records.size());
    for (const auto& rec : records) {
        if (!buffer->append(transform(rec))) {
            log_error("transform failed for record {}", rec.id());
            delete buffer;
            return;  // RISK: if a second failure path is added later, it will miss the delete
        }
    }
    flush(*buffer);
    delete buffer;
}
```

Adding a second early-return path — or worse, an exception thrown from `transform()` — requires another `delete` or a `try`/`catch` wrapper. The code does not scale with complexity. Every new branch is a new opportunity to forget cleanup.

### 1.2.3 Output parameters and borrowed pointers

Legacy APIs often use output parameters — `bool get_config(Config** out)` — forcing the caller to allocate, pass a pointer-to-pointer, and then manage lifetime. The ownership transfer is invisible at the call site. A reviewer cannot tell whether the callee allocated, whether the caller must free, or whether the pointer aliases something with a longer or shorter lifetime.

These patterns persist in production because they were written before better tools existed, or because a codebase's style predates C++11. They are not wrong because they use pointers. They are wrong because the ownership contract is not encoded in the type system and therefore cannot be verified by reading.

## 1.3 The Modern C++ Approach

### 1.3.1 RAII as an ownership primitive

Resource Acquisition Is Initialization (RAII) ties a resource's lifetime to an object's lifetime. When the object is constructed, the resource is acquired. When the object is destroyed — by going out of scope, by exception unwinding, by container erasure — the resource is released. The compiler guarantees the destructor runs on every exit path. This removes an entire class of "forgot to clean up" bugs.

RAII is not limited to memory. It applies to file descriptors, mutex locks, network connections, GPU handles, database transactions, temporary files, and any resource with an acquire/release pair.

```cpp
// A scoped handle for a POSIX file descriptor.
// Intentional partial: on Windows, use the same pattern with `_close()`.
class ScopedFd {
public:
    explicit ScopedFd(int fd) noexcept : fd_(fd) {}

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~ScopedFd() { reset(); }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

    int release() noexcept { return std::exchange(fd_, -1); }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);  // POSIX close; Windows ports use `_close()`.
        fd_ = fd;
    }

private:
    int fd_ = -1;
};
```

This type makes ownership visible. A function that accepts `ScopedFd` by value takes ownership. A function that accepts `ScopedFd&` borrows it. A function that returns `ScopedFd` transfers ownership to the caller. Copy is deleted because a file descriptor cannot be duplicated by value copy (you would need `dup()`). Move transfers the handle and nullifies the source. The destructor closes the descriptor exactly once.

The connection pool problem from section 1.2 becomes structurally safe:

```cpp
class ConnectionPool {
public:
    // Returns an owned, scoped connection. Cleanup is automatic.
    [[nodiscard]] ScopedConnection acquire(const Endpoint& ep);

private:
    // ...
};

void handle_request(ConnectionPool& pool, const Request& req) {
    auto conn = pool.acquire(req.endpoint());  // ownership is in 'conn'
    if (!validate(req)) {
        return;  // conn's destructor returns it to the pool — no leak
    }
    auto resp = conn->execute(req.query());
    // conn's destructor runs here — no manual release needed
}
```

Every exit path is safe. No comments required. A reviewer can verify correctness by checking the type.

### 1.3.2 Move semantics: transferring ownership without copying

Move semantics exist to express ownership transfer. When a `ScopedFd` is moved, the source gives up its handle and enters a valid-but-empty state. This is not an optimization trick. It is a semantic statement: ownership has been transferred from one scope to another.

The key rule is that a moved-from object must be in a valid, destructible state, but its value is unspecified. For RAII types, this means the moved-from object holds no resource. For value types like `std::string`, it means the moved-from object may be empty, but it is not dangling.

```cpp
// Transferring ownership of a parsed config into a server.
class Server {
public:
    explicit Server(Config config)  // takes ownership by value
        : config_(std::move(config)) {}

private:
    Config config_;
};

auto config = parse_config(path);
Server server(std::move(config));
// 'config' is now moved-from. Do not read its contents.
```

Move semantics eliminate the need for pointer-based ownership transfer in most cases. Instead of `Config* parse_config(...)` with an ambiguous lifetime, you return `Config` by value and let the compiler apply move or copy elision.

### 1.3.3 Smart pointers: encoding ownership policy in the type

When a resource must live on the heap — because its lifetime is not bound to a single scope, or because it is polymorphic — smart pointers encode the ownership policy.

**`std::unique_ptr<T>`** expresses sole ownership. There is exactly one owner. When it goes out of scope, the resource is destroyed. It is move-only: copying is forbidden because two owners would violate the sole-ownership invariant.

```cpp
// A pipeline stage that owns its successor.
class PipelineStage {
public:
    explicit PipelineStage(std::unique_ptr<PipelineStage> next)
        : next_(std::move(next)) {}

    void process(Message msg) {
        transform(msg);
        if (next_) next_->process(std::move(msg));
    }

    virtual ~PipelineStage() = default;

private:
    std::unique_ptr<PipelineStage> next_;
};

// Construction reads bottom-up: the last stage is created first.
auto pipeline = std::make_unique<LogStage>(
    std::make_unique<FilterStage>(
        std::make_unique<SinkStage>()));
```

Ownership is visible in the type. A function that takes `std::unique_ptr<T>` by value consumes the resource. A function that takes `T&` or `T*` borrows it. This distinction is enforceable by the compiler: you cannot accidentally copy a `unique_ptr`.

**`std::shared_ptr<T>`** expresses shared ownership via reference counting. The resource is destroyed when the last `shared_ptr` to it is destroyed.

`shared_ptr` has real costs:

- Atomic reference-count increments and decrements on every copy and destruction. In hot paths, this is measurable contention.
- The control block is a separate heap allocation (unless `std::make_shared` is used, which coalesces it).
- It obscures lifetime reasoning. When five components hold a `shared_ptr` to the same object, "who owns this?" becomes "everyone, sort of" — and destruction time becomes nondeterministic.

Use `shared_ptr` when shared ownership is the actual semantic requirement: an object whose lifetime genuinely depends on multiple independent consumers and cannot be restructured so that one component outlives the others. In practice, this is rarer than most codebases suggest. Many uses of `shared_ptr` are a substitute for thinking about lifetime architecture.

**Anti-pattern: `shared_ptr` as a default to avoid ownership design**

```cpp
class RequestContext {
    std::shared_ptr<Session> session_;         // RISK: who actually owns the session?
    std::shared_ptr<MetricsCollector> metrics_; // RISK: shared across threads with no synchronization story
    std::shared_ptr<Config> config_;            // RISK: config could be replaced mid-request
};
// Every field is shared_ptr, but the code never explains which component
// is responsible for keeping these alive or when they may be destroyed.
// This is not shared ownership — it is deferred ownership design.
```

A better design makes one component the owner and passes references or `std::weak_ptr` to observers:

```cpp
class RequestContext {
    Session& session_;                          // borrowed — lifetime bound to the connection
    MetricsCollector& metrics_;                 // borrowed — lifetime bound to the server
    const Config& config_;                      // borrowed — snapshotted at request start
};
```

The references make it clear that `RequestContext` does not control these lifetimes. If a component needs to check whether the session is still alive (e.g., an async callback that outlives the request), `std::weak_ptr` is the correct tool — it models "observe if alive" rather than "keep alive."

### 1.3.4 `std::weak_ptr`: observation without ownership

`weak_ptr` is a non-owning observer of a `shared_ptr`-managed resource. Calling `lock()` returns a `shared_ptr` if the object is still alive, or an empty `shared_ptr` if it has been destroyed. This is the correct tool for caches, callbacks, and any pattern where a component should react to an object's existence without extending its lifetime.

```cpp
class SessionCache {
public:
    void register_session(std::shared_ptr<Session> session) {
        std::lock_guard lock(mu_);
        sessions_[session->id()] = session;  // weak_ptr: does not extend lifetime
    }

    std::shared_ptr<Session> find(SessionId id) {
        std::lock_guard lock(mu_);
        if (auto it = sessions_.find(id); it != sessions_.end()) {
            if (auto session = it->second.lock()) {
                return session;
            }
            sessions_.erase(it);  // expired — clean up stale entry
        }
        return nullptr;
    }

private:
    std::mutex mu_;
    std::unordered_map<SessionId, std::weak_ptr<Session>> sessions_;
};
```

### 1.3.5 Custom deleters and type-erased cleanup

`unique_ptr` and `shared_ptr` both support custom deleters, which makes them usable for C-library handles, OS resources, and any type where cleanup is not a simple `delete`.

```cpp
// Wrapping a C library handle with automatic cleanup.
struct PngImageDeleter {
    void operator()(png_structp png) const noexcept {
        png_destroy_read_struct(&png, nullptr, nullptr);
    }
};
using ScopedPng = std::unique_ptr<png_struct, PngImageDeleter>;

// For shared_ptr, the deleter is type-erased and stored in the control block:
auto db = std::shared_ptr<sqlite3>(nullptr, [](sqlite3* db) {
    if (db) sqlite3_close(db);
});
sqlite3_open(":memory:", std::out_ptr(db));  // C++23 std::out_ptr
```

`std::out_ptr` and `std::inout_ptr` (C++23) bridge the gap between smart pointers and C APIs that return resources through output parameters. They eliminate the manual dance of releasing and re-wrapping. This is the part of the chapter to revisit when auditing ownership at C API boundaries.

### 1.3.6 Factory functions and the `[[nodiscard]]` contract

Functions that return owning types should be marked `[[nodiscard]]` so that discarding the result — and therefore leaking the resource — produces a compiler warning.

```cpp
[[nodiscard]] std::unique_ptr<Pipeline> create_pipeline(const PipelineConfig& cfg);

// Calling without capturing the result:
create_pipeline(cfg);  // warning: ignoring return value with 'nodiscard' attribute
```

This is a low-cost, high-signal annotation. Apply it to every factory function that returns an owning handle.

### 1.3.7 Ownership at architectural boundaries

Within a single function, RAII handles cleanup. The harder problem is lifetime across architectural boundaries: between layers, between threads, between a request and the server that spawned it.

The guiding principle: **ownership flows in one direction, and borrowing is bounded by the owner's lifetime.** When a server owns a `Config` object, request handlers borrow it by reference. If the config can be replaced at runtime, a snapshot mechanism (e.g., `std::shared_ptr<const Config>` that is atomically swapped) ensures that in-flight requests hold a stable reference.

```cpp
class Server {
public:
    void reload_config(Config new_cfg) {
        auto snapshot = std::make_shared<const Config>(std::move(new_cfg));
        std::atomic_store(&config_, snapshot);
    }

    std::shared_ptr<const Config> current_config() const {
        return std::atomic_load(&config_);
    }

private:
    std::shared_ptr<const Config> config_;
};
```

Each request captures a `shared_ptr<const Config>` at its start. The config is immutable once published. The old config is destroyed when the last request referencing it completes. This is one of the legitimate uses of `shared_ptr`: the ownership is genuinely shared across an unpredictable number of concurrent readers, and the lifetime of the old config depends on when the last reader finishes.

## 1.4 Tradeoffs and Boundaries

### 1.4.1 When RAII does not fit

RAII assumes that destruction is the right time to release a resource. This breaks down when:

- **Cleanup can fail.** Destructors must be `noexcept` (in practice, always). If closing a file descriptor can return an error that must be handled, the destructor cannot propagate it. The solution is to provide an explicit `close()` method that the caller invokes before destruction, and have the destructor close silently as a safety net.
- **Cleanup order is external.** In plugin systems or DLL-based architectures, objects may outlive the library that knows how to destroy them. Shared-library unload order is not governed by C++ scope rules. The fix is usually to ensure that all objects are destroyed before unloading, or to use weak references to detect stale handles.
- **Resources are cyclically dependent.** Two objects that own each other via `shared_ptr` will never be destroyed. Break cycles with `weak_ptr`, or restructure so that one side has a strictly longer lifetime.

### 1.4.2 The cost of `shared_ptr`

Reference counting is not free. On x86-64, each `shared_ptr` copy and destruction involves an atomic increment or decrement (`lock xadd`). Under contention — many threads copying the same `shared_ptr` — this becomes a cache-line bounce. Profile before assuming it is negligible. In hot paths, prefer passing by `const&` to avoid unnecessary reference-count churn.

`shared_ptr` also makes lifetime analysis harder for tooling. Static analyzers and sanitizers can reason about `unique_ptr` ownership transfers. `shared_ptr` ownership is determined at runtime, making static analysis weaker.

### 1.4.3 Moved-from state pitfalls

The standard guarantees that moved-from standard library types are in a "valid but unspecified" state. For your own types, you must define what that means. A moved-from `ScopedFd` holds `-1`. A moved-from `std::vector` or `std::string` is often empty on mainstream implementations, but that is not a portable guarantee. Do not read moved-from objects except to assign to them or destroy them — unless your type documents a stronger guarantee.

**Anti-pattern: Using a moved-from object**

```cpp
auto pipeline = build_pipeline(cfg);
server.install(std::move(pipeline));
pipeline->process(msg);  // BUG: pipeline is moved-from, likely null
```

### 1.4.4 Dangling references

RAII protects owned resources. It does not protect references and pointers to objects whose lifetime is managed elsewhere. The most common source of dangling references in modern C++ is returning a reference or iterator to a local or temporary, or capturing a reference in a lambda that outlives the referent.

**Anti-pattern: Dangling reference from a temporary**

```cpp
const std::string& name = get_user(id).name();
// BUG: if get_user returns User by value, the temporary is destroyed
// at the semicolon, and 'name' is a dangling reference.
```

**Anti-pattern: Lambda capturing a reference that dies**

```cpp
void Server::start_async_task(const Request& req) {
    auto task = [&req]() {  // RISK: req may be destroyed before task runs
        process(req);
    };
    thread_pool_.submit(std::move(task));
}
```

The fix is to capture by value or by `shared_ptr` when the lambda may outlive the referent.

## 1.5 Testing and Tooling Implications

### 1.5.1 AddressSanitizer (ASan)

ASan detects use-after-free, heap buffer overflow, stack buffer overflow, and memory leaks at runtime. Compile with `-fsanitize=address` (GCC, Clang) or `/fsanitize=address` (MSVC). Run your test suite and integration tests under ASan. It adds roughly 2x runtime overhead and 2-3x memory overhead, which is acceptable for CI but not production.

ASan catches the dangling-reference and use-after-free bugs that ownership confusion produces. It is the single most effective tool for this chapter's failure modes.

### 1.5.2 LeakSanitizer (LSan)

LSan detects memory leaks. It is included in ASan by default on Linux (Clang, GCC). It runs at process exit and reports any heap memory that is still reachable but was not freed. This catches `shared_ptr` cycles and forgotten `unique_ptr` releases.

### 1.5.3 MemorySanitizer (MSan)

MSan detects reads of uninitialized memory. It is relevant when a moved-from object's state is used accidentally. Available in Clang; not supported by GCC or MSVC as of this writing.

### 1.5.4 Static analysis

Clang-Tidy checks relevant to this chapter:

- `bugprone-use-after-move` — flags reads of moved-from objects.
- `clang-analyzer-cplusplus.NewDeleteLeaks` — detects unmatched `new`/`delete`.
- `cppcoreguidelines-owning-memory` — enforces `gsl::owner<T>` annotations on raw owning pointers.
- `misc-uniqueptr-reset-release` — detects inefficient `unique_ptr` transfer patterns.
- `performance-unnecessary-copy-initialization` — flags copies where a reference or move would suffice.

### 1.5.5 Lifetime annotations (Clang)

Clang's `-Wdangling` family of warnings catches some classes of dangling references at compile time. The `-Wreturn-stack-address` warning catches returns of pointers/references to locals. These are not exhaustive, but they are free and should be enabled.

### 1.5.6 Compile flags for ownership hygiene

At minimum, enable these across your build:

```
-Wall -Wextra -Wpedantic
-Werror=return-type        # undefined behavior if a non-void function falls off the end
-Wdangling                 # Clang: warn on some dangling reference patterns
-Wnull-dereference         # GCC: warn on null pointer dereference paths
```

## 1.6 Review Checklist

Apply these checks when reviewing code that acquires, transfers, or releases resources:

1. **Every resource has a single, visible owner.** If ownership is shared, the sharing is intentional and documented with `shared_ptr`. If ownership is unclear, the code is not ready for review.

2. **Owning types are non-copyable or have well-defined copy semantics.** A handle type that wraps a file descriptor, connection, or OS resource should delete its copy constructor or implement deep-copy semantics with a clear cost.

3. **Factory functions are `[[nodiscard]]`.** Callers cannot silently discard an owning return value.

4. **No `new`/`delete` outside of RAII wrappers.** Direct `new` should appear only inside `make_unique`, `make_shared`, or a custom allocator. Direct `delete` should appear only inside a destructor or `reset()`.

5. **Moved-from objects are not read.** After `std::move(x)`, the only operations on `x` should be destruction or assignment.

6. **Lambdas that escape their scope capture by value or by `shared_ptr`.** Reference captures in lambdas submitted to thread pools, timers, or async continuations are presumed dangling until proven otherwise.

7. **`shared_ptr` usage is justified.** Each `shared_ptr` field should have a comment or design note explaining why sole ownership (`unique_ptr`) or borrowing (reference) is insufficient.

8. **Destructors are `noexcept`.** If cleanup can fail, an explicit `close()`/`flush()` method handles the error, and the destructor is a silent safety net.

9. **Custom deleters are tested.** If a `unique_ptr` or `shared_ptr` uses a custom deleter for a C handle or OS resource, the cleanup path has a test that verifies the resource is actually released.

10. **ASan and LSan pass.** The CI pipeline runs the test suite under AddressSanitizer with LeakSanitizer enabled. No suppressions are added without a tracking issue.
