# 接口设计与依赖方向

本章假定你已经读过函数签名设计、所有权、不变量和失败边界的相关内容。这里要讨论的不是怎么写好一个函数，而是怎么设计一条经得起团队扩张和系统演进压力的边界。

## 生产问题

大多数接口问题并非源自一眼就能看出的烂代码，而是源自那些局部看来合理、却逐渐固化为系统级耦合的决定。存储层直接返回数据库行类型——反正手头就有嘛。服务边界接收一个硕大的配置对象——万一将来要加选项呢。库里所有回调一律用 `std::function`——看着挺灵活的。半年之后，测试要拉起半张依赖图，调用点到处泄漏传输层细节，改个实现细节就成了破坏性变更。

本章讨论的是源码层面的接口设计：边界暴露什么、依赖应当朝哪个方向指、如何防止策略、所有权和表示形式跨层泄漏。运行时分派机制是下一章的事，二进制兼容性和分发是再下一章的事。这里的关注点更窄，也更重要：决定程序的一部分可以知道另一部分的哪些事实。

核心规则很简单：依赖应当指向稳定的策略和领域含义，而非易变的实现细节。落到实处，就是接口应该用调用方已经理解的概念来构建，而不是围绕被调用方的存储、传输、框架或日志方案来设计。

## 为什么这会变得昂贵

错误的依赖方向会以代码评审很难察觉的方式悄悄放大成本。

一旦领域逻辑直接依赖了 SQL 行类型、protobuf 生成类、HTTP 请求包装器或文件系统遍历状态，那么每次测试、benchmark 和重构都得把这些细节一并拖进来。依赖图会比设计实际需要的宽得多。传递性的头文件包含和模板实例化把实现细节散播到各处，拖慢构建速度。边界违规一旦成为家常便饭，评审质量也会随之下滑。最要命的是，那些本应局部化的设计决策，再也局部不了了。

代价不仅仅是编译时间，更在于概念层面的稳定性。好的接口扛得住数据库变更、队列替换或日志系统重写；坏的接口会迫使代码库的其他部分去重新学习那些本就与它们无关的内部细节。

## 从边界问题开始

动手写接口之前，先强迫自己把生产问题压缩成一句话。

对于原生服务，问题通常不是”repository 怎么暴露？”而是”订单工作流怎样获取客户的信用状态？”对于共享库，问题不是”解析器的内部实现怎么公开？”而是”调用方需要怎样的契约才能校验并转换输入记录？”

这个视角转换很重要，因为它直接影响类型的形状。围绕实现名词来设计的接口，往往会泄漏机制；围绕工作职责和不变量来设计的接口，则往往能保持精简。

一个接口应当能清楚回答四个问题：

1. 调用方需要什么能力？
2. 数据和生命周期归哪一侧所有？
3. 失败在哪里被转换为第 3 章的错误模型？
4. 哪些策略在这里已经定死，哪些留给调用方自行决定？

如果这些问题答不清楚，接口多半是在混层。

## 依赖方向意味着策略方向

依赖倒置常被机械地解释为”依赖抽象，而非具体实现”。没错，但光这样说还不够。真正管用的判据是：依赖箭头是否指向稳定的策略。

在一个服务里，业务规则的变化速度通常远慢于传输胶水代码。欺诈策略不应依赖 HTTP handler，订单校验不应依赖 SQL 记录包装器。领域逻辑可以定义自己需要的 port，让数据库或网络 adapter 去实现它。

但这并不是说每个边界都得有一个抽象基类。很多时候根本不需要。有时候正确的边界就是一个接收领域数据的自由函数；有时候是内部库里一个受 concept 约束的模板；有时候是一对值类型的请求/结果对象，完全不涉及虚派发。设计上真正要问的不是”接口类型放在哪？”，而是”哪一侧有资格给契约命名？”

答案通常是：拥有更稳定词汇的那一侧。

## 反模式：接口由依赖项来定义

一旦契约由实现细节来命名，依赖箭头就已经指反了。

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

