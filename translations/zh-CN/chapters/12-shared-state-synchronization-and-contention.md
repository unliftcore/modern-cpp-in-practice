# 共享状态、同步与争用

本章假定你已经能在单线程代码中准确地推理所有权与不变量。现在要面对的问题是：当多个线程都能观察和修改同一份状态时，哪些结论还靠得住？

## 生产问题

大多数并发故障的根源不是缺少同步原语，而是共享策略不清晰。

缓存”基本都是读操作”。连接池”只有一把 mutex”。metrics registry 为了”提速”用了 atomics。请求协调器把几个计数器和一个队列放在锁后面，代码评审时看起来没什么问题。然后生产负载来了——锁持有时间把本不相关的工作耦合在一起；读线程看到了只更新了一半的状态，因为不变量横跨了两个字段；后台清理路径和热路径共用同一把 mutex。还没等谁看到崩溃，吞吐量就先垮了；等崩溃真正出现时，也不是什么干净的失败，而是数据竞争导致的未定义行为、死锁或线程饥饿。

本章探讨的是生产级 C++23 系统中共享可变状态的设计取舍。核心问题不是该记住哪种 mutex 类型，而是：究竟有多少状态需要共享？哪些不变量必须同步保护？真实流量下争用会如何表现？当共享不可避免时，怎样的设计才经得起评审？

注意与后续章节的边界。本章聚焦于共享可变状态的并发访问。第 13 章讨论协程在挂起点上的生命周期管理。第 14 章讨论任务组编排、取消与背压。这些主题虽然互有关联，但不可混为一谈。

## 共享可变状态是成本中心

共享状态换来的是协调上的便利，付出的是耦合的代价。

一旦两个线程可以修改同一个对象图，局部推理就不够用了。每次访问都牵涉同步策略、锁顺序、内存序、唤醒行为和析构时序。评审者不仅要考虑某个操作自身是否正确，还要追问：两步操作之间，状态会不会被别人看到？回调会不会在持锁时发生重入？等待中的代码是不是正好持有别人推进所需的资源？争用会不会把一个正确的设计拖成慢设计？

既然代价如此之高，并发设计的第一个决定就应该是结构性的：

1. 这份状态能不能改为线程私有（thread-confined）？
2. 更新能不能批处理、分片、快照化，或者走消息传递？
3. 如果共享不可避免，哪些不变量必须原子地维护？

很多团队上来就选锁，这是本末倒置。真正昂贵的抉择在于共享拓扑的设计，而不是 `std::mutex` 怎么写。

## 先缩小共享面

最安全的共享状态，就是压根不共享的状态。

在服务端代码中，很多看似需要共享的结构，其实可以按流量键、请求生命周期或所有权角色拆分。metrics 采集可以先按线程各自聚合，再定期合并。会话表可以按会话 ID 分片。缓存可以把不可变的值 blob 和一个小型可变索引分离。队列消费者可以持有自己的本地工作缓冲区，只对外发布已完成的快照。

这些做法比换用不同的同步原语更有价值，因为它们从根本上减少了"正确性依赖于线程交错顺序"的代码位置。

在生产系统中，以下三种缩减方式尤其常见：

### 线程私有化

让一个线程或一个 executor 独占修改权，其他方通过消息、快照或所有权移交来通信。对于请求调度器、连接管理器和事件循环来说，这往往是最简单的方案。好处不仅仅是减少了锁，更在于不变量始终保持在局部范围内。

### 分片

按稳定的键对状态做分区，让争用程度与热点键的集中度成正比，而非与总流量成正比。分片不会消除同步需求，但能缩小每个临界区的影响范围。

### 快照化

如果读远多于写，且读方能容忍轻微的数据滞后，就发布不可变快照，在旁路完成更新。读方获得低开销且稳定的访问，写方只需承担一次性的复杂度成本。

这些方式都有代价。线程私有化可能制造瓶颈，分片会让跨分片操作更复杂，快照化会增加内存分配和拷贝开销。但这些都是看得见的成本，远好过在各处暗中承受意外争用。

## 没有同步时会发生什么

在讨论该用哪种原语之前，先看看完全没有同步保护会怎样。下面的代码存在数据竞争，在 C++ 中属于未定义行为。

