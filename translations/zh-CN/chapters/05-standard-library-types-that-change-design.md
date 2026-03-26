# 改变设计的标准库类型

标准库真正重要的时刻，不是充当工具层，而是它开始改变 API 所能表达的含义。在生产级 C++ 中，这种转变集中体现在一小组词汇类型上。它们让借用关系变得显式，把缺失和失败区分开来，表达封闭的备选集合，也避免了所有权信息散落在函数签名中。

本章不打算逐一浏览头文件。要讨论的问题更聚焦，也更实用：在 C++23 代码库中，哪些标准类型应该改变你编写日常代码的设计方式？这些类型又会在哪些场景下产生误导或带来额外开销？

问题在边界处最为突出。一个典型服务的流程是：从网络解析字节，将借来的文本交给校验环节，构造领域值，记录局部失败，最后将结果写入存储或发往下游服务。如果这些步骤全靠裸指针、哨兵值和重容器的函数签名来表达，代码虽然能编译，但契约仍然是模糊的。读者不得不从实现细节中去猜测所有权、生命周期、可空性和错误含义——而这些信息恰恰最不应该被藏在实现里。

## 借用类型会改变 API 形状

`std::string_view` 和 `std::span` 是现代 C++ 中最重要的日常设计类型，因为它们将访问与所有权分离。这看似微不足道，实则影响深远。一旦代码库统一采用借用类型，函数签名就不会再暗示不必要的内存分配，也不会再假装拥有那些它只是查看一下的数据。

以一个遥测数据采集层为例，它需要解析按行组织的文本记录和二进制属性 blob：

```cpp
struct MetricRecord {
std::string name;
std::int64_t value;
std::vector<std::byte> attributes;
};

auto parse_metric_line(std::string_view line,
   std::span<const std::byte> attribute_bytes)
-> std::expected<MetricRecord, ParseError>;
```

这个签名一目了然地传达了几件重要的事：

- 函数借用了两个输入，不取得所有权。
- 文本输入不要求以 null 结尾。
- 二进制输入是一段连续的只读序列。
- 解析结果的所有权仅通过返回值转移。
- 失败与缺失是两码事。

而老式的替代写法会模糊这些信息。`const std::string&` 暗示某处持有字符串的所有权，即便调用方手里的只是更大 buffer 中的一个切片。`const std::vector<std::byte>&` 毫无理由地排斥了栈 buffer、`std::array`、内存映射区域和 packet 视图。`const char*` 则悄悄带回了生命周期歧义和 C 风格字符串的隐含假设。

为了更直观地感受差异，来看看在借用类型出现之前，同一个边界是怎么写的：

```cpp
// Pre-C++17: raw pointer + length, no type safety on the binary side
auto parse_metric_line(const char* line, std::size_t line_len,
                       const unsigned char* attr_bytes, std::size_t attr_len,
                       MetricRecord* out_record) -> int; // 0 = success, -1 = error

// Or the "safe" version that forces callers into specific containers
auto parse_metric_line(const std::string& line,
                       const std::vector<unsigned char>& attribute_bytes)
    -> MetricRecord; // throws on failure, no way to distinguish absence from error
```

“指针加长度”版本完全没有类型系统层面的支持——无论是连续性、只读访问，还是”这段二进制 buffer 表示的是字节而非字符”这一事实。调用方必须为每个参数手动维护两个裸值，而参数错位 bug（比如把 `attr_len` 传到本应是 `line_len` 的位置）也能悄无声息地通过编译。容器引用版本则强迫所有调用方都分配 `std::string` 和 `std::vector`，哪怕数据本来就在内存映射文件或栈 buffer 里。两种版本都无法通过类型系统表达所有权契约。

借用类型确实需要使用者自律。`std::string_view` 仅在其数据源仍然存活且未被修改时才安全；`std::span` 仅在其引用的存储仍然有效时才安全。这不是缺陷，恰恰是设计意图——它们迫使接口在类型系统中明确声明：这里发生的是借用，而非所有权转移。

典型的出错方式是：生命周期保证仅在局部范围内成立，却把借用存了下来。

```cpp
class RequestContext {
public:
void set_tenant(std::string_view tenant) {
tenant_ = tenant; // BUG: borrowed view may outlive caller storage
}

private:
std::string_view tenant_;
};
```

