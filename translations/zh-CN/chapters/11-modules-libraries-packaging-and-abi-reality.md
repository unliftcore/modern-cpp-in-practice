# 模块、库、打包与 ABI 现实

本章假定源码级接口的设计已经到位。接下来要讨论的是：代码在真实工具链中经历构建、分发、版本迭代和被其他项目使用时，这些接口会如何表现。

## 生产问题

团队经常把模块、库和 ABI 混为一谈。它们确实相关，但绝不是一回事。

模块主要解决源码组织、依赖治理和构建可扩展性问题。库的打包方式决定代码如何分发和链接——静态库、共享库、header-only 包、源码分发、内部 monorepo 组件，还是插件 SDK。ABI 则关系到独立构建的二进制文件，能否在内存布局、调用约定、异常行为、内存分配归属、符号命名和对象生命周期等方面取得一致。

把这三者混为一谈，代价高昂。有团队引入 C++20 模块后就以为公共二进制边界从此稳如磐石；有团队发布共享库时，在公共头文件里跨编译器暴露了 `std::string`、`std::vector`、异常和大量内联模板，结果发现”在我们的 CI 上能跑”根本不是兼容性策略；还有插件宿主直接导出 C++ 类继承体系，等到发现编译器版本一升级就是一次部署事故时，为时已晚。

本章会严格区分这三者。源码整洁性自有其价值，分发方式是架构层面的抉择，而 ABI 稳定性是一种契约——要么精心设计并主动提供，要么干脆不承诺。

## 模块解决的是源码问题，不是二进制问题

C++ 模块能降低解析开销、隔离宏、管控依赖关系。这些好处实实在在，对头文件负担沉重的大型代码库尤其明显。设计良好的模块接口能减少实现细节的意外泄露，让对外暴露的导入面更加清晰。

但模块并不能创造可移植的二进制契约，也无法抹平编译器之间的 ABI 差异。不同厂商之间的布局规则、异常互操作性、标准库二进制兼容性，模块一概不保证。模块替代不了打包策略。

### 模块替代了什么：头文件包含模型及其隐患

在没有模块的时代，C++ 的编译就是文本粘贴。每个 `#include` 都会把头文件全文逐字插入编译单元。这带来了三类切实存在的问题。

**包含顺序依赖。** 如果头文件 A 定义了头文件 B 所依赖的宏或类型，交换 `#include` 的顺序就可能悄然改变行为，甚至导致编译失败。这绝非假想——大型代码库中总会不知不觉积累起无人记录的隐式顺序依赖。

```cpp
// order_matters.cpp
#include <windows.h>    // defines min/max as macros
#include <algorithm>     // std::min/std::max are now broken

auto x = std::min(1, 2); // compilation error or wrong overload
```

**宏污染。** 所有被间接包含进来的头文件中定义的宏，对后续代码一律可见。某个库只要 `#define` 了 `ERROR`、`OK`、`TRUE`、`CHECK` 或 `Status`，就可能和完全无关的代码产生冲突。经典防御手段（include guard、`#undef`、`NOMINMAX`）既脆弱，又必须在每个包含点都记得使用。

```cpp
// some_vendor_lib.h
#define STATUS int
#define ERROR -1

// your_code.cpp
#include "some_vendor_lib.h"
#include "your_domain.h"  // any enum named ERROR or type named STATUS is now broken

enum class Status { ok, error };  // fails to compile: STATUS expands to int
```

**传递依赖爆炸。** 包含一个头文件，就可能连锁引入数百个其他头文件。内部头文件看似微小的一处改动，便能触发数千个编译单元的重新编译。构建时间取决于传递包含的总深度，而非程序的实际依赖图。

模块同时解决了这三个问题：不泄漏宏，import 语义与顺序无关且定义明确，只导出显式声明的内容。虽然不涉及二进制兼容性，但对源码整洁性的提升是实实在在的。

### 模块语法实践

`examples/web-api/` 示例项目由七个 C++20 模块接口单元构成。每个 `.cppm` 文件声明一个具名模块，显式导入其依赖，并只导出公共接口。以下是一个典型模块的结构：

```cpp
// examples/web-api/src/modules/error.cppm
module;

// 全局模块片段：尚未模块化的标准头文件在此包含
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>

export module webapi.error;   // 模块声明——为此模块命名

export namespace webapi {
    enum class ErrorCode : std::uint8_t { not_found, bad_request, conflict, internal_error };
    struct Error { /* ... */ };

    template <typename T>
    using Result = std::expected<T, Error>;
}  // 只有 'export' 内的内容对导入方可见
```

