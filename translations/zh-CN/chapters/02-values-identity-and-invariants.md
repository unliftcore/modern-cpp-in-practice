# 值、身份与不变量

所有权理清之后，接下来的问题就从机械层面转向了语义层面：这个对象究竟应该是什么？

如果把每个类型都当成”附带方法的可变状态”来处理，现代 C++ 代码就会比它本该有的更难写。有些对象是值，代表一段自包含的含义，通常可以整体复制或替换，也应该便于比较和测试。有些对象则携带身份（identity），对应一个特定的会话、账户、worker 或连接，即使字段随时间变化，其连续性仍然重要。在一个含糊的类型里混用这两种角色，就会冒出一堆看似无关的 bug：错误的相等性判断、不稳定的缓存键、意外的别名、糟糕的并发行为，以及说不清”改完之后到底还是不是同一个东西”的 API。

本章讨论如何让这两种分类保持清晰，并通过不变量（invariant）的强制来保证类型在压力下仍然可信。参数传递和所有权转移机制留给后续章节。这里的重点是建模：什么时候把类型设计成值，什么时候必须显式保留身份，以及不变量如何防止对象图退化为一堆松散的字段。

## 值与实体解决的是不同问题

值由其内容定义，而非由来源定义。两个表示同一份配置、同一时间窗口或同一金额的值，通常可以互换使用。你可以随意复制、比较，也可以在线程之间传递，不用费心区分哪个才是”真正的”实例。

实体（即携带身份的对象）则不同。一个活跃的客户端会话，不能和恰好在某一时刻字段相同的另一个会话互换。一个连接对象可能会重连、累积统计信息、持有同步状态，但在系统看来它始终是同一个连接。身份之所以存在，就是为了让程序能表达"跨越时间的连续性"。

道理说出来都懂。但团队一旦没有明确决定某个类型属于哪一类，设计上的伤害就会悄然出现。

假设一个 `Order` 类型既可变、又共享、还按全部字段做相等比较，同时又被用作缓存键，程序实际上就在同时讲几个互相矛盾的故事。又假设一个配置快照被塞进了引用计数的可变对象里，而调用方只需要一组不可变的值，代码平白为别名和生命周期的复杂度买了单，却没有换来任何语义上的好处。

一条默认原则：如果一个类型不需要跨越时间的连续性，就优先设计成值。

## 值类型能减少意外耦合

值语义消除了隐式共享。调用方拿到的是自己的副本或 move 过来的实例，修改只影响自身，相等性通常可以按结构判定。写测试时可以直接构造小例子，不必搭建对象图或 mock 基础设施。

配置是个典型例子。很多系统因为产品中某处需要更新配置，就把配置建模成全局共享的可变对象。这个选择会殃及那些只需要稳定快照的代码。

更好的设计通常是：

- 把原始配置解析成一个经过验证的值对象。
- 配置变化时发布一个新的快照。
- 让消费者持有它们收到的那份快照。

这种设计让每个读取者面对的世界都是确定的。处理某个请求的代码只需针对手中那一份配置值做判断，没有更新到一半的对象图，不用为了读一个超时值就加锁，也不会搞不清两个调用方看到的是不是同一个可变实例。

### 没有值语义会出什么问题

当配置被建模成共享可变对象而非值快照时，别名 bug 就会随之而来：

```cpp
// Anti-pattern: shared mutable configuration.
struct AppConfig {
    std::string db_host;
    int db_port;
    std::chrono::seconds timeout;
};

// A single global mutable instance, shared by reference.
AppConfig g_config;

void handle_request(RequestContext& ctx) {
    auto conn = connect(g_config.db_host, g_config.db_port);
    // ... long operation ...
    // BUG: another thread calls reload_config(), mutating g_config
    // mid-request. conn was opened with the old host, but now
    // ctx uses the new timeout. The request operates against
    // an incoherent mix of old and new configuration.
    conn.set_timeout(g_config.timeout);
}
```

采用值语义后，每个请求都持有自己的不可变快照。读取字段无需加锁，中途也不可能出现不一致的状态：

```cpp
void handle_request(RequestContext& ctx, const ServiceConfig& config) {
    // config is a value -- it cannot change during this call.
    auto conn = connect(config.db_host(), config.db_port());
    conn.set_timeout(config.timeout());
    // Entire request sees a single consistent configuration.
}
```

