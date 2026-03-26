# 分配、局部性与成本模型

## 生产问题

数据形状确定之后，下一个性能问题是：字节从哪里来，又要搬动多少次。很多团队一看到”这条路径很慢”，就直接跳去调 allocator 或设计对象池，却连成本模型都没建立。这是本末倒置。在动分配策略之前，你得先弄清楚瓶颈到底在哪：是分配延迟、allocator 内部的锁同步、page fault、对象散落引发的缓存和 TLB miss、过大值类型的拷贝开销，还是抽象层叠加出来的指令成本。

本章就是要建立这样一个模型。重点不是背 allocator API，而是具体推演某个设计到底会让机器干什么。”`std::function` 零开销””arena 总是更快””小分配现在很便宜”——这些都不算工程论证。真正的论证从这里开始：你能说清楚实际的对象图长什么样、分配发生在何时何处有多少次、相关对象的所有权跨度有多长，以及每多一层间接访问会对局部性造成怎样的影响。

注意和上一章划清界线。第 15 章问的是”该用什么表示”，本章问的是”选定了某种表示之后，它在运行过程中会带来哪些成本”。容器仍然会出现，但侧重点是分配频率、生命周期聚类、对象图深度和局部性，而非容器语义本身。

## 从分配清单开始

第一个有用的性能模型简单到令人不好意思说出口：把你关心的路径上所有会触发分配的地方列出来。

绝大多数代码库在这方面做得都比自以为的差。请求解析器为每个 header value 分配字符串；路由层用类型擦除 wrapper 存回调；JSON 转换过程物化出中间对象；日志路径往临时 buffer 里做格式化。单看每个决策可能都说得过去，但加在一起，一个请求还没开始做真正的业务工作，就已经执行了几十甚至上百次堆分配。

示例项目中的 `parse_request()`（`examples/web-api/src/modules/http.cppm`）就是这种模式的具体体现。每次调用都会为每个 header 的名称和值各分配一个 `std::string`（第 191 行：`headers.emplace_back(std::string(name), std::string(value))`），再加上 path 和 body 各一个 `std::string`。一个带十个 header 的请求，在任何 handler 开始执行之前就至少需要十二次堆分配。这是 PMR 优化的天然候选场景：用一个以栈上 buffer 为后端的 `std::pmr::monotonic_buffer_resource`，可以让所有这些字符串都从同一个 arena 中获取内存，彻底消除逐 header 的 allocator 调用，并在请求作用域结束时以一次性批量操作完成析构。

做清单时要区分三个层面的问题：

- 稳态热路径上，哪些操作会触发分配？
- 哪些分配属于一次性初始化或批量重建的成本？
- 哪些分配可以通过调整所有权或数据流来消除，而不是靠换一个更好的 allocator？

第三个问题最关键。如果系统频繁分配的根因在于它硬要把一段密集处理拆成大量短命的堆对象，那换 allocator 只是止痛，治不了病。杠杆最大的改进，往往是从根本上消除对这些分配的需求。

下面用一个具体例子来展示热路径上分配密集型代码的样子，以及如何改写成低分配版本：

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

轻量版有一系列约束：输入 buffer 的生存期必须覆盖所有 view，tag 数量有上限，metadata 也要换种方式处理。这些约束就是免去分配所付出的代价。值不值得取决于具体负载，但把代价摆到明面上——这才是关键。

## 分配成本不只是调用 `new`

工程师谈到分配时，常常觉得成本就是 allocator 函数调用那一下。在生产环境里，这往往只是总账的一小部分。分配还会影响缓存局部性、同步行为、碎片化、页面工作集，以及后续的销毁开销。如果对象图把逻辑上相邻的数据散落到毫不相关的堆地址上，之后每次遍历都在为这个决定买单。如果每个请求的分配都从多个线程撞进同一个全局 allocator，allocator 争用就会变成延迟抖动的来源。如果大量短命对象逐个销毁，burst 期间光是清理流量就可能主导尾延迟。

