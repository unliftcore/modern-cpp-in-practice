# 使用概念与约束编写泛型代码

问题中确实存在”家族相似性”时，泛型代码才有价值；用模板来推迟接口决策只会带来破坏。生产中真正要回答的问题不是”怎么让它可复用”，而是”怎样消除重复的同时，不让调用契约、诊断信息和失败行为变得不透明”。

concepts（概念约束）是 C++ 很长时间以来第一个能在边界处直接改善局面的特性。它不会让泛型代码自动变简单，但你可以用更贴近实际设计的方式说明模板对使用者的期望。与”先实例化再祈祷编译器报错能指到正确的行”的旧时代相比，这是很大的进步。

本章关注普通产品团队也能维护的受约束泛型代码：可复用的变换、窄小的扩展点、策略对象，以及假设条件必须随时可供评审的算法族。

## 从变化开始，而不是从模板开始

大多数糟糕的泛型代码始于一个错误前提：”这些函数看起来很像，应该模板化。”表面语法相似远远不够。真正要问的是：设计中哪些部分允许变化，哪些不变量必须保持固定。

设想一个内部可观测性库，需要把指标批次写入不同的 sink：内存中的测试收集器、本地文件、网络导出器。不变的部分很明确：批次有 schema，一次 flush 内时间戳必须单调递增，序列化失败必须上报，关闭时不能丢掉已确认的数据。变化的部分只有一个，字节最终写到哪里。

只需要一条窄小的泛型接缝，没有理由把整条流水线都模板化。

从解析到重试逻辑再到传输机制全部模板化，你写的就不再是可复用代码，而是在代码库里造了一门新语言。concept 只有在变化边界本身划得合理时才帮得上忙。

## 约束好边界，实现就能保持平凡

concept 在实践中的主要用途不是花哨的重载排序，而是告诉调用方和编译器：你的算法可以假设哪些操作存在。

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

这段代码有几个优点：

- concept 名描述的是角色，不是实现技巧。
- 要求的操作少，每个都有明确的业务含义。
- 失败行为是契约的一部分。
- 函数体就是普通代码，抽象程度没有超出问题本身的需要。

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

这个版本更短，但在每个生产相关的维度上都更差：假设没有明说，错误契约不清楚，对记录结构的要求纯属偶然。类型不匹配时，编译器在使用点抛出一堆噪声，不会清楚说明接口要什么。

### 错误信息的实际差异

把错误类型传给无约束版本时会发生什么：

```cpp
struct BadSink {};
BadSink sink;
std::vector<EncodedRecord> batch = /* ... */;
flush_batch(sink, batch);
```

没有 concept 时，编译器直接实例化模板体，然后在实现深处报错。典型错误大致如下：

```
error: 'class BadSink' has no member named 'push'
    in instantiation of 'auto flush_batch(Sink&, const auto&) [with Sink = BadSink; ...]'
    required from here
note: in expansion of 'sink.push(record.data(), record.size())'
error: 'class BadSink' has no member named 'commit'
note: in expansion of 'sink.commit()'
```

这还只是简单例子就已经报了两个错误。生产中模板很少这么浅，sink 可能经过三层 adapter 传递，每一层又都是模板。真正的错误出现在实例化栈底部，程序员得在脑子里把调用链倒着拆开才能搞清楚问题。叠上深层嵌套模板和标准库类型，这种诊断信息动辄几十行。

有了 `ByteSink` 概念后，同样的错误只会在调用点产生一条精准的诊断：

```
error: constraints not satisfied for 'auto flush_batch(Sink&, ...) [with Sink = BadSink]'
note: because 'BadSink' does not satisfy 'ByteSink'
note: because 'sink.write(bytes)' would be ill-formed
```

错误信息点明了 concept 名称和未满足的具体要求，位置指向调用点而非实现内部。程序员一眼就能看出 `BadSink` 缺了什么接口。

### concept 替代了什么：SFINAE

concept 出现之前，约束模板的标准技术是 SFINAE（Substitution Failure Is Not An Error，替换失败不是错误）。思路是：让模板签名对不合适的类型变得不合法，使编译器在重载决议中静默排除，而非报硬错误。

