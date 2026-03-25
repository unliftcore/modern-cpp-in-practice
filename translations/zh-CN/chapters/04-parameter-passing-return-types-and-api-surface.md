# 参数传递、返回类型与 API 表面

在 C++ 中，一个函数签名表达的信息往往比作者原本打算表达的更多。它会说明被调用方是借用还是保留数据，常常还会暗示 null 是否有意义、是否允许修改、是否可能发生拷贝，以及失败是否属于正常控制流。如果签名把这些语义表达错了，实现即使在局部上是正确的，API 仍然会昂贵且难以评审。

本章讨论的正是这种语义表面积。目标不是背下“总是把 X 通过 Y 传递”这样的口诀，而是选择那些能在调用方必须做决策的边界上，如实传达所有权、生命周期、修改性、可空性和成本的参数与返回形式。

这种边界视角让本章和前后章节有所区分。第 1 章讨论了谁拥有资源。第 2 章讨论了模型中存在哪些对象。第 3 章讨论了失败应当如何跨越边界。这里的问题更窄，也更务实：在已经做出这些设计选择之后，一个函数签名应该长什么样，才能让调用契约一目了然？

## 签名是契约，不是类型检查仪式

很多糟糕的 C++ API，源于把签名当成“能编译通过的最小类型集合”。这种做法忽略了一个事实：参数和返回值的选择，是带牙齿的文档。

以一个解析器边界为例。

```cpp
auto parse_frame(std::span<const std::byte> bytes)
-> std::expected<Frame, ParseError>;
```

这一行立刻传达了几件事。

- 这个函数借用的是连续的只读字节。
- 它不需要源 buffer 的所有权。
- 它会产出一个拥有所有权的 `Frame` 值。
- 失败是预期的，并且显式。

把它和 `Frame parse_frame(const std::vector<std::byte>&);` 对比一下。后者强加了一个解析器根本不需要的容器选择，隐藏了失败策略，也没有说明返回的 `Frame` 是包含了借用输入的视图，还是包含独立拥有的数据。

区别不在于风格打磨，而在于调用点能否在不打开实现的情况下推理契约。

## 借用参数应当看起来就是借用

如果一个函数在调用期间读取调用方拥有的数据，并且不会保留它，那么签名就应该直接表达借用。

对于文本，在 null 终止无关且不发生所有权转移时，`std::string_view` 往往是正确的参数类型。对于连续的二进制或元素序列，`std::span<const T>` 往往是正确的只读形式。对于可变借用访问，`std::span<T>` 或非常量引用都可能合适，具体取决于抽象更像序列还是更像对象。

这样做有两个好处。

1. 调用点保持灵活。它们可以传入字符串、切片、数组、vector 和映射 buffer，而不必被迫分配或转换容器。
2. 契约是诚实的。借用就是借用。

最常见的误用，是让借用参数泄漏进被保留的状态。一个接受 `string_view` 然后把它缓存到调用之外的函数，并不聪明；它是在对契约撒谎。

### 悬空借用：把这件事做错的代价

当一个借用参数活得比它的源对象更久时，结果就是未定义行为，而且往往表现为间歇性损坏，而不是一次干净的崩溃：

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

修复方式很直接：如果成员必须活得比这次调用更久，它就必须拥有自己的数据。

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

这也是为什么一个有用的评审启发式非常简单：如果参数类型表示借用，那么所有保留行为都必须在实现里以显式拷贝或转换为拥有类型的形式可见。

## 当被调用方反正需要自己的副本时，就按值传递

现代 C++ 中最有用的模式之一，就是当被调用方打算存储或以其他方式拥有参数时，按值传递。这常常会让那些受过“无论如何都要避免拷贝”训练的人感到意外。

考虑一个存储租户名的请求对象。

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

- 它诚实地表明，对象将拥有一个字符串。
- 对左值调用方来说，要付出一次拷贝，而这本来就不可避免。
- 对右值调用方来说，可以直接 move。
- 不会诱发意外保留借用视图的冲动。

规则不是“总是按常量引用传递昂贵类型”。规则是：“当所有权转移给被调用方是预期契约，并且额外的 move/copy 叙事可接受时，就按值传递。”

