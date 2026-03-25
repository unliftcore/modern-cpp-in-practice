# Chapter 2: Errors, Results, and Failure Boundaries

*Prerequisites: Chapter 1 (ownership determines which cleanup paths run when failure occurs). Familiarity with RAII, move semantics, and basic exception mechanics.*

## 2.1 The Production Problem

A payment service processes card authorizations. Each request touches a configuration cache, a fraud-scoring client, a ledger database, and an audit logger. Four subsystems, four ways they can fail: the cache returns stale data, the fraud scorer times out, the database rejects a write due to a constraint violation, the audit logger hits a full disk. The team has no policy for how failure information crosses these boundaries. Over time, the codebase accumulates all of the following:

- Exceptions thrown from deep in a gRPC stub, caught three layers up where the context is gone.
- Integer error codes returned from the database driver, silently cast to `bool`.
- `std::optional` used as a poor error channel with no diagnostic information.
- `LOG(ERROR)` calls that substitute for returning failure, leaving the caller to proceed on invalid state.

The result is not one bug. It is an environment where the cost of every incident is inflated because nobody can trace the failure path without reading the full call stack, and nobody can write a test that exercises a specific failure mode without mocking four subsystems at once.

This chapter is about making error policy explicit enough to avoid that situation. The question is not "exceptions versus error codes." It is: what failure information crosses each boundary, how does the caller know a call failed, and what happens to intermediate state when it does.

## 2.2 Naive and Legacy Approaches

### 2.2.1 Integer and enum status codes

C-heritage codebases use integer return values or output parameters. The pattern is well-understood but has two structural problems in C++ that do not exist in C.

First, ignoring a return value is silent. C++23 has `[[nodiscard]]`, but legacy interfaces lack it, and adding it retroactively generates warnings across every existing call site. Second, constructors cannot return a status code. This forces a two-phase initialization pattern that defeats RAII.

**Anti-pattern: two-phase initialization**

```cpp
class DatabaseConnection {
public:
    DatabaseConnection() = default;

    // BUG: caller must remember to call init() before use.
    // If init() fails, the object exists in a zombie state
    // that is neither valid nor safely destructible in all cases.
    int init(const std::string& connection_string) {
        handle_ = db_open(connection_string.c_str());
        if (!handle_) return -1;
        return 0;
    }

    void query(std::string_view sql) {
        // RISK: no check that init() succeeded.
        db_exec(handle_, sql.data());
    }

    ~DatabaseConnection() {
        if (handle_) db_close(handle_);
    }

private:
    db_handle* handle_ = nullptr;
};
```

The zombie state between construction and `init()` is a source of bugs that scales with the number of developers using the class. Every call site carries an implicit obligation that the type system does not enforce.

### 2.2.2 Exceptions everywhere

The opposite failure mode is a codebase that uses exceptions for all errors, including expected conditions like "user not found" or "cache miss." This has two costs.

Performance cost is real but often overstated. On major implementations, the happy path is nearly free; the throw path is expensive (typically microseconds, involving RTTI, stack unwinding, and heap allocation for the exception object). The larger problem is control-flow clarity. When any function might throw, reasoning about intermediate state requires understanding every possible throw point between the start of an operation and its commit. Exception-safety analysis becomes the responsibility of every author and every reviewer, with no mechanical enforcement beyond sanitizers that catch UB after the fact.

**Anti-pattern: exception as control flow**

```cpp
std::string lookup_user_name(Database& db, UserId id) {
    auto row = db.query_row("SELECT name FROM users WHERE id = ?", id);
    if (!row.has_value()) {
        // RISK: "not found" is an expected result, not an exceptional condition.
        // Callers must catch this to distinguish it from actual failures,
        // but nothing in the type signature tells them to.
        throw UserNotFoundException{id};
    }
    return row->get<std::string>("name");
}
```

The caller sees a function returning `std::string`. Nothing in the signature communicates that "not found" is a normal outcome. The only documentation is the exception specification (which C++ does not check at compile time) or a comment.

### 2.2.3 Logging as error handling

A third failure mode occurs when functions report errors by logging and then continuing or returning a default value.