这不是回避 `std::string_view` 的理由，而是说明应该把它的使用范围限制在：函数参数、局部算法中的临时拼接，以及生命周期契约一目了然、便于评审的返回类型。如果对象需要持久保存数据，就用 `std::string`。如果子系统需要稳定的二进制数据所有权，就用容器或专用 buffer 类型来存储。

示例项目清晰地演示了这种"借用到拥有"的转换。在 `examples/web-api/src/modules/task.cppm` 中，`Task::from_json` 接受一个 `string_view` 来借用原始 JSON body，但返回的 `optional<Task>` 的 `std::string` 成员独立拥有自己的数据：

```cpp
[[nodiscard]] static std::optional<Task> from_json(std::string_view sv);
```

函数从借来的输入中提取字段值，move 进拥有型字符串，返回一个自包含的 `Task`。调用方的 buffer 在函数返回后可以立即复用或销毁。这正是前文所述的模式：检查时借用，存储时拥有。

实践中有一个简单好用的评审准则：这个对象只是看一下调用方的数据，还是需要留下来长期持有？如果是后者，用借用类型做成员变量就已经值得警惕了。

## `optional`、`expected` 和 `variant` 解决的是不同问题

把某一种词汇类型当作万能方案，生产代码的维护成本就会迅速攀升。`std::optional`、`std::expected` 和 `std::variant` 各自建模的语义截然不同。在它们之间做选择是设计决策，不是风格偏好。

当值的缺失属于正常情况、本身不构成错误时，用 `std::optional<T>`。缓存查找可能未命中，配置覆盖项可能没有设置，HTTP 请求可能带幂等键、也可能不带。如果调用方只需根据”有还是没有”来分支，无需额外解释原因，`optional` 就是正确的选择。

当失败信息对控制流、日志记录或用户可见行为有实际影响时，用 `std::expected<T, E>`。解析、校验、协议协商以及边界 I/O 通常属于这一类。如果这些操作返回 `optional`，失败原因就被丢掉了，只能另辟蹊径来传递诊断信息。

当结果是若干合法领域状态中的一种，而非成功与失败的二元对立时，用 `std::variant<A, B, ...>`。消息系统可能将命令建模为若干种 packet 形态之一；调度器可能用 `std::variant<TimerTask, IoTask, ShutdownTask>` 来表示不同类型的工作。这不是失败，而是一个显式的封闭集合。

常见的错误是把这三者混为一谈，当成”不确定性的通用包装”。

- `optional` 用于“也许有”。
- `expected` 用于“成功，或者附带解释的失败”。
- `variant` 用于“若干有效形式之一”。

示例项目直观地体现了这种区分。在 `examples/web-api/src/modules/repository.cppm` 中，一次可能找不到结果的查找使用 `optional`：

```cpp
[[nodiscard]] std::optional<Task> find_by_id(TaskId id) const;
```

没有需要报告的错误——task 要么存在，要么不存在。如果这里返回 `expected`，就会迫使调用方检查一个它根本无法采取行动的错误。`optional` 才是"普通缺失"的正确信号。

把定位理清楚之后，很多 API 层面的争论就迎刃而解了。

### 这些类型出现之前，设计是什么样的

在 `std::optional` 之前，“可能有一个值”的标准习惯用法是哨兵值或输出参数：

```cpp
// Sentinel: -1 means "not found." Every caller must know the convention.
int find_port(const Config& cfg); // returns -1 if unset

// Out-parameter: success indicated by bool return, value written through pointer.
bool find_port(const Config& cfg, int* out_port);

// Nullable pointer: caller must check for null, and ownership is ambiguous.
const Config* find_override(std::string_view key); // null means absent... or error?
```

每一种写法都迫使调用方记住一套口头约定。`-1` 或 `nullptr` 之类的哨兵值在类型系统中毫无痕迹，没有任何机制能阻止调用方拿哨兵值去做算术运算。输出参数颠倒了数据流方向，让链式调用变得很别扭。而 `std::optional<int>` 把”可能缺失”的语义编码进了类型本身，编译器也能帮你强制执行检查。

在 `std::variant` 出现之前，封闭备选集合通常靠 `union` 加 enum 判别字段再加人工纪律来实现：

