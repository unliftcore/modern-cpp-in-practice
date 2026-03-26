# Errors, Results, and Failure Boundaries

Most large C++ systems do not fail because they chose the wrong single error mechanism. They fail because several mechanisms are used at once with no layer policy. One subsystem throws exceptions, another returns status codes, a third logs and continues, and a fourth converts every failure into `false`. Individually, each choice may have looked reasonable. Together they produce code where callers cannot tell which operations may fail, which failures are recoverable, where diagnostics were emitted, or whether cleanup and rollback still ran.

The production question is not "exceptions or `std::expected`?" The production question is where each error model belongs, how failure information crosses boundaries, and which parts of the system are responsible for translation, logging, and crash decisions.

That boundary focus matters because error handling is architectural. A parsing layer, a domain layer, a storage adapter, and a process entry point all face different constraints. Conflating them is what makes code noisy and operationally fragile.

This chapter keeps the distinctions sharp. It does not try to outlaw exceptions or declare `std::expected` a universal replacement. It argues for a policy that preserves useful failure information without letting every low-level mechanism leak across the whole codebase.

## Start by Classifying Failure

Not every failure deserves the same transport.

At a minimum, production code should distinguish among these categories:

1. Invalid input or failed validation.
2. Environmental or boundary failure such as file IO, network errors, or storage timeouts.
3. Contract violation or impossible internal state.
4. Process-level startup or shutdown failure.

These categories influence both recovery and observability. Invalid input is often expected at system edges and should usually become a local error result with enough detail to reject the request or config cleanly. Environmental failure may need translation, retry policy, or escalation. Contract violation often means the program or subsystem has already lost a key invariant; that is closer to crash territory than to "return an error and continue." Startup failure is special because there may be no useful degraded mode at all. Failing fast can be the correct behavior.

Once these categories are explicit, API design gets easier. Not every function should expose every class of failure directly. A high-level domain function should not need to understand a vendor-specific SQL error enumeration if the only actionable outcomes are `not_found`, `conflict`, and `temporarily_unavailable`.

### The Error-Code-Only Approach and Its Pitfalls

Before `std::expected` and before exceptions were widely adopted, C++ codebases (and C codebases that C++ inherited) relied on integer error codes and sentinel return values. That approach is still common, and its failure modes are worth examining concretely.

```cpp
// Error-code-only style: caller must check, but nothing enforces it.
enum ConfigErrorCode { kOk = 0, kFileNotFound = 1, kParseError = 2, kInvalidValue = 3 };

ConfigErrorCode load_service_config(const std::string& path, ServiceConfig* out);

void startup() {
    ServiceConfig cfg;
    load_service_config("/etc/app/config.yaml", &cfg); // BUG: return code silently ignored

    // cfg may be uninitialized garbage -- the program continues anyway.
    listen(cfg.port); // binds to nonsense port or zero
}
```

The core problem is that error codes are advisory. The compiler does not require the caller to inspect them. Even with `[[nodiscard]]`, a cast to `void` or an accidental omission silences the warning. In large codebases, studies of C error-code APIs consistently find that 30-60% of error returns are never checked at some call sites.

The secondary problem is information loss. An integer code cannot carry structured context (which file failed to parse, which value was invalid, what the underlying OS error actually said). Callers who do check the code often log a generic message and discard the detail, producing diagnostics that are useless during incidents.

`std::expected` solves both problems. The caller must explicitly access either the value or the error; attempting to use the value when the result holds an error is a visible, reviewable decision (and undefined behavior if done carelessly, which sanitizers catch). The error type can carry structured diagnostics without side-channel logging.

The companion web-api project applies this approach throughout. In `error.cppm`, a single `Result<T>` alias makes the pattern consistent across the entire codebase:

```cpp
// examples/web-api/src/modules/error.cppm
template <typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error>
make_error(ErrorCode code, std::string detail) {
    return std::unexpected<Error>{Error{code, std::move(detail)}};
}
```

`Error` carries a typed `ErrorCode` enum and a human-readable detail string — structured enough for programmatic branching, descriptive enough for diagnostics. No integer codes are exposed to callers; every failure path goes through `Result<T>`.

## Exceptions Are Good for Unwinding and Local Clarity

Exceptions remain valuable in C++ because stack unwinding composes naturally with RAII. When a constructor fails halfway through a resource-owning object graph, exceptions let the language drive destruction without hand-written cleanup ladders. When a local implementation has several nested helper calls and any of them can fail in the same way, exceptions can keep the main path readable.

That does not make exceptions a universal boundary model.

Their strengths are real:

- they separate ordinary flow from failure flow,
- they preserve concise code through multiple call layers,
- and they pair well with RAII because cleanup stays automatic.

Their weaknesses are also real:

- they hide failure from the signature,
- they can cross boundaries that were never designed for them,
- and they invite sloppy layering when low-level exception types leak into high-level code.

The right conclusion is modest. Exceptions are often a good internal mechanism inside a layer. They are usually a poor language for broad subsystem boundaries unless the codebase has committed to that model consistently and can enforce it.

