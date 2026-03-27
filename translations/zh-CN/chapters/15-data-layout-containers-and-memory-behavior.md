# 数据布局、容器与内存行为

## 生产问题

C++ 中的许多性能问题，表面上看是算法选择之争，实则是数据表示的失败。团队还在讨论某次查找该是 $O(1)$ 还是 $O(\log n)$，真正的问题早已暴露在别处：热路径上堆节点的逐一遍历、过多缓存行（cache line，CPU 缓存的最小读写单位，通常 64 字节）的触碰、每次迭代中冷字段的无谓搬运。编译器无法弥补糟糕的数据形状，只能在既有的形状上做有限优化。

本章关注的是决定数据如何驻留在内存中的一系列决策：容器选择、元素布局、迭代顺序、失效行为，以及视图如何在不掩盖生命周期的前提下暴露存储数据。不是抽象地讨论”哪个容器最快”，而是一个更具体的问题：在你实际构建的系统中，什么样的数据表示能让核心操作足够高效？

回答这个问题时，以真实的负载压力为参照。管理数百万路由条目的请求路由器、持续摄取高密度时序样本的遥测 pipeline、每帧扫描组件状态的游戏或模拟更新循环、逐批遍历事件的分析作业。在这些系统中，容器选择是性能契约的一部分。

## 当 Big-O 不再解释运行时间

渐近复杂度是必要的，但远远不够。它能排除明显低效的设计，却无法描述内存访问流量、分支预测命中率、prefetch 行为、false sharing 以及指针间接跳转的代价。`std::list` 的插入是常数时间，但如果每个有意义的操作都要在冷内存里追指针，这一点几乎毫无意义。对于中等规模的数据，排序后的 `std::vector` 常常能打赢哈希表，因为连续内存上的二分查找和廉价迭代带来的优势盖过名义上的查找复杂度差异。

这种错位很重要，因为生产环境的负载很少均匀。某些操作主导了总体开销，另一些操作对延迟敏感，多出几次缓存未命中可能比多做一次比较影响更大。选择容器之前，先用直白的语言描述清楚实际的负载特征：

- 主导操作是什么——全量遍历、单点查找、尾部追加、中间删除，还是批量重建？
- 数据构建完成后主要是只读的，还是会被持续修改？
- 是否需要稳定地址、稳定的迭代顺序，或可预测的失效规则？
- 数据集是小到能放进私有缓存，还是大到 TLB 和内存带宽已成为主要瓶颈？

如果这些问题没有答案，容器选择就只能靠经验传闻。

## 默认优先连续存储

对于频繁遍历的数据，连续存储应当是默认起点。`std::vector`、`std::array`、`std::span`、`std::mdspan`、flat buffer 和列式数组屡屡胜出，因为硬件偏爱可预测的访问模式。顺序扫描使处理器能高效 prefetch（预取）、摊薄 TLB 开销，并保持简单的分支行为。硬件层面的优势往往比理论上”更高级”的数据结构所带来的算法优势更大。

差距十分明显。一个简单例子：对元素数量相同的 `std::vector<int>` 和 `std::list<int>` 分别求和：

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

在典型硬件上处理一百万个元素时，纯遍历场景下 vector 版本通常比 list 版本快 10-50 倍。list 有每节点开销（大多数实现中每个元素附带两个指针加 allocator 元数据），但主导成本不在空间上。真正的瓶颈在于：推进到下一个节点时需要加载一个指针，而该指针指向的地址对 prefetcher 完全不可预测。每一步都可能触发一次 last-level cache miss，代价 50-100 ns；反观 vector 扫描，硬件 prefetcher 能识别顺序访问模式，几乎每次都命中 L1 或 L2 缓存。

这不是刻意构造的最坏场景，而是 `std::list` 在通用 allocator（分配器）下随时间分配节点的常态。换用 pool allocator 使节点在内存中更连续，效果也有限：next-pointer 的间接跳转和每节点的额外开销依然无法消除。

许多高性能设计初看起来都很"朴素"：把记录存进 `std::vector`，排序一次，之后通过二分查找或批量扫描来响应查询；热数据保持紧凑；用粗粒度批处理重建索引，而不是增量维护 pointer-rich 结构；尽可能将随机访问转化为规则性的顺序访问。

