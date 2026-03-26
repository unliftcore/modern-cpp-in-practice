# Building a Reusable Library in Modern C++

Application code can get away with local convenience for a surprisingly long time. Library code cannot. Once an API escapes its original caller set, every vague ownership contract, every overloaded error channel, every accidental allocation, and every unstable type dependency becomes somebody else's problem. The library may still compile. It becomes expensive to trust.

The production question for this chapter is therefore not "how do I write a nice API?" It is "what does a reusable modern C++23 library have to make explicit so that other teams can adopt it without inheriting hidden coupling, unstable behavior, or unverifiable claims?" The answer starts with scope. A good reusable library makes a narrow promise, expresses that promise in types and contracts that survive real use, and refuses to leak its implementation economics into the whole dependency graph.

The sample shape here is a parsing and transformation library used by several services and command-line tools. That is a useful domain because it brings together most of the hard pressures: input boundaries, allocation behavior, diagnostics, performance expectations, extensibility, and packaging.

## Start by Choosing a Narrow Promise

Many bad libraries fail before the first line of public API. They are designed as "the shared place for everything related to X." That usually means they accumulate multiple responsibilities, grow extension points for unrelated concerns, and become difficult to version because every change touches somebody.

A reusable library should instead make one narrow promise. Parse and validate this format. Normalize these records. Expose this storage abstraction. Compute these derived values. The promise can be substantial. It should still have a clear center.

This sounds obvious, but it changes interface design immediately. A narrow promise leads to a small vocabulary of public types, a small set of failure categories, and a smaller number of places where callers need customization. A vague promise pushes complexity outward into templates, callback forests, configuration maps, and documentation that reads like negotiated truce.

The most important library design decision is therefore not whether to use modules, concepts, or `std::expected`. It is where the public contract stops.

## Public Types Should Encode Ownership and Invariants Directly

Callers should not need repository context to answer basic questions about ownership, lifetime, mutability, or validity. If the library returns a view, the caller should know what owns the underlying data. If it accepts a callback, the callback lifetime and threading expectations should be obvious. If a configuration object may be invalid, the invalid state should be representable only long enough to validate it.

Value types are often the right center of gravity for library APIs because they travel well across teams and tests. They make copying cost visible, move semantics deliberate, and invariants attachable to construction or validation boundaries. Borrowed inputs such as `std::string_view` and `std::span` are still excellent for call boundaries, but they should be used only when the library can complete its work within the borrowed lifetime or copy what it must retain.

### Intentional partial: a caller-facing API with explicit contracts

```cpp
enum class parse_error {
	invalid_syntax,
	unsupported_version,
	duplicate_key,
	resource_limit_exceeded,
};

struct parse_options {
	std::size_t max_document_bytes = 1 << 20;
	bool allow_comments = false;
};

struct document {
	std::pmr::vector<entry> entries;
};

[[nodiscard]] auto parse_document(std::string_view input,
								  parse_options const& options,
								  std::pmr::memory_resource& memory)
	-> std::expected<document, parse_error>;
```

This excerpt does a few useful things. It separates caller-controlled policy from input bytes. It makes allocation strategy visible without forcing a global allocator policy. It returns a domain error rather than a transport-specific or parser-internal type. It also makes result checking harder to forget through `[[nodiscard]]` and `std::expected`.

The tradeoff is that the function signature is less minimal than `document parse(std::string_view)`. That is fine. Reusable libraries do not get paid for looking compact in slides. They get paid for making costs and contracts legible.

## Make Failure Shape Stable

Application code can sometimes afford to let exceptions and internal error categories drift because one team controls both sides. Libraries should be much stricter. The caller must know which failures are part of the contract, which are programming errors, and which remain implementation accidents.

That usually leads to one of three designs.

1. Use `std::expected` or a similar result type for routine domain failures the caller is expected to handle.
2. Reserve exceptions for invariant violations, exceptional environment failures, or APIs whose ecosystem already assumes exception-based use.
3. Translate low-level errors into a stable public error vocabulary at the boundary.

