# Modules, Libraries, Packaging, and ABI Reality

This chapter assumes the source-level interface is already well designed. The question now is how that interface behaves when code is built, shipped, versioned, and consumed across real toolchains.

## The Production Problem

Teams often talk about modules, libraries, and ABI as if they were one topic. They are related and not interchangeable.

Modules are mostly about source organization, dependency hygiene, and build scalability. Library packaging is about how code is distributed and linked: static library, shared library, header-only package, source distribution, internal monorepo component, or plugin SDK. ABI is about whether separately built binaries can agree on layout, calling convention, exception behavior, allocation ownership, symbol naming, and object lifetime.

Treating these as one problem causes expensive mistakes. A team adopts C++20 modules and assumes this somehow stabilizes a public binary boundary. Another ships a shared library whose public headers expose `std::string`, `std::vector`, exceptions, and inline-heavy templates across compilers, then discovers that "works on our build agent" is not a compatibility strategy. A plugin host exports C++ class hierarchies and learns too late that compiler version changes are now deployment events.

This chapter keeps the distinctions sharp. Source hygiene is valuable. Distribution choices are architectural. ABI stability is a contract you either design intentionally or do not offer.

## Modules Solve Source Problems, Not Binary Problems

C++ modules help with parse cost, macro isolation, and dependency control. Those are real wins, especially in large codebases with header-heavy libraries. A well-factored module interface can reduce accidental exposure of implementation detail and make the intended import surface clearer.

But modules do not create a portable binary contract. They do not erase compiler ABI differences. They do not guarantee the same layout rules, exception interoperability, or standard library binary compatibility across vendors. They are not a substitute for packaging strategy.

### What modules replace: the header inclusion model and its hazards

Without modules, C++ compilation is textual inclusion. Every `#include` pastes a header's full text into the translation unit. This creates three classes of real problems.

**Include order dependencies.** If header A defines a macro or type that header B consumes, swapping `#include` order can silently change behavior or break compilation. This is not hypothetical. Large codebases accumulate implicit ordering contracts that no one documents.

```cpp
// order_matters.cpp
#include <windows.h>    // defines min/max as macros
#include <algorithm>     // std::min/std::max are now broken

auto x = std::min(1, 2); // compilation error or wrong overload
```

**Macro pollution.** Every macro defined in every transitively included header is visible everywhere below. A library that `#define`s `ERROR`, `OK`, `TRUE`, `CHECK`, or `Status` can silently collide with unrelated code. The classic defense (include guards, `#undef`, `NOMINMAX`) is fragile and must be remembered at every inclusion site.

```cpp
// some_vendor_lib.h
#define STATUS int
#define ERROR -1

// your_code.cpp
#include "some_vendor_lib.h"
#include "your_domain.h"  // any enum named ERROR or type named STATUS is now broken

enum class Status { ok, error };  // fails to compile: STATUS expands to int
```

**Transitive dependency explosion.** Including one header can pull in hundreds of others. A seemingly small change to an internal header triggers recompilation of thousands of translation units. Build times scale with total transitive include depth, not with the actual dependency graph of the program.

Modules address all three problems: they do not leak macros, they have well-defined import semantics independent of order, and they export only what is explicitly declared. This is a meaningful improvement for source hygiene, even though it does not touch binary compatibility.

### Module syntax in practice

The example project in `examples/web-api/` is structured as seven C++20 module interface units. Each `.cppm` file declares a named module, explicitly imports its dependencies, and exports only the public surface. Here is the structure of a typical module:

```cpp
// examples/web-api/src/modules/error.cppm
module;

// Global module fragment: standard headers that predate modules
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>

export module webapi.error;   // module declaration — names this module

export namespace webapi {
    enum class ErrorCode : std::uint8_t { not_found, bad_request, conflict, internal_error };
    struct Error { /* ... */ };

    template <typename T>
    using Result = std::expected<T, Error>;
}  // only what is inside 'export' is visible to importers
```

Modules that depend on other modules use `import` declarations rather than `#include`:

```cpp
// examples/web-api/src/modules/handlers.cppm
module;
#include <format>
#include <string>
// ...

export module webapi.handlers;

import webapi.error;        // typed error model
import webapi.http;         // Request, Response, Handler
import webapi.json;         // JsonSerializable concept
import webapi.repository;   // TaskRepository
import webapi.task;         // Task, TaskId
```

