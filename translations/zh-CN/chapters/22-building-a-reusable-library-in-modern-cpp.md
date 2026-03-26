# 在现代 C++ 中构建可复用库

应用代码靠着局部便利，往往能撑很久。库代码不行。API 一旦流出最初的调用者圈子，每一条含糊的所有权契约、每一个滥用的错误通道、每一次不经意的分配、每一处不稳定的类型依赖，都会变成别人的麻烦。库或许还能编译通过，但别人已经不敢信任它了。

所以本章要回答的实际问题不是”怎么写出漂亮的 API”，而是”一个可复用的现代 C++23 库，到底要把哪些东西讲明白，其他团队才能放心接入，不至于连带继承那些隐藏耦合、不稳定行为和无从验证的承诺？”答案要从范围说起。好的可复用库只做一个精确的承诺，用经得起真实使用的类型和契约来表达它，并且坚决不让自身的实现成本渗透到整张依赖图中。

本章选用的示例是一个供多个服务和命令行工具共同使用的解析与转换库。之所以选这个领域，是因为它几乎涵盖了所有棘手的现实压力：输入边界、分配行为、诊断信息、性能预期、可扩展性，以及打包分发。

## 从一个精确的承诺开始

很多糟糕的库，还没写出第一行公开 API 就已经注定失败了——它们的定位是”凡是跟 X 相关的东西都放这里”。后果不难预见：职责不断堆积，为各种不相干的需求长出扩展点，任何改动都牵一发动全身，版本演进举步维艰。

可复用库应该只做一个精确的承诺：解析并验证某种格式；归一化一批记录；暴露一层存储抽象；计算一组派生值。承诺本身可以很有分量，但必须有一个清晰的核心。

这道理听起来显而易见，却会立刻改变接口设计的走向。精确的承诺意味着更少的公开类型、更少的失败类别、更少需要调用者自行定制的地方。而含糊的承诺只会把复杂度往外推——推成泛滥的模板、回调丛林、配置映射表，以及读起来像谈判停火协议的文档。

因此，库设计中最重要的决定不是该不该用模块、concept 还是 `std::expected`，而是公开契约到哪里为止。

## 公开类型应直接编码所有权与不变量

调用者不应该非得翻代码仓库，才能搞清楚所有权、生命周期、可变性或有效性这些基本问题。库返回一个视图，调用者就该知道底层数据归谁所有。库接收一个回调，回调的生命周期和线程要求就该一目了然。一个配置对象可能处于无效状态，那这个无效状态只应在验证完成前短暂存在。

值类型通常是库 API 的最佳重心，因为它们在跨团队和跨测试场景中传递起来最可靠。值类型让复制成本一目了然，让 move 语义成为有意识的选择，也让不变量可以绑定在构造或验证的边界上。`std::string_view` 和 `std::span` 这类借用型输入在调用边界上仍然好用，但前提是库能在借用的生命周期内完成工作，或者及时复制需要保留的数据。

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

这段代码做了几件值得注意的事：把调用者可控的策略与输入字节分开；让分配策略对外可见，但不强制统一使用某个全局分配器；返回领域层面的错误类型，而非传输层或解析器内部的类型；通过 `[[nodiscard]]` 和 `std::expected`，让调用者更难忽略返回值。

代价是函数签名不再像 `document parse(std::string_view)` 那样简洁。这完全没问题。可复用库靠的不是在幻灯片上看起来紧凑，而是让成本和契约清晰可读。

## 让失败形态保持稳定

应用代码有时还能容忍异常和内部错误类别到处漂移——毕竟两头都是同一个团队在管。库就不能这样了。调用者必须分得清：哪些失败属于契约的一部分、哪些属于编程错误、哪些只是实现层面的意外。

这通常会引向三种设计之一。

1. 对调用者本就需要处理的常规领域失败，使用 `std::expected` 或类似的结果类型。
2. 把异常留给不变量被破坏、环境出现异常故障，或者所在生态本来就以异常为主的 API。
3. 在边界处把底层错误翻译成稳定的公开错误词汇。

