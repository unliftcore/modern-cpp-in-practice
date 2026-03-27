# 原生系统的可观测性

测试和 sanitizer 能减少缺陷逃逸到生产环境的概率，但无法杜绝线上故障，也无法解释系统在真实负载、真实数据、真实依赖故障和真实发布条件下的实际行为。可观测性（observability）就是为此而生。

在原生系统中，可观测性不足的代价格外高。托管服务卡住时，好歹还有运行时提供异常信息、堆快照和标准化的追踪钩子。而 C++ 系统面对的可能是：符号化残缺的崩溃转储、被某个阻塞依赖卡死的线程池、无法对应到逻辑归属的 RSS 增长，以及只知道"某次部署后延迟涨了三倍"的运维人员。如果日志、指标、追踪和转储制品不是在事故发生之前就设计进系统的，调查就只能靠猜。

本章的主题是运行时证据。测试回答的是代码在发布前是否满足契约。Sanitizer 和静态分析在开发与 CI 阶段机械地搜索已知缺陷类型。可观测性要回答的是另一个问题：当原生系统实际运行时，哪些信号能让工程师快速定位故障、过载或性能劣化的原因？

## 从运维问题出发

如果可观测性的起点是"加几条日志"，那它注定是薄弱的；只有从运维问题出发，才能真正做好。

对于一个服务，这些问题可能是：

- 为什么请求延迟升高了，而 CPU 占用却保持平稳？
- 哪些依赖故障正在引发重试、队列膨胀或工作只完成了一半？
- 关闭时卡住了，是因为任务没有响应取消信号，还是因为下游依赖始终没有排空？
- 内存增长是泄漏、碎片化、缓存，还是积压导致的？

对于一个库，问题则会转变：

- 哪个宿主操作触发了失败，输入或版本上下文是什么？
- 库的时间消耗在解析、等待、加锁、分配还是 I/O 上？
- 宿主能否将库的失败与自身的请求或任务标识符相关联？

这些问题决定了哪些字段、指标和 span 值得记录。缺少这些问题的指引，团队往往产出大量但价值低的遥测数据：堆满字符串的日志、缺乏维度的计数器，或者什么都记了唯独漏掉排队、重试和取消的追踪。

### 没有可观测性时调试是什么样子

举个具体的例子。假设有一个处理文件上传的服务，某用户反映上传超时。以下是没有结构化可观测性时的处理函数：

```cpp
void handle_upload(const http_request& req) {
    std::cout << "INFO: Processing upload" << std::endl;

    std::cout << "INFO: Starting validation" << std::endl;
    auto validation = validate(req);
    if (!validation) {
        std::cerr << "ERROR: Validation failed: "
                  << validation.error().message() << std::endl;
        return;
    }

    std::cout << "INFO: Storing file" << std::endl;
    auto store_result = store(req);
    if (!store_result) {
        std::cerr << "ERROR: Store failed: "
                  << store_result.error().message() << std::endl;
        return;
    }

    notify(req);
    std::cout << "INFO: Upload complete" << std::endl;
}
```

哪次上传失败了？是同一个用户还是不同用户？在等什么，磁盘 I/O、下游服务还是锁？store 调用花了多长时间？失败后有没有重试？这些问题从这个函数产生的日志中一个都答不上来。值班工程师只能按时间戳范围 grep 日志，靠猜测建立关联，然后请用户重现问题。

再看同一个函数加上结构化日志、关联 ID 和维度指标之后的效果：

