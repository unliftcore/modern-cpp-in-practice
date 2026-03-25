# 接口设计与依赖方向

本章假定你已经读过函数签名设计、所有权、不变量和失败边界。这里的问题不是如何写一个函数，而是如何塑造一个能经受团队增长和系统压力的边界。

## 生产问题

大多数接口损伤，并不是来自显而易见的糟糕代码。它来自那些在局部看来合理、却逐渐固化成系统级耦合的选择。存储层返回数据库行类型，因为那正是它手头已经有的东西。服务边界接收一个巨大的配置对象，因为未来的选项可能会重要。库到处都把回调写成 `std::function`，因为它看起来很灵活。六个月之后，测试需要半个依赖图，调用点泄漏传输层关注点，而改变一个实现细节也成了破坏性变更。

本章讨论的是源码层面的接口设计：边界暴露什么、依赖应当朝哪个方向指，以及如何防止策略、所有权和表示形式跨层泄漏。它不是关于运行时分派机制的章节；那属于下一章。它也不是关于二进制兼容性或分发的章节；那属于再下一章。这里的焦点更窄，也更重要：决定程序的一部分被允许了解另一部分的哪些事实。

核心规则很简单：依赖应当指向稳定的策略和领域含义，而不是指向易变的实现细节。在实践中，这意味着接口应当建立在调用方已经理解的概念之上，而不是建立在被调用方的存储、传输、框架或日志选择之上。

## 为什么这会变得昂贵

错误的依赖方向会以代码评审常常看不到的方式放大成本。

如果领域逻辑直接依赖 SQL 行类型、protobuf 生成类、HTTP 请求包装类型，或者文件系统遍历状态，那么每个测试、benchmark 和重构都必须把这些细节一起拖进来。依赖图会比设计实际要求的更宽。构建时间会上升，因为传递包含和模板会把实现细节扩散到所有地方。评审质量会下降，因为边界违规会逐渐常态化。最重要的是，本应局部的设计选择将不再局部。

成本不只体现在编译时间上。它还体现在概念稳定性上。一个好的接口可以经受数据库变更、队列替换或日志系统重写。一个坏的接口则会要求代码库的其他部分重新学习那些本就与它们无关的内部事实。

## 从边界问题开始

在写接口之前，先强迫自己把生产问题压缩成一句话。

在原生服务里，问题通常不是“我如何暴露 repository？”，而是“订单工作流如何询问客户的信用状态？”在共享库里，问题不是“我如何公开解析器内部实现？”，而是“调用方需要什么契约，才能校验并转换输入记录？”

这种转变很重要，因为它会改变类型的形状。围绕实现名词设计的接口，往往会泄漏机制。围绕工作和不变量设计的接口，则往往能保持狭窄。

一个接口应当清楚回答四个问题：

1. 调用方需要什么能力？
2. 数据和生命周期由哪一侧拥有？
3. 失败在什么地方被转换成第 3 章的错误模型？
4. 哪些策略在这里是固定的，哪些仍然由调用方选择？

如果这些答案都很模糊，这个接口大概率就是在混层。

## 依赖方向意味着策略方向

依赖倒置常被机械地解释为：依赖抽象，而不是具体实现。这是对的，但还不够。真正有用的测试，是看依赖箭头是否追随稳定策略。

在一个服务中，业务规则的变化速度往往比传输胶水更慢。欺诈策略不应依赖 HTTP handler。订单校验不应依赖 SQL 记录包装类型。领域逻辑可以定义自己需要的 port，而数据库或网络 adapter 则去实现这个 port。

这并不意味着每个边界都需要一个抽象基类。很多时候并不需要。有时正确边界是一个接收领域数据的自由函数。有时它是内部库里一个受概念约束的模板。有时它是一个值类型请求/结果对象，完全没有任何虚派发。设计决策不是“我把接口类型放在哪里？”，而是“哪一侧有资格给契约命名？”

通常应该由拥有更稳定词汇的一侧来命名。

## 反模式：接口由依赖项来定义

当契约由实现细节来命名时，依赖箭头就已经错了。

```cpp
// Anti-pattern: domain code now depends on storage representation.
struct AccountRow {
std::string id;
std::int64_t cents_available;
bool is_frozen;
std::string fraud_flag;
};

class AccountsTable {
public:
virtual std::expected<AccountRow, DbError>
fetch_by_id(std::string_view id) = 0;
virtual ~AccountsTable() = default;
};

std::expected<PaymentDecision, PaymentError>
authorize_payment(AccountsTable& table, const PaymentRequest& request);
```