```cpp
// BUG: data race — two threads read and write counter without synchronization.
struct metrics {
	int request_count = 0;
	int error_count = 0;
};

metrics g_metrics;

void record_request(bool success) {
	++g_metrics.request_count;            // unsynchronized read-modify-write
	if (!success) ++g_metrics.error_count; // same
}
```

这不只是正确性风险——按照标准，这就是未定义行为。编译器和硬件完全可以对这些操作进行重排、撕裂或省略。实际表现上，计数器可能丢失更新、报告不可能的值，甚至在不支持原子字存储的架构上破坏相邻内存。Sanitizer 能立刻检测到这类问题，但 sanitizer 不一定在生产环境中开启。

一个更隐蔽的变体涉及多字段不变量：

```cpp
// BUG: readers can observe state_ == READY while payload_ is half-written.
struct shared_result {
	std::string payload_;
	enum { EMPTY, READY } state_ = EMPTY;
};

// Writer thread:
result.payload_ = build_payload();   // not yet visible to readers
result.state_ = READY;              // may be reordered before payload_ write

// Reader thread:
if (result.state_ == READY)
	process(result.payload_);        // may see partially constructed string
```

即使 `state_` 是原子的，如果内存序不对，`payload_` 的写入仍可能被重排到 `state_` 之后。这里的教训是：数据竞争不只是单个变量的问题，更是相关修改之间可见性顺序的问题。

## 裸 mutex 操作的误用与 RAII guard

手动 lock/unlock 是最经典的 mutex bug 来源。看这个例子：

```cpp
// BUG: exception between lock and unlock leaks the lock.
std::mutex mtx;
std::vector<int> data;

void push(int value) {
	mtx.lock();
	data.push_back(value); // may throw (allocation failure)
	mtx.unlock();          // never reached if push_back throws — deadlock on next access
}
```

一旦 `push_back` 抛出异常，`unlock()` 就被跳过了。之后所有试图获取 `mtx` 的线程都将永久阻塞。这绝非假想场景——内存压力下的分配失败，或者一个会抛异常的拷贝构造函数，都足以触发它。

修复方式很机械：使用 RAII guard。

```cpp
void push(int value) {
	std::scoped_lock lock(mtx);
	data.push_back(value); // if this throws, ~scoped_lock releases the mutex
}
```

`std::scoped_lock` 可以处理单个或多个 mutex，并自带死锁规避。`std::unique_lock` 在此基础上增加了延迟加锁、转移所有权和配合 condition variable 的能力。除非确实需要这些额外灵活性，否则优先选择 `scoped_lock`。

```cpp
// unique_lock: needed when the lock must be released before scope exit.
void transfer_expired(registry& reg, std::vector<session>& out) {
	std::unique_lock lock(reg.mutex_);
	auto expired = reg.extract_expired(); // modifies registry under lock
	lock.unlock();                        // release before expensive cleanup
	for (auto& s : expired)
		s.close_socket();                 // no lock held — safe to block
	// out is caller-owned, no synchronization needed
	out.insert(out.end(),
		std::make_move_iterator(expired.begin()),
		std::make_move_iterator(expired.end()));
}
```

## 不一致的加锁顺序导致死锁

当代码需要获取多把 mutex 时，加锁顺序不一致就是最经典的死锁根源。

```cpp
// BUG: deadlock if thread 1 calls transfer(a, b) while thread 2 calls transfer(b, a).
struct account {
	std::mutex mtx;
	int balance = 0;
};

void transfer(account& from, account& to, int amount) {
	std::lock_guard lock_from(from.mtx); // locks 'from' first
	std::lock_guard lock_to(to.mtx);     // then 'to' — opposite order on another thread
	from.balance -= amount;
	to.balance += amount;
}
```

线程 1 锁住 `a.mtx`，等着拿 `b.mtx`；线程 2 锁住 `b.mtx`，等着拿 `a.mtx`——谁也走不了。`std::scoped_lock` 内部使用 `std::lock` 同时获取两把 mutex 来规避死锁：

```cpp
void transfer(account& from, account& to, int amount) {
	std::scoped_lock lock(from.mtx, to.mtx); // deadlock-free acquisition
	from.balance -= amount;
	to.balance += amount;
}
```

这不只是方便，而是正确性保障。任何需要多把 mutex 的设计，要么用 `std::scoped_lock` 一次性获取，要么就严格执行有文档记录的全局加锁顺序。靠口头约定的加锁规矩，很少能挺过一轮重构。

