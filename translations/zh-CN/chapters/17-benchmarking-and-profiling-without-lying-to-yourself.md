# 不自欺的基准测试与性能剖析

## 生产中的问题

团队一旦把数字误当成证据，性能讨论的代价就会急剧上升。基准测试号称有 20% 的收益，生产服务却纹丝未动。Profiler 指向某个热点函数，真正的瓶颈却是锁争用或 off-CPU 等待。回归悄悄混进主干——要么因为基准测试选错了输入形状，要么因为有人”优化”了编译器早已删掉的无用计算。

本章的主题是测量纪律。前几章谈的是数据表示与成本建模，而这里要回答的问题不一样：怎样才能拿到足够有力的证据，来决定是否改代码、是否值得引入复杂度、或是否该否决一个看似有效的优化？这需要严谨的基准测试设计、对 profiler 的基本功，以及不被漂亮图表蒙蔽因果判断的定力。

现代 C++ 的性能工作尤其容易自欺欺人，因为语言本身提供了大量强力的局部优化手段：容器类型、所有权模型、内联边界、allocator 策略、range pipeline、协程结构、类型擦除方案，都可以随手调整。其中有些改动确实有效，但很多根本不起作用。测量，就是用来区分两者的。

## 先看问题，再选工具

并不是每个性能问题都该从 microbenchmark 入手。

如果要在一个紧凑循环中比较两种数据表示，受控基准测试可能恰到好处。如果要排查请求延迟在突发负载下飙升的原因，对真实系统做性能剖析或收集生产 trace 才更合适。如果怀疑是锁争用，scheduler 行为和阻塞时间远比单独跑一个吞吐循环有说服力。如果回归只在端到端服务流量中才出现，人为隔离出来的合成基准测试反而会把你引向歧途。

可以按以下层级来选择：

- 范围窄、隔离充分的问题，用 microbenchmark。
- 想知道时间或采样点实际流向了哪里，用 profiler。
- 想观察队列、线程、I/O、缓存和争用之间的相互作用，用类生产负载测试。
- 想确认改动在真正跑业务的环境里是否有效，用生产可观测性。

搞混这些层次，是浪费数周时间的最快方式之一。

## 基准测试必须先说清要验证什么

可信的基准测试从一句话开始，而不是从代码开始。比如：”在 1k、10k、100k 条目、真实 key 长度条件下，比较读多写少路由表中排序连续存储与 hash 查找的延迟差异。”这就是一个明确的主张。而”benchmark containers”则不是。

这句话应当包含：

- 被测操作是什么。
- 数据规模与分布如何。
- 读写比例是多少。
- 有哪些重要的机器或环境假设。
- 这次测试要为哪个决策提供依据。

如果你写不出这句话，说明你还没想清楚这个基准测试到底在测什么。

之所以强调这一点，是因为性能天然受负载形状制约。用随机整数键跑的基准测试，对一个使用 `string_view` 且前缀局部性很强的生产路由器可能毫无参考价值。只测均匀 hash 命中的基准测试，会掩盖真实倾斜键下的冲突行为。每次迭代都重建容器的基准测试，会不公平地惩罚那些”一次构建、反复查找”的设计。

## 测量完整的相关操作

一个常见的自欺手法是只测方便测的片段，却绕过真正关键的决策边界。比如，一个解析 pipeline 号称被”优化”了，但测的只是 token 转换，而且输入早已在缓存中、内存分配也早已预留好。又比如，容器对比只测了查找，却把构建、排序、去重和内存回收全部排除在外——而生产负载恰恰会频繁重建这个结构。

基准测试不必规模很大，但必须涵盖设计在实际使用中带来的成本。如果某个 API 选择会在你目前测量的那行代码之前强制触发分配、拷贝、hash 或校验，那么除非你有充分理由将其排除，否则这些成本就应当纳入测量范围。

这正是第 16 章成本模型该发挥作用的地方——沿着成本真正累积的边界去测量。否则，结果在技术上也许没错，但对实际决策毫无帮助。

在实际项目中，值得做基准测试的位置往往不像 `vector<int>` 那么明显，但也更有价值。以 `examples/web-api/` 示例项目为例：

