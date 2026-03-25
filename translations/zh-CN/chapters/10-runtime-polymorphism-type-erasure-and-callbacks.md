# 运行时多态、类型擦除与回调

本章假定你已经知道如何定义一个良好的源码级边界。现在的问题是，如何在这个边界上表示运行时可变性，同时不对成本、生命周期或所有权撒谎。

## 生产问题

迟早，生产环境中的 C++ 代码都必须在运行时选择行为。服务会从配置中挑选重试策略。调度器会存储任意调用方提交的工作。库会接收遥测、过滤或认证的 hook。插件宿主会从分别编译的模块中加载行为。这些问题都不能只靠模板解决，因为在决策真正重要的位置，具体类型在编译期并不可知。

这正是团队常常抓住某个熟悉抽象并到处复用的时候。有的团队把虚函数用在所有地方。有的团队把一切都包进 `std::function`。还有的团队因为害怕分配，围绕 `void*` 和函数指针手写类型擦除。结果通常是接口虽然能工作，却隐藏了重要事实：这个 callable 是否被拥有、是否会分配、是否会抛异常、是否可能比捕获状态活得更久，以及分派成本在真实热路径上是否重要。

本章把现代 C++23 中主要的运行时间接工具拆开来看：经典虚派发、类型擦除和各种回调形式。目标不是评出谁最好。目标是选择与所构建边界的生命周期、所有权和性能要求相匹配的最小工具。

## 先决定你需要哪一种可变性

并不是所有运行时灵活性都一样。

至少有四种常见情况：

1. 具有多个长生命周期实现的稳定对象协议。
2. 提交给以后执行的 callable。
3. 在一次操作期间同步调用的短生命周期 hook。
4. 穿越打包或 ABI 边界的插件或扩展点。

从高空看，这些情况之所以相似，是因为它们都在调用“某种动态的东西”。但它们在被调用方需要拥有什么、行为必须存活多久，以及哪些成本重要这些方面差异很大。

如果你跳过这个分类步骤，错误的抽象可能会多年看起来都还过得去。一个同步 hook 可能仅仅因为被建模成 `std::function`，就意外要求堆分配。一个后台任务系统可能因为 API 看起来方便，就存储了借用的回调状态。一个插件系统可能跨编译器边界暴露 C++ 类层级，还把这称为架构。这些错误是结构性的，不是语法性的。

## 虚派发：适合稳定对象协议

当你需要在一组长生命周期对象之上定义稳定协议时，虚函数依然是最清晰的工具。存储后端接口、消息 sink，或者选定一次并被大量复用的策略对象，都符合这个模型。

它的优点很直接：

1. 所有权通常会在周围的对象图中显式体现。
2. 协议很容易以具名操作的形式文档化。
3. 只有在确实必要时，接口才可以通过新增方法谨慎演进。
4. 工具链、调试器和评审者都能立刻理解它。

它的缺点也同样真实。层级结构会诱导过度泛化。逐次调用的派发是间接的，也很难内联。即使一个更简单的 callable 就足够，接口仍然必须承诺对象身份和变更语义。公开继承还会让调用方耦合到一种定制表达方式：带 vtable 的对象。

当抽象天然就是对象协议时，使用虚派发。不要只是因为行为会变化就使用它。

## 反模式：深层继承层级与脆弱基类

当虚派发膨胀成深或宽的层级结构，并且基类会随着时间积累义务时，它就会成为负担。

```cpp
// Anti-pattern: a growing base class that every derived type must satisfy.
class Widget {
public:
	virtual void draw(Canvas& c) = 0;
	virtual void handle_input(const InputEvent& e) = 0;
	virtual Size preferred_size() const = 0;
	virtual void set_theme(const Theme& t) = 0;
	virtual void serialize(Archive& ar) = 0;    // added in v2
	virtual void animate(Duration dt) = 0;       // added in v3
	virtual AccessibilityInfo accessibility() = 0; // added in v4
	virtual ~Widget() = default;
};
```