```cpp
void handle_upload(upload_context& ctx) {
    auto span = ctx.tracer().start_span("handle_upload", {
        {"request_id", ctx.request_id()},
        {"user_id",    ctx.user_id()},
        {"file_size",  ctx.file_size()},
        {"shard",      ctx.shard_id()},
    });

    ctx.log(severity::info, "upload_started", {
        {"request_id", ctx.request_id()},
        {"file_name",  ctx.file_name()},
        {"file_size",  std::to_string(ctx.file_size())},
    });

    auto validation = validate(ctx);
    if (!validation) {
        ctx.log(severity::warning, "validation_failed", {
            {"request_id", ctx.request_id()},
            {"reason",     validation.error().category()},
        });
        ctx.metrics().increment("upload_failures", 1,
            {{"reason", "validation"}, {"shard", ctx.shard_id()}});
        return;
    }

    auto store_result = store(ctx);
    if (!store_result) {
        ctx.log(severity::error, "store_failed", {
            {"request_id",  ctx.request_id()},
            {"dependency",  "blob_store"},
            {"error_class", store_result.error().category()},
            {"latency_ms",  std::to_string(store_result.elapsed_ms())},
        });
        ctx.metrics().increment("upload_failures", 1,
            {{"reason", "store"}, {"shard", ctx.shard_id()}});
        return;
    }

    ctx.metrics().observe_latency("upload_duration_ms", span.elapsed_ms(),
        {{"shard", ctx.shard_id()}});
    ctx.log(severity::info, "upload_complete", {
        {"request_id", ctx.request_id()},
        {"latency_ms", std::to_string(span.elapsed_ms())},
    });
}
```

现在值班工程师按 `request_id` 过滤，立刻看到超时发生在 `store` 阶段、`dependency=blob_store`，再查看按 shard 分组的 `upload_duration_ms` 直方图，发现 shard-3 延迟在 09:40 飙升。blob store 仪表盘印证了该依赖当时确实在劣化。整个调查从数小时缩短到数分钟。

两个版本的 `handle_upload` 做的事情一样：校验、存储、通知。差别在于写代码时就把运维问题考虑在内了。

## 日志应当解释决策与状态转换

日志最有价值的时候，是在记录系统做了什么决策、当时的关键状态是什么，而不是流水账式地记录每个函数调用。在原生系统中，这种克制尤其重要，因为日志的数量和开销很容易变成性能瓶颈。

良好的生产日志是结构化的、稀疏的、稳定的。

- 结构化：重要字段以机器可读的键值对输出，不要藏在大段文字里。
- 稀疏：正常路径保持安静，只在异常路径上输出详细信息。
- 稳定：字段名称和含义不会每个迭代周期都变。

当操作失败时，日志通常应记录身份标识、分类信息和当时的局部运行上下文。

- 请求或任务标识符。
- 操作名或路由名。
- 失败类别，而不仅仅是格式化消息。
- 可重试性或永久性（如果代码能判断的话）。
- 对诊断有帮助的资源指标，例如队列深度、shard、对端、重试次数等。
- 在发布状态可能相关时，还应包含版本或构建元数据。

避免两个常见错误。

第一，不要把日志当成指标类问题的唯一信息来源。如果需要了解重试率或队列深度，直接发指标，不要逼运维人员从文本里重新拼凑。第二，不要仅仅因为某次事故需要就把高基数数据或敏感内容全部记下来，放在专门的采样或调试路径中按需开启。

`std::source_location` 在低频的内部诊断或基础设施代码中很实用，特别是需要一个稳定的调用点标签又不想手工维护字符串的时候。但它不能替代有意义的操作名称。一条 `source=foo.cpp:412` 的日志，远不如 `operation=manifest_reload phase=commit` 有用。

示例项目中的 `request_logger()` 中间件（见 `examples/web-api/src/modules/middleware.cppm`）展示了一个最小但结构化的逐请求日志起点：

```cpp
// examples/web-api/src/modules/middleware.cppm — request_logger()
return [](const http::Request& req, const http::Handler& next) -> http::Response {
    auto start = std::chrono::steady_clock::now();
    auto resp = next(req);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    std::cout << std::format("[{}] {} {} → {} ({} μs)\n",
        "LOG",
        http::method_to_string(req.method),
        req.path,
        resp.status,
        ms
    );
    return resp;
};
```

每条响应都携带了 method、path、状态码和延迟。这还不是生产级的结构化日志，字段嵌在格式化文本里，而非以机器可查询的键值对输出。但它体现了正确的直觉：在一个横切点上捕获身份标识（路由）、结果（状态码）和时序（延迟）。将其演化为结构化 JSON 或键值格式输出是自然的下一步。