- **`json::serialize_array()`**（`json.cppm`）遍历一个 range 并通过反复拼接字符串来构建 JSON 数组。用不同集合规模（10、100、1000 个 task）来 benchmark 这个函数，可以揭示是拼接策略还是逐元素 `to_json()` 占主导，以及预分配结果字符串是否有意义。
- **`TaskRepository::find_by_id()`**（`repository.cppm`）在 `shared_lock` 下用 `std::ranges::find` 做线性扫描。如果要把它和 `unordered_map<TaskId, Task>` 方案做对比，就必须把加锁成本包含在内，并在真实的 repository 规模下测试——而不只是测裸 find 操作。
- **`Router::to_handler()`**（`router.cppm`）通过值捕获路由表，返回一个在每次请求分发时做线性扫描的 lambda。在 5、50、500 条注册路由下 benchmark 路由分发，可以看出线性扫描是否仍可接受，还是需要换成排序 vector 或 trie。这个 benchmark 必须涵盖完整的分发路径——匹配 method、比较 pattern、调用 handler——而不只是循环本身。

这就是值得在写代码之前用一句话说清楚的 benchmark 主张："在 5、50、500 条路由、真实 HTTP method 和 path 分布条件下，比较线性扫描与排序 vector 的路由分发延迟。"

## 警惕编译器和测试框架引入的假象

C++ 的 microbenchmark 特别容易产生误导，因为优化器会毫不犹豫地删除、折叠、外提和向量化那些没有锚定到可观测行为上的代码。基准测试框架（harness）的存在，部分就是为了防止这些问题，但这不意味着可以放松警惕。

至少要做到以下几点：

- 确保计算结果以编译器无法消除的方式被使用。
- 刻意将一次性初始化与每轮迭代的工作分开。
- 充分预热，避免无意中测到 first-touch 效应。
- 控制数据初始化方式，保证每次迭代都能命中目标分支和缓存行为。
- 结果出乎意料时，检查生成的汇编代码。

如果某个基准测试声称复杂操作几乎不耗时，在排除其他原因之前，先假定是优化器把计算删掉了。如果基准测试结果波动极大，先假定是环境不稳定或负载定义不够明确。

### 典型反面教材：死代码消除

这是 microbenchmark 中最常见的陷阱。编译器发现某个结果从未被使用，就直接把整段计算删掉了：

```cpp
// BROKEN: the compiler may eliminate the entire loop because
// 'total' is never observed.
static void BM_bad_dce(benchmark::State& state) {
	std::vector<double> data(1'000'000, 1.0);
	for (auto _ : state) {
		double total = 0.0;
		for (double d : data)
			total += d * d;
		// total is dead.  Optimizer removes the loop.
		// Benchmark reports ~0 ns/iteration.
	}
}
```

```cpp
// FIXED: benchmark::DoNotOptimize prevents the compiler from
// proving the result is unused.
static void BM_good_dce(benchmark::State& state) {
	std::vector<double> data(1'000'000, 1.0);
	for (auto _ : state) {
		double total = 0.0;
		for (double d : data)
			total += d * d;
		benchmark::DoNotOptimize(total);
	}
}
```

`benchmark::DoNotOptimize` 并非万能。在大多数实现中，它本质上是对目标值做一次不透明读取（通常通过 inline asm 让编译器认为该变量”可能被观察”）。应当只在最终结果上使用，而不是对每个中间步骤都加——否则可能抑制生产代码同样能受益的正常优化。如果拿不准 DCE 是否影响了结果，用 `-S` 编译后检查汇编即可。

### 典型反面教材：测的是初始化，不是真正的工作

```cpp
// BROKEN: construction cost dominates. The benchmark is
// measuring vector allocation and initialization, not lookup.
static void BM_bad_lookup(benchmark::State& state) {
	for (auto _ : state) {
		std::vector<int> v(1'000'000);
		std::iota(v.begin(), v.end(), 0);
		auto it = std::lower_bound(v.begin(), v.end(), 500'000);
		benchmark::DoNotOptimize(it);
	}
}

// FIXED: setup goes outside the timing loop.
static void BM_good_lookup(benchmark::State& state) {
	std::vector<int> v(1'000'000);
	std::iota(v.begin(), v.end(), 0);
	for (auto _ : state) {
		auto it = std::lower_bound(v.begin(), v.end(), 500'000);
		benchmark::DoNotOptimize(it);
	}
}
```

