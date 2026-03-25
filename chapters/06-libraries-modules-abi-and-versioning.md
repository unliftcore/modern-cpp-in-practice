# Chapter 6: Libraries, Modules, ABI, and Versioning

*Prerequisites: Chapter 5 (API shape determines what crosses a binary boundary).*

> **Prerequisites:** You should already understand the distinction between interface and implementation, dependency direction, and why an API's surface area is a long-term liability. Chapter 5 covered these at the source level. This chapter moves into the territory where source-level reasoning is no longer sufficient: shared libraries, binary compatibility, separate compilation, mixed toolchains, and the versioning contracts that hold a deployed system together across independent release cadences.
>
> You will also benefit from familiarity with your platform's linker model (ELF symbol visibility on Linux, dllexport/dllimport on Windows) and a working knowledge of how translation units become object files and then libraries. The chapter does not teach linking from scratch; it focuses on the decisions that go wrong when linking is treated as someone else's problem.

---

## 6.1 The Production Problem

A team ships a shared library. Internal consumers link against it. Six months later, a field is added to a widely-used struct, a virtual function is inserted into a base class, and a `std::string` parameter changes from libstdc++ to libc++. None of these changes produce a compiler error at the consumer's call site — the headers still parse, the symbols still resolve. The failures arrive at runtime: memory corruption, wrong-vtable dispatch, silent data truncation, crashes deep inside the allocator.

The root cause is not a code defect in the traditional sense. It is an **ABI break** — a change to the binary layout or calling convention of a type that crosses a compilation boundary. The compiler cannot detect it because the consumer was compiled against the old headers. The linker cannot detect it because the mangled symbol names have not changed. Only the runtime discovers the mismatch, and its diagnostics are typically a segfault or heap corruption.

This class of failure scales with the number of independently-compiled consumers, the frequency of library releases, and the degree to which teams rely on implicit binary compatibility. In organizations with dozens of services consuming the same core library, a careless struct change can trigger a cascade of production incidents that take days to diagnose and weeks to fully remediate.

C++20 modules add a separate axis of complexity: they change how declarations reach consumers, how build systems discover dependencies, and what the compiler can optimize across translation unit boundaries. Modules solve real problems — header pollution, macro leakage, redundant parsing — but they do not solve ABI stability, and their interaction with shared libraries and versioning requires deliberate design.

---

## 6.2 The Legacy Approach

### 6.2.1 Headers-everywhere, hope-for-the-best

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

## 6.3 The Modern Approach

Solving library packaging, ABI, and versioning in production C++ requires working at three distinct layers: the **source boundary** (what consumers see at compile time), the **binary boundary** (what survives separate compilation), and the **version contract** (what guarantees persist across releases). Conflating these layers is where most designs fail.

### 6.3.1 Separating the ABI Surface from the API Surface

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

### 6.3.2 The Pimpl Pattern as an ABI Firewall

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

### 6.3.3 Modules as an Organizational Tool

C++20 modules address a different set of problems than ABI stability: they eliminate header re-parsing, prevent macro leakage, and give library authors control over which declarations are exported.

```cpp
// mylib.cppm — module interface unit
export module mylib;

import <cstdint>;
import <string_view>;

export namespace mylib {

// This struct is exported at the source level.
// It is NOT automatically ABI-stable across shared library boundaries.
struct Config {
    int32_t max_connections = 128;
    int32_t timeout_ms = 5000;
};

export class Engine {
public:
    explicit Engine(Config cfg);
    ~Engine();

    void run();

private:
    struct Impl;
    Impl* impl_;  // Pimpl still needed for ABI isolation
};

} // namespace mylib
```

Key points about modules and ABI:

**Modules do not provide ABI stability.** A module interface unit is still compiled into object code with the same layout rules as a header. If you change a struct's fields in a module interface and recompile the library but not its consumers, you get the same binary mismatch as with headers.

**Modules do provide build isolation.** Importing a module does not expose its non-exported declarations, internal `#include` chains, or macros. This narrows the effective dependency surface, which indirectly reduces the risk of accidental coupling — but it is a source-level benefit, not a binary-level one.

**Modules do not remove template instantiation cost.** An exported template still has to be instantiated in consumer code unless you pair the module boundary with explicit instantiation or another compilation firewall. Modules solve repeated parsing and macro leakage; they do not make template-heavy APIs free.

**Module support in build systems is still maturing.** As of 2025, CMake (3.28+) supports C++20 modules with Ninja and MSVC, and partial support exists for GCC and Clang. Production teams should verify that their specific toolchain combination handles module dependency scanning correctly before adopting modules for widely-consumed libraries. The risk is not in the language feature but in the build infrastructure around it.

**Practical guidance:** use modules for internal libraries where you control the full build graph. For externally-distributed libraries, continue to ship headers alongside a stable ABI surface (C or Pimpl-based) until module support stabilizes across all target toolchains.

### 6.3.4 Plugin Interfaces and Dynamic Loading

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

### 6.3.5 Versioning Strategy

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

## 6.4 Tradeoffs and Boundaries

### 6.4.1 C boundary vs. C++ boundary

A C-linkage boundary maximizes compatibility at the cost of expressiveness. You lose overloading, templates, RAII at the boundary, and namespacing. The C++ wrapper pattern (Section 3.1) recovers most of this on the consumer side, but the library author must maintain two layers: the stable C ABI and the internal C++ implementation.