Several things are worth noting. First, the global module fragment (between `module;` and `export module ...;`) is where standard library headers live. These headers predate the module system and must be included textually. Second, each `import` names a specific module -- there is no transitive inclusion. `handlers.cppm` imports `webapi.error` but does not accidentally pull in everything that `error.cppm` itself includes. Third, the `export` keyword controls visibility precisely: only exported names are reachable by importers. Private helpers, internal implementation details, and non-exported types stay invisible.

The consuming side is equally clean. In `main.cpp`, six import declarations replace what would have been a chain of `#include` directives and all their transitive dependencies:

```cpp
// examples/web-api/src/main.cpp
import webapi.handlers;
import webapi.http;
import webapi.middleware;
import webapi.repository;
import webapi.router;
import webapi.task;
```

No include guards, no macro collisions, no order sensitivity. The build system sees the module dependency graph directly and compiles modules in the right order. This is the source-level improvement that modules deliver.

That means the first decision is not "should we use modules?" It is "what are we promising consumers?"

If the answer is internal source reuse inside one repository with one toolchain baseline, modules may be excellent. If the answer is "we ship a public SDK consumed by unknown build systems and compiler versions," modules may still help your own build, but they do not remove the need for strict binary-boundary discipline.

## Packaging Choices Express Operational Intent

Packaging is where architecture meets deployment.

### Header-only or source-distributed libraries

These avoid many ABI promises because consumers compile the code into their own program. The cost is compile time, larger dependency surfaces, and more exposure of implementation detail. Templates, concepts, and inline functions fit naturally here. This is often a good choice for internal generic utilities or narrow public libraries where performance and optimizer visibility matter more than distribution simplicity.

### Static libraries

Static linkage simplifies deployment and avoids some runtime compatibility issues. It can still create ODR and allocator-boundary problems if the public interface is careless, but it usually reduces cross-version operational complexity. Static libraries fit well for internal components deployed as one unit or for consumers who prefer self-contained binaries.

### Shared libraries and SDKs

These offer deployment and patching advantages, but now you own a real binary boundary. That means symbol visibility, versioning policy, exception rules, allocator ownership, and data layout are no longer private engineering choices. They are part of your product behavior.

### Plugin boundaries

These are the harshest case because host and plugin may be built separately, loaded dynamically, upgraded independently, and sometimes compiled with different flags or compilers. Here, the safest public boundary is often C ABI plus opaque handles and explicit function tables, even if the internal implementation is modern C++ throughout.

The packaging decision should be made from operational constraints, not from what looks elegant in local code.

## Internal Library Versus Public Binary Contract

Many libraries never need stable ABI. That is normal.

If producer and consumer are rebuilt together from the same commit and toolchain, source compatibility matters much more than ABI stability. In that environment, modern C++ APIs can be expressive. Returning vocabulary types, using templates, adopting modules, and relying on inlining may all be good tradeoffs.

The moment you need independently upgradeable binaries, the constraints change. Now even innocent-looking public types become liability. A changed private member order, different standard library implementation, different compiler, or different exception model can break consumers without any source-level signature change.

Do not accidentally promise stable ABI just because you shipped a DLL once.

## ABI Stability Requires Deliberate Narrowing

Stable ABI is expensive because it forbids many convenient language habits at the boundary.

These are common sources of ABI fragility:

1. Exposing standard library types in public binary interfaces.
2. Exposing class layouts whose size or members may change.
3. Throwing exceptions across compiler or runtime boundaries.
4. Allocating on one side and freeing on the other without a shared allocator contract.
5. Exporting inline-heavy templates or virtual hierarchies as the binary extension mechanism.

That does not mean standard C++ library types are bad. It means they are often the wrong public binary boundary.

### Concrete ABI breakage scenarios

These are real scenarios, not theoretical risks.

**Adding a private member changes class size.** A consumer compiled against v1 of a library allocates objects based on `sizeof(Widget)` at their compile time. If v2 adds a private member, the library's methods now write past what the consumer allocated. The result is silent memory corruption, not a linker error.

```cpp
// v1: shipped in libwidget.so
class EXPORT Widget {
    int x_;
    int y_;
public:
    void move(int dx, int dy);  // accesses x_, y_
};
// sizeof(Widget) == 8 for the consumer

// v2: added a z-index member
class EXPORT Widget {
    int x_;
    int y_;
    int z_;  // sizeof(Widget) is now 12
public:
    void move(int dx, int dy);  // same signature, same symbol
};
// Consumer still allocates 8 bytes. Library writes 12. Corruption.
```