`std::vector` 不是放之四海而皆准，但举证责任通常在非连续方案一侧。需要 node-based 或 hash-based 结构时，理由应当具体：变更过程中需要迭代器稳定、确实存在大量中间位置的插入、需要并发所有权模式、外部 handle 必须在容器变更后仍然有效，或者查找模式的规模和稀疏度确实大到让 hashing 物有所值。

## 容器编码的是权衡，不只是操作

有经验的评审者应当把每个容器选择解读为一组承诺与代价。

`std::vector` 承诺连续存储、高效尾部追加和快速遍历。代价在于偶发的重分配、扩容时的迭代器失效，以及昂贵的中间删除。对于批处理、索引、稠密状态，以及可以重建或压缩的表，它通常是正确选择。

`std::deque` 牺牲严格的连续性，换来更高效的双端增长和避免整块缓冲区搬迁。对队列式负载可能有价值，但遍历时的局部性弱于 `std::vector`。扫描密集的场景中，把它当成”基本等同于 vector”是错误的。

`std::map` 和 `std::set` 等有序关联容器，提供的是稳定的排列顺序以及在频繁变更下仍然稳定的引用，代价则是节点分配、间接访问和分支密集的遍历。只有当排序在语义上不可或缺，或者变更模式使得”一次构建、定期重建”的策略不可行时，使用它们才有充分理由。对于读多写少数据上的热查找，它们不是好的默认选项。

`std::unordered_map` 和 `std::unordered_set` 放弃顺序，换取平均情况下更快的查找速度。但它们有实在的内存开销：bucket 数组、load factor 管理、许多实现中的节点存储，以及不可预测的 probe 行为。键空间较大且需要频繁精确查找时，它们有价值。但如果迭代是主要操作、内存占用很重要，或工作集小到排序后的连续数据完全放得进缓存，优势就不明显了。

C++23 新增了 `std::flat_map` 和 `std::flat_set`（分别在 `<flat_map>` 和 `<flat_set>` 头文件中），将生产代码库多年来的通行做法标准化：用排序后的连续键值数组做索引，读多写少的场景中往往表现更佳。C++23 之前，团队只能依赖 Boost.Container、Abseil 或自行实现的等价物。标准版本接受底层容器作为模板参数，可以根据局部性和分配需求选用 `std::vector`（默认）、`std::deque` 或 `std::pmr::vector` 作为后端。`std::flat_map` 的迭代器失效规则与 `std::vector` 一致，中间位置的插入由于元素搬移仍是 O(n)。它是为读优化设计的，不适合频繁写入。

## 元素内部的布局与容器本身同样重要

选对容器（比如 `std::vector<Order>`）还不够，`Order` 本身的结构照样可能浪费带宽。每次迭代只需要读取 `price`、`quantity` 和 `timestamp`，但每个对象还附带一个大 symbol 字符串、审计元数据、重试策略和调试状态，扫描这个 vector 时大量冷字节仍然会被拖进缓存。

这正是 hot/cold splitting（冷热分离）发挥作用的地方。原则很简单：时间上经常一起访问的字段在物理上也放到一起。不常用的状态移到单独的表、side structure 或 handle 背后能显著降低扫描成本，就应当这样做。不必过度抽象成通用的”entity system”，除非代码库确实需要。很多时候正确做法更直接：一个紧凑的热记录加一个存放冷元数据的辅助存储。

同样的性能压力也驱动着 array-of-structures（AoS，结构体数组）与 structure-of-arrays（SoA，数组结构体）之间的抉择。对象作为整体在系统中流转时，AoS 更易于理解和维护。处理本质上是列式的（过滤所有 timestamp、对所有 price 做计算、聚合所有计数器、为向量化 kernel 提供输入），SoA 则更具优势。数据表示应当匹配主导访问模式，而非迁就想象中的对象模型。

比较同一批数据的两种表示方式：

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

处理一百万条 tick 时，在现代 x86 硬件上做列式聚合，SoA 版本通常快 2-4 倍。根本原因是带宽利用率：AoS 循环每个元素加载约 48 字节却只用到 16 字节，每次缓存行读取有三分之二是浪费。SoA 循环只访问所需的两个 8 字节数组，两者都是完美的顺序访问。编译器也更容易为 SoA 版本生成 SIMD 指令，因为没有交错步长妨碍向量化。