这看起来可测试，因为它用了抽象基类。但接缝仍然是错的。支付工作流不应该知道可用信用额度被表示为分，并且和一个从表行里加载出来的欺诈标记字符串并列存储。这个抽象保留了依赖，却没有改善依赖方向。

更好的 port 应由工作流命名，并且只返回工作流所需的最小稳定事实。

```cpp
struct CreditState {
Money available;
bool frozen;
RiskLevel risk;
};

class CreditPolicyPort {
public:
virtual std::expected<CreditState, PaymentError>
load_credit_state(AccountId account) = 0;
virtual ~CreditPolicyPort() = default;
};

std::expected<PaymentDecision, PaymentError>
authorize_payment(CreditPolicyPort& credit, const PaymentRequest& request);
```

现在，工作流依赖的是领域含义，而不是存储形状。和 SQL 对话的 adapter 去做转换。这种转换确实是工作量，但它是正确的工作：把易变性收束在易变事物附近。

## 反模式：会把周围一切都吸进来的胖接口

臃肿的接口不只是违反美观。它会制造耦合引力：每个新特性都会被拧到现有表面上，因为加一个方法比重新思考边界更容易。

```cpp
// Anti-pattern: a "god interface" that mixes query, mutation, lifecycle,
// metrics, and configuration concerns in one surface.
class UserService {
public:
virtual std::expected<UserProfile, ServiceError>
get_profile(UserId id) = 0;

virtual void update_profile(UserId id, const ProfilePatch& patch) = 0;

virtual void ban_user(UserId id, std::string_view reason) = 0;

virtual std::vector<AuditEntry>
get_audit_log(UserId id, TimeRange range) = 0;

virtual void flush_cache() = 0;

virtual MetricsSnapshot get_metrics() const = 0;

virtual void set_rate_limit(RateLimitConfig config) = 0;

virtual ~UserService() = default;
};
```

这个接口至少混合了四条彼此无关的变化轴：用户数据访问、审核策略、运维可观测性和运行时配置。一个只需要读取 profile 的调用方，现在却会传递性地依赖审计、缓存、metric 和限流相关类型。为了伪造一个行为，测试替身必须实现七个方法。增加一个新的审核动作，会迫使只读消费者重新编译。这个接口不是灵活，而是一个依赖汇点，让每次变更都昂贵，让每个测试都脆弱。

修复方式是按职责边界拆开：

```cpp
class UserProfileQuery {
public:
virtual std::expected<UserProfile, ServiceError>
get_profile(UserId id) = 0;
virtual ~UserProfileQuery() = default;
};

class ModerationActions {
public:
virtual void ban_user(UserId id, std::string_view reason) = 0;
virtual std::vector<AuditEntry>
get_audit_log(UserId id, TimeRange range) = 0;
virtual ~ModerationActions() = default;
};
```

现在，只读消费者只依赖 `UserProfileQuery`，审核工具只依赖 `ModerationActions`，运维关注点则住在另一个接口里。它们都可以独立演化。测试替身也会变得简单。

## 反模式：通过接口泄漏实现细节

即使接口很小，只要暴露了错误的类型，它也可能损伤整个系统。

```cpp
// Anti-pattern: interface leaks the JSON library into every consumer.
#include <nlohmann/json.hpp>

class ConfigProvider {
public:
virtual nlohmann::json get_config(std::string_view key) = 0;
virtual ~ConfigProvider() = default;
};
```

现在，每个包含这个头文件的翻译单元都会依赖 JSON 库，不管它是否关心 JSON。改成 TOML、YAML 或二进制配置格式，都会变成影响整个代码库的破坏性变更。JSON 库的编译时间、宏定义和传递包含，会扩散到无关组件中。更糟的是，调用方现在在 JSON 树上摸索，而不是使用有类型的配置值，于是隐式 schema 知识会被散布到整个代码库。

修复方式是返回具有领域含义的类型：

```cpp
struct RetryConfig {
std::chrono::milliseconds initial_backoff;
std::chrono::milliseconds max_backoff;
std::uint32_t max_attempts;
};

class RetryConfigProvider {
public:
virtual std::expected<RetryConfig, ConfigError>
load_retry_config() = 0;
virtual ~RetryConfigProvider() = default;
};
```