The right choice depends on the domain. Parsing, validation, and recoverable business-rule failures usually fit `std::expected` well. Low-level infrastructure libraries integrated into exception-based application frameworks may reasonably use exceptions. What matters most is consistency. A library that returns `std::expected` for some recoverable failures, throws for others, and leaks `std::system_error` from one backend but not another is forcing callers to reverse-engineer policy from implementation.

Do not make the public error surface too granular. Callers need distinctions they can act on. They rarely benefit from twenty parser-internal states that only the library can interpret. Keep the stable categories small, then provide optional richer diagnostics through separate channels.

## Separate Mechanism from Policy Without Abstracting Everything

Reusable libraries often need some customization: allocation, logging hooks, host-owned diagnostics, clock sources, I/O adapters, or user-defined handlers. This is where teams either over-template the entire surface or bury the library inside runtime-polymorphic interfaces that allocate and dispatch everywhere.

The better approach is to choose a small number of explicit policy seams and keep the core mechanism concrete. Concepts help when the customization must stay zero-overhead and compile-time checked. Type erasure or callback interfaces help when binary boundaries, plugin models, or operational decoupling matter more than template transparency.

For example, an internal parsing engine probably does not need to be a giant policy-based template on logging, allocation, diagnostics, and error formatting simultaneously. It can parse concretely, accept a `std::pmr::memory_resource&`, and optionally emit diagnostics through a narrow sink interface. That keeps most call sites simple while still allowing hosts to control the expensive or environment-specific parts.

This is also where library authors need discipline about dependencies. If the public headers include a networking stack, formatting library, metrics SDK, and filesystem abstraction just to support optional features, the library has already lost portability and build hygiene. Optional operational concerns belong behind narrow seams or in companion adapters, not in the center of the API.

### Mistake: exposing internal types in public headers

One of the most common library design failures is leaking implementation types into the public API surface. This creates hidden coupling: callers transitively depend on headers they never asked for, build times grow, and internal refactors become breaking changes.

```cpp
// BAD: public header pulls in implementation details
#pragma once
#include <boost/asio/io_context.hpp>      // transport detail
#include <spdlog/spdlog.h>                // logging detail
#include "internal/parser_state_machine.h" // implementation detail

class document_parser {
public:
    document_parser(boost::asio::io_context& io,
                    std::shared_ptr<spdlog::logger> log);

    auto parse(std::string_view input) -> document;

private:
    boost::asio::io_context& io_;          // caller now depends on Boost.Asio
    std::shared_ptr<spdlog::logger> log_;  // caller now depends on spdlog
    internal::parser_state_machine fsm_;   // caller now depends on internal layout
};
// Every caller's translation unit now includes Boost.Asio and spdlog headers.
// Changing the logging library is a breaking change for all consumers.
```

The fix is to keep the public header minimal and push implementation types behind forward declarations, PIMPL, or narrow callback interfaces.

```cpp
// BETTER: public header exposes only the library's own vocabulary
#pragma once
#include <string_view>
#include <expected>
#include <memory>

namespace mylib {

enum class parse_error { invalid_syntax, resource_limit_exceeded };

struct diagnostic_event {
    std::string_view message;
    std::size_t line;
};

using diagnostic_sink = std::function<void(diagnostic_event const&)>;

class document_parser {
public:
    struct options {
        std::size_t max_bytes = 1 << 20;
        diagnostic_sink on_diagnostic = {};  // optional, no spdlog dependency
    };

    explicit document_parser(options opts = {});
    ~document_parser();
    document_parser(document_parser&&) noexcept;
    document_parser& operator=(document_parser&&) noexcept;

    [[nodiscard]] auto parse(std::string_view input)
        -> std::expected<document, parse_error>;

private:
    struct impl;
    std::unique_ptr<impl> impl_;  // Boost, spdlog, FSM all hidden here
};

} // namespace mylib
// Callers include only standard headers. Internal deps are invisible.
// Changing from spdlog to another logger requires zero caller changes.
```