SoA 的代价在别处。新增一条 tick 意味着要向每个列 vector 都追加一次，写起来繁琐也容易出错。想把”一条 tick”传给函数，要么传索引加整张表的引用，要么从各列临时拼装一个 struct。系统的主要操作是逐条记录处理且需要访问大部分字段时，AoS 布局既避免这种拼装开销，也让相关数据在内存中紧挨着。

一个实用的折中方案是 hot/cold splitting，而非完全转向 SoA：保留一个只包含热路径所需字段的紧凑”热” struct，冷字段放进按相同下标索引的并行 side table。

这种表示不因为”看起来更现代”就天然更好。优势只在特定条件下成立：负载确实会反复独立处理各列，或者紧凑的数值列能显著改善内存行为。下游逻辑频繁需要包含多个相关字段的完整 tick 对象时，数据拼装的成本或代码清晰度的损失完全可能抵消布局上的收益。

## 稳定 handle 很昂贵；先问你是否真的需要它们

许多糟糕的数据表示，根源在于一个未被明言的隐含需求：地址稳定性。代码把裸指针或引用存入容器元素，容器的选择不再由访问成本驱动，而被生命周期和失效问题绑架。这往往把设计推向 node-based 结构，地址是稳定了，其他路径上的局部性却全面恶化。

有时这种权衡合理。但更多时候，深层问题在于系统把对象身份与物理地址耦合了。组件需要持久的外部引用时，应当使用显式 handle、带 generation counter 的索引，或指向稳定间接层的 key。这样底层存储保持紧凑和可移动，外部引用也安全。

handle 不是零成本的，会引入查找步骤、校验逻辑和过期引用的错误处理。但这些成本显式且局部化，通常好过为了少数长生命周期的 alias 就让整个数据层被迫使用 pointer-stable 容器。

## 失效规则是 API 设计的一部分

容器不仅是存储机制，它还隐含地创造或破坏 API 保证。返回一个指向 `std::vector<T>` 内部的 `std::span<T>`，等于告诉调用方：只有底层存储存活且未发生导致失效的变更时，这个视图才有效。返回哈希表中的迭代器，把 rehash 敏感性暴露给了调用方。返回 node 容器中的引用，暴露了关于对象生命周期和同步的隐含假设。

数据表示与接口设计无法彻底分开。一个模块如果希望保留在内部压缩、排序、重建或重新分配的自由，就不该轻易向外暴露原始迭代器或长生命周期引用。实现需要移动数据的自由时，应优先采用返回值、短生命周期的回调式访问、拷贝出的摘要，或 opaque handle。

ranges 让非拥有访问的表达更整洁，但也更容易被误用。一条 view pipeline 表面上看起来是纯函数式的，背后可能引用着生命周期比 pipeline 更短的存储。底层数据位于临时 buffer、query object 或请求级 arena 中，一个写得再漂亮的 range 表达式仍然是生命周期 bug。存储模型才是第一位的。

## 在热路径上，稠密数据胜过聪明的对象模型

团队过于字面地对问题领域建模时，数据密集型系统的性能往往急剧退化。比如一个日志处理阶段，因为领域"听起来很面向对象"，就被建成了由对象图、virtual method 和分散所有权构成的体系。高负载下，profiler 暴露出的瓶颈不是昂贵的算术运算，而是大量的缓存未命中和 allocator 抖动。

热路径上，应优先选择能让主要遍历操作既简单又稠密的数据表示。数据包分类器可以把解析后的头字段存入紧凑数组，罕见的扩展数据另行存放；推荐引擎可以把不可变的 item 特征与请求级的评分 buffer 分离；订单簿在价格区间有界的情况下，可以把价格档位放入按归一化 tick 偏移量索引的连续数组，而非使用堆节点构成的树。

这些设计看上去可能不如对象图"优雅"，但它们更诚实地反映了硬件的工作方式。硬件执行的是内存访问，不是类图。

## 实际中的缓存不友好结构

仔细看看缓存不友好结构到底是什么样子、为什么会拖垮性能。这种失败模式很常见，而且在代码评审中难以发现。