乍一看好像可测试——毕竟用了抽象基类。但接缝本身就是错的。支付工作流不应该知道可用额度是以"分"为单位存储的，更不应该知道旁边还躺着一个从表行加载出来的欺诈标记字符串。这个抽象保住了依赖关系，却丝毫没有改善依赖方向。

更好的做法是让工作流来定义 port，只返回工作流所需的最少稳定事实。

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

现在工作流依赖的是领域含义，而非存储形状。与 SQL 打交道的 adapter 负责做转换。这确实有额外工作量，但这才是正确的工作量——把易变性收束在易变事物附近。

## 反模式：会把周围一切都吸进来的胖接口

臃肿的接口不仅仅是不好看。它会形成耦合引力场：每个新功能都往现有接口上加，因为加个方法总比重新审视边界来得省事。

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

这个接口至少混杂了四条互不相关的变化轴：用户数据访问、审核策略、运维可观测性、运行时配置。一个只想读 profile 的调用方，却被迫传递性地依赖审计、缓存、metric 和限流的类型。测试替身为了伪造一个行为，得实现全部七个方法。加一个审核动作，只读消费者也得重新编译。这个接口谈不上灵活，它是一个依赖黑洞——每次变更都很贵，每个测试都很脆。

解决办法是沿职责边界拆分：

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

这样一来，只读消费者只依赖 `UserProfileQuery`，审核工具只依赖 `ModerationActions`，运维相关的东西住在另一个接口里。各自独立演化，测试替身也很简单。

## 反模式：通过接口泄漏实现细节

接口再小，只要暴露了不该暴露的类型，照样能伤害整个系统。

```cpp
// Anti-pattern: interface leaks the JSON library into every consumer.
#include <nlohmann/json.hpp>

class RetryConfigProvider {
public:
virtual nlohmann::json load_retry_config() = 0;
virtual ~RetryConfigProvider() = default;
};
```

这样一来，每个包含这个头文件的编译单元都依赖了 JSON 库，不管它自己用不用 JSON。想换成 TOML、YAML 或二进制配置格式？对不起，整个代码库都得跟着改。JSON 库带来的编译开销、宏定义和传递性头文件也跟着扩散到无关组件里。更糟的是，调用方必须在 JSON 树上手动提取重试参数——初始退避时间、最大退避时间、最大重试次数——隐式的 schema 知识因此散落在代码库各处。

解决办法是返回有领域含义的类型：

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

现在 JSON 依赖被关在 adapter 实现内部。消费者拿到的是强类型、已校验的值。接口传达的是领域含义，而非序列化格式。

## 反模式：抽象层级错误

抽象层级不对的接口，要么逼调用方去做本应被封装的工作，要么拦着调用方做它真正需要做的事。

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

这个接口号称抽象了存储，实际上却把 SQL 当作字符串协议直接暴露出来。调用方仍然得了解 schema、拼正确的 SQL、解析 `RowSet` 结果。SQL 注入防不住，schema 耦合也甩不掉。它不过是个直通层——多了一层间接，依赖一点没少。

反过来，接口也可能抽象得太高，反而妨碍了正常使用：

```cpp
// Anti-pattern: too high-level. No way to paginate, filter,
// or control what gets loaded.
class OrderRepository {
public:
virtual std::vector<Order> get_all_orders() = 0;
virtual ~OrderRepository() = default;
};
```

正确的抽象层级应当贴合调用方实际要做的操作，使用领域词汇，同时提供足够的控制力以保证效率。

## 通过分离命令与查询来保持接口小巧

臃肿接口的根源通常是把毫不相关的变更理由混到了一起。一个边界如果既读状态、又改状态、还发审计事件、开事务、暴露 metric 快照——那它不是灵活，而是又一个依赖汇点。

把命令和查询分开，往往就能恢复清晰度。查询路径想要的是值类型的请求和结果、可预测的开销、没有隐藏的副作用。命令路径想要的是显式的所有权转移、明确的副作用和严格的失败语义。硬塞成一个接口，就是在纵容偶然耦合——调用方迟早会依赖上个季度顺手塞进去的某个方法。

