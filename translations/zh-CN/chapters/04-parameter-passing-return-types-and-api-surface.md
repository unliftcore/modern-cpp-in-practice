# 参数传递、返回类型与 API 接口设计

在 C++ 中，函数签名承载的信息往往超出作者的本意。它说明了被调用方是借用还是保留数据，通常还隐含着 null 是否有意义、能否修改、会不会发生拷贝、失败算不算正常控制流等语义。签名一旦传达了错误的语义，哪怕实现本身在局部是正确的，API 用起来仍然代价高昂，审查起来仍然困难。

本章聚焦的是这层语义接口。目标不是死记”X 一律按 Y 传递”之类的教条，而是学会选择恰当的参数和返回形式，在调用方需要做决策的边界上，如实传达所有权（ownership）、生命周期（lifetime）、可变性、可空性与开销。

第 1 章讨论了谁拥有资源，第 2 章讨论了模型中有哪些对象，第 3 章讨论了错误应当如何跨越边界。本章的问题更具体：在这些设计决策已经确定的前提下，函数签名该怎么写，才能让调用契约一目了然？

## 签名是契约，不是类型检查仪式

很多糟糕的 C++ API，根源在于把签名仅仅看作”能通过编译的最小类型集合”。参数和返回值的选择本身就是带有约束力的文档。

以一个解析器边界为例。

```cpp
auto parse_frame(std::span<const std::byte> bytes)
    -> std::expected<Frame, ParseError>;
```

仅这一行就传达了几层含义：

- 函数借用一段连续的只读字节。
- 不需要获取源 buffer 的所有权。
- 产出一个拥有所有权的 `Frame` 值。
- 失败是预期内的，且显式表达。

对比 `Frame parse_frame(const std::vector<std::byte>&);`，后者强加了解析器根本不需要的容器选择，隐藏了失败策略，也没有说明返回的 `Frame` 究竟借用了输入数据的视图，还是持有独立的数据副本。

示例项目中的 HTTP 解析器遵循了同样的模式。在 `examples/web-api/src/modules/http.cppm` 中，`parse_request` 借用输入并返回一个拥有所有权的结果：

```cpp
[[nodiscard]] inline std::optional<Request>
parse_request(std::string_view raw);
```

函数接受一个指向栈 buffer 的 `string_view`，解析出 method、path、headers 和 body，返回一个成员全部为 `std::string` 的 `Request`——完全拥有自己的数据。调用方的 buffer 函数返回后即可复用或销毁。这种"借用输入、拥有输出"的契约，仅从签名就能看清。

差别在于调用方能否不翻看实现就理解契约。

## 借用参数应当看起来就是借用

如果函数在调用期间只读取调用方的数据、不做保留，签名就应该直接体现借用语义。

对于文本，当不需要 null 终止符、也不涉及所有权转移时，`std::string_view` 通常是最合适的参数类型。对于连续的二进制或元素序列，`std::span<const T>` 是常见的只读形式。对于可变借用，`std::span<T>` 或非 const 引用都可以，取决于抽象对象更像序列还是更像独立实体。

这样做有两个好处：

1. 调用方保持灵活——可以传入字符串、切片、数组、vector、内存映射 buffer 等，无需强制分配或转换容器。
2. 契约诚实透明——借用就是借用。

最常见的误用是让借用参数泄漏到长期状态中。一个接受 `string_view` 却把它缓存到调用结束之后的函数，是在对契约撒谎。

### 悬空借用：把这件事做错的代价

当借用参数的生存期超过了源对象，就会引发未定义行为——而且往往表现为间歇性的数据损坏，而非一次干脆利落的崩溃：

```cpp
class Logger {
public:
    void set_prefix(std::string_view prefix) {
        prefix_ = prefix; // BUG: stores a view, not a copy
    }

    void log(std::string_view message) {
        fmt::print("[{}] {}\n", prefix_, message); // reads dangling view
    }

private:
    std::string_view prefix_; // non-owning -- lifetime depends on caller
};

void configure_logger(Logger& logger) {
    std::string name = build_service_name();
    logger.set_prefix(name); // name is destroyed at end of scope
} // name destroyed here -- logger.prefix_ is now dangling
```

修复方法很简单：如果成员的生存期需要超过单次调用，就必须持有数据的所有权。

