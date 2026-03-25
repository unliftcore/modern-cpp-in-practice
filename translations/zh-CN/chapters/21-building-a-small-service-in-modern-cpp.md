# 在现代 C++ 中构建一个小型服务

小型服务是许多 C++ 团队要么建立纪律、要么受其反噬的地方。代码库还足够小，人们很容易临场发挥；但流程已经有了真实的失败模式：过载、启动时半配置状态、部分写入、依赖超时、队列增长、关闭竞态，以及在证据不完整时排查生产故障。语言本身救不了你。

本章不是框架教程。这里的生产问题更窄：如果你希望六个月后所有权、失败处理、并发与运维仍然可评审，一个小型 C++23 服务应该长什么样？答案不是“把最新特性全用上”。答案是选择一种服务形态，让生命周期足够明显、异步工作有明确归属、资源限制显式可见，并且在压力下仍然能诊断问题。

示例系统是一个由配置驱动的小型服务：它接收请求、验证请求、执行有界的后台工作、持久化状态，并暴露指标与健康信息。这里的细节刻意保持平常。大多数生产服务在概念上并不新奇。它们失败，是因为基本工程边界被留得过于模糊。

## 先定义所有权单元，再定义部署单元

小型服务的第一个架构错误，是围绕端点、处理器或框架回调组织代码，而不是围绕被拥有的资源组织代码。一个可部署的服务拥有一组固定的长生命周期对象：配置、监听器、执行器、连接池、存储适配器、遥测汇聚点，以及关闭协调机制。如果这些对象没有被显式建模，代码就会逐渐滑向全局变量、共享单例、脱离控制的后台工作，以及“靠运气”的关闭流程。

因此，服务对象应当直接建模所有权。它应当是构造、启动和停止长生命周期依赖的地方。这不意味着要造一个巨大的上帝对象。它意味着要有一个清晰的根对象，拥有那些生命周期必须一起结束的部分。

### 有意保留为局部片段：拥有时间与关闭流程的服务根

```cpp
struct service_components {
config cfg;
request_router router;
storage_client storage;
bounded_executor executor;
telemetry telemetry;
http_listener listener;
};

class service {
public:
explicit service(service_components components)
: components_(std::move(components)) {}

auto run(std::stop_token stop) -> std::expected<void, service_error>;
void request_stop() noexcept;

private:
auto start() -> std::expected<void, service_error>;
auto drain() noexcept -> void;

service_components components_;
std::atomic<bool> stopping_{false};
};
```

这段代码刻意写得很“无聊”。服务有一个所有权根、一条停止路径，以及一个可以推理启动与排空顺序的地方。小型服务不需要架构表演。

这个根通常应当拥有具体的基础设施类型，而不是一张由堆分配接口组成、再用共享所有权缝起来的对象图。依赖反转仍然重要，但反转点通常在存储、传输或遥测适配器这类边界上。在进程内部，静态所有权比一片 `std::shared_ptr` 森林更简单、更便宜；而在那片森林里，真正的所有者早已无法在纸面上说清。

### 反模式：用 shared_ptr 杂糅请求状态

一种常见失败模式是，用 `std::shared_ptr` 把请求生命周期横跨回调、队列和重试强行延长，却没有显式的所有权模型。代码能编译，看起来也安全，但没人能说清请求资源究竟何时释放、取消是否能到达所有持有者，或者关闭流程是否能确定性完成。

```cpp
// BAD: shared_ptr soup — every callback extends lifetime indefinitely
void handle_request(std::shared_ptr<http_request> req) {
    auto ctx = std::make_shared<request_context>(req->parse_body());
    ctx->db_future = db_.async_query(ctx->query, [ctx](auto result) {
        ctx->result = result;
        cache_.async_store(ctx->key, ctx->result, [ctx](auto status) {
            ctx->respond(status);  // when does ctx die? who knows
        });
    });
    // ctx is now kept alive by two lambdas, the future, and possibly
    // a retry timer. cancellation cannot reach it. shutdown cannot
    // drain it. memory profile is non-deterministic.
}
```

修正方式是提取一个被拥有的工作项，并让它沿着流水线在明确的交接点上移动。

```cpp
// BETTER: owned work item with explicit lifetime boundaries
struct request_work {
    parsed_query query;
    std::stop_token stop;
    response_sink sink;  // move-only, writes exactly once
};

void handle_request(http_request& req, std::stop_token stop) {
    auto work = request_work{
        .query = req.parse_body(),
        .stop  = stop,
        .sink  = req.take_response_sink(),
    };
    executor_.submit(std::move(work));
    // work is now owned by the executor. cancellation reaches it
    // through stop_token. shutdown drains the executor.
}
```

