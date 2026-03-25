# 不自欺的基准测试与性能剖析

## 生产问题

当团队把数字误当成证据时，性能讨论就会变得昂贵。一个基准测试报告了 20% 的收益，但生产服务并没有改进。一个 profiler 显示某个函数是热点，但真正的问题却是锁争用或 off-CPU 等待。一个回归溜进主干，因为基准测试测错了输入形状，或者因为有人“优化”了编译器早已移除的死工作。

本章讨论的是测量纪律。前几章讨论的是表示与成本建模。这里的问题不同：你如何收集足够强的证据，去修改代码、证明复杂度合理，或否决一个所谓的优化？答案要求基准测试设计、对 profiler 的基本素养，以及拒绝让好看的图表替代因果推理。

现代 C++ 中的性能工作尤其容易自我欺骗，因为这门语言暴露了许多强大的局部变换。你可以改变容器类型、所有权形状、内联边界、allocator 策略、range pipeline、协程结构，以及类型擦除选择。这些变化有些重要，很多并不重要。测量就是用来区分它们的。

## 为问题选择正确的仪器

并不是每个性能问题都该从 microbenchmark 开始。

如果你是在为一个紧凑循环里的两种数据表示做选择，受控基准测试可能正合适。如果你在调试为什么请求延迟会在突发负载下飙升，那么对真实系统做剖析，或者收集生产 trace，才更合适。如果你怀疑是锁争用，那么 scheduler 行为和阻塞时间比单独的吞吐循环更重要。如果一个回归只会在端到端服务流量下出现，那么人为隔离出来的合成基准测试甚至会主动误导你。

使用一个简单的层级：

- 对范围很窄、隔离良好的问题，使用 microbenchmark。
- 用 profiler 找出一个进程里时间或 sample 实际流向了哪里。
- 用类生产负载测试来观察队列、线程、I/O、缓存和争用之间的相互作用。
- 用生产可观测性确认这个变化在真正付账的环境里是否有意义。

把这些层次混淆，是浪费数周时间的最快方式之一。

## 基准测试必须先说明它的主张

可信的基准测试从一句话开始，而不是从代码开始。“比较在 1k、10k 和 100k 条目、真实 key 长度条件下，读多写少路由表中，排序后连续存储的查找延迟与 hash 查找的差异。” 这是一个主张。“benchmark containers” 不是。

这句话应当指出：

- 被测操作。
- 数据大小与分布。
- 变更与读取的比例。
- 重要的机器或环境假设。
- 这个基准测试试图支撑的决策。

如果你写不出这句话，那你还不知道这个基准测试到底意味着什么。

这个要求之所以重要，是因为性能是由负载形状决定的。一个针对随机整数键的基准测试，可能对一个使用 `string_view`、并且前缀局部性很强的生产路由器毫无意义。一个针对均匀 hash 命中的基准测试，可能掩盖真实倾斜键的冲突行为。一个每次迭代都重建容器的基准测试，可能会惩罚某个设计——而在生产中，这个设计的成本主要是“一次构建后反复查找”。

## 对整个相关操作做基准测试

一个常见谎言，是测量某个方便的片段，而不是测量真正重要的决策边界。比如，一个解析 pipeline 被“优化”了，但测量的只有 token 转换，而且输入早已驻留在缓存里、分配也早已预留好。一次容器对比测的是查找，却排除了构建、排序、去重和内存回收，而生产负载明明会频繁重建这份结构。

基准测试不必很大，但它必须包含设计在现实中施加的成本。如果某个 API 选择会在你当前测量的那一行代码之前，强制进行分配、拷贝、hash 或校验，那么除非你能为排除这些成本给出正当理由，否则它们就属于测量的一部分。

这正是第 16 章中的成本模型该用来指导基准测试的地方。测量成本累积的真实边界。否则，结果在技术上也许正确，在操作上却毫无用处。

## 控制编译器与 harness 伪影

