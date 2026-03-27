# 错误、结果与失败边界

大型 C++ 系统出问题，往往不是因为选错了某一种错误机制，而是因为多种机制并存却缺少分层策略。一个子系统抛异常，另一个返回状态码，第三个记完日志就继续跑，第四个把所有失败一律转成 `false`。孤立来看，每个选择似乎都说得通；合在一起，调用方就陷入了困境：搞不清哪些操作可能失败、哪些失败可恢复、诊断信息到底有没有发出去，清理和回滚是否还在正常执行。

生产中真正要回答的问题不是”异常还是 `std::expected`？”而是：每种错误模型该放在哪一层，失败信息怎样跨越边界，以及由系统的哪个部分负责转换、记日志和决定是否崩溃。

错误处理本质上是架构问题。解析层、领域层、存储适配层和进程入口点各有各的约束，混为一谈只会让代码既混乱又在运维上不堪一击。

本章把这些区分讲清楚。我们不取缔异常，也不宣称 `std::expected` 是万能替代品，而是主张一种策略：保留有用的失败信息，同时防止底层机制泄漏到整个代码库。

## 从对失败进行分类开始

并非所有失败都该用同一种方式传递。

生产代码至少应当区分以下几类：

1. 无效输入或验证失败。
2. 环境或边界失败，例如文件 IO、网络错误或存储超时。
3. 契约违反或不可能的内部状态。
4. 进程级启动或关闭失败。

这几类失败同时影响恢复策略和可观测性。无效输入在系统边缘往往可以预料，通常应返回一个局部错误结果，带上足够的细节来干净地拒绝请求或配置。环境失败可能需要做边界转换、制定重试策略或逐级上报。契约违反通常说明程序或子系统已丢失了某个不变量，这更接近于需要崩溃的场景，而不是”返回错误然后继续跑”。启动失败比较特殊，因为系统可能根本没有可用的降级模式，快速失败反而才是正确行为。

这些类别定义清楚之后，API 设计就轻松多了。不是每个函数都需要暴露所有类型的失败。如果唯一可操作的结果只有 `not_found`、`conflict` 和 `temporarily_unavailable`，高层领域函数就没必要去理解某个厂商特有的 SQL 错误枚举。

### 纯错误码方式及其陷阱

在 `std::expected` 出现之前、异常尚未被广泛采用的年代，C++ 代码库（以及从 C 继承来的代码库）主要依赖整数错误码和哨兵返回值。这种做法至今仍很常见，值得具体剖析一下它的问题。

```cpp
// Error-code-only style: caller must check, but nothing enforces it.
enum ConfigErrorCode { kOk = 0, kFileNotFound = 1, kParseError = 2, kInvalidValue = 3 };

ConfigErrorCode load_service_config(const std::string& path, ServiceConfig* out);

void startup() {
    ServiceConfig cfg;
    load_service_config("/etc/app/config.yaml", &cfg); // BUG: return code silently ignored

    // cfg may be uninitialized garbage -- the program continues anyway.
    listen(cfg.port); // binds to nonsense port or zero
}
```

核心问题在于：错误码只是"建议性"的，编译器不强制调用方检查。即便标注了 `[[nodiscard]]`，一个 `void` 强转或者一次无意遗漏就足以让警告消失。对大型代码库中 C 风格错误码 API 的研究反复表明，有 30%~60% 的错误返回值在某些调用点压根没被检查过。

其次是信息丢失。一个整数码承载不了结构化上下文——解析失败的是哪个文件、哪个配置值不合法、底层操作系统到底报了什么错。就算调用方检查了返回码，也往往只记一条笼统的消息就丢掉细节，最终在事故排查时留下一堆毫无用处的诊断。

`std::expected` 同时解决了这两个问题。调用方必须显式地取值或取错误；如果结果里装的是错误却硬要当值用，这一行为在代码评审中一眼可见（粗心这么干就是未定义行为，Sanitizer 会帮你抓住）。错误类型还可以直接承载结构化诊断信息，无需借助旁路日志。

示例项目在整个代码库中贯彻了这一做法。`error.cppm` 中定义了统一的 `Result<T>` 别名，使模式在所有模块间保持一致：

