# 范围、视图与生成器

ranges（范围）和 generator（生成器）吸引人，是因为它们能把迭代压缩成类似数据流的写法。这种写法有时确实是生产代码需要的；但有时它也会把生命周期 bug、隐性开销和难以调试的惰性行为引入原本简单直白的代码。

真正要问的不是 range pipeline 是否优雅，而是：惰性组合在什么场景下比普通循环更能揭示工作的结构？又在什么场景下反而掩盖了所有权、错误处理和性能开销？C++23 提供了 range 机制和用于拉取式序列的 `std::generator`，但它们不应成为一切迭代的默认写法。

本章的示例取自真实场景：导出前过滤日志、在批处理作业中转换数据行、将分页或协程驱动的数据源包装为拉取式序列。这些场景面临的设计压力相同，工作随时间陆续到达，代码需要表达一连串转换，而风险在于延迟执行、借用状态，以及”数据究竟存放在哪里”变得模糊。

## 只有当数据流是主线时，pipeline 才值得使用

很多任务用普通循环就够了。循环的执行顺序一目了然，副作用清楚，单步调试方便。Range pipeline 真正有价值的场景是：工作的本质结构可以概括为”取一个序列，筛掉部分元素，对留下的做转换，最后物化或消费结果”。

假设日志导出 worker 收到一批已解析的记录，需要挑出与安全相关的条目，投影到导出 schema 后发送。先看 C++20 之前手写迭代器的写法：

```cpp
// Pre-C++20: manual iterator loop with filter + transform
std::vector<ExportRow> export_rows;
for (auto it = records.begin(); it != records.end(); ++it) {
    if (it->severity >= Severity::warning && !it->redacted) {
        ExportRow row;
        row.timestamp = it->timestamp;
        row.service   = it->service;
        row.message   = it->message;
        export_rows.push_back(row);
    }
}
```

这个版本能用，但过滤逻辑和转换逻辑揉在了同一个循环体里。读者得先看懂 `if` 才知道保留了什么，再看循环体才知道产出了什么。场景一复杂，这类循环就会堆满嵌套条件、提前 `continue`、索引运算和手工记账，数据流看不清了。基于索引的变体（`for (size_t i = 0; i < records.size(); ++i)`）更是 off-by-one 错误的重灾区，尤其循环体还要修改容器或索引身兼多职时。

Range 版本在结构上把关注点分离开了：

```cpp
auto export_rows = records
    | std::views::filter([](const LogRecord& r) {
          return r.severity >= Severity::warning && !r.redacted;
      })
    | std::views::transform([](const LogRecord& r) {
          return ExportRow{
              .timestamp = r.timestamp,
              .service = r.service,
              .message = r.message,
          };
      })
    | std::ranges::to<std::vector>();
```

这就是好的 range 代码。Pipeline 本身就是业务逻辑，没有棘手的就地修改，源对象的生命周期不存在歧义，末尾有一个清晰的物化点。不需要中间存储，省掉它既提升了清晰度也降低了开销。

再和另一类循环比较：那种循环要更新共享计数器、发送 metric、原地修改记录，还要有条件地重试下游写入。pipeline 反而会把执行顺序和副作用藏起来。计算本身有状态、副作用又多时，range 语法不能提升可读性。

C++20 之前还有一个常见模式值得一看，就是用于原地过滤的 “erase-remove” 惯用法：

```cpp
// Pre-C++20 erase-remove idiom
records.erase(
    std::remove_if(records.begin(), records.end(),
                   [](const LogRecord& r) {
                       return r.severity < Severity::warning || r.redacted;
                   }),
    records.end());
```

这段代码正确，但写错它太容易了。漏掉传给 `erase` 的 `.end()` 参数是经典坑，编译能过但”已删除”的元素还留在容器里。逻辑也反直觉：你指定的是”要移除什么”而非”要保留什么”，谓词很容易写反。C++20 引入的 `std::erase_if` 简化了这个问题，range pipeline 则通过生成新视图而非原地修改，从根本上绕开了它。

配套项目 `examples/web-api/` 中有个例子。在 `repository.cppm` 中，`find_completed` 使用 `views::filter` 按完成状态过滤任务：

```cpp
// examples/web-api/src/modules/repository.cppm
[[nodiscard]] std::vector<Task> find_completed(bool completed) const {
    std::shared_lock lock{mutex_};
    auto view = tasks_
        | std::views::filter([completed](const Task& t) {
              return t.completed == completed;
          });
    return {view.begin(), view.end()};
}
```

Pipeline 本身就是业务逻辑，按谓词过滤，在跨越 API 边界之前物化为拥有型结果。不存在生命周期歧义：锁在整个过程中保持源数据存活，返回值是独立的 `vector`。

原则很简单：对序列做线性数据流处理时，用 range pipeline；如果重点是控制流、就地修改或操作步骤，就用循环。

## views（视图）是借用，惰性会让 bug 延迟暴露

