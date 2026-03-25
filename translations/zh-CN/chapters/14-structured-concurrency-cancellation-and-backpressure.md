# 结构化并发、取消与背压

本章假定你已经理解局部协程生命周期和挂起风险。现在的焦点是系统形状：在真实负载下，任务组如何开始、结束、失败，并彼此施加压力。

## 生产问题

许多异步系统即使每个单独任务看起来都还合理，也仍然会失败。

一个请求会 fan-out 到四个后端，并在其中三个返回后就响应，但第四个在客户端断开后仍继续运行。一个 worker pipeline 接收输入的速度快于下游存储提交的速度，于是内存使用持续攀升，直到进程被杀掉。一个关闭路径会永远等待，因为后台任务被 detach 了，而不是成为受监督树的一部分。一次重试风暴吞掉了系统恢复所需的容量。这些主要都不是局部协程 bug。它们是编排 bug。

结构化并发是一种纪律：让并发工作遵循词法结构和所有权结构。任务属于父作用域。生命周期有边界。失败会传播到某个明确位置。取消不是口耳相传的建议。背压是准入策略的一部分，而不是上线之后仪表盘上才看到的意外。

本章讨论的就是这些系统级规则。第 12 章讨论了共享可变状态。第 13 章讨论了单个协程在挂起期间拥有的内容。这里推理的单位，是一组共同实现请求路径、流处理器或有界服务阶段的任务。

## 非结构化工作扩大的不只是吞吐量，还有失败

启动并发工作的最简单方式，就是在需要时到处发起任务，然后希望完成能自己收拾残局。这种风格之所以诱人，是因为它把眼前的协调成本降到最低。它同样也是系统积累不可见工作的方式。

Detached task、临时线程池，以及 fire-and-forget 重试，会带来三个可预测后果：

1. 生命周期变成非局部。启动工作的代码不再负责证明它何时结束。
2. 失败变成观察性事件。只有在有人记得记录或轮询时，错误才会浮现。
3. 容量变成虚构。系统会继续接收工作，因为没有父作用域拥有准入压力。

如果流量轻、关闭少，一个服务靠这种做法也许能撑上几个月。一旦遇到突发负载、部署 churn 或缓慢的下游依赖，隐藏工作就会变成系统本身。

## Fire-and-forget：失败目录

在和结构化并发做对比之前，值得先准确看看非结构化工作是如何失败的。“Fire-and-forget”不是一种反模式，而是好几种，每一种都有不同的失败模式。

### 无主工作的资源泄漏

```cpp
// Anti-pattern: detached task leaks a database connection on cancellation.
void on_request(request req) {
	std::jthread([req = std::move(req)] {
		auto conn = db_pool.acquire();        // acquired, never returned on some paths
		auto result = conn.execute(req.query);
		send_response(req.client, result);
	}).detach(); // no owner, no cancellation, no cleanup guarantee
}
```

如果进程开始关闭，detached thread 不会收到 stop request。数据库连接不会归还给池子。把这种情况乘上滚动部署期间成千上万的在途请求：数据库会看到连接耗尽，而旧进程会卡在 `std::thread` 析构调用里；更糟的是，进程可能在这些线程仍引用已销毁全局对象时就退出。

### 未被观察的异常会静默消失

```cpp
// Anti-pattern: exception in detached task is never observed.
void start_background_sync() {
	auto handle = std::async(std::launch::async, [] {
		auto data = fetch_remote_config(); // throws on network error
		apply_config(data);
	});
	// handle is destroyed here — std::async's destructor blocks,
	// but if this were a custom fire-and-forget task, the exception
	// would be silently swallowed.
}
```

对 `std::async` 来说，析构会阻塞（这本身就可能令人意外）。但对大多数支持 detach 的自定义任务类型来说，如果销毁 handle 却不观察结果，异常就会蒸发。系统会带着陈旧配置继续运行，而失败只会在数小时后以一种神秘的行为退化出现。

### 孤儿工作导致关闭挂起

```cpp
// Anti-pattern: shutdown cannot complete because background tasks were never tracked.
class ingestion_service {
	void ingest(message msg) {
		// "just kick off enrichment in the background"
		pool_.submit([msg = std::move(msg), this] {
			auto enriched = enrich(msg);       // calls external service, may block
			store_.write(enriched);
		});
	}

	void shutdown() {
		store_.close();    // closes storage
		pool_.shutdown();  // waits for in-flight tasks
		// BUG: in-flight tasks may call store_.write() after store_ is closed
		// BUG: enrich() may block indefinitely — pool shutdown hangs
	}
};
```

线程池里确实有任务，但服务并没有一个模型来说明这些任务需要什么、以及该如何取消它们。关闭要么挂起（等待一个阻塞的外部调用），要么发生竞争（在任务仍使用依赖时先关闭依赖）。在生产环境里，这会把一次干净重启变成进程 kill，而进程 kill 又会变成数据丢失。

### 结构化替代方案概述

对这三个问题，结构化的答案其实是同一个原则：创建工作的作用域拥有它的完成。