每新增一个虚方法，都会迫使每个派生类去实现它，或者继承一个可能错误的默认实现。只需要绘制能力的类，仍然必须处理输入、序列化、动画和无障碍。测试一个简单的叶子 widget，却需要构造 `Canvas`、`InputEvent`、`Theme`、`Archive` 和 `AccessibilityInfo` 对象。基类成了变更放大器：对 `Widget` 的一次新增，会触发整个代码库中所有派生类的重新编译，甚至潜在修改。

这就是脆弱基类问题。层级结构看起来可扩展，实际上却很脆弱，因为基类接口会不断膨胀，以服务每个消费者。

### 菱形继承与语义歧义

接口层级中的多重继承会引入菱形问题，而 `virtual` 继承只能部分缓解它。

```cpp
class Readable {
public:
	virtual std::expected<std::size_t, IoError>
	read(std::span<std::byte> buffer) = 0;
	virtual void close() = 0;  // close the read side
	virtual ~Readable() = default;
};

class Writable {
public:
	virtual std::expected<std::size_t, IoError>
	write(std::span<const std::byte> data) = 0;
	virtual void close() = 0;  // close the write side
	virtual ~Writable() = default;
};

// Diamond: what does close() mean here? Read side? Write side? Both?
class ReadWriteStream : public virtual Readable, public virtual Writable {
public:
	// Single close() must now serve two different semantic contracts.
	// Callers holding a Readable* expect close() to close the read side.
	// Callers holding a Writable* expect close() to close the write side.
	// There is no way to satisfy both through one override.
	void close() override { /* ??? */ }
};
```

虚继承解决了布局重复，却没有解决语义冲突。结果是代码虽然能编译，但它的行为取决于调用方手里持有哪个基类指针。这种歧义是结构性的，不会因为实现更小心就消失。

### 对照：类型擦除避开了这些问题

类型擦除完全绕开了层级结构。每个擦除包装器都定义自己的最小契约，而不会强迫不相关的类型进入共同基类。

```cpp
// No base class. No hierarchy. No diamond.
// Any type that is callable with the right signature works.
using DrawAction = std::move_only_function<void(Canvas&)>;
using InputHandler = std::move_only_function<bool(const InputEvent&)>;

struct WidgetBehavior {
	DrawAction draw;
	InputHandler handle_input;
};

// A simple widget only provides what it needs.
// No obligation to implement serialize, animate, or accessibility.
WidgetBehavior make_label(std::string text) {
	return {
		.draw = [t = std::move(text)](Canvas& c) { c.draw_text(t); },
		.handle_input = [](const InputEvent&) { return false; }
	};
}
```

这里没有会膨胀的基类。增加动画支持也不会迫使 label widget 改动。测试 `draw` 也不需要构造 `InputEvent`。每个关注点都可以独立组合。代价是，你失去了类层级那种具名对象协议的清晰度；如果协议确实稳定而丰富，这一点可能很重要。这种权衡值得逐案评估。

## 类型擦除：适合拥有型的运行时灵活性

当你需要存储或传递在运行时选择的行为，又不想在用户模型中暴露具体类型，同时也不需要继承层级时，类型擦除就是正确工具。

`std::function` 是最熟悉的例子，但在 C++23 中，当可拷贝性不是契约的一部分时，`std::move_only_function` 往往是更好的默认选择。很多被提交的任务、完成处理器和延迟操作天然就是 move-only，因为它们拥有 buffer、promise、文件句柄或取消状态。在这种场景里要求可拷贝性，并不灵活，而是误导。

类型擦除带来三件事：

1. 调用方可以提供任意 callable 类型。
2. 被调用方可以在当前栈帧结束后仍然拥有这个 callable。
3. 公共契约可以谈调用语义，而不是具体类设计。

它也会引入一些成本，而这些成本必须被当作设计事实，而不是实现琐事：可能发生堆分配、间接调用开销、更大的对象表示，有时还会丢失 `noexcept` 或 cv/ref 限定细节，除非你做了谨慎建模。

对很多系统来说，这些成本可以接受。对某些系统，尤其是热分派循环或高频调度器内部，这些成本则是决定性的。先在真实工作负载下测量，再谈审美。

