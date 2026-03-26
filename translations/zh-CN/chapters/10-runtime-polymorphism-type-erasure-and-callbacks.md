# 运行时多态、类型擦除与回调

本章假定你已经知道如何定义良好的源码级边界。接下来要解决的问题是：如何在边界上表达运行时的可变性，同时如实反映成本、生命周期和所有权。

## 生产问题

生产环境中的 C++ 代码迟早都要在运行时做出行为选择。服务要根据配置挑选重试策略；调度器要存储来自各个调用方的工作；库要接受遥测、过滤或认证的 hook；插件宿主要从单独编译的模块中加载行为。这些问题仅靠模板无法解决——在真正需要做决策的地方，具体类型在编译期是未知的。

这时候团队往往会抓住一个熟悉的抽象到处套用。有人什么都用虚函数，有人什么都包进 `std::function`，还有人因为怕堆分配而围绕 `void*` 和函数指针手搓类型擦除。结果通常是：接口能跑，却掩盖了一系列关键信息——callable 是否被持有、是否会分配内存、是否会抛异常、是否可能比所捕获的状态活得更久，以及分派开销在实际热路径上是否值得关注。

本章将现代 C++23 中主要的运行时间接机制逐一拆解：经典虚派发、类型擦除，以及各种回调形式。目的不是评出哪个最好，而是帮你针对所构建边界的生命周期、所有权和性能要求，选出最合适且最轻量的工具。

## 先决定你需要哪一种可变性

并不是所有运行时灵活性都一样。

至少有四种常见情况：

1. 拥有多个长生命周期实现的稳定对象协议。
2. 提交后延迟执行的 callable。
3. 在一次操作中同步调用的短生命周期 hook。
4. 跨越打包或 ABI 边界的插件 / 扩展点。

从远处看它们很相似，因为都是在调用”某种动态的东西”。但往细处看，差异很大：被调用方需要持有什么、行为要存活多久、哪些成本才真正关键。

跳过这一步分类，错误的抽象就可能安安稳稳地用上好几年。一个同步 hook 仅仅因为被建模为 `std::function`，就莫名其妙地要求堆分配。一个后台任务系统因为 API 用起来方便，就存下了借来的回调状态。一个插件系统跨编译器边界暴露 C++ 类层级，还美其名曰"架构"。这些错误是结构性的，不是写法上的。

## 虚派发：适合稳定对象协议

如果你需要为一组长生命周期对象定义稳定的交互协议，虚函数仍然是最清晰的选择。存储后端接口、消息 sink、选定一次后反复使用的策略对象，都适合这个模型。

它的优点很直接：

1. 所有权通常会在周围的对象图中显式体现。
2. 协议很容易以具名操作的形式文档化。
3. 只有在确实必要时，接口才可以通过新增方法谨慎演进。
4. 工具链、调试器和评审者都能立刻理解它。

缺点同样不容回避。层级结构容易诱导过度泛化；每次调用都要走间接派发，难以内联；哪怕一个简单的 callable 就够用，接口也必须承诺对象身份和变更语义。公开继承还把调用方锁定在一种特定的定制方式上——带 vtable 的对象。

当抽象天然就是对象协议时，使用虚派发。不要只是因为行为会变化就使用它。

## 反模式：深层继承层级与脆弱基类

一旦虚派发膨胀为又深又宽的层级结构，基类随时间不断积累新义务，它就变成了负担。

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

每新增一个虚方法，所有派生类要么实现它，要么继承一个可能根本不对的默认实现。只需要绘制能力的类也得应付输入、序列化、动画和无障碍。测试一个最简单的叶子 widget，却不得不构造 `Canvas`、`InputEvent`、`Theme`、`Archive` 和 `AccessibilityInfo` 等一堆对象。基类成了变更放大器——往 `Widget` 加一个方法，整个代码库的派生类都要重新编译，甚至逐个修改。

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

虚继承解决了内存布局的重复问题，却解决不了语义上的冲突。代码能编译过，但运行行为取决于调用方手里拿的是哪个基类指针。这种歧义是结构性的，实现得再小心也无济于事。

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

这里不存在会不断膨胀的基类。新增动画支持不会逼着 label widget 改动，测试 `draw` 也不需要构造 `InputEvent`。各关注点可以独立组合。代价是失去了类层级所带来的、具名对象协议的清晰度——如果协议确实稳定且丰富，这一点可能很关键。这种取舍需要逐案权衡。

## 类型擦除：适合拥有型的运行时灵活性

如果你需要存储或传递运行时选定的行为，既不想暴露具体类型，也不需要继承层级，那么类型擦除就是正确的工具。

`std::function` 是大家最熟悉的例子，但在 C++23 中，如果可拷贝性不是契约的一部分，`std::move_only_function` 往往是更好的默认选择。许多被提交的任务、完成处理器和延迟操作天然就是 move-only 的，因为它们持有 buffer、promise、文件句柄或取消状态。这时候要求可拷贝不是"更灵活"，而是在误导。

类型擦除带来三件事：

