# Chapter 18: Legacy Migration, Interop, and Incremental Modernization

*Prerequisites: Parts I–III (the ownership, failure, and interface models that modernized code should target).*

Most teams do not begin with a clean C++23 codebase. They inherit old ownership models, macro-heavy interfaces, C compatibility constraints, partial platform support, and years of operational workarounds. This chapter focuses on modernization under constraint: how to introduce better boundaries, safer types, and stronger tooling without destabilizing shipping systems. The challenge is sequencing change so local improvements accumulate instead of producing a rewrite-shaped crater in the roadmap.

---

> **Prerequisites:** You should be comfortable with the ownership model from Chapter 1, the failure-boundary design from Chapter 2, and the interface principles from Chapter 5. This chapter assumes you understand why `unique_ptr` is preferable to a raw owning pointer, why `std::expected` or explicit result types improve failure propagation, and why narrow interfaces age better than wide ones. The goal here is not to re-derive those conclusions but to apply them under the constraints of existing, shipping code that was written before those patterns were available or adopted.
>
> Familiarity with C linkage (`extern "C"`), opaque pointer idioms, and basic build-system configuration (CMake or equivalent) is assumed. Chapter 6 covers ABI and module boundaries in depth; this chapter references those decisions without repeating them.

---

## 18.1 The Production Problem

The common framing is "we need to modernize." The actual problem is more specific: the codebase has regions where ownership is ambiguous, failure modes are invisible, interfaces are too wide, and tooling cannot reach. These regions resist change because they are load-bearing — they carry years of operational fixes, implicit contracts, and downstream dependencies that nobody mapped.

Three forces create the pressure:

**Defect density concentrates in unmodernized code.** Memory safety bugs, resource leaks, and undefined behavior cluster in code that uses raw owning pointers, manual `new`/`delete` pairs, C-style casts, and unchecked error codes. Sanitizers and static analysis flag these regions repeatedly, but the fixes are not mechanical — they require understanding the intended ownership model, which was never explicit.

**New features cannot compose with old interfaces.** A team that adopts `std::expected` for error handling still needs to call into a library that signals failure via `errno` and output parameters. A component using `unique_ptr` receives a raw pointer from a legacy factory and must decide who owns it. Every boundary between old and new code becomes a translation layer, and each translation layer is a place where bugs hide.

**Rewrites fail.** Large-scale rewrites of working systems have a well-documented failure mode: they take longer than estimated, introduce regressions in behavior that was correct but undocumented, and often get cancelled partway through, leaving the codebase in a worse state than before — half old, half new, with two sets of conventions and no clean boundary between them.

The alternative is incremental modernization: changing code in small, reviewable steps that each leave the system in a shippable state. This requires a strategy for sequencing changes, managing interop boundaries, and deciding what not to modernize.

---

## 18.2 The Naive Approach: Bottom-Up Rewrite

The instinct is to start at the bottom of the dependency graph — the lowest-level utility libraries — modernize them, then work upward. This fails for several reasons.

Low-level libraries have the most dependents. Changing their interfaces forces changes everywhere simultaneously. A utility that returns `char*` might be called from 400 sites. Changing it to return `std::string_view` is not a find-and-replace operation; each call site has different lifetime assumptions.

```cpp
// Anti-pattern: Big-bang interface change in a foundational library

// Old interface — stable for a decade, called everywhere
// RISK: changing this signature forces all 400+ call sites to update atomically
char* config_get_value(const char* section, const char* key);

// "Modernized" interface
std::string_view config_get_value(std::string_view section, std::string_view key);
// BUG: callers that stored the char* indefinitely now hold a view
// into a buffer whose lifetime they never had to reason about before
```

The second failure mode is losing operational behavior. Legacy code often contains implicit contracts: a function that returns `nullptr` on missing keys, callers that check for `nullptr` before proceeding, logging side effects that operations teams depend on. A rewrite that changes the return type also changes the failure signaling, and the new behavior may be correct in the abstract but wrong in the context of the system's actual error-handling topology.

The third failure mode is the motivation cliff. A months-long rewrite of utility code produces no user-visible improvement. The team loses patience, leadership loses confidence, and the project stalls at the worst possible moment — after the old code has been partially dismantled but before the new code is complete.

---

## 18.3 Modern Approach: Facade, Strangle, Harden

Incremental modernization follows three interlocking strategies, applied to different parts of the codebase depending on risk and value.

### 18.3.1 The Facade Strategy: New Interface Over Old Implementation