### 错误的参数选择及其成本

参数传递做错的代价，不一定总是戏剧性的，但它会在热路径和大对象上累积。

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

改成“按值传入再 move”后，临时对象可以直接 move 进容器，不需要拷贝：

```cpp
void register_name(std::string name) {
names_.push_back(std::move(name)); // rvalue callers: 1 move. lvalue callers: 1 copy + 1 move.
}
```

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

权衡在于：对于多态类型、很少希望拷贝左值的超大聚合体，或者保留行为只是有条件且并不常见的 API，按值传递可能是错误的。和往常一样，语义契约优先。

## 非 const 引用表达的不只是可修改性

非常量引用参数是一种很强的语法。它说明调用方必须提供一个活对象，null 没有意义，被调用方可以修改那个确切对象。这有时正是正确契约，但它也被过度使用了。

当修改是操作的核心，而且调用方理应把它理解为此次调用的主要目的时，才使用非常量引用。原地排序一个 vector、填充一个提供好的输出 buffer，或推进一个解析器状态对象，都可能符合这个条件。

不要仅仅为了避免返回一个值，或因为输出参数在 C API 中看起来很熟悉，就使用非常量引用。如果结果在概念上是函数的输出，而不是调用方刻意交给你去修改的对象，那么输出参数会削弱可读性。

在现代 C++ 中，主结果通常直接返回值会更清晰。把非常量引用参数留给真正的原地修改，或者留给那种多对象协调、且修改本身就是契约的场景。

## 裸指针主要用于可空性和互操作

裸指针在接口中仍然有合法角色。最干净的现代用法，是表示一个可选的借用对象，或者用于与更底层的 API 互操作。

这个角色比很多代码库赋予它们的要窄得多。

一个 `T*` 参数通常应当意味着两件事之一：

1. 被调用方可能根本收不到对象。
2. 接口正在跨入基于指针的互操作或底层数据结构，而指针身份本身就有意义。

如果 null 没有意义，引用通常更清晰。如果在转移所有权，`std::unique_ptr<T>` 或其他拥有类型更清晰。如果对象是数组或连续序列，`std::span<T>` 通常更清晰。一个裸指针如果同时意味着“非空借用、也许单个、也许多个、也许会被保留”，那就是语义债务。

同样的原则也适用于返回类型。在普通现代 C++ API 中，返回一个拥有所有权的裸指针几乎总是错误信号。返回一个观察者裸指针则可以是合理的，只要“缺失”有意义且生命周期由别处控制。

## 返回拥有的含义，而不是存储偶然性

返回类型需要与参数一样严格。主要问题是，调用方应该收到的是一个拥有所有权的含义、一个借用访问，还是像 `expected` 或 `optional` 这样承载决策的包装类型。

对于很多 API，即使涉及一次 move，返回一个拥有所有权的值仍然是最干净的设计。这会让生命周期保持局部，让组合更容易，并避免调用方依赖内部存储。现代 C++23 的 move 语义已经让值返回在很多情况下足够便宜，因此清晰性收益占了上风。

只有当源对象的生命周期显而易见、稳定，而且确实是契约的一部分时，借用返回类型才是合适的。把指向内部存储的 `std::string_view` 返回给调用方，只有在该存储显然比 view 活得更久，且调用方能安全使用这一事实时才是合理的。跨越宽边界时，这通常是糟糕的权衡，因为它把生命周期推理导出了本可由被调用方私下持有的地方。

可选性和失败也应当在返回类型中显式表达，而不是偷偷借助哨兵值。一个搜索操作返回“也许找到”，适合用 `std::optional<T>` 或观察者指针，前提是生命周期语义需要它。一个解析或加载操作，若其失败与决策相关，则适合用 `std::expected<T, E>`。一个在失败时返回空字符串或 `-1` 的函数，通常是在把 API 弄得比实现本身所需的更弱。

## 反模式：一个签名，藏着好几个故事

这种 API 在很多代码库里依然存在，因为它看起来很灵活。

```cpp
// Anti-pattern: signature hides ownership, failure, and buffer contract.
bool encode_record(const Record& record,
   std::vector<std::byte>& output,
   std::string* error_message = nullptr);
```

