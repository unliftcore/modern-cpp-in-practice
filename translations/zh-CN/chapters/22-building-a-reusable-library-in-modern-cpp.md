# 在现代 C++ 中构建可复用库

应用代码在相当长一段时间里，都还能靠局部便利混过去。库代码不行。一旦 API 脱离最初那一小撮调用者，每一条模糊的所有权契约、每一个重载的错误通道、每一次意外分配，以及每一种不稳定的类型依赖，都会变成别人的问题。库也许仍然能编译，但它会变得不值得信任。

因此，本章的生产问题不是“我该怎么写一个漂亮 API？”，而是“一个可复用的现代 C++23 库，必须把哪些东西显式说清，才能让其他团队在采用它时，不会一并继承隐藏耦合、不稳定行为或无法验证的声明？”答案从范围开始。一个好的可复用库会做出一个狭窄承诺，用能在真实使用中成立的类型与契约表达这个承诺，并且拒绝把自己的实现经济学泄漏进整张依赖图。

这里的示例形态，是一个被多个服务和命令行工具使用的解析与转换库。这是个很合适的领域，因为它把大部分难点压力都带进来了：输入边界、分配行为、诊断、性能预期、可扩展性，以及打包。

## 从选择一个狭窄承诺开始

很多糟糕的库，在第一行公开 API 写出来之前就已经失败了。它们被设计成“所有与 X 相关内容的共享场所”。这通常意味着它们会积累多个职责、为无关问题长出扩展点，并且因为任何改动都会碰到某些调用者，而变得难以做版本演进。

可复用库应当只做出一个狭窄承诺。解析并验证这种格式。归一化这些记录。暴露这个存储抽象。计算这些派生值。这个承诺可以很重要，但它仍应当有一个明确中心。

这听起来显而易见，但它会立刻改变接口设计。狭窄承诺会带来一小套公开类型、一小组失败类别，以及少量真正需要由调用者自定义的地方。模糊承诺则会把复杂度往外推，变成模板、回调丛林、配置映射，以及读起来像谈判停火协议的文档。

因此，库设计中最重要的决定，不是要不要用模块、概念或 `std::expected`。而是公开契约在什么地方结束。

## 公开类型应直接编码所有权与不变量

调用者不应依赖仓库上下文，才能回答关于所有权、生命周期、可变性或有效性的基本问题。如果库返回一个视图，调用者应当知道底层数据由谁拥有。如果它接收一个回调，回调的生命周期与线程预期应当是明显的。如果一个配置对象可能无效，那么这种无效状态的可表示时间应仅限于验证前那一小段。

值类型往往是库 API 最合适的重心，因为它们跨团队、跨测试传播起来最稳。它们让复制成本可见、让 move 语义成为有意的选择，也让不变量能够附着在构造或验证边界上。像 `std::string_view` 和 `std::span` 这样的借用型输入，在调用边界上仍然很好用；但只有当库能在借用生命周期内完成工作，或能复制自己必须保留的数据时，才应当使用它们。

### 有意保留为局部片段：面向调用者、契约显式的 API

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

这段摘录做了几件有用的事。它把调用者可控策略与输入字节分离开来。它让分配策略可见，却不强迫全局采用同一个分配器策略。它返回的是领域错误，而不是某个传输层特有或解析器内部的类型。它还通过 `[[nodiscard]]` 和 `std::expected` 让忽略结果这件事变得更困难。

代价在于，这个函数签名看起来不如 `document parse(std::string_view)` 那样极简。这没关系。可复用库的价值，不在于它在演示文稿里看起来有多紧凑，而在于它能否让成本与契约变得可读。

## 让失败形态保持稳定

应用代码有时还能容忍异常和内部错误类别四处漂移，因为两边都由同一个团队控制。库则应严格得多。调用者必须知道哪些失败是契约的一部分、哪些是编程错误、哪些仍然只是实现上的偶发产物。

这通常会导向三种设计之一。

1. 对调用者本来就需要处理的常规领域失败，使用 `std::expected` 或类似的结果类型。
2. 把异常保留给不变量破坏、异常环境故障，或其生态本来就默认以异常使用的 API。
3. 在边界处把底层错误转换成稳定的公开错误词汇表。