C++20 之前的代码里，与 `ByteSink` 约束等价的写法大致如下：

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

同样的约束，换成了没人想读的写法。`std::enable_if_t` 搭配 `decltype` 和 `std::declval`，表达的不是设计意图而是在利用编译器机制。SFINAE 拒绝某个重载时，典型错误信息往往只有一句”没有匹配 `flush_batch` 的函数调用”，不会告诉你哪条要求没满足。如果有多个受 SFINAE 保护的重载，编译器甚至会把所有被拒绝的候选逐一列出，却依然不解释各自失败的原因。concept 版本在可读性、错误质量和可维护性上全面胜出。

主动约束公共接口，让实现保持朴素。这才是正确的取舍。

## concept 应该描述语义，不只是语法

只检查成员名是否存在的 concept 比没有强，但算不上好设计。生产中的泛型代码要想可维护，concept 必须对应系统中的某个语义角色。

`SortableRange` 比 `HasBeginEndAndLessThan` 好，`ByteSink` 比 `HasWriteAndFlush` 好，`RetryPolicy` 比 `CallableWithErrorAndAttemptCount` 好。concept 的名字越像团队日常使用的设计术语，它在代码评审和诊断中就越有用。

concept 服务于两类受众：编译器用它们选择和拒绝实例化，人类用它们理解算法期待哪一类东西。名字和结构只满足编译器的话，价值就丢掉了一半。

但这也不意味着 concept 要试图证明所有语义规则。大多数有用的不变量更适合用测试来验证，而非靠编译期静态强制。`RetryPolicy` concept 可以要求调用签名和结果类型，却无法证明该策略对某个具体操作是幂等安全的。接受这个局限：能在接口里检查的就在接口里说清楚，其余交给测试和评审。

## 窄定制点优于模板蔓延

很多可复用组件不需要庞大的 concept 层级体系，一两个精心选定的扩展点就够了。

假设一个存储子系统要支持多种记录类型，它们都可以序列化到 wire buffer。常见的坏做法是定义一个主模板，让特化散布在整个代码库，靠 ADL 或隐式转换决定实际行为。行为难以发现，也容易被无意破坏。

更干净的做法通常是一个窄定制点加显式的签名要求。行为属于类型本身就用成员函数；类型应当与序列化库解耦就用非成员操作。无论哪种方式，都应该用 concept 约束其形状和结果。

配套项目 `examples/web-api/` 中有个例子。`JsonSerializable` concept 只要求一个操作，返回值可转换为 `std::string` 的 `to_json()` 成员：

```cpp
// examples/web-api/src/modules/json.cppm
template <typename T>
concept JsonSerializable = requires(const T& t) {
    { t.to_json() } -> std::convertible_to<std::string>;
};
```

这个 concept 设计上就是窄的：命名的是一种单一能力，没有把序列化、反序列化和校验全塞进一个庞大的要求列表。类型只需提供一个符合签名的 `to_json()` 成员即可接入，无需注册、无需基类、无需散落的特化。

项目中还定义了 `TaskUpdater` 来约束传给仓储 `update` 方法的 callable 参数：

```cpp
// examples/web-api/src/modules/repository.cppm
template <typename F>
concept TaskUpdater = std::invocable<F, Task&> &&
    requires(F f, Task& t) {
        { f(t) } -> std::same_as<void>;
    };
```

这防止了调用方传入返回意外值或接受错误参数的任意 callable。concept 在边界处记录了契约，而非把约束检查推迟到实现深处的模板实例化错误。

评审者应该能快速回答三个问题：

- 到底哪些东西可以变？
- 哪些不变量必须保持？
- 新的模型类型在哪里接入？

如果答案散落在十个头文件里还依赖偶然形成的重载决议规则，这个泛型设计就过于隐式了。

## 泛型代码不是隐藏成本的许可证

模板以漂亮的调用点掩盖内存分配、拷贝和代码膨胀，这不是新闻。concept 解决不了这个问题，它只能让”允许哪些形状”更清晰。