```cpp
// Structured: parent scope owns child tasks, propagates cancellation, awaits completion.
task<void> on_request(request req, std::stop_token stop) {
	auto conn = co_await db_pool.acquire(stop);  // respects cancellation
	auto result = co_await conn.execute(req.query, stop);
	co_await send_response(req.client, result);
	// conn returned to pool when coroutine frame is destroyed
	// if stop is triggered, co_await points observe it and unwind cleanly
}
```

父请求作用域可以在客户端断开或 deadline 到达时取消 token。协程的 awaitable 会在每个挂起点检查 token。资源通过普通 RAII 释放。没有工作会活得比拥有者更久。和 fire-and-forget 的对比，不是风格差异；而是能否干净关闭一个系统的差异。

## 结构化并发意味着父作用域拥有子工作

核心思想很简单：如果某个作用域启动了子任务来完成自己的工作，那么在该作用域被视为完成之前，这些子任务就应当完成、失败或被取消。

这带来三个属性，而这些属性是临时拼凑的异步代码默认很少具备的：

1. 工作的生命周期被父操作约束。
2. 失败可以在一个地方被聚合或升级。
3. 取消和关闭可以沿着树传播，而不是在进程里搜索散落的尾巴。

这不要求你必须使用某个特定库。它要求的是设计纪律。一个会 fan-out 到多个后端的请求处理器，不应该在那些后端调用仍继续运行时就返回；除非业务契约明确允许 detached 的后续工作，并且点名了它的所有者。一个 batch consumer 不应该把下游任务入队，却不同时决定谁会在关闭时排空它们，以及谁来吸收过载。

因此，结构化并发是对时间应用所有权规则。如果第 1 章讲的是每个资源都需要所有者，那么本章讲的就是把同样的原则应用到并发工作上。

## 取消必须是一等契约

取消常被描述成一种礼貌。在生产环境里，它是负载控制。

一旦客户端断开、deadline 到期，或者父任务失败，继续执行子工作就可能浪费 CPU、内存、数据库容量和重试预算。更糟的是，未取消的工作会与有用工作竞争。系统在受压时之所以经常失败，正是因为它们还在做那些已经不再重要的任务。

现代 C++ 提供了有用的构件，如 `std::stop_source`、`std::stop_token` 和 `std::jthread`。它们很重要，但更难的问题是语义：

1. 哪些操作是可取消的？
2. 在哪些边界上观察取消？
3. 在报告完成之前，保证了哪些清理？
4. 部分进度会被提交、回滚，还是通过补偿机制显式可见？

如果这些问题没有答案，那么仅仅把 stop token 穿过几个函数，只是在演戏。

取消还需要方向。父到子的传播应当是默认行为。子到父的升级则取决于策略：一个子任务失败，可能要取消同级任务，可能要降级结果，也可能只是被记录下来而让工作继续。重点是，这条规则必须在拥有该任务组的作用域上显式说明。

## 反模式：没有有界所有权的 fan-out

```cpp
// Anti-pattern: child work outlives the request and overload has no admission limit.
task<aggregate_reply> handle_request(request req) {
	auto a = fetch_profile(req.user_id);
	auto b = fetch_inventory(req.item_id);
	auto c = fetch_pricing(req.item_id);

	co_return aggregate_reply{
		co_await a,
		co_await b,
		co_await c,
	};
}
```

这段代码很整洁，也规定不足。

如果客户端在第一次 await 之后超时，谁来取消这三个子操作？什么机制能阻止一万个并发请求立刻启动三万个后端调用？如果 `fetch_inventory` 挂住，其他两个是否继续运行？如果某个调用快速失败，其余调用应当被取消，还是因为部分结果有用而允许它们继续完成？

问题不在于 fan-out 不好。问题在于，代码没有展示监督策略。

在结构化设计里，请求作用域拥有一个取消 source 或 token，子任务在该作用域内启动，附带 deadline，并且对下游依赖的并发度通过 permit 或 semaphore 做有界控制。具体抽象因代码库而异。关键性质是不让请求创建匿名工作。

## Deadline 与预算优于尽力而为的 timeout

Timeout 经常被以局部且不一致的方式实现。某个依赖用 200 ms timeout，另一个用 500 ms，而调用方自身的 deadline 是 300 ms，却没有人传播它。结果就是浪费工作和混乱遥测。

更好的模型是预算传播。父操作携带一个 deadline 或剩余预算。子操作从这个预算派生自己的限制，而不是自行发明无关的限制。这样，取消和延迟意图才能保持一致。

代价是，下游 API 必须显式接收 deadline 或取消上下文，timeout 行为也会在签名或任务构造器中可见。这是一个值得付出的成本。隐藏的 timeout 策略通常比吵闹的 timeout 策略更糟。

## 背压是准入控制，不是抱怨日志

背压意味着，系统对“当工作到达速度快于完成速度时会发生什么”这个问题，有一个刻意设计的答案。

如果没有这个答案，工作就会堆积在队列、buffer、重试循环和协程帧里。最先上涨的是内存，其次是延迟，只有到那之后故障才会变得明显。无界队列不是弹性。它只是承诺把过载转换成延迟失败。

真实的背压机制都很具体：

