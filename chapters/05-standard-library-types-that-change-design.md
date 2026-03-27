# Standard library types that change design

The standard library matters most when it stops being a utility layer and starts changing what your APIs are allowed to mean. In production C++, that shift happens with a small set of vocabulary types. They make borrowing explicit, separate absence from failure, represent closed sets of alternatives, and keep ownership from leaking through every function signature.

This chapter is not a tour of headers. The question is narrower and more useful: which standard types should change how you design ordinary code in a C++23 codebase, and where do those same types become misleading or expensive?

The pressure shows up at boundaries. A service parses bytes from the network, hands borrowed text into validation, constructs domain values, records partial failures, and emits results into storage or downstream services. If those steps are expressed with raw pointers, sentinel values, and container-heavy signatures, the code compiles but the contracts stay vague. Readers have to infer ownership, lifetime, nullability, and error meaning from implementation details. That is exactly the wrong place to hide them.

## Borrowing types change API shape

`std::string_view` and `std::span` are the most important everyday design types in modern C++ because they separate access from ownership. That sounds small. It is not. Once a codebase adopts borrowing types consistently, function signatures stop implying allocations they do not need and stop pretending to own data they merely inspect.

Consider a telemetry ingestion layer that parses line-based text records and binary attribute blobs:

```cpp
struct MetricRecord {
    std::string name;
    std::int64_t value;
    std::vector<std::byte> attributes;
};

auto parse_metric_line(std::string_view line,
                       std::span<const std::byte> attribute_bytes)
    -> std::expected<MetricRecord, ParseError>;
```

This signature says several important things immediately.

- The function borrows both inputs.
- The text input is not required to be null-terminated.
- The binary input is a contiguous read-only sequence.
- Ownership of the parsed result is transferred only in the return value.
- Failure is not the same thing as absence.

The older alternatives blur those statements. `const std::string&` suggests string ownership exists somewhere, even when callers are holding a slice into a larger buffer. `const std::vector<std::byte>&` excludes stack buffers, `std::array`, memory-mapped regions, and packet views for no good reason. `const char*` quietly reintroduces lifetime ambiguity and C-string assumptions.

To see the difference concretely, consider how the same boundary looked before borrowing types existed:

```cpp
// Pre-C++17: raw pointer + length, no type safety on the binary side
auto parse_metric_line(const char* line, std::size_t line_len,
                       const unsigned char* attr_bytes, std::size_t attr_len,
                       MetricRecord* out_record) -> int; // 0 = success, -1 = error

// Or the "safe" version that forces callers into specific containers
auto parse_metric_line(const std::string& line,
                       const std::vector<unsigned char>& attribute_bytes)
    -> MetricRecord; // throws on failure, no way to distinguish absence from error
```

The pointer-and-length version has no type system support for contiguity, read-only access, or even the fact that the binary buffer represents bytes rather than characters. Every caller must manually track two raw values per parameter, and a transposition bug (passing `attr_len` where `line_len` was expected) compiles silently. The container-reference version forces every caller to allocate a `std::string` and a `std::vector` even when the data already lives in a memory-mapped file or a stack buffer. Neither version communicates the ownership contract through the type system.

Borrowing types do impose discipline. A `std::string_view` is safe only while its source remains alive and unchanged. A `std::span` is safe only while the referenced storage remains valid. That is not a weakness of the types. It is the point. They force the boundary to say, in the type system, that this is a borrowing relationship and not an ownership transfer.

The failure mode is storing the borrow when the lifetime guarantee was local.

```cpp
class RequestContext {
public:
    void set_tenant(std::string_view tenant) {
        tenant_ = tenant; // BUG: borrowed view may outlive caller storage
    }

private:
    std::string_view tenant_;
};
```

This is not a reason to avoid `std::string_view`. It is a reason to use it only for parameters, local algorithm plumbing, and return types whose lifetime contract is obvious and reviewable. If the object needs to keep the data, store `std::string`. If a subsystem needs stable binary ownership, store a container or dedicated buffer type.

The example project demonstrates this borrow-then-own transition cleanly. In `examples/web-api/src/modules/task.cppm`, `Task::from_json` accepts a `string_view` to borrow the raw JSON body, but returns an `optional<Task>` whose `std::string` members own their data independently:

```cpp
[[nodiscard]] static std::optional<Task> from_json(std::string_view sv);
```

The function extracts field values from the borrowed input, moves them into owned strings, and returns a fully self-contained `Task`. The caller's buffer can be reused or destroyed immediately. This is the pattern described above: borrow for inspection, own for storage.

In practice, a good review question is simple: does this object merely inspect caller-owned data, or does it need to retain it? If the latter, a borrowing type in storage is already suspicious.

## `optional`, `expected`, and `variant` solve different problems

Production code gets expensive when teams use one vocabulary type as a universal answer. `std::optional`, `std::expected`, and `std::variant` all model different semantics. Choosing among them is a design decision, not a style preference.