## `std::expected` Is Strong at Decision Boundaries

`std::expected<T, E>` is not better than exceptions in the abstract. It is better when the caller is expected to make a visible decision based on the failure.

Parsing, validation, boundary translation, and request-level operations often fall into this category. The call site usually needs to branch, emit a structured rejection, choose retry behavior, or attach context. Returning an `expected` makes that decision point explicit.

Consider a configuration loader:

```cpp
enum class ConfigErrorCode {
    file_not_found,
    parse_error,
    invalid_value,
};

struct ConfigError {
    ConfigErrorCode code;
    std::string message;
    std::string source;
};

auto load_service_config(std::filesystem::path path)
    -> std::expected<ServiceConfig, ConfigError>;
```

This contract tells the reader something important. Failure is part of normal control flow at this boundary. The caller must decide whether to abort startup, fall back to a default environment, or report a clear diagnostic. That is different from a deep internal helper whose only sensible failure policy is to unwind to the boundary that can actually choose.

Compare the `expected`-based loader with a traditional output-parameter-plus-bool approach to see how much information the old style loses:

```cpp
// Old style: bool return, output parameter, no structured error.
bool load_service_config(const std::filesystem::path& path,
                         ServiceConfig* out,
                         std::string* error_msg = nullptr);

void startup() {
    ServiceConfig cfg;
    std::string err;
    if (!load_service_config("/etc/app/config.yaml", &cfg, &err)) {
        // What kind of failure? File missing? Parse error? Permission denied?
        // err is a free-form string -- no programmatic branching possible.
        LOG_ERROR("config load failed: {}", err);
        std::exit(1); // only option: cannot distinguish retriable from fatal
    }
}
```

With `std::expected<ServiceConfig, ConfigError>`, the caller can branch on `ConfigErrorCode::file_not_found` versus `ConfigErrorCode::parse_error`, choose different recovery strategies, and still access a human-readable message for logging. The type system carries the decision-relevant information rather than burying it in a string.

The danger with `expected` is over-propagation. If every tiny helper returns `expected` merely because a public boundary does, the implementation can become littered with repetitive forwarding logic that obscures the main algorithm. Keep `expected` where the error belongs in the design. Do not force it through every private function unless that really improves local clarity.

## Anti-pattern: Side-Effect Errors With No Boundary Policy

One of the most common production failures is logging plus partial status plus occasional throwing, all in the same subsystem.

```cpp
// Anti-pattern: side effects and transport are mixed.
bool refresh_profile(Cache& cache, DbClient& db, UserId user_id) {
    try {
        auto row = db.fetch_profile(user_id);
        if (!row) {
            LOG_ERROR("profile not found for {}", user_id);
            return false;
        }

        cache.put(user_id, to_profile(*row));
        return true;
    } catch (const DbTimeout& e) {
        LOG_WARNING("db timeout: {}", e.what());
        throw; // RISK: some failures logged here, some rethrown, signature hides both
    }
}
```

This function is expensive to use because the caller does not know what `false` means, which failures were already logged, or whether it still needs to add context. If several layers follow this pattern, incidents become noisy and under-explained at the same time.

Boundary code should choose one transport and one logging policy. Either the function returns a structured failure and leaves logging to a higher layer that can attach request context, or it handles the failure completely and makes that clear in the contract. Mixing both is how duplicate logs and missing decisions enter a system.

## Anti-pattern: Silent Failures From Unchecked Returns

A subtler variant of the side-effect problem is code that converts failures into default values without any signal to the caller.

```cpp
// Anti-pattern: failure becomes a silent default.
int get_retry_limit(const Config& cfg) {
    auto val = cfg.get_int("retry_limit");
    if (!val) {
        return 3; // silent fallback -- no log, no metric, no trace
    }
    return *val;
}
```

This is seductive because the code never crashes. But when the configuration file has a typo (`retry_limt` instead of `retry_limit`), the system silently uses a hardcoded default. During an incident, operators change the configuration expecting behavior to update, and nothing happens. The bug is invisible precisely because the error was swallowed.

The better approach makes the default explicit and the fallback observable:

```cpp
auto get_retry_limit(const Config& cfg) -> std::uint32_t {
    constexpr std::uint32_t default_limit = 3;
    auto val = cfg.get_uint("retry_limit");
    if (!val) {
        LOG_INFO("retry_limit not configured, using default={}", default_limit);
        return default_limit;
    }
    return *val;
}
```

Or, if the caller should decide whether a missing value is acceptable, return the `expected` or `optional` directly and let the boundary make the policy choice.

## Translate Near Volatile Dependencies

Boundary translation is where most error design work should happen.

A storage adapter may receive driver exceptions, status codes, retry hints, or platform errors. The rest of the system usually does not want those details directly. It wants decision-relevant categories and maybe enough attached context for diagnostics.

That means translation should happen close to the unstable dependency, not in business logic three layers away.

