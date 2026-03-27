# 在现代 C++ 中构建一个小型服务

小型服务往往是 C++ 团队养成工程纪律或栽跟头的分水岭。代码库尚小，人们容易即兴行事，但流程中已经埋着不少真实的失败模式：过载、启动时配置加载不全、部分写入、依赖超时、队列无限增长、关闭时的竞态条件，以及证据不足时的生产环境排障。语言本身帮不了你。

本章不是框架教程。要回答的问题更具体：如果你希望六个月后回头看，所有权、失败处理、并发和运维依然清晰可审，那么一个小型 C++23 服务该是什么样子？答案不是”把最新特性全用上”，而是选择一种服务形态，让生命周期一目了然、异步工作归属明确、资源限制显式可控，在高压下仍能快速定位问题。

本章的示例是一个配置驱动的小型服务：接收请求、校验请求、执行有界的后台工作、持久化状态，对外暴露指标和健康信息。细节刻意选得很普通。大多数生产服务在概念上并无新意，之所以出问题，是因为基本的工程边界没有划清。

## 先定义所有权单元，再定义部署单元

小型服务最常见的架构错误，是按端点、处理器或框架回调来组织代码，而非按它拥有的资源来组织。一个可部署的服务拥有一组固定的长生命周期对象：配置、监听器、执行器、连接池、存储适配器、遥测汇聚点、关闭协调机制。如果不把这些对象显式建模出来，代码就会慢慢滑向全局变量、共享单例、脱管的后台工作，以及”听天由命”的关闭流程。

服务对象应当直接体现所有权关系，由它来构造、启动和停止所有长生命周期的依赖。目的不是造一个无所不包的上帝对象，而是要有一个清晰的根，管辖那些生命周期必须一同结束的组件。

### 刻意只展示片段：掌控时序与关闭流程的服务根

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

这段代码故意写得很朴素。服务只有一个所有权根、一条停止路径、一个可以推导启动和排空顺序的地方。小型服务不需要花哨的架构。

示例项目 `examples/web-api/` 就是这一模式的具体实现。其 `main.cpp` 在一个连续的作用域中构造所有长生命周期资源——仓储、路由、中间件管道、服务器——并在进程开始接受请求前完成组装：

```cpp
// examples/web-api/src/main.cpp — scoped multi-resource construction
webapi::TaskRepository repo;
// ... seed data ...

webapi::Router router;
router
    .get("/health",       webapi::handlers::health_check(repo))
    .get("/tasks",        webapi::handlers::list_tasks(repo))
    // ... remaining routes ...

std::vector<webapi::middleware::Middleware> pipeline{
    webapi::middleware::request_logger(),
    webapi::middleware::require_json(),
};
auto handler = webapi::middleware::chain(pipeline, router.to_handler());

webapi::http::Server server{port, std::move(handler)};
server.run_until(shutdown_requested);
```

所有权根只有一个（`main`），关闭协调点也只有一个（`shutdown_requested`），每个资源都有明确的作用域。当 `run_until` 返回后，析构按声明的逆序进行——先是 server，再是 handler、router，最后是 repository。没有共享指针，没有全局注册表，没有脱管工作。

这个根通常应当直接持有具体的基础设施类型，而不是一张堆分配接口拼成的对象图再靠共享所有权勉强缝合。依赖反转仍然重要，但反转点通常落在存储、传输或遥测适配器这类边界上。进程内部，静态所有权比到处散落的 `std::shared_ptr` 更简单也更廉价，后者往往让真正的所有者在纸面上无从追溯。

### 反模式：shared_ptr 大杂烩式的请求状态

一种常见的失败模式：用 `std::shared_ptr` 把请求的生命周期横跨回调、队列和重试逻辑一路延长，却始终没有显式的所有权模型。代码编译没问题，看上去也安全，但没人说得清请求资源什么时候释放、取消信号能不能到达每个持有者、关闭流程能否确定性地完成。

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