```cpp
// examples/web-api/src/modules/error.cppm
template <typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error>
make_error(ErrorCode code, std::string detail) {
    return std::unexpected<Error>{Error{code, std::move(detail)}};
}
```

`Error` 携带一个类型化的 `ErrorCode` 枚举和一条人类可读的详情字符串，既能支撑程序化分支，也能提供诊断信息。不会向调用方暴露任何整数错误码；每条失败路径都经由 `Result<T>`。

## 异常适合展开和局部清晰性

异常在 C++ 中依然有其价值，因为栈展开天然与 RAII 配合。当构造函数在资源拥有对象图构建到一半时失败，异常可以让语言自动驱动析构，省去手写的清理阶梯。当一段局部实现里有多层嵌套的辅助调用、且它们都可能以同样的方式失败时，异常也能让主路径保持清爽可读。

但这并不意味着异常可以充当通用的边界模型。

优点：

- 正常流程与失败流程分离，
- 跨越多层调用时代码仍然简洁，
- 与 RAII 搭配良好，清理自动完成。

缺点：

- 失败信息不体现在函数签名中，
- 可能穿越那些根本没有为异常做过设计的边界，
- 底层异常类型一旦泄漏到上层代码，就会破坏分层。

结论是保持克制：异常通常在层内部很好用。但除非整个代码库已经统一约定了异常模型并有手段强制执行，否则异常通常不适合作为跨子系统边界的通用语言。

## `std::expected` 擅长决策边界

`std::expected<T, E>` 在抽象层面并不比异常更优。它的优势在于：当调用方需要根据失败做出分支决策时，`expected` 把决策点摆到了台面上。

解析、验证、边界转换和请求级操作经常属于这类场景。调用点通常需要分支处理、发出结构化拒绝、选择重试策略或附加上下文信息。返回 `expected` 让这个决策点一目了然。

以一个配置加载器为例：

```cpp
enum class ConfigErrorCode {
    file_not_found,
    parse_error,
    invalid_value,
};

struct ConfigError {
    ConfigErrorCode code;
    std::string message;
    std::string source;
};

auto load_service_config(std::filesystem::path path)
    -> std::expected<ServiceConfig, ConfigError>;
```

这个签名直接告诉读者：在这个边界上，失败属于正常控制流。调用方必须做出决定，中止启动、回退到默认环境，还是输出一条清晰的诊断信息。这跟深层内部的辅助函数不同，后者面对失败时唯一合理的做法往往是向上展开，交给真正能拍板的边界去处理。

把这个基于 `expected` 的加载器和传统的”输出参数 + bool”方式放在一起比较，就能看出旧风格丢掉了多少信息：

```cpp
// Old style: bool return, output parameter, no structured error.
bool load_service_config(const std::filesystem::path& path,
                         ServiceConfig* out,
                         std::string* error_msg = nullptr);

void startup() {
    ServiceConfig cfg;
    std::string err;
    if (!load_service_config("/etc/app/config.yaml", &cfg, &err)) {
        // What kind of failure? File missing? Parse error? Permission denied?
        // err is a free-form string -- no programmatic branching possible.
        LOG_ERROR("config load failed: {}", err);
        std::exit(1); // only option: cannot distinguish retriable from fatal
    }
}
```

换用 `std::expected<ServiceConfig, ConfigError>` 后，调用方可以根据 `ConfigErrorCode::file_not_found` 和 `ConfigErrorCode::parse_error` 分别走不同的恢复策略，同时照样能拿到适合记日志的可读消息。决策所需的信息由类型系统承载，而不是埋在一个字符串里。

`expected` 的风险在于过度传播。如果每个细小的辅助函数仅仅因为 public 边界用了 `expected` 就跟着返回 `expected`，实现里就会堆满重复的转发逻辑，把主算法淹没掉。`expected` 应当放在设计上确实需要暴露错误的地方，不要硬塞进每个 private 函数，除非那样确实能提升局部可读性。

## 反模式：没有边界策略的副作用式错误处理

生产环境中常见的翻车方式：同一个子系统里同时存在”记日志””返回部分状态”和”偶尔抛异常”三种做法。