接口越小，评审也越容易。评审者可以直接问：这里的每个函数到底属不属于同一个边界？而一旦接口沦为”附近操作的大杂烩”，这个问题就很难回答了。

`examples/web-api/src/modules/repository.cppm` 中的 `TaskRepository` 就是一个保持聚焦的窄接口。它的公共面只有 CRUD 操作：`create`、`find_by_id`、`find_all`、`find_completed`、`update`、`remove` 和 `size`。没有日志方法，没有配置开关，没有 metric 快照，没有缓存刷新。锁策略（`std::shared_mutex`）、存储表示（`std::vector<Task>`）、ID 生成（`std::atomic<TaskId>`）全部是 private 的。调用方依赖的是领域操作，而不是 repository 碰巧如何实现它们。

## 数据形状：接受稳定视图，返回拥有型含义

第 4 章讨论的是局部的签名选择。到了接口边界，同样的规则就升格为架构规则。

如果被调用方不需要保留数据，输入通常应接受非拥有视图：`std::string_view`、`std::span<const std::byte>`、领域对象的 span，或者引用调用方所持数据的轻量请求结构体。这样调用点既便宜又坦诚。

输出通常应返回拥有型值或生命周期明确的领域对象。如果返回的是指向 adapter 内部存储的视图、指向 cache line 的借用指针、指向内部状态的迭代器，那就把边界变成了生命周期谜题，很少值得这么做。

这种不对称是刻意为之的。当开销敏感而数据无需留存时，就从调用方借用；跨边界往回传时，则交出所有权——因为被调用方掌控着自己的内部实现，不应该强迫调用方操心这些实现能活多久。

当然也有例外。热路径解析器、零拷贝数据流水线、内存映射处理阶段可能有意返回视图。但即便如此，生命周期边界也必须写进接口契约，而不能靠口口相传。一个与特定 buffer 拥有者绑定的 `ParsedFrameView` 类型，远比泄漏裸 `std::string_view` 或原始指针、然后指望评审者自己发现这层耦合，要安全得多。

## 不要通过可选参数偷运策略

想让接口迅速变得含混，最简单的办法就是用配置对象或默认参数，把策略决策塞到调用方根本推理不了的地方。

如果一个函数挂着 `skip_cache`、`best_effort`、`emit_audit`、`allow_stale`、`retry_count` 之类的标志，它多半是在干太多事。问题不在美观，而在于调用方现在可以拼出语义不清、未经测试甚至运维上危险的参数组合。

应优先考虑以下三种替代方案：

1. 拆成几个命名更清晰的独立操作。
2. 把策略提升为显式类型，使无效状态要么不可能出现，要么一眼可见。
3. 把策略选择上移一层，让低层接口保持确定性行为。

策略被显式命名，而不是埋在参数乱炖里，接口才容易演化。

## 可测试性是结果，不是目标

团队经常用”方便测试”来为引入接口辩护。这是因果倒置。首先要问的是：边界是否反映了真实的设计意图？如果是，测试自然会变简单；如果不是，测试替身只是在帮你维护一个错误。

举个例子，仅仅为了在单元测试里伪造数据库访问就引入一个 repository 接口，理由是站不住的——尤其当领域层仍然依赖表结构的数据和传输层的错误类型时。测试也许确实更好写了，但设计照样是错的。

好的边界之所以能产出好的测试，是因为它把策略和机制分离了。你可以用简单的 fake 测试业务逻辑，因为业务逻辑要的是领域事实，不是框架对象。你可以单独对 adapter 做集成测试，因为转换逻辑被收拢在一处。这比”现在我们能 mock 它了”强得多。

## 在内部使用概念和模板，而不要把它们当成公共逃生口

现代 C++ 让你很容易用约束而非虚类来表达接口。在组件内部或严格受控的代码库里，这往往是正确选择。受 concept 约束的模板可以做到零分配、可内联，表达力也常常胜过深层继承体系。

然而，一个试图用模板包打天下的公共接口，往往到最后已经不像接口了。它同时充当策略配置面、编译期集成机制和文档负担。报错信息劣化，构建依赖膨胀，调用点的预期也变得模糊不清。

