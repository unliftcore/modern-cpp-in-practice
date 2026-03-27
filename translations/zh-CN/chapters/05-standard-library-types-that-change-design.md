# 改变设计的标准库类型

标准库真正发挥作用的地方，不是当工具层用，而是它改变了 API 能表达什么含义。在生产级 C++ 中，这体现在一小组词汇类型（vocabulary types）上。它们让借用关系变得显式，把缺失和失败区分开来，表达封闭的备选集合，避免所有权信息散落在函数签名中。

本章不逐一浏览头文件。问题更聚焦：在 C++23 代码库中，哪些标准类型应该改变你日常的设计方式？它们又在哪些场景下会产生误导或带来额外开销？

问题在边界处最突出。典型服务的流程是：从网络解析字节，将借来的文本交给校验环节，构造领域值，记录局部失败，最后写入存储或发往下游。如果这些步骤全靠裸指针、哨兵值和重容器的函数签名来表达，代码能编译，但契约是模糊的。读者只能从实现细节中猜测所有权、生命周期、可空性和错误含义，而这些信息最不该藏在实现里。

## 借用类型会改变 API 形状

`std::string_view` 和 `std::span` 是现代 C++ 中最常用的设计类型，因为它们将访问与所有权分离。一旦代码库统一采用借用类型，函数签名就不再暗示不必要的内存分配，也不再假装拥有那些只是查看一下的数据。

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

这个签名一目了然：

- 函数借用了两个输入，不取得所有权。
- 文本输入不要求以 null 结尾。
- 二进制输入是一段连续的只读序列。
- 解析结果的所有权仅通过返回值转移。
- 失败与缺失是两码事。

老式写法会模糊这些信息。`const std::string&` 暗示某处持有字符串所有权，即便调用方手里只是更大 buffer 的一个切片。`const std::vector<std::byte>&` 无理由地排斥了栈 buffer、`std::array`、内存映射区域和 packet 视图。`const char*` 则悄悄带回了生命周期歧义和 C 风格字符串的隐含假设。

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

“指针加长度”版本完全没有类型系统的支持，无论是连续性、只读访问，还是”这段二进制 buffer 是字节而非字符”。调用方必须为每个参数手动维护两个裸值，参数错位 bug（比如把 `attr_len` 传到 `line_len` 的位置）能悄无声息地通过编译。容器引用版本则强迫所有调用方分配 `std::string` 和 `std::vector`，哪怕数据本来就在内存映射文件或栈 buffer 里。两种版本都无法通过类型系统表达所有权契约。

借用类型需要使用者自律。`std::string_view` 仅在数据源仍然存活且未被修改时才安全；`std::span` 仅在其引用的存储仍然有效时才安全。这就是设计意图：它们迫使接口在类型系统中声明，这里发生的是借用而非所有权转移。

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

这不是回避 `std::string_view` 的理由，而是说它的使用范围应限于：函数参数、局部算法中的临时拼接、生命周期契约清晰且便于评审的返回类型。对象需要持久保存数据就用 `std::string`；子系统需要稳定的二进制数据所有权就用容器或专用 buffer 类型。

示例项目演示了这种"借用到拥有"的转换。在 `examples/web-api/src/modules/task.cppm` 中，`Task::from_json` 接受 `string_view` 借用原始 JSON body，但返回的 `optional<Task>` 的 `std::string` 成员独立拥有数据：

```cpp
[[nodiscard]] static std::optional<Task> from_json(std::string_view sv);
```

函数从借来的输入中提取字段值，move 进拥有型字符串，返回自包含的 `Task`。调用方的 buffer 在函数返回后可以立即复用或销毁。检查时借用，存储时拥有。

评审时有个简单准则：这个对象只是看一下调用方的数据，还是需要长期持有？如果是后者，用借用类型做成员变量就该警惕了。

## `optional`、`expected` 和 `variant` 解决的是不同问题

把某一种词汇类型当万能方案，维护成本会迅速攀升。`std::optional`、`std::expected` 和 `std::variant` 各自建模的语义不同。在它们之间做选择是设计决策，不是风格偏好。

值的缺失属于正常情况、本身不构成错误时，用 `std::optional<T>`。缓存查找可能未命中，配置覆盖项可能没有设置，HTTP 请求可能带幂等键也可能不带。调用方只需根据”有还是没有”来分支，无需额外解释原因，`optional` 就是正确的选择。

失败信息对控制流、日志记录或用户可见行为有实际影响时，用 `std::expected<T, E>`。解析、校验、协议协商和边界 I/O 通常属于这一类。如果这些操作返回 `optional`，失败原因就丢掉了，只能另辟蹊径传递诊断信息。

