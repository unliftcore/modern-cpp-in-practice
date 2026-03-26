# 结构化并发、取消与背压

本章假定你已经理解局部协程生命周期和挂起风险。现在要关注的是系统的整体形态：在真实负载下，一组任务如何启动、如何结束、如何失败，又如何彼此施加压力。

## 生产问题

很多异步系统会失败——即便其中每个任务单独看都没什么问题。

一个请求扇出到四个后端，三个返回后就给出响应，但第四个在客户端断开后仍在运行。一条 worker pipeline 的输入速度快于下游存储的提交速度，内存一路攀升，直到进程被杀。关闭路径永远等不到结束，因为后台任务被 detach 了，而不是挂在受监督的任务树下。重试风暴吞掉了系统恢复本身所需的容量。这些归根结底都不是局部协程 bug，而是编排 bug。

结构化并发是一种工程纪律：让并发工作遵循词法作用域和所有权结构。任务归属于父作用域，生命周期有明确边界，失败会传播到确定的位置。取消不是口头约定，背压是准入策略的组成部分，而非上线后仪表盘上冒出来的意外。

本章讨论的就是这些系统级规则。第 12 章讲的是共享可变状态，第 13 章讲的是单个协程跨挂起点的所有权。而本章的分析单元是一组任务——它们共同构成一条请求路径、一个流处理器，或一个有界服务阶段。

## 非结构化工作放大的不只是吞吐量，还有故障

启动并发工作最简单的做法，就是哪里需要就在哪里发起任务，然后指望最终都能自行收场。这种风格之所以诱人，是因为它把眼前的协调成本降到了最低。但它同样也是系统暗中积累不可见工作的方式。

Detached task、临时线程池和 fire-and-forget 重试，会带来三个可以预见的后果：

1. **生命周期变得非局部。** 启动工作的代码不再有责任证明这项工作何时结束。
2. **失败变成被动观察。** 错误只有在有人记得去记录或轮询时才会浮现。
3. **容量变成空中楼阁。** 系统不断接受新工作，因为没有任何父作用域掌控准入压力。

如果流量不大、很少重启，服务靠这种做法也许能撑上几个月。一旦遇到突发负载、频繁部署或缓慢的下游依赖，那些隐藏的工作就会喧宾夺主，成为系统的主要行为。

## Fire-and-forget：一份失败清单

在与结构化并发做对比之前，有必要先看清非结构化工作到底是怎么出问题的。”Fire-and-forget”不是一种反模式，而是好几种，每种都有各自的失败方式。

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

进程一旦开始关闭，detached thread 不会收到 stop request，数据库连接也不会归还连接池。把这种情况乘以滚动部署期间成千上万的在途请求：数据库会遭遇连接耗尽，旧进程卡在 `std::thread` 的析构函数里；更糟的情况是，进程在这些线程仍引用已销毁的全局对象时就退出了。

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

`std::async` 的析构函数会阻塞等待（这本身就可能出乎意料）。但对于大多数支持 detach 的自定义任务类型，销毁 handle 而不观察结果，异常就会无声无息地消失。系统继续带着过期配置运行，直到数小时后出现一次莫名其妙的行为退化，失败才会被注意到。

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

线程池里确实有任务，但服务对这些任务需要什么资源、该如何取消，完全没有建模。关闭要么挂起（卡在一个阻塞的外部调用上），要么产生竞态（任务还在使用依赖，依赖就被先关掉了）。到了生产环境，一次本该干净的重启变成了进程 kill，进程 kill 又演变成数据丢失。

### 结构化替代方案概述

上述三个问题，结构化的回答都指向同一个原则：谁创建了工作，谁就负责它的完成。

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

父请求作用域可以在客户端断开或 deadline 到期时取消 token。协程的 awaitable 在每个挂起点检查 token，资源通过普通 RAII 释放，没有任何工作能比拥有者活得更久。与 fire-and-forget 的差别不在于风格，而在于系统能否干净地关闭。

## 结构化并发意味着父作用域拥有子工作

核心思想很简单：如果某个作用域启动了子任务来完成自己的工作，那么在该作用域被视为完成之前，这些子任务就应当完成、失败或被取消。

由此你获得三个属性，而这恰恰是临时拼凑的异步代码默认不具备的：

1. 工作的生命周期受父操作约束。
2. 失败可以在同一个地方被聚合或升级。
3. 取消和关闭沿着任务树传播，而不必在进程里到处搜索散落的尾巴。

这不要求使用某个特定库，要求的是设计纪律。一个向多个后端扇出的请求处理器，不应该在后端调用仍在运行时就返回——除非业务契约明确允许 detached 的后续工作，并且指定了它的所有者。一个 batch consumer 不应该把下游任务入队，却不同时决定关闭时由谁来排空它们、过载时由谁来吸收压力。

所以说，结构化并发就是把所有权规则应用到时间维度。如果第 1 章教的是每个资源都需要所有者，本章教的就是把同样的原则应用到并发工作上。

## 取消必须是一等契约

取消常被当成一种客气的建议。但在生产环境里，它就是负载控制。