```cpp
// Anti-pattern: side effects and transport are mixed.
bool refresh_profile(Cache& cache, DbClient& db, UserId user_id) {
    try {
        auto row = db.fetch_profile(user_id);
        if (!row) {
            LOG_ERROR("profile not found for {}", user_id);
            return false;
        }

        cache.put(user_id, to_profile(*row));
        return true;
    } catch (const DbTimeout& e) {
        LOG_WARNING("db timeout: {}", e.what());
        throw; // RISK: some failures logged here, some rethrown, signature hides both
    }
}
```

这个函数用起来代价很高：调用方既不知道 `false` 到底代表什么，也不知道哪些失败已经记过日志了，更不知道自己还需不需要补充上下文。如果好几层都是这个套路，事故排查时就会同时面对一片噪音和一堆信息空白。

边界代码应当只选择一种传递方式、一种日志策略。要么函数返回结构化失败，把日志交给能附加请求上下文的上层来写；要么就在本层彻底处理失败，并在契约中写明白。两者一混用，重复日志和遗漏决策就会混入系统。

## 反模式：未检查返回值导致静默失败

副作用问题还有一种更隐蔽的变体：把失败悄悄转成默认值，不给调用方留下任何线索。

```cpp
// Anti-pattern: failure becomes a silent default.
int get_retry_limit(const Config& cfg) {
    auto val = cfg.get_int("retry_limit");
    if (!val) {
        return 3; // silent fallback -- no log, no metric, no trace
    }
    return *val;
}
```

这种写法很有诱惑力，因为代码永远不会崩。但当配置文件里有拼写错误（写成了 `retry_limt` 而不是 `retry_limit`），系统就会悄悄用上硬编码的默认值。事故期间运维人员改了配置、指望行为跟着变，结果什么都没发生。这个 bug 之所以无迹可寻，恰恰是因为错误被吞掉了。

更好的做法是让默认值显式化，并让回退行为可观测：

```cpp
auto get_retry_limit(const Config& cfg) -> std::uint32_t {
    constexpr std::uint32_t default_limit = 3;
    auto val = cfg.get_uint("retry_limit");
    if (!val) {
        LOG_INFO("retry_limit not configured, using default={}", default_limit);
        return default_limit;
    }
    return *val;
}
```

或者，如果调用方应该决定缺失值是否可接受，就直接返回 `expected` 或 `optional`，让边界去做策略选择。

## 在易变依赖附近做转换

边界转换是错误设计的主战场。

存储适配层可能收到驱动抛出的异常、状态码、重试提示或平台错误。系统的其余部分不想直接面对这些细节，它们想要的是与决策相关的分类，外加刚好够用于诊断的上下文。

转换应当发生在靠近不稳定依赖的地方，而不是在三层之外的业务逻辑里。

```cpp
auto AccountRepository::load(AccountId id)
    -> std::expected<AccountSnapshot, AccountLoadError>
{
    try {
        auto row = client_.fetch_account(id);
        if (!row) {
            return std::unexpected(AccountLoadError::not_found(id));
        }
        return to_snapshot(*row);
    } catch (const DbTimeout& e) {
        return std::unexpected(AccountLoadError::temporarily_unavailable(
            id, e.what()));
    } catch (const DbProtocolError& e) {
        return std::unexpected(AccountLoadError::backend_fault(
            id, e.what()));
    }
}
```

这样做没有抹掉有用信息，只是把它们包装成了调用方能直接使用的形式。业务逻辑现在可以区分 not-found 和暂时不可用，而无需去学存储客户端自己的错误体系。

同样的原则适用于网络边界、文件系统边界和第三方库：在靠近边缘的地方统一转换一次，不要让原始后端错误一路渗透。

示例项目在 HTTP 边界展示了同样的模式。`handlers.cppm` 中的 `result_to_response()` 在边缘处将领域 `Result<T>` 一次性转换为 HTTP 响应：

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

领域逻辑只和 `Result<T>` 打交道。到 HTTP 状态码和 JSON 错误体的转换集中在 handler 边界的这一个函数里，领域代码不引入 HTTP 概念，handler 代码也不窥探错误内部，只调用这个转换函数。

## 构造函数、析构函数和启动需要不同规则

错误策略应当因生命周期阶段而异。

构造函数通常适合用异常，因为”部分构造 + RAII”是 C++ 最强的组合之一。一个持有资源却无法进入有效状态的对象，不该被创建出来。返回一个半初始化的对象再附带一个状态码，几乎不会更好。