现在 JSON 依赖留在 adapter 实现内部。消费者处理的是有类型、已校验的值。接口传达的是领域含义，而不是序列化格式。

## 反模式：抽象层级错误

抽象层级不对的接口，要么迫使调用方做本应封装起来的工作，要么阻止它们做自己需要的工作。

```cpp
// Anti-pattern: too low-level. Caller must assemble SQL semantics
// even though this is supposed to abstract away storage.
class DataStore {
public:
virtual std::expected<RowSet, DbError>
execute_query(std::string_view sql) = 0;

virtual std::expected<std::size_t, DbError>
execute_update(std::string_view sql) = 0;

virtual ~DataStore() = default;
};
```

这个接口声称自己抽象了存储，但它把 SQL 当作字符串协议直接暴露出来。调用方必须了解 schema、构造正确的 SQL，并解析 `RowSet` 结果。这个抽象既没有防止 SQL 注入，也没有防止 schema 耦合。它只是一个直通层：增加了间接性，却没有减少依赖。

反过来，一个接口也可能过于高层，以至于妨碍合法使用：

```cpp
// Anti-pattern: too high-level. No way to paginate, filter,
// or control what gets loaded.
class OrderRepository {
public:
virtual std::vector<Order> get_all_orders() = 0;
virtual ~OrderRepository() = default;
};
```

正确的抽象层级，应当匹配调用方实际执行的操作，使用领域词汇，并提供足够的控制能力来保持高效。

## 通过分离命令与查询来保持接口小巧

臃肿接口通常源自把彼此无关的变更原因混在一起。一个既能读取状态、又能修改状态、还能发审计事件、开启事务并暴露 metric 快照的边界，并不灵活。它是一个依赖汇点。

把命令和查询分开，往往就足以恢复清晰性。查询路径通常想要以值为中心的请求和结果类型、可预测的成本，以及没有隐藏修改。命令路径通常想要显式所有权转移、清楚的副作用和强失败语义。把它们当成同一个接口，会鼓励偶然耦合，因为调用方会开始依赖上个季度顺手放进去的任何东西。

更小的接口也会改进评审质量。评审者可以问：这里的每个函数到底是否属于同一个边界？一旦接口变成“附近操作的大杂烩”，这个问题就不再容易回答。

## 数据形状：接受稳定视图，返回拥有型含义

第 4 章讨论的是局部签名选择。在接口边界上，同样的规则会升级成架构规则。

当被调用方不需要保留数据时，输入通常应接受非拥有视图：`std::string_view`、`std::span<const std::byte>`、领域对象 span，或者引用调用方拥有数据的轻量请求结构。这会让调用点保持廉价且诚实。

输出通常应返回拥有型值，或者生命周期清楚的领域对象。返回指向 adapter 自有存储的视图、指向 cache line 的借用指针，或者指向内部状态的迭代器，会把边界变成生命周期谜题。这很少值得。

这种不对称是刻意的。当成本重要而保留无关紧要时，从调用方那里借用。跨边界返回时，则返回所有权，因为被调用方控制自己的内部实现，不应该强迫调用方关心这些内部实现能活多久。

当然也有例外。热路径解析器、零拷贝数据流水线和内存映射处理阶段，可能会有意返回视图。即便如此，生命周期边界也必须是接口契约的一部分，而不是部落知识。一个和特定 buffer 拥有者绑定的 `ParsedFrameView` 类型，远比泄漏裸 `std::string_view` 或原始指针，然后指望评审者自己注意到这种耦合，要安全得多。

## 不要通过可选参数偷运策略

让接口变得不清晰的最快方式之一，就是用配置对象或默认参数，把策略决策塞进调用方无法推理的地方。

如果一个函数带着 `skip_cache`、`best_effort`、`emit_audit`、`allow_stale` 和 `retry_count` 之类的标志，它大概率就是在做太多工作。问题不在于美观，而在于调用方现在可以形成一些语义不清、未被测试，或者在运维上很危险的组合。

优先考虑下面三种替代方案之一：

1. 把能力拆成几个名字更清楚的独立操作。
2. 把策略提升为显式类型，让无效状态变得不可能或显而易见。
3. 把策略选择上移一层，使较低层接口保持确定性。

当策略是被显式命名的，而不是藏在参数乱炖里时，接口会更容易演化。