```cpp
double convert_currency(double amount, std::string_view from, std::string_view to) {
    auto rate = rate_cache_.get(from, to);
    if (!rate) {
        // BUG: logging is not error handling. The caller receives 0.0
        // and has no way to distinguish "conversion unavailable"
        // from "the converted amount is zero."
        LOG(ERROR) << "Missing rate for " << from << " -> " << to;
        return 0.0;
    }
    return amount * (*rate);
}
```

The log line exists. The operational signal is lost in noise. The caller proceeds on a garbage value. The resulting ledger entry is wrong, and the postmortem reveals that the "error" was logged two thousand times per second for a week before anyone noticed.

## 2.3 Modern C++ Approach: Layered Failure Strategy

The core idea is that different kinds of failure deserve different mechanisms, and the boundaries where those mechanisms change must be explicit.

### 2.3.1 Classifying failures

Not all errors are alike. A useful taxonomy for production code:

| Category | Example | Mechanism |
|---|---|---|
| **Programming error** (precondition violation, logic bug) | Null pointer dereference, out-of-bounds index, violated invariant | Contract check / assertion / terminate. Not recoverable at runtime. |
| **Operational failure** (expected, recoverable) | Network timeout, missing cache entry, invalid user input, constraint violation | `std::expected`, result types, error codes with `[[nodiscard]]`. |
| **Resource exhaustion / system failure** | Out of memory, disk full, stack overflow | Exception or termination, depending on recovery architecture. |
| **Propagated failure** (crossing a subsystem boundary) | A batch processor reports that 3 of 200 items failed | Aggregate result type, not exceptions. |

The mistake is using one mechanism for all four categories. Exceptions are a reasonable default for resource exhaustion because the alternative — checking every allocation — is impractical and obscures business logic. But using exceptions for "user not found" forces callers into try/catch blocks for expected outcomes, and using assertions for network timeouts terminates a process that could have retried.

### 2.3.2 `std::expected` for operational failures

`std::expected<T, E>` (C++23, `<expected>`) holds either a value of type `T` or an error of type `E`. It makes the possibility of failure visible in the return type without exceptions.

```cpp
enum class LookupError {
    not_found,
    connection_lost,
    timeout,
};

std::expected<UserRecord, LookupError>
find_user(Database& db, UserId id) {
    auto conn = db.try_acquire_connection();
    if (!conn)
        return std::unexpected{LookupError::connection_lost};

    auto row = conn->query_row("SELECT * FROM users WHERE id = ?", id);
    if (!row)
        return std::unexpected{LookupError::not_found};

    return UserRecord::from_row(*row);
}
```

The caller sees that this function can fail and must handle the error variant. Ignoring the result is a compile-time error when `[[nodiscard]]` is applied (and `std::expected` is `[[nodiscard]]` by intent — apply it on the function if the implementation does not enforce it).

```cpp
void handle_request(Database& db, const Request& req) {
    auto user = find_user(db, req.user_id());
    if (!user) {
        switch (user.error()) {
            case LookupError::not_found:
                respond(req, 404, "User not found");
                return;
            case LookupError::connection_lost:
            case LookupError::timeout:
                respond(req, 503, "Service unavailable");
                return;
        }
    }
    // proceed with *user
    process_authorization(*user, req);
}
```

There is no try/catch. The error path is an ordinary branch. The type system ensures the caller acknowledges the possibility of failure before accessing the value.

### 2.3.3 Monadic operations on `std::expected`

C++23 provides `and_then`, `transform`, and `or_else` on `std::expected`, which allow chaining operations that each might fail without nested if-checks.

```cpp
struct AuthorizationResult {
    TransactionId txn_id;
    Amount settled_amount;
};

std::expected<AuthorizationResult, ServiceError>
authorize_payment(Services& svc, const PaymentRequest& req) {
    return svc.fraud_check(req)
        .and_then([&](FraudScore score) -> std::expected<Amount, ServiceError> {
            if (score.risk_level() > RiskLevel::high)
                return std::unexpected{ServiceError::fraud_rejected};
            return svc.calculate_settlement(req, score);
        })
        .and_then([&](Amount settled) -> std::expected<AuthorizationResult, ServiceError> {
            return svc.ledger_write(req, settled)
                .transform([&](TransactionId txn_id) {
                    return AuthorizationResult{txn_id, settled};
                });
        });
}
```

