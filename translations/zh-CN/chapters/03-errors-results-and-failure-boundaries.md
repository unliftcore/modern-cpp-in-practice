# 错误、结果与失败边界

大多数大型 C++ 系统失败，不是因为它们选错了某一种单一的错误机制，而是因为它们同时使用了多种机制，却没有分层策略。一个子系统抛异常，另一个返回状态码，第三个记录日志后继续，第四个把所有失败都转换成 `false`。单独看，每个选择都可能有其道理；放在一起，代码就会变成这样：调用方无法判断哪些操作可能失败、哪些失败可恢复、诊断信息是否已经发出，以及清理和回滚是否仍然执行。

生产问题不是“异常还是 `std::expected`？”生产问题是：每种错误模型应该属于哪里，失败信息如何跨越边界，系统中的哪些部分负责转换、记录日志和决定崩溃。

这种边界视角很重要，因为错误处理是架构问题。解析层、领域层、存储适配层和进程入口点面对的是不同约束。把它们混在一起，正是代码变得嘈杂且在运维上脆弱的原因。

本章会把这些区分保持清晰。它不会试图取缔异常，也不会宣布 `std::expected` 是通用替代品。它主张的是一种策略：保留有用的失败信息，同时不让每一种底层机制泄漏到整个代码库。

## 从对失败进行分类开始

并非每一种失败都值得用同样的方式传递。

至少，生产代码应当区分这些类别：

1. 无效输入或验证失败。
2. 环境或边界失败，例如文件 IO、网络错误或存储超时。
3. 契约违反或不可能的内部状态。
4. 进程级启动或关闭失败。

这些类别同时影响恢复策略和可观测性。无效输入通常是在系统边缘可以预期的，通常应当变成一个局部错误结果，并携带足够的细节，以便干净地拒绝请求或配置。环境失败可能需要边界转换、重试策略或升级。契约违反通常意味着程序或子系统已经失去了某个关键不变量；这更接近崩溃领域，而不是“返回一个错误后继续”。启动失败则是特殊情况，因为系统可能根本没有有意义的降级模式。快速失败可能就是正确行为。

一旦这些类别明确了，API 设计就会容易得多。不是每个函数都应该直接暴露每一类失败。高层领域函数不应需要理解某个厂商特定的 SQL 错误枚举，如果唯一可操作的结果只有 `not_found`、`conflict` 和 `temporarily_unavailable`。

### 纯错误码方式及其陷阱

在 `std::expected` 出现之前，在异常被广泛采用之前，C++ 代码库（以及 C++ 继承而来的 C 代码库）依赖整数错误码和哨兵返回值。这种方式今天仍然很常见，因此值得具体看看它的失败模式。

```cpp
// Error-code-only style: caller must check, but nothing enforces it.
enum StatusCode { kOk = 0, kNotFound = 1, kTimeout = 2, kCorrupt = 3 };

StatusCode load_account(const char* id, AccountRow* out);

void process_request(const char* account_id) {
AccountRow row;
load_account(account_id, &row); // BUG: return code silently ignored

// row may be uninitialized garbage -- the program continues anyway.
publish(row.balance); // publishes nonsense data
}
```

核心问题在于，错误码只是建议性的。编译器不会强制调用方检查它们。即使有 `[[nodiscard]]`，转成 `void` 或一次无意遗漏也足以压掉警告。在大型代码库里，对 C 风格错误码 API 的研究一再发现，30% 到 60% 的错误返回在某些调用点根本没有被检查。

第二个问题是信息丢失。一个整数码无法携带结构化上下文（哪个文件、哪个账户、后端到底说了什么）。即便调用方检查了返回码，也常常只记录一条泛泛的消息，然后丢掉细节，结果是在事故期间产生毫无用处的诊断。

`std::expected` 同时解决了这两个问题。调用方必须显式访问值或错误；如果结果持有的是错误，却试图使用值，这会成为一个可见、可评审的决定（粗心这么做时就是未定义行为，而 Sanitizer 会抓到）。错误类型也可以在没有旁路日志的情况下承载结构化诊断信息。

## 异常适合展开和局部清晰性

异常在 C++ 中仍然有价值，因为栈展开能自然地与 RAII 组合。当一个构造函数在资源拥有对象图构造到一半时失败，异常能让语言驱动销毁，而不必手写清理梯子。当一个局部实现里有多个嵌套辅助调用，而它们都可能以同一种方式失败时，异常也能让主路径保持可读。

