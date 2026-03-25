# 数据布局、容器与内存行为

## 生产问题

许多 C++ 性能失败，本质上是表示失败，却伪装成算法讨论。团队争论一次查找应当是 $O(1)$ 还是 $O(\log n)$，而真正的问题却是热路径在遍历堆节点、触碰过多缓存行，或在每次迭代里拖带冷字段。编译器无法绕过糟糕的数据形状做优化。它只能让已选定的形状稍微便宜一点。

本章讨论决定数据如何驻留在内存中的那些决策：容器选择、元素布局、迭代顺序、失效行为，以及视图如何在不掩盖生命周期的前提下暴露已存储数据。重点不是抽象地讨论“哪个容器最快”。问题更窄，也更有用：在你实际构建的系统里，什么表示能让主导操作变得便宜？

回答这个问题时，要使用真实的压力模型。想想一个拥有数百万路由条目的请求路由器、一条摄取高密度时序样本的遥测 pipeline、一个每帧扫描组件状态的游戏或模拟更新循环，或者一个遍历事件 batch 的分析作业。在这些系统里，容器选择不是实现细节。它是性能契约的一部分。

## 当 Big-O 不再解释运行时间

渐近复杂度是必要的，但不充分。它能筛掉明显糟糕的设计，却无法描述内存流量、分支可预测性、prefetch 行为、false sharing，或间接层级的代价。`std::list` 的插入是常数时间，但如果每个有意义的操作也都需要在冷内存里 pointer chasing，这个事实几乎无关紧要。对中等规模的数据，排序后的 `std::vector` 往往能击败哈希表，因为连续的二分查找和便宜的迭代经常压过名义上的查找复杂度。

这种错位之所以重要，是因为生产负载很少是均匀的。有些操作主导总成本。还有些操作对延迟极其敏感，以至于多出几次缓存未命中比多做一次比较更重要。选择容器之前，先用直白的语言说明真实的负载形状：

- 主导操作是全量遍历、点查找、追加、中间擦除，还是 batch 重建？
- 数据在构建后大多是只读，还是会持续变更？
- 你是否在意稳定地址、稳定迭代顺序，或可预测的失效规则？
- 数据集小到能放进私有缓存，还是大到 TLB 和内存带宽行为占主导？

如果这些答案缺失，容器选择就会滑向传闻。

## 默认优先连续存储

对于经常被遍历的数据，连续存储应当是默认起点。`std::vector`、`std::array`、`std::span`、`std::mdspan`、flat buffer 和列式数组反复获胜，因为硬件会奖励可预测的访问。顺序扫描让处理器可以有效 prefetch、摊薄 TLB 工作，并保持简单的分支行为。这种优势常常比某个理论上“更高级”结构的算法优势还大。

差异并不微妙。考虑对元素数量相同的 `std::vector<int>` 和 `std::list<int>` 求和：

```cpp
#include <list>
#include <vector>
#include <numeric>
#include <cstdint>

// Contiguous: hardware prefetcher has a good day.
std::int64_t sum_vector(const std::vector<int>& v) {
	return std::accumulate(v.begin(), v.end(), std::int64_t{0});
}

// Scattered: every node is a pointer chase.  Each dereference is
// a potential cache miss if nodes were allocated at different times
// and landed on different cache lines or pages.
std::int64_t sum_list(const std::list<int>& l) {
	return std::accumulate(l.begin(), l.end(), std::int64_t{0});
}
```

在典型硬件上处理一百万元素时，vector 版本在纯遍历场景下通常会比 list 版本快 10-50 倍。list 带着每节点开销（在大多数实现中，每个元素有两个指针，再加上 allocator 元数据），但主导成本不是空间，而是推进到下一个节点需要加载一个指针，而它的目标地址对 prefetcher 不可预测。每一步都可能遇到一次 last-level cache miss，代价是 50-100 ns；而 vector 扫描几乎总能命中 L1 或 L2，因为硬件 prefetcher 能识别顺序模式。