The alternative — exporting C++ classes directly — works when you control the full toolchain and rebuild everything together. Many organizations do this for internal libraries and accept the coupling. The decision hinges on deployment topology: if the library and its consumers always ship as a unit, C++ export is viable. If they can be updated independently, a C or Pimpl boundary is necessary.

### 6.4.2 Pimpl overhead

The Pimpl indirection (one pointer dereference per method call, one heap allocation per object) is rarely measurable in application-level code. It becomes relevant for types that are allocated in millions and accessed in tight loops — and for those types, you probably should not be exposing them across a shared library boundary at all. The types that cross binary boundaries should be session-lived, not per-element-lived.

### 6.4.3 Modules vs. headers for distributed libraries

Modules provide cleaner dependency hygiene and faster builds for consumers who use a compatible toolchain. But they impose a toolchain constraint: the consumer must use a compiler that can consume your module's BMI (binary module interface) format, which is not standardized and not portable across compiler vendors or even major versions of the same compiler.

For libraries distributed as source or as binary packages to unknown consumers, headers remain the pragmatic choice. For monorepo-internal libraries where the toolchain is controlled, modules offer real build-time savings.

### 6.4.4 Inline namespaces for version management

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

### 6.4.5 The hidden cost of `std::` types at boundaries

`std::string`, `std::vector`, `std::function`, and other standard library types have **implementation-defined layout**. Their size, alignment, and internal pointer structure differ across:

- Standard library implementations (libstdc++ vs. libc++ vs. MSVC STL)
- Major versions of the same implementation (libstdc++ pre/post COW `std::string`)
- Debug vs. release configurations (MSVC iterator debugging changes container layout)
- Different `-D_GLIBCXX_USE_CXX11_ABI` settings

Passing these types across a shared library boundary is safe only when both sides are compiled with the identical standard library version and configuration. In a controlled monorepo with uniform toolchain enforcement, this holds. In any other deployment model, it is a latent defect.

---

## 6.5 Testing and Tooling Implications

### 6.5.1 ABI compatibility testing

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

### 6.5.2 Module build testing

Module dependency scanning failures are subtle and often manifest as stale BMI files or missing imports. Test configurations should include:

- Clean builds (no cached BMIs) in CI to catch implicit dependency ordering bugs.
- Incremental builds after touching a module interface unit to verify that all dependent translation units are rebuilt.
- Cross-compiler testing if consumers may use different toolchains.

### 6.5.3 Plugin interface testing

Test plugins in isolation against a mock host and test the host against mock plugins. Specifically, test version negotiation: load a plugin that reports a version lower than the minimum, higher than the maximum, and exactly at each supported boundary. Test that the host gracefully refuses plugins with incompatible versions rather than calling through invalid function pointers.

### 6.5.4 Sanitizers and dynamic loading

AddressSanitizer and UndefinedBehaviorSanitizer work across `dlopen` boundaries, but only if both the host and the plugin are compiled with sanitizer instrumentation. A sanitized host loading an unsanitized plugin (or vice versa) can produce false positives or miss real violations. For plugin architectures, either sanitize everything or test the sanitized path separately from the production path.

---

## 6.6 Review Checklist

Use this checklist when reviewing code that defines, modifies, or consumes a library boundary.

### 6.6.1 Binary boundary design

- [ ] No `std::` container or string types appear in the exported function signatures of shared libraries, unless all consumers are guaranteed to use the identical standard library version and configuration.
- [ ] Opaque handle types or Pimpl are used for types whose layout may change between releases.
- [ ] Allocation and deallocation of opaque types happen on the same side of the boundary (inside the library).
- [ ] Symbol visibility is set to hidden by default; only intended exports are marked visible.
- [ ] Inline functions in public headers do not depend on private data members or internal types that may change.

### 6.6.2 Versioning

- [ ] The library exposes a runtime-queryable version (function or embedded metadata).
- [ ] ABI version (SONAME or equivalent) is tracked independently of source-level semver.
- [ ] Inline namespaces are used to mangle version into symbol names when ABI breaks are possible.
- [ ] Build metadata (compiler, standard library, flags) is embedded and queryable for diagnostic purposes.

### 6.6.3 Plugin interfaces

- [ ] Plugin API uses C linkage and fixed-layout types only.
- [ ] The API struct contains a version field as its first member.
- [ ] New functionality is added by appending to the struct, never by reordering or removing entries.
- [ ] The host checks the plugin's reported version before calling any function pointer.
- [ ] Error handling at the plugin boundary does not rely on exceptions crossing the `dlopen`/`LoadLibrary` boundary.

### 6.6.4 Modules

- [ ] Module interfaces do not export types whose layout must remain stable across separate compilations unless a Pimpl or opaque-handle pattern is used.
- [ ] Build system correctly scans module dependencies and rebuilds consumers when a module interface changes.
- [ ] Externally-distributed libraries ship headers as the primary interface; modules are provided as an optional consumer convenience, not the sole mechanism.

### 6.6.5 Testing and CI

- [ ] CI includes a separate-compilation test: consumer compiled against old headers, linked against new library binary.
- [ ] ABI diff tooling (e.g., `abidiff`) runs on every release candidate.
- [ ] Symbol export lists are checked for unintended additions.
- [ ] Plugin version negotiation is tested at all supported version boundaries.
- [ ] Sanitizer builds cover both sides of any dynamic-loading boundary.