正确做法是提取一个有明确归属的工作项，让它沿着流水线在清晰的交接点之间传递。

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

有明确归属的工作项让设计问题浮出水面：哪些数据需要跨越请求边界存活、谁有权取消它，以及关闭时它最终归向何处。

## 启动要么交出一个可运行的服务，要么干净地失败

很多服务故障在第一个请求到达之前就已经埋下。配置只加载了一半；一个子系统正常，另一个还没起来；线程已经启动，健康状态却尚未建立；后台定时器在依赖校验完成前就开始跑了。进程之所以报告”ready”，仅仅是因为某个构造函数碰巧返回了。

启动要回答的核心问题不是每个组件能否单独初始化，而是进程能否达到一个整体一致的运行状态。启动应当按阶段组织，围绕依赖校验和显式的失败边界展开。

常见且有效的启动顺序如下：

1. 加载并验证不可变配置。
2. 构造带有显式限制的资源拥有型适配器。
3. 校验就绪性所依赖的下游服务。
4. 只有在进程整体一致之后，才启动监听器和后台工作。
5. 只有前面步骤全部成功后，才发布就绪状态。

在 C++23 中，启动路径用 `std::expected` 往往比异常更合适，因为启动过程天然会积累各种基础设施故障，这些故障需要归入稳定的运维类别。配置文件格式错误、端口被占用、存储 schema 不兼容，这些信息应当作为设计好的启动失败暴露出来，而不是把实现内部碰巧泄漏的异常文本原样抛到最上层。

代价是代码更啰嗦。`std::expected` 要求你逐一写出错误转换点。但在服务启动场景下，这笔开销通常值得，因为隐藏的异常路径会让进程状态更难推理。叶子函数或内部辅助函数，如果包裹它们的边界足够清晰，用异常仍然没问题。关键在于启动阶段对外暴露的必须是一个连贯统一的契约。

## 请求处理中，借用数据就该是短命的

小型服务的一个典型错误是把临时的请求数据悄悄变成长生命周期的内部状态。请求头变成了异步任务中的 `std::string_view` 成员；解析后的载荷视图被存进缓存；回调捕获的引用指向的对象早已随请求一起销毁。服务之所以”看起来没问题”，只是因为慢路径、重试路径或队列延迟还没来得及暴露这个错误。

规则很简单：借用视图适合做同步检查，不适合用作隐式存储。在请求路径内部，只要生命周期局部且明确，尽管放心使用 `std::string_view`、`std::span` 和范围视图。一旦数据要跨越时间、线程、队列或重试边界，就必须在那之前将其转换为拥有所有权的表示。

这也是服务代码受益于显式请求模型的原因。先把输入解析、校验成一个值类型，由它持有后台工作必须保留的数据。保持这个模型足够小，让复制成为一个可感知的成本；当设计上需要时间解耦时，再将它 move 进异步工作。

示例项目中的 handler 函数（见 `examples/web-api/src/modules/handlers.cppm`）遵循了这一原则。每个 handler 工厂返回一个 `std::function<Response(const Request&)>`，即一个值类型，仅捕获对 repository 的引用。handler 通过 const 引用接收请求、提取所需数据（路径参数、解析后的 body）、执行操作、返回响应值。没有请求状态泄漏到函数调用之外：

```cpp
// examples/web-api/src/modules/handlers.cppm — 值类型 handler 设计
[[nodiscard]] inline http::Handler
get_task(TaskRepository& repo) {
    return [&repo](const http::Request& req) -> http::Response {
        auto segment = req.path_param_after("/tasks/");
        if (!segment) {
            return http::Response::error(400, R"({"error":"missing task id"})");
        }
        auto id_result = parse_task_id(*segment);
        if (!id_result) {
            return http::Response::error(
                id_result.error().http_status(), id_result.error().to_json());
        }
        auto task = repo.find_by_id(*id_result);
        if (!task) {
            return http::Response::error(404,
                std::format(R"({{"error":"task {} not found"}})", *id_result));
        }
        return http::Response::ok(task->to_json());
    };
}
```