这并不是刻意构造的最坏情况。这就是 `std::list` 通过通用 allocator 随时间分配节点时的默认行为。即便给 `std::list` 配一个把节点放得更连续的 pool allocator，也只能部分挽回，因为 next-pointer 的间接性和每节点开销仍然存在。

这就是为什么许多高性能设计初看起来都很朴素。它们把记录存进 `std::vector`，排序一次，再通过二分查找或 batch 扫描回答查询。它们让热数据保持紧凑。它们以粗粒度 batch 重建索引，而不是增量维护 pointer-rich 结构。它们把工作从随机访问转向规则访问。

这并不意味着 `std::vector` 在任何情况下都正确。它意味着，举证责任通常落在非连续的替代方案身上。如果确实需要 node-based 或 hash-based 结构，理由应当是具体的：变更过程中需要稳定迭代器、确实存在大量中间插入、存在并发所有权模式、外部 handle 必须保持有效，或者查找模式足够大且足够稀疏，以至于 hashing 真的能回本。

## 容器编码的是权衡，不只是操作

有经验的评审者应把容器选择读成一组承诺和成本。

`std::vector` 承诺连续存储、尾部追加便宜，以及遍历高效。它向你收费的方式，是偶发重分配、增长时的迭代器失效，以及昂贵的中间擦除。对于 batch、索引、稠密状态，以及那些可以重建或压缩的表，它通常是正确答案。

`std::deque` 放宽了连续性，以保留双端增长更便宜、并避免整个缓冲区搬迁。这对队列式负载可能很有价值，但它的遍历局部性弱于 `std::vector`；在扫描密集的代码里，把它当成“基本就是 vector”是错误的。

像 `std::map` 和 `std::set` 这样的有序关联容器，买来的是稳定顺序和在许多变更下依然稳定的引用；它们支付的代价则是节点分配、间接访问，以及分支密集的遍历。只有当顺序在语义上是必需的，或者变更模式让“构建一次后再重建”的策略不可行时，它们才站得住脚。对于读多写少数据上的热查找，它们不是好默认值。

`std::unordered_map` 和 `std::unordered_set` 用放弃顺序来换取平均情况下更快的查找。但它们也带着真实的内存成本：bucket、load factor、许多实现中的节点存储，以及不可预测的 probe 行为。对于大键空间、并且需要频繁按精确键查找的场景，它们很有价值。但当迭代占主导、内存占用重要，或者工作集小到排序后的连续数据仍能待在缓存里时，它们就没那么有说服力了。

C++23 引入了 `std::flat_map` 和 `std::flat_set`（位于 `<flat_map>` 和 `<flat_set>`），把生产代码库多年来一直在做的事情标准化了：一个排序后的连续键值数组，而它经常更适合读多写少的索引。在 C++23 之前，团队依赖的是 Boost.Container、Abseil 或手写等价物。标准版本接受底层容器模板参数，因此你可以按局部性和分配需求，用 `std::vector`（默认）、`std::deque` 或 `std::pmr::vector` 作为后端。还要注意，`std::flat_map` 在变更时和 `std::vector` 一样会使迭代器失效，而中间插入由于元素搬移仍是 O(n)。它是读优化结构，不是写优化结构。

## 元素内部的布局与容器本身同样重要

仅仅选择 `std::vector<Order>` 还不够。`Order` 的形状本身仍可能浪费带宽。如果每次迭代只读取 `price`、`quantity` 和 `timestamp`，但每个对象还带着一个大 symbol string、审计元数据、重试策略和调试状态，那么对这个 vector 的一次扫描仍会把冷字节拖进缓存。

这正是 hot/cold splitting 发挥作用的地方。把那些在时间上一起被访问的字段，物理上也放得更近。如果把不常用状态移到单独的表、side structure 或 handle 后面，能显著降低扫描成本，就这么做。不要过度抽象成通用的“entity system”，除非代码库真的需要。很多时候，正确做法更简单：一个紧凑的热记录，加上一个存放冷元数据的辅助存储。