## 过度同步的性能成本

争用不仅关乎正确性，过度加锁还会把本可并行的工作强制串行化。

```cpp
// Over-synchronized: every stat update contends on one lock.
class request_stats {
	std::mutex mtx_;
	uint64_t total_requests_ = 0;
	uint64_t total_bytes_ = 0;
	uint64_t error_count_ = 0;
public:
	void record(uint64_t bytes, bool error) {
		std::scoped_lock lock(mtx_);
		++total_requests_;
		total_bytes_ += bytes;
		if (error) ++error_count_;
	}
};
```

在一台每秒处理数百万请求的 64 核机器上，所有线程都在同一条 cache line 上排队。锁获取、cache line 反复弹跳和调度器唤醒成了主要开销。更好的设计取决于你能容忍什么：

- 如果字段之间不需要精确一致性，就让每个线程各自计数，定期合并。
- 如果只需要近似总数，就对每个计数器独立使用 `std::atomic<uint64_t>` 配合 `memory_order_relaxed`。
- 如果需要跨字段一致性（例如错误率 = errors / total），就保留 mutex，但按线程或请求键分片。

重点不是 mutex 慢，而是一把所有核心共享的 mutex 会把并行负载变成串行瓶颈。要分开测量锁持有时间和等待时间——等待时间高、持有时间低，就是过度同步的典型信号。

## 围绕不变量设计，而不是围绕字段设计

锁保护的不是变量，而是不变量。

这一区分至关重要，因为生产环境中的对象很少在单个字段层面出错。真正出问题的场景是：多个字段必须一起修改，而某个线程恰好在修改之间观察到了中间状态。

连接池的正确性不在于 `available_count` 是不是原子的，而在于并发访问下以下关系始终成立：已借出的连接不会同时出现在空闲列表中；已关闭的连接不会被重新分发；当有可用资源时，等待者能被唤醒。这些才是不变量。如果设计没有明确列出这些不变量，同步边界就已经定义不清了。

正因如此，粗粒度加锁有时反而更优。如果一把 mutex 能干净地覆盖一个完整的不变量域，它可能比几把细粒度锁更好——后者容易导致不可能的中间状态，或者要求脆弱的锁顺序。细粒度加锁不见得更优，往往只是更难审查。

## 反模式：不断膨胀的服务对象外面包一把锁

最常见的失败模式不是”没有同步”，而是”一把起初看着合理的锁，随着功能迭代逐渐变成了整个服务的瓶颈”。

```cpp
// Anti-pattern: one mutex protects unrelated invariants and long operations.
class session_registry {
public:
	std::optional<session_info> find(session_id id) {
		std::scoped_lock lock(mutex_);
		auto it = sessions_.find(id);
		if (it == sessions_.end()) {
			return std::nullopt;
		}
		return it->second;
	}

	void expire_idle_sessions(std::chrono::steady_clock::time_point now) {
		std::scoped_lock lock(mutex_);
		for (auto it = sessions_.begin(); it != sessions_.end();) {
			if (it->second.expires_at <= now) {
				close_socket(it->second.socket); // RISK: blocking work under the lock.
				it = sessions_.erase(it);
			} else {
				++it;
			}
		}
	}

private:
	std::mutex mutex_;
	std::unordered_map<session_id, session_info> sessions_;
};
```

这个对象在早期测试中可能毫无问题，因为局部看起来很简单。后来出问题，是因为不相关的工作被迫共享了同一个排队点。一次读操作被清理任务阻塞；清理任务在持有 mutex 的同时做 I/O；后续新功能还会不断往同一个临界区里塞 metrics、回调和日志——因为"锁已经在那了"。

问题不只是临界区太长，还在于这个对象根本没有清晰的不变量边界。生命周期管理、查找、过期和副作用全部塞进了同一个同步域。

更好的做法通常是把状态变更与外部动作分离：在锁内确定哪些会话该过期，将它们移出共享结构，释放锁，然后再关闭 socket。这样既缩短了锁的范围，也让不变量更好表述：受保护区域只负责更新 registry；外部清理在所有权转移之后进行。

## 最小化锁作用域，但不要盲目拆分逻辑

“让锁的范围尽量小”这句话对，但不完整。

临界区应当恰好包含维护不变量所需的操作，一步不多、一步不少。具体而言：