这并不意味着异常是通用的边界模型。

它们的优点是真实的：

- 它们把正常流程与失败流程分开，
- 它们在跨越多层调用时仍然保持代码简洁，
- 它们和 RAII 配合得很好，因为清理仍然是自动的。

它们的弱点也是真实的：

- 它们把失败从签名里隐藏起来，
- 它们可能跨越那些从未为它们设计的边界，
- 当低层异常类型泄漏到高层代码时，它们会诱发糟糕的分层。

正确的结论应当克制。异常通常是层内的一种好用的内部机制。除非整个代码库已经一致地承诺采用这种模型，并且能够强制它，否则它通常不是广泛子系统边界上的良好语言。

## `std::expected` 擅长决策边界

`std::expected<T, E>` 并不是抽象意义上比异常更好。它更适合那些调用方预期需要基于失败做出可见决策的场景。

解析、验证、边界转换和请求级操作经常属于这一类。调用点通常需要分支、发出结构化拒绝、选择重试行为或附加上下文。返回 `expected` 会把这个决策点显式化。

考虑一个配置加载器：

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

这个契约立即告诉读者一件重要的事：在这个边界上，失败是正常控制流的一部分。调用方必须决定，是中止启动、回退到默认环境，还是报告一条清晰的诊断。这和某个深层内部辅助函数不同——后者唯一合理的失败策略，可能只是展开到那个真正能做出选择的边界。

把这个基于 `expected` 的加载器，与传统的“输出参数加 bool”方式对比一下，就能看出旧风格丢失了多少信息：

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

使用 `std::expected<ServiceConfig, ConfigError>` 后，调用方可以根据 `ConfigErrorCode::file_not_found` 和 `ConfigErrorCode::parse_error` 分支，选择不同恢复策略，同时仍然拿到适合记录日志的人类可读消息。类型系统携带了与决策相关的信息，而不是把它埋进一个字符串里。

`expected` 的危险在于过度传播。如果每个细小的辅助函数仅仅因为一个 public 边界这么做，就全都返回 `expected`，实现里就可能到处都是重复的转发逻辑，遮蔽主算法。应当把 `expected` 放在设计中真正属于错误的地方，不要强迫它穿过每一个 private 函数，除非那样真的能改善局部清晰性。

## 反模式：没有边界策略的副作用式错误处理

生产环境中最常见的一类失败，是在同一个子系统里同时出现“记录日志”“部分状态返回”和“偶尔抛异常”。

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

这个函数之所以昂贵，是因为调用方不知道 `false` 究竟意味着什么，不知道哪些失败已经被记录，也不知道自己是否还需要补充上下文。如果多个层都遵循这种模式，事故就会同时变得嘈杂且缺乏解释。

边界代码应当选择一种传递方式和一种日志策略。要么函数返回结构化失败，并把日志留给能附加请求上下文的更高层；要么它彻底处理失败，并在契约中明确说明。把两者混用，就是让重复日志和缺失决策进入系统的方式。

## 反模式：未检查返回值导致静默失败

副作用问题还有一种更隐蔽的变体：代码把失败转换成默认值，却不给调用方任何信号。

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

这很诱人，因为代码永远不会崩。但当配置文件里有拼写错误（`retry_limt` 而不是 `retry_limit`）时，系统会悄悄使用一个硬编码默认值。事故期间，运维人员修改配置，希望行为发生变化，结果什么也没发生。这个 bug 之所以不可见，正是因为错误被吞掉了。

更好的做法是让默认值显式，并让回退行为可观测：

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

边界转换，才是大多数错误设计工作应当发生的地方。

存储适配层可能收到驱动抛出的异常、状态码、重试提示或平台错误。系统其余部分通常并不想直接看到这些细节。它想要的是与决策相关的类别，以及也许足以诊断的少量附带上下文。

这意味着，转换应当发生在不稳定依赖附近，而不是三层外的业务逻辑里。

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

这并没有抹去有用信息，而是把它们打包成调用方可以采取行动的形式。业务逻辑现在可以区分 not-found 和暂时不可用，而不必学习存储客户端自己的失败分类。

同样的规则也适用于网络边界、文件系统边界和第三方库。靠近边缘转换一次，不要让原始后端错误一路泄漏，直到每一层都被迫理解它们。

## 构造函数、析构函数和启动需要不同规则

错误策略应当尊重生命周期上下文。