被拥有的工作项让设计问题变得可见：哪些数据跨越了请求边界、谁可以取消它，以及关闭时它最终会落到哪里。

## 启动要么产出一个正在运行的服务，要么干净失败

许多服务故障在第一个请求到来前就已经开始。配置只加载了一半。一个子系统健康，另一个不健康。在线程启动之前还没有健康状态。后台定时器在依赖验证完成前就已经开始。进程之所以报告“ready”，只是因为某个构造函数返回了。

正确的启动问题不是每个组件能否分别初始化，而是进程能否进入一个一致的运行状态。因此，启动应当围绕依赖验证和显式的失败边界分阶段组织。

常见且有效的启动顺序通常如下：

1. 加载并验证不可变配置。
2. 构造带有显式限制的资源拥有型适配器。
3. 校验对就绪性至关重要的下游依赖。
4. 只有在进程整体一致之后，才启动监听器和后台工作。
5. 只有前面步骤全部成功后，才发布就绪状态。

在 C++23 中，这条路径通常更适合用 `std::expected`，而不是异常，因为启动天然会积累基础设施故障，这些故障需要被转换成稳定的运维类别。一个因为配置文件格式错误、端口不可用或存储模式不兼容而失败的服务，应当把这些暴露成有意设计的启动失败，而不是把实现细节里偶然泄漏出来的异常文本直接抛给最上层。

代价是冗长。`std::expected` 要求你把转换点显式写出来。在服务启动里，这个成本通常值得，因为隐藏的异常路径会让进程状态更难推理。在叶子函数或内部辅助函数里，如果包裹它们的边界足够清晰，异常仍然可能是可接受的。真正重要的是：启动在顶层暴露的是一个连贯的契约。

## 请求处理应把借用数据视为短命数据

小型服务常见的失败方式，是把临时请求数据变成长生命周期的内部状态。请求头变成异步任务里的 `std::string_view` 成员。解析后的负载视图被保存在缓存中。回调捕获了引用，而这些引用的生命周期早在请求结束时就已经结束。服务之所以“看起来可用”，只是因为慢路径、重试路径或队列延迟还没把错误暴露出来。

规则很简单：借用视图非常适合同步检查，不适合充当隐式存储。当生命周期局部且明显时，可以在请求路径内部积极使用 `std::string_view`、`std::span` 和范围视图。但在数据跨越时间、线程、队列或重试边界之前，必须把它转换成拥有型表示。

这也是为什么服务代码受益于显式请求模型。先把输入解析并验证成一个值类型，让它拥有后台工作必须保留的数据。让这个模型足够小，使复制成为一个有意识的成本；然后在设计需要时间解耦时，把它移动进异步工作中。

这也是许多 C++ 服务代码库滥用 `std::shared_ptr<request_context>` 的地方。共享所有权看起来像异步生命周期的方便逃生口，但它往往掩盖了真正的设计选择：请求的哪些部分需要存活、谁拥有它们、以及它们何时可以被丢弃。在小型服务里，通常更好的做法是提取一个被拥有的工作项并把它移动进队列，而不是把整个请求对象图的生命周期一并延长。

## 并发必须是有界的、被拥有的，而且可取消

服务的并发模型，比它具体用了哪些原语名字更重要。小型服务很少需要大型自定义调度器，但它确实需要三样东西。

第一，并发工作量必须有界。如果过载可以直接变成无界队列增长，那你设计的就不是服务，而只是把故障延后爆发。有界执行器、信号量、准入控制，以及按请求划分的时间预算，比花哨的线程池内部实现更有价值。

第二，工作必须有归属。分离线程和 fire-and-forget 任务之所以诱人，是因为它们让局部代码看起来更短。但它们也摧毁了关闭语义。如果服务可以入队工作，服务就应当知道工作何时开始、何时结束，以及取消是如何传递到那里的。

第三，取消必须是常规模型的一部分，而不是事后补丁。`std::jthread` 和 `std::stop_token` 在这里很有帮助，因为它们让停止传播成为类型级契约的一部分。它们并不能解决一切。你仍然需要工作单元在合理边界上检查 token，也仍然需要存储或网络操作把取消映射成一致的错误。但它们至少把这个问题逼进了代码，而不是留在注释里。

### 反模式：阻塞事件循环

最常见的服务故障之一，是在本应用于驱动 I/O 或分发请求的线程上执行同步阻塞工作。轻载时服务看起来健康；流量一上来，事件循环却卡在数据库调用、DNS 解析或文件读取里，整个服务随之塌陷。