具体怎么选取决于领域。解析、验证、可恢复的业务规则失败，一般都很适合 `std::expected`。嵌入异常式应用框架的底层基础设施库，用异常也合情合理。但最关键的是保持一致。如果一个库对部分可恢复失败返回 `std::expected`，对另一些却抛异常，某个后端泄漏出 `std::system_error` 而另一个后端不会——那它就是在逼调用者从实现细节里反推策略。

公开错误不要切得太细。调用者需要的是能指导自己下一步行动的区分，二十种只有库内部才看得懂的解析器状态对他们毫无用处。稳定的错误类别应保持精简，更丰富的诊断信息可以通过独立通道按需提供。

## 分离机制与策略，但不要把一切都抽象掉

可复用库往往确实需要一些定制能力：分配、日志钩子、宿主侧的诊断、时钟源、I/O 适配器、用户自定义的处理器。问题就出在这里——团队要么把整个接口层过度模板化，要么把库包裹在运行时多态接口里，搞得到处都是分配和虚分派。

更好的做法是只开放少量显式的策略接缝，核心机制保持具体实现。需要零开销且编译期可检查的定制点时，concept 很有用；二进制边界、插件模型或运维解耦比模板透明性更重要时，类型擦除或回调接口更合适。

举个例子，一个内部解析引擎通常没必要做成同时参数化日志、分配、诊断和错误格式化的庞大策略模板。它完全可以用具体代码做解析、接收一个 `std::pmr::memory_resource&`、再通过一个窄接口按需输出诊断信息就够了。这样大多数调用点保持简洁，宿主仍然能控制那些代价高昂或与环境强相关的部分。

库作者还必须在依赖管理上保持自律。如果公开头文件为了支持几个可选功能就引入了网络栈、格式化库、指标 SDK 和文件系统抽象，那可移植性和构建整洁度就已经丢了。可选的运维功能应当藏在窄接缝后面或放进配套的适配器里，不要塞进 API 核心。

### 错误：在公开头文件中暴露内部类型

最常见的库设计失误之一，就是让实现类型泄漏到公开 API 中。由此产生的隐藏耦合后患无穷：调用者被迫传递性地依赖自己从未主动引入的头文件，构建时间随之膨胀，库内部的重构也会变成对外的破坏性变更。

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

如果库用裸整数、裸 `std::string` 消息或平台特有的异常类型来报告错误，调用者就只能从实现细节里反推失败语义。最终结果是脆弱的错误处理——库内部一改，调用者的处理逻辑就跟着崩。

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

如果需要比错误类别更丰富的诊断信息，应通过独立通道提供（诊断 sink、错误详情访问器或结构化日志），而不是把调用者无法程序化处理的实现细节硬塞进主错误类型。

## 版本与 ABI 需要策略，不能靠乐观

哪怕只是内部共享库，把版本管理当作设计的一部分也远比当作发布流程的一道手续有价值。真正要回答的问题是：库向调用者承诺了哪些变更是可以安全承受的。源码兼容、ABI 兼容、线协议稳定、序列化数据稳定、语义兼容——这些概念彼此相关，但并不等价。

对大多数 C++ 库而言，最诚实也最可行的策略是：在一个主版本内保证源码兼容，不对跨任意工具链的 ABI 做笼统承诺。这种姿态看似保守，实际上比口头宣称 ABI 稳定、公开表面却到处是标准库类型、内联密集模板和平台相关布局要靠谱得多。

如果 ABI 稳定性确实重要，整个设计就得跟着变。这通常意味着更窄的导出面、opaque 类型、PIMPL 式的边界、更严格的异常策略、更少的模板暴露，以及对编译器和标准库版本的明确约束。这些绝不是收尾修饰，而是影响整个 API 形态的基础性决策。

### 错误：通过内联变更破坏 ABI

源码层面看起来完全安全的改动，可能悄无声息地破坏二进制兼容性。给类加一个成员、改一个内联函数的默认参数值、调整字段顺序——这些都会改变 ABI，编译器却不会发出任何警告。

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

PIMPL 会给每个对象多带来一次堆分配和一层间接访问。对于创建不频繁的类型（文档、连接、会话），这几乎总是可以接受的代价。但对于热路径上每秒创建数百万次的类型就不行了——这时候该重新考虑的是这类类型是否真的需要 ABI 稳定。

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

