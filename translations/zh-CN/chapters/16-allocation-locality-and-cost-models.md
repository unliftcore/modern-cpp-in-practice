# 分配、局部性与成本模型

## 生产问题

一旦数据形状合理，接下来的性能问题就是字节从哪里来，以及它们多频繁地移动。许多团队会从“这条路径很慢”直接跳到 allocator 调优或 pool 设计，却没有先说明成本模型。这是本末倒置。改变分配策略之前，你需要先知道哪些成本占主导：分配延迟、allocator 内部的同步、page fault、对象分散导致的缓存和 TLB 未命中、过大值带来的拷贝成本，还是抽象层带来的指令开销。

本章就是在建立这个模型。重点不是记住 allocator API。重点是具体推理某个设计会迫使机器做什么。像“`std::function` 是零成本的”“arena 总是更快”“小分配现在已经很便宜了”这样的说法，都不是工程论证。只有当你能指出真实的对象图、分配的次数与时机、相关对象的所有权生命周期跨度，以及每一层间接访问带来的局部性后果时，论证才算开始。

和上一章的边界要保持清晰。第 15 章问的是：“表示应该是什么？”本章问的是：“在某种表示既定的前提下，它随时间会施加哪些成本？”这里仍会提到容器，但重点是分配频率、生命周期聚类、对象图深度和局部性，而不是容器语义。

## 从分配清单开始

第一个有用的性能模型简单得近乎尴尬：把你关心路径上的所有分配列出来。

大多数代码库在这件事上都比自己想象得更差。一个请求解析器会为每个 header value 分配字符串，一层路由会把回调用类型擦除 wrapper 存起来，一次 JSON 转换会物化中间对象，而日志路径会格式化到临时 buffer 里。每个决策在局部看都可能合理；合在一起，就会让一个请求在真正的业务工作开始之前，先执行几十次甚至上百次分配。

做分配清单时，应把三个问题分开：

- 稳态热路径上的哪些操作会分配？
- 哪些分配属于一次性 setup 或 batch 重建成本？
- 哪些分配可以通过不同的所有权或数据流来避免，而不是通过换一个更好的 allocator 来避免？

最后一个问题最重要。如果系统之所以频繁分配，是因为它执意把稠密处理拆成许多短生命周期的堆对象，那么更换 allocator 也许能减轻痛苦，却修不好设计。杠杆最大的变化，往往是让这些分配不再成为必需。

下面是一个热路径上“分配密集型代码”长什么样，以及替代方案可能是什么样的具体例子：

```cpp
// Allocation-heavy: every event creates a temporary string,
// a vector, and a map entry.  Under load this path may perform
// 5-10 heap allocations per event.
struct Event {
	std::string type;
	std::string payload;
	std::vector<std::string> tags;
	std::unordered_map<std::string, std::string> metadata;
};

void process_batch_heavy(std::span<const RawEvent> raw,
                         std::vector<Event>& out) {
	for (const auto& r : raw) {
		Event e;
		e.type = parse_type(r);         // allocates
		e.payload = parse_payload(r);   // allocates
		e.tags = parse_tags(r);         // allocates vector + each string
		e.metadata = parse_meta(r);     // allocates map buckets + nodes
		out.push_back(std::move(e));    // may reallocate out's buffer
	}
}
```

```cpp
// Allocation-light: pre-sized arena, string views into stable
// input buffer, fixed-capacity inline storage.
struct EventView {
	std::string_view type;
	std::string_view payload;
	// Use a small fixed-capacity container for tags.
	// boost::static_vector or a similar stack-allocated small vector.
	std::array<std::string_view, 8> tags;
	std::uint8_t tag_count = 0;
};

void process_batch_light(std::string_view input_buffer,
                         std::span<const RawEvent> raw,
                         std::vector<EventView>& out) {
	out.clear();
	out.reserve(raw.size());  // one allocation, amortized
	for (const auto& r : raw) {
		EventView e;
		e.type = parse_type_view(r, input_buffer);
		e.payload = parse_payload_view(r, input_buffer);
		e.tag_count = parse_tags_view(r, input_buffer, e.tags);
		out.push_back(e);
	}
	// Zero heap allocations per event if input_buffer is stable
	// and out has sufficient capacity.
}
```

轻量版本施加了约束：输入 buffer 必须比这些 view 活得更久，tag 数量有上界，metadata 需要用不同方式处理。避免分配的代价就是这些约束。这个代价能不能接受，要看负载；而把它显式化，正是重点所在。

## 分配成本不只是调用 `new`