同样的压力，也驱动着 array-of-structures 与 structure-of-arrays 的选择。当对象作为整体在系统中流动时，array-of-structures 更容易推理。当处理是列式的：过滤所有 timestamp、对所有 price 做计算、聚合所有计数器，或把数据送进向量化 kernel 时，structure-of-arrays 更占优。表示应当匹配主导访问模式，而不是匹配想象中的对象模型。

要更具体地看到这种权衡，比较下面两种对同一批数据的表示：

```cpp
// Array of Structures (AoS): natural object-oriented layout.
// Each Tick is self-contained.  Good when you routinely need all
// fields of a single tick (e.g., serializing one record, looking
// up a specific event).
struct Tick {
	std::int64_t timestamp_ns;
	std::int32_t instrument_id;
	double bid;
	double ask;
	char exchange[8];       // cold: rarely used in hot aggregation
	std::uint32_t seq_no;   // cold
	std::uint16_t flags;    // cold
	// sizeof(Tick) ~ 48 bytes with padding on most ABIs
};

double mid_price_sum_aos(std::span<const Tick> ticks) {
	double total = 0.0;
	for (const auto& t : ticks) {
		// Each iteration loads a full 48-byte Tick, but only
		// reads bid and ask (16 bytes).  The remaining 32 bytes
		// pollute cache lines and reduce effective bandwidth.
		total += (t.bid + t.ask) * 0.5;
	}
	return total;
}
```

```cpp
// Structure of Arrays (SoA): columnar layout.
// Each field lives in its own contiguous array.
struct TickColumns {
	std::vector<std::int64_t> timestamp_ns;
	std::vector<std::int32_t> instrument_id;
	std::vector<double> bid;
	std::vector<double> ask;
	std::vector<std::array<char, 8>> exchange;
	std::vector<std::uint32_t> seq_no;
	std::vector<std::uint16_t> flags;

	void append(std::int64_t ts, std::int32_t id, double b, double a) {
		timestamp_ns.push_back(ts);
		instrument_id.push_back(id);
		bid.push_back(b);
		ask.push_back(a);
		// ...other columns omitted for brevity
	}
};

double mid_price_sum_soa(const TickColumns& ticks) {
	double total = 0.0;
	// Only bid[] and ask[] are touched.  Each cache line is 100%
	// useful payload.  The compiler can auto-vectorize this loop,
	// and the prefetcher has two clean sequential streams.
	for (std::size_t i = 0; i != ticks.bid.size(); ++i) {
		total += (ticks.bid[i] + ticks.ask[i]) * 0.5;
	}
	return total;
}
```

面对一百万条 tick，在现代 x86 硬件上做列式聚合时，SoA 版本通常会快 2-4 倍。原因是带宽效率：AoS 循环每个元素大约加载 48 字节，却只用到 16 字节，等于浪费了每次缓存行抓取中的三分之二。SoA 循环只触碰自己需要的两个 8 字节数组，而且二者都是完美顺序访问。编译器也更可能为 SoA 版本发出 SIMD 指令，因为不存在会妨碍向量化的交错步长。

SoA 的代价会在别处出现。新增一条 tick 需要向每个列 vector 追加数据，这既别扭又容易出错。把“单条 tick”传给函数时，要么传一个索引加整张表的引用，要么从各列临时组装一个 struct。如果主导操作是逐记录处理，并且会触碰大多数字段，那么 AoS 布局可以避免这种组装成本，也能把相关数据保持在一起。

一个实用的中间地带，是做 hot/cold splitting，而不是完全 SoA：保留一个只含热路径所需字段的紧凑“热” struct，再把冷字段放进一个按相同位置索引的并行 side table。

这种表示本身并不会因为“更现代”就更好。只有在负载会反复独立处理各列，或者紧凑的数值列能显著改善内存行为时，它才更好。如果下游逻辑不断需要一个包含许多相关字段的完整逻辑 tick 对象，那么转换成本或清晰度损失，可能会把收益抵消掉。