客户端一旦断开、deadline 一旦到期、父任务一旦失败，继续执行子工作就是在浪费 CPU、内存、数据库容量和重试预算。更严重的是，未取消的工作会与有效工作争抢资源。系统在高压下频频崩溃，往往就是因为它还在忙着做那些已经没有意义的事。

现代 C++ 提供了 `std::stop_source`、`std::stop_token` 和 `std::jthread` 等有用的构件。示例项目的 `Server::run(std::stop_token)`（`examples/web-api/src/modules/http.cppm`）正是使用了这些构件：accept 循环在每次迭代时检查 `stop_token.stop_requested()`，并通过带一秒超时的 `select()` 确保即使没有客户端连接，检查也能及时进行。这就是真实服务器中的协作式取消——token 是契约，基于超时的轮询是机制。

但仅有原语是不够的。更难回答的是语义层面的问题：

1. 哪些操作是可取消的？
2. 在哪些边界上观察取消？
3. 在报告完成之前，保证了哪些清理？
4. 部分进度会被提交、回滚，还是通过补偿机制显式可见？

如果这些问题没有答案，把 stop token 透传几个函数也不过是做做样子。

取消还有方向性。从父到子的传播应当是默认行为；从子到父的升级则取决于策略：一个子任务失败，可能需要取消兄弟任务，可能需要降级结果，也可能只是记录下来、让其余工作继续。关键是：这条规则必须在拥有该任务组的作用域上写明。

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

这段代码很整洁，但定义严重不足。

客户端在第一次 await 之后超时了，谁来取消这三个子操作？有什么机制阻止一万个并发请求瞬间启动三万个后端调用？`fetch_inventory` 如果卡住，另外两个还继续跑吗？某个调用快速失败后，其余的应该被取消，还是因为部分结果有用而继续完成？

问题不在于扇出本身不好，而在于代码里看不到任何监督策略。

在结构化设计中，请求作用域持有一个取消 source 或 token，子任务在该作用域内启动并附带 deadline，对下游依赖的并发度则通过 permit 或 semaphore 做有界控制。具体用什么抽象因代码库而异，但核心性质不变：请求不得创建匿名工作。

## Deadline 与预算优于尽力而为的 timeout

Timeout 往往各处各设、毫无一致性。某个依赖用 200 ms，另一个用 500 ms，而调用方自身的 deadline 是 300 ms，却没人把它传下去。结果就是白做工加上混乱的遥测数据。

更好的做法是预算传播。父操作携带一个 deadline 或剩余预算，子操作从中派生自己的限制，而不是各自发明一套毫无关联的超时值。这样取消意图和延迟预期才能保持一致。

代价是下游 API 必须显式接收 deadline 或取消上下文，timeout 行为也会在函数签名或任务构造器中暴露出来。这个代价值得付。隐藏的 timeout 策略通常比显式的 timeout 策略更危险。

## 背压是准入控制，不是抱怨日志

背压意味着系统对”工作到来的速度快于处理速度时该怎么办”这个问题，有一个经过深思熟虑的答案。

没有这个答案，工作就会堆积在队列、buffer、重试循环和协程帧里。先涨的是内存，然后是延迟，最后故障才变得不可忽视。无界队列不是弹性，它只不过是把过载变成了延时爆炸。

真正的背压机制都是具体的：

1. 有界队列——满了就拒绝或延后新工作。
2. Semaphore 或 permit——限制对稀缺依赖的并发访问数。
3. 生产者节流——下游阶段饱和时自动减速。
4. 负载丢弃——当服务全部流量会拖垮所有人的延迟时，主动放弃一部分。
5. Batch 大小和 flush 策略——与下游提交成本匹配。

每种机制背后都是业务策略：哪些工作可以丢？哪些必须等？哪些客户端会收到显式的过载信号？这些都是用并发控制手段表达的产品决策。

## 有界并发通常优于更大的线程池

依赖变慢时，很多团队的第一反应是加大线程池或加深队列。这往往适得其反。

一个数据库如果只能承受五十个有效并发请求，放进去两百个在途操作，多出来的部分基本只会加剧争用和 timeout 重叠。CPU 密集的解析阶段、压缩任务、以及本身就有内部瓶颈的远程服务调用，道理都一样。

应该在稀缺资源实际所在的位置约束并发，让这个上限在代码和遥测中都清晰可见，然后再决定触及上限后该怎么办：等待、快速失败、降级还是重定向。盲目扩大线程池只会把过载隐藏起来，直到整个系统一起饱和。

## Pipeline 需要让压力向上游传播

Pipeline 是背压纪律最无法回避的场景。

假设有一个消息消费者：解析记录、用远程查找做 enrich、最后把 batch 写入存储。如果解析快过存储，总得有个阶段降速。如果 enrich 快过解析，它也不该仅仅因为"能做"就继续制造更多在途请求。如果系统开始关闭，所有阶段都需要协调好的 drain 或 cancel 策略。

因此，好的 pipeline 设计会明确定义：