工程师有时谈论分配，就像唯一的成本只是 allocator 函数调用本身。到了生产环境，这通常只占账单的一小部分。分配还会影响缓存局部性、同步行为、碎片化、页工作集，以及之后的销毁成本。如果一个对象图把逻辑上相邻的数据散布到无关的堆位置上，之后每次遍历都要为这个决定付费。如果每个请求的分配都要从多个线程击中共享的全局 allocator，那么 allocator 争用就会成为延迟方差的一部分。如果许多短生命周期对象都是单独销毁的，那么 burst 期间清理流量本身就可能主导尾延迟。

这就是为什么“我们用了 pool，所以问题解决了”常常是错的。pool 也许能降低 allocator 调用开销，甚至减少争用；但如果得到的对象图仍然 pointer-heavy 且分散，遍历依然昂贵。反过来，一个把请求局部状态放进连续 buffer 的设计，即使用默认 allocator，也可能只做很少的分配并享受更好的局部性。

## 生命周期聚类通常胜过聪明的复用

那些一起死亡的对象，通常也应该一起分配。这正是 arena 和 monotonic resource 背后的核心直觉：如果一批数据共享同一个生命周期边界，那么为单个释放付费就是浪费工作。请求局部的 parse tree、临时 token buffer、图搜索的 scratch state，以及一次性编译元数据，都是经典例子。

在 C++23 里，这套标准词汇仍主要依赖 `std::pmr`。它的价值不是风格性的，而是架构性的。memory resource 让你能够表达：一族对象属于同一个共享生命周期区域，而不必把某个自定义 allocator 类型硬编码进每个模板实例。

```cpp
struct RequestScratch {
	std::pmr::monotonic_buffer_resource arena;
	std::pmr::vector<std::pmr::string> tokens{&arena};
	std::pmr::unordered_map<std::pmr::string, std::pmr::string> headers{&arena};

	explicit RequestScratch(std::span<std::byte> buffer)
		: arena(buffer.data(), buffer.size()) {}
};
```

这个设计传达了一个重要事实：这些字符串和容器不是彼此独立的堆公民。它们是请求作用域内的 scratch。这样做降低了分配开销，也让析构变成一次批量操作。

更完整的例子能更清楚地展示差异。比较标准分配与带栈上 buffer 的 `pmr`，在请求处理路径上的不同：

```cpp
#include <memory_resource>
#include <vector>
#include <string>
#include <array>

// Standard allocation: every string, every vector growth, and the
// map internals go through the global allocator.  Under contention
// from many threads, this serializes on allocator locks.
void handle_request_standard(std::span<const std::byte> input) {
	std::vector<std::string> tokens;
	std::unordered_map<std::string, std::string> headers;
	parse(input, tokens, headers);  // many small allocations
	route(tokens, headers);
	// Destruction: each string freed individually, each map node freed.
}

// PMR with stack buffer: small requests never touch the heap.
// The monotonic_buffer_resource first allocates from the stack buffer.
// If the request is large enough to exhaust it, it falls back to
// the upstream resource (default: new/delete).
void handle_request_pmr(std::span<const std::byte> input) {
	std::array<std::byte, 4096> stack_buf;
	std::pmr::monotonic_buffer_resource arena{
		stack_buf.data(), stack_buf.size(),
		std::pmr::null_memory_resource()
		// null_memory_resource: fail loudly if buffer is exceeded.
		// Replace with std::pmr::new_delete_resource() to allow
		// fallback to heap for oversized requests.
	};

	std::pmr::vector<std::pmr::string> tokens{&arena};
	std::pmr::unordered_map<std::pmr::string, std::pmr::string>
		headers{&arena};
	parse_pmr(input, tokens, headers);
	route_pmr(tokens, headers);
	// Destruction: arena destructor releases everything in one shot.
	// No per-string, per-node deallocation calls.
}
```

对于能放进栈上 buffer 的请求，`pmr` 版本完全消除了逐对象的释放调用，也彻底避免了全局 allocator 争用。在处理小请求的高吞吐服务里，这能让 allocator 开销降低一个数量级。权衡在于：`std::pmr` 容器会额外携带一个指向 memory resource 的指针（`sizeof` 会略大），而 monotonic resource 不会回收单个释放产生的空间——它只会不断增长，直到 resource 自身被销毁。对请求作用域的 scratch，这没问题；对会随时间增长又收缩的长生命周期容器，这就是错误做法。

但 monotonic allocation 不是通用升级。当对象需要选择性释放时，它并不合适；当某个病态请求造成的内存峰值不该膨胀稳态占用时，它也不合适；如果错误地保留了其中某一个对象，就会把整个 arena 一起保留下来，它同样不合适。区域分配会把生命周期假设变得更尖锐；如果这些假设错了，失败会比单独所有权更大。

## 局部性关注的是图形状，不只是原始字节数

即使分配次数很低，局部性仍可能非常糟。少量大分配，如果其中放的是指向各自独立分配节点的指针数组，那么在遍历持续跨页来回跳转时，可能比许多小分配更差。因此，成本模型还需要再问一个问题：热代码遍历这份结构时，在碰到有用载荷之前，需要做多少次指针解引用？