Instead of rewriting a component, wrap it in a new interface that expresses the ownership, failure, and lifetime contracts you want. The old implementation continues to run. The new interface is what new code calls.

```cpp
// legacy_config.h — untouched legacy code
extern "C" {
    // Ownership: caller must free() the returned string.
    // Returns NULL on missing key. Sets errno on parse failure.
    char* legacy_config_get(const char* section, const char* key);
    void legacy_config_free(char* value);
}

// config_facade.h — modern wrapper, new code targets this
#include <expected>
#include <string>
#include <system_error>

enum class ConfigError {
    missing_key,
    parse_failure,
    internal_error
};

[[nodiscard]]
std::expected<std::string, ConfigError>
config_get(std::string_view section, std::string_view key) {
    // Null-terminate for the C API. string_view is not guaranteed
    // null-terminated, so we must copy.
    std::string sec(section);
    std::string k(key);

    errno = 0;
    char* raw = legacy_config_get(sec.c_str(), k.c_str());

    if (!raw && errno != 0) {
        return std::unexpected(ConfigError::parse_failure);
    }
    if (!raw) {
        return std::unexpected(ConfigError::missing_key);
    }

    // Transfer ownership into std::string, then free the C allocation.
    std::string result(raw);
    legacy_config_free(raw);
    return result;
}
```

The facade does several things at once. It makes ownership explicit: the caller receives a `std::string` by value and owns it. It makes failure explicit: the return type forces callers to handle the error case. It isolates the C interop — the `string_view`-to-null-terminated conversion, the `errno` check, the `free` call — in one place instead of scattering it across every call site.

New code calls `config_get`. Old code continues to call `legacy_config_get`. Both work. Migration happens call site by call site, at whatever pace the team can sustain.

### 18.3.2 The Strangler Strategy: Route New Traffic Through New Code

When a component needs behavioral changes — not just interface cleanup — the strangler pattern applies. Build the new implementation alongside the old one. Route new callers (or a controlled subset of existing callers) to the new implementation. Remove the old implementation only after all traffic has migrated and the new one has proven stable under production load.

```cpp
// Connection pool: old implementation uses raw pointers, manual refcounting.
// New implementation uses shared ownership with proper shutdown coordination.

class ConnectionPool {
public:
    // Old path — still active for existing callers
    [[deprecated("Use acquire() returning unique_ptr instead")]]
    Connection* acquire_raw();
    void release_raw(Connection* conn);

    // New path — ownership is explicit, release is automatic
    struct ConnectionDeleter {
        ConnectionPool* pool;
        void operator()(Connection* c) const noexcept {
            if (pool) pool->return_to_pool(c);
        }
    };
    using PooledConnection = std::unique_ptr<Connection, ConnectionDeleter>;

    [[nodiscard]] std::expected<PooledConnection, PoolError> acquire();

private:
    void return_to_pool(Connection* c) noexcept;
};
```

The `[[deprecated]]` attribute produces a compiler warning at every remaining call site for the old interface. This makes migration progress visible in build logs and CI dashboards. The old path remains functional — no runtime behavior changes — but the team has a mechanical way to track how many call sites still need migration.

### 18.3.3 The Hardening Strategy: Make Old Code Safer Without Changing Its Interface

Not everything can be wrapped or replaced. Some code is too deeply embedded, too performance-sensitive, or too risky to restructure. For those regions, the goal is hardening: adding runtime checks, assertions, sanitizer annotations, and static analysis suppressions that reduce defect risk without altering the interface or behavior.

```cpp
// Legacy buffer management — cannot change the interface, but can harden it.

void legacy_process_buffer(char* buf, size_t len, size_t capacity) {
    // Hardening: precondition checks that were previously implicit
    assert(buf != nullptr && "legacy_process_buffer: null buffer");
    assert(len <= capacity && "legacy_process_buffer: length exceeds capacity");

    // Hardening: sanitizer annotation so ASan can detect out-of-bounds
    // access inside this function's callees
    #if defined(__has_feature)
    #if __has_feature(address_sanitizer)
        __sanitizer_annotate_contiguous_container(
            buf, buf + capacity, buf + capacity, buf + len);
    #endif
    #endif

    // ... original implementation unchanged ...
}
```

Hardening is low-risk and high-value. It does not change behavior for correct callers. It makes incorrect usage detectable earlier — at development time under sanitizers, or at runtime via assertions in debug builds. It also documents the implicit contract: the `assert` statements are a machine-readable specification of preconditions that previously existed only in the original author's memory.

---

## 18.4 Tradeoffs and Boundaries

### 18.4.1 What Not to Modernize

