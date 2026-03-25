# 模块、库、打包与 ABI 现实

本章假定源码级接口已经设计得很好。现在的问题是，当代码在真实工具链之间被构建、分发、版本化和消费时，这个接口会表现成什么样。

## 生产问题

团队经常把模块、库和 ABI 当成一个话题来谈。它们有关联，但不能互换。

模块主要关乎源码组织、依赖卫生和构建可伸缩性。库打包关乎代码如何分发和链接：静态库、共享库、header-only 包、源码分发、内部 monorepo 组件，或者插件 SDK。ABI 关乎分别构建的二进制是否能在布局、调用约定、异常行为、分配所有权、符号命名和对象生命周期上达成一致。

把这些当成一个问题，会导致昂贵错误。某个团队采用 C++20 模块，并以为这就能稳定公共二进制边界。另一个团队发布了共享库，其公共头文件跨编译器暴露 `std::string`、`std::vector`、异常和大量内联模板，随后才发现“在我们的构建代理上能工作”并不是兼容性策略。一个插件宿主导出 C++ 类层级，最终太晚才意识到，编译器版本变化现在已经成了部署事件。

本章会把这些区别保持清晰。源码卫生很有价值。分发选择属于架构决策。ABI 稳定性则是一种契约：要么你有意设计并提供它，要么就不要提供。

## 模块解决的是源码问题，不是二进制问题

C++ 模块有助于降低解析成本、隔离宏并控制依赖。这些收益是真实的，尤其是在头文件负担沉重的大型代码库里。经过良好拆分的模块接口可以减少实现细节的意外暴露，也能让预期的 import 表面更加清晰。

但模块不会创造可移植的二进制契约。它们不会抹平编译器 ABI 差异。它们不保证不同厂商之间具有相同的布局规则、异常互操作性或标准库二进制兼容性。它们不能替代打包策略。

### 模块替代了什么：头文件包含模型及其风险

没有模块时，C++ 编译是文本包含。每个 `#include` 都会把头文件的完整文本粘贴进翻译单元。这会产生三类真实问题。

**包含顺序依赖。** 如果头文件 A 定义了头文件 B 会使用的宏或类型，交换 `#include` 顺序就可能悄悄改变行为，甚至破坏编译。这不是假想。大型代码库会积累没人文档化的隐式顺序契约。

```cpp
// order_matters.cpp
#include <windows.h>    // defines min/max as macros
#include <algorithm>     // std::min/std::max are now broken

auto x = std::min(1, 2); // compilation error or wrong overload
```

**宏污染。** 在每个传递包含头文件中定义的每个宏，都会在下方全部可见。一个会 `#define` `ERROR`、`OK`、`TRUE`、`CHECK` 或 `Status` 的库，可能会悄悄和无关代码冲突。经典防御手段（include guard、`#undef`、`NOMINMAX`）很脆弱，而且必须在每个包含点都记得使用。

```cpp
// some_vendor_lib.h
#define STATUS int
#define ERROR -1

// your_code.cpp
#include "some_vendor_lib.h"
#include "your_domain.h"  // any enum named ERROR or type named STATUS is now broken

enum class Status { ok, error };  // fails to compile: STATUS expands to int
```

**传递依赖爆炸。** 包含一个头文件就可能连带引入数百个其他头文件。对内部头文件一个看起来很小的改动，会触发成千上万个翻译单元重新编译。构建时间按总传递包含深度扩张，而不是按程序真实依赖图扩张。

模块同时解决这三个问题：它们不会泄漏宏，具有与顺序无关、定义良好的 import 语义，而且只导出显式声明的内容。即使它们不触碰二进制兼容性，这依然是对源码卫生的实质性改进。

这意味着，首先要做的决定不是“我们要不要用模块？”，而是“我们在向消费者承诺什么？”

如果答案是在一个仓库里、基于一个工具链基线进行内部源码复用，那么模块可能非常合适。如果答案是“我们要交付一个由未知构建系统和编译器版本消费的公共 SDK”，那么模块仍然可能帮助你自己的构建，但它们并不能消除对严格二进制边界纪律的需求。

## 打包选择表达运维意图

打包是架构与部署相遇的地方。

### Header-only 或源码分发库

这类方式避开了许多 ABI 承诺，因为消费者会把代码编译进自己的程序。代价是编译时间、更大的依赖表面，以及更多实现细节暴露。模板、concept 和内联函数天然适合这里。对内部泛型工具，或那些性能与优化器可见性比简化分发更重要的狭窄公共库来说，这通常是不错的选择。

### 静态库

静态链接简化了部署，也避开了一些运行时兼容性问题。如果公共接口设计草率，它仍然会制造 ODR 和分配器边界问题，但通常能降低跨版本运维复杂度。静态库很适合那些作为一个整体部署的内部组件，或偏好自包含二进制的消费者。

### 共享库与 SDK