## 稳定 handle 很昂贵；先问你是否真的需要它们

许多糟糕的表示，根源都是一个未明说的“地址稳定性”要求。代码把原始指针或引用存进容器元素中，于是容器选择受生命周期和失效问题约束，而不是受访问成本约束。这常常把设计推向 node-based 结构：地址保住了，代价却是其他地方的局部性全面变差。

有时这种权衡是正确的。更多时候，更深层的问题是系统把身份绑在了物理地址上。如果组件需要持久的外部引用，就使用显式 handle、带 generation counter 的索引，或指向稳定间接层的 key。这样，底层存储仍可保持紧凑且可移动，同时又能让外部引用保持安全。

这不是免费的。handle 会带来额外查找、验证工作，以及围绕过期引用的失败模式。但这些成本是显式的、局部化的；通常这比为了满足少数长生命周期 alias，就用 pointer-stable 容器污染整个表示更可取。

## 失效规则是 API 设计的一部分

容器不只是存储机制。它还会创造或破坏 API 保证。返回指向 `std::vector<T>` 的 `std::span<T>`，是在告诉调用方：这个视图只有在底层存储仍存活且没有发生会使其失效的变更时才有效。返回哈希表中的迭代器，会把 rehash 敏感性暴露出去。返回 node 容器里的引用，则会暴露对象生命周期和同步方面的假设。

这就是为什么表示与接口设计无法彻底分离。如果一个模块希望自己保有压缩、排序、重建或重新分配的内部自由，它就不该随意泄露原始迭代器或长生命周期引用。当实现需要自由移动时，应优先返回值、短生命周期的基于回调的访问、复制出来的摘要，或 opaque handle。

ranges 让这件事更整洁，但也更容易被误用。一个 view pipeline 看起来可以非常函数式，但它仍可能借用某段存储，而那段存储的生命周期比 pipeline 的使用范围更窄。如果底层数据位于瞬时 buffer、query object 或请求局部 arena 里，那么一个写得很漂亮的 range 表达式，仍然可能是生命周期 bug。存储模型依然是第一位的。

## 在热路径上，稠密数据胜过聪明的对象模型

当团队把问题领域建模得过于字面时，数据密集型系统往往会退化。一个日志处理阶段会被建成由对象图、virtual method 和分散所有权组成的结构，因为领域听起来像是面向对象的。上了负载之后，profiler 报出的不是昂贵的算术，而是缓存未命中和 allocator 抖动。

对于热路径，优先选择能让主导遍历简单、稠密的表示。一个数据包分类器，可能把解析后的头字段放在紧凑数组中，而把罕见的扩展数据放在别处。一个推荐引擎，可能把不可变的 item 特征与请求局部的评分 buffer 分开。一个订单簿，如果价格带宽界足够有限，可能会把价格档位放进按归一化 tick 偏移索引的连续数组，而不是用堆节点组成的树。

这些设计看上去可能不如对象图优雅。它们通常更诚实。硬件执行的是内存流量，不是类图。

## 实际中的缓存不友好结构

值得准确看看一个缓存不友好结构究竟长什么样，以及它为什么会伤性能，因为这种失败模式很常见，而代码评审里又看不出来。

```cpp
// A naive priority queue built from scattered heap nodes.
// Each node is individually allocated and linked by pointer.
struct Task {
	int priority;
	std::string description;  // may allocate on heap
	Task* next;
};

class NaivePriorityQueue {
	Task* head_ = nullptr;
public:
	void insert(int priority, std::string desc) {
		auto* node = new Task{priority, std::move(desc), nullptr};
		// Sorted insert: walk the list to find position.
		// Each step dereferences a pointer to a random heap location.
		Task** pos = &head_;
		while (*pos && (*pos)->priority <= priority)
			pos = &(*pos)->next;
		node->next = *pos;
		*pos = node;
	}

	// Find highest-priority task.  Cheap -- it is at the head.
	// But any operation that scans (e.g., "remove by description",
	// "count tasks above threshold") pointer-chases through
	// potentially thousands of cold cache lines.
	Task* top() const { return head_; }

	~NaivePriorityQueue() {
		while (head_) { auto* n = head_; head_ = head_->next; delete n; }
	}
};
```