设计泛型代码时，要把两类成本说清楚：所有模型上都固定的成本，以及随模型而变化的成本。算法是否要求连续存储？是否会物化中间 buffer？某个策略类型会不会被内联进每个翻译单元？某个 concept 是否同时接受抛异常和不抛异常的操作，导致失败处理在接口上到处渗透？

这些是设计问题，不是优化琐事。一个模板看起来很抽象，实际上只在某一类类型上表现良好，这往往意味着它是个不稳定的抽象。要么收窄 concept，要么为成本模型差异显著的场景分别提供重载。

大型代码库中广泛引用的头文件尤其要注意这一点。每多一次实例化就多一份编译成本，代码体积也可能膨胀。组件需要跨越共享库或插件边界时，普通虚接口或类型擦除 callable 往往更划算，即便要牺牲一些内联机会。稳定的边界通常比理论上的”零开销纯度”更有价值。

## 普通重载何时优于 concept

总有一种诱惑想用 concept 来证明设计”够现代”。要抵制这种冲动。如果只有三种已知输入类型且没有迹象表明未来会增长，重载往往更清晰。需要运行时多态边界就直接用。变化只在测试中才体现时，一个函数对象或可 mock 的小接口往往比泛型子系统更好维护。

同时满足以下条件时，concept 才能发挥最大作用：

- 算法确实适用于一族类型。
- 所需操作能精确、窄小地描述。
- 调用方能从编译期拒绝中获益。
- 实现的成本模型清晰可读。

这些条件不满足时，模板就会迅速变成负担。

## 失败模式与边界条件

泛型代码的失败模式往往反复出现。

**无约束渗漏。** 一个泛型函数接受”任何像 range 的东西”，另一个期待”任何可写的东西”，很快代码库中原本无关的组件之间出现偶然兼容。concept 的作用是收窄这些接缝，不是放大它们。

**约束重复。** 多个辅助函数之间复制着几乎一样的 `requires` 子句，直到接口再也无法演进。反复出现的要求应优先提取为具名 concept。具名 concept 本身就是文档，不只是语法压缩。

**语义漂移。** 原本为清晰角色设计的 concept，因为又有调用方需要又一个操作，逐渐积累越来越多无关的要求。出现这种苗头时要么拆 concept 要么拆算法，不能让一个抽象沦为”差不多相关”用例的杂物筐。

**诊断表演。** concept 层叠得很讲究，编译器输出却依然不可读。使用者搞不清楚自己的类型为什么不满足某个 concept 时，就该简化设计。好的泛型设计应当让失败信息具备可操作性。

## 验证与评审

泛型代码的验证不止”实例化一次”。

- 如果某个 concept 足够核心、值得配备稳定示例，就为有代表性的正向和反向模型添加 `static_assert` 检查。配套项目中 `task.cppm` 展示了这一做法，在编译期验证领域类型满足其序列化契约：

```cpp
// examples/web-api/src/modules/task.cppm
static_assert(json::JsonSerializable<Task>,
              "Task must satisfy JsonSerializable");
static_assert(json::JsonDeserializable<Task>,
              "Task must satisfy JsonDeserializable");
```

这些断言是活文档：如果有人修改了 `Task` 导致 JSON 契约被破坏，编译器会立即拒绝构建，而非把错误推迟到运行时测试甚至生产环境。

- 用少量本质不同的模型类型测试算法，不要罗列一堆只有表面差异的变体。
- 审视 concept 是否命名了系统中的真实角色，而非一堆操作的打包。
- 泛型组件位于频繁引用的头文件路径上时，要测量编译时间和代码体积。
- 确认错误行为、内存分配行为和所有权假设在受约束的边界处清晰可见。

编译期拒绝只有在被拒绝的程序确实违反了团队能说清楚的设计规则时才有帮助。否则只是在一个不清晰的接口外面加了一道花哨的门禁。

## 要点

- 只有问题中的变化真实且持久时，才值得写泛型代码。
- 用 concept 约束公共边界，让实现保持朴素可读。
- 按语义角色给 concept 命名，而非按偶然的语法特征。
- 窄定制点优先，避免开放式特化方案。
- 编译期多态反而让成本、诊断或边界变得更差时，改用重载或运行时抽象。