1. 有界队列，用来拒绝或延后新工作。
2. semaphore 或 permit，用来限制对稀缺依赖的并发访问。
3. 当下游阶段饱和时，对生产者节流。
4. 当为所有流量提供服务会摧毁整体延迟时，主动丢弃部分负载。
5. 与下游提交成本匹配的 batch 大小和 flush 策略。

每种机制都在编码业务策略。哪些工作可以丢？哪些必须等？哪些客户端会收到显式过载信号？这些都是通过并发控制表达出来的产品决策。

## 有界并发通常优于更大的线程池

当依赖变慢时，许多团队首先会增加池大小或队列深度。这通常会放大问题。

如果一个数据库最多只能承受五十个有用并发请求，那么允许两百个在途操作，大多只会增加争用和 timeout 重叠。对 CPU 密集的解析阶段、压缩工作，以及有自身内部瓶颈的远程服务调用，也同样如此。

在稀缺资源真正存在的地方约束并发。让这个边界在代码和遥测里都可见。然后再决定达到边界时应当怎么做：等待、快速失败、降级，还是重定向。没有策略地扩大线程池，只会把过载藏起来，直到整个系统饱和。

## Pipeline 需要让压力向上游传播

Pipeline 正是背压纪律无法回避的地方。

考虑一个消息消费者：它解析记录，用远程查找做 enrich，然后把 batch 写入存储。如果解析速度超过存储速度，总得有一个阶段慢下来。如果 enrich 比解析更快，它也不应该仅仅因为做得到，就继续制造更多在途请求。如果系统开始关闭，所有阶段都需要一个协调好的 drain 或 cancel 策略。

因此，一个好的 pipeline 设计会明确命名：

1. 每个阶段允许的最大在途工作量。
2. 阶段之间的最大队列深度。
3. 队列满时，生产者会被阻塞、丢弃输入，还是触发负载削减。
4. 取消时，是排空部分完成的 batch，还是丢弃它们。
5. 哪些 metrics 能在内存压力变得关键之前揭示饱和。

这不是可选的基础设施打磨。它决定了系统是会逐步退化，还是会在工作积压到死亡之前一直累积。

## 失败传播需要策略，而不是希望

一旦工作被结构化成任务组，失败处理就会从意外变成设计选择。

常见策略包括：

1. Fail-fast 任务组：一个子任务失败就取消同级任务，因为没有所有部分结果就毫无意义。
2. Best-effort 任务组：允许某些子任务失败，并把它们记录下来。
3. Quorum 任务组：只要足够多的子任务成功就满足操作，其余任务会被取消。
4. Supervisory loop：在速率限制和预算约束下，重启隔离的子工作。

这四种策略在合适的领域里都有效。重要的是，代码和抽象必须让策略显而易见。子任务失败后悄悄继续运行，不是韧性。那是歧义。

## 关闭是照妖镜

并发结构薄弱的系统，往往在平时看起来都没问题，直到关闭时才露馅。

一个干净的关闭路径会把所有隐藏问题都逼出来。还有哪些任务在运行？哪些能被安全中断？哪些队列必须排空？关闭开始后，哪些副作用仍可能被提交？哪些后台循环拥有 stop source，又由谁等待它们完成？

这也是为什么关闭测试价值特别高。它们能暴露 detached 工作、缺失的取消点、无界队列，以及没有所有者的任务。如果一个子系统说不清自己在负载下如何停止，那么它就还没有真正拥有自己的并发模型。

## 结构化异步系统的验证与遥测

单元测试不足以验证结构化并发或背压。你需要证据证明系统在压力下仍然行为正常。

有用的验证包括：

1. 把系统驱到标称容量之外，并确认内存仍然有界的负载测试。
2. 注入断连、deadline 到期和部分子任务失败的取消测试。
3. 启动工作、触发停止并验证能迅速静止的关闭测试。
4. 关于在途任务数、队列深度、permit 利用率、拒绝率、deadline 到期和取消延迟的 metrics。
5. 能展示父子关联关系的 trace 或日志，从而让孤儿工作可见。

如果可观测性无法揭示工作在哪里堆积，或者哪个父作用域拥有它，那么这种结构就只存在于作者脑中。

## 结构化并发的评审问题

在批准异步编排设计之前，先问：

1. 每组子任务由谁拥有？
2. 是什么事件会取消它们：父任务完成、失败、deadline、关闭，还是过载？
3. 什么机制会对每个稀缺依赖的并发工作做有界限制？
4. 当队列或 permit 达到上限时会发生什么？
5. 失败会取消同级任务、优雅降级，还是等待 quorum？
6. 在峰值负载下，关闭能否迅速完成？
7. 哪些 metrics 能证明背压真的在工作？

如果这些答案不清楚，系统大概率就是靠乐观和剩余容量在运行。

## 要点

结构化并发，就是把所有权应用到时间和任务树上。

不要启动匿名工作。让父作用域拥有子任务生命周期，有意传播取消，并在资源真正稀缺的地方对并发做有界控制。把背压当作准入策略，而不是调参之后才想到的补救。一个说不清工作何时停止、由谁取消、以及如何限制过载的系统，还不能算拥有并发模型。它只是在拥有异步代码。