依赖其他模块的模块使用 `import` 声明，而非 `#include`：

```cpp
// examples/web-api/src/modules/handlers.cppm
module;
#include <format>
#include <string>
// ...

export module webapi.handlers;

import webapi.error;        // 类型化的错误模型
import webapi.http;         // Request, Response, Handler
import webapi.json;         // JsonSerializable concept
import webapi.repository;   // TaskRepository
import webapi.task;         // Task, TaskId
```

有几点值得注意。第一，全局模块片段（`module;` 和 `export module ...;` 之间的部分）是标准库头文件的归属。这些头文件早于模块系统，必须以文本方式包含。第二，每个 `import` 指定的是具体模块——不存在传递性包含。`handlers.cppm` 导入了 `webapi.error`，但不会连带拉入 `error.cppm` 本身包含的所有东西。第三，`export` 关键字精确地控制可见性：只有被导出的名字才能被导入方使用。私有辅助函数、内部实现细节和未导出的类型对外不可见。

使用方同样简洁。在 `main.cpp` 中，六条 import 声明取代了本该存在的一长串 `#include` 指令及其所有传递性依赖：

```cpp
// examples/web-api/src/main.cpp
import webapi.handlers;
import webapi.http;
import webapi.middleware;
import webapi.repository;
import webapi.router;
import webapi.task;
```

不需要 include guard，没有宏冲突，不受包含顺序影响。构建系统可以直接看到模块依赖图，并按正确的顺序编译各模块。这就是模块在源码层面带来的改善。

因此，首要问题不是”我们要不要用模块？”，而是”我们对使用者做出了什么承诺？”

如果答案是在同一个仓库内、基于同一套工具链做内部源码复用，模块可能是绝佳选择。如果答案是”我们要发布一个公共 SDK，供未知的构建系统和编译器版本使用”，模块仍然有助于改善你自己的构建流程，但它无法免除你在二进制边界上严格自律的责任。

## 打包选择表达运维意图

打包是架构决策落地为部署方案的环节。

### Header-only 或源码分发库

由于使用者自行将代码编译进自己的程序，这类方式规避了大部分 ABI 承诺。代价是更长的编译时间、更大的依赖面，以及更多的实现细节暴露。模板、concept 和内联函数天然适合这种形式。对于内部泛型工具库，或者那些对性能和优化器可见性要求高于分发便捷性的小型公共库，这通常是不错的选择。

### 静态库

静态链接简化部署，也回避了部分运行时兼容性问题。公共接口设计不当时，仍然会引发 ODR 和分配器边界问题，但总体上能降低跨版本运维的复杂度。静态库适合作为整体部署的内部组件，也适合偏好自包含二进制产物的使用者。

### 共享库与 SDK

共享库在部署和热补丁方面有优势，但代价是你从此拥有了一条真正的二进制边界。符号可见性、版本策略、异常规则、分配器归属、数据布局——这些都不再是内部工程决策，而是产品行为的一部分。

### 插件边界

这是约束最严苛的场景：宿主和插件可能分开构建、动态加载、独立升级，有时甚至使用不同的编译标志乃至不同的编译器。此时最安全的公共边界往往是 C ABI 配合不透明句柄和显式函数表——即使内部实现全程使用现代 C++ 也不例外。

打包方式应由运维约束驱动，而不是由"哪种写法在本地代码里更好看"来决定。

## 内部库与公共二进制契约

很多库根本不需要稳定 ABI。这很正常。

如果生产方和使用方始终基于同一 commit、同一工具链一起重建，那么源码兼容性远比 ABI 稳定性重要。在这种环境下，现代 C++ API 可以充分发挥表达力：返回词汇类型、使用模板、引入模块、依赖内联——都是合理的取舍。

一旦需要支持二进制的独立升级，游戏规则就变了。哪怕看起来人畜无害的公共类型也可能变成定时炸弹——私有成员顺序调整、换了标准库实现、换了编译器，甚至仅仅是异常模型不同，都可能在源码级签名毫无变化的情况下，让使用方的程序崩溃。

别因为发布过一次 DLL，就无意间背上了稳定 ABI 的承诺。

## ABI 稳定性需要有意收窄

维持稳定 ABI 的代价很高，因为它迫使你在边界处放弃许多便利的语言用法。

这些是 ABI 脆弱性的常见来源：