Each step either produces a value that feeds the next step, or short-circuits with the first error. The error type is uniform (`ServiceError`), which makes propagation mechanical. This is significantly more readable than nested try/catch blocks when three or four sequential operations each have distinct failure modes.

### 2.3.4 Preserving diagnostic context

A bare enum error code loses context. In production, you need to know *which* database query failed, against *which* host, at *what* timestamp. The standard library does not solve this, but the pattern is straightforward: use a structured error type as the `E` parameter.

```cpp
struct ServiceError {
    enum Code {
        connection_lost,
        timeout,
        constraint_violation,
        fraud_rejected,
        internal,
    };

    Code code;
    std::string message;
    std::source_location origin;
    std::chrono::system_clock::time_point when;

    static ServiceError make(Code c,
                             std::string msg,
                             std::source_location loc = std::source_location::current()) {
        return {c, std::move(msg), loc, std::chrono::system_clock::now()};
    }
};
```

`std::source_location` (C++20) captures file, line, and function without macros. The error object carries enough information to produce a useful log line at the boundary where the error is finally handled, without logging at every intermediate layer.

```cpp
std::expected<void, ServiceError>
write_ledger_entry(DbConnection& conn, const LedgerEntry& entry) {
    auto result = conn.execute(
        "INSERT INTO ledger (account, amount, ts) VALUES (?, ?, ?)",
        entry.account, entry.amount, entry.timestamp);
    if (!result)
        return std::unexpected{ServiceError::make(
            ServiceError::constraint_violation,
            std::format("ledger insert failed for account {}: {}",
                        entry.account, conn.last_error_message()))};
    return {};
}
```

### 2.3.5 When exceptions are still the right tool

Exceptions remain appropriate in specific situations:

1. **Constructors.** A constructor that cannot establish its invariant should throw. The alternative is a zombie object. If constructing the object means acquiring a resource that might be unavailable, throwing ensures the caller never holds an invalid handle.

2. **Deep library code where every caller would propagate the error unchanged.** If a low-level allocator or serialization library fails in a way that no immediate caller can handle, an exception lets the failure propagate to a boundary that can handle it, without every intermediate layer carrying `expected` plumbing.

3. **Operator overloads and generic code.** `operator[]`, `operator*`, and similar overloads cannot return `expected` without breaking their type contracts. Throwing (or terminating on precondition violation) is the only viable option.

The discipline is: **throw at the point where recovery is not the caller's responsibility, and catch at the boundary where recovery or reporting is.**

**Policy summary:** use `std::expected<T, E>` for recoverable operational failures that the caller must distinguish and act on. Use exceptions in constructors, in deep library code that would otherwise propagate the same failure mechanically through many frames, and at outer failure boundaries where unexpected failures are translated into logs, process termination, or transport-level errors. Do not mix `expected`, exceptions, and raw status codes arbitrarily at the same boundary.

### 2.3.6 Defining failure boundaries

A failure boundary is a point in the architecture where one error policy ends and another begins. In a service, this is typically:

- The request handler (where exceptions become HTTP status codes).
- The subsystem facade (where internal exceptions become `expected` values for the business logic layer).
- The thread boundary (where an exception in a worker task must not propagate to the thread pool infrastructure).

```cpp
// Request handler: the outermost failure boundary.
// Every exception becomes an HTTP response. No exception escapes.
void handle_authorization(HttpContext& ctx) noexcept {
    try {
        auto req = parse_request(ctx);          // may throw on malformed input
        auto result = authorize_payment(services_, req);  // returns expected
        if (!result) {
            log_error(result.error());
            ctx.respond(to_http_status(result.error().code), result.error().message);
            return;
        }
        ctx.respond(200, serialize(*result));
    } catch (const std::exception& e) {
        // Unexpected failure. Log, increment error counter, return 500.
        LOG(ERROR) << "Unhandled exception in authorization: " << e.what();
        ctx.respond(500, "Internal server error");
    } catch (...) {
        LOG(ERROR) << "Unknown exception in authorization handler";
        ctx.respond(500, "Internal server error");
    }
}
```

The structure is deliberate. `authorize_payment` uses `expected` for operational errors that the handler must distinguish between. The try/catch block exists only for genuinely unexpected failures. The handler is `noexcept` — no exception escapes to the framework.

