# Chapter 8: Modules, Libraries, ABI, and Versioning

*Prerequisites: Chapter 7 (API shape determines what crosses a binary boundary).*

> **Prerequisites:** You should already understand the distinction between interface and implementation, dependency direction, and why an API's surface area is a long-term liability. Chapter 7 covered these at the source level. This chapter moves into the territory where source-level reasoning is no longer sufficient: shared libraries, binary compatibility, separate compilation, mixed toolchains, and the versioning contracts that hold a deployed system together across independent release cadences.
>
> You will also benefit from familiarity with your platform's linker model (ELF symbol visibility on Linux, dllexport/dllimport on Windows) and a working knowledge of how translation units become object files and then libraries. The chapter does not teach linking from scratch; it focuses on the decisions that go wrong when linking is treated as someone else's problem.

---

## 8.1 The Production Problem

A team ships a shared library. Internal consumers link against it. Six months later, a field is added to a widely-used struct, a virtual function is inserted into a base class, and a `std::string` parameter changes from libstdc++ to libc++. None of these changes produce a compiler error at the consumer's call site — the headers still parse, the symbols still resolve. The failures arrive at runtime: memory corruption, wrong-vtable dispatch, silent data truncation, crashes deep inside the allocator.

The root cause is not a code defect in the traditional sense. It is an **ABI break** — a change to the binary layout or calling convention of a type that crosses a compilation boundary. The compiler cannot detect it because the consumer was compiled against the old headers. The linker cannot detect it because the mangled symbol names have not changed. Only the runtime discovers the mismatch, and its diagnostics are typically a segfault or heap corruption.

This class of failure scales with the number of independently-compiled consumers, the frequency of library releases, and the degree to which teams rely on implicit binary compatibility. In organizations with dozens of services consuming the same core library, a careless struct change can trigger a cascade of production incidents that take days to diagnose and weeks to fully remediate.

C++20 modules add a separate axis of complexity: they change how declarations reach consumers, how build systems discover dependencies, and what the compiler can optimize across translation unit boundaries. Modules solve real problems — header pollution, macro leakage, redundant parsing — but they do not solve ABI stability, and their interaction with shared libraries and versioning requires deliberate design.

---

## 8.2 The Legacy Approach

### 8.2.1 Headers-everywhere, hope-for-the-best

The traditional model is straightforward. A library exposes a set of headers and a static or shared archive. Consumers `#include` the headers, link the archive, and trust that the two are mutually consistent. Version management, if any, is a major version number on the `.so` or `.dylib` name and an informal promise not to break things.

```cpp
// Anti-pattern: ABI-sensitive type exposed directly in a public header
// mylib/session.h
#pragma once
#include <string>
#include <vector>

namespace mylib {

struct Session {
    std::string user_id;       // RISK: std::string layout is ABI-dependent
    std::vector<int> roles;    // RISK: std::vector layout is ABI-dependent
    int flags = 0;
    // Adding a field here later breaks every consumer's sizeof(Session)
};

// RISK: returning a struct by value across shared library boundary
Session create_session(const std::string& user);

} // namespace mylib
```

This works when every translation unit is compiled with the same compiler, the same standard library, the same flags, and rebuilt atomically. It falls apart the moment any of those conditions is violated — which in production happens routinely:

- A consumer upgrades their compiler but the library provider has not rebuilt yet.
- Two libraries in the same process link different versions of a common dependency.
- A library is distributed as a binary package and the consumer cannot rebuild it.
- Debug and release builds of the same library end up loaded in the same process.

The legacy response is either "rebuild everything" (expensive, sometimes impossible) or "don't change anything" (which freezes API evolution and accumulates technical debt). Neither scales.

---

## 8.3 The Modern Approach

Solving library packaging, ABI, and versioning in production C++ requires working at three distinct layers: the **source boundary** (what consumers see at compile time), the **binary boundary** (what survives separate compilation), and the **version contract** (what guarantees persist across releases). Conflating these layers is where most designs fail.

### 8.3.1 Separating the ABI Surface from the API Surface

The most reliable technique for ABI stability is to ensure that the types crossing the binary boundary have a layout you control completely. This means: no standard library types in the public interface of a shared library, no compiler-generated vtables that consumers depend on, and no inline functions whose behavior depends on private members.

**The C-compatible firewall.** Despite writing modern C++ internally, the shared library's exported interface uses a C-compatible calling convention and POD-like types at the boundary:

```cpp
// mylib/export.h — the ABI-stable public interface
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#  ifdef MYLIB_BUILDING
#    define MYLIB_API __declspec(dllexport)
#  else
#    define MYLIB_API __declspec(dllimport)
#  endif
#else
#  define MYLIB_API __attribute__((visibility("default")))
#endif

extern "C" {

struct mylib_session;  // opaque handle

MYLIB_API mylib_session* mylib_session_create(const char* user_id);
MYLIB_API void           mylib_session_destroy(mylib_session* s);
MYLIB_API int32_t        mylib_session_flags(const mylib_session* s);
MYLIB_API const char*    mylib_session_user_id(const mylib_session* s);

// Versioning support
MYLIB_API int32_t        mylib_version_major(void);
MYLIB_API int32_t        mylib_version_minor(void);

} // extern "C"
```

Behind this firewall, the implementation uses whatever C++ it likes:

```cpp
// mylib/session.cpp — internal, never exposed in headers
#include "export.h"
#include <string>
#include <vector>

struct mylib_session {
    std::string user_id;
    std::vector<int> roles;
    int flags = 0;
};

extern "C" {

mylib_session* mylib_session_create(const char* user_id) {
    auto* s = new mylib_session{};
    s->user_id = user_id;
    return s;
}

void mylib_session_destroy(mylib_session* s) {
    delete s;
}

int32_t mylib_session_flags(const mylib_session* s) {
    return s ? s->flags : 0;
}

const char* mylib_session_user_id(const mylib_session* s) {
    return s ? s->user_id.c_str() : "";
}

} // extern "C"
```

This is not elegant. It is effective. The consumer never sees `std::string`, never knows the size of the struct, and is immune to internal layout changes. The allocation and destruction happen inside the library, which guarantees they use the same allocator.

**C++ wrappers over the C boundary.** A header-only C++ wrapper can provide RAII and type safety on the consumer side without reintroducing ABI coupling:

```cpp
// mylib/session.hpp — header-only, consumer-side convenience
#pragma once
#include "export.h"
#include <memory>
#include <string_view>

namespace mylib {

class Session {
    struct Deleter {
        void operator()(mylib_session* s) const noexcept {
            mylib_session_destroy(s);
        }
    };
    std::unique_ptr<mylib_session, Deleter> handle_;

public:
    explicit Session(std::string_view user_id)
        : handle_(mylib_session_create(user_id.data())) {}

    int flags() const noexcept { return mylib_session_flags(handle_.get()); }
    std::string_view user_id() const noexcept {
        return mylib_session_user_id(handle_.get());
    }
};

} // namespace mylib
```

This wrapper is compiled entirely on the consumer side. It depends on the C ABI, not on any C++ layout. It can use `std::unique_ptr`, `std::string_view`, or any other type freely because those types never cross the library boundary.

### 8.3.2 The Pimpl Pattern as an ABI Firewall

When a C-level interface is too restrictive — for example, if the library must support inheritance, overloading, or templates at the boundary — the Pimpl (pointer-to-implementation) idiom provides a weaker but still useful ABI firewall within C++:

```cpp
// mylib/codec.hpp — public header
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace mylib {

class MYLIB_API Codec {
public:
    explicit Codec(int level);
    ~Codec();

    Codec(Codec&&) noexcept;
    Codec& operator=(Codec&&) noexcept;

    // ABI-safe: parameters and return types are fixed-layout
    size_t compress(std::span<const std::byte> input,
                    std::span<std::byte> output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mylib
```

The `Impl` struct is defined only in the `.cpp` file. Adding fields to it does not change `sizeof(Codec)` — which is always the size of one pointer. Consumers never need recompilation when the implementation changes.

The costs: every method call is an extra indirection through the pointer; the object is always heap-allocated (or at least its innards are); and move semantics must be explicitly defined because the compiler-generated ones interact poorly with incomplete types in some configurations. For types that are constructed rarely and used heavily, the indirection cost is negligible. For types that live in hot loops or tight containers, measure before committing.

### 8.3.3 C++20/23 Modules: Full Treatment

C++20 modules represent the most significant change to the C++ compilation model since the language's inception. They replace the textual inclusion model (`#include`) with a structured, semantically-aware import mechanism. Understanding their mechanics, capabilities, and limitations is essential for any team building or consuming libraries in modern C++.

#### Module Units and Partitions

A module consists of one or more **module units** — translation units that declare themselves as belonging to a named module. The two fundamental kinds are:

**Module interface units** declare what the module exports. There is exactly one **primary module interface unit** per module, which carries the module's name:

```cpp
// mylib.cppm — primary module interface unit
export module mylib;

export namespace mylib {

struct Config {
    int32_t max_connections = 128;
    int32_t timeout_ms = 5000;
};

class Engine {
public:
    explicit Engine(Config cfg);
    ~Engine();
    void run();

private:
    struct Impl;
    Impl* impl_;
};

} // namespace mylib
```

**Module implementation units** provide definitions for declarations in the interface without adding to the module's exported surface:

```cpp
// mylib_impl.cpp — module implementation unit
module mylib;

#include <iostream>  // OK: #include in implementation unit, not exported

namespace mylib {

struct Engine::Impl {
    Config cfg;
    // internal state...
};

Engine::Engine(Config cfg) : impl_(new Impl{cfg}) {}
Engine::~Engine() { delete impl_; }
void Engine::run() { /* ... */ }

} // namespace mylib
```

**Module partitions** split a large module's interface or implementation across multiple files while keeping everything under a single module name. This is essential for modules that would otherwise require a 10,000-line interface file:

```cpp
// mylib-types.cppm — interface partition
export module mylib:types;

export namespace mylib {

struct Config {
    int32_t max_connections = 128;
    int32_t timeout_ms = 5000;
};

enum class LogLevel { debug, info, warn, error };

} // namespace mylib
```

```cpp
// mylib-engine.cppm — interface partition
export module mylib:engine;

import :types;  // import the :types partition

export namespace mylib {

class Engine {
public:
    explicit Engine(Config cfg);
    ~Engine();
    void run();
    void set_log_level(LogLevel level);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace mylib
```

```cpp
// mylib.cppm — primary interface re-exports partitions
export module mylib;

export import :types;
export import :engine;
```

Consumers write `import mylib;` and see everything. The partition structure is invisible to them — it is an internal organizational concern.

**Implementation partitions** (without `export`) provide internal shared code that multiple implementation units can use without exposing anything to consumers:

```cpp
// mylib-internal.cpp — implementation partition (not exported)
module mylib:internal;

namespace mylib::detail {
    void log_message(const char* msg) { /* ... */ }
}
```

#### Export Declarations

The `export` keyword controls what crosses the module boundary. It can be applied at multiple granularities:

```cpp
export module mylib;

// Export a single declaration
export int global_version();

// Export a block of declarations
export {
    struct Widget { int id; };
    Widget make_widget(int id);
    void destroy_widget(Widget& w);
}

// Export an entire namespace and its contents
export namespace mylib {
    class Session { /* ... */ };
    Session create_session();
}
```

Declarations not marked `export` in a module interface unit are **module-local**: they are visible within the module's own translation units but invisible to importers. This is a fundamentally stronger encapsulation mechanism than anything headers provide — there is no equivalent of "just include the internal header."

```cpp
export module mylib;

// Not exported: visible within this module only
namespace detail {
    int compute_hash(const char* data, int len);
}

export namespace mylib {
    // Exported: visible to consumers
    class Cache {
    public:
        void insert(const char* key, int key_len);
    private:
        // Private members are part of the interface for layout purposes,
        // but detail::compute_hash remains hidden from consumers
    };
}
```

#### `import std` (C++23)

C++23 introduced `import std;` as a way to import the entire standard library as a single module. This eliminates the need to remember which header provides which declaration and eliminates all standard library macro leakage:

```cpp
export module mylib;

import std;  // C++23: imports all of std:: in one declaration

export namespace mylib {

// std::string, std::vector, std::int32_t — all available
class Processor {
public:
    explicit Processor(std::string name);
    std::vector<std::byte> process(std::span<const std::byte> input);
};

} // namespace mylib
```

There is also `import std.compat;` which additionally provides the C library global-namespace names (`::printf`, `::size_t`, etc.) for codebases that use them.

Key caveats about `import std`:

- **Compiler support:** as of early 2026, MSVC (17.8+) and Clang (18+) support `import std`. GCC support landed in GCC 15. Verify your specific toolchain before depending on it.
- **Build system support:** the standard library module must be pre-built or built as part of your project. CMake 3.30+ handles this for MSVC; other toolchains may require manual configuration.
- **No macros:** `import std;` does not provide `assert`, `errno`, `NULL`, `offsetof`, or any other standard library macro. If you need them, you must `#include` the relevant header. This is intentional — macros are not part of the module system.
- **Performance:** importing the standard library module is typically faster than including even a handful of standard headers, because the compiler reads a pre-built BMI rather than parsing thousands of lines of header text.

#### Private Module Fragments

A **private module fragment** allows a module to include implementation details in the same file as the interface, while guaranteeing that those details do not affect consumers. When the private fragment changes, the compiler is permitted (but not required) to skip recompilation of consumers:

```cpp
export module mylib.config;

export namespace mylib {

class ConfigStore {
public:
    ConfigStore();
    ~ConfigStore();
    int get(const char* key) const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace mylib

module :private;  // everything below is invisible to consumers

#include <unordered_map>
#include <string>

namespace mylib {

struct ConfigStore::Impl {
    std::unordered_map<std::string, int> data;
};

ConfigStore::ConfigStore() : impl_(new Impl{}) {}
ConfigStore::~ConfigStore() { delete impl_; }

int ConfigStore::get(const char* key) const {
    auto it = impl_->data.find(key);
    return it != impl_->data.end() ? it->second : -1;
}

} // namespace mylib
```

The private module fragment is particularly useful for small, self-contained modules where splitting into separate interface and implementation files would be overkill. It combines the convenience of a single file with the encapsulation guarantee that consumers cannot depend on implementation details.

**Limitation:** a module with a private fragment cannot have separate module implementation units. It is a single-file-per-module design. For larger modules, use partitions and separate implementation units instead.

### 8.3.4 Module Build System Integration

Modules fundamentally change how build systems work. With headers, the build system only needs to know which `.cpp` files to compile and which libraries to link. With modules, the build system must understand **inter-translation-unit dependencies at compile time** — a `.cpp` file that writes `import mylib;` cannot be compiled until `mylib.cppm` has been compiled and its BMI (binary module interface) is available.

#### Dependency Scanning and Build Graphs

Traditional C++ builds are embarrassingly parallel: every translation unit can be compiled independently, and the linker combines them. Modules break this property. The build graph now has compile-time edges:

```
mylib-types.cppm  ──→  mylib-engine.cppm  ──→  mylib.cppm  ──→  consumer.cpp
     (compile first)     (needs :types BMI)   (needs both BMIs)  (needs mylib BMI)
```

The build system must **scan** source files to discover `import` and `export module` declarations before it can determine the compilation order. This is called **dependency scanning** or **P1689 scanning** (after the paper that standardized the scan output format).

#### CMake Module Support

CMake 3.28+ supports C++20 modules through the `CXX_MODULES` source file property and the `FILE_SET` mechanism:

```cmake
cmake_minimum_required(VERSION 3.28)
project(mylib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(mylib)

target_sources(mylib
    PUBLIC
        FILE_SET CXX_MODULES FILES
            src/mylib.cppm
            src/mylib-types.cppm
            src/mylib-engine.cppm
    PRIVATE
        src/mylib_impl.cpp
)

# Consumer links against mylib and gets module access
add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE mylib)
```

Critical build system considerations:

- **Generator support:** as of CMake 3.30, module support works reliably with Ninja and MSBuild (MSVC). Makefiles have experimental support. Always use Ninja for module-heavy projects unless you have a specific reason not to.
- **Incremental builds:** when a module interface unit changes, all translation units that import it must be recompiled. This is analogous to changing a widely-included header, but the build system enforces it precisely — only actual importers are rebuilt, not everything that transitively included a header chain.
- **BMI caching:** binary module interface files are compiler-specific and flag-specific. They cannot be shared across different compilers, different optimization levels, or different standard library configurations. CI systems that cache build artifacts must key the cache on the full compiler configuration.
- **No BMI standardization:** there is no portable BMI format. A BMI produced by GCC cannot be consumed by Clang, and vice versa. Even different major versions of the same compiler may produce incompatible BMIs. This is a toolchain constraint, not a language limitation.

#### Dependency Scanning in Practice

Build systems discover module dependencies through a two-phase approach:

1. **Scan phase:** the build system invokes the compiler in a scanning mode (e.g., `clang-scan-deps`, or MSVC's built-in scanning) to discover which modules each source file imports and exports, without performing full compilation.
2. **Build phase:** using the dependency graph from the scan phase, the build system compiles modules in topological order.

This two-phase approach means that adding or renaming a module requires the build system to rescan. Most build systems handle this automatically, but custom build setups that bypass the standard mechanisms (e.g., hand-written Makefiles) will need explicit scanning integration.

### 8.3.5 Migrating from Headers to Modules

Migrating an existing codebase from headers to modules is not an atomic operation. It is an incremental process that can span months or years, depending on the codebase's size and the maturity of toolchain support.

#### Header Units: The Bridge

**Header units** allow existing headers to be imported rather than included, providing a migration stepping stone:

```cpp
// Instead of:
#include <vector>
#include <string>
#include "mylib/utils.h"

// You can write:
import <vector>;
import <string>;
import "mylib/utils.h";
```

When a header is imported as a header unit, the compiler processes it once, produces a BMI, and subsequent imports reuse that BMI. This provides the build-time benefits of modules (no redundant parsing) without requiring the header to be rewritten as a module.

Header units are a transitional mechanism. They preserve macro export (unlike named modules) and work with headers that were not designed for modular consumption. Their primary value is enabling gradual migration: convert `#include` to `import` one header at a time, then later convert the most important headers into proper named modules.

**Limitations of header units:**
- Not all headers are importable. Headers that rely on inclusion-order-dependent macros, or that define entities differently depending on previously-defined macros, may not work as header units.
- Compiler support varies. MSVC has the broadest header unit support; GCC and Clang support standard library header units but may have gaps with arbitrary third-party headers.
- Header units still export macros, which means they do not provide the macro isolation benefit of named modules.

#### Incremental Adoption Strategy

A practical migration plan proceeds in layers:

**Phase 1: Leaf modules.** Convert self-contained utility libraries — logging, string helpers, configuration — into named modules. These have few dependencies and many consumers, so the build-time savings compound. Keep the original headers as a parallel path for consumers that have not migrated.

```cpp
// Before: mylib/logging.h with #pragma once, include guards, etc.
// After: mylib-logging.cppm
export module mylib.logging;

import std;

export namespace mylib::logging {

enum class Level { debug, info, warn, error };
void log(Level level, std::string_view message);
void set_minimum_level(Level level);

} // namespace mylib::logging
```

**Phase 2: Wrapper modules.** For libraries with large header sets, create a module that wraps the existing headers:

```cpp
// mylib.cppm — wraps existing headers during transition
export module mylib;

// Internal: include the existing headers
#include "mylib/config.h"
#include "mylib/session.h"
#include "mylib/codec.h"

// Re-export the public API
export namespace mylib {
    using mylib::Config;
    using mylib::Session;
    using mylib::Codec;
}
```

This wrapper approach lets consumers switch to `import mylib;` immediately while the library internals are migrated to proper module units over time.

**Phase 3: Full modularization.** Convert the library's internal structure to module partitions, eliminate the legacy headers from the build, and remove the wrapper layer.

#### Compatibility Patterns

During migration, a library must support both `#include` and `import` consumers simultaneously. The standard pattern uses a conditional approach:

```cpp
// mylib/config.h — dual-use header
#pragma once

// When consumed via module, this header's content is already available.
// When consumed via #include, provide the declarations directly.
#ifndef MYLIB_MODULE_MODE

#include <cstdint>

namespace mylib {

struct Config {
    int32_t max_connections = 128;
    int32_t timeout_ms = 5000;
};

} // namespace mylib

#endif // MYLIB_MODULE_MODE
```

```cpp
// mylib.cppm — module interface
export module mylib;

#define MYLIB_MODULE_MODE
#include "mylib/config.h"  // include but suppress duplicate declarations

import std;

export namespace mylib {
    // Export the types defined in the module
    struct Config {
        int32_t max_connections = 128;
        int32_t timeout_ms = 5000;
    };
}
```

In practice, maintaining this dual mode is tedious. A cleaner approach is to keep the authoritative declarations in the module interface and generate or maintain a thin compatibility header that includes what legacy consumers need. Once all consumers have migrated, remove the compatibility layer.

### 8.3.6 Module Interaction with ABI

This is the most commonly misunderstood aspect of C++20 modules. The short version: **modules change how declarations reach consumers; they do not change the binary representation of those declarations.**

#### What Modules Do NOT Guarantee

- **Layout stability.** A `struct` exported from a module has the same layout rules as a `struct` defined in a header. If you add a field, `sizeof` changes, and any consumer compiled against the old module interface will access memory incorrectly.

- **Symbol compatibility.** Modules do not define a stable binary interface. The mangled names of exported entities are still compiler-specific. A BMI produced by one compiler version cannot generally be consumed by another.

- **Cross-library safety.** Exporting `std::vector<int>` from a module does not make it safe to pass across a shared library boundary. The vector's layout is still implementation-defined and configuration-dependent.

- **ABI versioning.** Modules have no built-in mechanism for versioning their binary interface. There is no module-level equivalent of SONAME or inline namespaces (though you can still use inline namespaces within a module).

#### What Modules DO Provide

- **Reduced accidental ABI surface.** With headers, any declaration in any transitively-included header is part of your de facto interface. Consumers can (and do) depend on internal types that happen to be visible. Modules export only what you mark `export`. Non-exported declarations are invisible to consumers, which means they cannot accidentally depend on internal layout details.

- **Controlled template visibility.** A module can export a function that uses a template internally without exporting the template itself. This reduces the number of types whose layout consumers depend on.

- **Build-time mismatch detection.** When a module interface changes, the build system forces recompilation of all importers. This does not prevent ABI breaks across shared library boundaries (where the consumer may have been compiled separately), but it eliminates a class of stale-header bugs within a unified build.

#### Practical Implication

Modules and ABI stability are complementary concerns that require separate solutions. Use modules for build hygiene and encapsulation. Use C-linkage boundaries, Pimpl, opaque handles, and inline namespaces for ABI stability. The two work together — a module can export a Pimpl-based class that is ABI-stable — but neither substitutes for the other.

```cpp
// Good: module + Pimpl = clean interface + ABI stability
export module mylib;

import std;

export namespace mylib {

class MYLIB_API Processor {
public:
    explicit Processor(std::string_view name);
    ~Processor();
    Processor(Processor&&) noexcept;
    Processor& operator=(Processor&&) noexcept;

    // ABI-safe: fixed-layout parameters and return types
    int process(std::span<const std::byte> input,
                std::span<std::byte> output);

private:
    struct Impl;                    // defined in implementation unit
    std::unique_ptr<Impl> impl_;   // sizeof(Processor) = sizeof(unique_ptr)
};

} // namespace mylib
```

### 8.3.7 Plugin Interfaces and Dynamic Loading

Systems that support runtime extensibility — plugin architectures, driver models, scripted customization — face the most extreme version of the ABI problem. The plugin and the host may be compiled by different teams, with different compilers, at different times.

The viable contract is narrow: a C-linkage entry point that returns a pointer to a struct of function pointers.

```cpp
// plugin_api.h — shared between host and all plugins, versioned carefully
#pragma once
#include <cstdint>

#define PLUGIN_API_VERSION 3

struct plugin_api {
    int32_t api_version;  // must be first, always
    const char* (*name)(void);
    int (*init)(const char* config_json);
    void (*shutdown)(void);
    int (*process)(const void* input, int32_t input_len,
                   void* output, int32_t output_capacity);
};

// Plugin must export this symbol with C linkage
typedef const plugin_api* (*plugin_entry_fn)(void);
```

The host loads the plugin at runtime (`dlopen`/`LoadLibrary`), resolves `plugin_entry`, checks `api_version`, and dispatches through function pointers. This is deliberately primitive. The simplicity is the feature: it works across compilers, across C++ standard library implementations, and across years of separate development.

Version negotiation is critical. The `api_version` field lets the host refuse to load plugins built against an incompatible API. When the API must evolve, add new function pointers at the end of the struct and bump the version. The host checks the version before calling any new function pointer. Never reorder or remove entries — that is a binary-incompatible change.

```cpp
// Intentional partial: POSIX implementation shown.
// Windows uses LoadLibrary / GetProcAddress with the same version checks.
// Host-side loading with version check
#include <dlfcn.h>  // POSIX; use LoadLibrary on Windows
#include <cstdio>

bool load_plugin(const char* path) {
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return false;
    }

    auto entry = reinterpret_cast<plugin_entry_fn>(
        dlsym(handle, "plugin_entry"));
    if (!entry) {
        dlclose(handle);
        return false;
    }

    const plugin_api* api = entry();
    if (!api || api->api_version < 2) {
        // Minimum supported version is 2
        std::fprintf(stderr, "plugin %s: unsupported API version %d\n",
                     path, api ? api->api_version : -1);
        dlclose(handle);
        return false;
    }

    // Safe to use api->name, api->init, etc.
    // Fields added in version 3 must be checked:
    if (api->api_version >= 3 && api->process) {
        // api->process is available
    }

    return true;
}
```

### 8.3.8 Versioning Strategy

Version numbers are promises. Breaking them is expensive; making the wrong promise is worse.

**Semantic versioning (semver)** works at the source level: major version changes signal API breaks, minor versions add functionality, patches fix bugs. For C++ shared libraries, semver alone is insufficient because source compatibility does not imply binary compatibility. A change that is API-compatible (adding a defaulted parameter, for instance) can still be ABI-incompatible if it changes mangled symbol names.

A workable scheme for shared libraries:

1. **SO version / binary version** tracks ABI compatibility. Bump it whenever the binary interface changes in any way that an existing consumer cannot tolerate. On Linux, this is the `SONAME` (e.g., `libmylib.so.4`). On Windows, it can be encoded in the DLL filename or a side-by-side manifest.

2. **Source version** tracks API compatibility using semver or a similar scheme. Document it in the package metadata.

3. **Build metadata** captures the exact compiler, flags, and standard library version used to produce a given binary. Embed it in a queryable function:

```cpp
extern "C" MYLIB_API const char* mylib_build_info(void) {
    return
        "mylib 2.3.1"
        " abi=4"
        " compiler=" MYLIB_COMPILER_ID    // set by build system
        " stdlib=" MYLIB_STDLIB_ID        // e.g., "libstdc++-14"
        " flags=" MYLIB_BUILD_FLAGS;      // e.g., "-O2 -DNDEBUG"
}
```

This function costs nothing at runtime and provides invaluable diagnostic information when chasing a binary mismatch in production.

---

## 8.4 Tradeoffs and Boundaries

### 8.4.1 C boundary vs. C++ boundary

A C-linkage boundary maximizes compatibility at the cost of expressiveness. You lose overloading, templates, RAII at the boundary, and namespacing. The C++ wrapper pattern (Section 8.3.1) recovers most of this on the consumer side, but the library author must maintain two layers: the stable C ABI and the internal C++ implementation.

The alternative — exporting C++ classes directly — works when you control the full toolchain and rebuild everything together. Many organizations do this for internal libraries and accept the coupling. The decision hinges on deployment topology: if the library and its consumers always ship as a unit, C++ export is viable. If they can be updated independently, a C or Pimpl boundary is necessary.

### 8.4.2 Pimpl overhead

The Pimpl indirection (one pointer dereference per method call, one heap allocation per object) is rarely measurable in application-level code. It becomes relevant for types that are allocated in millions and accessed in tight loops — and for those types, you probably should not be exposing them across a shared library boundary at all. The types that cross binary boundaries should be session-lived, not per-element-lived.

### 8.4.3 Modules vs. headers for distributed libraries

Modules provide cleaner dependency hygiene and faster builds for consumers who use a compatible toolchain. But they impose a toolchain constraint: the consumer must use a compiler that can consume your module's BMI (binary module interface) format, which is not standardized and not portable across compiler vendors or even major versions of the same compiler.

For libraries distributed as source or as binary packages to unknown consumers, headers remain the pragmatic choice. For monorepo-internal libraries where the toolchain is controlled, modules offer real build-time savings.

### 8.4.4 Inline namespaces for version management

The standard library itself uses inline namespaces to version ABI. The technique is available to library authors:

```cpp
namespace mylib {
inline namespace v2 {

class Codec {
    // Current version — consumers see mylib::Codec
    // but the mangled name includes v2, so it won't link
    // against a binary built with v1::Codec
};

} // namespace v2
} // namespace mylib
```

If a consumer is compiled against v2 headers but links against a v1 binary, the linker reports an undefined symbol rather than silently mismatching. This converts a runtime crash into a link-time error — a significant improvement. The cost is that every ABI-breaking change requires bumping the inline namespace, and old symbols must be retained in the library binary if you need backward compatibility.

### 8.4.5 The hidden cost of `std::` types at boundaries

`std::string`, `std::vector`, `std::function`, and other standard library types have **implementation-defined layout**. Their size, alignment, and internal pointer structure differ across:

- Standard library implementations (libstdc++ vs. libc++ vs. MSVC STL)
- Major versions of the same implementation (libstdc++ pre/post COW `std::string`)
- Debug vs. release configurations (MSVC iterator debugging changes container layout)
- Different `-D_GLIBCXX_USE_CXX11_ABI` settings

Passing these types across a shared library boundary is safe only when both sides are compiled with the identical standard library version and configuration. In a controlled monorepo with uniform toolchain enforcement, this holds. In any other deployment model, it is a latent defect.

---

## 8.5 Testing and Tooling Implications

### 8.5.1 ABI compatibility testing

ABI breaks are invisible to unit tests that compile everything together. Detecting them requires:

1. **Separate compilation tests.** Compile the consumer against the old library headers, then link against the new library binary. If the test suite passes under this configuration, the ABI is preserved. Automate this in CI with a matrix that crosses library version against consumer version.

2. **ABI checker tools.** `abidiff` (from libabigail) compares two versions of a shared library and reports ABI differences: changed symbol types, struct layout changes, removed exports. Integrate it into the release pipeline:

```bash
# CI step: verify ABI compatibility between releases
abidiff libmylib.so.old libmylib.so.new --headers-dir1 include/old \
                                          --headers-dir2 include/new
```

3. **Symbol visibility auditing.** On Linux, `nm -D` or `readelf --dyn-syms` lists exported symbols. On Windows, `dumpbin /EXPORTS`. If a symbol you did not intend to export appears in this list, it is part of your ABI whether you meant it to be or not. Hide internal symbols with `-fvisibility=hidden` (GCC/Clang) and export only what you mark explicitly.

```cmake
# CMakeLists.txt — default to hidden visibility
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)
```

### 8.5.2 Module build testing

Module dependency scanning failures are subtle and often manifest as stale BMI files or missing imports. Test configurations should include:

- Clean builds (no cached BMIs) in CI to catch implicit dependency ordering bugs.
- Incremental builds after touching a module interface unit to verify that all dependent translation units are rebuilt.
- Cross-compiler testing if consumers may use different toolchains.
- Partition rename and reorder tests: verify that moving declarations between partitions does not break consumers who import only the top-level module.
- `import std;` round-trip: confirm that the standard library module builds correctly on every CI platform, particularly when the compiler or standard library is upgraded.
- Header unit fallback: if header units are used during migration, test that reverting to `#include` produces identical behavior, ensuring header units are not masking order-dependent bugs.

### 8.5.3 Plugin interface testing

Test plugins in isolation against a mock host and test the host against mock plugins. Specifically, test version negotiation: load a plugin that reports a version lower than the minimum, higher than the maximum, and exactly at each supported boundary. Test that the host gracefully refuses plugins with incompatible versions rather than calling through invalid function pointers.

### 8.5.4 Sanitizers and dynamic loading

AddressSanitizer and UndefinedBehaviorSanitizer work across `dlopen` boundaries, but only if both the host and the plugin are compiled with sanitizer instrumentation. A sanitized host loading an unsanitized plugin (or vice versa) can produce false positives or miss real violations. For plugin architectures, either sanitize everything or test the sanitized path separately from the production path.

---

## 8.6 Review Checklist

Use this checklist when reviewing code that defines, modifies, or consumes a library boundary.

### 8.6.1 Binary boundary design

- [ ] No `std::` container or string types appear in the exported function signatures of shared libraries, unless all consumers are guaranteed to use the identical standard library version and configuration.
- [ ] Opaque handle types or Pimpl are used for types whose layout may change between releases.
- [ ] Allocation and deallocation of opaque types happen on the same side of the boundary (inside the library).
- [ ] Symbol visibility is set to hidden by default; only intended exports are marked visible.
- [ ] Inline functions in public headers do not depend on private data members or internal types that may change.

### 8.6.2 Versioning

- [ ] The library exposes a runtime-queryable version (function or embedded metadata).
- [ ] ABI version (SONAME or equivalent) is tracked independently of source-level semver.
- [ ] Inline namespaces are used to mangle version into symbol names when ABI breaks are possible.
- [ ] Build metadata (compiler, standard library, flags) is embedded and queryable for diagnostic purposes.

### 8.6.3 Plugin interfaces

- [ ] Plugin API uses C linkage and fixed-layout types only.
- [ ] The API struct contains a version field as its first member.
- [ ] New functionality is added by appending to the struct, never by reordering or removing entries.
- [ ] The host checks the plugin's reported version before calling any function pointer.
- [ ] Error handling at the plugin boundary does not rely on exceptions crossing the `dlopen`/`LoadLibrary` boundary.

### 8.6.4 Modules

- [ ] Module interfaces do not export types whose layout must remain stable across separate compilations unless a Pimpl or opaque-handle pattern is used.
- [ ] Build system correctly scans module dependencies and rebuilds consumers when a module interface changes.
- [ ] Externally-distributed libraries ship headers as the primary interface; modules are provided as an optional consumer convenience, not the sole mechanism.
- [ ] Module partitions are used to keep individual interface files manageable; no single module interface unit exceeds a reasonable size.
- [ ] `import std;` usage is gated on verified compiler and build system support for the target toolchain matrix.
- [ ] Non-exported declarations in module interfaces are intentionally non-exported, not accidentally omitted from `export` blocks.
- [ ] Header units are used as a transitional mechanism where full module conversion is not yet practical; a migration timeline exists.
- [ ] Dual-mode (header + module) libraries have clear documentation on which consumption mode is primary and when header-only mode will be deprecated.
- [ ] BMI files are not committed to version control or shared across incompatible toolchain configurations in CI caches.

### 8.6.5 Testing and CI

- [ ] CI includes a separate-compilation test: consumer compiled against old headers, linked against new library binary.
- [ ] ABI diff tooling (e.g., `abidiff`) runs on every release candidate.
- [ ] Symbol export lists are checked for unintended additions.
- [ ] Plugin version negotiation is tested at all supported version boundaries.
- [ ] Sanitizer builds cover both sides of any dynamic-loading boundary.