生产环境中 range 最棘手的 bug 不在算法，而在生命周期。视图通常不持有数据所有权，惰性 pipeline 把实际工作推迟到迭代时才执行。视图的构造位置和 bug 的暴露位置可能相距甚远。

来看一个请求处理代码中常见的反模式：

```cpp
auto tenant_ids() {
    return load_tenants()
        | std::views::transform([](const Tenant& t) {
              return std::string_view{t.id};
          }); // BUG: returned view depends on destroyed temporary container
}
```

代码看着干净，但它是错的。`load_tenants()` 返回一个拥有数据的临时容器，视图 pipeline 借用了这个容器。函数返回时临时对象已经销毁，视图变成了延时炸弹。

视图的核心原则：数据拥有者必须比视图活得更久，这层关系必须让读者一眼看出来。如果生命周期关系需要仔细推敲才能理清，说明抽象已经过度了。

几种安全的做法：

- 在同一个局部作用域内构建并消费 pipeline，确保拥有者显然还活着。
- 跨越边界之前，先物化为拥有型结果。
- 当调用方无法合理地追踪源对象的生命周期时，直接返回拥有型 range 或领域对象。
- 当序列是逐步生成而非借用自已有存储时，使用 `std::generator` 或其他拥有型抽象。

借用本身没有问题，隐蔽的借用才是隐患。

## 不要随意把深层惰性 pipeline 暴露为公共 API

模块内部，range 往往是很好的胶水代码。到了子系统边界就得格外谨慎。公共 API 返回层层嵌套的视图栈，暴露给调用方的就不只是一个序列，而是一整套生命周期假设、迭代器类别行为、求值时机，甚至出人意料的失效规则。

对于只想拿到”过滤后的记录”的调用方，这些语义负担太重了。

库或大型服务的边界上，先想清楚调用方需要什么。需要拥有型结果就直接返回。需要对昂贵或分页数据做拉取式遍历就考虑用 generator。需要可定制的遍历并保持调用侧控制权就提供基于回调的 visitor 或专门设计的迭代器抽象。

返回惰性组合视图最合适的场景是在一个局部实现区域内，同一个团队拥有接口两侧，生命周期链条短到一眼就能看清。

配套项目中有两个保持在安全边界内的 range 设计。`json.cppm` 定义了一个函数模板，接受任何元素满足 `JsonSerializable` 的 `input_range`，将 range 约束与 concept 结合：

```cpp
// examples/web-api/src/modules/json.cppm
template <std::ranges::input_range R>
    requires JsonSerializable<std::ranges::range_value_t<R>>
[[nodiscard]] std::string serialize_array(R&& range) {
    std::string result = "[";
    bool first = true;
    for (const auto& item : range) {
        if (!first) result += ',';
        result += item.to_json();
        first = false;
    }
    result += ']';
    return result;
}
```

函数返回拥有型的 `std::string`，没有惰性视图逃逸出边界。Range 约束确保只有可序列化类型的集合才被接受，约束违反时错误信息指向 concept 名称而非循环体内部。

`middleware.cppm` 使用 `std::ranges::rbegin` 和 `std::ranges::rend` 反向迭代中间件集合，确保列表中第一个中间件包裹在最外层：

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

这是在局部算法内善用 range 工具的好例子：用 `rbegin`/`rend` 清晰表达了反向迭代意图而非依赖索引运算，函数产出的也是拥有型结果。

同样的谨慎适用于 pipeline 中的 `string_view` 和 `span`。转换步骤产生的借用切片，只要源对象的生命周期局部且明显就没有问题。但如果这些切片被跨线程传递、排入队列留待后续处理、或者被缓存起来就很危险了。

## `std::generator` 适用于拉取式数据源，不是用来替代每一个循环的

C++23 的 `std::generator` 有用，因为有些序列天生不适合”先全部存下来再遍历”。它们是增量产生的：分页数据库扫描、目录遍历、分块文件读取、带重试的轮询，以及随着字节到达而逐条产出完整消息的协议解码器。

这正是 generator 能改变设计的场景。它让生产者可以在产出元素之间保留状态，不需要把调用方推入回调反转的泥潭，也不需要手写迭代器类。

从远程 API 分页读取数据的批处理作业就是个典型例子。generator 出现之前，要表达增量式的分页获取序列，要么用回调反转要么手写迭代器类：

```cpp
// Pre-C++23: hand-written iterator for paged results
class PagedResultIterator {
public:
    using value_type = Row;
    using difference_type = std::ptrdiff_t;

    PagedResultIterator() = default; // sentinel
    explicit PagedResultIterator(Client& client)
        : client_(&client) { fetch_next_page(); }

    const Row& operator*() const { return rows_[index_]; }
    PagedResultIterator& operator++() {
        if (++index_ >= rows_.size()) {
            if (next_token_.empty()) { client_ = nullptr; return *this; }
            fetch_next_page();
        }
        return *this;
    }
    bool operator==(const PagedResultIterator& other) const {
        return client_ == other.client_;
    }

private:
    void fetch_next_page() {
        auto page = client_->fetch(next_token_);
        rows_ = std::move(page.rows);
        next_token_ = std::move(page.next_token);
        index_ = 0;
    }

    Client* client_ = nullptr;
    std::vector<Row> rows_;
    std::string next_token_;
    std::size_t index_ = 0;
};
```