只有同时满足以下条件时，才适合使用 concept 约束的接口：

1. 调用方和被调用方一起编译。
2. 定制点对性能或数据表示至关重要。
3. 你能把语义契约讲清楚，而不仅仅是语法契约。

条件不满足的话，一个更小的、以值为中心的 API 或运行时边界通常是更好的选择。

## 失败转换属于边界

接口同时也是失败语义显式化的地方。adapter 内部说的可能是 SQL 异常、gRPC 状态码或平台错误值，但这不意味着系统其他部分也得讲同样的”方言”。

应尽可能在靠近易变依赖的位置完成失败转换。面向领域的接口应暴露调用方真正能据以决策的失败类别。这既能防止业务逻辑对传输层或厂商错误分类体系的依赖，也能让日志和重试逻辑更好理解。

但也不要把错误过度泛化到毫无信息量。”操作失败”算不上边界模型。关键在于暴露稳定的、与决策相关的类别，同时把不稳定的后端细节封装起来。

`examples/web-api/` 示例项目给出了一个具体示范。`handlers.cppm` 中的 `result_to_response()` 恰好坐落在领域逻辑与 HTTP 传输之间的边界上：

```cpp
// examples/web-api/src/modules/handlers.cppm
template <json::JsonSerializable T>
[[nodiscard]] http::Response
result_to_response(const Result<T>& result, int success_status = 200) {
    if (result) {
        return {.status = success_status, .body = result->to_json()};
    }
    return http::Response::error(result.error().http_status(),
                                 result.error().to_json());
}
```

领域代码始终只与 error 模块中的 `Result<T>` 和 `ErrorCode` 打交道。HTTP 状态码映射在 `error.cppm` 的 `to_http_status()` 中一次定义，转换为 HTTP 响应的工作则发生在 handler 层。领域类型不知道 HTTP 响应长什么样，handler 也不向传输层泄漏领域错误的内部结构。边界负责翻译，两侧各说各的词汇。

## 什么时候不该抽象

有些代码就该直接依赖具体类型。过度抽象只会制造间接层、隐藏开销、让简单路径变得难读。

如果一个类型只在单个子系统里用、只有一种显而易见的实现、换掉它也不会带来不同的部署或测试策略，那直接依赖它通常就是对的。内部辅助类型、解析器、作用域限于组件内的分配器、单后端的 pipeline 阶段——它们不会因为套上了 port 就自动变好。

判断标准不是”理论上能不能抽象”，而是”这个边界是否隔离了一条真实的变化轴或策略”。如果答案是否定的，就让依赖保持具体、保持局部。

## 验证与评审问题

接口设计应当像性能和并发一样，受到同等严格的评审。

评审时可以问这些问题：

1. 接口暴露的是领域含义还是实现细节？
2. 边界处的所有权和生命周期是否一目了然？
3. 失败类型是否已转换为调用方能据以决策的形式？
4. 调用方能否在不了解存储、传输或框架内部的前提下正确使用这个 API？
5. 依赖箭头是否指向更稳定的策略词汇？
6. 这里的抽象是有真实变化轴支撑的，还是纯粹为了能 mock？

验证不只靠代码评审。集成测试应当覆盖真正发生转换的 adapter 边界。构建性能分析同样有价值：如果一个看似干净的接口仍然把大量传递性依赖拖得到处都是，那这个设计很可能只是给源码级耦合披了层伪装。

## 要点

接口设计说到底，就是决定什么东西绝不能泄漏出去。

依赖方向要对齐稳定策略，而非一时方便的实现。无需留存数据时，接受廉价的借用输入；跨边界返回时，交出拥有型的领域含义。按职责拆分接口，而非堆砌一堆操作。在易变依赖进入系统的位置完成失败转换。只在真实的设计接缝处做抽象。

如果调用方想正确使用你的 API，却不得不了解你的数据库 schema、传输包装类型、框架句柄或内部存储的生命周期，那这个边界承载的东西就已经太多了。趁耦合还没变成常态，赶紧重新设计。
