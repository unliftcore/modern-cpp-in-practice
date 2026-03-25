# 值、身份与不变量

一旦所有权清晰，下一个生产问题就不再是机械性的，而是语义性的：这个对象究竟应该是什么样的东西？

当每个类型都被当作“附带方法的可变状态”时，现代 C++ 代码会比它本该有的更难。有些对象是值。它们表示一段自包含的含义，通常可以整体复制或替换，也应该易于比较和测试。有些对象携带身份。它们表示一个特定的会话、账户、worker 或连接，即使其字段随时间变化，它的连续性仍然重要。把这些角色混在一个模糊的类型里，会制造出看似彼此无关的缺陷：错误的相等性、不稳定的缓存键、意外别名、不良的并发行为，以及无法说明“修改之后是否还是同一个东西”的 API。

本章讨论的是如何让这些类别保持清晰，并强制不变量，使类型在压力下仍然可信。这不是一章关于参数传递或所有权转移机制的内容；那些属于别处。这里关注的是建模：决定某个类型何时应表现为值，何时必须显式保留身份，以及不变量如何阻止对象图退化成一袋字段。

## 值与实体解决的是不同问题

值由它所包含的内容定义，而不是它来自哪里。两个表示同一份配置、同一时间窗口或同一金额的值，通常应该可以互换。你可以复制它们、比较它们，并把它们在线程之间移动，而不必编造哪个才是“真正的”实例。

实体或携带身份的对象则不同。一个活跃的客户端会话，并不能和另一个恰好在某一时刻具有相同字段的会话互换。一个连接对象可能会重连、累积统计信息并持有同步状态，但从系统视角看，它仍然是同一个连接。身份的存在，是为了让程序能够谈论跨时间的连续性。

这听起来很显然。设计损害出现在团队没有决定某个类型属于哪一类的时候。

如果一个 `Order` 类型是可变的、共享的、按所有字段比较相等，而且还被拿来当缓存键，那么程序现在就同时拥有了几套彼此不兼容的叙事。如果一个配置快照被包进了一个带引用计数的可变对象，而调用方实际上只需要一组不可变值，那么代码就在没有获得语义收益的前提下，为别名和生命周期复杂度付了成本。

一个有用的默认值，比许多代码库意识到的更强：如果一个类型不需要跨时间的连续性，就优先把它设计成值。

## 值类型能减少意外耦合

值语义之所以强大，是因为它减少了不可见的共享。调用方拿到的是自己的副本或 move 过来的实例。修改是局部的。相等性往往可以是结构性的。测试可以构造小型样例，而不必分配对象图或 mock 基础设施。

配置就是一个很好的例子。很多系统把配置建模成全局共享的可变对象，因为产品中的某处存在更新。这种选择会污染那些只需要稳定快照的代码。

通常，更好的设计是这样的：

- 把原始配置解析成一个经过验证的值对象。
- 配置变化时发布一个新的快照。
- 让消费者持有它们收到的那份快照。

这种设计让每个读取者所面对的世界都是显式的。处理单个请求的代码可以针对一个配置值进行推理。不会存在更新到一半的对象图，不需要仅仅为了读取一个超时值就加锁，也不会搞不清两个调用方是不是在看同一个可变实例。

### 没有值语义会出什么问题

当配置被建模成共享可变对象而不是值快照时，别名缺陷就会出现：

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

采用值语义时，每个请求都会捕获自己的不可变快照。读取字段不需要锁，中途修改也无法制造不一致状态：

```cpp
void handle_request(RequestContext& ctx, const ServiceConfig& config) {
// config is a value -- it cannot change during this call.
auto conn = connect(config.db_host(), config.db_port());
conn.set_timeout(config.timeout());
// Entire request sees a single consistent configuration.
}
```

更深一层的要点是，值很容易组合。它们可以放进容器、跨线程传递、参与确定性的测试，并成为稳定的哈希或比较输入。携带身份的对象也能做这些事，但它们需要更多规则和更多谨慎。只有当模型真正需要时，才去承担这种复杂性。

