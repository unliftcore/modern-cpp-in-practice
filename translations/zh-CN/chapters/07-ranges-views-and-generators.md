# 范围、视图与生成器

Range 和 generator 很有吸引力，因为它们把迭代压缩成一种看起来像数据流的形式。有时这正是生产代码库需要的东西。有时它们则是把生命周期 bug、隐藏工作量和几乎无法调试的惰性行为带进本来很普通代码的方式。

真正有用的问题不是 range pipeline 是否优雅，而是什么时候惰性组合会比普通循环更清楚地表达工作的结构，什么时候它又会遮蔽所有权、错误处理或成本。C++23 为你提供了强大的 range machinery，以及用于拉取式序列的 `std::generator`。它们都不该成为所有迭代的默认形状。

这里的示例领域都很现实：在导出前过滤日志、转换 batch 作业中的行，以及把分页源或协程驱动的源暴露成拉取式序列。每个场景的设计压力都一样。工作会随着时间到来。代码希望表达一连串转换。风险则是延迟执行、借用状态，以及“数据实际住在哪里”这一点变得混乱。

## 只有当数据流是主线时，pipeline 才值得出现

普通循环对很多任务来说依然是正确工具。它让顺序关系一目了然，让副作用显式，也容易单步调试。只有当工作的本质结构是“取一个序列，丢弃一些元素，转换保留下来的元素，然后物化或消费它们”时，range pipeline 才算有资格出现。

假设一个日志导出 worker 收到一批已解析记录，只需要把与安全相关的条目投影到导出 schema 后发出去。先看一下使用手写迭代器的 pre-ranges 写法：

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

这个版本能工作，但过滤逻辑和转换逻辑被揉在同一个循环体里。读者必须解析 `if` 才能理解保留了什么，又必须解析循环体才知道产出了什么。在更复杂的场景里，这类循环会逐渐积累嵌套条件、提前 `continue`、索引运算和手工记账，从而遮蔽数据流。基于索引的变体（`for (size_t i = 0; i < records.size(); ++i)`）尤其容易出现 off-by-one 错误，特别是在循环体会修改容器，或者索引用于多个目的时。

range 版本则在结构上把关注点分开了：

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

这就是好的 range 代码，因为 pipeline 本身就是业务逻辑。这里没有棘手的修改，没有源对象生命周期歧义，而且在结尾有一个清晰的物化点。中间存储在概念上本来就不存在，因此不分配它，既改进了清晰度，也改进了成本。

再把它和另一类循环比较一下：循环要更新共享计数器、发出 metric、原地修改记录，并有条件地重试下游写入。那种地方的 pipeline 往往会把最重要的部分藏起来：顺序关系和副作用。当计算是有状态且副作用很重时，range 语法并不会带来可读性收益。

另一个值得看看、在 pre-ranges 时代很常见的模式，是用于原地过滤的 “erase-remove” 惯用法：

```cpp
// Pre-C++20 erase-remove idiom
records.erase(
    std::remove_if(records.begin(), records.end(),
                   [](const LogRecord& r) {
                       return r.severity < Severity::warning || r.redacted;
                   }),
    records.end());
```

这段代码是正确的，但众所周知也很容易写错。忘掉传给 `erase` 的 `.end()` 参数，是一个会编译通过却把“已删除”元素留在容器里的经典 bug。逻辑本身也是反着的：你写的是“要移除什么”，而不是“要保留什么”，这也是谓词错误的常见来源。C++20 引入了 `std::erase_if` 来简化这个问题，而 range pipeline 则通过生成新视图而不是原地修改，完全绕开了它。

规则很直接：对一个序列做线性数据流处理时，用 range pipeline。若故事的主线是控制流、修改或运维步骤，就用循环。

## 视图是借用，而惰性会把 bug 推迟到更晚的时间点

生产环境中和 range 相关的最难 bug 类型，不是算法问题，而是生命周期问题。视图通常不拥有数据，而惰性 pipeline 会把工作推迟到真正迭代时才执行。这意味着视图的构造位置和 bug 变得可见的位置，可能相隔很远。