```cpp
class Logger {
public:
    void set_prefix(std::string prefix) { // takes ownership by value
        prefix_ = std::move(prefix);
    }
    // ...
private:
    std::string prefix_; // owning -- no lifetime dependency on caller
};
```

由此得出一条简洁实用的审查准则：凡是参数类型标明了借用，实现中若要保留数据，就必须通过显式拷贝或转换为拥有类型来完成。

## 当被调用方反正需要自己的副本时，就按值传递

现代 C++ 中一个实用的模式是：被调用方需要存储或拥有参数时，直接按值传递。对于被灌输了”拷贝能省则省”观念的开发者来说，这可能令人意外。

以一个需要存储租户名的请求对象为例：

```cpp
class RequestContext {
public:
    explicit RequestContext(std::string tenant)
        : tenant_(std::move(tenant)) {}

private:
    std::string tenant_;
};
```

这个构造函数通常优于 `const std::string&` 和 `std::string_view`。

- 明确表达了对象会拥有一份字符串。
- 传入左值时付出一次拷贝——但这本来就无法避免。
- 传入右值时可以直接 move。
- 不会引发误将借用视图意外保留的风险。

准则不是”昂贵类型一律按 const 引用传递”，而是”当所有权转移本身就是契约的一部分，且额外的 move/copy 开销可以接受时，按值传递即可。”

### 错误的参数选择及其成本

参数传递选错了，后果未必立竿见影，但在热路径和大对象场景下不断累积。

**当需要所有权时，`const std::string&` 导致不必要的拷贝：**

```cpp
class Registry {
public:
    void register_name(const std::string& name) {
        names_.push_back(name); // always copies, even if caller passed a temporary
    }
private:
    std::vector<std::string> names_;
};

// Caller:
registry.register_name(build_name()); // builds a temporary string, copies it,
                                      // then destroys the temporary. The move
                                      // that pass-by-value would have enabled
                                      // is lost.
```

改用”按值传入 + move”的写法后，临时对象可以直接 move 进容器，免去不必要的拷贝：

```cpp
void register_name(std::string name) {
    names_.push_back(std::move(name)); // rvalue callers: 1 move. lvalue callers: 1 copy + 1 move.
}
```

示例项目的错误模块也运用了同样的手法。在 `examples/web-api/src/modules/error.cppm` 中，`make_error` 按值接收 `std::string`，然后 move 进错误对象：

```cpp
[[nodiscard]] inline std::unexpected<Error>
make_error(ErrorCode code, std::string detail) {
    return std::unexpected<Error>{Error{code, std::move(detail)}};
}
```

传入字符串字面量或临时对象的调用方零拷贝；传入左值的调用方付出一次拷贝——而这次拷贝本来就无法避免。签名诚实地传达了 `detail` 将被结果错误对象所拥有。

**当 `std::span` 已经足够时，`const std::vector<T>&` 强迫分配：**

```cpp
// Anti-pattern: forces callers to allocate a vector even if data is in an array or span.
double average(const std::vector<double>& values);

// Caller with a C array or std::array must construct a vector just to call this:
std::array<double, 4> readings = {1.0, 2.0, 3.0, 4.0};
auto avg = average(std::vector<double>(readings.begin(), readings.end())); // pointless heap allocation
```

改用 `std::span<const double>` 后，函数可以接受任意连续来源，而不必强迫调用方选择特定容器：

```cpp
double average(std::span<const double> values);

// Now works with vector, array, C array, span -- no allocation required.
auto avg = average(readings);
```

当然也有不适合按值传递的情况：多态类型、极少需要拷贝左值的超大聚合体，以及仅在特定条件下才保留参数的 API。一如既往，语义契约优先。

## 非 const 引用表达的不只是可修改性

非 const 引用参数语义很强。它意味着调用方必须提供一个存活的对象，null 没有意义，被调用方可以就地修改这个对象。有时这恰好是正确的契约，但也常常被滥用。

只有在修改是操作的核心目的，且调用方应当将其视为此次调用的主要意图时，才适合使用非 const 引用。典型场景：原地排序一个 vector、填充调用方提供的输出 buffer、推进解析器状态对象。

不要仅仅为了省掉一个返回值，或者因为 C 风格的 out 参数用着顺手，就使用非 const 引用。如果结果在概念上就是函数的输出，而不是调用方有意交出来让你修改的对象，那么 out 参数反而会降低可读性。