结果是若干合法领域状态之一，而非成功与失败的二元对立时，用 `std::variant<A, B, ...>`。消息系统可能把命令建模为若干种 packet 形态之一；调度器可能用 `std::variant<TimerTask, IoTask, ShutdownTask>` 表示不同类型的工作。这不是失败，是一个显式的封闭集合。

常见的错误是把这三者混为一谈，当成”不确定性的通用包装”。

- `optional` 用于“也许有”。
- `expected` 用于“成功，或者附带解释的失败”。
- `variant` 用于“若干有效形式之一”。

示例项目体现了这种区分。在 `examples/web-api/src/modules/repository.cppm` 中，一次可能找不到结果的查找使用 `optional`：

```cpp
[[nodiscard]] std::optional<Task> find_by_id(TaskId id) const;
```

没有需要报告的错误，task 要么存在要么不存在。如果返回 `expected`，就迫使调用方检查一个它根本无法采取行动的错误。`optional` 才是"普通缺失"的正确信号。

定位理清楚之后，很多 API 层面的争论自然消解。

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

每种写法都迫使调用方记住一套口头约定。`-1` 或 `nullptr` 之类的哨兵值在类型系统中毫无痕迹，没有机制阻止调用方拿哨兵值做算术运算。输出参数颠倒了数据流方向，链式调用很别扭。`std::optional<int>` 把”可能缺失”的语义编码进类型本身，编译器也能帮你强制检查。

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

`union` 能存放数据，但语言本身不保证 `kind` 与当前激活成员保持同步。每新增一个备选项就得手动更新所有 `switch` 分支，编译器也未必对遗漏发出警告。`std::variant` 将当前激活的备选项纳入类型的运行时状态，重新赋值时自动析构旧值；配合 `std::visit`，编译器能在分支遗漏时给出警告。

假设一个配置加载器有三种可能结果：没找到覆盖项、解析出有效覆盖项、拒绝了格式错误的输入。三者语义不同。硬塞进 `optional<Config>` 就丢掉了”格式错误为何被拒绝”的信息。返回 `expected<optional<Config>, ConfigError>` 看起来有点重，但它精确表述了契约：缺失是正常的，格式错误才是失败。

服务边界上同样如此。如果内部客户端库返回 `variant<Response, RetryAfter, Redirect>`，调用方可以对合法的协议结果做模式匹配。而如果返回 `expected<Response, Error>`，重试和重定向即便属于正常控制流，也会被错误地归入异常路径。

示例项目在领域边界上用了这种方式。在 `examples/web-api/src/modules/error.cppm` 中，一个项目级类型别名让模式在整个代码库中保持一致：

```cpp
template <typename T>
using Result = std::expected<T, Error>;
```

然后在 `examples/web-api/src/modules/repository.cppm` 中，可能因有意义的原因而失败的创建操作返回 `Result<Task>`：

```cpp
[[nodiscard]] Result<Task> create(Task task);
```

校验拒绝输入时，调用方会收到包含错误码和人类可读详情的 `Error`，而非光秃秃的 `false` 或空 optional。`examples/web-api/src/modules/handlers.cppm` 中的 `create_task` handler 在边界处把 `Result` 翻译为 HTTP 响应，无需 out 参数或异常处理：

```cpp
auto result = repo.create(std::move(*parsed));
return result_to_response(result, 201);
```

`expected` 也会改变异常策略。在很少使用异常或禁止异常跨越特定边界的代码库中，`expected` 让错误处理保持局部化和显式化，无需退化到状态码加输出参数的老路。但取舍是真实的：如果把 `expected` 一路传递到每个私有辅助函数，直线式代码会变成重复的错误传播样板。把它留在错误信息确实有用的边界上。封闭实现内部，局部异常边界或更细粒度的函数拆分，往往更清晰。

## 容器不应该假装自己是契约

C++ 代码中最顽固的设计错误之一：函数明明只需要一个序列，参数类型却用了拥有型容器。签名中出现 `std::vector<T>` 很少是中立选择，它隐含了分配策略、连续性和调用方的数据表示方式。有时是故意的，更多时候纯属偶然。

如果函数只读取序列，就接受 `std::span<const T>`；如果需要对调用方的连续存储做可变访问，就接受 `std::span<T>`；如果需要接管所有权，就显式使用拥有型类型；如果需要特定关联容器（因为查找复杂度或键稳定性本身就是契约的一部分），那就直接写明。