`path_param_after` 返回的借用 `string_view` 绝不会超出同步 handler 调用的范围。repository 返回的 `Task` 是值拷贝。响应以值构造并返回。没有生命周期歧义。

这也是很多 C++ 服务代码库过度使用 `std::shared_ptr<request_context>` 的原因。共享所有权看起来像是应对异步生命周期的便捷后门，但它遮蔽了真正需要做出的设计决策：请求的哪些部分需要存活、归谁管、什么时候可以丢弃。对小型服务而言，提取一个有明确归属的工作项并 move 进队列，通常比把整个请求对象图的生命周期一股脑延长要好得多。

## 并发必须有界、有主、可取消

服务的并发模型远比具体用了哪些并发原语更重要。小型服务很少需要大型自定义调度器，但有三样东西必须到位。

第一，并发工作量必须有上限。如果过载能直接转化为无限制的队列增长，你设计的就不是服务，而只是把故障推迟了。有界执行器、信号量、准入控制以及按请求分配的时间预算，比精巧的线程池内部实现更有价值。

第二，工作必须有归属。分离线程和 fire-and-forget 任务很诱人，因为它们让局部代码显得更短；但它们同时也摧毁了关闭语义。既然服务能往队列里塞工作，它就应当知道工作何时开始、何时结束、取消信号如何送达。

第三，取消必须是常规模型的一部分，不能事后补救。`std::jthread` 和 `std::stop_token` 在这方面很有帮助，因为它们把停止传播提升到了类型级契约。它们并非万能，你仍然需要让工作单元在合理的边界点检查 token，也需要让存储或网络操作把取消映射为一致的错误。但它们至少把这个问题强制写进了代码，而不是留在注释里。

### 反模式：阻塞事件循环

最常见的服务故障之一，是在本该驱动 I/O 或分发请求的线程上做同步阻塞操作。轻负载时一切看起来正常；流量一上来，事件循环卡在数据库调用、DNS 解析或文件读取里，整个服务随之崩塌。

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

正确做法是把阻塞工作分派到有界执行器，让监听线程始终保持非阻塞。

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

缺乏显式关闭逻辑的服务会导致 use-after-free、部分写入、孤儿连接，以及只能靠编排器发 `SIGKILL` 强杀的僵死进程。开发环境里这类问题往往看不见，因为进程退出太快。到了生产环境，正在处理中的工作和后台定时器会带来真实的竞态条件。

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

正确做法是把关闭变成一个显式的、有序的操作，先排空在途工作，再销毁资源。

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

关键在于：析构顺序是语言层面的机制，不是关闭策略。两者必须协同设计。凡是在途工作仍然依赖的资源，都必须先通过显式的排空逻辑处理完毕，然后才能开始拆除。

示例项目展示了这一模式的干净版本。在 `examples/web-api/src/main.cpp` 中，信号处理函数设置一个原子标志；`Server::run_until()`（见 `examples/web-api/src/modules/http.cppm`）轮询该标志并将停止请求转发给 `std::jthread`：

```cpp
// examples/web-api/src/main.cpp — signal handler
namespace {
    std::atomic<bool> shutdown_requested{false};
    extern "C" void signal_handler(int) {
        shutdown_requested.store(true, std::memory_order_release);
    }
}

// examples/web-api/src/modules/http.cppm — Server::run_until()
void run_until(const std::atomic<bool>& should_stop) {
    std::jthread server_thread{[this](std::stop_token st) {
        run(st);  // checks st.stop_requested() each accept loop iteration
    }};
    while (!should_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server_thread.request_stop();
    // jthread auto-joins on destruction
}
```

流程是：SIGINT/SIGTERM 设置标志 → `run_until` 将停止转发到 jthread 的 `stop_source` → accept 循环退出 → jthread join → `run_until` 返回 → `main` 按声明的逆序销毁资源。每一步都是显式的、有序的、协作式的。没有脱管线程，不需要 `SIGKILL`。