### Mistake: poor error reporting

Libraries that report errors as raw integers, bare `std::string` messages, or platform-specific exception types force callers to reverse-engineer failure semantics from implementation details. The result is fragile error handling that breaks whenever the library changes its internals.

```cpp
// BAD: error reporting through mixed, unstable channels
auto parse(std::string_view input) -> document {
    if (input.empty())
        throw std::runtime_error("empty input");  // string-based
    if (input.size() > max_size)
        return {};  // default-constructed "null" document — is this an error?
    if (!validate_header(input))
        throw parser_exception(ERR_INVALID_HEADER);  // internal enum leaked
    // caller must catch two exception types AND check for empty documents
}
```

```cpp
// BETTER: single, stable error channel with actionable categories
[[nodiscard]] auto parse(std::string_view input)
    -> std::expected<document, parse_error>
{
    if (input.empty())
        return std::unexpected(parse_error::invalid_syntax);
    if (input.size() > max_size)
        return std::unexpected(parse_error::resource_limit_exceeded);
    if (!validate_header(input))
        return std::unexpected(parse_error::invalid_syntax);
    // one return type, one error vocabulary, no exceptions for routine failures
}
```

For richer diagnostics beyond the category, provide a separate channel (a diagnostic sink, an error details accessor, or a structured log) rather than overloading the primary error type with implementation-specific fields that callers cannot act on programmatically.

## Versioning and ABI Need a Policy, Not Optimism

Even an internal shared library benefits from treating versioning as part of design rather than release paperwork. The practical question is what kinds of change the library promises callers they can survive. Source compatibility, ABI compatibility, wire-format stability, serialized-data stability, and semantic compatibility are related but different promises.

For many C++ libraries, the easiest honest policy is source compatibility within a major version and no blanket ABI promise across arbitrary toolchains. That is often a stronger real-world posture than pretending ABI stability while exposing standard-library types, inline-heavy templates, or platform-dependent layout in the public surface.

If ABI stability does matter, the design must change accordingly. That usually means narrower exported surfaces, opaque types, PIMPL-like boundaries, stricter exception policy, reduced template exposure, and controlled compiler and standard-library assumptions. Those are not finishing touches. They affect the entire API shape.

### Mistake: breaking ABI with inline changes

A change that appears safe at the source level can break binary compatibility silently. Adding a member to a class, changing a default parameter value in an inline function, or reordering fields all change the ABI without any compiler diagnostic.

```cpp
// v1.0 — shipped as shared library
struct document {
    std::pmr::vector<entry> entries;
    // sizeof(document) == N, known to callers at compile time
};

// v1.1 — "just added a field"
struct document {
    std::pmr::vector<entry> entries;
    std::optional<metadata> meta;  // sizeof(document) changed
    // callers compiled against v1.0 still assume size N
    // stack allocations, memcpy, placement new — all wrong
};
```

The fix for ABI-stable libraries is to hide layout behind a PIMPL boundary, so that callers never depend on `sizeof` or field offsets.

```cpp
// ABI-stable public header
class document {
public:
    document();
    ~document();
    document(document&&) noexcept;
    document& operator=(document&&) noexcept;

    [[nodiscard]] auto entries() const -> std::span<entry const>;
    [[nodiscard]] auto metadata() const -> std::optional<metadata_view>;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// In the .cpp file (not visible to callers):
struct document::impl {
    std::pmr::vector<entry> entries;
    std::optional<metadata> meta;
    // add fields freely — callers see only the pointer
};
```

PIMPL adds one heap allocation per object and one indirection per access. For types created infrequently (documents, connections, sessions), this is nearly always an acceptable cost. For types created millions of times per second in a hot loop, it is not, and ABI stability for those types should be reconsidered.

### Versioning pattern: inline namespaces for source versioning

When the library must support multiple API versions simultaneously (for example, during a migration period), inline namespaces let you version the symbols without forcing callers to change their code.