值天然易于组合：可以放进容器、跨线程传递、用于确定性测试，也能充当稳定的哈希或比较输入。携带身份的对象也能做到这些，但需要更多规则和更多谨慎。只在模型真正需要的时候，才值得承担这份复杂性。

## 不变量是拥有类型的根本理由

一个允许无效状态组合的类型，说到底就是披着 struct 外衣的 bug 载体。

不变量是对象在任何可被外部观察到的时刻都应成立的条件。时间窗口要求 `start <= end`；金额要求携带货币并使用有界整数表示；批处理策略要求 `max_items > 0` 且 `flush_interval > 0ms`；连接状态对象禁止”已认证但未连接”。

不变量的意义在于大幅缩减后续代码需要防御的无效状态空间。

考虑一个调度子系统。

```cpp
class RetryPolicy {
public:
    static auto create(std::chrono::milliseconds base_delay,
                       std::chrono::milliseconds max_delay,
                       std::uint32_t max_attempts)
        -> std::expected<RetryPolicy, ConfigError>;

    auto base_delay() const noexcept -> std::chrono::milliseconds {
        return base_delay_;
    }

    auto max_delay() const noexcept -> std::chrono::milliseconds {
        return max_delay_;
    }

    auto max_attempts() const noexcept -> std::uint32_t {
        return max_attempts_;
    }

private:
    RetryPolicy(std::chrono::milliseconds base_delay,
                std::chrono::milliseconds max_delay,
                std::uint32_t max_attempts) noexcept
        : base_delay_(base_delay),
          max_delay_(max_delay),
          max_attempts_(max_attempts) {}

    std::chrono::milliseconds base_delay_;
    std::chrono::milliseconds max_delay_;
    std::uint32_t max_attempts_;
};
```

错误传递的细节留到下一章，但建模的要点此处已经够清楚：`RetryPolicy` 不应该以荒谬的状态存在。一旦创建成功，使用它的代码就不必再检查延迟是否倒置、尝试次数是否为零。

如果类型自身不强制不变量，这份负担就会外溢到每一个调用方和每一次代码评审中。

### 不强制不变量时会发生什么

把上面那个通过工厂验证的 `RetryPolicy` 与下面这个把验证留给调用方的普通聚合体做个对比：

```cpp
// Anti-pattern: invariants left to the caller.
struct RetryPolicy {
    std::chrono::milliseconds base_delay;
    std::chrono::milliseconds max_delay;
    std::uint32_t max_attempts;
};

void schedule_retries(const RetryPolicy& policy) {
    // Caller forgot to validate. base_delay is negative, max_attempts is 0.
    // This loop does nothing, silently dropping work.
    for (std::uint32_t i = 0; i < policy.max_attempts; ++i) {
        auto delay = std::min(policy.base_delay * (1 << i), policy.max_delay);
        enqueue_after(delay); // never executes when max_attempts == 0
    }
}
```

这样一来，每个接收 `RetryPolicy` 的函数都得自行检查荒谬值，要么就寄希望于上游某层已经做过检查。实际情况往往是有的调用方查了，有的没查，行为随调用路径不同而不一致。前面的工厂做法从结构上杜绝了这类 bug：只要你手里有一个 `RetryPolicy`，它一定是有效的。

示例项目的领域模型采用了同样的模式。`Task::validate()` 是一个返回 `Result<Task>` 的静态工厂，在边界处拒绝空标题或超长标题：

```cpp
// examples/web-api/src/modules/task.cppm
[[nodiscard]] static Result<Task> validate(Task t) {
    if (t.title.empty()) {
        return make_error(ErrorCode::bad_request, "title must not be empty");
    }
    if (t.title.size() > 256) {
        return make_error(ErrorCode::bad_request, "title exceeds 256 characters");
    }
    return t;
}
```

每条存储 `Task` 的路径都必须先经过 `validate()`，包括更新操作——仓库在突变后会重新验证。不变量由类型自己拥有，而不是由各个调用方分头负责。

## 反模式：把实体语义偷偷塞进值类型

一个反复出现的问题：某个类型看上去像值，会被复制、会被比较，但内部藏着带身份的可变状态。

```cpp
// Anti-pattern: one type tries to be both a value and a live entity.
struct Job {
    std::string id;
    std::string owner;
    std::vector<Task> tasks;
    std::mutex mutex; // RISK: identity-bearing synchronization hidden inside data model
    bool cancelled = false;
};
```