这一区分在库设计中尤为明显。如果压缩库暴露的接口是 `compress(const std::vector<std::byte>&)`，就等于替所有调用方决定了输入 buffer 的存储方式。更好的边界几乎总是一个字节借用 range，通常是 `std::span<const std::byte>`。怎么持有数据留给调用方决定，可以是池化 buffer、内存映射文件区域、栈数组或 vector。

反方向的错误同样常见：函数生成了拥有型数据，返回类型却是视图。解析器内部构造了局部 vector 却返回 `std::span<const Header>`，这就错了。正确做法是返回 `std::vector<Header>` 或拥有型领域对象。借用类型在如实反映数据关系时提升 API 质量；如果只是用来逃避契约本就要求的那次拷贝，反而让 API 变差。

还有可变性的问题。传入可变容器，往往暴露了远超算法所需的操作权限。一个只做追加的函数没理由接受整个可变 map，如果真正的契约只是”往里面插入输出”。遇到这种情况，应考虑更窄的抽象：回调 sink、专门的 appender 类型，或下一章讨论的受约束泛型接口。类型应该表达被调用方可以做什么假设，而非”什么写法碰巧能编译通过”。

## 一个真实边界：解析，同时不让契约走样

一个从 socket 接收类 protobuf frame 的原生服务通常分三层：

1. 传输层：拥有 buffer，负责重试读取。
2. 解析器：借用字节，校验 framing。
3. 领域层：拥有经过规范化的值。

标准库类型应当强化这些层次划分，而非模糊它们。

传输层可能需要暴露拥有型 frame 存储，因为它管理部分读取、容量复用和背压。解析器通常应接受 `std::span<const std::byte>`，查看调用方拥有的字节，产出领域对象或解析错误。领域层应返回普通值而非指向 packet buffer 的 span，因为业务逻辑不应无意间继承传输层的生命周期。

写出来时，这些道理似乎显而易见。但一旦某次以性能为名的重构开始把 `string_view` 和 `span` 往系统更深处渗透，”为了省掉拷贝”，就没那么显而易见了。那次拷贝有时候恰恰是将稳定的领域对象与易变的传输 buffer 解耦的必要代价。省掉它，可能只是把成本转嫁到了生命周期复杂性、延迟暴露的解析 bug 和更高的评审难度上。

一条实用的原则是：检查边界用借用，语义边界用拥有。而解析代码恰好坐落在两者的交汇点上。

## 这些类型在哪里会帮倒忙

词汇类型只有在语义保持清晰时才能改进代码。

把 `std::string_view` 当廉价字符串替代品而非借用来用就会出问题。代码真正需要非连续遍历或稳定所有权时，`std::span` 反而添乱。`std::optional` 一旦抹掉失败原因就变成信息黑洞。备选集合是开放式的或经常跨模块扩展时，`std::variant` 会越用越痛苦。`std::expected` 如果被深埋在实现内部，本来用局部异常边界或简单的函数拆分就能写得更清楚，它也只会增加噪音。

另一个常见问题是包装类型层层嵌套，直到 API 难以阅读。`expected<optional<variant<...>>, Error>` 这样的类型偶尔是正确的，但对读者永远不轻松。如果理解契约需要这么多"解包"工作，通常说明早该引入一个具名领域类型了。

词汇类型的意义不在于不计语法代价追求最大精度，而在于让核心语义一目了然，评审者无需反推实现就能看懂所有权、缺失和失败的处理方式。

## 验证与评审

这里的验证重点主要在契约层面：

- 将存储在成员变量中的 `string_view` 和 `span` 视为潜在的生命周期 bug 加以审查。
- 用短生命周期 buffer、截断输入、空输入和格式错误的 payload 来测试解析器和边界 API。
- 检查 `optional` 返回值是否在悄悄吞掉运维层面需要关注的错误信息。
- 审查容器参数是否无意间绑定了特定的所有权模式或数据表示。
- 将借用视图到拥有值的转换当作有意义的设计决策点来对待，而非偶然的实现细节。

Sanitizer 很有帮助，特别是借用视图跨越异步或延迟执行边界时。但它们无法替代 API 评审，许多误用模式在运行时暴露之前，从逻辑上就已经错了。

## 要点

- 检查边界优先用借用类型，存储边界优先用拥有型类型。
- `optional`、`expected` 和 `variant` 对应三种不同语义：缺失、失败和封闭备选集合，不要混用。
- 不要让容器的实现选择泄漏到 API 中，除非该实现本身就是契约的一部分。
- 省掉一次拷贝如果会把生命周期复杂性推给无关层次，那就算不上收益。
- 当词汇类型的嵌套让契约变得更难读时，该引入具名领域类型了，不要继续堆包装。
