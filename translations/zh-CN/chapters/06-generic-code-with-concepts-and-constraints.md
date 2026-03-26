# 使用概念与约束编写泛型代码

当问题中确实存在”家族相似性”时，泛型代码才有价值；用模板来推迟接口决策，只会带来破坏。生产中真正要回答的问题不是”怎么让它可复用”，而是”怎样消除重复的同时，不让调用契约、诊断信息和失败行为变得不透明”。

概念（concepts）是 C++ 很长时间以来第一个能在边界处——也就是读者最需要帮助的地方——直接改善局面的特性。它不会让泛型代码自动变简单，但你可以用更贴近实际设计的方式，说明模板对使用者的期望。与”先实例化、再祈祷编译器报错能指到正确的行”的旧时代相比，这是一次重大转变。

本章关注的是普通产品团队也能维护的受约束泛型代码：可复用的变换、窄小的扩展点、策略对象，以及假设条件必须随时可供评审的算法族。

## 从变化开始，而不是从模板开始

大多数糟糕的泛型代码都始于一个错误前提：”这些函数看起来很像，应该模板化。”光是表面语法相似远远不够。真正要问的是：设计中哪些部分允许变化，哪些不变量必须保持固定。

设想一个内部可观测性库，需要把指标批次写入不同的 sink：内存中的测试收集器、本地文件、网络导出器。不变的部分很明确：批次有 schema，一次 flush 内时间戳必须单调递增，序列化失败必须上报，关闭时不能丢掉已确认的数据。变化的部分只有一个——字节最终写到哪里。

这说明只需要一条窄小的泛型接缝，完全没有理由把整条流水线都模板化。

如果从解析到重试逻辑再到传输机制全部模板化，你写的就不再是可复用代码，而是在代码库里造了一门新语言。概念只有在变化边界本身就划得合理时，才帮得上忙。

## 约束好边界，实现就能保持平凡

概念在实践中的主要用途不是花哨的重载排序，而是告诉调用方和编译器：你的算法可以假设哪些操作存在。

考虑一个 batching 辅助函数，它把已经序列化好的记录写入某个 sink：

```cpp
template <typename Sink>
concept ByteSink = requires(Sink sink,
std::span<const std::byte> bytes) {
{ sink.write(bytes) } -> std::same_as<std::expected<void, WriteError>>;
{ sink.flush() } -> std::same_as<std::expected<void, WriteError>>;
};

template <ByteSink Sink>
auto flush_batch(Sink& sink,
 std::span<const EncodedRecord> batch)
-> std::expected<void, WriteError>
{
for (const auto& record : batch) {
if (auto result = sink.write(record.bytes); !result) {
return std::unexpected(result.error());
}
}
return sink.flush();
}
```

这段代码有几处值得注意的优点：

- 概念名描述的是角色，而非实现技巧。
- 要求的操作很少，且每个都有明确的业务含义。
- 失败行为是契约的一部分。
- 函数体就是普通代码——实现的抽象程度没有超出问题本身的需要。

另一种写法是经典的无约束模板：

```cpp
template <typename Sink>
auto flush_batch(Sink& sink, const auto& batch) {
for (const auto& record : batch) {
sink.push(record.data(), record.size()); // RISK: hidden, undocumented assumptions
}
sink.commit();
}
```

这个版本看起来更短，但在每一个生产相关的维度上都更差：假设没有明说，错误契约不清不楚，对记录结构的要求纯属偶然。一旦类型不匹配，编译器会在使用点抛出一堆噪声，而不是清楚地说明接口到底要什么。

### 错误信息问题的实际体感

要体会概念到底改善了什么，看看把错误类型传给无约束版本时会发生什么：

```cpp
struct BadSink {};
BadSink sink;
std::vector<EncodedRecord> batch = /* ... */;
flush_batch(sink, batch);
```

没有概念时，编译器会直接实例化模板体，然后在实现深处报错。主流编译器给出的典型错误大致如下：

```
error: 'class BadSink' has no member named 'push'
    in instantiation of 'auto flush_batch(Sink&, const auto&) [with Sink = BadSink; ...]'
    required from here
note: in expansion of 'sink.push(record.data(), record.size())'
error: 'class BadSink' has no member named 'commit'
note: in expansion of 'sink.commit()'
```

这还只是个简单例子，就已经报了两个错误。在生产中，模板很少这么浅——sink 可能经过三层 adapter 传递，每一层又都是模板。真正的错误出现在很深的实例化栈底部，程序员得在脑子里把这条调用链倒着拆开，才能搞清楚问题出在哪里。一旦叠上深层嵌套模板和标准库类型，这种诊断信息动辄几十行。

有了 `ByteSink` 概念后，同样的错误只会在调用点产生一条精准的诊断：