### 非结构化日志与结构化日志的实际差异

非结构化日志与结构化日志的差异在事故中体现得最明显。此时读日志的人时间紧迫，而且往往不是写这段代码的人。

```cpp
// 非结构化：人类可读，但对机器不友好。
log("Failed to connect to database server db-prod-3 after 3 retries "
    "(last error: connection refused), request will be dropped");
```

这行日志包含有用信息，但提取它们就得解析自然语言文本。不靠脆弱的正则表达式，根本没法按重试次数、依赖名称或错误类型过滤。在整个服务集群范围内从这类日志中聚合失败模式，费力又容易出错。

```cpp
// 结构化：相同信息，机器可查询。
ctx.log(severity::error, "dependency_connect_failed", {
    {"dependency",  "db-prod-3"},
    {"attempts",    "3"},
    {"last_error",  "connection_refused"},
    {"action",      "request_dropped"},
    {"request_id",  ctx.request_id()},
});
```

现在 `dependency_connect_failed` 事件可以计数、按依赖名称过滤、与特定请求关联。字段名跨代码变更保持稳定，所以即使有人改写了日志措辞，仪表盘和告警也不会失效。

## 指标应当追踪吞吐量、饱和度和失败形态

指标能回答日志答不好的问题：速率、分布、长期趋势漂移，以及跨实例的对比。对原生系统，最有价值的指标通常分三类。

第一类是吞吐量与延迟：请求速率、任务完成率、重试率，以及关键阶段的延迟直方图。延迟务必用直方图，不能只看均值，原生系统的性能问题往往出在长尾。

第二类是饱和度：队列深度、工作线程利用率、打开的文件描述符数、连接池占用率、分配器压力、待触发的定时器，以及未完成的后台任务。这些信号能说明系统是在健康地忙碌，还是在不断积压消化不了的工作。

第三类是失败形态：按错误类别统计的计数、超时次数、取消次数、解析失败、丢弃的工作、崩溃重启，以及降级模式的激活次数。这些指标能揭示系统失败的根源：是依赖变慢了，是输入质量变了，还是内部背压被触发了。

示例项目中的 `ErrorCode` 枚举（见 `examples/web-api/src/modules/error.cppm`）展示了失败形态指标的基础。这个封闭集合——`not_found`、`bad_request`、`conflict`、`internal_error`——配合 `constexpr to_http_status()` 映射，为每种失败赋予了稳定的类别。在生产演进中，你可以在 handler 边界按 `ErrorCode` 维度递增计数器，把类型系统的分类直接变成可查询的指标标签，而无需发明临时的字符串标签。

标签要谨慎使用。高基数标签是拖垮指标系统的最快途径。请求 ID、用户 ID、文件路径、任意异常文本和原始对端地址通常不该做标签。地域、路由、依赖名称、结果类别和有限范围的 shard 标识符通常是合适的。

Gauge 也需要警惕，加起来容易，读起来容易误判。队列深度 gauge 突然跳升，到底是短暂峰值还是持续恶化？尽量把 gauge 和速率或直方图搭配使用，这样运维人员才能判断情况是在好转还是在恶化。

## 追踪需要跟随异步所有权，而非仅跟随同步调用

在分布式服务和异步原生系统中，追踪（tracing）是弄清端到端时间花在哪里的唯一实用手段。但 C++ 代码往往因为没有在执行器、回调、线程跳转和协程挂起等边界处保持上下文，白白损失了追踪的价值。

如果一个请求进入服务、入队后台工作、等待下游调用，然后在另一个 worker 上恢复执行，追踪仍应呈现为一次连贯的操作。这就要求在工作所有权发生交接的边界处显式传播追踪上下文。

前面章节的设计决策在这里体现出价值。结构化并发和显式取消作用域天然让追踪更清晰，因为父子关系本身就是有意义的。而脱管的工作和随意派生的线程则会让追踪碎片化为一堆互不相关的 span。

应当为那些对应实际等待或服务边界的阶段创建 span：

