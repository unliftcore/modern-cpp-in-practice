# 共享状态、同步与争用

本章假定你已经能在单线程代码中精确推理所有权和不变量。现在的问题是，一旦多个线程都能观察和变更同一份状态，还有哪些推理能够成立。

## 生产问题

大多数并发故障，并不是因为缺少原语。它们来自模糊的共享策略。

一个 cache“基本都是读”。一个连接池“只有一把 mutex”。一个 metrics registry 为了“提速”用了 atomics。一个请求协调器把几个计数器和一个队列放在锁后面，在代码评审里看起来似乎正确。然后生产负载来了。锁持有时间让无关工作彼此耦合。读者观察到一半更新的状态，因为不变量跨越了两个字段。后台清理路径与热路径共用同一把 mutex 来等待。在任何人看到崩溃之前，吞吐量就已经塌了；而等到崩溃真的出现时，往往也不是干净失败，而是数据竞争 UB、死锁或饥饿。

本章讨论的是生产 C++23 系统中共享可变状态的形状。核心问题不是记住哪种 mutex 类型，而是到底有多少状态应该被共享、哪些不变量需要同步、争用会如何在真实流量下出现，以及当共享不可避免时，什么样的设计才是可评审的。

请把与后续章节的边界保持清楚。本章讨论的是对共享可变状态的同时访问。第 13 章讨论的是挂起边界上的协程生命周期。第 14 章讨论的是任务组编排、取消和背压。这些主题会相互作用，但并不能互换。

## 共享可变状态是成本中心

共享状态用协调便利换来了耦合成本。

一旦两个线程都能变更同一对象图，仅靠局部推理就不够了。现在，每一次访问都依赖同步策略、锁顺序、内存顺序、唤醒行为和析构时机。评审者不仅要问某个操作单独看是否正确，还要问：状态是否会在两个步骤之间被观察到；回调是否会在持锁时重入；等待中的代码是否持有推进所需资源；以及争用是否会把一个正确的设计变成慢设计。

这种成本意味着，并发中的第一个决定应该是结构性的：

1. 这份状态能否改成线程限制（thread-confined）？
2. 更新能否批处理、分片、快照化，或通过消息传递完成？
3. 如果共享不可避免，哪个不变量必须以原子方式保持？

团队经常直接跳到锁选择。这是本末倒置。昂贵的选择是共享拓扑，而不是 `std::mutex` 的拼写。

## 先缩小共享表面

最安全的共享状态，是你根本不共享的状态。

在服务代码中，很多看似共享的结构，其实可以按流量键、请求生命周期或所有权角色拆开。metrics 摄取可以按线程聚合，再周期性合并。会话表可以按会话标识符分片。cache 可以把不可变值 blob 和一个小的可变索引分开。队列消费者可以拥有自己的本地工作 buffer，只发布完成后的快照。

这些动作比在原语之间切换更重要，因为它们减少了那些正确性依赖于交错执行的位置。

在生产系统中，尤其常见的三种缩减方式是：

### 线程限制

让一个线程或一个 executor 拥有变更。其他人都通过消息、快照或移交来通信。对请求调度器、连接管理器和事件循环来说，这往往是最简单的答案。收益不只是更少的锁，而是不变量能保持局部。

### 分片

按稳定键对状态做分区，使争用与热点键集中度成比例，而不是与总流量成比例。分片不会消除同步，但会缩小每个临界区的爆炸半径。

### 快照化

如果读者占主导，而且它们能容忍略微陈旧的数据，就发布不可变快照，并在旁边更新它们。读者获得廉价而稳定的访问。写者只承担一次复杂度成本。

这些方式都不是免费的。限制在单线程可能制造瓶颈。分片会让跨分片操作更复杂。快照化会增加分配和拷贝成本。但这些成本是显式的，总比在所有地方付出意外争用要好。

## 没有同步时会发生什么

在讨论该用哪种原语之前，先看看没有原语会怎样。下面的代码有数据竞争，在 C++ 中这属于未定义行为。

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

这不只是正确性风险；按标准它就是未定义行为。编译器和硬件都可以自由重排、撕裂或省略这些操作。在实践中，计数器可能会丢失更新、报告不可能的值，或者在采用非原子字存储的架构上破坏相邻内存。Sanitizer 会立刻标出这一点，但 sanitizer 并不总是在生产环境中运行。

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

即使 `state_` 是原子的，如果没有合适的内存顺序，`payload_` 的写入也可能被重排到它之后。教训是：数据竞争不只是单个变量的问题。它还是相关变更的可见性顺序问题。

## 原始 mutex 误用与作用域 guard 的对比

手动 lock/unlock 是最古老的 mutex bug 来源。看这个例子：

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

如果 `push_back` 抛异常，`unlock()` 就会被跳过。此后所有尝试获取 `mtx` 的线程都会永久阻塞。这不是假想场景；内存压力下的分配失败，或一个会抛异常的拷贝构造函数，都会触发它。

修复方式很机械：使用 RAII guard。