### 常见陷阱：`std::function` 会把 move-only 状态强行变成可拷贝

`std::function` 要求其 target 可拷贝。起初这看起来无害，直到真实的回调状态出现。

```cpp
// This will not compile. std::function requires CopyConstructible.
auto handler = std::function<void()>{
	[conn = std::make_unique<DbConnection>()](){ conn->heartbeat(); }
};
```

团队通常会用 `shared_ptr` 包装 `unique_ptr` 来绕过这个限制，于是给原本天然单一拥有者的代码引入引用计数和共享可变性。这个变通办法虽然能编译，却削弱了所有权模型。

```cpp
// Workaround: shared_ptr "fixes" compilation but lies about ownership.
auto conn = std::make_shared<DbConnection>();
auto handler = std::function<void()>{
	[conn]() { conn->heartbeat(); }
};
// Now conn is shared. Who shuts it down? When? The ownership story is gone.
```

`std::move_only_function` 完全绕开了这个问题。如果你的回调会被提交、入队或延后执行，而且不会被拷贝，那么它就是 C++23 中正确的默认选择。

## 回调形式：借用、拥有与一次性

“回调”这个词掩盖了三种不同契约。

### 借用型同步回调

这类回调会在当前调用期间被执行，但不会被保留。日志 visitor、逐记录校验 hook 或遍历回调通常属于这一类。关键性质是，被调用方不会存储这个 callable。

在这种情况下，强迫 API 穿过一个拥有型包装器通常没有必要。如果调用保持在组件内部，模板化 callable 参数可能最简单。如果你需要一个非模板表面，轻量的借用型 callable view 也可行，但标准 C++23 还没有提供 `function_ref`。因此，很多团队会在组件内部对同步 hook 使用受约束模板，把类型擦除留给那些确实需要所有权的场景。

### 拥有型延后回调

队列、调度器、时间轮或异步客户端，往往需要在当前栈帧之外保留工作。这正是 `std::move_only_function` 或带显式分配规则的自定义擦除任务类型的天然主场。

这里的问题很具体：

1. 队列是否拥有这个 callable？
2. 可拷贝是 API 的一部分，还是只允许 move？
3. 提交时能否分配？
4. callable 是否必须是 `noexcept`？
5. 关闭时未运行的回调会怎样处理？

这些都是接口问题，不是实现细节。

### 一次性完成处理器

恰好触发一次的完成路径，通常从一开始就更适合建模为 move-only。这会让类型与现实对齐，也能防止状态化 handler 被意外共享。

```cpp
class TimerQueue {
public:
	using Task = std::move_only_function<void() noexcept>;

	void schedule_after(std::chrono::milliseconds delay, Task task);
};
```

这个签名表达了重要信息。队列接管所有权，可能稍后运行，并且要求任务不要跨调度器边界抛出异常。这比通用的 `std::function<void()>` 参数精确得多。

## 反模式：表面灵活，生命周期含糊

很多回调 bug 都来自那些看似灵活、却没有说明谁拥有谁的 API。

```cpp
// Anti-pattern: ambiguous retention and capture lifetime.
class EventSource {
public:
	void on_message(std::function<void(std::string_view)> handler);
};

void wire(EventSource& source, Session& session) {
	source.on_message([&session](std::string_view payload) {
		session.record(payload);
	});
	// RISK: if EventSource stores the handler past Session lifetime, this dangles.
}
```

问题不只在于按引用捕获。问题在于，API 没有说明 handler 是只借用一次操作、一直存储到注销、并发调用，还是在析构期间调用。这个抽象选择了泛化，却没有给出可用契约。

更强的设计会显式命名所有权和生命周期模型。如果回调会被保留，注册就应返回一个 handle 或注册对象，其生命周期支配订阅关系。如果回调是同步的，API 就不该只是为了显得泛型，而接收一个拥有型擦除 callable。

## 虚协议还是擦除 callable？

有一个实用的决策规则。

当行为天然更适合描述为具名对象协议，并且具有多个操作或有意义的状态转换时，选择虚接口。带有 `record_counter`、`record_histogram` 和 `flush` 的 metrics sink 是对象协议。带有 `get`、`put` 和 `remove` 的存储后端是对象协议。