- 工作开始前在队列中等待的时间。
- 执行本地 CPU 工作的时间。
- 等待下游 I/O 的时间。
- 重试或退避的时间。
- 因取消、关闭或过载丢弃造成的时间损失。

不要为每个辅助函数都创建 span，那只会制造噪声。追踪的目的是揭示延迟的结构和依赖的形态，不是把调用图再描述一遍。

示例项目中的中间件管道（见 `examples/web-api/src/modules/middleware.cppm` 中的 `middleware::chain()`）天然适合作为追踪上下文的传播载体。每个中间件包装下一个 handler，同时能访问请求和响应。向管道中插入一个追踪中间件，在调用 `next` 前启动 span，附加到请求上下文，响应返回后关闭 span，就足够了。管道已经以 `std::function` 包装器链的形式组合，添加追踪阶段不需要修改 handler 签名，这正是让追踪传播切实可行的横切插入点。

### 没有追踪上下文：隐形的排队时间

异步原生服务中有一个常见问题：延迟其实花在了排队上，而不是执行上。如果追踪上下文没有跨执行器边界传播，这部分时间就是不可见的：

```cpp
// 没有追踪上下文传播。span 只覆盖执行阶段，不覆盖等待阶段。
void enqueue_work(thread_pool& pool, request req) {
    pool.submit([req = std::move(req)] {
        auto span = tracer::start_span("process_request");  // 工作运行时才开始计时。
        process(req);
    });
    // submit() 到 lambda 实际执行之间的时间丢失了。
    // 如果线程池饱和，请求在队列中等待 500ms，
    // 但追踪显示 2ms 的执行时间。运维人员在追踪中
    // 看到低延迟，而用户体验到高延迟。排队时间是
    // 一个盲点。
}
```

正确传播上下文后，完整图景一目了然：

```cpp
void enqueue_work(thread_pool& pool, request req, trace_context ctx) {
    auto enqueue_time = steady_clock::now();
    pool.submit([req = std::move(req), ctx = std::move(ctx), enqueue_time] {
        auto queue_span = ctx.start_span("queued", {
            {"queue_ms", std::to_string(duration_cast<milliseconds>(
                steady_clock::now() - enqueue_time).count())},
        });
        queue_span.end();

        auto exec_span = ctx.start_span("process_request");
        process(req);
    });
}
```

现在追踪中显示两个 span——排队和执行——都挂在父请求下面。一旦排队时间占了大头，在追踪瀑布图中一眼就能看出来。这恰恰是只靠指标（比如平均处理时间）会系统性掩盖的那类延迟。

## 崩溃诊断是可观测性的一部分，不是独立的应急工作

原生服务和工具必须在第一次崩溃发生之前就准备好崩溃处理方案，不能止步于"开启转储"。你需要知道转储存在哪里、怎么符号化、怎么映射到精确的构建版本、运维人员如何把它和日志与追踪关联起来，以及上面附带了哪些进程元数据。

至少，一个崩溃事件应当能够关联到：

- 精确的二进制或构建 ID。
- 匹配构建生成的符号文件。
- 部署元数据，例如版本、环境和上线环。
- 失败操作前后最近的结构化面包屑（breadcrumb）。
- 线程标识，以及尽可能提供的相关线程栈回溯。

上一章讨论的构建工作正是这一切的基础。符号服务器、构建 ID 和发布元数据属于构建层面的事务，但它们的运维价值在凌晨三点出事故、值班工程师需要看到有效栈回溯而不是一堆裸地址的时候才体现出来。

崩溃上报还需要策略上的考量。有些组件应该快速失败，继续运行可能导致数据损坏。另一些组件可以隔离失败的请求或插件，让宿主进程继续存活。可观测性应该让这种决策在事后清晰可查。如果进程因不变量被违反而主动中止，终止前应输出足够的上下文，让这次崩溃能和随机的段错误区分开来。

## 原生系统中资源可见性更为重要