```cpp
void push(int value) {
	std::scoped_lock lock(mtx);
	data.push_back(value); // if this throws, ~scoped_lock releases the mutex
}
```

`std::scoped_lock` 可以处理单个或多个 mutex，并带死锁规避。`std::unique_lock` 则增加了延后加锁、转移所有权以及配合 condition variable 使用的能力。除非你确实需要这些额外灵活性，否则优先使用 `scoped_lock`。

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

当代码需要获取多个 mutex 时，不一致顺序是经典死锁来源。

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

线程 1 锁住 `a.mtx` 并等待 `b.mtx`。线程 2 锁住 `b.mtx` 并等待 `a.mtx`。两者都无法前进。`std::scoped_lock` 通过内部使用 `std::lock` 同时获取两个 mutex 并规避死锁，解决了这个问题：

```cpp
void transfer(account& from, account& to, int amount) {
	std::scoped_lock lock(from.mtx, to.mtx); // deadlock-free acquisition
	from.balance -= amount;
	to.balance += amount;
}
```

这不只是便利性。这是正确性边界。任何需要多个 mutex 的设计，要么应该使用 `std::scoped_lock` 同时获取它们，要么就强制执行一个有文档说明的全序锁顺序。临时约定的顺序纪律很少能撑过重构。

## 过度同步的性能成本

争用不只是正确性问题。过度加锁会把本可并行的工作串行化。

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

在一台每秒处理数百万请求的 64 核机器上，每个线程都会在同一条 cache line 上串行化。锁获取、cache line 来回弹跳和调度器唤醒会占据主导。更好的设计取决于容忍度：

- 如果字段之间不需要精确一致性，就使用按线程计数并周期性合并。
- 如果只需要近似总数，就对每个计数器独立使用带 `memory_order_relaxed` 的 `std::atomic<uint64_t>`。
- 如果需要跨字段一致性（例如错误率 = errors / total），那就保留 mutex，但按线程或请求键分片。

重点不是 mutex 慢。重点是，一把被所有核心共享的 mutex，会把并行工作负载变成串行瓶颈。分别测量锁持有时间和等待时间；等待时间高而持有时间低，是过度同步的标志。

## 围绕不变量设计，而不是围绕字段设计

锁保护的不是变量。它们保护的是不变量。

这个区别很重要，因为生产对象很少在字段层面失败。它们失败时，往往是因为多个字段必须一起变更，而某个线程能在这些变更之间观察到状态。

一个连接池之所以正确，不是因为 `available_count` 是原子的。它之所以正确，是因为在并发访问下，下列关系始终成立：已借出的连接不会同时出现在空闲列表里；已关闭的连接不会被重新发出；当进展变得可能时，等待者会被唤醒。这些都是不变量表述。如果设计没有显式命名它们，同步边界其实就已经规定不足了。

这也是为什么粗粒度加锁有时反而会赢。如果一把 mutex 能干净地覆盖一个不变量域，那么它可能比几把更细的锁更好；后者会允许不可能的中间状态，或要求脆弱的锁顺序。细粒度加锁默认并不高级。它往往只是更难评审。

## 反模式：不断膨胀的服务对象外面包一把锁

最常见的失败形状不是“没有同步”。而是“一把本来看起来合理的锁，逐渐变成了服务边界”。

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

这个对象在早期测试中可能活得不错，因为它在局部看起来很简单。它后来失败，是因为无关工作现在共享了同一个排队点。一次读会被清理阻塞。清理会在持有 mutex 时执行 I/O。未来特性还会在同一个临界区里加入 metrics、回调和日志，因为锁已经在那里了。

问题不只是临界区持续时间。问题还在于，这个对象没有显式的不变量边界。生命周期管理、查找、过期和副作用被压缩进了同一个同步域。

更好的方向通常是把状态转换与外部动作分开：在锁下识别哪些会话应当过期，把它们移出，释放锁，然后再关闭 socket。这样既缩短了锁作用域，也让不变量更容易陈述：受保护区域更新 registry；在所有权被转移出共享结构之后，再执行外部清理。

## 最小化锁作用域，但不要盲目拆分逻辑

“让锁作用域尽量小”是对的，但并不完整。

一个临界区应当只包含维护不变量所需的工作，不多也不少。这意味着：

1. 不要在锁下做阻塞 I/O。
2. 不要在锁下回调外部代码。
3. 如果能移出去，就不要在锁下执行大量分配或大量日志的慢路径。
4. 不要只是为了让作用域看起来更短，就拆开那些逻辑上必须原子的状态更新。

最后一点正是团队容易出问题的地方。一把保护多步不变量的锁，可能确实需要跨越多个操作。如果你为了看起来“更快”而在步骤之间释放它，就可能制造不可能状态。先明确什么必须保持原子，再做优化。

## Atomics 适合狭窄事实，不适合复杂所有权

Atomics 很关键，也很容易被误用。