在现代 C++ 中，主要结果通常直接返回更为清晰。非 const 引用参数应当留给真正的原地修改场景，或者多对象协调且修改本身就是契约核心的情况。

## 裸指针主要用于可空性和互操作

裸指针在接口中仍有其正当用途。在现代 C++ 中，最清晰的用法是表示一个可选的借用对象，或用于与底层 API 互操作。

但这个角色比很多代码库实际赋予裸指针的要窄得多。

`T*` 参数通常只应表达两种含义之一：

1. 被调用方可能收到空指针——即不提供对象。
2. 接口需要跨越指针层面的互操作或底层数据结构，指针身份本身就承载语义。

如果 null 没有意义，引用通常更清晰。如果在转移所有权，`std::unique_ptr<T>` 或其他拥有类型更清晰。如果对象是数组或连续序列，`std::span<T>` 通常更清晰。一个裸指针如果同时被解读为”非空、借用、可能是单个也可能是多个、也许会被保留”，那就是语义债务。

同样的原则适用于返回类型。在现代 C++ API 中，返回拥有所有权的裸指针几乎总是错误的信号。返回一个观察者裸指针则可以接受，前提是”缺失”本身有意义，且对象的生命周期由其他机制管理。

## 返回有意义的值，而非存储的副产品

返回类型应当和参数一样严谨。核心问题是：调用方应该得到一个拥有所有权的值、一个借用访问，还是一个像 `expected` 或 `optional` 那样承载决策信息的包装类型？

对于很多 API 来说，即使涉及一次 move，返回拥有所有权的值仍然是最干净的设计。它让生命周期保持局部化，让组合更容易，也避免了调用方对内部存储的依赖。C++23 的 move 语义已经让值返回在大多数场景下足够廉价。

借用返回类型只在源对象的生命周期显而易见、足够稳定，且确实属于契约一部分时才适合。返回指向内部存储的 `std::string_view`，前提是该存储的生存期明显长于 view，且调用方可以安全地依赖这一点。在较宽的接口边界上，这通常不是好的权衡，因为它把生命周期推理的负担推给了调用方。

可选性和失败也应当在返回类型中显式表达，而不是靠哨兵值偷偷传递。搜索操作返回”也许找到了”，适合用 `std::optional<T>`，或者在生命周期语义需要时用观察者指针。解析或加载操作的失败如果关乎后续决策，适合用 `std::expected<T, E>`。失败时返回空字符串或 `-1` 的函数，通常是把 API 设计得比实际需要更弱了。

## 反模式：一个签名背后藏着多个故事

这类 API 在很多代码库中依然常见，因为它看起来很灵活。

```cpp
// Anti-pattern: signature hides ownership, failure, and buffer contract.
bool encode_record(const Record& record,
               std::vector<std::byte>& output,
               std::string* error_message = nullptr);
```

这一个函数身上就隐含着好几条未说明的规则：

- 是追加到 `output`，还是覆盖 `output`？
- `error_message` 设为可选，是因为诊断信息不重要，还是因为日志记录在别处进行？
- 失败时 `output` 是否会被部分修改？
- `false` 代表的是校验失败、编码 bug、容量不足，还是内部异常被转换了？

这些问题，签名本身一个都答不上来。

更好的做法是把语义拆分开来。

```cpp
auto encode_record(const Record& record)
    -> std::expected<std::vector<std::byte>, EncodeError>;

auto append_encoded_record(const Record& record,
                   ByteAppender& output)
    -> std::expected<void, EncodeError>;
```

现在调用方可以在”生成拥有所有权的结果”和”追加式写入”之间明确选择，失败契约也是显式的。两种本质不同的操作不再伪装成一个万能的”灵活”接口。

## 工厂和获取函数必须提前说明所有权

创建函数是所有权不清晰代价最高的地方。返回 `T*` 的工厂让调用方不得不追问：谁负责 delete？通过 out 参数加 bool 返回值的工厂，往往隐藏了部分构造的规则。默认返回 `shared_ptr<T>` 的工厂，则可能在设计尚未证明需要共享所有权时就引入了共享。

对于普通的独占所有权，`std::unique_ptr<T>` 通常是最清晰的返回类型。对于值语义的对象，直接返回值即可，如果失败发生在边界上则用 `expected<T, E>`。只有当创建出的对象确实需要共享生命周期时，才返回 `shared_ptr<T>`。