pointer-rich 设计在语义上往往很有吸引力，因为它们直接映射领域关系。树指向子节点。多态对象指向实现。pipeline 存着一串堆分配的 stage。有时这不可避免。更多时候，这只是把偷懒伪装成建模。

解决办法不是“永远别用指针”。解决办法是把身份与拓扑，同存储区分开。图可以存成连续节点数组加基于索引的邻接。若操作集合是已知的，多态 pipeline 往往可以表示成一个小而封闭的 `std::variant` step 类型集合。字符串密集的解析器，可以对重复 token 做 intern，或者保留指向稳定输入 buffer 的 slice，而不是为每个字段都分配自有字符串。

这些都不是什么语言技巧式优化。它们是图形状决策。它们减少了在有用工作开始之前必须进行的内存追逐。

## `std::shared_ptr` 的隐藏成本

`std::shared_ptr` 值得特别关注，因为它的成本经常被低估。最显眼的是分配成本：`std::make_shared` 会把控制块和托管对象一起做一次分配，而从原始指针构造则会做两次。但分配只是开始。

更深层的成本是引用计数。每次复制 `std::shared_ptr` 都会做一次原子递增；每次销毁都会做一次带 acquire-release 语义的原子递减。在 x86 上，一次原子递增相对便宜（一个 locked 指令，无争用时大约 10-20 ns），但在跨核共享下，保存控制块的缓存行会在不同核心之间来回 bounce。在高争用下，这会把本应并行的工作串行化。

```cpp
// Looks innocent: passing shared_ptr by value into a thread pool.
// Each enqueue copies the shared_ptr (atomic increment), and each
// task completion destroys it (atomic decrement + potential dealloc).
void submit_work(std::shared_ptr<Config> cfg,
                 ThreadPool& pool,
                 std::span<const Request> requests) {
	for (const auto& req : requests) {
		// Copies cfg: atomic ref-count increment per task.
		pool.enqueue([cfg, &req] {
			handle(req, *cfg);
		});
	}
	// If 10,000 requests are enqueued, that is 10,000 atomic
	// increments on submission and 10,000 atomic decrements
	// on completion, all contending on the same cache line.
}
```

```cpp
// Fix: cfg outlives all tasks, so pass a raw pointer or reference.
void submit_work_fixed(const Config& cfg,
                       ThreadPool& pool,
                       std::span<const Request> requests) {
	for (const auto& req : requests) {
		pool.enqueue([&cfg, &req] {
			handle(req, cfg);
		});
	}
	// Zero reference-counting overhead.  Caller guarantees
	// cfg lives until all tasks complete.
}
```

规则不是“永远别用 `std::shared_ptr`”。规则是：不要为了逃避生命周期思考而使用共享所有权。当对象有明确的所有者和借用者时，就用唯一所有者加引用或视图来表达。把 `std::shared_ptr` 留给那些真正共享、且生命周期非确定的对象。并且，当 `const&` 或原始引用已足够时，绝不要按值传递 `std::shared_ptr`——每次复制都是一次毫无收益的原子往返。

还有其他会累积的成本：`std::shared_ptr` 本身宽度是两个指针（对象指针 + 控制块指针），是原始指针的两倍。因此，`std::shared_ptr` 容器的缓存密度更差。弱引用计数又增加了一个原子变量。存放在控制块里的自定义 deleter，则会在销毁时增加类型擦除的间接层。

## 隐式分配是设计异味

现代 C++ 提供了许多抽象，只有在其成本模型对代码评审来说仍足够可见时，这些抽象才是合适的。问题不在抽象本身。问题在于：某种抽象的分配行为是隐式的、依赖负载的，或者以团队忽视的实现定义方式存在。

`std::string` 可能会分配，也可能落进 small-string buffer。`std::function` 对较大的 callable 可能会分配，对较小的则可能不会。类型擦除 wrapper、协程帧、regex engine、locale-aware formatting 以及基于 stream 的组合，都可能以从当前调用点看不出来的方式发生分配。

这些类型都不是禁用品。它们危险，是因为在热路径上使用时却没有明确证据。如果一个服务为每条消息都构造一个 `std::function`，或者因为下游 API 默认要求拥有权，就反复把稳定的字符串 slice 变成自有 `std::string` 对象，那么真正的问题不只是“分配太多”。而是 API 表面把成本进入系统的位置藏了起来。

像审视线程同步一样严肃地审视热路径抽象。问：

- 这个 wrapper 会分配吗？
- 它会强制多出一层间接访问吗？
- 它会把对象尺寸放大到足以降低打包密度吗？
- 同样的行为能否用封闭替代方案来表达，比如 `std::variant`、模板边界，或借用视图？

