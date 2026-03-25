# 改变设计的标准库类型

标准库最重要的时候，是当它不再只是一个工具层，而开始改变你的 API 被允许表达的含义。在生产 C++ 中，这种转变体现在一小组词汇类型上。它们让借用变得显式，把缺失与失败分开，表示闭合集合的备选项，并防止所有权从每个函数签名中泄漏出来。

本章不是对各个头文件的巡礼。问题更窄，也更有用：在 C++23 代码库中，哪些标准类型应该改变你设计普通代码的方式，而这些类型又会在什么地方变得误导或昂贵？

压力出现在边界处。一个服务从网络解析字节，把借用来的文本交给校验，构造领域值，记录部分失败，并把结果发往存储或下游服务。如果这些步骤是用原始指针、哨兵值以及以容器为中心的签名表达的，代码虽然能编译，但契约依然模糊。读者必须从实现细节中推断所有权、生命周期、可空性以及错误含义。把这些东西藏在那里，恰恰是最糟糕的做法。

## 借用类型会改变 API 形状

`std::string_view` 和 `std::span` 是现代 C++ 中最重要的日常设计类型，因为它们把访问与所有权分开。这听起来像小事，但其实不是。一旦代码库一致地采用借用类型，函数签名就不再暗示它们并不需要的分配，也不再假装拥有它们只是检查的数据。

考虑一个遥测采集层，它要解析基于文本行的记录和二进制属性 blob：

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

这个签名立刻说明了几件重要的事。

- 这个函数借用两个输入。
- 文本输入不要求以 null 终止。
- 二进制输入是连续的只读序列。
- 解析结果的所有权只通过返回值转移。
- 失败和缺失不是一回事。

更早期的替代写法会模糊这些表述。`const std::string&` 暗示某处存在字符串所有权，即使调用方持有的其实只是更大 buffer 中的一个切片。`const std::vector<std::byte>&` 无端排除了栈 buffer、`std::array`、内存映射区域和 packet 视图。`const char*` 则悄悄重新引入了生命周期歧义和 C 字符串假设。

要更具体地看出差异，可以看一眼借用类型出现之前，同一个边界长什么样：

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

“指针加长度”版本在类型系统中完全得不到连续性、只读访问，甚至“这个二进制 buffer 表示字节而不是字符”这一事实的支持。每个调用方都必须为每个参数手工跟踪两个原始值，而一个参数错位 bug（把 `attr_len` 传到原本期望 `line_len` 的位置）也会悄无声息地通过编译。容器引用版本则强迫每个调用方都分配一个 `std::string` 和一个 `std::vector`，即使数据本来就位于内存映射文件或栈 buffer 中。这两个版本都无法通过类型系统传达所有权契约。

借用类型确实要求纪律。`std::string_view` 只有在其源对象仍然存活且未变更时才是安全的。`std::span` 只有在被引用的存储仍然有效时才是安全的。这不是这些类型的弱点，而是它们的目的。它们强迫边界在类型系统中明确表述：这是借用关系，而不是所有权转移。

失败模式是：在生命周期保证只在局部成立时，把借用保存下来。

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

这不是回避 `std::string_view` 的理由。这是要求你只把它用于参数、局部算法拼接，以及那些生命周期契约显而易见且可评审的返回类型。如果对象需要保留数据，就存 `std::string`。如果某个子系统需要稳定的二进制所有权，就存容器或专用的 buffer 类型。

在实践中，一个好的评审问题很简单：这个对象只是检查调用方拥有的数据，还是需要保留它？如果是后者，那么把借用类型放进存储里就已经很可疑了。

## `optional`、`expected` 和 `variant` 解决的是不同问题

当团队把某一种词汇类型当成通用答案时，生产代码就会变得昂贵。`std::optional`、`std::expected` 和 `std::variant` 都在建模不同的语义。在它们之间做选择，是设计决策，不是风格偏好。

当值的缺失是正常情况，并且这种缺失本身不是错误时，用 `std::optional<T>`。缓存查找可能 miss。配置覆盖项可能未设置。一个 HTTP 请求可能带有幂等键，也可能没有。如果调用方预期只是根据“是否存在”分支，而不需要进一步解释，`optional` 就是正确的信号。