```
error: constraints not satisfied for 'auto flush_batch(Sink&, ...) [with Sink = BadSink]'
note: because 'BadSink' does not satisfy 'ByteSink'
note: because 'sink.write(bytes)' would be ill-formed
```

错误信息点明了概念名称和未满足的具体要求，位置直接指向调用点而非实现内部。程序员一眼就能看出 `BadSink` 缺了什么接口。

### 概念替代了什么：SFINAE

概念出现之前，约束模板的标准技术是 SFINAE（Substitution Failure Is Not An Error）。其思路是：让模板签名对不合适的类型变得不合法，使编译器在重载决议中静默将其排除，而不是报硬错误。

在 C++20 之前的代码里，与 `ByteSink` 约束等价的写法大致如下：

```cpp
// SFINAE approach (using enable_if to constrain the same interface)
template <typename Sink,
          std::enable_if_t<
              std::is_same_v<
                  decltype(std::declval<Sink&>().write(
                      std::declval<std::span<const std::byte>>())),
                  std::expected<void, WriteError>
              > &&
              std::is_same_v<
                  decltype(std::declval<Sink&>().flush()),
                  std::expected<void, WriteError>
              >,
              int> = 0>
auto flush_batch(Sink& sink, std::span<const EncodedRecord> batch)
    -> std::expected<void, WriteError>;
```

同样的约束，换成了没人想读的写法。`std::enable_if_t` 搭配 `decltype` 和 `std::declval`，表达的不是设计意图，而是在利用编译器机制。当 SFINAE 拒绝某个重载时，典型的错误信息往往只有一句”没有匹配 `flush_batch` 的函数调用”，不会告诉你到底哪条要求没满足。如果存在多个受 SFINAE 保护的重载，编译器甚至会把所有被拒绝的候选逐一列出，却依然不解释各自失败的原因。概念版本在可读性、错误质量和可维护性上全面胜出。

主动约束公共接口，让实现保持朴素。这才是正确的取舍。

## 概念应该描述语义，而不只是语法

只检查成员名是否存在的概念，比没有强，但仍然算不上好设计。生产中的泛型代码要想可维护，概念就必须对应系统中的某个语义角色。

`SortableRange` 比 `HasBeginEndAndLessThan` 好，`ByteSink` 比 `HasWriteAndFlush` 好，`RetryPolicy` 比 `CallableWithErrorAndAttemptCount` 好。概念的名字越像团队日常使用的设计术语，它在代码评审和诊断中就越有用。

为什么这很重要？因为概念服务于两类受众。

1. 编译器用它们来选择和拒绝实例化。
2. 人类用它们来理解算法期待的是哪一类东西。

如果名字和结构只满足了编译器，价值就丢掉了一半。

但这也不意味着概念要试图证明所有语义规则。大多数有用的不变量更适合用测试来验证，而不是靠编译期静态强制。`RetryPolicy` 概念可以要求调用签名和结果类型，却无法证明该策略对某个具体操作是幂等安全的。接受这个局限就好：能在接口里检查的就在接口里说清楚，其余的交给测试和评审。

## 窄定制点优于模板蔓延

很多可复用组件根本不需要庞大的概念层级体系，一两个精心选定的扩展点就够了。

假设一个存储子系统要支持多种记录类型，它们都可以序列化到 wire buffer。常见的坏做法是：定义一个主模板，让特化散布在整个代码库，再靠 ADL 或隐式转换决定实际行为。这样做会让行为难以发现，也很容易被无意破坏。

更干净的做法通常是一个窄定制点加上显式的签名要求。如果行为属于类型本身，可以用成员函数；如果类型应当与序列化库解耦，可以用非成员操作。无论哪种方式，都应该用概念约束其形状和结果。

配套项目 `examples/web-api/` 中有一个具体的例证。其 `JsonSerializable` concept 只要求一个操作——返回值可转换为 `std::string` 的 `to_json()` 成员：

```cpp
// examples/web-api/src/modules/json.cppm
template <typename T>
concept JsonSerializable = requires(const T& t) {
    { t.to_json() } -> std::convertible_to<std::string>;
};
```

这个概念在设计上就是窄的：它命名的是一种单一能力，而不是把序列化、反序列化和校验全塞进一个庞大的要求列表。类型只需提供一个符合签名的 `to_json()` 成员即可接入——无需注册、无需基类、无需散落的特化。三个评审问题都有简短的本地答案。

同样，项目中还定义了 `TaskUpdater` 来约束传给仓储 `update` 方法的 callable 参数：

```cpp
// examples/web-api/src/modules/repository.cppm
template <typename F>
concept TaskUpdater = std::invocable<F, Task&> &&
    requires(F f, Task& t) {
        { f(t) } -> std::same_as<void>;
    };
```

这防止了调用方传入返回意外值或接受错误参数的任意 callable。概念在边界处记录了契约，而不是把约束检查推迟到实现深处的模板实例化错误。

关键在于局部性。评审者应该能快速回答三个问题：