正确答案取决于代码体积、ABI、编译时间和替换灵活性。重点是把这种权衡显式化。

## 池、freelist 复用及其失败模式

池化之所以吸引人，是因为它提供了一套关于复用与可预测性的叙事。有时这套叙事是真的。当分配尺寸一致、对象生命周期短、复用频繁、而 allocator 争用又确实重要时，固定大小对象池会有帮助。类似 slab 的设计，也可能相较于完全通用的堆分配改善空间局部性。

但 pool 会以可重复的方式失败。

当对象大小变化足够大，以至于需要多个池，或者内部碎片化把收益吃掉时，它会失败。当 pool 掩盖了无界保留——对象名义上是“以后会复用”，但实际上复用频率低到内存永远回不去系统——它会失败。当每线程 pool 在负载倾斜时让平衡更复杂，它会失败。当代码开始围绕 pool 的可用性编码生命周期，而不是围绕领域所有权编码时，它会失败。当开发者因为“已经有 pool 了”就停止测量时，它也会失败。

操作层面的规则很直白：用池化去支撑一个已知的负载形状，而不是把它当成通用性能姿态。如果你说不清分配分布和复用模式，就还没准备好设计这个 pool。

## 值大小与参数表面仍然重要

分配只是成本模型的一部分。那些在 API 间被随意复制的大值类型，同样会造成破坏。一个“方便”的 record 类型里塞着多个 `std::string` 成员、可选 blob 和 vector，也许通过 move semantics 避免了部分堆流量，但它仍会扩大工作集、增加缓存压力，并让按值传递的选择更昂贵。

这也是第 4 章的 API 指导重新进入视野的地方。当所有权转移或廉价移动才是真正契约时，按值传递非常好。当一条路径反复复制或移动大型聚合对象，只是为了满足通用便利性时，它就很糟。成本模型必须把对象尺寸、移动成本，以及数据穿越各层边界的频率都算进去。

小值更容易在系统中流动。大值往往更适合稳定存储加借用访问、摘要提取，或拆成热部分和冷部分。如果这些选项让 API 更复杂，这种复杂度也可能是合理的。性能设计里有大量案例说明：某一层更“干净”的接口，会在其他所有地方制造本可避免的成本。

## 用于评审的实用成本模型

在生产工作里，一个非形式化但显式的模型通常就够了。对正在评审的路径，写下：

- 每次操作的稳态分配次数。
- 这些分配是线程局部的、全局争用的，还是隐藏在抽象后面的。
- 被分配对象的生命周期分组。
- 热遍历路径上的指针间接访问次数。
- 热工作集的大致大小。
- 遍历是连续的、跨步的、hashed 的，还是图状的。
- 销毁是逐个进行、批量进行，还是基于区域。

这份清单不会给出精确到 cycle 的预测，但它能阻止空谈。它让评审者能区分“这看起来有成本”和“这个设计在 burst 负载下必然产生 allocator 流量、分散读取和糟糕的 teardown 行为”。

## 边界条件

成本模型不是让你把一切都过度专门化的许可证。有时堆分配是正确的，因为对象确实会活过局部作用域，并参与共享所有权。有时类型擦除是跨库边界做替换时正确的权衡。有时 arena allocation 并不合适，因为保留风险或调试复杂度，比吞吐收益更重要。

目标不是局部速度最大化。目标是在真实系统压力下，让成本可预测、可解释。如果某个设计稍微增加了稳态成本，却在一个非热边界上极大改善了正确性或可演化性，这仍然可能是正确选择。成本模型存在，是为了支撑权衡，而不是废除权衡。

## 在调优之前要验证什么

在引入自定义 allocator、池化或大范围 `pmr` 管线之前，先验证四件事。

第一，确认这条路径足够热，以至于分配和局部性在量级上真的重要。第二，确认当前设计确实会以你认为的方式发生分配或散布数据。第三，确认相关对象确实共享你提议中的 allocator 策略所假设的生命周期形状。第四，确认新设计不会只是把成本挪到别处，比如更大的保留内存、更差的调试体验，或更复杂的所有权边界。

下一章会直接讨论证据这一面。成本模型是假设。只有当基准测试和性能剖析验证了正确的假设时，它才变成工程。

## 要点总结

- 在诉诸 allocator 技术之前，先做分配清单。
- 把分配成本看作延迟、局部性、争用、保留和 teardown 成本，而不只是 `new` 的价格。
- 当对象确实共享同一销毁边界时，按生命周期聚类它们。
- 当区域内存策略与所有权匹配时，用 `std::pmr` 表达它，而不是把它当装饰性的现代感。
- 对那些在热路径上隐藏分配和间接访问行为的抽象保持怀疑。
- 为可测的负载形状设计 pool；否则就别设计。