```cpp
// A naive priority queue built from scattered heap nodes.
// Each node is individually allocated and linked by pointer.
struct Job {
    int priority;
    std::string description;  // may allocate on heap
    Job* next;
};

class NaivePriorityQueue {
    Job* head_ = nullptr;
public:
    void insert(int priority, std::string desc) {
        auto* node = new Job{priority, std::move(desc), nullptr};
        // Sorted insert: walk the list to find position.
        // Each step dereferences a pointer to a random heap location.
        Job** pos = &head_;
        while (*pos && (*pos)->priority <= priority)
            pos = &(*pos)->next;
        node->next = *pos;
        *pos = node;
    }

    // Find highest-priority job.  Cheap -- it is at the head.
    // But any operation that scans (e.g., "remove by description",
    // "count jobs above threshold") pointer-chases through
    // potentially thousands of cold cache lines.
    Job* top() const { return head_; }

    ~NaivePriorityQueue() {
        while (head_) { auto* n = head_; head_ = head_->next; delete n; }
    }
};
```

再来看一个将相同数据连续存放的缓存友好版本：

```cpp
struct Job {
    int priority;
    std::string description;
};

class FlatPriorityQueue {
    std::vector<Job> jobs_;
public:
    void insert(int priority, std::string desc) {
        jobs_.push_back({priority, std::move(desc)});
        // Could maintain sorted order with std::lower_bound + insert,
        // or just push_back and sort/partial_sort when needed.
    }

    // Rebuild top in O(n) but with contiguous memory access.
    // For scan-heavy workloads this dominates pointer-chasing.
    const Job& top() const {
        return *std::min_element(jobs_.begin(), jobs_.end(),
            [](const auto& a, const auto& b) {
                return a.priority < b.priority;
            });
    }
};
```

flat 版本调用 `top()` 时比较次数可能更多，但所有比较都是在连续内存上流式完成的。实践中，对于几千个元素以内的集合，flat scan 往往比链表版本的”O(1) 头访问 + O(n) 插入”更快，因为链表插入过程中每访问一个节点就可能触发一次缓存未命中。对于更大的集合，`std::priority_queue`（底层用 `std::vector` 维护一个连续堆）正是出于同样的原因而成为标准工具。

总结规律：pointer-linked 结构在每一步遍历时都要缴纳一次"节点税"——这在算法复杂度分析中完全不可见，却往往主导了实际的执行时间。

## 常见失败模式

有几类反复出现的错误。

第一类：根据操作速查表而非实际负载特征选容器。”需要快速查找”太笼统了。多少元素？键的分布如何？遍历多频繁？重建多频繁？没有这些数字，”快”毫无意义。

第二类：因为"放在一个 struct 里更整洁"就把热字段和冷字段混在一起。当代码每秒要遍历数百万个元素时，紧凑布局绝不是过早优化。

第三类：让偶然产生的 alias 决定整个数据表示。少量长生命周期的指针不应逼迫整个系统转向 node-based 存储——如果一个 handle 层就能把需求隔离开来，就应当这样做。

第四类：把视图误当作生命周期管理工具。它们不是。`std::span` 和 ranges 只是让非拥有式访问变得显式，但并不能让这种访问自动变得安全。

第五类：对单个 microbenchmark 过拟合。一种数据表示在孤立的查找测试中胜出，不代表它在解码、过滤、聚合和序列化相互交织的完整 pipeline 中同样出色，事实上可能差距悬殊。

## 在真实代码中要验证什么

数据表示层面的决策应当在代码评审和测量计划中有所体现，不只是埋在实现里。

评审者应当追问：

- 哪些操作主导了时间开销和内存流量？
- 所选容器是在优化这些主导操作，还是只为了让某个局部调用点写起来更方便？
- 地址稳定性是真正的需求，还是代码通过 alias 无意间泄露了内部的表示约束？
- 哪些失效规则已经成为 API 契约的组成部分？
- 热字段在物理上是否紧密相邻，还是每次扫描都在把冷状态拖进缓存？
- 如果返回了视图，是哪些存储条件和变更操作限定了视图的有效生命周期？

下一章将把这些问题转化为具体的成本模型。本章的核心观点很简单：数据表示往往是影响性能的第一要素。在讨论 allocator、内联策略或基准测试方法论之前，先把数据的形状设计对。

## 要点总结

- 从主导访问模式出发，而非从容器的经验传闻出发。
- 对扫描密集和读多写少的负载，默认优先选择连续存储。
- 将稳定地址和稳定迭代器视为昂贵的需求，使用时须有充分理由。
- 当反复遍历使带宽成为瓶颈时，将热数据与冷数据分离。
- 当对象身份需要在存储移动后依然有效时，有意识地引入 handle 或间接层。
- 将失效规则和视图生命周期视为数据表示在 API 层面的直接后果。