- 到底哪些东西可以变？
- 哪些不变量必须保持？
- 新的模型类型在哪里接入？

如果答案散落在十个头文件里、还依赖一些偶然形成的重载决议规则，这个泛型设计就已经过于隐式了。

## 泛型代码不是隐藏成本的许可证

模板以漂亮的调用点掩盖内存分配、拷贝和代码膨胀，这早已不是新闻。概念解决不了这个问题，它只能让”允许哪些形状”更清晰。

设计泛型代码时，要强迫自己把两类成本说清楚：所有模型上都固定不变的成本，以及随模型而变化的成本。算法是否要求连续存储？是否会物化中间 buffer？某个策略类型会不会被内联进每个翻译单元？某个概念是否同时接受抛异常和不抛异常的操作，导致失败处理在接口上到处渗透？

这些是设计问题，不是优化琐事。一个模板看起来很抽象，实际上只在某一类类型上表现良好——这往往意味着它是个不稳定的抽象。要么收窄概念，要么为成本模型差异显著的场景分别提供重载。

这一点对于大型代码库中广泛引用的头文件尤为重要。每多一次实例化就多一份编译成本，代码体积也可能随之膨胀。如果组件需要跨越共享库或插件边界，普通虚接口或类型擦除 callable 往往是更划算的选择，即便要牺牲一些内联机会。稳定的边界通常比理论上的”零开销纯度”更有价值。

## 普通重载何时优于概念

总有一种诱惑想用概念来证明设计”够现代”。要抵制这种冲动。如果只有三种已知输入类型、也没有迹象表明未来会增长，重载往往更清晰。如果需要运行时多态边界，就直接用。如果变化只在测试中才体现，一个函数对象或可 mock 的小接口往往比泛型子系统更好维护。

只有同时满足以下条件时，概念才能发挥最大作用：

- 算法确实适用于一族类型；
- 所需操作能精确而窄小地描述出来；
- 调用方能从编译期拒绝中获益；
- 实现的成本模型依然清晰可读。

一旦这些条件不满足，模板就会迅速变成负担。

## 失败模式与边界条件

泛型代码的失败模式往往反复出现。

第一种是无约束渗漏。一个泛型函数接受”任何像 range 的东西”，另一个期待”任何可写的东西”，很快代码库中原本没有关联的组件之间就会出现偶然兼容。概念的作用是收窄这些接缝，而不是放大它们。

第二种是约束重复。多个辅助函数之间复制着几乎一模一样的 `requires` 子句，直到接口再也无法演进。遇到反复出现的要求，应优先提取为具名概念。具名概念本身就是文档，不只是语法压缩。

第三种是语义漂移。一个原本为清晰角色设计的概念，因为又有调用方需要又一个操作，逐渐积累了越来越多无关的要求。出现这种苗头时，要么拆概念，要么拆算法，绝不能让一个抽象沦为”差不多相关”用例的杂物筐。

第四种是诊断表演。概念层叠得很讲究，编译器输出却依然不可读。如果使用者搞不清楚自己的类型为什么不满足某个概念，就该简化设计。好的泛型设计应当让失败信息具备可操作性。

## 验证与评审

泛型代码的验证远不止”实例化一次”那么简单。

- 如果某个概念足够核心、值得配备稳定示例，就为有代表性的正向和反向模型添加 `static_assert` 检查。配套项目中 `task.cppm` 直接展示了这一做法——在编译期验证领域类型满足其序列化契约：

```cpp
// examples/web-api/src/modules/task.cppm
static_assert(json::JsonSerializable<Task>,
              "Task must satisfy JsonSerializable");
static_assert(json::JsonDeserializable<Task>,
              "Task must satisfy JsonDeserializable");
```

这些断言充当活文档：如果有人修改了 `Task` 导致 JSON 契约被破坏，编译器会立即拒绝构建，而不是把错误推迟到运行时测试甚至生产环境中才暴露。

- 用少量本质上不同的模型类型测试算法，而不是罗列一大堆只有表面差异的变体。
- 审视概念是否命名了系统中的真实角色，而不只是一堆操作的打包。
- 如果泛型组件位于频繁引用的头文件路径上，需要测量编译时间和代码体积。
- 确认错误行为、内存分配行为和所有权假设在受约束的边界处清晰可见。

编译期拒绝只有在被拒绝的程序确实违反了团队能说清楚的设计规则时才有帮助。否则，你只是在一个不清晰的接口外面加了一道花哨的门禁。

## 要点

- 只有问题中的变化真实且持久时，才值得写泛型代码。
- 用概念约束公共边界，让实现保持朴素可读。
- 按语义角色给概念命名，而非按偶然的语法特征。
- 窄定制点优先，避免开放式特化方案。
- 如果编译期多态反而让成本、诊断或边界变得更差，就改用重载或运行时抽象。