这个对象没法当值用，复制一个互斥量和一个活跃的取消标志毫无意义。它也没法当严格的实体模型，因为整个可变表示都是 public 的。这种类型会把含糊性传染给代码库的其余部分。

更清晰的拆分通常是：

- 用 `JobSpec` 或 `JobSnapshot` 之类的值类型承载稳定的领域数据，
- 再用 `JobExecution` 之类携带身份的运行时对象去拥有同步、进度和取消状态。

这样拆分之后，哪些部分可以序列化、比较、缓存、安全地跨线程传递，哪些部分是系统中一个正在运行的过程，就一目了然了。

示例项目清晰地展示了这种分离。`Task` 是纯粹的值类型，可复制、可比较、可序列化；`TaskRepository` 则是携带身份的实体，拥有 `shared_mutex`、ID 生成器以及可变集合。值承载领域数据，实体管理生命周期和同步，两者互不越界。

## 相等性应当匹配含义

检验一个类型的语义角色是否清晰，最好的办法是看它的相等性是否不言自明。

对于大多数值类型，相等性应该是结构性的：两个 host、port、TLS 模式都相同的 endpoint 配置，就是同一个值；两个货币和最小单位都相同的金额，就是同一个值；两个起止点相同的时间范围，就是同一个值。

而对于携带身份的对象，结构性相等反而容易误导。两个 user id 和远端地址都相同的活跃会话，并不是同一个会话；两个指向同一 shard 的连接，如果各自处于不同的生命周期阶段、各有各的待处理工作，就不能互换。

如果团队说不清某个类型的相等性该怎么定义，十有八九是这个类型把值数据和带身份的运行时关注点搅在了一起。

示例项目在这一点上处理得很直接。`Task` 声明了默认三路比较，相等性完全基于结构：

```cpp
// examples/web-api/src/modules/task.cppm
[[nodiscard]] auto operator<=>(const Task&) const = default;
```

因为 `Task` 是值类型，结构性相等就是正确答案。而携带身份的 `TaskRepository` 根本没有相等运算符——比较两个仓库毫无意义。

影响是实实在在的。相等性会波及缓存键、去重逻辑、diff 生成、测试断言和变更检测。语义模糊的类型产生语义模糊的相等性，进而拖垮多个子系统。

### 浅拷贝与别名：一个具体陷阱

如果一个类型看起来像值，内部却通过指针或引用共享状态，那么拷贝得到的就不是独立值，而是别名：

```cpp
// Anti-pattern: shallow copy creates aliasing bugs.
struct Route {
    std::string name;
    std::shared_ptr<std::vector<Endpoint>> endpoints; // shared, not owned
};

void reconfigure(Route primary) {
    Route backup = primary; // looks like a copy, but endpoints are shared

    backup.name = "backup-" + primary.name;
    backup.endpoints->push_back(fallback_endpoint()); // BUG: mutates primary too

    // primary.endpoints and backup.endpoints point to the same vector.
    // The caller who passed primary now sees an endpoint they never added.
}
```

解决方法是让该类型拥有真正的值语义：把 vector 直接作为成员存储（拷贝即深拷贝），或者采用 copy-on-write 策略，又或者把类型设计成不可变的，使共享本身就是安全的：

```cpp
struct Route {
    std::string name;
    std::vector<Endpoint> endpoints; // owned, copied on assignment

    auto with_endpoint(Endpoint ep) const -> Route {
        Route copy = *this;
        copy.endpoints.push_back(std::move(ep));
        return copy;
    }
};
```

现在 `Route` 就是一个真正的值：拷贝彼此独立，`with_endpoint` 会生成新值而不影响原值，别名意外也就无从谈起了。

## 修改应当尊重建模选择

值和实体对修改的容忍方式不同。

对于值类型，最干净的做法通常是验证后即不可变，至少也应该限制只能通过保持不变量的窄操作来修改。整体替换配置快照或生成新路由表，往往比原地修改一个共享实例更容易理解和推理。

对于实体，修改是天经地义的，因为对象建模的就是跨时间的连续性。但这不等于 public 可写字段或不受约束的 setter 就合理了。实体同样需要受控的状态机。一个 `Connection` 可以从 `connecting` 转到 `ready`、再到 `draining`、再到 `closed`；不能仅仅因为各个字段单独看都合法，就允许任意组合。

真正的设计问题不是”能不能改”，而是”在哪儿能改”以及”改完之后还剩下哪些保证”。