大约四十行样板代码就为了表达”逐页获取，逐行产出”。换成 `std::generator`，同样的逻辑只需要：

```cpp
std::generator<Row> paged_rows(Client& client) {
    std::string token;
    do {
        auto page = client.fetch(token);
        for (auto& row : page.rows)
            co_yield std::move(row);
        token = std::move(page.next_token);
    } while (!token.empty());
}
```

Generator 版本把所有状态（page token、当前 buffer、当前位置）隐含在协程帧里，控制流一目了然。手写迭代器版本的 sentinel 比较、索引记账、边界处的页面抓取逻辑全靠手工维护，稍有不慎就引入微妙错误。

先把所有行全部物化再处理，既浪费内存又推迟了第一条有用数据的产出。Generator 可以逐行产出，同时把 page token、buffer 和重试状态封装在生产者内部。

话虽如此，generator 是协程机制，随之而来的是挂起点、帧生命周期管理，以及因实现和优化策略不同而变化的分配开销。并非零成本，调试也比”一个局部 vector 加一个循环”更麻烦。增量生产确实是问题的本质结构时才值得使用，不要因为时髦就拿它替代普通容器。

还有一个边界问题：generator 产出的是拥有型值还是指向内部 buffer 的借用引用？产出借用引用并非不可以，但前提是跨越挂起点的生命周期关系足够显式、易于推理。很多时候产出较小的拥有型值更安全。

如果目标工具链对 `std::generator` 的标准库支持尚不完整，上述设计指导同样适用于任何等价的协程驱动 generator 类型。关键在于结构设计，不是具体的厂商实现。

## 惰性虽好，但可观测性和错误处理可能更重要

惰性 pipeline 把工作推迟到真正需要时才执行，这通常有用。但副作用是：插桩、异常或 `expected` 的传播、失败归因，都可能比读者预期的要晚得多。

以日志处理路径为例：一个按需过滤、解析、充实和序列化的 pipeline 看上去很优雅，但从运维角度看它可能把故障扩散到最终消费者身上。解析失败时错误该归到哪一步？需要统计被丢弃的记录数时计数器该在哪里递增？链路追踪需要分阶段计时时，一个融合的惰性 pipeline 里"阶段"怎么界定？

这就是物化点存在的意义。把一条长 pipeline 拆成若干具名阶段，中间用拥有型结果衔接，能让系统更易观察、更好推理，哪怕代价是多一点内存。并非所有临时分配都是浪费，有些正是你挂载指标、隔离故障、让调试器停在正确位置的基础。

不要把惰性等同于高效。融合操作有时能减少工作量，但有时也会阻碍并行化、干扰分支预测，或者让最昂贵的环节更难被发现。对整条路径做 benchmark，不要想当然地认为 pipeline 形式一定更快。

## Range 和 generator 不再适用的场景

Range 不是好选择的场景：就地修改是核心逻辑、控制流不规则、提前退出伴随重要副作用，或算法已被外部 I/O 和锁操作主导。Generator 不是好选择的场景：普通容器返回结果既便宜又简单、序列需要反复遍历，或协程跨子系统的生命周期比局部 buffer 更难理清。

还有一种常见误区：把 pipeline 当成性能秀场。在不稳定的借用状态上串五个 adaptor，并不比一个用三个好名字变量写成的循环更高明。真正胜出的设计是所有权、成本和失败行为都能轻松说清楚的设计。

## 验证与评审

Range 和 generator 在代码评审时值得关注以下问题：

- 视图遍历的数据由谁拥有？在整个迭代过程中，拥有者是否确定还存活？
- 惰性工作实际在哪里执行？这个执行时机对错误处理和指标采集是否可接受？
- 是否有刻意设置的物化点，用于让所有权或可观测性变得显式？
- 换成普通循环，控制流和副作用是否会更清晰？
- Generator 产出的是拥有型值还是借用值？该生命周期跨越挂起点后是否仍然有效？

动态工具在这方面很有价值。AddressSanitizer 和 UndefinedBehaviorSanitizer 能在运行时暴露视图的生命周期错误。以吞吐量为由引入 pipeline 的场景需要做 benchmark。但代码评审仍然是最主要的防线，很多惰性相关的生命周期 bug 只要沿着所有权链条认真追踪一遍，在结构上就是显而易见的。

## 要点

- 当线性数据流是主线、副作用只是次要因素时，使用 range pipeline。
- 始终把视图当借用抽象看待，数据拥有者必须明确可见且保持存活。
- 生命周期契约不清晰时，不要把深层惰性 pipeline 暴露到宽泛的 API 边界之外。
- 用 generator 表达增量产生的序列，不要拿它当普通容器的时髦替代品。
- 可观测性、错误隔离或生命周期清晰度比极致惰性更重要时，果断插入物化点。