如果服务本身适合做异步组合，协程确实能改善代码结构，尤其是 I/O 密集的请求路径。但如果引入协程只是为了省掉回调，而生命周期模型依旧模糊，那就是亏本买卖。当一个协程帧捕获了借用的请求数据、执行器引用和取消状态，却找不到一个明确的所有者时，你只是把 bug 压缩了，并没有消除它。协程值得用来简化设计的前提是所有权模型本身已经站得住脚。

## 背压是产品决策，不是队列细节

在小型服务里，背压（backpressure，即下游向上游反馈负载信号的机制）恰恰是局部技术选择变成用户可感知策略的临界点。系统饱和时该怎么办？让请求排队等待、立刻快速失败、丢掉非必要的工作、降级到陈旧数据，还是在有限等待后超时？如果答案是”队列继续涨”，说明这个服务在运维层面还缺一个决策。

现代 C++ 能帮你落地这些决策，但不会替你拍板。`std::expected` 可以把过载表达为稳定的错误类别；值类型工作项让队列开销一目了然；基于 `std::chrono` 的截止时间可以显式贯穿整个调用链；结构化取消让请求在调用方不再需要结果时及时放弃子任务。但这一切都不能替代"过载时该怎么办"这个决策。

对于小型服务，一般建议优先选择显式拒绝，而非默默地把延迟越拖越高。一个有界队列配合清晰的拒绝指标，比一个”好心”吞下突发流量的队列更好运维，后者会一直吸收请求，直到内存和尾延迟变成别人的线上事故。代价是在高负载下更早地向用户暴露失败。这通常仍是正确的取舍：它保住了系统的整体形态，也让容量问题变得可度量。

### 中间件管道：横切关注点的组合

示例项目的中间件系统（见 `examples/web-api/src/modules/middleware.cppm`）展示了横切关注点如何在彼此解耦、也不与 handler 逻辑耦合的前提下完成组合。`Middleware` 是一个 `std::function`，接受请求和下一个 handler，返回响应。`middleware::chain()` 将一组中间件折叠到基础 handler 之上：

```cpp
// examples/web-api/src/modules/middleware.cppm
template <std::ranges::input_range R>
    requires std::same_as<std::ranges::range_value_t<R>, Middleware>
[[nodiscard]] http::Handler
chain(R&& middlewares, http::Handler base) {
    http::Handler current = std::move(base);
    for (auto it = std::ranges::rbegin(middlewares);
         it != std::ranges::rend(middlewares); ++it)
    {
        current = apply(*it, std::move(current));
    }
    return current;
}
```

在 `main.cpp` 中，管道以声明式方式组装：

```cpp
std::vector<webapi::middleware::Middleware> pipeline{
    webapi::middleware::request_logger(),
    webapi::middleware::require_json(),
};
auto handler = webapi::middleware::chain(pipeline, router.to_handler());
```

每个中间件都是独立的、可单独测试的，以值组合。添加新的横切关注点（限流、认证、追踪传播）只需向 vector 追加元素，无需修改 handler 签名。这是函数式装饰器模式应用于 HTTP 处理的实践：运维需求增长时，服务结构仍能保持扁平。

## 保持依赖边界狭窄且负责转换

小型服务的内部代码往往要依赖数据库、RPC 客户端、文件系统、时钟和遥测厂商。常见的两种错误做法：一是上来就把它们全部抽象化，二是任由厂商类型在整个代码库里到处流窜。两种做法长期后果都很糟。

窄边界适配器是务实的中间路线。服务层应当依赖用自身语言表达的契约：持久化这条记录、获取这个快照、发送这个指标、发布这个事件。由适配器负责将其翻译为外部 API 调用、错误模型和内存分配策略。

这为服务提供了一个统一的位置来规范超时策略、归类失败、补充可观测性字段，以及控制分配与复制决策。它同时也防止了传输层细节渗透到业务逻辑中。处理器拿到的应当是与领域相关的、服务能一致应对的失败类别。