## 不变量是拥有类型的根本理由

一个允许无效状态组合的类型，往往只是在用 struct 的外形承载 bug。

不变量是指：每当对象能被程序其余部分观察到时，本应成立的条件。一个时间窗口可能要求 `start <= end`。一个金额可能要求包含货币，并使用有界整数表示。一个批处理策略可能要求 `max_items > 0` 且 `flush_interval > 0ms`。一个连接状态对象可能禁止“已认证但未连接”。

不变量的意义，不是让构造函数看起来更花哨，而是减少后续代码必须防御的无效状态数量。

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

关于错误传递的细节会在下一章更完整地讨论，但这里的建模要点已经很清楚了：`RetryPolicy` 不应该以荒谬状态存在。一旦创建成功，使用它的代码就不应再询问延迟是否倒置，或者尝试次数是否为零——除非这些正是领域真正想表达的有效含义。

如果一个类型不强制自己的不变量，这个负担就会向外转移到每一个调用方和每一次代码评审。

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

现在，任何接收 `RetryPolicy` 的函数都必须独立检查那些荒谬值，或者假定更早的一层已经做过。在实践中，一些调用方会检查，一些不会，于是行为会随着调用路径不同而不一致。前面展示的工厂式做法从结构上让这类 bug 不可能出现：如果你手里有一个 `RetryPolicy`，它就是有效的。

## 反模式：把实体语义偷偷塞进值类型

一种反复出现的失败模式是：某个类型看起来像值，因为它会被复制和比较，但实际上却携带了带身份的可变状态。

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

这个对象无法诚实地表现为值，因为复制一个互斥量和一个活跃的取消标志没有合理含义。它也无法诚实地表现为一个狭义的实体模型，因为整个可变表示都是 public 的。这个类型会把模糊性扩散到代码库其余部分。

更清晰的拆分通常是：

- 用 `JobSpec` 或 `JobSnapshot` 之类的值类型承载稳定的领域数据，
- 再用 `JobExecution` 之类携带身份的运行时对象去拥有同步、进度和取消状态。

这种拆分能明确哪些部分可以序列化、比较、缓存，并安全地跨线程移动，哪些部分则是在系统中建模一个活的过程。

## 相等性应当匹配含义

判断一个类型是否具有连贯语义角色的最好测试之一，就是它的相等性是否显而易见。

对于很多值类型，相等性应该是结构性的。两个经过验证、且 host、port 和 TLS 模式相同的 endpoint 配置，就是同一个值。两个货币和最小单位都相同的金额，就是同一个值。两个端点相同的时间范围，就是同一个值。

对于携带身份的对象，结构性相等往往会积极地误导人。两个具有相同 user id 和远端地址的活跃会话，并不是同一个会话。两个指向同一 shard 的连接，如果各自带有不同的生命周期状态和待处理工作，它们就不可互换。

如果一个团队无法回答某个类型的相等性应该意味着什么，这个类型很可能把值数据和带身份的运行时关注点混在了一起。

这在运维层面也很重要。相等性会影响缓存键、去重逻辑、diff 生成、测试断言和变化检测。一个语义模糊的类型，往往会产生语义模糊的相等性，从而同时破坏多个系统。

### 浅拷贝与别名：一个具体陷阱

当一个类型看起来像值，却通过指针或引用共享内部状态时，拷贝就会变成别名，而不是独立值：

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

修复方式是赋予该类型真正的值语义。要么把 vector 直接作为成员存储（这样拷贝就是深拷贝），要么使用 copy-on-write 策略，要么把类型做成不可变的，从而让共享是安全的：

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

现在 `Route` 的行为像一个值。拷贝彼此独立。通过 `with_endpoint` 的修改会产生一个新值，而不会打扰原值。不会再有别名带来的意外。

## 修改应当尊重建模选择

值和实体对修改的容忍方式不同。

对于值类型，最干净的设计通常是在验证后保持不可变，或者至少只通过能保持不变量的狭窄操作来修改。替换一份配置快照，或者生成一张新的路由表，往往比原地修改一个共享实例更容易推理。