当失败信息会影响控制流、日志或用户可见行为时，用 `std::expected<T, E>`。解析、校验、协议协商以及边界 I/O 通常属于这里。从这些操作返回 `optional` 会丢掉失败原因，并迫使你用边信道提供诊断信息。

当结果是若干有效领域状态之一，而不是成功/失败二元组时，用 `std::variant<A, B, ...>`。一个消息系统可能把命令建模为若干 packet 形状之一。调度器可能把工作表示为 `std::variant<TimerTask, IoTask, ShutdownTask>`。这不是失败；这是一个显式的闭合集合。

错误在于把这些类型当成“围绕不确定性的一层可互换包装”。

- `optional` 用于“也许有”。
- `expected` 用于“成功，或者附带解释的失败”。
- `variant` 用于“若干有效形式之一”。

一旦你这样表述，很多 API 争论就会很快结束。

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

这些写法每一种都迫使调用方记住一套非正式协议。像 `-1` 或 `nullptr` 这样的哨兵在类型系统中是不可见的；没有任何东西能阻止调用方把哨兵值拿去做算术。输出参数颠倒了数据流，使链式调用变得别扭。有了 `std::optional<int>`，类型本身就携带了“可能缺失”的语义，而编译器会帮助强制检查。

在 `std::variant` 之前，闭合集合的备选项通常是用 `union`、一个 enum 判别字段和手工纪律来建模的：

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

`union` 持有数据，但语言本身并不保证 `kind` 和当前激活的成员保持同步。增加一个新备选项时，必须手工更新每一个 `switch` 位置，而编译器也不一定会警告缺失分支。`std::variant` 让当前激活的备选项成为类型运行时状态的一部分，在重新赋值时会正确析构旧值，而 `std::visit` 则提供了一种模式，使编译器能够在分支缺失时发出警告。

假设一个配置加载器可能找不到覆盖项，可能解析出一个有效覆盖项，也可能拒绝格式错误的输入。这三种结果在语义上是不同的。把它们硬塞进 `optional<Config>` 会丢掉“为什么格式错误输入会被拒绝”的原因。返回 `expected<optional<Config>, ConfigError>` 看起来也许重一点，但它准确陈述了契约：缺失是正常情况，格式错误输入是失败。

同样的精确性在服务边界上也很重要。如果一个内部客户端库返回 `variant<Response, RetryAfter, Redirect>`，调用方就可以对合法的协议结果做模式匹配。如果它返回的是 `expected<Response, Error>`，那么重试和重定向即使属于预期控制流，也会被误分类成错误路径。

`expected` 也会改变异常策略。在那些很少使用异常、或者禁止异常跨越某些边界的代码库中，`expected` 让失败保持局部且显式，而不必退化为状态码和输出参数。但这里有真实的权衡：把 `expected` 贯穿到每个私有辅助函数，会让直线式代码变成重复的传播逻辑。把它保留在错误信息确实重要的边界上。在紧凑的实现内部，一个局部异常边界，或者更小的辅助函数拆分，仍然可能产生更干净的代码。

## 容器不应该假装自己是契约

C++ 代码中最顽固的设计错误之一，是在函数其实只需要一个序列时，把拥有型容器当作参数类型。签名里的 `std::vector<T>` 很少是一个中立选择。它会表明分配策略、连续性和调用方表示形式。有时这是有意的，但更多时候只是偶然。

如果一个函数消费只读序列，就接受 `std::span<const T>`。如果它需要对调用方连续存储的一个可变视图，就接受 `std::span<T>`。如果它需要所有权转移，就显式接受拥有型类型。如果它需要某个特定的关联容器，因为查找复杂度或键稳定性是契约的一部分，那就直接说明。

这一区分在库里尤其重要。一个压缩库如果暴露 `compress(const std::vector<std::byte>&)`，就等于悄悄告诉每个调用方应当如何存储输入 buffer。更好的边界几乎总是一个借用的字节 range，通常是 `std::span<const std::byte>`。至于拥有方式，则留给调用方决定：池化 buffer、内存映射文件区域、栈数组还是 vector。