1. 每个阶段允许的最大在途工作量。
2. 阶段之间的最大队列深度。
3. 队列满时，生产者会被阻塞、丢弃输入，还是触发负载削减。
4. 取消时，是排空部分完成的 batch，还是丢弃它们。
5. 哪些 metrics 能在内存压力变得致命之前揭示饱和。

这不是锦上添花的基础设施优化。它决定了系统是能优雅降级，还是一路积压工作直到崩溃。

## 失败传播需要策略，而不是希望

工作一旦被组织成任务组，失败处理就从碰运气变成了设计选择。

常见策略包括：

1. Fail-fast 任务组：一个子任务失败就取消同级任务，因为没有所有部分结果就毫无意义。
2. Best-effort 任务组：允许某些子任务失败，并把它们记录下来。
3. Quorum 任务组：只要足够多的子任务成功就满足操作，其余任务会被取消。
4. Supervisory loop：在速率限制和预算约束下，重启隔离的子工作。

这四种在各自适用的场景中都是正确的。关键在于代码和抽象必须让所选策略一目了然。子任务失败后悄悄继续运行，那不叫韧性，那叫含糊不清。

## 关闭是照妖镜

并发结构薄弱的系统平时看起来往往一切正常，直到关闭时才原形毕露。

干净的关闭路径会把所有隐藏问题逼到台面上：还有哪些任务在跑？哪些可以安全中断？哪些队列必须排空？关闭开始后哪些副作用仍可能被提交？哪些后台循环持有 stop source，又由谁来等待它们完成？

正因如此，关闭测试的价值远超一般测试。它们能暴露 detached 工作、缺失的取消点、无界队列和无主任务。一个子系统如果说不清自己在负载下如何停止，就说明它还没有真正掌控自己的并发模型。

示例项目实现了一条完整的结构化关闭链，将上述原则付诸实践。这条链横跨三个文件：

**信号处理器设置标志位**（`examples/web-api/src/main.cpp`）：

```cpp
std::atomic<bool> shutdown_requested{false};

extern "C" void signal_handler(int /*sig*/) {
    shutdown_requested.store(true, std::memory_order_release);
}
```

**`Server::run_until` 将标志位桥接到 `jthread` 的 stop token**（`examples/web-api/src/modules/http.cppm`）：

```cpp
void run_until(const std::atomic<bool>& should_stop) {
    std::jthread server_thread{[this](std::stop_token st) {
        run(st);
    }};
    while (!should_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server_thread.request_stop();
    // jthread 在析构时自动 join
}
```

**`Server::run` 在每次 accept 循环迭代时检查 token**：

```cpp
void run(std::stop_token stop_token) {
    // ...
    while (!stop_token.stop_requested()) {
        // 带一秒超时的 select()，然后再次检查 stop
        // accept 并处理连接……
    }
    std::cout << "Server shutting down gracefully\n";
}
```

所有权链清晰可见：`main` 拥有 `Server`，`Server` 拥有 `jthread`，`jthread` 拥有 `stop_source`。当 Ctrl+C 到来时，信号处理器设置 atomic 标志，`run_until` 观察到它并调用 `request_stop()`，accept 循环在下一次迭代退出，`jthread` 析构函数 join 线程。没有 detached 工作，没有孤立连接，关闭和在途请求之间没有竞态。这就是结构化并发应用于真实服务生命周期的样子。

## 结构化异步系统的验证与遥测

光靠单元测试无法验证结构化并发和背压。你需要实际证据表明系统在压力下行为依然正常。

有效的验证手段包括：

1. 负载测试：把系统推到标称容量以上，确认内存仍然有界。
2. 取消测试：注入断连、deadline 到期和部分子任务失败。
3. 关闭测试：启动工作、触发停止，验证系统能迅速归于静止。
4. 监控指标：在途任务数、队列深度、permit 利用率、拒绝率、deadline 到期次数、取消延迟。
5. 链路追踪或日志：展示父子关联关系，让孤儿工作无所遁形。

如果可观测性无法揭示工作堆积在哪里、由哪个父作用域负责，那么所谓的结构就只存在于作者脑中。

## 结构化并发的评审问题

在批准一个异步编排方案之前，问问自己：

1. 每组子任务归谁管？
2. 什么事件触发取消：父任务完成、失败、deadline、关闭，还是过载？
3. 用什么机制限制对每个稀缺依赖的并发访问？
4. 当队列或 permit 达到上限时会发生什么？
5. 失败会取消同级任务、优雅降级，还是等待 quorum？
6. 在峰值负载下，关闭能否迅速完成？
7. 哪些 metrics 能证明背压真的在工作？

如果答不上来，系统多半就是在靠乐观和余量撑着。

## 要点

结构化并发，就是把所有权延伸到时间和任务树上。

不要发起匿名工作。让父作用域掌管子任务的生命周期，有意识地传播取消，在资源真正稀缺的地方限制并发。把背压当作准入策略来设计，而非事后调参的补救措施。一个说不清工作何时停止、由谁取消、过载如何限制的系统，算不上拥有并发模型——它只是拥有异步代码。