```cpp
// C-style tagged union: no automatic destruction, no compiler-checked exhaustiveness
enum class ValueKind { Integer, Float, String };

struct Value {
    ValueKind kind;
    union {
        std::int64_t as_int;
        double as_float;
        char as_string[64]; // fixed buffer, truncation risk
    };
};

void process(const Value& v) {
    switch (v.kind) {
    case ValueKind::Integer: /* ... */ break;
    case ValueKind::Float:   /* ... */ break;
    // Forgot String? Compiles fine. UB at runtime if String arrives.
    }
}
```

`union` 虽然能存放数据，但语言本身并不保证 `kind` 与当前激活成员保持同步。每新增一个备选项，就得手动更新所有 `switch` 分支，而编译器也未必会对遗漏发出警告。`std::variant` 将当前激活的备选项纳入类型的运行时状态，重新赋值时自动正确析构旧值；配合 `std::visit`，编译器还能在分支遗漏时给出警告。

假设一个配置加载器有三种可能的结果：没找到覆盖项、解析出了有效覆盖项、或者拒绝了格式错误的输入。三者在语义上截然不同。把它们硬塞进 `optional<Config>` 就丢掉了”格式错误为何被拒绝”的信息。返回 `expected<optional<Config>, ConfigError>` 看起来有点重，但它精确表述了契约：缺失是正常的，格式错误才是失败。

在服务边界上，同样的精确性也至关重要。如果内部客户端库返回 `variant<Response, RetryAfter, Redirect>`，调用方就可以对合法的协议结果做模式匹配。而如果返回的是 `expected<Response, Error>`，那么重试和重定向就算属于正常控制流，也会被错误地归入异常路径。

示例项目在领域边界上采用了这一方式。在 `examples/web-api/src/modules/error.cppm` 中，一个项目级的类型别名让这一模式在整个代码库中保持一致：

```cpp
template <typename T>
using Result = std::expected<T, Error>;
```

然后在 `examples/web-api/src/modules/repository.cppm` 中，可能因有意义的原因而失败的创建操作返回 `Result<Task>`：

```cpp
[[nodiscard]] Result<Task> create(Task task);
```

如果校验拒绝了输入，调用方会收到一个包含错误码和人类可读详情的 `Error`——而非一个光秃秃的 `false` 或空 optional。`examples/web-api/src/modules/handlers.cppm` 中的 `create_task` handler 在边界处把 `Result` 翻译为 HTTP 响应，无需 out 参数或异常处理：

```cpp
auto result = repo.create(std::move(*parsed));
return result_to_response(result, 201);
```

`expected` 也会改变异常策略。在那些很少使用异常、或禁止异常跨越特定边界的代码库中，`expected` 让错误处理保持局部化和显式化，无需退化到状态码加输出参数的老路子。不过这里存在真实的取舍：如果把 `expected` 一路传递到每个私有辅助函数，原本简洁的直线式代码就会变成重复的错误传播样板。应该把它留在错误信息确实重要的边界上。在封闭的实现内部，局部异常边界或更细粒度的辅助函数拆分，往往能写出更清晰的代码。

## 容器不应该假装自己是契约

C++ 代码中最顽固的设计错误之一，是函数明明只需要一个序列，参数类型却用了拥有型容器。签名中出现 `std::vector<T>` 很少是无意的中立选择——它隐含了分配策略、连续性和调用方的数据表示方式。有时这确实是故意的，但更多时候纯属偶然。

如果函数只读取序列，就接受 `std::span<const T>`；如果需要对调用方的连续存储做可变访问，就接受 `std::span<T>`；如果需要接管所有权，就显式使用拥有型类型；如果需要特定关联容器（因为查找复杂度或键稳定性本身就是契约的一部分），那就直接写明。

这一区分在库设计中尤为关键。如果压缩库暴露的接口是 `compress(const std::vector<std::byte>&)`，就等于暗中替所有调用方决定了输入 buffer 的存储方式。更好的边界几乎总是一个字节借用 range，通常是 `std::span<const std::byte>`。怎么持有数据，留给调用方自己决定——可以是池化 buffer、内存映射文件区域、栈数组或 vector。

反方向的错误同样常见：函数实际上生成了拥有型数据，返回类型却是视图。解析器内部构造了局部 vector，却返回 `std::span<const Header>`——这就是错的。正确做法是返回 `std::vector<Header>` 或拥有型领域对象。借用类型在如实反映数据关系时能提升 API 质量；如果只是用来逃避契约本就要求的那次拷贝，反而会让 API 变差。