模块能改善构建结构和分发流程，但抹不掉 ABI 层面的现实。concept 能改善诊断和约束表达，但不会自动让一个模板密集的库变得易于版本管理。这些工具应被视为局部改进手段，而非策略层面的替代方案。

## 文档应回答集成问题

面向有经验程序员的库文档，重点应放在帮助对方做出采用决策，而不是罗列功能卖点。正在评估一个可复用库的调用者，需要知道的是：

- 库解决什么问题，明确不管什么。
- 哪些输入是借用的，哪些输出自带存储。
- 哪些失败属于常规情况，报告方式是什么。
- 正常使用时会产生哪些分配、复制或后台开销。
- 有没有线程安全保证，如果有，保证到什么程度。
- 版本和兼容性方面，哪些承诺是真的。

简短、看起来像生产代码的示例在这里最有用，尤其是那些同时展示了错误处理路径和配置边界的示例。大而全的教程式演练通常帮助不大。文档的目的，是让另一个团队不用打听内部”口耳相传”的知识，也能顺利集成。

性能方面的说法同样需要克制。不要笼统地说库”很快”，而要讲清楚：测了什么场景、在什么负载下、跟什么基线对比、成本模型对哪些因素敏感。解析库的性能往往与分配策略、输入大小分布和失败率密切相关，这些都应当直说。

## 验证应当匹配库的公开承诺

可复用库需要比末端应用组件更严格的验证纪律，因为调用者没办法审查你的全部假设。测试应当直接对应公开承诺。

承诺对某个文档化的格式版本保持稳定解析行为？那就保留基于 fixture 的契约测试。承诺畸形输入只会返回结构化错误而不触发未定义行为？那就在公开解析入口上跑 fuzzing 和 sanitizer。宣称在特定模式下分配量有上界？那就用 benchmark 或埋点来验证。暴露了宿主侧的诊断通道？那就测试 sink 收到的确实是稳定的事件类别，而不是实现层的噪音。

兼容性检查同样属于这个环节。承诺次版本间源码兼容，就要有集成测试或样例客户端来覆盖旧的调用模式。ABI 很重要，就要用能真正检验符号和布局预期的方式来测试产物。”在我机器上还能编译”不是兼容性策略。

## 知道什么时候还不该做成库

团队经常过早地把代码做成共享库。如果只有一个应用在用，领域词汇每周都在变，或者所谓的”复用”更多只是组织层面的美好愿望，那么强行冻结出一个稳定的公开接口，往往只会把错误的假设固化下来。有时正确的做法是：在契约真正稳定之前，先把组件留在内部。

这并不是反对复用，而是要求认真计算公开 API 的真实成本。其他团队一旦依赖上这个库，修改语义、所有权或错误策略的代价就会远高于改内部代码。只有当问题本身和契约都已成熟到值得承担这笔代价时，复用才真正有意义。

## 要点总结

好的可复用 C++23 库只做一个精确的承诺，在公开类型中直接编码所有权与不变量，保持失败形态的稳定，只开放少量经过深思熟虑的定制接缝，并如实陈述版本与性能方面的承诺。它应尽量减少依赖拖拽，让有经验的调用者能轻松做出是否采用的判断。

这些权衡并不陌生，只是在库的语境下要付得更明确。更丰富的签名可能不够优雅；稳定的错误类别需要在边界做转换；面向 ABI 稳定的设计会限制公开模板的自由度；窄接缝要求你有纪律地决定哪些东西不开放定制。之所以这些成本可以接受，是因为库是一份长期契约，而不仅仅是一堆可复用的代码。

复习问题：

- 这个库做出的核心承诺是什么？它明确拒绝承担哪些职责？
- 哪些公开类型在不依赖隐藏假设的前提下，清晰地传达了所有权、生命周期和不变量？
- 公开的错误类别是否足够精简、足够稳定，且能让调用者据此采取行动？
- 哪些定制接缝确实必要？哪些依赖应当推到适配器后面，而不是出现在公开头文件中？
- 实际做出的兼容性承诺到底是什么——源码兼容、ABI 兼容、序列化格式稳定、语义行为不变，还是其中几项的组合？