所以”我们上了 pool，问题就解决了”这种说法往往站不住脚。pool 或许能降低 allocator 调用开销甚至减少争用，但如果最终的对象图依然指针满天飞、布局零散，遍历照样很贵。反过来，把请求局部状态都放到连续 buffer 里的设计，哪怕用默认 allocator，也可能分配极少、局部性极好。

## 生命周期聚类通常胜过聪明的复用

同生共死的对象，最好也一起分配。这就是 arena 和 monotonic resource 背后的核心直觉：如果一批数据共享同一个生命周期边界，为逐个释放付费就是白花功夫。请求局部的 parse tree、临时 token buffer、图搜索的 scratch state、一次性编译元数据，都是经典适用场景。

C++23 在这方面的标准词汇仍以 `std::pmr` 为主。它的价值不在风格，而在架构。memory resource 让你能表达"这一族对象属于同一个生命周期区域"，而无需把自定义 allocator 类型硬编码到每一个模板实例里。

```cpp
struct RequestScratch {
    std::pmr::monotonic_buffer_resource arena;
    std::pmr::vector<std::pmr::string> tokens{&arena};
    std::pmr::unordered_map<std::pmr::string, std::pmr::string> headers{&arena};

    explicit RequestScratch(std::span<std::byte> buffer)
        : arena(buffer.data(), buffer.size()) {}
};
```

这个设计表达了一件重要的事：这些字符串和容器不是各自独立的堆上居民，而是请求作用域内的临时工作区。这样既降低了分配开销，也让析构变成一次性的批量操作。

下面用一个更完整的例子来展示差异。对比请求处理路径上，标准分配与带栈上 buffer 的 `pmr` 方案有何不同：

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

只要请求能装进栈上 buffer，`pmr` 版本就能完全消除逐对象释放，也彻底绕开全局 allocator 争用。在高吞吐的小请求服务里，allocator 开销可以降低一个数量级。代价是：`std::pmr` 容器多带一个指向 memory resource 的指针（`sizeof` 略大），且 monotonic resource 不回收单次释放的空间——只会一直增长，直到 resource 本身被销毁。用在请求作用域的临时工作区上完全合适；用在会随时间反复增缩的长生命周期容器上就是错误选择。

但 monotonic allocation 绝非万能升级。对象需要选择性释放时不适用；某个病态请求引发的内存尖峰不应该撑大稳态占用时不适用；只要误留一个对象就会把整个 arena 拖住不放时也不适用。区域分配让生命周期假设变得更刚性——一旦假设错了，后果比逐对象管理更严重。

## 局部性关注的是图形状，不只是原始字节数

分配次数低不代表局部性就好。少量大块分配里装的若是指向各自独立分配节点的指针数组，遍历时不断跨页跳转，可能比大量小分配还糟。因此成本模型还得多问一个问题：热代码走查这个数据结构时，要经过多少次指针解引用才能碰到有用的数据？

指针密集的设计在语义上很诱人，因为它直接映射领域关系：树指向子节点，多态对象指向实现，pipeline 串着一连串堆分配的 stage。有时确实别无他法，但更多时候，不过是给偷懒披上了建模的外衣。

出路不是”永远别用指针”，而是把身份与拓扑关系同底层存储分开。图可以用连续节点数组加索引邻接表来存储。若操作集合是已知的，多态 pipeline 通常可以表示成一个小而封闭的 `std::variant` step 类型。字符串密集的解析器可以对重复 token 做 intern，或保留指向稳定输入 buffer 的 slice，而不必为每个字段都分配一份自有字符串。

这些都不是语言层面的奇技淫巧，而是图形状层面的设计决策，目的是减少真正有用的工作开始前需要追着指针跑的次数。

## `std::shared_ptr` 的隐藏成本

`std::shared_ptr` 值得单独拿出来说，因为它的实际成本被低估得太频繁了。最容易看到的是分配成本：`std::make_shared` 把控制块和托管对象合并为一次分配，而从裸指针构造则要分配两次。但分配只是冰山一角。