1. 在公共二进制接口里暴露标准库类型。
2. 暴露大小或成员可能变化的类布局。
3. 让异常跨编译器或运行时边界传播。
4. 没有共享分配器契约时，在一侧分配、另一侧释放。
5. 把大量内联模板或虚层级作为二进制扩展机制导出。

这并不是说标准库类型本身有问题，而是说把它们放在公共二进制边界上，往往是错误的选择。

### 具体的 ABI 破坏场景

这些都是真实场景，不是理论风险。

**新增私有成员导致类大小变化。** 使用方基于库 v1 编译，按当时的 `sizeof(Widget)` 分配对象。v2 新增了一个私有成员后，库的方法就会写越使用方分配的空间。结果不是链接报错，而是静默的内存踩踏。

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

**标准库实现不同。** 共享库用 libstdc++ 构建，API 中暴露了 `std::string`；使用方用 libc++ 构建后链接该库。两套标准库的 `std::string` 内部布局完全不同（SSO buffer 大小、指针排列等）。跨边界调用会破坏字符串状态，而编译期和链接期都不会有任何诊断信息。

**编译标志不匹配。** 库用 `-fno-exceptions` 构建，使用方却开启了异常，二者的栈展开行为可能不兼容。`-std=` 标志不同可能改变标准类型的布局。struct packing 或对齐标志不同，同样会悄无声息地破坏 ABI。

### Header-only 库引发的 ODR 违规

Header-only 库之所以流行，正是因为免去了二进制分发的麻烦。但它们会引入另一类问题：单一定义规则（ODR）违规。

如果两个编译单元包含了同一个 header-only 库，但使用的编译标志、预处理器定义或模板实参不同，从而导致内联函数行为各异，链接器就可能悄悄选取其中一个定义、丢弃另一个。最终，基于两套不同假设编译出的代码被链接进了同一个二进制文件。

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

这并非人为构造的极端案例。凡是用 `#ifdef` 选择代码路径、或根据 `NDEBUG`、`_DEBUG`、平台宏而表现各异的库，在编译设置不统一的项目中都可能触发 ODR 违规。Sanitizer（尤其是带 ODR 违规检测的 `-fsanitize=undefined`）和 `ld` 的 `--detect-odr-violations` 等链接期工具能捕获一部分，但无法全覆盖。

对于需要稳定的共享库或插件契约，应优先采用不透明句柄、精简的 C 风格值类型、显式所有权函数、带版本号的结构体，以及清晰的生命周期规则。内部尽管放手使用现代 C++；但在边界处必须保守——二进制接口上的含糊不清，最终要由使用方来承担后果。

## 反模式：公共二进制接口照搬内部 C++ 类型

```cpp
// Anti-pattern: fragile ABI surface for a shared library.
class EXPORT Session {
public:
	virtual std::string send(const std::string& request) = 0;
	virtual ~Session() = default;
};

std::unique_ptr<Session> create_session();
```

在同一个构建内部，这个接口看起来很有吸引力。但一旦作为公共 SDK 的边界，风险就很高了。

`std::string` 的内部表示和分配器行为都是实现细节；`std::unique_ptr` 内嵌了 deleter 和运行时假设；跨边界的虚派发把宿主和使用方都绑定在对象模型的兼容性细节上；异常也可能泄漏出去，除非明确记录并加以控制。实际上，这个接口已经把你所用的编译器、标准库和编译标志都变成了契约的一部分。

对于真正的跨二进制边界，带版本的 C ABI 往往更安全。

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

写法上不那么漂亮，但诚实得多。这种边界把内存分配归属、版本化接口面和错误传递方式全都摆到了明面上。墙的另一边，内部实现照样可以用 `std::expected`、`std::pmr`、协程、模块，以及任何 C++23 技术。

## Pimpl 权衡依然存在

对于使用同一工具链的 C++ 使用方，pimpl 模式仍有用武之地。它能减少重建波及范围、隐藏私有成员，并在部分实现变更时保持类大小不变。但它也带来了额外的间接寻址、堆分配和代码复杂度。Pimpl 不是免费的"现代化勋章"。

只在以下条件同时成立时才考虑使用：

1. 确实需要隐藏内部表示，或减少头文件暴露的编译期依赖。
2. 对象不在热路径上——多一次指针间接寻址不会成为可测量的瓶颈。
3. 库确实需要在实现演进过程中保持类布局稳定。

不要仅仅因为头文件凌乱就搬出 pimpl。内部构建中，模块可能更适合解决这类源码组织问题。Pimpl 本质上是控制表示和兼容性的工具，不是代码风格的要求。

## 真实构建系统中的模块