### 典型反面教材：选错了比较基线

拿两个设计跟一个不公平的基线做比较，问题更隐蔽，危害也更大：

```cpp
// MISLEADING: comparing hash lookup against linear scan.
// Concludes "hash map is 100x faster" -- but the real alternative
// in production is sorted vector with binary search, which may
// be within 2x and uses half the memory.
static void BM_linear_scan(benchmark::State& state) {
	std::vector<std::pair<int,int>> data(100'000);
	// ... fill with random kv pairs, unsorted ...
	for (auto _ : state) {
		auto it = std::find_if(data.begin(), data.end(),
			[](const auto& p) { return p.first == 42; });
		benchmark::DoNotOptimize(it);
	}
}
```

正确的基线应当是实际可能采用的替代方案，而不是最差的选项。每次做比较都要说清楚：在跟什么比？为什么选它作为对照？

### 典型反面教材：热缓存幻觉

```cpp
// MISLEADING: data fits in L2 cache and is hot from the previous
// iteration.  Production accesses the same structure after
// processing unrelated data that evicts it from cache.
static void BM_warm_cache(benchmark::State& state) {
	std::vector<int> v(1'000);  // ~4 KB, fits in L1
	std::iota(v.begin(), v.end(), 0);
	for (auto _ : state) {
		int sum = 0;
		for (int x : v) sum += x;
		benchmark::DoNotOptimize(sum);
	}
	// Reports ~50 ns.  In production, with cache-cold data,
	// the same operation takes 10-50x longer.
}
```

如果生产环境中实际会遇到冷数据，就要么把工作集做大到超出缓存容量，要么在迭代之间显式刷新缓存行（依赖平台、比较脆弱，但有时为了拿到真实结果别无选择）。

### Google Benchmark 常见陷阱

Google Benchmark（`benchmark::`）使用广泛、整体可靠，但有几个反复出现的错误值得专门说明：

1. **忘记 `benchmark::ClobberMemory()`**：`DoNotOptimize` 能阻止编译器消除某个值的 dead store，但不会让编译器认为内存内容已经改变。如果基准测试在原地修改数据结构，编译器可能将读取提到写入之前，甚至跨迭代重排。修改之后需要调用 `benchmark::ClobberMemory()` 来强制重新加载：

```cpp
static void BM_modify(benchmark::State& state) {
	std::vector<int> v(10'000, 0);
	for (auto _ : state) {
		for (auto& x : v) x += 1;
		benchmark::ClobberMemory();
		// Without ClobberMemory, the compiler could theoretically
		// observe that v is never read and eliminate the writes,
		// or combine multiple iterations into one.
	}
}
```

2. **没有调用 `state.SetItemsProcessed()`**：缺了它，输出只有每轮迭代耗时，很难横向比较不同 batch 大小的测试。记得始终调用 `state.SetItemsProcessed(state.iterations() * num_items)`，让输出多一列吞吐量。

3. **忽视 `state.PauseTiming()` / `state.ResumeTiming()` 自身的开销**：这两个调用内部要读取时钟，在很多平台上本身就需要 20-100 ns。如果被测操作耗时不到一微秒，pause/resume 的开销反而成了大头。对于亚微秒级的工作，要么把初始化完全移到循环外面，要么把开销摊到大量迭代中去。

4. **只测单一规模**：用 `->Range(8, 1 << 20)` 或 `->DenseRange()` 覆盖多种规模。1K 元素上胜出的设计，到 1M 时可能反而更慢。性能不是一个标量。

条件允许时，应该使用正规的测试框架。具体选哪个库不重要，重要的是纪律：稳定的重复执行、清晰的初始化边界、显式地防止死代码消除。如果项目尚未统一使用某个框架，而某个基准测试又刻意只覆盖部分场景，就要明确说明，并记录省略了哪些部分。