反过来的错误，是在函数实际在生成拥有型数据时返回视图。一个解析器如果构造了局部 vector，却返回 `std::span<const Header>`，那就是错的。返回 `std::vector<Header>` 或一个拥有型领域对象才是对的。借用类型在描述真实情况时会改进 API；如果用它们只是为了规避契约本来就要求的一次拷贝，它们只会把 API 变得更差。

这里还有一个修改性问题。传入一个可变容器，往往会暴露出远多于算法实际需要的自由度。一个只会追加解析记录的函数，不应该接受整个可变 map，如果真实契约只是“输出插入”。在这些情况下，应考虑更窄的抽象：回调 sink、专门的 appender 类型，或下一章讨论的受约束泛型接口。类型应表达被调用方被允许假设什么，而不只是“什么东西能编译”。

## 一个真实边界：在不漂移契约的情况下解析

一个从 socket 采集类 protobuf frame 的原生服务，通常有三个不同的层：

1. 拥有 buffer 并重试读取的传输层。
2. 借用字节并校验 framing 的解析器。
3. 拥有规范化值的领域层。

标准库类型应当强化这些层，而不是模糊它们。

传输层可能暴露拥有型 frame 存储，因为它必须管理部分读取、容量复用和背压。解析器通常应接受 `std::span<const std::byte>`，因为它检查调用方拥有的字节，并且要么产出一个领域对象，要么返回一个解析错误。领域层则应返回普通值，而不是返回指向 packet buffer 的 span，因为业务逻辑不应该意外继承传输层的生命周期。

这些话写出来时似乎很明显。但当一次以性能为导向的重构开始把 `string_view` 和 `span` 更深地穿进系统“以避免拷贝”时，它们就没那么明显了。那次拷贝有时恰恰是把一个稳定的领域对象与易变的传输 buffer 解耦的成本。去掉它，可能会把成本转移到生命周期复杂性、延迟暴露的解析 bug，以及评审难度上。

一个有用的规则是：在检查边界上借用，在语义边界上拥有。解析代码常常正坐落在这两者的交界处。

## 这些类型在什么地方会伤人

词汇类型只有在语义保持清晰时才会改进代码。

当开发者把 `std::string_view` 当成廉价字符串替代品，而不是借用时，它会伤人。当代码真正需要的是非连续遍历或稳定所有权时，`std::span` 会伤人。当 `std::optional` 抹掉了工作失败的原因时，它会伤人。当备选集合是开放的，或者经常跨模块扩展时，`std::variant` 会伤人。当 `std::expected` 被深埋在实现代码内部，而一个局部异常边界或更简单的辅助函数拆分会更清晰时，它也会伤人。

另一个常见失败模式，是一层层套包装类型，直到 API 不再像人类语言。像 `expected<optional<variant<...>>, Error>` 这样的类型偶尔确实正确，但它们对读者来说从不廉价。如果一个契约需要这么多解码工作，那么通常早就该引入一个具名领域类型了。

词汇类型的目的，不是在任何语法代价下追求最大精度。它们的目的，是让主导语义足够明显，使评审者无需逆向实现，也能推理所有权、缺失和失败。

## 验证与评审

这里的验证负担主要是契约层面的。

- 把被存储的 `string_view` 和 `span` 成员当作潜在生命周期 bug 来评审。
- 用短生命周期 buffer、切片输入、空输入和格式错误 payload 测试解析器和边界 API。
- 检查 `optional` 结果是否悄悄丢弃了在运维上重要的错误。
- 审计容器参数里是否夹带了偶然的所有权或表示形式承诺。
- 把从借用视图到拥有值的转换视为有意义的设计点，而不是偶然的实现细节。

Sanitizer 很有帮助，尤其当借用视图跨越异步或延迟执行边界时，但它们不能替代 API 评审。许多误用模式在变成动态可观测问题之前，逻辑上就已经错了。

## 要点

- 在检查边界优先使用借用类型，在存储边界优先使用拥有型类型。
- 用 `optional`、`expected` 和 `variant` 表达三种不同含义：缺失、失败和闭合备选项。
- 不要让容器把表示形式选择泄漏进 API，除非那种表示形式本身就是契约的一部分。
- 如果去掉一次拷贝会把生命周期复杂性推给无关层次，那么它并不会自动成为收益。
- 当词汇类型不再让契约更易读时，不要继续堆包装，改为引入具名领域类型。