Modernization has a cost. Every facade is a new abstraction layer. Every strangler migration requires maintaining two implementations in parallel. Every hardening annotation is code that must be reviewed and maintained.

Some code should be left alone:

- **Stable, correct, rarely-touched code.** If a component has no open bugs, no active development, and no interface problems, the return on modernization is near zero. The risk of introducing regressions during migration is real; the benefit is aesthetic.
- **Code scheduled for removal.** If a subsystem is being replaced by an external dependency or a different service, modernizing it is waste.
- **Code where the C interface is the product.** Libraries that expose a C API for cross-language interop cannot simply switch to `std::expected` and `string_view`. The C boundary is a feature, not a defect. Modernization applies to the implementation behind that boundary, not the boundary itself.

### 18.4.2 The Interop Tax

Every boundary between old and new code has a cost. The facade in Section 3.1 copies a `string_view` into a `std::string` to null-terminate it, then copies the C string into another `std::string` for the return value. In a tight loop, those allocations matter. In a configuration-loading path called once at startup, they do not.

Be explicit about where the interop tax is acceptable. Performance-critical paths may need a thinner wrapper — or no wrapper at all, with modernization deferred until the underlying implementation can be replaced entirely.

```cpp
// Anti-pattern: Wrapping a hot-path C function with unnecessary copies

// RISK: this wrapper allocates on every call in a per-packet processing path
std::string packet_get_field(const Packet& pkt, std::string_view field_name) {
    std::string name(field_name);  // allocation to null-terminate
    const char* val = legacy_packet_field(pkt.raw(), name.c_str());
    return val ? std::string(val) : std::string{};  // allocation for return
    // BUG: two allocations per call in a path handling 1M packets/sec
}

// Better: thin wrapper that preserves the zero-copy contract
std::optional<std::string_view>
packet_get_field_view(const Packet& pkt, const char* field_name) {
    const char* val = legacy_packet_field(pkt.raw(), field_name);
    if (!val) return std::nullopt;
    return std::string_view(val);
    // Caller must respect the lifetime of pkt — documented in the header.
}
```

The second version preserves the zero-copy property of the C API. The tradeoff is that the caller must reason about the lifetime of the returned `string_view`. This is acceptable in a performance-critical path where the callers are few and expert. It is not acceptable in a widely-used utility where most callers would mishandle the lifetime.

### 18.4.3 Macro Elimination

Legacy C++ codebases often rely heavily on macros for configuration, logging, platform abstraction, and code generation. Eliminating macros is worthwhile when they obscure control flow, defeat tooling (debuggers, IDEs, static analyzers cannot see through macros), or create subtle hygiene bugs.

The replacement depends on what the macro does:

| Macro purpose | Modern replacement |
|---|---|
| Constants | `constexpr` variables or `enum class` |
| Type-parameterized code | Function templates or `constexpr` functions |
| Conditional compilation | `if constexpr`, `requires` clauses, or build-system configuration |
| Logging boilerplate | Variadic templates with `std::source_location` |
| X-macros for enum-string mapping | `constexpr` arrays or code generation |

```cpp
// Legacy: X-macro for enum definition and string mapping
#define STATUS_LIST \
    X(ok)          \
    X(timeout)     \
    X(refused)     \
    X(internal)

// Generates enum values
enum Status {
    #define X(name) status_##name,
    STATUS_LIST
    #undef X
    status_count
};

// Generates string table
const char* status_strings[] = {
    #define X(name) #name,
    STATUS_LIST
    #undef X
};

// Modern: constexpr array, no macros, same outcome
enum class Status : std::uint8_t {
    ok, timeout, refused, internal
};

inline constexpr std::array<std::string_view, 4> status_strings = {
    "ok", "timeout", "refused", "internal"
};

constexpr std::string_view to_string(Status s) {
    auto i = std::to_underlying(s);
    if (i < status_strings.size()) return status_strings[i];
    return "unknown";
}
```

The modern version is debuggable, visible to static analysis, and produces clear compiler errors when the enum and string table fall out of sync (assuming a test or `static_assert` on the array size). The X-macro version is invisible to every tool except the preprocessor.

However, not all macros are worth replacing. Platform-detection macros (`#ifdef _WIN32`) have no superior alternative in standard C++23. Header include guards are universally understood. Replace macros that damage tooling and code clarity; leave macros that serve a purpose no language feature can replicate.

### 18.4.4 Managing `extern "C"` Boundaries