C++ 特别容易产生误导性的 microbenchmark，因为优化器非常乐于删除、折叠、提升和向量化那些没有被锚定到可观测行为上的代码。基准测试 harness 的存在，部分就是为了防止这一点，但这并不意味着你可以不再保持怀疑。

至少要做到：

- 确保结果会以编译器无法消除的方式被消费。
- 有意识地把一次性 setup 与每次迭代的工作分开。
- 足够 warm up，避免无意中测到 first-touch 效应。
- 控制数据初始化，让每次迭代都走到目标分支与缓存行为。
- 当结果出乎意料时，检查生成代码。

如果某个基准测试声称一个复杂操作几乎不耗时，那就在证实之前先假定优化器把工作删掉了。如果某个基准测试表现出巨大的方差，那就在证实之前先假定环境不稳定，或者负载没有说明清楚。

### 有缺陷的基准测试：死代码消除

这是最常见的 microbenchmark 谎言。编译器看到某个结果从未被使用，于是把整个计算都删掉了：

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

`benchmark::DoNotOptimize` 不是魔法。在大多数实现上，它的作用是把这个值做成一次不透明读取（通常是编译器会认为“可能观察了该变量”的 inline asm）。把它用在最终结果上，而不是用在每个中间步骤上；否则你就有可能抑制掉生产代码同样会受益的合法优化。如果你不确定 DCE 是否影响了结果，就用 `-S` 编译，并检查汇编。

### 有缺陷的基准测试：测到了 setup，而不是工作

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

### 有缺陷的基准测试：错误的 baseline

拿两个设计与一个不公平的 baseline 做比较，会更隐蔽，也更危险：

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

正确的 baseline 是现实中的替代方案，而不是最糟糕的选项。始终说明这个基准测试在和什么比较，以及为什么那个替代方案才是团队实际会选的东西。

### 有缺陷的基准测试：热缓存幻觉

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

如果生产访问模式会遇到冷数据，要么把工作集做大到超出缓存，要么在迭代之间显式刷新缓存行（平台相关且脆弱，但有时是获得诚实结果所必需的）。

### Google Benchmark 陷阱

Google Benchmark（`benchmark::`）被广泛使用，整体上也相当可靠，但有几个反复出现的错误值得点名：

1. **忘记 `benchmark::ClobberMemory()`**：`DoNotOptimize` 能防止某个值的 dead store 被消除，但它不会强迫编译器假设内存已经变化。如果你的基准测试会原地修改某个数据结构，编译器可能会把读取提升到写入之前，甚至跨迭代提升。变更之后调用 `benchmark::ClobberMemory()`，强制重新加载：

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

2. **没有使用 `state.SetItemsProcessed()`**：否则输出里只会有每次迭代耗时，很难比较处理了不同 batch 大小的基准测试。始终调用 `state.SetItemsProcessed(state.iterations() * num_items)`，让输出包含吞吐列。

3. **忽略 `state.PauseTiming()` / `state.ResumeTiming()` 的开销**：这些调用本身会读取时钟，在许多平台上就要 20-100 ns。如果你测的操作耗时不到一微秒，pause/resume 开销就会占主导。对于亚微秒级工作，要么把 setup 完全移到循环外，要么在很多次迭代上摊薄它。

4. **只测一个尺寸**：用 `->Range(8, 1 << 20)` 或 `->DenseRange()` 覆盖不同规模。一个在 1K 元素上获胜的设计，到了 1M 可能会输。性能不是一个标量。

在可能时，使用严肃的 harness。具体是哪一个库并不重要，重要的是纪律：稳定重复、清晰的 setup 边界，以及显式防止死代码消除。如果仓库并未标准化某个 harness，而某个基准测试又刻意保持为 partial，就要明确说出来，并记录被省略的 scaffolding。

## 测到的是噪声，而不是信号

即使基准测试本身正确，环境噪声也可能压过结果。频率缩放、热降频、后台进程、NUMA 效应和中断合并，都会注入与你的代码改动无关的方差。

实用防线：