考虑一个会出现在请求处理代码里的反模式：

```cpp
auto tenant_ids() {
return load_tenants()
| std::views::transform([](const Tenant& t) {
  return std::string_view{t.id};
  }); // BUG: returned view depends on destroyed temporary container
}
```

这段代码看起来很整洁，但它是错的。`load_tenants()` 返回的是一个拥有型容器临时对象。这个视图 pipeline 借用自那个容器。返回这个视图，就是返回一个延迟触发的生命周期 bug。

这就是视图的核心设计规则：拥有者必须活得比视图更久，而且这一点必须对读者保持明显。如果生命周期关系很微妙，这个抽象就已经过于聪明了。

有几种安全模式。

- 在一个局部作用域内构建并消费 pipeline，在那里拥有者显然还活着。
- 跨边界之前，物化一个拥有型结果。
- 当调用方不可能合理跟踪源对象生命周期时，返回拥有型 range 或领域对象。
- 当序列是随时间生成，而不是借用自现有存储时，使用 `std::generator` 或其他拥有型抽象。

借用不是缺陷。隐藏的借用才是。

## 不要把深层惰性 pipeline 当作随意的 API 导出出去

在内部，range 往往是很好的胶水。在子系统边界上，则需要更谨慎。公共 API 返回一个很深的视图栈，导出的就不只是一个序列，而是一整包生命周期假设、迭代器类别行为、求值时机，以及有时会令人意外的失效规则。

对于一个可能只想要“过滤后记录”的调用方来说，这是一大块语义表面积。

在库边界或大型服务边界上，先问调用方真正需要什么。

- 如果他们需要一个拥有型结果，就返回它。
- 如果他们需要对昂贵或分页数据进行拉取式遍历，可以考虑 generator。
- 如果他们需要可定制的遍历并保持本地控制，就暴露基于回调的 visitor，或者专门的迭代器抽象。

返回惰性组合视图，最适合的是一个局部实现区域：在那里同一个团队拥有契约两侧，而且生命周期故事在视觉上很短。

同样的谨慎也适用于 pipeline 里的 `string_view` 和 `span`。如果转换步骤生成的是借用切片，只要源对象生命周期保持局部且明显，这就没有问题。如果这些切片会被偷渡过线程边界、排队等待后续工作，或者被缓存起来，那就很危险。

## `std::generator` 用于拉取式源，而不是替换每一个循环

C++23 的 `std::generator` 很有用，因为有些序列天然不是“先存起来再遍历”。它们是增量产生的：分页数据库扫描、目录遍历、分块文件读取、带重试的轮询，或者随着字节到达而产出完整消息的协议解码器。

这正是 generator 会改变设计的地方。它让生产者能在元素之间保留状态，而不必把调用方推入回调倒置，或者手写迭代器 machinery。

一个从远程 API 读取页面的 batch 作业就是很好的例子。在 generator 出现之前，要表达一个增量式的分页获取序列，要么使用回调倒置，要么手写一个迭代器类：

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

大约四十行样板代码，只是为了表达“抓取页面并产出各行”。而用 `std::generator` 写出来，同样逻辑是这样的：

```cpp
std::generator<const Row&> paged_rows(Client& client) {
    std::string token;
    do {
        auto page = client.fetch(token);
        for (const auto& row : page.rows)
            co_yield row;
        token = std::move(page.next_token);
    } while (!token.empty());
}
```

generator 版本把所有状态（page token、当前 buffer、当前位置）都隐含在协程帧里。控制流一目了然。手写迭代器版本则很容易出错：sentinel 比较、索引记账和边界处抓取页面的逻辑，全都得手工维护，而且很容易产生微妙错误。

在处理前先把所有行都物化出来，可能会浪费内存，也会推迟第一份有用工作的开始。generator 可以表达一串被依次产出的行，同时把 page token、buffer 和重试状态局部保留在生产者内部。