1. 调用方可以提供任意 callable 类型。
2. 被调用方可以在当前栈帧结束后仍然拥有这个 callable。
3. 公共契约可以谈调用语义，而不是具体类设计。

随之而来的成本也不可忽视，必须作为设计约束而非实现细节来对待：可能发生堆分配、间接调用开销、更大的对象表示，有时还会丢失 `noexcept` 或 cv/ref 限定信息——除非你在建模时专门考虑了这些。

对大多数系统而言，这些成本完全可以接受。但对某些系统——尤其是热分派循环或高频调度器内部——它们可能是决定性的。先拿真实负载量一量，再谈审美偏好。

`examples/web-api/src/modules/middleware.cppm` 中的中间件系统展示了类型擦除组合的实际用法。`Middleware` 被定义为 `std::function<http::Response(const http::Request&, const http::Handler&)>`——一个类型擦除的 callable，它包装一个 handler 并产出新 handler。`apply()` 将一个中间件与一个 handler 组合；`chain()` 通过逆序折叠，将一系列中间件组合到基础 handler 之上：

```cpp
// examples/web-api/src/modules/middleware.cppm
template <std::ranges::input_range R>
    requires std::same_as<std::ranges::range_value_t<R>, Middleware>
[[nodiscard]] http::Handler
chain(R&& middlewares, http::Handler base) {
    http::Handler current = std::move(base);
    for (auto it = std::ranges::rbegin(middlewares);
         it != std::ranges::rend(middlewares); ++it)
    {
        current = apply(*it, std::move(current));
    }
    return current;
}
```

这里没有任何继承层级。每个中间件（日志、CORS、Content-Type 强制检查）都是独立的 callable，通过 `chain()` 组合，彼此毫不知情。最终结果是一个可以直接交给 server 的 `http::Handler`。这正是类型擦除的强项：行为可组合，实现却无需耦合进同一棵类继承树。

### 常见陷阱：`std::function` 会把 move-only 状态强行变成可拷贝

`std::function` 要求其 target 可拷贝。这个限制看起来人畜无害，直到真实的回调状态登场。

```cpp
// This will not compile. std::function requires CopyConstructible.
auto handler = std::function<void()>{
	[conn = std::make_unique<DbConnection>()](){ conn->heartbeat(); }
};
```

团队常见的做法是把 `unique_ptr` 换成 `shared_ptr` 来绕过编译错误，结果给原本天然属于单一所有者的代码凭空引入了引用计数和共享可变性。编译倒是通过了，所有权模型却被破坏了。

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

这类回调在当前调用期间执行，用完即弃。日志 visitor、逐记录校验 hook、遍历回调通常属于此类。核心特征是：被调用方不会存储这个 callable。

这时候强行让 API 经过拥有型包装器，通常没有必要。如果调用不出组件边界，模板化的 callable 参数可能是最简单的做法。如果需要非模板接口，轻量的借用型 callable view 也可行，但标准 C++23 尚未提供 `function_ref`。因此许多团队在组件内部对同步 hook 使用受约束模板，只在真正需要所有权时才动用类型擦除。

### 拥有型延后回调

队列、调度器、时间轮、异步客户端——这些组件往往需要把工作保留到当前栈帧之后。`std::move_only_function` 或带有显式分配规则的自定义擦除任务类型，天然适合这类场景。

这里的问题很具体：

1. 队列是否拥有这个 callable？
2. 可拷贝是 API 的一部分，还是只允许 move？
3. 提交时能否分配？
4. callable 是否必须是 `noexcept`？
5. 关闭时未运行的回调会怎样处理？

这些都是接口问题，不是实现细节。

### 一次性完成处理器

只会触发一次的完成路径，从一开始就应该建模为 move-only。这样类型本身就反映了"只用一次"的事实，也能防止有状态的 handler 被意外共享。

```cpp
class TimerQueue {
public:
	using Task = std::move_only_function<void() noexcept>;

	void schedule_after(std::chrono::milliseconds delay, Task task);
};
```

这个签名传递了丰富的信息：队列接管所有权，可能延后运行，且要求任务不得跨调度器边界抛异常。这比笼统的 `std::function<void()>` 参数精确得多。

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

问题不仅是引用捕获本身，而在于 API 根本没交代 handler 的去向：是只在一次操作中借用，还是存到注销为止？会不会被并发调用，或者在析构期间调用？抽象追求了泛化，却没给出可用的契约。

更好的设计会把所有权和生命周期模型写在明面上。如果回调会被保留，注册就应返回一个 handle 或注册对象，用它的生命周期来管控订阅关系。如果回调是同步的，API 就不该为了显得通用而接收拥有型的擦除 callable。

`examples/web-api/` 的 `main.cpp` 展示了对引用捕获的作用域生命周期纪律。`TaskRepository` 在 router 和 handler 之前声明，server 最后声明。因为 C++ 按构造的逆序销毁局部变量，所以 repository 一定比所有捕获了其引用的 handler 和中间件活得更久。这个顺序不是偶然的——它是确保擦除 callable 中捕获的 `repo` 引用永远不会悬垂的结构性保证。当回调以引用方式捕获外部对象时，包围作用域必须保证被引用对象比 callable 活得更久。`main()` 中的声明顺序，正是这一保证的落脚点。