```cpp
auto AccountRepository::load(AccountId id)
    -> std::expected<AccountSnapshot, AccountLoadError>
{
    try {
        auto row = client_.fetch_account(id);
        if (!row) {
            return std::unexpected(AccountLoadError::not_found(id));
        }
        return to_snapshot(*row);
    } catch (const DbTimeout& e) {
        return std::unexpected(AccountLoadError::temporarily_unavailable(
            id, e.what()));
    } catch (const DbProtocolError& e) {
        return std::unexpected(AccountLoadError::backend_fault(
            id, e.what()));
    }
}
```

This does not erase useful information. It packages it in a form the caller can act on. Business logic can now distinguish not-found from temporary unavailability without learning the storage client's failure taxonomy.

The same rule applies to network boundaries, filesystem boundaries, and third-party libraries. Translate once near the edge. Do not let raw backend errors leak until every layer has to understand them.

The companion web-api project shows the same pattern at the HTTP boundary. In `handlers.cppm`, `result_to_response()` translates a domain `Result<T>` into an HTTP response exactly once, at the edge:

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

Domain logic works exclusively with `Result<T>`. The translation to HTTP status codes and JSON error bodies happens in a single function at the handler boundary — no domain code imports HTTP concepts, and no handler code inspects error internals beyond calling the translation.

## Constructors, Destructors, and Startup Need Different Rules

Error policy should respect lifecycle context.

Constructors are often a good place to use exceptions because partial construction plus RAII is one of C++'s strongest combinations. A resource-owning object that cannot be made valid should usually refuse to exist. Returning a half-initialized object plus status is rarely an improvement.

Destructors are the opposite. Throwing across destruction is usually catastrophic or forbidden by the design. If cleanup can fail meaningfully, the type may need an explicit `close`, `flush`, or `commit` operation that reports failure while the object is still in a controlled state. The destructor then becomes best-effort cleanup or final safety net.

Startup is its own case. At process startup, configuration loading, dependency initialization, and binding to ports often have only one sensible failure policy: produce a clear diagnostic and fail the process. That is not the same as saying every startup helper should call `std::exit`. It means the top-level startup boundary should own the decision and the lower layers should return enough structured information to make that failure obvious and precise.

## Diagnostics Must Be Rich Without Being Contagious

Good error handling preserves context. Bad error handling spreads context-building code into every branch until the main behavior disappears.

Useful failure information often includes:

- a stable category or code,
- a human-readable message,
- key identifiers such as file path, tenant, shard, or request id,
- and sometimes captured backend detail or stacktrace data when it materially helps debugging.

The trick is to keep the error object meaningful without letting it become a dump of every internal detail. A domain-facing error type should expose what callers need to decide and what operators need to diagnose, not every low-level exception string encountered on the way.

This is one reason named error types matter. `expected<T, std::string>` is quick to write and weak as a system design. Strings are good final diagnostics and poor architectural contracts.

## Where to Log

The cleanest default is to log at boundaries that have enough context to make the event operationally useful.

That usually means request boundaries, background job supervisors, startup entry points, and outer retry loops. It usually does not mean every helper that notices failure. Logging too early strips context. Logging at every layer duplicates noise. Logging nowhere until the process dies loses evidence.

The core rule is simple: the layer that decides what the failure means operationally is usually the right place to log it.

That rule works well with `expected`-style boundaries and with exception translation. Lower layers preserve information. Boundary layers classify, add context, decide recovery, and emit the event once.

## Contract Violations Are Not Just Another Error Path

Some failures indicate that the program received bad input. Others indicate that the program broke its own assumptions.

If an invariant that should have been enforced earlier is now false, or a supposedly unreachable state is reached, pretending this is just another recoverable business error often hides a deeper bug. That does not always require immediate process termination, but it does require different treatment from routine validation failure.

A good codebase makes these distinctions explicit. Input failure is modeled as input failure. Backend unavailability is modeled as environmental failure. Internal invariant breakage is surfaced as a bug, not normalized into an ordinary "operation failed" code path.

## Verification and Review

Failure handling should be reviewed as a system property, not function by function in isolation.

Useful review questions:

1. Which failures are expected and decision-relevant at this boundary?
2. Are exceptions being used internally for clarity, or leaking unpredictably across layers?
3. Is `expected` carrying real decision information, or merely replacing exceptions with boilerplate?
4. Where is backend-specific failure translated into stable categories?
5. Is logging happening once, at the layer with enough context to make it useful?

Testing should include unhappy paths deliberately. Parse invalid input. Simulate timeouts and not-found cases. Verify translation from backend failures into domain-facing failures. Exercise startup failure paths and explicit close or commit operations. A codebase that only tests happy-path behavior will eventually discover its actual error model in production.

## Takeaways

- Choose error transport by layer and boundary, not by ideology.
- Use exceptions where unwinding and local clarity help, especially inside layers and during construction.
- Use `std::expected` where callers must make explicit decisions based on failure.
- Translate unstable backend errors near the dependency boundary into stable, decision-relevant categories.
- Log at the layer that understands the operational meaning of the failure.

If callers cannot tell what failed, whether it was already logged, and what they are expected to do next, the failure boundary is badly shaped. That is a design flaw long before it becomes an outage.