还有一个关于可变性的问题。传入可变容器，往往暴露了远超算法实际所需的操作权限。一个只做追加的函数，没有理由接受整个可变 map——如果真正的契约只是”往里面插入输出”。遇到这种情况，应考虑更窄的抽象：回调 sink、专门的 appender 类型，或下一章讨论的受约束泛型接口。类型应该表达被调用方可以做什么假设，而不仅仅是”什么写法碰巧能编译通过”。

## 一个真实边界：解析，同时不让契约走样

一个从 socket 接收类 protobuf frame 的原生服务，通常分为三个层次：

1. 传输层——拥有 buffer，负责重试读取。
2. 解析器——借用字节，校验 framing。
3. 领域层——拥有经过规范化的值。

标准库类型应当强化这些层次划分，而不是模糊它们。

传输层可能需要暴露拥有型 frame 存储，因为它要管理部分读取、容量复用和背压。解析器通常应接受 `std::span<const std::byte>`——它查看的是调用方拥有的字节，产出要么是领域对象，要么是解析错误。领域层则应返回普通值而非指向 packet buffer 的 span，因为业务逻辑不应无意间继承传输层的生命周期。

写出来的时候，这些道理似乎显而易见。但一旦某次以性能为名的重构开始把 `string_view` 和 `span` 往系统更深处渗透——“为了省掉拷贝”——就没那么显而易见了。那次拷贝有时候恰恰是将稳定的领域对象与易变的传输 buffer 解耦的必要代价。省掉它，可能只是把成本转嫁到了生命周期复杂性、延迟暴露的解析 bug 和更高的评审难度上。

一条实用的原则是：检查边界用借用，语义边界用拥有。而解析代码恰好坐落在两者的交汇点上。

## 这些类型在哪里会帮倒忙

词汇类型只有在语义保持清晰时才能改进代码。

把 `std::string_view` 当廉价字符串替代品而非借用来用，就会出问题。代码真正需要非连续遍历或稳定所有权时，`std::span` 反而添乱。`std::optional` 一旦抹掉了失败原因，就变成了信息黑洞。备选集合是开放式的、或经常跨模块扩展时，`std::variant` 会越用越痛苦。而 `std::expected` 如果被深埋在实现内部——本来用局部异常边界或简单的函数拆分就能写得更清楚——它也只会增加噪音。

另一个常见的失败模式是包装类型层层嵌套，直到 API 变得难以阅读。像 `expected<optional<variant<...>>, Error>` 这样的类型偶尔确实是正确的，但对读者来说永远不轻松。如果理解一个契约需要这么多"解包"工作，通常说明早就该引入一个具名领域类型了。

词汇类型的意义不在于不计语法代价地追求最大精度，而在于让核心语义一目了然——评审者无需反推实现，就能看懂所有权、缺失和失败的处理方式。

## 验证与评审

这里的验证重点主要在契约层面：

- 将存储在成员变量中的 `string_view` 和 `span` 视为潜在的生命周期 bug 加以审查。
- 用短生命周期 buffer、截断输入、空输入和格式错误的 payload 来测试解析器和边界 API。
- 检查 `optional` 返回值是否在悄悄吞掉运维层面需要关注的错误信息。
- 审查容器参数是否无意间绑定了特定的所有权模式或数据表示。
- 将借用视图到拥有值的转换当作有意义的设计决策点来对待，而非偶然的实现细节。

Sanitizer 很有帮助，特别是当借用视图跨越异步或延迟执行边界时。但它们无法替代 API 评审——许多误用模式在运行时暴露之前，从逻辑上就已经错了。

## 要点

- 检查边界优先用借用类型，存储边界优先用拥有型类型。
- `optional`、`expected` 和 `variant` 对应三种不同语义：缺失、失败和封闭备选集合，不要混用。
- 不要让容器的实现选择泄漏到 API 中，除非该实现本身就是契约的一部分。
- 省掉一次拷贝如果会把生命周期复杂性推给无关层次，那就算不上收益。
- 当词汇类型的嵌套让契约变得更难读时，该引入具名领域类型了，不要继续堆包装。