更深层的成本来自引用计数。每拷贝一次 `std::shared_ptr` 就做一次原子递增，每销毁一次就做一次带 acquire-release 语义的原子递减。在 x86 上，单次原子递增并不贵（一条 locked 指令，无争用时约 10-20 ns），但一旦跨核共享，控制块所在的缓存行就会在核心之间来回弹跳。争用激烈时，本该并行的工作会被串行化。

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

规则不是”永远别用 `std::shared_ptr`”，而是：不要用共享所有权来逃避生命周期思考。对象有明确的所有者和借用者时，就用唯一所有者加引用或视图来表达。`std::shared_ptr` 应该留给那些确实需要共享、且生命周期无法静态确定的场景。另外，当 `const&` 或裸引用就够用时，千万别按值传 `std::shared_ptr`——每次拷贝都是一次毫无收益的原子往返。

示例项目对这一原则的实践值得参考。在 `examples/web-api/src/modules/handlers.cppm` 中，每个 handler 工厂（如 `list_tasks()`、`get_task()`、`create_task()`）都通过引用接收 `TaskRepository&`，并在返回的 lambda 中以引用方式捕获。repository 由 `main()` 持有，生命周期覆盖所有 handler，因此完全不需要 `std::shared_ptr<TaskRepository>`。这样每次请求都避免了原子引用计数的开销，handler 的捕获也更紧凑——只是一个普通指针，而不是两指针宽的 `shared_ptr` 外加控制块。

还有一些零碎但容易忽视的开销：`std::shared_ptr` 自身就有两个指针宽（对象指针 + 控制块指针），是裸指针的两倍大。容器里装 `std::shared_ptr` 时缓存密度自然更差。弱引用计数又多出一个原子变量。控制块里存放的自定义 deleter 还会在析构时引入一层类型擦除间接调用。

## 隐式分配是设计异味

现代 C++ 提供了大量抽象，但只有当成本模型在代码评审中仍然可见时，这些抽象才用得安心。问题不在于抽象本身，而在于：有些抽象的分配行为是隐式的、随负载变化的，或者以团队从未关注过的实现定义方式发生。

`std::string` 可能分配堆内存，也可能靠 small-string 优化放在栈上。`std::function` 对大 callable 可能分配，对小的则未必。类型擦除 wrapper、协程帧、regex 引擎、locale 相关的格式化，以及基于 stream 的组合，都可能在调用点完全看不出来的情况下触发分配。

这些类型本身没有问题。危险在于把它们用在热路径上却缺乏明确的成本依据。如果一个服务每收到一条消息都构造一个 `std::function`，或者仅仅因为下游 API 默认要求所有权，就反复把稳定的字符串 slice 复制成自有 `std::string`，那真正的症结不只是”分配太多”——而是 API 接口把成本的入口藏了起来。

审视热路径上的抽象，应该像审视线程同步一样认真。问自己：

- 这个 wrapper 会不会触发堆分配？
- 它是否强制增加了一层间接访问？
- 它会不会把对象撑大到明显降低打包密度？
- 同样的行为能不能用封闭方案替代，比如 `std::variant`、模板边界或借用视图？

最终答案取决于代码体积、ABI、编译时间和替换灵活性。关键是把这笔权衡账算到明面上。

## 池、freelist 复用及其失败模式

池化的吸引力在于它许诺了复用和可预测性。有时这个许诺是兑现的。固定大小对象池在以下条件下确实有效：分配尺寸一致、对象生命周期短、复用频繁、且 allocator 争用确实是问题。类似 slab 的设计也可能比完全通用的堆分配提供更好的空间局部性。

但 pool 的失败方式有迹可循。

对象大小差异大到需要多个池、或内部碎片把收益吃掉——失败。pool 掩盖了无界保留，对象名义上”以后会复用”，实际复用率低到内存永远回不去系统——失败。每线程 pool 在负载倾斜时让均衡变得更复杂——失败。代码开始围绕 pool 的可用性来编排生命周期，而不是围绕领域所有权——失败。开发者觉得”有 pool 了”就不再测量——同样失败。