This is the pattern: **`expected` inside the domain logic, exceptions only at construction and for truly exceptional conditions, `noexcept` catch-all at the outermost boundary.**

### 2.3.7 `std::expected<void, E>` for side-effecting operations

Operations that produce no value but can fail — writes, deletes, configuration reloads — use `std::expected<void, E>`. This is cleaner than returning a bool or an integer status, because the error type carries information.

```cpp
std::expected<void, ServiceError>
reload_configuration(ConfigStore& store, const std::filesystem::path& path) {
    auto contents = read_file(path);
    if (!contents)
        return std::unexpected{ServiceError::make(
            ServiceError::internal,
            std::format("failed to read config file: {}", path.string()))};

    auto parsed = parse_config(*contents);
    if (!parsed)
        return std::unexpected{parsed.error()};

    store.swap(*parsed);
    return {};
}
```

### 2.3.8 Error type design across subsystem boundaries

When a failure crosses a subsystem boundary, the receiving layer should not depend on the internal error vocabulary of the producing layer. This is the same principle as interface narrowing, applied to errors.

```cpp
// Internal to the fraud-scoring subsystem.
enum class FraudInternalError {
    model_load_failed,
    feature_extraction_error,
    scoring_timeout,
};

// Public interface of the fraud-scoring subsystem.
// Callers see ServiceError, not FraudInternalError.
std::expected<FraudScore, ServiceError>
FraudService::check(const PaymentRequest& req) {
    auto result = internal_score(req);  // returns expected<FraudScore, FraudInternalError>
    if (!result) {
        // Translate internal error to boundary error, preserving diagnostic context.
        return std::unexpected{ServiceError::make(
            ServiceError::internal,
            std::format("fraud scoring failed: {}", to_string(result.error())))};
    }
    return *result;
}
```

The internal error type can change without affecting callers. The boundary function is responsible for translation and for ensuring that diagnostic information is preserved in the `ServiceError::message` field.

## 2.4 Tradeoffs and Boundaries

### 2.4.1 `std::expected` is not free

`std::expected<T, E>` has the size of the larger of `T` and `E`, plus discriminant storage and alignment padding. For hot paths returning small values, this can matter. Measure before deciding it does not.

The monadic interface (`and_then`, `transform`) generates function objects at each step. Compilers inline aggressively, but deeply chained pipelines can produce large stack frames and complex debug info. Profile before chaining ten operations.

### 2.4.2 Exception overhead

On GCC and Clang (Itanium ABI), exceptions have near-zero cost on the non-throwing path, but the throwing path involves `__cxa_allocate_exception`, RTTI lookup, and stack unwinding. MSVC's exception model has different characteristics — stack-frame-based unwinding with a more predictable cost profile, but slower entry into `catch` blocks.

If a particular function is measured to throw in more than a few percent of calls, that is a signal to switch to `expected`. The threshold is workload-specific.

### 2.4.3 `noexcept` is a commitment, not an optimization hint

Marking a function `noexcept` tells the compiler and the reader that the function will not throw. If it does throw, the program calls `std::terminate`. This is a hard contract. Misapplying `noexcept` converts a recoverable failure into a crash.

Apply `noexcept` to:
- Move constructors and move assignment operators (enables optimizations in standard containers).
- Swap functions.
- Destructors (implicitly `noexcept` already).
- Boundary functions that must not leak exceptions (request handlers, thread entry points, C API callbacks).

Do not apply `noexcept` to functions that might plausibly need to throw in the future, or to functions whose callees might throw and where catching and converting would be artificial.

### 2.4.4 Mixed codebases

Real systems contain libraries that throw, C APIs that return error codes, and modern code that uses `expected`. The boundary functions between these worlds must be explicit.

```cpp
// Wrapping a C library that uses errno.
std::expected<FileHandle, ServiceError>
open_data_file(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::unexpected{ServiceError::make(
            ServiceError::internal,
            std::format("open() failed: {} (errno={})",
                        std::strerror(errno), errno))};
    }
    return FileHandle{fd};  // RAII wrapper takes ownership
}

// Wrapping a library that throws.
std::expected<Document, ServiceError>
parse_document(std::span<const std::byte> data) {
    try {
        return Document::parse(data);  // third-party, throws on malformed input
    } catch (const ParseException& e) {
        return std::unexpected{ServiceError::make(
            ServiceError::internal,
            std::format("document parse failed: {}", e.what()))};
    }
}
```