- **在基准测试期间固定 CPU 频率**（Linux 上可以用 `cpupower frequency-set -g performance`，或关闭 turbo boost）。如果某个基准测试这次以 4.5 GHz 运行、下次以 3.2 GHz 运行，那它测到的是 governor，而不是你的代码。
- **隔离核心**（`isolcpus` 内核参数或 `taskset` / `numactl`），防止 scheduler 干扰。
- **运行多轮试验**，报告 median，而不是 mean。median 能抵抗由中断或 page fault 带来的偶发尖峰；mean 会被离群值拖偏。 
- **要求统计显著性**之后再宣布收益。变好 3%，但变异系数有 5%，那就是噪声。Google Benchmark 支持 `--benchmark_repetitions=N` 并报告 stddev；用它。
- **尽可能在同一台机器、同一次启动、同一个二进制上比较**。跨机器比较需要非常谨慎的归一化，通常也没那么可信。

如果某个基准测试在一台空闲机器上的两次相同运行之间变化超过 1-2%，那就应该先修好基准测试设置，再谈结果有何意义。

## 分布比单个数字更重要

平均运行时间是一个很弱的摘要。许多生产系统关心的是 percentile、方差，以及倾斜负载下的最坏行为。某个表示如果改善了平均吞吐，却在突发分配或锁争用下让尾延迟更差，那它仍然可能是一次回归。同样，一个只报告“每次迭代纳秒数”的基准测试，也可能掩盖由 rehash、page fault、分支预测器翻转，或偶发的大分配导致的双峰行为。

像读取可用性或延迟遥测那样读取性能数字。问的是分布，而不是仅仅问中心位置。问离群值到底是噪声、环境不稳，还是设计的真实行为。问基准测试的形状是否让那些罕见但昂贵的事件发生得足够频繁，以至于值得关心。

对 CI 回归控制来说，这意味着要谨慎使用阈值和趋势分析。一个噪声很大的基准测试会制造误报，训练团队忽视真正的信号。一个太宽松的阈值，则会让有意义的回归逐步积累。稳定的基准测试设计，通常比花哨的报告更有价值。

## Profiler 回答的是不同的问题

profiler 不是更慢的基准测试。它是一个采样或插桩工具，用来理解真实进程里的时间、分配、缓存未命中或等待发生在哪里。当你还不知道瓶颈在哪，或者某个 microbenchmark 结果需要在完整系统行为中得到验证时，就该用它。

不同 profiler 暴露的是不同失败类别：

- CPU sampling profiler 回答的是活跃 CPU 时间花在了哪里。
- allocation profiler 回答的是哪些路径在分配并保留内存。
- 能感知硬件计数器的工具，回答的是缓存未命中、分支预测失败或 stalled cycle 集中在何处。
- 并发与 tracing 工具，回答的是线程在哪里阻塞、等待或争用。

不要让一个工具去回答它根本看不见的问题。CPU profiler 无法解释为什么线程大多在等锁而处于空闲。allocation flame graph 不会告诉你：如果遍历成本仍然占主导，那么更快的 allocator 到底有没有意义。wall-clock trace 也许能显示某个请求很慢，却无法区分是 CPU 工作慢，还是 scheduler 延迟。

在 Linux 上，这可能意味着把 `perf`、allocator profiling 和 tracing 组合起来。在 Windows 上，可能意味着基于 ETW 的工具、Visual Studio Profiler 或 Windows Performance Analyzer。在 macOS 上，Instruments 起着类似作用。工具选择是次要的，习惯才是重点：让问题和真正能回答它的仪器配对。

## 让基准测试与性能剖析彼此约束

基准测试与性能剖析应当彼此约束。

如果一个 microbenchmark 说某个改动应该有帮助，因为它减少了分配，那么在真实进程上的 profiler 也应该显示分配变少了，或者在分配密集路径上的时间变少了。如果 profile 说某个循环因为 pointer-rich 遍历中的缓存未命中而变热，那么基准测试就应该隔离这种遍历形状并测试替代方案。如果两者不一致，不要把它们平均成一种安慰。去调查这种不匹配。