构造函数通常是使用异常的好地方，因为“部分构造 + RAII”是 C++ 最强的组合之一。一个拥有资源、却无法被构造成有效状态的对象，通常就不应该存在。返回一个半初始化对象再附带状态，通常不会更好。

析构函数则相反。在析构过程中抛异常通常是灾难性的，或者从设计上就是被禁止的。如果清理失败具有实际意义，那么这个类型可能需要一个显式的 `close`、`flush` 或 `commit` 操作，在对象仍处于受控状态时报告失败。析构函数随后退化为尽力清理或最后的安全网。

启动是它自己的特殊场景。在进程启动期间，配置加载、依赖初始化和端口绑定往往只有一种合理的失败策略：产出清晰诊断并让进程失败。这并不等于每个启动辅助函数都应该调用 `std::exit`。它意味着顶层启动边界应当拥有这个决策，而更低层应当返回足够结构化的信息，让失败显而易见且精确。

## 诊断必须丰富，但不能传染

良好的错误处理会保留上下文。糟糕的错误处理会把构造上下文的代码扩散到每个分支里，直到主要行为消失。

有用的失败信息通常包括：

- 一个稳定的类别或代码，
- 一条人类可读的消息，
- 关键标识符，例如文件路径、tenant、shard 或 request id，
- 有时还包括在确实有助于调试时才保留的后端细节或栈追踪数据。

诀窍是，让错误对象有意义，同时不要让它变成每个内部细节的大杂烩。一个面向领域的错误类型，应当暴露调用方做决策所需的信息，以及运维排障所需的信息，而不是途中遇到的每一条底层异常字符串。

这也是命名良好的错误类型重要的原因之一。`expected<T, std::string>` 写起来很快，作为系统设计却很弱。字符串适合作为最终诊断，不适合作为架构契约。

## 在哪里记录日志

最干净的默认做法，是在那些拥有足够上下文、能让事件在运维上真正有用的边界记录日志。

这通常意味着请求边界、后台作业监督者、启动入口点和外层重试循环。通常不意味着每个察觉失败的辅助函数。日志打得太早，会丢掉上下文；每层都打，会制造重复噪音；直到进程死掉才打，则会失去证据。

核心规则很简单：那个决定“这个失败在运维上意味着什么”的层，通常就是记录日志的正确位置。

这条规则与 `expected` 风格的边界和异常转换都配合得很好。底层保留信息。边界层分类、附加上下文、决定恢复方式，并只发出一次事件。

## 契约违反不只是另一条错误路径

有些失败表示程序收到了坏输入，另一些失败表示程序打破了自己的假设。

如果某个本应更早被强制的不变量此刻为假，或者触达了一个按理说不可达的状态，那么假装这只是另一种可恢复业务错误，往往是在掩盖更深层的 bug。这并不总是要求立刻终止进程，但确实要求以不同于常规验证失败的方式对待它。

一个良好的代码库会把这些区分显式化。输入失败就被建模为输入失败。后端不可用被建模为环境失败。内部不变量破坏则被当作 bug 暴露出来，而不是被规范化进普通的“操作失败”代码路径。

## 验证与评审

失败处理应当作为系统属性来评审，而不是把每个函数孤立地看。

有用的评审问题：

1. 在这个边界上，哪些失败是预期的，并且与决策相关？
2. 异常是在层内为清晰性服务，还是在层间不可预测地泄漏？
3. `expected` 承载的是真正的决策信息，还是仅仅把异常换成了样板代码？
4. 后端特定失败是在何处被转换成稳定类别的？
5. 日志是否只在那个拥有足够上下文、因而真正有用的层记录一次？

测试应当刻意覆盖非快乐路径。解析无效输入。模拟超时和 not-found。验证后端失败到面向领域失败的转换。演练启动失败路径，以及显式 `close` 或 `commit` 操作。一个只测试快乐路径行为的代码库，迟早会在生产环境中发现自己真正的错误模型。

## 要点

- 按层和边界选择错误传递方式，而不是按意识形态选择。
- 在展开和局部清晰性有帮助的地方使用异常，特别是层内部和构造期间。
- 在调用方必须基于失败做出显式决策的地方使用 `std::expected`。
- 在依赖边界附近，把不稳定的后端错误转换成稳定的、与决策相关的类别。
- 在理解失败运维含义的那一层记录日志。

如果调用方无法判断失败了什么、是否已经被记录，以及接下来自己该做什么，那么失败边界的形状就是糟糕的。这在演变成故障之前，就已经是设计缺陷了。