1. 不要在持锁时做阻塞 I/O。
2. 不要在持锁时回调外部代码。
3. 如果能移到锁外，就别在临界区里跑大量分配或写日志的慢路径。
4. 不要仅仅为了让临界区看起来更短，就拆散逻辑上必须原子完成的状态更新。

最后一点恰恰是团队最容易踩坑的地方。保护多步不变量的锁可能确实需要横跨多个操作。如果为了追求”更快”的观感而在中间释放锁，就可能引入不可能出现的中间状态。先弄清楚什么必须是原子的，再谈优化。

## Atomics 适合狭窄事实，不适合复杂所有权

Atomics 很关键，也很容易被误用。

适合用 atomics 的场景是共享信息确实很简单：停止标志、版本计数器、环形缓冲区索引、所有权模型已经完善的引用计数，或者只需要 relaxed 语义的统计计数器。不要把 atomics 当作结构化所有权的替代方案，也不要用它来取代多字段不变量的保护。

示例项目的 `TaskRepository`（`examples/web-api/src/modules/repository.cppm`）清楚地展示了这一区分。ID 生成器是一个单调递增计数器——教科书式的狭窄事实——因此使用 `std::atomic<TaskId>` 配合 `memory_order_relaxed`。而任务集合本身是一个多字段不变量（vector 内容必须与已发放的 ID 保持一致），所以由 `shared_mutex` 保护。在同一个类中混用这两种策略完全正确，因为它们的作用域互不重叠：atomic 负责一个独立的事实，mutex 负责其余所有状态。

一个原子计数器不能让队列变安全。一个原子指针也不能让对象生命周期变简单。几个 `memory_order` 参数修不好一个本来就允许线程看到半成品状态的设计。

C++23 在这方面提供了实用工具，包括 `std::atomic::wait`、`notify_one` 和 `notify_all`，可以为简单的状态转换省去一些 condition variable 的样板代码。但这不改变一个根本前提：你仍然必须先把状态机设计清楚。

如果评审者说不清哪些值转换是合法的、为什么所选的内存序就够用，那这段原子代码就还没写完。

## 读多写少的数据需要不同的设计

争用的根源，与其说是写入频率高，不如说是读取路径设计不当。

配置表、路由映射或特性策略快照，每个请求都可能读取，但很少更新。用一把中心 mutex 保护它们，功能上没问题，却会引入本可避免的尾延迟。这类场景下，不可变快照或 copy-on-write 式发布往往比更细粒度的加锁更能产出好的系统。

权衡很明确：

1. 读方获得稳定、低争用的访问。
2. 写方承担拷贝和发布的开销。
3. 由于新旧版本共存，内存压力可能上升。
4. 业务上必须能容忍一定的数据滞后。

这在请求路由、鉴权策略和读多写少的元数据场景中，通常是正确的取舍。但对于写入密集的订单簿或频繁变更的共享索引，就不适用了。

当两种极端都不适用时——读取频繁，但每次 create、update 或 delete 请求都会写入——`std::shared_mutex` 配合读端 `std::shared_lock`、写端 `std::scoped_lock` 就是务实的折中方案。示例项目的 `TaskRepository`（`examples/web-api/src/modules/repository.cppm`）正是这种模式的体现：

```cpp
// repository.cppm — 读写锁的实际用法
class TaskRepository {
    mutable std::shared_mutex mutex_;
    std::vector<Task>         tasks_;
    std::atomic<TaskId>       next_id_{1};
public:
    // 读操作使用 shared_lock — 多个读者可以并行执行。
    [[nodiscard]] std::optional<Task> find_by_id(TaskId id) const {
        std::shared_lock lock{mutex_};
        auto it = std::ranges::find(tasks_, id, &Task::id);
        if (it == tasks_.end()) return std::nullopt;
        return *it;
    }

    // 写操作使用 scoped_lock — 独占访问以维护不变量。
    [[nodiscard]] Result<Task> create(Task task) {
        std::scoped_lock lock{mutex_};
        // 校验、分配 id、存储……
    }
};
```

所有读路径（`find_by_id`、`find_all`、`find_completed`、`size`）获取 `shared_lock`，允许并发读取。所有写路径（`create`、`update`、`remove`）获取 `scoped_lock`，独占访问。mutex 保护的是不变量域——`tasks_` 的内容与已分配 ID 之间的关系——而非单个字段。