Each wrapper is a failure boundary. The internal mechanism (errno, exception) is translated to the project's standard error protocol. No exception leaks past the wrapper. No errno value propagates upward as a raw integer.

## 2.5 Testing and Tooling Implications

### 2.5.1 Testing error paths

`std::expected` makes error paths ordinary branches. They can be unit-tested without mock frameworks that inject exceptions:

```cpp
TEST(FindUser, ReturnsNotFoundForMissingUser) {
    FakeDatabase db;  // no user inserted
    auto result = find_user(db, UserId{42});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LookupError::not_found);
}

TEST(FindUser, ReturnsUserWhenPresent) {
    FakeDatabase db;
    db.insert_user(UserId{42}, "Alice");
    auto result = find_user(db, UserId{42});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->name, "Alice");
}
```

Testing exception-based error paths requires `EXPECT_THROW`, which tests that the function throws but makes it harder to test what state the function leaves behind. With `expected`, the returned error is a value you can inspect, compare, and serialize.

### 2.5.2 Static analysis

Clang-tidy checks relevant to error handling:

- `bugprone-unchecked-optional-access` — catches `.value()` on `std::optional` without a prior check. A similar pattern applies to `std::expected`.
- `bugprone-exception-escape` — flags functions marked `noexcept` that may throw.
- `misc-unused-return-value` — catches ignored `[[nodiscard]]` returns.

Apply `[[nodiscard]]` consistently on functions returning `expected`. The warning is your cheapest enforcement mechanism.

### 2.5.3 Sanitizers

AddressSanitizer and UndefinedBehaviorSanitizer do not directly catch ignored error codes, but they catch the consequences: use of uninitialized state, out-of-bounds access on a container that was never populated because an error was ignored. Running tests under ASan and UBSan closes some of the gap that static analysis misses.

### 2.5.4 Structured logging at boundaries

Log the error once, at the failure boundary where it is handled. Not at every layer it passes through. The `ServiceError` structure with `std::source_location` and timestamp gives the boundary handler enough information to produce a complete log line without intermediate logging:

```cpp
void log_error(const ServiceError& err) {
    LOG(ERROR) << std::format("[{}] {} at {}:{} ({})",
        to_string(err.code),
        err.message,
        err.origin.file_name(),
        err.origin.line(),
        err.when);
}
```

This eliminates the pattern of scattered `LOG(ERROR)` calls that produce duplicate, context-free noise.

## 2.6 Review Checklist

Use these questions during code review for any change that touches error handling.

- [ ] **Is the failure category clear?** Programming error (assert/terminate), operational failure (`expected`/error code), resource exhaustion (exception), or propagated aggregate failure (result collection).
- [ ] **Does the return type communicate failure?** Functions that can fail for operational reasons should return `std::expected<T, E>`, not throw, and not return sentinel values.
- [ ] **Is `[[nodiscard]]` applied?** Every function returning `expected`, `optional`, or a status code should be `[[nodiscard]]`.
- [ ] **Are failure boundaries explicit?** Identify where exceptions are caught and converted, where `expected` errors are translated, and where error policy changes between layers.
- [ ] **Is diagnostic context preserved?** The error type should carry enough information (message, origin, timestamp) to produce a useful log line at the handling boundary without logging at intermediate layers.
- [ ] **Is `noexcept` used correctly?** Applied to move operations, swap, destructors, and boundary functions. Not applied speculatively to functions whose callees might throw.
- [ ] **Are third-party and C library errors wrapped?** Exceptions from external libraries and errno-based errors are translated at the boundary into the project's error type. No raw errno or foreign exception type propagates into domain logic.
- [ ] **Are error paths tested?** Each error variant returned by a function has at least one test that exercises it and inspects the returned error value.
- [ ] **Is logging separated from error propagation?** Functions return errors to their callers rather than logging and continuing. Logging happens at the boundary where the error is finally handled.
- [ ] **Are exceptions reserved for exceptional conditions?** "Not found," "invalid input," and similar expected outcomes are not exceptions. Constructors that cannot establish their invariant may throw. Resource exhaustion may throw.