实操规则很直白：池化是为已知的负载形状服务的，不是一种通用性能姿态。如果你说不清分配分布和复用模式，那就还没到设计 pool 的时候。

## 值大小与参数表面仍然重要

分配只是成本模型的一部分。在 API 之间被随手拷来拷去的大值类型，杀伤力同样不小。一个”图方便”的 record 类型塞了好几个 `std::string`、可选 blob 和 vector，靠 move semantics 或许能省掉部分堆流量，但工作集照样膨胀、缓存压力照样增大、按值传递的代价照样变高。

这里就要回到第 4 章的 API 指导了。当真正的契约是所有权转移或廉价移动时，按值传递很好。但如果一条路径反复拷贝或移动大型聚合对象，仅仅是图个通用方便，那就很糟了。成本模型必须把对象大小、移动开销，以及数据穿越各层边界的频率都纳入考量。

小值天然容易在系统中流转。大值则更适合做稳定存储，再通过借用访问、提取摘要，或拆分冷热部分来使用。如果这些做法让 API 变复杂了，这种复杂度往往也是值得的。性能设计里有太多案例表明：某一层追求”干净”的接口，结果在其他所有层面制造了本可避免的开销。

## 用于评审的实用成本模型

生产实践中，非形式化但写清楚的模型通常就足够了。对于正在评审的路径，把以下内容写下来：

- 每次操作在稳态下的分配次数。
- 这些分配是线程局部的、全局争用的，还是藏在抽象背后的。
- 分配出的对象按生命周期如何分组。
- 热遍历路径上要经过多少次指针间接访问。
- 热工作集大约多大。
- 遍历模式是连续、跨步、哈希查找还是图遍历。
- 销毁方式是逐个、批量还是按区域整体释放。

这份清单给不出精确到时钟周期的预测，但能终结空口白话。它让评审者有能力区分”这看起来有成本”和”这个设计在 burst 负载下必然导致 allocator 流量激增、读取分散、teardown 行为恶化”。

## 边界条件

成本模型不是过度特化的通行证。有些时候堆分配就是对的——对象确实活过了局部作用域，确实参与了共享所有权。有些时候类型擦除就是正确的权衡——为了跨库边界的可替换性。有些时候 arena 分配反而不合适——保留风险或调试复杂度可能比吞吐收益更值得担心。

目标不是把局部速度压榨到极致，而是让成本在真实系统压力下可预测、可解释。如果某个设计稍微抬高了稳态开销，却在非热点边界上大幅改善了正确性或可演进性，这完全可能是对的选择。成本模型的存在是为了支撑权衡决策，而不是消灭权衡。

## 在调优之前要验证什么

在引入自定义 allocator、对象池或大面积铺设 `pmr` 之前，先验证四件事。

第一，确认这条路径确实够热，分配和局部性在量级上真的构成问题。第二，确认当前设计的分配行为和数据布局确实如你所想。第三，确认相关对象确实共享你设想的 allocator 策略所依赖的生命周期形状。第四，确认新方案不会只是把成本搬到别处——更大的驻留内存、更差的调试体验、更复杂的所有权边界。

下一章直接进入证据环节。成本模型终究只是假设，要靠基准测试和性能剖析去验证正确的假设，才能把它变成真正的工程。

## 要点总结

- 动用 allocator 技术之前，先做分配清单。
- 分配成本包括延迟、局部性、争用、保留和 teardown，而不只是调一次 `new` 的代价。
- 对象确实同生共死时，就按生命周期聚类分配。
- 当区域内存策略与所有权模型匹配时，用 `std::pmr` 来表达——别把它当装饰性的现代感。
- 对热路径上隐藏分配和间接访问的抽象保持警惕。
- pool 要为已测明的负载形状而设计，否则就不要设计。