当共享事实确实很狭窄时，使用 atomics：停止标志、代计数器、环形 buffer 的索引、已健全所有权模型中的引用计数，或者只需要 relaxed 顺序的统计计数器。不要把 atomics 当作结构化所有权的替代品，也不要用它来替代多字段不变量。

一个原子计数器并不能让队列变得安全。一个原子指针也不能让对象生命周期变得简单。几个 `memory_order` 实参，修不好那种允许一个线程观察到部分发布状态的设计。

C++23 在这里提供了有用工具，包括 `std::atomic::wait`、`notify_one` 和 `notify_all`。它们能为狭窄状态转换消除一部分 condition variable 样板代码。但它们并不会改变这样一个事实：你仍然必须先把状态机设计清楚。

如果评审者无法解释哪些值转换是合法的，以及为什么所选顺序足够，那么这段原子代码就还没完成。

## 读多写少的数据需要不同形状

争用的来源，往往与其说是写入频率，不如说是读取设计。

配置表、路由映射或特性策略快照，可能在每个请求上都被读取，而更新很少。用中心 mutex 保护它们，功能上可行，却会制造本可避免的尾延迟。在这些情况下，不可变快照或 copy-on-write 式发布，往往比更细的加锁更能得到好的系统。

权衡是显式的：

1. 读者获得稳定、低争用的访问。
2. 写者支付拷贝和发布时间成本。
3. 由于世代重叠，内存压力可能增加。
4. 业务域必须能接受陈旧性。

这通常是请求路由、认证策略和读多写少元数据中的正确权衡。对于高写入的订单簿或频繁变更的共享索引，则是错误权衡。

## Condition variable 与唤醒纪律

Condition variable 是许多原本谨慎的设计开始变得含糊的地方。

规则很简单：等待谓词是不变量的一部分，不是图方便写的表达式。等待线程必须重新检查一个谓词，而这个谓词必须受同一个同步域保护，正是这个同步域让该谓词有意义。通知只是要求重新检查的信号，不是进展一定会发生的证明。

落到实践里：

1. 精确命名谓词：队列非空、请求关闭、容量可用、世代已变化。
2. 先更新谓词状态，再发送通知。
3. 让等待代码对伪唤醒和关闭竞争保持健壮。
4. 决定唤醒一个等待者还是全部等待者，是否符合进展模型。

大多数损坏的 condition variable 代码，并不是因为作者忘了写循环。它们之所以损坏，是因为谓词规定不足，或者被拆散到不同代码路径不一致更新的状态里。

## 隐藏的共享状态仍然是共享状态

并发 bug 经常藏在那些从调用点看并不像共享的对象里。

例子包括：

1. 被许多线程使用的分配器或 memory resource。
2. 带内部 buffer 的日志 sink。
3. 带共享控制块的引用计数 handle。
4. 藏在看似纯函数 helper 背后的 cache。
5. 用于插件发现、metrics 或 tracing 的全局 registry。

这些对象应当受到与显式共享 map 和 queue 一样的审查。“这个 helper 是线程安全的”并不够。还要问：它是否把所有调用方都串行化了？它是否会在争用下分配？它是否可能在持有内部锁时调用用户代码？它是否在热路径上引入了 API 没有显式体现的争用成本？

## 测量争用会改变设计

正确性只是第一道门。过了这道门以后，共享状态设计就是一个测量问题。

争用很少以明显的源码味道出现。它表现为排队时间、锁持有时间分布、convoy 行为、cache line 弹跳，以及调度器可见的停顿。这意味着验证必须包含运维证据：

1. 在热路径上测量锁持有时间和等待时间。
2. 跟踪尾延迟，而不只是吞吐量平均值。
3. 在使用分片时观察热点键偏斜。
4. 分析临界区内的分配。
5. 使用 ThreadSanitizer 做竞争检测，并用有针对性的压力测试覆盖死锁和饥饿模式。

一个逻辑上正确、却在第 99 百分位崩塌的设计，依然是糟糕的并发设计。

## 共享状态的评审问题

在批准并发共享状态代码之前，先问：

1. 每把锁或每个 atomic 精确保护了哪个不变量？
2. 这份状态能否改为限制在线程内、分片化，或快照化，而不是共享？
3. 是否有任何临界区执行了 I/O、大量分配工作、日志或回调？
4. 相对于观察者，跨字段更新是否真的是原子的？
5. Condition variable 的谓词是否精确，并且在正确的同步域下更新？
6. 在突发负载或热点键偏斜下，争用会出现在哪里？
7. 除了“测试过了”之外，我们还有什么证据？

如果这些问题没有清晰答案，这个设计就还没准备好承受生产负载。

## 要点

共享可变状态不是并发设计的默认形状。它是昂贵的形状。

当共享不可避免时，先定义不变量，再选择原语。优先使用限制在线程内、分片和快照，而不是越来越花哨的加锁。用 mutex 保护不变量域，用 atomics 表达狭窄事实，只有在谓词清楚陈述时才使用 condition variable。然后在真实负载下测量结果，因为即使同步是正确的，如果争用主导了行为，系统仍然会是错的。