析构函数恰好相反。析构过程中抛异常通常要么是灾难性的，要么从设计上就被禁止。如果清理操作可能失败且后果不可忽视，类型设计上应该提供显式的 `close`、`flush` 或 `commit` 方法，趁对象还处于受控状态时报告错误。析构函数退化为尽力清理的最后一道防线。

启动又有所不同。进程启动期间的配置加载、依赖初始化和端口绑定，往往只有一种合理的失败策略：输出清晰的诊断信息，然后让进程退出。不是说每个启动辅助函数都该调 `std::exit`，而是说顶层启动边界应当掌握这个决策权，底层只需返回足够结构化的信息，让失败原因一目了然。

## 诊断必须丰富，但不能传染

好的错误处理保留上下文，差的错误处理则把拼装上下文的代码塞进每个分支，直到主逻辑被淹没。

有用的失败信息通常包括：

- 一个稳定的类别或代码，
- 一条人类可读的消息，
- 文件路径、tenant、shard 或 request id 等标识符，
- 有时包括在确实有助于调试时才保留的后端细节或栈追踪数据。

错误对象应当有意义，但不要沦为内部细节的垃圾桶。面向领域的错误类型应当暴露调用方做决策所需的信息和运维排障所需的信息，而不是一路上遇到的每一条底层异常字符串。

命名良好的错误类型很重要。`expected<T, std::string>` 写起来快，但作为系统设计很弱。字符串适合做最终的诊断输出，不适合当架构契约。

## 在哪里记录日志

最干净的默认做法：在那些拥有足够上下文、能让日志条目具备运维价值的边界上记录。

通常是请求边界、后台作业监控器、启动入口点和外层重试循环，而不是每个察觉到失败的辅助函数。日志打得太早会丢掉上下文，每层都打会制造重复噪音，一直不打到进程挂了才发现则会丧失证据。

核心规则：由哪一层来判定”这个失败在运维上意味着什么”，那一层就是记日志的正确位置。

这条规则无论和 `expected` 风格的边界还是异常转换都能很好地配合。底层负责保留信息，边界层负责分类、附加上下文、决定恢复方式，并且只记录一次。

## 契约违反不只是另一条错误路径

有些失败说明程序收到了坏输入，另一些则说明程序自身违背了自己的假设。

如果某个不变量本应在更早阶段就已成立，此刻却不成立，或者代码走到了一个理论上不可达的状态，那么把它当成又一种可恢复的业务错误来处理，往往只会掩盖更深层的 bug。这不一定要求立刻终止进程，但确实需要用不同于普通验证失败的方式来对待。

好的代码库会把这些区分显式化：输入失败就建模为输入失败，后端不可用就建模为环境失败，内部不变量被破坏则作为 bug 暴露出来，而不是塞进”操作失败”的通用代码路径里一笔带过。

## 验证与评审

失败处理应当作为系统级属性来评审，而不是逐个函数孤立地看。

有用的评审问题包括：

1. 在这个边界上，哪些失败是预期内的，并且与决策相关？
2. 异常是在层内部服务于代码清晰性，还是在层间不可控地泄漏？
3. `expected` 承载的是真正的决策信息，还是只不过把异常换成了样板代码？
4. 后端特有的失败是在哪一层被转换成稳定分类的？
5. 日志是否只在那个拥有足够上下文的层记录了一次？

测试应当有意覆盖非正常路径：解析无效输入、模拟超时和 not-found、验证后端失败到领域失败的转换、演练启动失败路径，以及显式 `close` 或 `commit` 操作。一个只测正常路径的代码库，迟早会在生产环境里被迫直面自己真正的错误模型。

## 要点

- 根据层次和边界来选择错误传递方式，而不是凭信条。
- 在栈展开和局部可读性有帮助的地方使用异常，尤其是层内部和构造阶段。
- 在调用方必须根据失败做出明确决策的地方使用 `std::expected`。
- 在依赖边界附近，把不稳定的后端错误转换成稳定的、面向决策的分类。
- 在能理解失败运维含义的那一层记录日志。

如果调用方搞不清到底出了什么错、日志是否已经记过、自己接下来该怎么办，失败边界的设计就有问题。这在演变成线上故障之前，就已经是设计缺陷了。