对应的压力测试（`examples/web-api/tests/test_repository.cpp`）在并发负载下验证了这一设计：

```cpp
void test_concurrent_access() {
    webapi::TaskRepository repo;
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 100;

    std::vector<std::jthread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&repo, i]() {
            for (int j = 0; j < ops_per_thread; ++j) {
                auto title = std::format("Task-{}-{}", i, j);
                auto result = repo.create(webapi::Task{.title = std::move(title)});
                assert(result.has_value());
            }
        });
    }
    threads.clear(); // jthread 在析构时自动 join
    assert(repo.size() == num_threads * ops_per_thread);
}
```

八个线程并发执行 `create`，然后校验总数与预期一致。这是一个基线正确性测试，而非争用基准测试——但它能在 ThreadSanitizer 下暴露数据竞争和丢失更新。

## Condition variable 与唤醒规范

Condition variable 是很多原本严谨的设计开始变得模糊的地方。

规则本身很简单：等待谓词是不变量的一部分，而不是随手写的便利表达式。等待线程必须重新检查一个谓词，而这个谓词必须受到同一个同步域的保护——正是这个同步域赋予了谓词意义。通知只是"请重新检查"的信号，不是"一定有进展"的保证。

具体来说：

1. 明确命名谓词：队列非空、已请求关闭、容量可用、版本已变更。
2. 先更新谓词对应的状态，再发通知。
3. 等待代码要能正确应对虚假唤醒和关闭时的竞争。
4. 唤醒一个等待者还是全部等待者，要与进展模型匹配。

大多数有问题的 condition variable 代码，病根不在于作者忘了写循环，而在于谓词本身定义不清，或者被分散到了多条代码路径中、由不同逻辑各自更新。

## 隐藏的共享状态仍然是共享状态

并发 bug 经常潜伏在那些从调用方角度看不出是共享的对象中。

常见的例子：

1. 多线程共用的分配器或 memory resource。
2. 内部带缓冲区的日志 sink。
3. 带共享控制块的引用计数句柄。
4. 隐藏在看似无副作用的辅助函数背后的缓存。
5. 用于插件发现、metrics 或 tracing 的全局 registry。

这些对象应该和显式共享的 map、queue 接受同等审查。”这个辅助类是线程安全的”远远不够，还要追问：它会不会把所有调用方都串行化？它在争用下会不会触发内存分配？它有没有可能在持有内部锁时回调用户代码？它是否在热路径上引入了 API 表面看不出来的争用开销？

## 测量争用会改变设计

正确性只是第一关。过了这关之后，共享状态的设计就变成了测量问题。

争用很少在源码层面一眼看出。它表现为排队延迟、锁持有时间分布、convoy 效应、cache line 乒乓，以及调度器层面的停顿。这意味着验证工作必须包括运行时证据：

1. 在热路径上分别测量锁持有时间和等待时间。
2. 关注尾延迟，而不仅仅是吞吐量均值。
3. 使用分片时，观察热点键的倾斜程度。
4. 分析临界区内是否存在内存分配。
5. 用 ThreadSanitizer 做竞争检测，用有针对性的压力测试覆盖死锁和饥饿场景。

一个逻辑上正确、但在 P99 就扛不住的设计，仍然是不合格的并发设计。

## 共享状态的评审问题

在审批涉及并发共享状态的代码之前，问自己：

1. 每把锁、每个 atomic 保护的不变量到底是什么？
2. 这份状态能否改为线程私有、分片或快照，而不是直接共享？
3. 有没有临界区在执行 I/O、大量分配、写日志或回调？
4. 站在观察者角度，跨字段更新是真正原子的吗？
5. Condition variable 的谓词是否准确，并在正确的同步域下更新？
6. 突发流量或热点键倾斜时，争用会出现在哪里？
7. 除了”跑过测试”之外，还有什么证据？

如果这些问题没有明确答案，这个设计就还没准备好上生产。

## 要点

共享可变状态不是并发设计的默认选择，而是代价高昂的选择。

当共享不可避免时，先定义不变量，再选原语。优先考虑线程私有化、分片和快照，而非堆砌越来越巧妙的锁。用 mutex 保护不变量域，用 atomics 表达简单事实，condition variable 只在谓词清晰定义后才使用。最后，在真实负载下测量结果——因为即便同步本身是正确的，如果争用主导了系统行为，这个设计仍然是失败的。