它们带来部署和补丁方面的优势，但此时你就真正拥有了一个二进制边界。这意味着符号可见性、版本策略、异常规则、分配器所有权和数据布局，都不再只是私有工程选择。它们是产品行为的一部分。

### 插件边界

这是最苛刻的情况，因为宿主和插件可能分别构建、动态加载、独立升级，而且有时还会用不同标志或不同编译器编译。在这里，最安全的公共边界往往是 C ABI，再加上不透明句柄和显式函数表，即便内部实现从头到尾都使用现代 C++。

打包决策应当来自运维约束，而不是来自本地代码里什么看起来更优雅。

## 内部库与公共二进制契约

很多库根本不需要稳定 ABI。这很正常。

如果生产者和消费者会用相同 commit 和工具链一起重建，那么源码兼容性远比 ABI 稳定性重要。在这种环境里，现代 C++ API 可以很有表达力。返回词汇类型、使用模板、采用模块、依赖内联，都可能是合理权衡。

一旦你需要可独立升级的二进制，约束就变了。此时，即使是看起来无害的公共类型也会变成负担。私有成员顺序变化、不同标准库实现、不同编译器，或者不同异常模型，都可能在源码级签名完全不变的情况下破坏消费者。

不要只是因为发布过一次 DLL，就意外承诺稳定 ABI。

## ABI 稳定性需要有意收窄

稳定 ABI 之所以昂贵，是因为它禁止你在边界上使用许多方便的语言习惯。

这些是 ABI 脆弱性的常见来源：

1. 在公共二进制接口里暴露标准库类型。
2. 暴露大小或成员可能变化的类布局。
3. 让异常跨编译器或运行时边界传播。
4. 没有共享分配器契约时，在一侧分配、另一侧释放。
5. 把大量内联模板或虚层级作为二进制扩展机制导出。

这并不意味着标准 C++ 库类型不好。它意味着，在公共二进制边界上，它们往往是错误选择。

### 具体的 ABI 破坏场景

这些都是真实场景，不是理论风险。

**新增私有成员会改变类大小。** 某个消费者针对库的 v1 版本编译，并按照自己编译时看到的 `sizeof(Widget)` 来分配对象。如果 v2 新增了一个私有成员，库的方法现在就会写越过消费者分配的空间。结果是静默内存破坏，而不是链接错误。

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

**不同的标准库实现。** 一个用 libstdc++ 构建的共享库在其 API 中暴露 `std::string`。一个用 libc++ 构建的消费者链接它。这两个实现的内部布局不同（SSO buffer 大小、指针排布等）。跨这个边界调用会破坏字符串状态。既不会有编译期诊断，也不会有链接期诊断。

**编译器标志不匹配。** 用 `-fno-exceptions` 构建库、而消费者开启异常，可能产生不兼容的栈展开行为。使用不同的 `-std=` 标志可能改变标准类型的布局。使用不同的 struct packing 或对齐标志，也会悄无声息地改变 ABI。

### Header-only 库引发的 ODR 违规

Header-only 库之所以流行，是因为它们避开了二进制分发复杂度。但它们引入了另一类问题：单一定义规则违规。

如果两个翻译单元都包含同一个 header-only 库，却使用了不同的标志、预处理器定义，或者会影响内联函数行为的模板实参来编译，链接器就可能悄悄选取其中一个定义并丢弃另一个。程序把基于两套不同假设编译出来的代码链接进了同一个二进制。

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

这不是刻意构造的场景。那些用 `#ifdef` 选择代码路径，或者根据 `NDEBUG`、`_DEBUG` 或平台宏表现不同的库，在任何混合编译设置的项目里都可能产生 ODR 违规。Sanitizer（特别是带 ODR 违规检测的 `-fsanitize=undefined`）和像 `ld` 的 `--detect-odr-violations` 这样的链接期工具能抓到其中一部分，但抓不全。

对于稳定的共享库或插件契约，应优先使用不透明句柄、狭窄的 C 风格值类型、显式所有权函数、带版本的结构体以及清晰的生命周期规则。内部则应积极使用现代 C++。在边界上，要保守，因为消费者要为你的二进制歧义买单。

## 反模式：公共二进制表面映照内部 C++ 类型

```cpp
// Anti-pattern: fragile ABI surface for a shared library.
class EXPORT Session {
public:
	virtual std::string send(const std::string& request) = 0;
	virtual ~Session() = default;
};

std::unique_ptr<Session> create_session();
```

这个接口在单个构建内部很诱人。作为公共 SDK 边界时，它风险很高。

`std::string` 的表示形式和分配器行为都是实现细节。`std::unique_ptr` 则内嵌了 deleter 和运行时假设。跨边界的虚派发把宿主和消费者都绑定到兼容的对象模型细节上。异常也可能泄漏出去，除非被文档化并受控。这个接口实际上已经把你的编译器、标准库和构建标志变成了契约的一部分。

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