对于实体，修改是自然的，因为对象建模的是跨时间的连续性。但这并不能为 public 可写字段或不受约束的 setter 开脱。实体仍然需要受控的状态机。一个 `Connection` 可以从 `connecting` 转换到 `ready`、再到 `draining`、再到 `closed`；它不应该仅仅因为各个字段单独看起来都合法，就允许任意组合。

真正的设计问题，不是“是否允许修改”，而是“允许在何处修改，以及修改后还能保留什么保证”。

如果修改可能在两次字段赋值之间破坏不变量，那么这个类型可能需要更强的操作边界。如果调用方必须先加锁、更新三个字段，再记得重算一个派生标志，那么不变量其实从来就不属于这个类型。

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

一个封装良好的类型会通过让外部无法打破不变量，消除这类 bug：

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

调用方无法制造一个无效的 `TimeWindow`。不变量 `start <= end` 被集中在类型内部强制，而不是散落在每个修改点上。

## 小型领域类型值得这点形式成本

有经验的程序员有时会抗拒小型包装类型，因为与普通整数或字符串相比，它们看起来像额外的形式成本。在生产级 C++ 中，这些类型往往很快就能回本。

`AccountId`、`ShardId`、`TenantName`、`BytesPerSecond` 或 `Deadline` 这样的类型，可以消除参数颠倒、澄清日志，并让无效组合更难表达。更重要的是，这些类型可以在本地承载不变量和转换，而不是把它们分散到解析、存储和格式化代码里。

需要警惕的是，包装类型只有在它真正让含义更锋利时才有价值。一个只是包着 `std::string`、却保留了所有无效状态、也没有添加任何语义操作的薄壳，基本只是噪音。正确的问题是：这个类型是否在强制或传达系统真正关心的某种区分。

## 当值保持为值时，并发会更容易

很多并发问题，秘密里其实是建模问题。共享可变状态之所以难，很大程度上是因为程序在本可使用不可变值的地方，使用了携带身份的对象。

把一个经过验证的快照在线程或流水线中传递，很容易推理。在同一条流水线上共享一个带内部锁的可变配置服务对象，就困难得多。把面向值的请求描述符传进工作队列，要比传递一个带隐藏别名和同步的活跃会话对象容易。

这并不意味着每个并发系统都能消灭实体。它意味着，值语义是减少必须共享和同步状态量的最有效方式之一。当代码可以用快照发布或值消息传递来替换修改时，正确性和可评审性都会提升。

## 验证与评审

那些宣称自己有特定语义角色的类型，应该按照这些角色直接接受评审。

有用的评审问题：

1. 这个类型主要是值，还是主要是携带身份的对象？
2. 它的相等性、复制规则和修改规则是否匹配这种选择？
3. 哪些不变量是由类型自己强制的？
4. 把稳定的领域数据和活跃的运行时状态拆开，是否会让设计更简单？
5. 共享可变状态之所以存在，是因为模型确实需要身份，还是因为从未尝试过值语义？

测试也遵循同样的逻辑。值类型值得拥有验证不变量保持、相等性以及在相关场景下序列化稳定性的性质类测试。携带身份的类型则值得拥有生命周期和状态机测试，用来验证合法状态转换，并拒绝非法转换。

## 要点

- 如果跨时间的连续性不是领域含义的一部分，就默认使用值语义。
- 当对象表示的是一个特定的活物，而不是可互换的数据时，要把身份显式化。
- 在类型内部强制不变量，这样调用方就不必一次次被动重新发现它们。
- 让相等性、复制和修改规则跟随类型的语义角色。
- 当一个对象试图同时承担两项工作时，把稳定的领域值与运行时控制状态拆开。

当一个类型对“这究竟是什么样的东西”有清楚答案时，其余设计都会变得更容易：所有权更明显，API 更狭窄，测试更简单，并发也不再和隐藏别名对抗。这就是为什么语义清晰应当在本书这么靠前的位置出现。