```cpp
namespace mylib {
inline namespace v2 {
    struct document { /* v2 layout */ };
    auto parse_document(std::string_view input) -> std::expected<document, parse_error>;
}

namespace v1 {
    struct document { /* v1 layout, kept for compatibility */ };
    auto parse_document(std::string_view input) -> std::expected<document, parse_error>;
}
}

// Callers using `mylib::document` get v2 by default.
// Callers that need v1 use `mylib::v1::document` explicitly.
// Linker symbols are distinct, so v1 and v2 can coexist in one binary.
```

Modules improve build structure and distribution hygiene, but they do not erase ABI reality. Likewise, concepts improve diagnostics and constraints, but they do not automatically make a heavily templated library versionable. Treat these tools as local improvements, not policy substitutes.

## Documentation Should Answer Integration Questions

Library documentation for experienced programmers should focus on adoption decisions, not feature advertising. A caller evaluating a reusable library needs to know:

- What problem the library owns and what it refuses to own.
- Which inputs are borrowed and which outputs own storage.
- Which failures are routine and how they are reported.
- Which allocations, copies, or background work are part of normal use.
- Which thread-safety guarantees exist, if any.
- Which versioning and compatibility promises are real.

Short, production-looking examples help here, especially when they show the normal error path and configuration boundary. Huge tutorial walkthroughs usually do not. The purpose of the docs is to let another team integrate the library without learning internal folklore.

Performance claims deserve the same restraint. Do not say the library is fast. Say what was measured, under what workload, against what baseline, and where the cost model is sensitive. Parsing libraries often have costs that depend heavily on allocation strategy, input size distribution, and failure rate. Say that directly.

## Verification Should Match the Library's Public Promises

A reusable library needs stronger verification discipline than a leaf application component because callers cannot inspect all of your assumptions. Tests should therefore map directly to public promises.

If the library guarantees stable parsing behavior for a documented format version, keep fixture-based contract tests. If it guarantees that malformed input returns structured failures without undefined behavior, run fuzzing and sanitizers on the public parse entry points. If it claims bounded allocations under certain modes, benchmark or instrument that claim. If it exposes host-provided diagnostics, test that the sink receives stable event categories rather than implementation chatter.

This is also where compatibility checks belong. If source compatibility across minor versions is part of the promise, keep integration tests or sample clients that exercise old call patterns. If ABI matters, test produced artifacts in a way that actually checks symbol and layout expectations. "It still compiled on my machine" is not a compatibility policy.

## Know When Not To Build a Library Yet

Teams often create shared libraries too early. If only one application uses the code, if the domain vocabulary is still moving weekly, or if the supposed reuse is mostly organizational aspiration, forcing a stable public surface can freeze bad assumptions into place. Sometimes the right answer is to keep the component internal until the contract stabilizes.

This is not an argument against reuse. It is an argument for charging the real cost of public APIs. Once other teams depend on the library, changing semantics, ownership, or error policy becomes much more expensive than changing internal code. Reuse is valuable when the problem and contract are mature enough to deserve that cost.

## Takeaways

A good reusable C++23 library makes a narrow promise, encodes ownership and invariants directly in public types, keeps failure shape stable, exposes only a few deliberate customization seams, and states versioning and performance claims honestly. It should minimize dependency drag and make adoption decisions easy for experienced callers.

The tradeoffs are the usual ones but paid more explicitly than in applications. Richer signatures can look less elegant. Stable error categories require translation. ABI-aware design limits public template freedom. Narrow seams require discipline about what not to customize. Those are acceptable costs because a library is a long-lived contract, not just a pile of reusable code.

Review questions:

- What narrow promise does this library make, and what important responsibilities does it explicitly refuse?
- Which public types communicate ownership, lifetime, and invariants without hidden assumptions?
- Is the public error surface small, stable, and actionable for callers?
- Which customization seams are genuinely necessary, and which dependencies should be pushed behind adapters instead of into public headers?
- What compatibility promise is actually being made: source, ABI, serialized format, semantic behavior, or some combination?