## 别把噪声当信号

即使基准测试本身没有问题，环境噪声也可能盖过真正的结果。CPU 频率缩放、热降频、后台进程、NUMA 效应、中断合并——这些都会注入与代码改动毫无关系的波动。

几条实用对策：

- **测试期间锁定 CPU 频率**（Linux 上用 `cpupower frequency-set -g performance`，或关闭 turbo boost）。如果一次跑在 4.5 GHz、下一次跑在 3.2 GHz，你测的是调频策略，不是代码。
- **隔离 CPU 核心**（`isolcpus` 内核参数，或用 `taskset` / `numactl`），避免调度器干扰。
- **多跑几轮**，报告中位数而不是均值。中位数不受中断或 page fault 引起的偶发尖峰影响；均值则容易被离群值拉偏。
- **先确认统计显著性**，再宣称有收益。提升 3%，但变异系数达到 5%，那就是噪声。Google Benchmark 支持 `--benchmark_repetitions=N` 并报告标准差，务必使用。
- **尽量在同一台机器、同一次开机、同一个二进制上做对比**。跨机器比较需要极其谨慎的归一化，通常可信度也打折扣。

如果在一台空闲机器上连续跑两次同样的测试，结果变化超过 1-2%，那就该先修好测试环境，再讨论结果的含义。

## 分布比单个数字更重要

平均运行时间是一个很粗糙的指标。许多生产系统真正关心的是分位数、方差，以及负载倾斜时的最坏表现。一个改动如果提升了平均吞吐，却在突发分配或锁争用场景下恶化了尾延迟，那它仍然算是回归。同理，只报告”每迭代纳秒数”的基准测试，可能掩盖 rehash、page fault、分支预测翻转或偶发大分配所造成的双峰分布。

看性能数据要像看可用性或延迟监控一样——关注分布，不要只盯着均值。要追问：离群值究竟是噪声、环境波动，还是设计本身的真实行为？基准测试的场景设置是否让那些罕见但代价高昂的事件出现得足够频繁？

对 CI 回归检测而言，阈值和趋势分析都需要谨慎设置。噪声大的基准测试会频繁误报，时间一长团队就会习惯性忽略警报。阈值太宽松，又会让有意义的回归悄悄积累。归根结底，稳定的基准测试设计比花哨的报表更有价值。

## Profiler 回答的是另一类问题

Profiler 不是"跑得更慢的基准测试"。它是采样或插桩工具，用来搞清楚真实进程中的时间、内存分配、缓存未命中或等待究竟发生在哪里。当你还不知道瓶颈在哪，或者需要在完整系统中验证某个 microbenchmark 结论时，就该请它出场。

不同类型的 profiler 能揭示不同类型的问题：

- CPU sampling profiler——CPU 活跃时间花在了哪里。
- Allocation profiler——哪些代码路径在分配和持有内存。
- 硬件计数器感知工具——缓存未命中、分支预测失败或 stalled cycle 集中在何处。
- 并发与 tracing 工具——线程在哪里阻塞、等待或争用。

不要指望一个工具回答它根本看不到的问题。CPU profiler 解释不了"为什么线程大部分时间在等锁"。Allocation flame graph 告诉不了你"如果遍历成本仍是大头，换个更快的 allocator 到底有没有用"。Wall-clock trace 也许能看到某个请求很慢，却分不清是 CPU 计算慢还是调度延迟。

在 Linux 上，这往往意味着将 `perf`、allocator profiling 和 tracing 组合使用。在 Windows 上，可能要用基于 ETW 的工具、Visual Studio Profiler 或 Windows Performance Analyzer。在 macOS 上，Instruments 承担类似角色。具体选哪个工具是次要的，关键是养成习惯：让问题和真正能回答它的工具配对。

## 让基准测试与性能剖析相互印证

基准测试和性能剖析应当互为校验。