## 虚协议还是擦除 callable？

这里有一条实用的判断原则。

如果行为天然可以描述为一组具名操作或有意义的状态转换，就选虚接口。带有 `record_counter`、`record_histogram` 和 `flush` 的 metrics sink 是对象协议；带有 `get`、`put` 和 `remove` 的存储后端也是对象协议。

如果抽象的本质是”这里有一段工作或逻辑，留着以后调用”，而非”这里有一个承担语义角色的对象”，就选擦除 callable。重试策略、完成处理器、任务提交、谓词 hook，通常都更契合这种模式。

把两者搞混，代码很快就会别扭。只有单个方法的虚类型，往往说明用回调更合适。反过来，参数列表又长又复杂的 callable 签名，往往说明这里该有一个正经的协议对象。

`examples/web-api/` 示例项目在同一个代码库中同时展示了这两种选择。`router.cppm` 中的 `Router` 是一个对象协议：它有流式注册 API（`get`、`post`、`put_prefix` 等），管理着一张路由表，包含多个具名操作。这天然就是一个有状态的具名对象，而非单个 callable。相比之下，`http::Handler` 被定义为 `std::function<Response(const Request&)>`——一个类型擦除的 callable。每个 handler 表达的是"请求匹配时要执行的工作"。Router 的 `to_handler()` 方法将整张路由表折叠为一个擦除 callable，干净地桥接了两种模型。

## 成本模型很重要，但通常只在特定位置重要

运行时间接总有成本，问题在于不能脱离场景空谈。

与 IO、解析、加锁、内存分配或 cache miss 相比，虚派发的开销通常微乎其微。但在热数值内层循环或每秒处理数百万包的分类器里，它的影响可能非常实在。类型擦除可能会分配内存；这是否要紧，取决于提交速率、分配器行为和尾延迟敏感度。小缓冲优化或许有用，但依赖未明确规定的阈值并不是稳定的契约。

不要猜。如果分派在热路径上，就用有代表性的场景实测分支行为、指令足迹、分配速率和端到端吞吐量。如果不在热路径上，就选那个最容易理清所有权和生命周期的抽象。

## 异常、取消与关闭语义

运行时间接往往恰好处于那些容易在错误处理上偷懒的边界。回调 API 如果没有说明异常能否跨越边界，就是没做完。任务队列如果接受了工作却没定义关闭语义，也是没做完。插件 hook 如果能在宿主不变量只更新了一半时重入，同样是没做完。

明确决定：

1. callable 能抛异常吗？如果不能，尽量在类型上体现出来。
2. 调用失败时，谁负责转换或记录错误？
3. 回调会被并发调用吗？
4. 关闭期间，尚未执行的回调怎么取消或排空？
5. 是否允许重入？

这些决定的重要性，远超抽象底层用的是 vtable 还是擦除函数包装器。

## 打包边界会改变答案

在作为一个整体构建的程序内部，三种技术都可以自由使用。一旦跨越插件边界或公共 SDK，情况就不同了。跨二进制边界暴露 C++ 运行时多态会连带引入 ABI 假设；携带分配器或异常行为的擦除 callable 在模块或共享库边界上同样可能变得不稳定。

正因如此，第 11 章将打包和 ABI 作为独立问题来讨论。进程内的运行时灵活性是一类设计问题，分别构建的工件之间的二进制契约是另一类。别让一个顺手的进程内抽象不知不觉变成了你的分发契约。

## 验证与评审问题

评审运行时间接机制时，要问的是"类型承诺了什么"，而不是"实现目前做了什么"。

1. 这个边界拥有 callable，还是只借用它？
2. callable 会被保留、跨线程移动，还是在关闭期间被调用？
3. 可拷贝性是契约要求的，还是只是从某个方便的包装器里顺带继承来的？
4. 异常和取消语义是否已明确定义？
5. 分派开销在此工作负载中真的重要吗？还是说隐藏的分配才是真正的问题？
6. 用具名协议对象是否比庞大的 callable 签名更清晰，还是反过来？

验证手段应包括：对回调密集路径做分配追踪，用 sanitizer 覆盖捕获状态的生命周期 bug，以及在确认为热路径时做有针对性的 benchmark。单元测试在这里能力有限，因为代价最高的故障往往是生命周期竞争、关闭时序 bug，以及持续负载下的吞吐量崩塌。

## 要点

运行时间接不是单一的技术，而是一组工具，分别对应不同的生命周期和所有权模型。

稳定对象协议用虚派发。需要持有运行时选定的行为、又不想暴露层级结构时，用类型擦除。根据调用是同步、延后还是一次性的，选择对应的回调形式。所有权是单一的、可拷贝性会歪曲契约时，优先用 `std::move_only_function` 而非 `std::function`。

最重要的是，把生命周期、保留策略、异常行为和关闭语义摆在边界的明面上。隐藏的分配令人烦恼，而隐藏的生命周期规则，才是系统真正崩溃的根源。