来看具体的对比：

```cpp
// Anti-pattern: raw pointer factory -- caller does not know who owns the result.
Widget* create_widget(const WidgetConfig& cfg);

void setup() {
    auto* w = create_widget(cfg);
    // Does the caller own w? Does a global registry own it?
    // Must the caller call delete? delete[]? A custom deallocator?
    // Nothing in the signature answers these questions.
    use(w);
    // If the caller guesses wrong, the result is a leak or a double-free.
}
```

```cpp
// Clear: unique_ptr states exclusive caller ownership unambiguously.
auto create_widget(const WidgetConfig& cfg)
    -> std::expected<std::unique_ptr<Widget>, WidgetError>;

void setup() {
    auto result = create_widget(cfg);
    if (!result) { /* handle error */ }
    auto widget = std::move(*result); // ownership transferred, no ambiguity
    // widget is destroyed automatically when it leaves scope
}
```

示例项目展示了面向值类型的同类模式。在 `examples/web-api/src/modules/task.cppm` 中，`Task::validate` 是一个工厂风格的函数，按值接收 `Task`，返回 `Result<Task>`（即 `std::expected<Task, Error>` 的别名）：

```cpp
[[nodiscard]] static Result<Task> validate(Task t) {
    if (t.title.empty()) {
        return make_error(ErrorCode::bad_request, "title must not be empty");
    }
    return t;
}
```

而在 `examples/web-api/src/modules/repository.cppm` 中，`TaskRepository::create` 与之配合——按值接收 `Task`，校验后分配 ID，返回存储结果或校验错误：

```cpp
[[nodiscard]] Result<Task> create(Task task);
```

两个函数都没有使用 out 参数或 bool 返回码。所有权故事与上面的 `unique_ptr` 工厂如出一辙，只是适配了值类型：调用方 move 一个值进去，拿回一个有效的拥有型结果或一个显式的错误。

具体选哪种词汇类型不是重点，重点是：创建边界恰恰是所有权必须表达得毫无歧义的地方。

## API 接口也是成本接口

签名的选择会以调用方切实感受到的方式影响开销。

`std::function` 参数即使回调只在同步场景中使用，也可能带来堆分配和类型擦除的开销。`std::span<const T>` 能避免强迫调用方转换为特定容器。按值接收 `std::string` 的 sink 构造函数可以让临时对象高效 move 进来。返回拥有所有权的 vector 虽然分配一次内存，却能消除长期的生命周期隐患。这些是设计层面的权衡，不是微优化。

正确的做法是把调用方需要知道的成本摆在明面上，避免无从推断的隐性开销。好的签名不承诺零成本，而是让重要的成本不会令人意外。

过于宽泛的”便利”重载集合反而有害。当一个 API 同时接受指针、字符串、span、vector、view 等各种组合时，重载接口本身可能比原始问题还难以理解。应当优先保留少数几个语义清晰的形式。

## 验证与评审

函数签名是发现设计缺陷成本最低的环节。

审查时可以问自己：

1. 每个参数是否如实传达了借用、所有权转移、可变性或可选性？
2. 按值传递是否用在了被调用方确实需要所有权的场合，而非出于习惯或教条？
3. 裸指针是否仅限于可选的借用访问或互操作，而非充当含糊的万能契约？
4. 返回类型是否清楚地表达了拥有所有权的结果、借用访问还是显式失败？
5. API 是否把重要的开销暴露出来，而只隐藏了无关紧要的实现细节？

测试应当验证签名所蕴含的语义，不仅仅是核心功能。要验证是追加还是覆盖行为；验证返回的 view 在文档承诺的生命周期内有效，过期后确实失效；验证失败后输出参数或状态保持在承诺的条件下。再清晰的签名，也需要测试作为证据来支撑。

## 要点

- 把签名当作语义契约来设计，而不仅仅是编译器能接受的类型组合。
- 被调用方只读取调用方数据时，使用借用参数类型。
- 被调用方需要获取所有权、且这一契约应当显而易见时，按值传递。
- 通过引用、指针和返回包装器有意识地表达可变性、可选性和失败。
- 保持 API 接口足够精简，让调用方无需翻看实现代码就能理解生命周期和开销。

优秀的 C++ API 不只是能通过编译，而是让调用方第一次阅读签名就能正确使用。