正确选择取决于领域。解析、验证和可恢复的业务规则失败，通常很适合 `std::expected`。集成进异常式应用框架的底层基础设施库，合理地使用异常也未尝不可。最重要的是一致性。一个库如果对某些可恢复失败返回 `std::expected`，对另外一些抛异常，而且从某个后端泄漏出 `std::system_error`、从另一个后端却没有，那它就是在逼迫调用者通过逆向实现细节来推断策略。

不要让公开错误表面过于细碎。调用者需要的是自己能据以行动的区分。他们很少会从二十种只有库内部才能解释的解析器状态中受益。稳定类别应当保持精简，再通过独立通道提供可选的更丰富诊断。

## 分离机制与策略，但不要把一切都抽象掉

可复用库往往确实需要一些定制点：分配、日志钩子、宿主拥有的诊断、时钟源、I/O 适配器，或用户定义的处理器。团队就是在这里，要么把整个表面过度模板化，要么把库埋进运行时多态接口中，导致到处分配、到处派发。

更好的方式，是只选择少量显式策略接缝，并保持核心机制具体化。当定制点必须是零开销并且要接受编译期检查时，概念会很有帮助。当二进制边界、插件模型或运维解耦比模板透明性更重要时，类型擦除或回调接口会更合适。

例如，一个内部解析引擎通常没必要同时成为一个关于日志、分配、诊断和错误格式化的巨大策略模板。它完全可以用具体实现解析、接收一个 `std::pmr::memory_resource&`，并通过一个狭窄的 sink 接口可选地发出诊断。这让大多数调用点保持简单，同时仍然允许宿主控制那些昂贵或与环境密切相关的部分。

这里也要求库作者对依赖保持纪律。如果公开头文件为了支持可选功能，就包含了网络栈、格式化库、指标 SDK 和文件系统抽象，那么这个库在可移植性和构建卫生上就已经输了。可选的运维关切应当藏在狭窄接缝后面，或放进伴随适配器里，而不是放进 API 中心。

### 错误：在公开头文件中暴露内部类型

最常见的库设计失败之一，就是让实现类型泄漏进公开 API 表面。这会制造隐藏耦合：调用者会传递性依赖自己从未主动要求的头文件，构建时间变长，内部重构也会变成破坏性变更。

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

修正方式是让公开头文件保持最小化，把实现类型推到前向声明、PIMPL 或狭窄回调接口之后。

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

### 错误：糟糕的错误报告

把错误表示成裸整数、裸 `std::string` 消息，或平台特有的异常类型，会迫使调用者通过实现细节逆向失败语义。结果就是脆弱的错误处理：库一改内部实现，调用者的处理逻辑就跟着破裂。

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

如果需要比类别更丰富的诊断，请提供一个独立通道（诊断 sink、错误详情访问器，或结构化日志），而不是把调用者无法以程序方式处理的实现字段硬塞进主错误类型里。

## 版本与 ABI 需要策略，不是乐观

即使是内部共享库，把版本看成设计的一部分，也会比把它当成发布手续更有价值。实际问题是：库向调用者承诺了哪些类型的变更是他们能承受的。源码兼容性、ABI 兼容性、线协议稳定性、序列化数据稳定性和语义兼容性，彼此相关，但不是同一件事。

对许多 C++ 库来说，最诚实也最容易做到的策略，是在一个主版本内承诺源码兼容，而不对任意工具链之间的 ABI 做一揽子承诺。这往往比假装 ABI 稳定却在公开表面上暴露标准库类型、内联密集模板或平台相关布局，更接近真实世界。

如果 ABI 稳定性确实重要，设计本身就必须随之改变。这通常意味着更窄的导出表面、opaque 类型、类似 PIMPL 的边界、更严格的异常策略、更少的模板暴露，以及受控的编译器与标准库假设。这些都不是收尾工作，而是会影响整个 API 形态的基础决策。

### 错误：通过内联变更破坏 ABI

一个在源码层面看起来安全的改动，可能会悄无声息地破坏二进制兼容性。给类增加成员、修改内联函数的默认参数值，或者重排字段，都会改变 ABI，而编译器通常不会给出任何诊断。

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

对 ABI 稳定库来说，修正方式是把布局藏在 PIMPL 边界之后，让调用者永远不要依赖 `sizeof` 或字段偏移。

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

PIMPL 会为每个对象多带来一次堆分配，以及每次访问多一次间接层。对那些创建频率不高的类型（文档、连接、会话）来说，这几乎总是可以接受的成本。对那些在热循环中每秒创建数百万次的类型来说，它就不是了；这时就应重新评估这种类型是否真的值得追求 ABI 稳定。