不要把这些接口过度泛化。小型服务需要的是薄端口，不是企业级的抽象帝国。适配器的目的是守住所有权和失败边界，不是在进程内部模拟一个平台团队。

## 可观测性应当跟随服务形态

当服务根、请求模型和并发模型都是显式的，可观测性自然水到渠成。请求标识、队列深度、活跃工作数、依赖延迟、取消计数、启动失败和关闭时长，都能映射到代码中的具名边界上。反过来，如果代码库充斥着隐藏的全局变量和脱管的后台工作，遥测数据也会跟着含混不清，没人说得清工作从哪里开始、在哪里结束。

一个小型服务通常至少应当暴露以下信号：

- 按类别划分的启动成功或失败。
- 请求速率、延迟直方图和失败类别。
- 有界工作队列的深度和拒绝计数。
- 下游依赖的延迟和超时计数。
- 关闭时长，以及被取消的在途操作数量。

超出这个范围的指标，都应当有对应的运维问题来支撑，而非出于"万一用得上"的恐惧。目标是在服务过载、配置出错或卡在关闭流程中时能尽快定位问题，不是采集尽可能多的遥测数据。

## 测试应针对生命周期，而不仅仅是行为

对小型服务最有价值的测试，很少是”合法输入返回 200”这一类。真正有价值的是那些在压力下验证生命周期行为的测试：非法配置能否阻止就绪、过载是否触发显式拒绝、已取消的工作是否不会提交半成品状态、关闭流程是否能排空在途任务且不引入 use-after-free 风险、依赖故障是否仍被正确归类。

这类测试通常包括：针对适配器和边界转换的聚焦单元测试、围绕启动与关闭场景的集成测试、配合 sanitizer 运行以排查内存和并发隐患，以及在运维契约要求严格时对可观测性输出的断言。例如，如果选定的策略是”过载即拒绝”，那服务就应当暴露一个指标或结构化事件，用以证明该策略在线上确实生效。

本章刻意没有重复的内容：测试分类体系、sanitizer 的用法、遥测流水线的搭建，这些在前面的章节已经讲过。本章的综合要点在于服务的形态决定了这些工具能否产出有价值的证据。

## 这种形态在什么地方开始不够用

本章的建议适用于真正意义上的小型服务：单进程、少量长生命周期依赖、有界的后台工作，以及团队仍然能把完整运行时模型装在脑子里的代码规模。再往上走一步，你可能就需要更显式的子系统所有权划分、更强的组件隔离、服务级准入控制，或者一个对生命周期管理已有成熟方案的专用异步框架。

当领域主要受限于本章只略微提及的某个约束时，你也应选择不同的架构形态：超低延迟交易、硬实时系统、需要防范恶意扩展的插件宿主，或者拥有专用协议栈的公网服务器。同样的原则依然适用，但工程的重心会有所偏移。

## 要点总结

一个好的小型 C++23 服务，围绕有明确归属的资源、显式的启动与关闭、有界的并发、短命的借用、窄边界的依赖适配器，以及与真实生命周期边界对齐的可观测性来构建。代码应当让人一眼看出进程拥有什么、工作如何被准入、取消如何传播、故障期间哪些状态仍然有效。

这些取舍是刻意为之的。显式边界会带来更多样板代码；有界队列会更早拒绝请求；值类型工作项可能比满是视图的设计多一些复制开销；窄适配器会增加边界转换代码。但和调试一个生命周期与过载行为全藏在实现细节里的服务相比，这些成本通常微不足道。

复习问题：

- 长生命周期服务资源的唯一所有权根在哪里？
- 哪些请求数据跨越了时间或线程边界？在跨越之前，它们是否已转为拥有所有权的数据？
- 并发工作在哪里被限定为有界？由此对应的显式过载策略是什么？
- 取消信号如何送达在途工作？关闭完成后保证了哪些状态？
- 哪些依赖故障被转换成了稳定的服务级错误类别，而非把供应商内部细节直接暴露出去？