这一个函数现在就携带了好几条隐藏规则。

- 它是追加到 `output`，还是覆盖 `output`？
- `error_message` 是可选的，因为诊断不重要，还是因为日志会在别处记录？
- 失败时 `output` 会不会被部分修改？
- `false` 表示验证失败、编码 bug、容量问题，还是内部异常转换？

签名本身没有清楚回答这些问题。

更强的 API 通常会把语义拆开。

```cpp
auto encode_record(const Record& record)
-> std::expected<std::vector<std::byte>, EncodeError>;

auto append_encoded_record(const Record& record,
   ByteAppender& output)
-> std::expected<void, EncodeError>;
```

现在调用方可以在“生成拥有结果”和“追加式修改”之间做选择，而且失败契约是显式的。两种不同操作不再假装自己是一个通用而“灵活”的接口。

## 工厂和获取函数必须提前说明所有权

创建函数是所有权不清晰会变得格外昂贵的地方。返回 `T*` 的工厂会让调用方追问谁来 delete。写入输出参数再返回 bool 的工厂，通常会隐藏部分构造规则。默认返回 `shared_ptr<T>` 的工厂，则可能在设计还没证明其必要性时就强加共享所有权。

对于普通的独占所有权，`std::unique_ptr<T>` 通常是最清晰的结果。对于像值一样被创建的对象，直接返回值，或者在失败属于边界时返回 `expected<T, E>`。对于共享所有权，只有当创建出的对象本来就打算拥有共享生命周期时，才返回 `shared_ptr<T>`。

区别是很具体的：

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

关键点不在于具体用了哪种词汇类型，而在于：创建边界正是所有权应当变得毫不含糊的地方。

## API 表面也是成本表面

签名选择会以对调用方重要的方式影响成本。

一个 `std::function` 参数，即使回调只会被同步使用，也可能产生分配和类型擦除。`std::span<const T>` 可以避免把调用方强行塞进某种容器表示。按值接收 `std::string` 的 sink 构造函数，可能让临时对象高效地 move 进来。返回一个拥有所有权的 vector 可能只分配一次，却能消除一个长期存在的生命周期风险。这些都是设计权衡，而不是微优化。

正确的约束方式是：暴露调用方需要知道的成本，避免那些调用方无法推断的意外成本。一个好的签名并不承诺零成本，它只是让重要成本不至于令人意外。

这也是为什么过于宽泛的“便利”重载集合可能有害。当一个 API 接受各种指针、字符串、span、vector 和 view 组合时，重载表面可能比原问题本身还难推理。应优先保留少量几个语义清晰的形式。

## 验证与评审

函数签名是最便宜也最早能发现设计错误的地方之一。

有用的评审问题：

1. 每个参数是否都如实传达了借用、所有权转移、修改性或可选性？
2. 按值传递是否是在被调用方需要所有权的地方使用，而不是出于习惯或教条？
3. 裸指针是否被保留给了可选的借用访问或互操作，而不是模糊契约？
4. 返回类型是否清楚表达了拥有结果、借用访问或显式失败？
5. API 是否暴露了重要成本，只隐藏了偶然的实现细节？

测试应当覆盖由签名驱动的语义，而不只是核心行为。验证追加还是覆盖行为。验证返回的 view 在文档承诺的生命周期内保持有效，并且之后不再有效。验证失败会让输出参数或状态保持在承诺的条件下。一个清晰的签名，仍然需要证据来支撑。

## 要点

- 把签名视为语义契约，而不仅仅是编译器能接受的类型。
- 当被调用方只检查调用方拥有的数据时，使用借用参数类型。
- 当被调用方需要获得所有权，并且这个契约应当显而易见时，按值传递。
- 用引用、指针和返回包装器来有意识地表达修改性、可选性和失败。
- 让 API 表面足够小，使调用方无需阅读实现代码，也能理解生命周期和成本。

优秀的 C++ API 不只是能编译。它们足够早地说出真相，让调用方在第一次阅读时就能正确使用它们。这就是为什么签名值得投入这么多注意力。