如果两次字段赋值之间就可能破坏不变量，说明该类型需要更强的操作边界。如果调用方必须先加锁、更新三个字段、再记得重算一个派生标志，那不变量就从来不曾真正属于这个类型。

```cpp
// Anti-pattern: public fields allow invariant-breaking mutation.
struct TimeWindow {
    std::chrono::system_clock::time_point start;
    std::chrono::system_clock::time_point end;
};

void extend_deadline(TimeWindow& window, std::chrono::hours extra) {
    window.end += extra; // fine
}

void shift_start(TimeWindow& window, std::chrono::hours shift) {
    window.start += shift;
    // BUG: if shift is large enough, start > end.
    // Every consumer of TimeWindow must now defend against this.
}
```

封装良好的类型从根本上杜绝了这类 bug——外部根本无法打破不变量：

```cpp
class TimeWindow {
public:
    static auto create(system_clock::time_point start,
                       system_clock::time_point end)
        -> std::optional<TimeWindow>
    {
        if (start > end) return std::nullopt;
        return TimeWindow{start, end};
    }

    auto start() const noexcept { return start_; }
    auto end() const noexcept { return end_; }

    auto with_extended_end(std::chrono::hours extra) const -> TimeWindow {
        return TimeWindow{start_, end_ + extra}; // always valid: end moves forward
    }

private:
    TimeWindow(system_clock::time_point s, system_clock::time_point e)
        : start_(s), end_(e) {}

    system_clock::time_point start_;
    system_clock::time_point end_;
};
```

调用方不可能构造出无效的 `TimeWindow`。`start <= end` 这条不变量只在类型内部集中强制一次，无需在每个修改点重复检查。

## 小型领域类型值得这点形式成本

有经验的程序员有时会排斥小型包装类型，觉得跟普通整数或字符串比起来像是多余的仪式。但在生产级 C++ 中，这些类型很快就能收回成本。

`AccountId`、`ShardId`、`TenantName`、`BytesPerSecond`、`Deadline` 这样的类型，能杜绝参数传反、让日志更清晰，也让无效组合更难写出来。不变量和转换逻辑可以集中在类型内部，而不是散落在解析、存储和格式化代码各处。

但也要警惕：包装类型只有在真正使含义更精确时才有价值。如果只是在 `std::string` 外面套了个壳，所有无效状态照样存在，也没增加任何语义操作，那就只是噪音。该问的是：这个类型是否在强制或传达系统真正关心的某种区分？

## 当值保持为值时，并发会更容易

很多并发问题的根源是建模问题。共享可变状态之所以难对付，很大程度上是因为程序在本可使用不可变值的地方，用了携带身份的对象。

在流水线中传递一份经过验证的快照，理解起来很容易；而在同一条流水线上共享一个带内部锁的可变配置服务对象，就难得多了。把面向值的请求描述符投入工作队列，也远比传入一个带隐藏别名和同步机制的活跃会话对象简单。

不是说每个并发系统都能完全消除实体，但值语义是减少需要共享和同步的状态量的有效手段。一旦代码能用快照发布或值的消息传递来取代就地修改，正确性和可审查性都会提升。

## 验证与评审

声称具有特定语义角色的类型，评审时就应该直接拿这些角色来检验。

有用的评审问题：

1. 这个类型主要是值，还是主要是携带身份的对象？
2. 它的相等性、复制规则和修改规则是否匹配这种选择？
3. 哪些不变量是由类型自己强制的？
4. 把稳定的领域数据和活跃的运行时状态拆开，是否会让设计更简单？
5. 共享可变状态之所以存在，是因为模型确实需要身份，还是因为从未尝试过值语义？

测试遵循同样的思路。值类型适合用性质测试来验证不变量是否始终成立、相等性是否正确、以及序列化是否稳定。携带身份的类型则更适合做生命周期和状态机测试——验证合法转换能走通，非法转换会被拒绝。

## 要点

- 如果跨越时间的连续性不属于领域含义，就默认采用值语义。
- 当对象代表的是一个特定的、活着的东西而非可互换的数据时，身份必须显式化。
- 在类型内部强制不变量，免得调用方反复被动地重新发现它们。
- 相等性、复制和修改规则应当与类型的语义角色保持一致。
- 当一个对象试图身兼两职时，把稳定的领域值和运行时控制状态拆开。

当一个类型能清楚地回答”我到底是什么”时，其余设计都会变得顺畅：所有权更明确，API 更精练，测试更简单，并发也不用再和隐藏别名纠缠。