```cpp
// BAD: synchronous blocking on the listener thread
void on_request(http_request& req) {
    auto record = db_.query_sync(req.key());   // blocks for 5-200ms
    auto enriched = enrich(record);             // CPU work, fine
    auto blob = fs::read_file(enriched.path()); // blocks again
    req.respond(200, serialize(blob));
}
// Under 50 concurrent requests, the listener thread is blocked
// for the entire duration of each request. Tail latency explodes.
// New connections queue at the OS level with no backpressure signal.
```

修正方式是把阻塞工作派发到有界执行器，让监听线程保持非阻塞。

```cpp
// BETTER: dispatch blocking work off the listener thread
void on_request(http_request& req) {
    auto work = request_work{req.key(), req.take_response_sink()};
    if (!executor_.try_submit(std::move(work))) {
        req.respond(503, "overloaded");  // explicit rejection
        metrics_.increment("request.rejected.overload");
    }
    // listener thread returns immediately, ready for next connection
}

// In the executor's worker threads:
void process(request_work work) {
    auto record = db_.query_sync(work.key);
    auto enriched = enrich(record);
    auto blob = fs::read_file(enriched.path());
    work.sink.respond(200, serialize(blob));
}
```

### 反模式：没有优雅关闭

缺少显式关闭逻辑的服务，会产生 use-after-free、部分写入、孤儿连接，以及只能由编排器用 `SIGKILL` 强杀的挂死进程。这个失败在开发环境里往往不可见，因为进程退出太快；但在生产环境里，飞行中的工作和后台定时器会引入真实竞态。

```cpp
// BAD: shutdown by destruction order and hope
class service {
    http_listener listener_;
    database_pool db_;
    std::vector<std::jthread> workers_;
public:
    ~service() {
        // listener_ destructor closes the socket (maybe)
        // workers_ destructors request stop and join (maybe)
        // db_ destructor closes connections (maybe)
        // but workers_ may still be using db_ when db_ destructs
        // destruction order is reverse-of-declaration, so db_
        // is destroyed BEFORE workers_ — use-after-free
    }
};
```

修正方式是把关闭做成一个显式、有序的操作：先排空工作，再销毁资源。

```cpp
// BETTER: explicit drain-then-destroy shutdown
class service {
    database_pool db_;           // destroyed last
    http_listener listener_;
    bounded_executor executor_;  // owns worker threads
    std::atomic<bool> stopping_{false};
public:
    void shutdown() noexcept {
        stopping_.store(true, std::memory_order_relaxed);
        listener_.stop_accepting();               // 1. stop new work
        executor_.drain(std::chrono::seconds{5});  // 2. finish in-flight
        db_.close();                               // 3. release deps
        metrics_.flush();                          // 4. final telemetry
    }
    // destructor now only releases already-drained resources
};
```

关键洞见在于：析构顺序是语言机制，不是关闭策略。两者必须一起设计，而且任何飞行中工作仍然依赖的资源，在开始拆资源之前，都应先通过显式排空逻辑处理完毕。

如果服务本来就能从异步组合中受益，协程确实可以改善结构，特别是在以 I/O 为主的请求路径上。但如果使用协程只是为了不写回调，而生命周期模型依旧模糊，那就是一笔糟糕的交易。若一个协程帧捕获了借用的请求数据、执行器引用和取消状态，却没有清晰的所有者，那你只是把 bug 压缩了，并没有消除它。只有在所有权模型本身已经成立时，协程才值得用来简化结构。

## 背压是产品决策，不是队列细节

在小型服务里，背压是局部技术选择开始变成用户可见策略的地方。系统饱和时会发生什么？请求会阻塞、快速失败、丢弃可选工作、退化到旧数据，还是在有限等待后超时？如果答案是“队列继续增长”，那这个服务还没有完成一个运维决策。

现代 C++ 能帮助你实现这些决策，但它不会替你做决策。`std::expected` 可以把过载表示成稳定错误。值类型工作项让队列成本变得可见。基于 `std::chrono` 的截止时间可以显式穿过调用图。结构化取消让请求在调用者不再受益时放弃其子工作。但这些都不能取代对过载行为的选择。

对于小型服务，通常建议优先选择显式拒绝，而不是默默把延迟越拖越长。一个有界队列加上清晰的拒绝指标，比一个“善意”的队列更容易运维；后者会把突发流量吞进去，直到内存和尾延迟变成别人的事故。代价是在负载下更快地向用户暴露失败。即便如此，这通常仍是正确的权衡，因为它保持了系统形态，并让容量问题可以被测量。