如果 microbenchmark 说某个改动有效、因为减少了分配，那么在真实进程中 profiler 也应当显示分配减少，或分配密集路径上的耗时下降。如果 profile 显示某个循环因指针密集遍历的缓存未命中而成为热点，基准测试就应当隔离出这种遍历模式，并测试替代方案。如果两边结论矛盾，不要把它们折中成一种心理安慰——去查清不一致的原因。

常见的不一致原因：

- 基准测试的数据形状与生产环境不符。
- 基准测试隔离出的开销，在端到端场景中被其他开销淹没。
- Profiler 指向的是表面症状，而非根本原因。
- 改动在完整二进制中对代码体积、内联或分支行为的影响，与隔离测试中不同。

优秀的性能工作致力于缩小这些差距；糟糕的性能工作选择无视它们。

## 警惕名不副实的”代表性”输入

团队常常用过于整洁的合成输入来自毁测量。Key 全是均匀随机的，消息大小整齐划一，队列永远不突发，哈希表从不遇到真实负载因子，解析器也从不碰到格式错误或恶意构造的数据。这类输入容易生成、容易稳定，但往往也是错的。

“有代表性”不等于盲目复制生产流量，而是要保留那些真正影响成本的特征：大小分布、数据倾斜、重复模式、读写比例、工作集规模、异常路径出现的频率。对缓存而言，可能需要 Zipf 式访问模式而非均匀分布。对解析器而言，可能需要长短字段混合再加上少量格式错误的记录。对调度器或队列而言，可能需要突发到达而非匀速到达。

如果因数据隐私或运维限制无法使用真实 trace，至少也要有意识地构造具有代表性的分布。基于失真输入的基准测试不是中立的——它会把团队引向错误的优化方向。

## 性能主张必须经得起代码评审

性能改动应当作为可评审的设计工作来对待，而不是个人英雄式的实验。一个可信的改动，应当附带一份简明的证据包：

- 要回答的性能问题是什么。
- 基准测试或性能剖析是怎么设置的。
- 负载假设是什么。
- 改动前后的对比结果，必要时包含方差或分位数数据。
- 引入了哪些权衡：代码复杂度、内存占用、API 限制、可移植性、维护成本。

这会形成一种良性约束。它能防止”在我机器上好像更快了”这种说法变成代码库里的既定事实。同时也留下了可复现的素材，方便未来评审者在编译器、标准库或负载特征发生变化后重新验证结论。

## 回归控制是工程体系，不是仪表盘

往 CI 里加一个 benchmark job、宣布"性能问题已解决"，做起来很容易。但实际上，回归控制只有在以下条件同时满足时才真正有效：被测基准足够稳定、运行成本能匹配合理的执行频率、而且确实覆盖到团队关心的代码路径。一个经常波动、没人信任的 nightly benchmark 套件，提供的不是安全保障，而只是一种仪式。

务实的做法通常分三层：一小组高度稳定的 microbenchmark 覆盖已知热点路径；一套更重的独立性能工作流用于更广泛的负载测试；以及上线后的生产可观测性，持续跟踪延迟、吞吐、CPU 时间和内存表现。三层在成本和保真度上各有侧重，缺一不可——没有哪一层能独自胜任。

## 什么才是诚实的测量

诚实的测量是克制的。它不会试图从一个基准测试中提炼普适结论，不会把 profile 上的热度直接等同于问题根源，也不会仅因为某个优化在汇编里看得见就认定它有意义。它做的事情很简单：把一个数字和特定的负载、特定的问题、特定的决策绑定在一起。

这种态度比任何具体的工具链都重要。硬件在换代，编译器在进步，标准库实现在迭代，生产流量也在演变。一个 C++ 团队真正需要养成的习惯，不是执着于某个 profiler 或 framework，而是在证据与决策不匹配时，绝不轻率地做出性能声明。

## 要点总结

- 根据问题选择匹配的测量手段：benchmark、profile、负载测试或生产遥测。
- 写代码之前，先用一句话说清这个 benchmark 要验证什么。
- 测量完整的相关操作，而非只测最方便的片段。
- 默认怀疑优化器假象、测试框架错误和失真输入。
- 关注方差和分位数，不要只看均值。
- 要求每个性能改动都附带可评审的证据包。