这不那么漂亮，却诚实得多。这个边界显式命名了分配所有权、版本表面和错误传输。内部实现仍然可以使用 `std::expected`、`std::pmr`、协程、模块，以及任何被隔在墙后的 C++23 技术。

## Pimpl 权衡依然存在

对处于同一工具链家族内的 C++ 消费者来说，pimpl 模式仍有位置。它可以减少重建扇出、隐藏私有成员，并在某些实现变化下保持类大小稳定。但它也会增加间接性、分配和复杂度。Pimpl 不是免费的现代化徽章。

只有当以下条件都成立时才使用它：

1. 你需要隐藏表示或降低编译期暴露。
2. 对象不处于那种多一次指针追逐就能测出问题的热路径上。
3. 库确实会从保持类布局稳定中受益。

不要只是因为头文件很乱就去用 pimpl。对于内部构建，模块可能更适合解决这个源码问题。Pimpl 首先是表示与兼容性工具，不是风格要求。

## 真实构建系统中的模块

面向 C++23 的建议必须保持现实。模块很有价值，但在不同工具链、包管理器和混合语言构建系统之间，其运维成熟度仍不均衡。

在受控构建环境中，使用 GCC 14+、Clang 18+ 或 MSVC 17.10+ 时，模块可以降低解析开销，并让依赖意图更清楚。在异构环境中，模块工件模型、构建图集成和包管理器支持仍然可能带来摩擦。这种摩擦不是反对模块的理由。它只是提醒你，采用模块属于构建架构问题，而不只是语言热情问题。

一个务实的默认做法是：

1. 对于一起构建的内部组件，优先使用模块。
2. 除非你的消费者生态是受控的，否则不要让公共包消费依赖模块支持。
3. 把二进制契约决策与模块决策分开。

## 版本策略是接口的一部分

没有版本策略的打包，只是一厢情愿。消费者需要知道，在发布之间允许哪种变更：只保证源码兼容、在主版本内保证 ABI 兼容，还是除了精确构建匹配之外不作任何承诺。

这个策略会影响技术设计。如果主版本内的 ABI 兼容性很重要，你的公共类型就必须被强力收窄，而且发布流程中必须包含 ABI 评审。如果消费者会从源码重建，这个策略就可以更宽松，接口也可以更符合惯用法。

版本化不只是语义版本号。它还包括在可用处做符号版本化、为源码级 API 设计 inline namespace 策略、做特性检测，以及提供能正确描述编译器和运行时要求的包元数据。

## 跨边界的内存、异常与所有权

大多数跨库故障并不华丽。它们来自所有权不匹配。

如果一侧分配内存、另一侧释放，就必须显式说明分配器契约。如果允许异常跨越边界，运行时和编译器假设就必须对齐。如果边界使用回调，就必须文档化保留规则和线程亲和规则。如果后台工作在卸载期间仍继续运行，那么打包设计本身就已经不安全。

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

修复方式是绝不让分配和释放跨越边界。负责分配的库，也必须提供释放函数；或者边界必须使用调用方提供的 buffer。

```cpp
// Safe: library owns both allocation and deallocation.
EXPORT char* get_name();
EXPORT void free_name(char* name);

// Also safe: caller provides the buffer.
EXPORT status_code get_name(char* buffer, std::size_t buffer_size);
```

正是这些细节，决定了一个在本地看起来干净的接口，是否能成为一个可运作的库。

## 验证与评审问题

要把打包和 ABI 与源码级 API 质量分开评审。

1. 这个库是用于同构建消费、源码消费，还是用于可独立升级的二进制消费？
2. 我们是否在使用模块改善源码卫生，却错误地以为它们解决了 ABI？
3. 公共边界是否暴露了那些布局或运行时行为实际上并不由我们控制的类型？
4. 边界上的分配所有权是否显式？
5. 版本化和兼容性承诺是否被文档化，并且可测试？
6. 对这个插件或 SDK 而言，C ABI 加不透明句柄是否比导出的 C++ 类更安全？

验证应包括跨受支持编译器与标准库的构建矩阵测试、符号可见性检查、在相关场景下使用 ABI 对比工具，以及模拟真实消费者集成的打包测试。生产者仓库内部的一套单元测试，并不足以证明一个公共二进制契约。

## 要点

模块、打包和 ABI 是三条不同的设计轴线。

使用模块改善源码边界和构建可伸缩性。根据部署与消费者约束来选择打包方式。只有在你愿意收窄公共边界并持续验证时，才承诺稳定 ABI。在实现内部，尽可以自由使用现代 C++23。在真正的二进制边界上，优先显式所有权、显式版本化和保守的表面面积。

库设计中最尖锐的错误，就是把内部的优雅导出成公共二进制策略。源码级的漂亮，并不能让 ABI 风险消失。只有有意设计的边界，才能做到。