Use `std::optional<T>` when the absence of a value is ordinary and not itself an error. A cache lookup may miss. A configuration override may be unset. An HTTP request may or may not include an idempotency key. If callers are expected to branch on presence without explanation, `optional` is the right signal.

Use `std::expected<T, E>` when failure information matters to control flow, logging, or user-visible behavior. Parsing, validation, protocol negotiation, and boundary I/O usually belong here. Returning `optional` from those operations throws away the reason the work failed and forces side channels for diagnostics.

Use `std::variant<A, B, ...>` when the result is one of several valid domain states, not a success-or-failure pair. A messaging system might model a command as one of several packet shapes. A scheduler might represent work as `std::variant<TimerTask, IoTask, ShutdownTask>`. That is not failure; it is an explicit closed set.

The mistake is treating these types as interchangeable wrappers around uncertainty.

- `optional` is for maybe-there.
- `expected` is for success-or-explanation.
- `variant` is for one-of-several-valid-forms.

The example project illustrates this distinction concretely. In `examples/web-api/src/modules/repository.cppm`, a lookup that may simply miss uses `optional`:

```cpp
[[nodiscard]] std::optional<Task> find_by_id(TaskId id) const;
```

There is no error to report — the task either exists or it does not. Returning `expected` here would force callers to inspect an error they cannot act on. `optional` is the right signal for ordinary absence.

Once you phrase them that way, many API debates end quickly.

### What designs looked like before these types

Before `std::optional`, the standard idiom for "maybe a value" was a sentinel or an out-parameter:

```cpp
// Sentinel: -1 means "not found." Every caller must know the convention.
int find_port(const Config& cfg); // returns -1 if unset

// Out-parameter: success indicated by bool return, value written through pointer.
bool find_port(const Config& cfg, int* out_port);

// Nullable pointer: caller must check for null, and ownership is ambiguous.
const Config* find_override(std::string_view key); // null means absent... or error?
```

Every one of these forces callers to remember an informal protocol. Sentinels like `-1` or `nullptr` are invisible in the type system; nothing prevents a caller from using the sentinel value in arithmetic. Out-parameters invert the data flow and make chaining awkward. With `std::optional<int>`, the type itself carries the "maybe absent" semantics and the compiler helps enforce the check.

Before `std::variant`, closed sets of alternatives were modeled with `union`, an enum discriminator, and manual discipline:

```cpp
// C-style tagged union: no automatic destruction, no compiler-checked exhaustiveness
enum class ValueKind { Integer, Float, String };

struct Value {
    ValueKind kind;
    union {
        std::int64_t as_int;
        double as_float;
        char as_string[64]; // fixed buffer, truncation risk
    };
};

void process(const Value& v) {
    switch (v.kind) {
    case ValueKind::Integer: /* ... */ break;
    case ValueKind::Float:   /* ... */ break;
    // Forgot String? Compiles fine. UB at runtime if String arrives.
    }
}
```

The `union` holds the data but the language provides no guarantee that `kind` and the active member stay in sync. Adding a new alternative requires updating every `switch` site manually, and the compiler is not required to warn about missing cases. `std::variant` makes the active alternative part of the type's runtime state, destructs the previous value correctly on reassignment, and `std::visit` provides a pattern where the compiler can warn if a case is missing.

Suppose a configuration loader may find no override, may parse a valid override, or may reject malformed input. Those are three semantically different outcomes. Cramming them into `optional<Config>` loses the reason malformed input was rejected. Returning `expected<optional<Config>, ConfigError>` may look slightly heavier, but it states the contract precisely: absence is normal, malformed input is failure.

The same precision matters across service boundaries. If an internal client library returns `variant<Response, RetryAfter, Redirect>`, callers can pattern-match on legitimate protocol outcomes. If it returns `expected<Response, Error>` instead, retry and redirect get misclassified as error paths even when they are part of the intended control flow.

The example project uses this approach at its domain boundary. In `examples/web-api/src/modules/error.cppm`, a project-wide alias makes the pattern consistent:

```cpp
template <typename T>
using Result = std::expected<T, Error>;
```

Then in `examples/web-api/src/modules/repository.cppm`, creation that can fail with a meaningful reason returns `Result<Task>`:

```cpp
[[nodiscard]] Result<Task> create(Task task);
```

If validation rejects the input, the caller receives an `Error` with a code and a human-readable detail string — not a bare `false` or an empty optional. The `create_task` handler in `examples/web-api/src/modules/handlers.cppm` translates that `Result` into an HTTP response at the boundary, without out-parameters or exception handling:

```cpp
auto result = repo.create(std::move(*parsed));
return result_to_response(result, 201);
```