## 保持依赖边界狭窄且负责转换

小型服务内部代码往往依赖数据库、RPC 客户端、文件系统、时钟和遥测供应商。错误做法要么是立刻把它们全部抽象化，要么是让供应商类型流经整个代码库。这两种做法都会老化得很糟。

狭窄的边界适配器是务实的中间地带。服务层应当依赖用服务语言表达的契约：持久化这条记录、获取这个快照、发出这个指标、发布这个事件。适配器负责把这些操作转换成外部 API、错误模型和分配行为。

这为服务提供了一个地方，用来规范超时、分类失败、补充可观测字段，并控制分配或复制决策。它也阻止了传输细节泄漏到应用逻辑。一个处理器应当拿到的是服务能够一致处理的、与领域相关的失败类别。

不要把这些接口过度泛化。小型服务通常需要的是薄端口，而不是企业级的抽象宇宙。适配器存在的目的，是维护所有权和失败边界，而不是在进程内模拟一个平台团队。

## 可观测性应当跟随服务形态

如果服务根、请求模型和并发模型都是显式的，可观测性就会更容易。请求标识、队列深度、活跃工作数、依赖延迟、取消计数、启动失败和关闭时长，都能自然映射到具名边界上。如果代码库由隐藏的全局变量和脱离控制的工作拼成，遥测也会变得模糊，因为没人说得清工作从哪里开始、又在哪里结束。

一个小型服务通常至少应暴露这些信号：

- 按类别划分的启动成功或失败。
- 请求速率、延迟直方图和失败类别。
- 有界工作队列的深度与拒绝计数。
- 下游依赖的延迟与超时计数。
- 关闭时长以及被取消的飞行中操作数量。

除此之外的任何内容，都应当对应一个明确的运维问题，而不是出于恐惧。目标不是采集最大体量的遥测。目标是在服务过载、配置错误或卡在关闭流程中时，能快速定位问题。

## 验证应当针对生命周期，而不仅是行为

对小型服务最有价值的测试，通常不是“合法输入返回 200”，而是那些能在压力下证明生命周期行为的测试：非法配置会阻止就绪、过载会产生显式拒绝、被取消的工作不会提交部分状态、关闭会排空被拥有的任务而不引入 use-after-free 风险，以及依赖故障仍然被正确分类。

这组测试通常包括：围绕适配器和边界转换的聚焦单元测试、围绕启动与关闭故事的集成测试、用于内存与并发风险的 sanitizer 支撑运行，以及在运维契约重要时对可观测性的断言。举例来说，如果策略选的是“过载即拒绝”，那服务就应当暴露一个指标或结构化事件，证明这个策略在线上确实正在发生。

注意本章没有重复什么。它没有重新解释测试分类、sanitizer 或遥测流水线。那些在前面的章节已经讲过。这里的综合点在于：服务形态决定了这些工具能否产出有用证据。

## 这种形态在什么地方开始不够用

本章的建议适用于真正的小型服务：单进程、少量长生命周期依赖、有界后台工作，以及一个团队仍能在脑中持有完整运行时模型的代码库。规模再大一些，你可能就需要更显式的子系统所有权、更强的组件隔离、服务级的准入控制，或一个本身就对生命周期持明确意见的专用异步框架。

当领域被某一个本章只轻触过的约束主导时，你也应选择不同的形态：极端低延迟交易、硬实时行为、带有敌意扩展的插件宿主，或者拥有特化协议栈的公网服务器。相同原则依然成立，但工程重心会发生偏移。

## 要点总结

一个好的小型 C++23 服务，应围绕被拥有的资源、显式的启动与关闭、有界并发、短生命周期借用、狭窄的依赖适配器，以及绑定到真实生命周期边界的可观测性来构建。代码应当让人一眼看出：进程拥有什么、工作如何准入、取消如何传播，以及失败期间哪些状态仍然有效。

这些权衡都是有意的。显式边界会增加样板代码。有界队列会更早拒绝工作。值类型工作项可能比大量视图的设计多一些复制。狭窄适配器会增加边界转换代码。但与调试一个生命周期和过载行为都隐含在实现细节里的服务相比，这些成本通常都很便宜。

Review questions:

- 长生命周期服务资源的单一所有权根是什么？
- 哪些请求数据跨越了时间或线程边界，它们在跨越前是否已经变成拥有型数据？
- 并发工作在什么地方被设为有界，这个边界对应的显式过载策略是什么？
- 取消如何到达飞行中的工作，关闭完成后又保证了什么状态？
- 哪些依赖故障被转换成了稳定的服务级类别，而不是把供应商细节直接泄漏出来？