## 可测试性是结果，不是目标

团队经常用“它更利于测试”来为接口辩护。这是本末倒置。首要问题是边界是否反映了真实设计。如果反映了，测试通常会随之变得更容易。如果没有，测试替身只是在保留这个错误。

例如，仅仅为了让单元测试伪造数据库访问，而引入一个 repository 接口，这种理由是薄弱的；尤其当领域层仍然依赖表形数据和传输形错误时。测试也许确实更容易写了，但设计依然是错的。

好的边界会产出更好的测试，因为它把策略和机制隔离开了。你可以用简单 fake 来测试业务逻辑，因为业务逻辑要求的是领域事实，而不是框架对象。你可以单独对 adapter 做集成测试，因为转换被收束在一个地方。这比“现在我们能 mock 它了”强得多。

## 在内部使用概念和模板，而不要把它们当成公共逃生口

现代 C++ 让你很容易把接口编码成约束，而不是虚类。在组件内部，或者在一个严格受控的代码库里，这往往是正确选择。受概念约束的模板可以让代码免分配、可内联，而且往往比深层继承层级更有表达力。

但是，一个试图通过模板包打一切的公共接口，往往最终根本不再像接口。它会同时变成策略表面、编译期集成机制和文档负担。错误信息会变差，构建依赖会变宽，调用点预期也会变得模糊。

只有当下面这些条件都成立时，才使用受概念约束的接口：

1. 调用方和被调用方是一起构建的。
2. 定制点对性能或表示形式至关重要。
3. 你能清楚陈述语义契约，而不只是语法契约。

如果这些条件不成立，一个更小、以值为中心的 API，或者一个运行时边界，通常会更好。

## 失败转换属于边界

接口也是失败语义变得显式的地方。如果 adapter 说的是 SQL 异常、gRPC 状态码或平台错误值，这并不意味着系统其余部分也应该说这些语言。

在尽可能靠近易变依赖的位置做失败转换。面向领域的接口应暴露调用方真正能据以决策的失败类别。这样可以防止业务逻辑依赖传输层或厂商错误分类体系，也会让日志和重试行为更容易推理。

不要把错误过度规范化成毫无用处的泛化错误。“操作失败”不是边界模型。重点是暴露稳定且与决策相关的类别，同时把不稳定的后端细节包住。

## 什么时候不该抽象

有些代码就应该直接依赖某个具体类型。过度抽象会制造间接性、隐藏成本，并让简单路径更难读。

如果一个类型局限在单个子系统内、只有一个显然的实现，而且即便改变它也不会产生不同的部署或测试策略，那么直接依赖它通常就是对的。局部辅助类型、解析器、作用域限制在组件内的分配器，以及单后端 pipeline 阶段，不会因为长出了 port 就自动变得更好。

测试标准不是“理论上是否可以抽象”。标准是：这个边界是否隔离了某条真实的变化轴或策略。如果没有，就让依赖保持具体且局部。

## 验证与评审问题

接口设计应当像性能或并发一样，被用同样的纪律来评审。

问这些问题：

1. 接口暴露的是领域含义，还是实现细节？
2. 边界上的所有权和生命周期是否显而易见？
3. 失败类型是否被转换成与决策相关的东西？
4. 调用方能否在不了解存储、传输或框架内部实现的情况下正确使用这个 API？
5. 依赖箭头是否指向更稳定的策略词汇？
6. 这里的抽象是由真实变化轴支撑的，还是仅仅因为想 mock？

验证不只是代码评审。集成测试应当覆盖真正发生转换的 adapter 边界。构建性能分析也很有用：如果一个看似干净的接口仍然把大量传递依赖拖到所有地方，那么这个设计很可能只是把源码级耦合伪装了起来。

## 要点

接口设计的核心，基本就是决定什么东西绝不能泄漏。

让依赖方向对齐稳定策略，而不是方便的实现。保留不必要时，接受廉价的借用输入；但跨越边界返回时，要返回拥有型含义。按职责拆分接口，而不是构造一袋操作。让失败在易变依赖进入系统的地方被转换。只有在真实设计接缝存在时才抽象。

如果调用方要想正确使用你的 API，就必须理解你的数据库 schema、传输包装类型、框架句柄，或者内部存储生命周期，那么这个边界就已经做得太多了。这正是需要在耦合常态化之前重新设计的信号。