当抽象的本质是“这里有一段稍后要调用的工作或逻辑”，而不是“这里有一个承担语义角色的对象”时，选择擦除 callable。重试策略、完成处理器、任务提交和谓词 hook，通常都更适合这个模式。

当团队把这两者混淆时，代码会很快变得别扭。只有单一方法的虚类型，往往是在提示回调会更清楚。反过来，带着参数元组的巨大 callable 签名，往往是在提示应该存在一个真正的协议对象。

## 成本模型很重要，但通常只在特定位置重要

运行时间接总有成本模型。错误在于把它抽象地讨论。

与 IO、解析、加锁、分配或 cache miss 相比，虚派发的成本通常微不足道。但在热数值内层循环，或每秒运行数百万次的逐包分类器里，它可能非常真实。类型擦除可能会分配；这是否重要，取决于提交速率、分配器行为以及尾延迟敏感性。小缓冲优化可能有帮助，但依赖未指定阈值并不是稳定契约。

不要猜。如果分派位于热路径上，就在有代表性的场景里测量分支行为、指令足迹、分配速率和端到端吞吐量。如果它不在热路径上，就选择那个最容易推理所有权和生命周期的抽象。

## 异常、取消与关闭语义

运行时间接常常坐落在那些失败处理会变得草率的边界上。一个没有文档说明异常是否可以跨越的回调 API，是未完成的。一个接受工作的任务队列如果没有定义关闭语义，也是未完成的。一个可以在不变量更新到一半时重入宿主的插件 hook，同样是未完成的。

明确决定：

1. callable 可以抛异常吗？如果不能，在可行时把这编码进类型。
2. 如果调用失败，谁来转换或记录这个失败？
3. 回调可以并发运行吗？
4. 关闭期间，待处理回调如何取消或排空？
5. 是否允许重入？

这些决定，比抽象背后用的是 vtable 还是擦除函数包装器更重要。

## 打包边界会改变答案

在一个作为整体构建的程序内部，这三种技术都可以使用。跨插件边界或公共 SDK 时，答案会改变。跨二进制边界暴露 C++ 运行时多态，会连带引入 ABI 假设。捕获分配器或异常行为的擦除 callable，在模块或共享库边界上同样可能变得不稳定。

这也是为什么第 11 章会把打包和 ABI 作为单独问题处理。进程内的运行时灵活性是一类设计问题。分别构建工件之间的二进制契约，则是另一类。不要让一个方便的进程内抽象，意外变成你的分发契约。

## 验证与评审问题

评审运行时间接时，要问类型承诺了什么，而不是实现当前做了什么。

1. 这个边界拥有 callable，还是只借用它？
2. callable 会被保留、跨线程移动，还是在关闭期间被调用？
3. 契约要求可拷贝性，还是只是从某个方便包装器里继承来的？
4. 异常和取消语义是否显式？
5. 分派成本在这个工作负载里是否重要，还是隐藏分配才是真正的问题？
6. 这里是具名协议对象比巨大 callable 签名更清楚，还是反过来？

验证应包括对回调密集路径的分配跟踪、对捕获状态生命周期 bug 的 sanitizer 覆盖，以及当分派位于已测得的热路径时进行有针对性的 benchmark。单元测试在这里作用有限，因为最昂贵的失败通常是生命周期竞争、关闭 bug，以及持续负载下的吞吐量崩溃。

## 要点

运行时间接不是一件事。它是一组分别适用于不同生命周期和所有权模型的工具。

为稳定对象协议使用虚派发。在你需要拥有型、运行时选择的行为而又不暴露层级结构时，使用类型擦除。根据调用是同步、延后还是一次性，选择匹配的回调形式。当所有权单一、而可拷贝性会误述契约时，优先使用 `std::move_only_function` 而不是 `std::function`。

最重要的是，让生命周期、保留方式、异常行为和关闭语义在边界上可见。隐藏分配很烦人。隐藏的生命周期规则，才是系统失败的方式。