以 C++23 为基础的建议必须立足现实。模块确实有价值，但各工具链、包管理器和混合语言构建系统对模块的支持成熟度参差不齐。

在可控的构建环境中（GCC 14+、Clang 18+ 或 MSVC 17.10+），模块能有效降低解析开销，让依赖意图更加明晰。但在异构环境中，模块产物模型、构建图集成和包管理器支持仍可能带来阻力。这些阻力不构成拒绝模块的理由，只是在提醒我们：采用模块是构建架构层面的决策，不能仅凭对新语言特性的热情。

务实的默认策略是：

1. 内部组件统一构建时，优先使用模块。
2. 除非使用方生态可控，否则不要让公共包的使用依赖于模块支持。
3. 二进制契约的决策与模块的决策要分开考虑。

`examples/web-api/` 项目正是这种务实策略的体现。它的七个 `.cppm` 文件（`error`、`task`、`json`、`http`、`repository`、`handlers`、`middleware`、`router`）构成了一张清晰的模块依赖图，统一由一份 CMakeLists 构建。标准库头文件仍然放在全局模块片段中，因为并非所有工具链都已将其模块化。项目没有试图把自己的模块作为公共包导出——它只是用模块来组织自己的源码。这才是正确的起步方式。

## 版本策略是接口的一部分

打包而不定义版本策略，无异于自欺欺人。使用方需要知道两个版本之间允许哪种程度的变更：仅保证源码兼容？在同一主版本内保证 ABI 兼容？还是除了精确的构建匹配，什么都不承诺？

版本策略直接影响技术设计。如果要在同一主版本内保持 ABI 兼容，公共类型就必须大幅精简，发布流程中也必须加入 ABI 审查环节。如果使用方总是从源码重新构建，策略可以放宽，接口也可以更加地道。

版本化远不止语义版本号那么简单，还涉及符号版本化（在平台支持的情况下）、源码级 API 的 inline namespace 策略、特性检测机制、废弃窗口期，以及能准确描述编译器和运行时要求的包元数据。

## 跨边界的内存、异常与所有权

大多数跨库故障并没有什么戏剧性，根源往往就是所有权不匹配。

一侧分配、另一侧释放？必须明确约定分配器契约。异常允许跨越边界？运行时和编译器的假设必须一致。边界处使用回调？必须写明回调的持有规则和线程亲和性。卸载时后台任务仍在运行？那打包设计本身就已经埋下了隐患。

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

解决办法是绝不让分配和释放跨越边界。谁分配谁释放——库负责分配，就必须同时提供释放函数；或者改用调用方预先提供的 buffer。

```cpp
// Safe: library owns both allocation and deallocation.
EXPORT char* get_name();
EXPORT void free_name(char* name);

// Also safe: caller provides the buffer.
EXPORT status_code get_name(char* buffer, std::size_t buffer_size);
```

正是这些细节，决定了一个在本地写得干净利落的接口，能否真正成为可靠运行的库。

## 验证与评审问题

打包和 ABI 问题应当与源码级 API 质量分开评审。

1. 这个库的使用场景是什么——与生产方一同构建、源码级引用，还是作为可独立升级的二进制发布？
2. 我们是否在用模块改善源码整洁性的同时，错误地以为它们也解决了 ABI 问题？
3. 公共边界上是否暴露了我们实际上无法控制其布局或运行时行为的类型？
4. 跨边界的内存分配归属是否已显式约定？
5. 版本化策略和兼容性承诺是否已文档化，并且可以通过测试验证？
6. 对于这个插件或 SDK，C ABI 加不透明句柄是否比导出 C++ 类更安全？

验证工作应包括：在所有支持的编译器和标准库组合上进行构建矩阵测试、符号可见性检查、在必要场景下使用 ABI 比对工具，以及模拟真实使用方集成流程的打包测试。仅靠生产方仓库中的单元测试，不足以证明一份公共二进制契约的可靠性。

## 要点

模块、打包和 ABI 是三个独立的设计维度。

用模块来改善源码边界和构建可扩展性。根据部署需求和使用方约束选择打包方式。只有在你愿意大幅收窄公共边界、并持续加以验证的前提下，才承诺稳定 ABI。实现内部尽管放心使用 C++23 的各种现代特性；而在真正的二进制边界上，坚持显式所有权、显式版本化和尽可能小的接口面。

库设计中最致命的错误，就是把内部的优雅原封不动地导出为公共二进制策略。源码写得再漂亮，也无法让 ABI 风险自动消失——只有刻意设计的边界才能做到。