`expected` also changes exception strategy. In codebases that use exceptions sparingly or forbid them across certain boundaries, `expected` lets failure stay local and explicit without collapsing into status codes and out-parameters. But there is a real tradeoff: plumbing `expected` through every private helper can turn straight-line code into repetitive propagation logic. Keep it at boundaries where the error information matters. Inside a tightly scoped implementation, a local exception boundary or a smaller helper decomposition may still produce cleaner code.

## Containers should not pretend to be contracts

One of the most persistent design mistakes in C++ code is using owning containers as parameter types when the function only needs a sequence. `std::vector<T>` in a signature is rarely a neutral choice. It says something about allocation strategy, contiguity, and caller representation. Sometimes that is intended. Often it is accidental.

If a function consumes a read-only sequence, accept `std::span<const T>`. If it needs a mutable view over contiguous caller storage, accept `std::span<T>`. If it needs ownership transfer, accept the owning type explicitly. If it needs a specific associative container because lookup complexity or key stability is part of the contract, say so directly.

That distinction is especially important in libraries. A compression library that exposes `compress(const std::vector<std::byte>&)` has silently told every caller how to store input buffers. A better boundary is almost always a borrowing range over bytes, often `std::span<const std::byte>`. The owning choice then stays with the caller: pooled buffer, memory-mapped file region, stack array, or vector.

The reciprocal mistake is returning views when the function is actually producing owned data. Returning `std::span<const Header>` from a parser that builds a local vector is wrong. Returning `std::vector<Header>` or an owning domain object is right. Borrowing types improve APIs when they describe reality. They make APIs worse when used to avoid a copy that the contract requires.

There is also a question of mutation. Passing a mutable container often exposes far more freedom than the algorithm needs. A function that only appends parsed records should not accept an entire mutable map if the real contract is output insertion. In those cases, consider a narrower abstraction: a callback sink, a dedicated appender type, or a constrained generic interface as discussed in the next chapter. Types should express what the callee is allowed to assume, not just what happens to compile.

## A real boundary: parsing without contract drift

A native service that ingests protobuf-like frames from a socket often has three distinct layers:

1. A transport layer that owns buffers and retries reads.
2. A parser that borrows bytes and validates framing.
3. A domain layer that owns normalized values.

The standard library types should reinforce those layers, not blur them.

The transport layer might expose owned frame storage because it must manage partial reads, capacity reuse, and backpressure. The parser should typically accept `std::span<const std::byte>` because it inspects caller-owned bytes and either produces a domain object or returns a parse error. The domain layer should return ordinary values, not spans into packet buffers, because business logic should not inherit transport lifetimes by accident.

That sounds obvious when written out. It becomes less obvious when a performance-minded refactor starts threading `string_view` and `span` deeper into the system “to avoid copies.” The copy is sometimes the cost of decoupling a stable domain object from a volatile transport buffer. Eliminating it may shift the cost into lifetime complexity, delayed parsing bugs, and review difficulty.

A useful rule is this: borrow at inspection boundaries, own at semantic boundaries. Parsing code often sits at the edge between them.

## Where these types hurt

Vocabulary types improve code only when the semantics stay crisp.

`std::string_view` hurts when developers treat it as a cheap string substitute rather than a borrow. `std::span` hurts when the code really needs noncontiguous traversal or stable ownership. `std::optional` hurts when it erases why work failed. `std::variant` hurts when the set of alternatives is open-ended or frequently extended across modules. `std::expected` hurts when used deep inside implementation code that would be clearer with a local exception boundary or a simpler helper split.

Another common failure is stacking wrappers until the API stops speaking human. Types such as `expected<optional<variant<...>>, Error>` are occasionally correct, but they are never cheap for readers. If a contract requires that much decoding, a named domain type is usually overdue.

The point of vocabulary types is not maximal precision at any syntactic cost. The point is to make the dominant semantics obvious enough that a reviewer can reason about ownership, absence, and failure without reverse-engineering the implementation.

## Verification and review

The verification burden here is mostly contractual.

- Review stored `string_view` and `span` members as potential lifetime bugs.
- Test parser and boundary APIs with short-lived buffers, sliced inputs, empty inputs, and malformed payloads.
- Check whether `optional` results are silently discarding errors that matter operationally.
- Audit container parameters for accidental ownership or representation commitments.
- Treat conversions from borrowed views to owned values as meaningful design points, not incidental implementation details.

Sanitizers help, especially when borrowed views cross asynchronous or deferred execution boundaries, but they do not replace API review. Many misuse patterns are logically wrong long before they become dynamically observable.

## Takeaways

- Prefer borrowing types for inspection boundaries and owning types for storage boundaries.
- Use `optional`, `expected`, and `variant` for three different meanings: absence, failure, and closed alternatives.
- Do not let containers leak representation choices into APIs unless that representation is part of the contract.
- A removed copy is not automatically a win if it pushes lifetime complexity into unrelated layers.
- When a vocabulary type stops making the contract easier to read, introduce a named domain type instead of stacking wrappers.