**Different standard library implementations.** A shared library built with libstdc++ exposes `std::string` in its API. A consumer built with libc++ links against it. The two implementations have different internal layouts (SSO buffer sizes, pointer arrangements). Calling across this boundary corrupts string state. There is no compile-time or link-time diagnostic.

**Compiler flag mismatch.** Building the library with `-fno-exceptions` and the consumer with exceptions enabled can produce incompatible stack unwinding behavior. Building with different `-std=` flags can change the layout of standard types. Building with different struct packing or alignment flags changes ABI silently.

### ODR violations from header-only libraries

Header-only libraries are popular because they avoid binary distribution complexity. They introduce a different class of problems: One Definition Rule violations.

If two translation units include the same header-only library but compile with different flags, preprocessor definitions, or template arguments that affect inline function behavior, the linker may silently pick one definition and discard the other. The program contains code compiled against two different assumptions linked into one binary.

```cpp
// translation_unit_a.cpp
#define LIBRARY_USE_SSE 1
#include "header_only_math.hpp"  // vector ops use SSE intrinsics

// translation_unit_b.cpp
// LIBRARY_USE_SSE not defined
#include "header_only_math.hpp"  // vector ops use scalar fallback

// Both define the same inline functions with different bodies.
// Linker picks one. Half the program uses the wrong implementation.
// No diagnostic. Possible wrong results or crashes.
```

This is not a contrived scenario. Libraries that use `#ifdef` to select code paths, or that behave differently based on `NDEBUG`, `_DEBUG`, or platform macros, can produce ODR violations in any project that mixes compilation settings. Sanitizers (specifically `-fsanitize=undefined` with ODR violation detection) and link-time tools like `ld`'s `--detect-odr-violations` can catch some of these, but not all.

For a stable shared-library or plugin contract, prefer opaque handles, narrow C-style value types, explicit ownership functions, versioned structs, and clear lifetime rules. Internally, use modern C++ aggressively. At the boundary, be conservative because consumers pay for your binary ambiguity.

## Anti-pattern: Public Binary Surface Mirrors Internal C++ Types

```cpp
// Anti-pattern: fragile ABI surface for a shared library.
class EXPORT Session {
public:
    virtual std::string send(const std::string& request) = 0;
    virtual ~Session() = default;
};

std::unique_ptr<Session> create_session();
```

This interface is attractive inside one build. As a public SDK boundary it is risky.

`std::string` representation and allocator behavior are implementation details. `std::unique_ptr` bakes in deleter and runtime assumptions. Virtual dispatch across the boundary ties host and consumer to compatible object model details. Exceptions may also leak unless documented and controlled. The interface has effectively made your compiler, standard library, and build flags part of the contract.

For a true cross-binary boundary, a versioned C ABI is often safer.

```cpp
struct session_v1;

struct request_buffer {
    const std::byte* data;
    std::size_t size;
};

struct response_buffer {
    const std::byte* data;
    std::size_t size;
};

struct session_api_v1 {
    std::uint32_t struct_size;
    session_v1* (*create)() noexcept;
    void (*destroy)(session_v1*) noexcept;
    status_code (*send)(session_v1*, request_buffer, response_buffer*) noexcept;
    void (*release_response)(response_buffer*) noexcept;
};
```

This is less pretty and much more honest. The boundary names allocation ownership, versioning surface, and error transport explicitly. Internally, the implementation can still use `std::expected`, `std::pmr`, coroutines, modules, and any C++23 technique that stays behind the wall.

## The Pimpl Tradeoff Still Exists

For C++ consumers within one toolchain family, the pimpl pattern still has a place. It can reduce rebuild fanout, hide private members, and preserve class size across some implementation changes. It also adds indirection, allocation, and complexity. Pimpl is not a free modernization badge.

Use it when all of the following are true:

1. You need to hide representation or reduce compile-time exposure.
2. The object is not so hot that another pointer chase is a measurable problem.
3. The library truly benefits from keeping class layout stable.

Do not reach for pimpl just because headers are messy. Modules may solve that source problem better for internal builds. Pimpl is primarily a representation and compatibility tool, not a style requirement.