C interop is not a legacy problem to be solved — it is a permanent feature of systems programming. Many production C++ codebases must expose or consume C interfaces for FFI with other languages, OS APIs, plugin systems, or embedded environments.

The key discipline is to keep the C boundary thin and to avoid letting C idioms leak into the C++ implementation.

```cpp
// Public C API — stable, minimal, no C++ types exposed
extern "C" {

typedef struct Session Session;  // opaque pointer

Session* session_create(const char* config_path);
int session_execute(Session* s, const char* query, char** result, size_t* len);
void session_result_free(char* result);
void session_destroy(Session* s);

}  // extern "C"

// Internal C++ implementation — full modern C++ behind the boundary
struct SessionImpl {
    std::unique_ptr<ConfigStore> config;
    ConnectionPool pool;
    // ...
};

// The C functions are thin translation layers
extern "C" Session* session_create(const char* config_path) {
    try {
        auto impl = std::make_unique<SessionImpl>();
        impl->config = load_config(config_path);
        impl->pool = ConnectionPool(impl->config->pool_settings());
        return reinterpret_cast<Session*>(impl.release());
    } catch (...) {
        // C API cannot propagate exceptions. Log and return null.
        log_current_exception();
        return nullptr;
    }
}

extern "C" void session_destroy(Session* s) {
    delete reinterpret_cast<SessionImpl*>(s);
}
```

The pattern is: opaque typedef in the C header, `reinterpret_cast` between the opaque type and the real implementation at the boundary, exception-to-error-code translation at every entry point, and no C++ types in the C-visible interface. This is well-understood and works across all major compilers and platforms. Chapter 6 covers the ABI implications in detail.

---

## 18.5 Testing and Tooling Implications

### 18.5.1 Migration Creates Temporary Duplication

During a strangler migration, both the old and new implementations exist. Both must be tested. The natural approach is to extract the behavioral specification into a shared test suite that runs against both implementations.

```cpp
// Parameterized test that verifies both old and new pool implementations
template <typename PoolFactory>
class PoolBehaviorTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool = PoolFactory::create(test_config());
    }
    std::unique_ptr<AbstractPool> pool;
};

struct LegacyPoolFactory {
    static std::unique_ptr<AbstractPool> create(const Config& c) {
        return std::make_unique<LegacyPoolAdapter>(c);
    }
};

struct ModernPoolFactory {
    static std::unique_ptr<AbstractPool> create(const Config& c) {
        return std::make_unique<ModernPool>(c);
    }
};

using PoolImpls = ::testing::Types<LegacyPoolFactory, ModernPoolFactory>;
TYPED_TEST_SUITE(PoolBehaviorTest, PoolImpls);

TYPED_TEST(PoolBehaviorTest, AcquireReturnsValidConnection) {
    auto conn = this->pool->acquire();
    ASSERT_TRUE(conn.has_value());
    EXPECT_TRUE(conn->is_valid());
}

TYPED_TEST(PoolBehaviorTest, ExhaustedPoolReportsError) {
    // Exhaust the pool
    std::vector<AbstractPool::Handle> held;
    while (auto c = this->pool->acquire()) {
        held.push_back(std::move(*c));
    }
    auto result = this->pool->acquire();
    EXPECT_FALSE(result.has_value());
}
```

This approach catches behavioral divergence between old and new implementations before it reaches production. When the old implementation is eventually removed, the test suite continues to verify the new one.

### 18.5.2 Sanitizers and Legacy Code

Address Sanitizer (ASan), Undefined Behavior Sanitizer (UBSan), and Thread Sanitizer (TSan) are most valuable in legacy code — that is where the bugs are. But legacy code also produces the most sanitizer noise: benign violations that were never fixed, platform-specific constructs that sanitizers do not understand, and third-party code that cannot be modified.

Use suppression files to manage noise without disabling sanitizers globally:

```
# asan_suppressions.txt
# Third-party XML parser — known false positive on custom allocator
leak:third_party/expat/xmlparse.c

# Legacy protocol handler — scheduled for replacement in Q3
leak:src/legacy/protocol_v1.cpp
```

The suppression file is a living document. Each entry should have a comment explaining why it exists and when it can be removed. Treat suppression additions as code changes that require review. The goal is to run sanitizers on every CI build so that new code never introduces the classes of bugs that legacy code still contains.

### 18.5.3 Static Analysis as a Migration Radar

Clang-tidy, with a curated set of checks, serves as a migration progress tracker. Enable modernization checks incrementally:

```yaml
# .clang-tidy — phased enablement
Checks: >
  -*,
  modernize-use-override,
  modernize-use-nullptr,
  modernize-use-auto,
  modernize-deprecated-headers,
  cppcoreguidelines-owning-memory,
  bugprone-use-after-move,
  misc-unused-using-decls

# Only enforce on new/modified code — use NOLINT sparingly
WarningsAsErrors: >
  modernize-use-override,
  modernize-use-nullptr
```

Start with checks that are mechanical and low-risk (`use-override`, `use-nullptr`). These can be auto-fixed across the codebase in a single commit with `clang-tidy --fix`. Progress toward checks that require judgment (`owning-memory`, `use-after-move`) as the team builds confidence.

### 18.5.4 Compiler Warnings as a Ratchet

Enable warnings incrementally and never allow them to regress. The practical mechanism is `-Werror` on a growing subset of warnings, enforced in CI:

```cmake
# CMakeLists.txt — warning ratchet
target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type        # always fatal: missing return is UB
    -Werror=uninitialized      # always fatal: reading uninit is UB
    -Werror=deprecated-declarations  # enforces migration from deprecated APIs
)
```

`-Werror=deprecated-declarations` is particularly useful during strangler migrations: once the old interface is marked `[[deprecated]]`, any new code that calls it will fail the build.

---

## 18.6 Sequencing a Migration

The order in which you modernize matters more than the techniques you use. A practical sequencing:

**Phase 1: Harden.** Add assertions, sanitizer annotations, and compiler warnings to legacy code. No interface changes, no behavior changes. This phase is low-risk and produces immediate safety improvements. It also reveals the actual shape of the codebase's problems — where ownership is ambiguous, where error handling is missing, where undefined behavior lurks.

**Phase 2: Wrap leaf dependencies.** Build facades around the lowest-risk, most-frequently-called legacy interfaces. Configuration loading, logging, string utilities, time handling. These tend to have simple contracts and many callers, so the modernization ROI is high and the behavioral risk is low.

**Phase 3: Introduce modern interfaces for new features.** New code should target the modern interfaces from Phase 2. This creates organic migration pressure: developers working on new features call the facade, not the legacy API. The legacy API's call count decreases over time without a dedicated migration effort.

**Phase 4: Strangle high-value components.** Connection pools, request handlers, serialization layers — components where ownership bugs, resource leaks, or failure-handling gaps are causing real incidents. These justify the cost of dual-implementation because the operational benefit is concrete and measurable.

**Phase 5: Remove dead code.** After traffic has migrated, delete the old implementations, the compatibility shims, and the suppression-file entries. This is often the most neglected step. Dead code that is not removed will attract new callers and complicate future changes.

Each phase should produce a shippable system. If the migration stalls at any point, the codebase is in a better state than where it started.

---

## 18.7 Review Checklist

**Migration strategy**

- [ ] Is there a documented plan for which components to modernize and in what order?
- [ ] Does the plan prioritize by defect density and operational risk, not by aesthetic preference?
- [ ] Is every phase independently shippable?

**Facade and interop boundaries**

- [ ] Do facades make ownership transfer explicit (value return, `unique_ptr`, or documented borrowing)?
- [ ] Do facades translate legacy error signaling (`errno`, null returns, output parameters) into the project's modern error type?
- [ ] Is the interop cost (copies, allocations, null-termination) documented and acceptable for the call site's performance requirements?
- [ ] Are `extern "C"` boundaries thin, with no C++ types in the C-visible interface?
- [ ] Do C boundary functions catch all exceptions and translate them to error codes?

**Strangler migrations**

- [ ] Are deprecated interfaces marked with `[[deprecated]]` and enforced in CI via `-Werror=deprecated-declarations`?
- [ ] Do both old and new implementations share a behavioral test suite?
- [ ] Is there a mechanism (metrics, build warnings, or call-site counts) to track migration progress?

**Hardening**

- [ ] Do legacy functions have precondition assertions for their implicit contracts?
- [ ] Are sanitizer suppressions documented with rationale and expected removal date?
- [ ] Are sanitizers running on every CI build, including against legacy code?

**Macro elimination**

- [ ] Have macros that obscure control flow or defeat tooling been replaced with `constexpr`, templates, or `if constexpr`?
- [ ] Are remaining macros limited to platform detection, include guards, and cases with no language-level alternative?

**Sequencing and risk**

- [ ] Is stable, correct, rarely-touched code explicitly excluded from modernization?
- [ ] Is code scheduled for removal explicitly excluded from modernization?
- [ ] Are new features written against modern interfaces, creating organic migration pressure?
- [ ] Is dead code being removed after migration, not left in place?