### 版本化模式：用 inline namespace 做源码版本化

当库必须同时支持多个 API 版本（例如迁移期间）时，inline namespace 可以在不强迫调用者改代码的前提下，对符号进行版本化。

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

模块可以改善构建结构与分发卫生，但它们不会抹掉 ABI 现实。概念也可以改善诊断与约束，但它们不会自动让一个高度模板化的库变得易于版本化。应把这些工具视作局部改进，而不是策略替身。

## 文档应回答集成问题

面向有经验程序员的库文档，应聚焦于采用决策，而不是功能宣传。一个正在评估是否接入某个可复用库的调用者，需要知道：

- 库负责解决什么问题，又明确拒绝负责什么。
- 哪些输入是借用的，哪些输出拥有存储。
- 哪些失败是常规失败，以及它们如何被报告。
- 哪些分配、复制或后台工作属于正常使用成本。
- 是否提供线程安全保证；如果提供，保证到什么程度。
- 哪些版本与兼容性承诺是真实存在的。

简短、看起来像生产代码的示例在这里很有帮助，尤其是那些既展示正常错误路径、又展示配置边界的示例。庞大的教程式 walkthrough 通常没有帮助。文档的目的是让另一个团队无需学习内部“口口相传的知识”，也能把这个库集成进去。

性能声明也应当保持同样的克制。不要说库“很快”。要说清：测了什么、在什么负载下测、跟什么基线比较，以及成本模型对哪些因素敏感。解析库的成本，往往高度依赖分配策略、输入大小分布和失败率。应当直接把这些说出来。

## 验证应当匹配库的公开承诺

可复用库比叶子应用组件需要更强的验证纪律，因为调用者无法检查你全部的假设。因此，测试应直接映射到公开承诺上。

如果库承诺对文档化格式版本保持稳定解析行为，就应保留基于 fixture 的契约测试。如果它承诺畸形输入会返回结构化失败而不是触发未定义行为，就应当在公开解析入口上运行 fuzzing 和 sanitizer。如果它声称在某些模式下分配是有界的，就应当对这个声明做 benchmark 或埋点。如果它暴露宿主提供的诊断通道，就应测试 sink 收到的是稳定事件类别，而不是实现层的噪音。

兼容性检查也应放在这里。如果你承诺在次版本之间保持源码兼容，就应保留集成测试或样例客户端来覆盖旧调用方式。如果 ABI 很重要，就应以真正检查符号与布局预期的方式测试产物。“它在我机器上还能编译”不是兼容性策略。

## 知道什么时候还不该做成库

团队经常过早创建共享库。如果只有一个应用在用它，如果领域词汇每周都在变化，或者所谓“复用”大多只是组织层面的愿望，那么强行冻结一个稳定公开表面，往往只会把糟糕假设固定下来。有时正确答案是：在契约真正稳定前，先把组件留在内部。

这不是反对复用，而是要求你给公开 API 计入真实成本。一旦其他团队依赖了这个库，修改语义、所有权或错误策略的成本，就会远高于修改内部代码。只有当问题和契约已经成熟到值得支付这笔成本时，复用才真正有价值。

## 要点总结

一个好的可复用 C++23 库，会做出一个狭窄承诺，在公开类型中直接编码所有权与不变量，让失败形态保持稳定，只暴露少量有意选择的定制接缝，并诚实陈述版本与性能承诺。它应尽量减少依赖拖拽，并让有经验的调用者能够轻松做出采用决策。

这些权衡仍然是熟悉的那些，只是比应用代码支付得更显式。更丰富的签名，看起来可能不够优雅。稳定错误类别需要边界转换。ABI 感知型设计会限制公开模板自由。狭窄接缝要求你有纪律地决定哪些东西不该定制。之所以这些成本可以接受，是因为库是一个长寿命契约，而不只是一堆可复用代码。

Review questions:

- 这个库做出的狭窄承诺是什么，又明确拒绝承担哪些重要职责？
- 哪些公开类型在没有隐藏假设的前提下，传达了所有权、生命周期和不变量？
- 公开错误表面是否足够小、足够稳定，并且对调用者可据以行动？
- 哪些定制接缝是真正必要的，哪些依赖应当被推到适配器后面，而不是塞进公开头文件？
- 实际做出的兼容性承诺是什么：源码、ABI、序列化格式、语义行为，还是它们的某种组合？