## Modules in Real Build Systems

C++23-first advice has to stay realistic. Modules are valuable and still operationally uneven across toolchains, package managers, and mixed-language build systems.

Inside a controlled build environment with GCC 14+, Clang 18+, or MSVC 17.10+, modules can reduce parse overhead and make dependency intent clearer. In heterogeneous environments, the module artifact model, build graph integration, and package-manager support may still impose friction. That friction is not an argument against modules. It is a reminder that adoption belongs to build architecture, not only to language enthusiasm.

A good default is pragmatic:

1. Use modules first for internal components built together.
2. Avoid making public package consumption depend on module support unless your consumer ecosystem is controlled.
3. Keep the binary contract decision separate from the module decision.

The `examples/web-api/` project demonstrates this pragmatic approach. Its seven `.cppm` files (`error`, `task`, `json`, `http`, `repository`, `handlers`, `middleware`, `router`) form a clear module dependency graph built together from one CMakeLists. Standard library headers remain in the global module fragment because they are not yet modularized on all toolchains. The project does not attempt to export its modules as a public package -- it uses modules to organize its own source. That is the right starting point.

## Versioning Policy Is Part of the Interface

Packaging without versioning policy is wishful thinking. Consumers need to know what kind of change is allowed between releases: source-compatible only, ABI-compatible within a major version, or no promises beyond exact build matching.

This policy affects technical design. If ABI compatibility within a major version matters, your public types must be narrowed aggressively and your rollout process must include ABI review. If consumers rebuild from source, the policy can be looser and the interfaces more idiomatic.

Versioning is not only about semantic version numbers. It is also about symbol versioning where available, inline namespace strategy for source-level APIs, feature detection, deprecation windows, and package metadata that correctly describes compiler and runtime requirements.

## Memory, Exceptions, and Ownership Across the Boundary

Most cross-library failures are not glamorous. They come from ownership mismatches.

If one side allocates memory and the other deallocates it, the allocator contract must be explicit. If exceptions are allowed to cross the boundary, runtime and compiler assumptions must align. If the boundary uses callbacks, retention and thread-affinity rules must be documented. If background work continues during unload, the packaging design is already unsafe.

```cpp
// Anti-pattern: cross-boundary allocation mismatch.
// Library (built with MSVC debug runtime, uses debug heap):
EXPORT char* get_name() {
    char* buf = new char[64];
    std::strcpy(buf, "session-001");
    return buf;
}

// Consumer (built with MSVC release runtime, uses release heap):
void use_library() {
    char* name = get_name();
    // ...
    delete[] name;  // CRASH: freeing debug-heap memory on release heap
}
```

The fix is to never let allocation and deallocation cross the boundary. The library that allocates must also provide the deallocation function, or the boundary must use caller-provided buffers.

```cpp
// Safe: library owns both allocation and deallocation.
EXPORT char* get_name();
EXPORT void free_name(char* name);

// Also safe: caller provides the buffer.
EXPORT status_code get_name(char* buffer, std::size_t buffer_size);
```

These are the details that turn a locally clean interface into an operable library.

## Verification and Review Questions

Review packaging and ABI separately from source-level API quality.

1. Is this library intended for same-build consumption, source consumption, or independently upgradeable binary consumption?
2. Are we using modules to improve source hygiene while mistakenly assuming they solve ABI?
3. Does the public boundary expose types whose layout or runtime behavior we do not actually control?
4. Is allocation ownership explicit across the boundary?
5. Are versioning and compatibility promises documented and testable?
6. Would a C ABI plus opaque handles be safer for this plugin or SDK than exported C++ classes?

Verification should include build-matrix testing across supported compilers and standard libraries, symbol visibility inspection, ABI comparison tooling where relevant, and packaging tests that simulate real consumer integration. A unit test suite inside the producer repository is not enough evidence for a public binary contract.

## Takeaways

Modules, packaging, and ABI are three different design axes.

Use modules to improve source boundaries and build scalability. Choose packaging based on deployment and consumer constraints. Promise stable ABI only when you are willing to narrow the public boundary and verify it continuously. Inside the implementation, use modern C++23 freely. At true binary boundaries, prefer explicit ownership, explicit versioning, and conservative surface area.

The sharpest mistake in library design is exporting internal elegance as public binary policy. Source-level beauty does not make ABI risk disappear. Only deliberate boundary design does.