常见的不匹配原因包括：

- 基准测试的数据形状与生产不符。
- 基准测试隔离出的那项成本，在端到端里被别的成本淹没了。
- profiler 指向的是症状，而不是根因。
- 被测改动在完整二进制里对代码尺寸、内联或分支行为的影响，与隔离环境中不同。

好的性能工作会缩小这些差距。糟糕的性能工作会忽略它们。

## 警惕那些并不“具有代表性”的输入

团队常常会用整洁的合成输入来自毁测量。key 都是均匀随机的。消息大小都一样。队列从不 burst。哈希表从不遇到真实的 load factor。解析器从不看到 malformed 或 adversarial 数据。这些输入更容易生成，也更容易稳定；它们也往往是错的。

有代表性的输入，不意味着盲目复制生产流量。它意味着保留那些驱动成本的属性：尺寸分布、倾斜、重复、变更比例、工作集大小，以及失败路径频率。对缓存来说，这可能意味着 Zipf 风格的访问模式，而不是均匀 key。对解析器来说，这可能意味着短字段和长字段的真实混合，再加上少量 malformed record。对 scheduler 或 queue 来说，这可能意味着突发模式，而不是平坦的到达率。

如果由于数据隐私或操作约束而无法使用真实 trace，至少也要有意识地合成分布。一个建立在不真实输入上的基准测试并不是中立的。它会主动把团队训练到错误的问题上去。

## 性能主张必须能经得起代码评审

把性能改动当成可评审的设计工作，而不是英雄式实验。一个可信的改动，应当附带一份紧凑的证据包：

- 被回答的性能问题是什么。
- 基准测试或性能剖析的设置是什么。
- 负载假设是什么。
- 改动前后的结果是什么；相关时还要包括方差或 percentile 数据。
- 引入了哪些权衡：代码复杂度、内存占用、API 限制、可移植性或维护成本。

这会强制一种有用的纪律。它能阻止“在我机器上看起来更快”作为制度记忆进入代码库。它也会留下可重跑的工件，供未来评审者在编译器、标准库或负载形状改变之后重新验证答案。

## 回归控制是工程系统，不是仪表盘

很容易往 CI 里加一个 benchmark job，然后就宣布性能问题解决了。实际中，回归控制只有在被测基准测试稳定、运行成本足够低以适配正确频率，并且确实绑定到团队关心的代码路径时，才会真正有效。一个没人信任的、经常抖动的 nightly benchmark 套件，不是安全。那只是仪式。

一个实用的设置通常包括：一小组对已知热 kernel 的高度稳定 microbenchmark、一套更重的独立性能工作流用于更广泛的负载测试，以及上线后持续跟踪延迟、吞吐、CPU 时间和内存影响的生产可观测性。这几层在成本与保真度上各不相同。你需要三者兼备，因为没有哪一层单独就足够。

## 什么才是诚实的测量

诚实的测量是克制的。它不会试图从单个基准测试里得出普遍真理。它不会把 profile 热度直接等同于立刻归责。它不会因为某个优化在汇编里可见，就假定它真的重要。它会把一个数字同某种负载、某个问题和一项决策绑定起来。

这种态度比任何具体工具链都更重要。硬件会变化，编译器会进步，标准库实现会变化，生产流量也会演化。你想在 C++ 团队里培养的习惯，不是依附某一个 profiler 或 framework，而是拒绝在证据与当前决策不匹配时做出性能主张。

## 要点总结

- 选择与问题匹配的测量工具：benchmark、profile、负载测试或生产遥测。
- 在写 benchmark 代码之前，先用自然语言写清 benchmark 的主张。
- 测量完整的相关操作，而不是最方便测的片段。
- 把优化器伪影、harness 错误和不真实输入当成默认嫌疑人。
- 关注方差和 percentile，而不只是 mean。
- 要求性能改动附带一份可评审的证据包。