话虽如此，generator 仍然是协程 machinery。它们带来挂起点、帧生命周期，以及取决于实现和优化情况的分配成本。它们不是免费的。它们也比“一个局部 vector 加一个循环”更难调试。只有当“增量生产”才是问题的真实结构时，才使用它们，而不是把它们当作时髦的普通容器替代品。

这里还有另一个边界问题：generator 产出的是拥有型值，还是指向内部 buffer 的借用引用？产出借用引用并非一定错误，但前提是跨挂起点的生命周期足够显式，也足够容易推理。在很多情况下，产出较小的拥有型值会是更安全的权衡。

如果你的某个目标工具链对 `std::generator` 的标准库支持还不完整，同样的设计指导仍然适用于等价的协程驱动 generator 类型。问题的关键是结构，而不是某个特定厂商实现。

## 在可观测性和错误处理更重要之前，惰性一直有帮助

惰性 pipeline 很诱人，因为它们会把工作推迟到真正需要时才执行。这通常很有用。但这也意味着，instrumentation、异常或 `expected` 传播，以及失败归因，都可能比读者预想得更晚发生。

在日志处理路径中，一个按需过滤、解析、增强和序列化的 pipeline 可能看起来很优雅，但在运维层面，它可能把失败摊平到最终消费者身上。解析失败时，错误应当归到哪里？需要对被丢弃的记录计数时，计数增量应该发生在哪里？当 tracing 需要按阶段计时时，在一个融合的惰性 pipeline 里，究竟什么算一个阶段？

这就是物化点值得保留的地方。把一个长 pipeline 切成几个具名阶段，并在其间使用拥有型中间结果，往往会让系统更易观察、更易推理，即使代价是多用一点内存。不是所有临时分配都是浪费。有些临时分配恰恰是让你能够挂指标、隔离故障并把调试器停在正确位置的手段。

不要把惰性等同于高效。有时融合操作会减少工作量。有时它反而会阻碍并行化、让分支预测更复杂，或者只是让最昂贵的部分更难被看见。要 benchmark 整条路径，而不是想当然地认为 pipeline 形式更快。

## Range 和 generator 在什么地方不再是正确工具

当修改是核心、控制流不规则、提前退出伴随重要副作用，或者算法本身已经被外部 I/O 或锁主导时，range 并不适用。当一个普通容器结果更便宜也更简单、序列需要被反复遍历，或者跨子系统的协程生命周期比一个局部 buffer 更难推理时，generator 也不适用。

另一个常见失败模式，是把 pipeline 变成性能表演。对不稳定借用状态串上五个 adaptor，并不比一个带三个好名字变量的循环更高级。胜出的设计，是那个所有权、成本和失败行为依然容易解释的设计。

## 验证与评审

Range 和 generator 值得一组专门的评审问题。

- 遍历中的视图所依赖的数据由谁拥有？在整个迭代期间，这个拥有者是否显然还活着？
- 惰性工作真正在哪里执行？这种时机对错误处理和 metric 是否可接受？
- 是否存在一个刻意设置的物化点，用来让所有权或可观测性变得显式？
- 普通循环是否会更清楚地表达控制流和副作用？
- generator 产出的是拥有型值还是借用值？这个生命周期跨越挂起是否有效？

动态工具在这里很重要。AddressSanitizer 和 UndefinedBehaviorSanitizer 很擅长在路径被实际执行后暴露视图生命周期错误。对于那些为了吞吐量主张而采用 pipeline 的场景，benchmark 也很有帮助。但评审仍然承担了主要负担，因为很多惰性生命周期 bug 只要认真追踪所有权故事，在结构上就是显而易见的。

## 要点

- 当线性数据流才是主线，而副作用只是次要因素时，使用 range pipeline。
- 把每一个视图都当作借用抽象，其拥有者必须保持明显且存活。
- 除非生命周期契约真的足够清楚，否则不要把深层惰性 pipeline 导出到宽泛的 API 边界之外。
- 用 generator 表达增量产生的序列，而不是把它当作简单存储结果的替代品。
- 当可观测性、错误边界或生命周期清晰度比极致惰性更重要时，插入物化点。