C++ 服务中有一个反复出现的运维难题：逻辑工作量增长和内存缺陷分不清。RSS 在涨、延迟在升，所有人都在问"是不是有泄漏？"有时确实是，但更多时候原因没那么干净：分配器保留、过大的缓存、无界队列、停滞的消费者、mmap 增长、文件描述符泄漏，或新流量模式下的内存碎片化。

靠单一指标解决不了这个问题。需要一组资源信号，把运行时行为和可能的原因串联起来。

- RSS 和虚拟内存，用于粗略了解进程形态。
- 分配器特有统计数据（在可用时）。
- 队列深度和积压时龄，反映内存中工作的堆积情况。
- 打开的文件描述符或句柄计数。
- 活跃线程数和阻塞线程指标。
- 连接池占用率和超时计数。
- 那些有意保留内存的组件的缓存大小和淘汰指标。

目的不是把每个分配器 bin 或内核计数器都暴露出来，而是让各种可能的失败模式能区分开。内存上升的同时队列深度和积压时龄也在涨，那过载比泄漏更可能是原因。内存上升但队列深度平稳、句柄计数在涨，资源泄漏就变得更可信。可观测性的作用就是缩小排查范围。

## 库需要宿主拥有的遥测边界

可复用库不应假定存在全局日志框架、指标后端或追踪 SDK。这和本书前面讨论过的依赖反转问题一样，只不过这次以运维的形式表现出来。库应当暴露一个窄小的诊断接口，由宿主来实现。

### 局部示例：一个面向库的诊断接收器

```cpp
enum class severity { debug, info, warning, error };

struct diagnostic_field {
    std::string_view key;
    std::string_view value;
};

struct diagnostics_sink {
    virtual ~diagnostics_sink() = default;

    virtual void record_event(severity level,
                              std::string_view event_name,
                              std::span<diagnostic_field const> fields) noexcept = 0;

    virtual void increment_counter(std::string_view name,
                                   std::int64_t delta,
                                   std::span<diagnostic_field const> dimensions) noexcept = 0;
};
```

这类接口让库保持本分。库可以上报解析失败、重试、缓存淘汰或重加载耗时，不必把某个厂商 SDK 硬编码进每个使用它的二进制文件。怎么附加请求 ID、怎么导出指标、怎么接入追踪系统，都由宿主决定。

代价是需要额外的抽象设计。对于仅限内部使用的小型组件，直接集成也无妨。但对于可复用库，由宿主掌控遥测通常是更清晰的长期方案。

## 应避免的做法

原生可观测性常见的几种走偏方式：

- 每次分配、加锁、函数入口都打日志，以为数据越多越保险。
- 用高基数标识符做指标维度。
- 追踪跟着同步辅助函数走，却在执行器或协程边界处丢掉了异步上下文。
- 发布时符号化做得很弱，出了事又指望崩溃分析能行。
- 把日志当成唯一的运维手段，而不是把日志、指标、追踪和转储组合使用。
- 让库的遥测绑死在特定的服务日志栈上。

以上做法都会增加成本，却带不来相应的诊断价值。

## 要点总结

原生系统的可观测性是运行时证据的设计工作。从具体的运维问题出发，用日志记录决策和状态转换，用指标追踪速率和饱和度，用追踪揭示端到端延迟结构，用崩溃制品支撑事后调试。保持异步和所有权边界的完整性，这些信号才有意义。暴露足够的资源信号，让运维人员能分清泄漏、积压、碎片化和依赖停滞。

核心权衡在于开销与解释质量。遥测会增加 CPU、内存、存储和设计复杂度；太稀疏则拉长事故处理时间，让原生故障变得模棱两可。选择能回答真实运维问题的最小信号集，然后保持它的稳定性。

评审问题：

- 这个服务或库今天不靠猜测能回答哪些运维问题？
- 哪些日志字段足够稳定、足够结构化，能支撑自动化处理，而不只是给人看的文本？
- 哪些指标能把吞吐量、饱和度和失败形态区分开，而不是混为一谈？
- 追踪上下文能否跨越执行器跳转、回调和协程挂起边界存活下来？
- 生产环境的崩溃能否关联到精确的构建标识、符号文件以及附近的运行上下文？