把它和一个把相同数据连续存放的缓存友好替代方案比较一下：

```cpp
struct TaskRecord {
	int priority;
	std::string description;
};

class FlatPriorityQueue {
	std::vector<TaskRecord> tasks_;
public:
	void insert(int priority, std::string desc) {
		tasks_.push_back({priority, std::move(desc)});
		// Could maintain sorted order with std::lower_bound + insert,
		// or just push_back and sort/partial_sort when needed.
	}

	// Rebuild top in O(n) but with contiguous memory access.
	// For scan-heavy workloads this dominates pointer-chasing.
	const TaskRecord& top() const {
		return *std::min_element(tasks_.begin(), tasks_.end(),
			[](const auto& a, const auto& b) {
				return a.priority < b.priority;
			});
	}
};
```

flat 版本在 `top()` 上可能会做更多比较，但它是在连续内存上流式完成这些比较。实际中，对于几千个元素以下的集合，flat scan 往往能击败“链表版本的 O(1) 头访问 + O(n) 插入”，因为链表版本的插入会为访问到的每个节点都付出一次缓存未命中的代价。对更大的集合，`std::priority_queue`（它把连续堆包在 `std::vector` 中）正是为这个原因而成为标准工具。

一般性的教训是：pointer-linked 结构会在每一步遍历时都缴纳一次每节点税，这在算法分析里不可见，却常常主导真实时间。

## 常见失败模式

有几类反复出现的错误，值得明确点名。

第一类，是按操作速查表而不是按可测的负载形状选容器。“需要快速查找”太模糊了。多少元素？键分布是什么？遍历频率多高？重建频率多高？没有这些数字，“快”就没有内容。

第二类，是因为单个 struct 看起来更整洁，就把热字段和冷字段混在一起。当代码每秒遍历数百万元素时，紧凑布局并不是过早优化。

第三类，是让偶然出现的 alias 决定表示。少量长生命周期指针，不该逼着整个系统都转向 node-based 存储；如果一个 handle 层就能把这个需求隔离出来，就应当这么做。

第四类，是把视图当作生命周期抽象。它们不是。`std::span` 和 ranges 让非拥有访问变得显式；它们本身并不会让非拥有访问变得安全。

第五类，是对单个 microbenchmark 过拟合。某种表示在隔离的查找测试里获胜，在解码、过滤、聚合和序列化相互作用的完整 pipeline 里，却可能输得很惨。

## 在真实代码中要验证什么

表示层面的工作，应该体现在代码评审和测量计划里，而不只是体现在实现里。

评审者应当追问：

- 哪些操作主导时间和内存流量？
- 所选容器是在优化这些操作，还是只让某个局部调用点更方便？
- 稳定地址真的是必需的吗，还是代码通过 alias 泄露了表示约束？
- 哪些失效规则现在已经成了 API 契约的一部分？
- 热字段在物理上是否被放在一起，还是扫描时仍在把冷状态拖进缓存？
- 如果返回的是视图，哪些存储与变更条件限定了它们的生命周期？

下一章会把这些问题变成一个成本模型。现在，核心点更简单：表示决策通常是一阶性能决策。在讨论 allocator、内联或基准测试方法之前，先把数据形状做对。

## 要点总结

- 从主导访问模式出发，而不是从容器传闻出发。
- 对扫描密集和读多写少的负载，默认优先连续存储。
- 把稳定地址和稳定迭代器视为昂贵需求，必须有正当理由。
- 当重复遍历让带宽成为瓶颈时，分离热数据和冷数据。
- 当身份必须跨越存储移动而保持时，刻意使用 handle 或间接层。
- 把失效规则和视图生命周期视为表示在 API 层面的后果。
