# 现代 C++ 代码评审检查清单

C++ 中大多数代码评审失败，并不是智力失败，而是评审姿态失败。评审者机械地顺着 diff 读下去，对命名或格式提几句意见，也许能发现一个局部 bug，却错过了这次改动真正引入的系统性问题：跨线程的新所有权关系、热路径中的隐藏分配、异常边界泄漏、被悄悄放宽的 API 契约、没有准入控制的队列，或者已经无法覆盖风险路径的 sanitizer lane。

现代 C++ 让许多高成本决策变得可见，但前提是评审者问对问题。本书的收官章节，就是把前面的内容收束成一套实用评审流程。它不是附录检查表的替代品，也不是风格指南。它是一组问题，用来塑造一个有经验的评审者在生产 C++ 中应该如何阅读一次变更。

核心思想很简单：先按失败经济学评审，再按局部优雅评审。一行看起来整洁的代码，仍然可能扩大生命周期、削弱不变量，或提高运维成本。下面的检查清单，正是围绕这些失败最常藏身的地方组织的。

## 第一遍：先识别这次改动真正属于哪一类

在逐行阅读之前，先给改动分类。分类错了，后面问的问题就会全错。

这次改动主要是：

- 一条新的所有权或生命周期路径？
- 一次 API 或契约变更？
- 一次并发或取消语义变更？
- 一次与数据布局或性能敏感路径相关的变更？
- 一次工具链、验证或构建流水线变更？
- 一次带有运维后果的服务行为变更？

很多 pull request 同时包含多类变化，但通常总有一类占主导。先从那里开始。如果一个改动新增了后台队列，它就不主要是重构。如果一个函数现在返回 `std::string_view`，它就不只是微优化。如果一个库开始在公开头文件中暴露模板化回调类型，它就不仅仅是“方便了一点”。评审应当先围绕主导风险展开。

这个分类步骤也会告诉你应该期待什么证据。API 变更应附带契约与兼容性推理。并发变更应附带取消与关闭行为的证据。性能声明应附带测量。工具链变更应解释它让哪类 bug 更容易或更难被发现。

## 所有权与生命周期问题

在生产 C++ 中，围绕所有权的评审仍然是性价比最高的一遍，因为生命周期 bug 仍然特别擅长把自己伪装成“局部看起来无害”。这些问题应当尽早问。

每一个新资源由谁拥有，它的所有权又在哪里结束？如果答案要靠读五个文件、再顺带猜测框架行为，那这个设计已经太弱了。所有权通常应当能从类型、对象图和构造点中直接看出来。

这次改动是否引入了跨时间的借用？这包括把 `std::string_view`、`std::span`、迭代器、引用或范围视图存进可能活得比源对象、请求、栈帧或容器 epoch 更久的状态里。代码一旦跨过异步边界、队列、回调、协程挂起点或脱离控制的线程，就应重新高强度审查每一种借用类型。

这次改动是否只是为了图方便，就把清晰所有权替换成了共享所有权？`std::shared_ptr` 有时是正确工具，但它也常常只是把设计决策往后拖。评审者应问：这里的共享所有权到底解决了什么具体生命周期问题？换成 moved value、被拥有的工作项或显式父级所有者，会不会更容易推理？

move 和 copy 成本的变化是有意的吗？值语义很强大，但评审者仍应判断：新的复制是契约的一部分，还是接口设计偶然带来的副作用成本？

在这个领域里，好的评审意见必须具体。“这看起来有风险”很弱；“这个队列现在存的是来自请求局部存储的 `std::string_view`，所以排队中的工作可能活得比缓冲区更久”就很强。

### 评审者应当标出的内容：悬空引用

悬空引用是评审者最值得抓住的一类 bug，因为类型系统看不见它，而且它常常能一路活过测试，直到竞争或重分配把问题暴露出来。

```cpp
// FLAG THIS: dangling reference from temporary
auto& config = get_configs()["database"];
// if get_configs() returns by value, the temporary map is destroyed
// at the semicolon. config is now a dangling reference.
// fix: auto config = get_configs()["database"];  (copy the value)

// FLAG THIS: reference into a vector that may reallocate
auto& first = items.front();
items.push_back(new_item);  // may reallocate, invalidating first
use(first);                 // undefined behavior

// FLAG THIS: string_view outliving its source
std::string_view name = get_user().name();
// if get_user() returns by value, the std::string inside the
// temporary is destroyed. name now points to freed memory.
// fix: std::string name = std::string{get_user().name()};

// FLAG THIS: lambda capturing reference to local
auto make_callback(request& req) {
    auto& headers = req.headers();  // reference to req's member
    return [&headers]() {           // captures by reference
        log(headers);               // dangling if req is destroyed
    };
    // fix: capture by value, or capture a copy of headers
}
```

评审者的问题永远一样：被引用对象是否可被证明在每次使用时都仍然存活？如果答案必须依赖对框架调度、队列时序或调用者纪律的推理，那么代码就应该复制，而不是借用。

### 评审者应当标出的内容：move 操作缺少 noexcept

没有标记 `noexcept` 的 move 构造函数或 move 赋值，会悄悄拉低标准库容器性能。如果 move 构造可能抛异常，`std::vector` 在重分配时会选择 copy 而不是 move，因为强异常保证要求它这么做。

```cpp
// FLAG THIS: move constructor without noexcept
class connection {
    std::unique_ptr<socket> sock_;
    std::string endpoint_;
public:
    connection(connection&& other)  // missing noexcept!
        : sock_(std::move(other.sock_))
        , endpoint_(std::move(other.endpoint_))
    {}
    // std::vector<connection> will COPY during reallocation
    // instead of moving. For large vectors, this is a silent
    // performance cliff — and may fail to compile if the type
    // is move-only.
};

// CORRECT:
connection(connection&& other) noexcept
    : sock_(std::move(other.sock_))
    , endpoint_(std::move(other.endpoint_))
{}
// now vector::push_back uses move during reallocation
```

这同样适用于 move 赋值。`std::move_if_noexcept` 和容器实现都会在编译期检查 `noexcept`。如果任一 move 操作都可能抛异常，回退路径就一定更贵。

### 评审者应当标出的内容：异常不安全的资源获取

当代码在没有 RAII 保护的情况下获取多个资源，而且获取动作之间又夹杂着可能抛异常的操作时，资源泄漏就会潜伏其中。

```cpp
// FLAG THIS: raw acquire/release with throwing code between
void setup_pipeline(config const& cfg) {
    auto* buf = allocate_buffer(cfg.buffer_size);  // raw allocation
    auto fd = open_file(cfg.path);                 // may throw
    auto conn = connect_to_db(cfg.db_url);         // may throw
    register_pipeline(buf, fd, conn);
    // if open_file throws, buf leaks
    // if connect_to_db throws, buf leaks AND fd leaks
}

// CORRECT: RAII from the first acquisition
void setup_pipeline(config const& cfg) {
    auto buf = std::unique_ptr<std::byte[]>(
        allocate_buffer(cfg.buffer_size));
    auto fd = owned_fd{open_file(cfg.path)};       // RAII wrapper
    auto conn = connect_to_db(cfg.db_url);         // already RAII (or should be)
    register_pipeline(buf.release(), fd.release(), std::move(conn));
    // every intermediate throw is safe — destructors clean up
}
```

要盯住的模式，是资源获取与 RAII 接管之间的任何空档。哪怕中间只有一行可能抛异常的代码，也足以造成泄漏。评审者还应标出把 `new` 直接作为函数参数的写法：如果另一个参数求值时抛异常，智能指针尚未构造完成，这次分配就可能泄漏。

## 不变量与失败边界问题

下一遍要看的是无效状态与失败形态。这个改动会不会让无效状态更容易被创建、更容易被观察到，或更难恢复？构造路径、配置对象、部分初始化和 mutation API，都是不变量会被悄悄削弱的常见地方。

然后再问：失败是如何报告的？可恢复的领域失败仍然被一致表示了吗，还是新添了第二条错误通道？之前被收口的依赖错误，现在是否泄漏到了更上层？一个标了 `[[nodiscard]]` 或返回 `std::expected` 的函数，是否多出了忽略结果的新调用点？如果涉及异常，改动是否无意中放宽了异常边界？

评审者还应检查回滚与清理行为，尤其是在资源拥有型操作周围。如果新路径在中途失败了，哪些东西仍然为真？部分写入的文件是否被删除，事务是否被取消，临时状态是否被丢弃，后台工作是否被停止，遥测是否仍然以正确类别发出？

一个很有用的纪律，是要求作者用一句话讲清失败故事。“如果依赖 X 在状态 Y 已预留后超时，请求会返回 `dependency_timeout`，预留会被释放，并且没有后台重试会活过关闭流程。”如果作者无法简洁说清这件事，那失败边界大概率还不够清楚。

## 接口与库表面问题

任何公开接口或广泛共享接口，都值得单独做一遍评审，因为局部实现质量并不能弥补契约的薄弱。

参数与返回类型是变得更诚实了，还是更不诚实了？返回 `std::span<const std::byte>` 也许能清晰表达借用；返回内部可变状态的引用，则可能隐藏耦合。对只读解析调用来说，接受 `std::string_view` 也许是对的；对会保留该字符串的对象来说，它就可能是错的。评审应聚焦于：签名现在对所有权、成本与失败到底做了什么承诺。

如果新增了模板、概念、回调或类型擦除，为什么要选这种形式？概念能改善诊断并阻止胡乱实例化，但也会扩大编译期表面。类型擦除能稳定调用点，但可能引入分配或间接调用成本。新的泛型化必须证明自己值得。

对库变更来说，还要问公开表面是否泄漏了实现细节。新头文件是否暴露了调用者本不该知道的传输类型、分配策略、同步原语或错误类型？一个看似无害的 inline helper，是否改变了 ABI 或源码兼容性故事？文档和示例是否随着契约一起更新了，还是新行为只有读 diff 才知道？

把接口评审好，意味着要像下一个调用者那样思考，而不是像当前作者那样思考。

### 评审者应当标出的内容：API 中的隐式转换与窄化

公开接口如果会静默接受错误类型或窄化数值，就很容易埋下 bug：单元测试里不一定暴露，直到生产数据到来才出问题。

```cpp
// FLAG THIS: implicit conversion hides a bug
class rate_limiter {
public:
    rate_limiter(int max_requests, int window_seconds);
};

// caller writes:
rate_limiter limiter(30, 60);     // OK: 30 requests per 60 seconds
rate_limiter limiter(60, 30);     // compiles fine, but the arguments
                                   // are swapped — 60 req per 30s
// no type safety distinguishes max_requests from window_seconds

// BETTER: use distinct types or a builder
struct max_requests { int value; };
struct window_seconds { int value; };

rate_limiter(max_requests max, window_seconds window);
// rate_limiter limiter(window_seconds{60}, max_requests{30}); // compile error
```

```cpp
// FLAG THIS: narrowing conversion in initialization
void set_buffer_size(std::size_t bytes);

int user_input = get_config_value("buffer_size");  // may be negative
set_buffer_size(user_input);  // silent narrowing: -1 becomes SIZE_MAX
// fix: validate before conversion, or use std::size_t throughout
```

## 并发、时间与关闭问题

并发评审，本质上大多是加上“时间”维度后的生命周期评审。关键问题不是代码用了哪些原语名称，而是工作在移动过程中，是否仍然保持有归属、且有边界。

要问：改动是否引入了脱离控制的工作、隐藏线程、执行器跳转，或所有者不明显的协程挂起点？停止请求是如何传播的？截止时间和重试是显式的，还是藏在辅助层里？队列增长是否有界，新的过载策略又是什么？

如果改动碰到了锁或共享状态，就要把争用和不变量一起评审。锁保护的是完整不变量，还是仅仅几个字段？是否在持锁状态下调用了回调？统计更新或缓存更新是否引入了一场以后会被辩解成“良性”的数据竞争？ThreadSanitizer 也许能抓住其中一部分，但评审仍应尽量在运行前消除这种模糊性。

关闭流程在几乎所有服务或工具改动里，都值得单独问一句：这次 diff 之后，当析构开始时，还有哪些工作可能仍在运行，它们又如何停止？如果答案不清楚，评审就还没结束。

## 数据布局与成本模型问题

很多性能 bug 在代码评审中都是伪装成“无害抽象”进入代码库的。因此，评审者应问的是成本移到了哪里，而不只是代码“看起来是否高效”。

改动是否在重要路径上新增了分配、增大了大量容器中对象的体积、增加了间接层，或把一个局部值变成了堆管理的共享状态？一次 ranges 管道改写是否在保住生命周期的前提下提升了清晰度，还是引入了隐藏迭代、临时物化或悬空视图风险？一次容器选择变化，是否改变了内存模型与失效模型，而作者却没有讨论？

这里的评审标准，应当是与声明成比例的证据。如果改动宣称提升了性能，就要 benchmark 或 profile 数据。如果它说新增的那次分配可以忽略不计，就要问是在什么负载下可以忽略。如果答案只是“应该没事”，那就要判断这段代码所在的位置，是否真允许“应该”。

并不是每个改动都需要 benchmark。但性能敏感的改动，必须有一个经得住基本追问的成本模型。

## 验证与交付问题

好的 C++ 评审不会停留在源码 diff。最后一遍要看的是：仓库是否仍然有一种可信方式，证明这次改动是可靠的。

现在有哪些测试覆盖了风险路径？如果改动新增了回滚分支、过载行为或宿主-库边界，是否有测试在刻意触发它？如果改动影响了内存、并发或输入处理，sanitizer 或 fuzzing lane 还覆盖这些路径吗？如果构建或 CI 配置变了，诊断矩阵是更强了，还是更弱了？

运维层变化也值得做可观测性评审。如果服务现在更早拒绝工作，运维人员能区分它与依赖故障吗？如果库新增了诊断，这些诊断是否足够稳定，能被宿主消费？如果崩溃处理或符号处理变了，交付产物之后还能否继续诊断？

这也是评审者该直接要求补充证据，而不是自己去考古的时候。评审不是无偿考古劳动。如果某个改动需要新的测试、benchmark、sanitizer 运行或迁移说明，就应当明确提出。

## 如何写出有用的评审意见

好的评审意见会指出风险、点明被违反或不清楚的契约，并说明需要什么证据才能消除问题。它不是简单表达个人口味。

强评审意见通常长这样：

- “这个回调捕获了 `this`，而它又被存进了可能活过关闭流程的工作里。`request_stop()` 之后，这个生命周期由谁拥有？”
- “公开 API 现在返回的是指向解析器拥有存储的 `std::string_view`。这块存储在什么地方被保证活得比调用者的使用更久？”
- “这个队列是有界的，但过载行为仍然是隐式的。我们是拒绝、阻塞，还是丢弃可选工作？运维上又如何体现？”
- “改动声称降低了延迟。哪次 benchmark 或 profile 运行证明新的分配模式在现实输入规模下更好？”

弱评审意见则通常模糊、纯风格化，或者明明问题是语义层面的，却用个人偏好包装出来。

评审者也应在证据充分时明确说出来。如果所有权清楚、测试命中了风险路径，而且契约确实变得更好了，就应当把这一点说清楚。好的评审不只是为了阻止变更，也是为了让“为什么可以接受”这件事变得显式。

## 什么时候应当阻止变更

并不是每个未解决问题都值得强行拦下。有些值得。

当所有权不清楚、借用状态可能活得比其来源更久、失败契约不一致、并发无界或关闭语义未定义、公开接口变更缺少兼容性推理、性能声明缺乏必要证据，或者验证体系已经不再覆盖风险路径时，就应当阻止这次变更。

不要仅仅因为“我会写得不一样”就阻止改动。现代 C++ 已经有足够多的偶然复杂度，评审不应再额外叠加一层由口味驱动的摩擦。

## 要点总结

最有效的 C++ 评审者，会在评审局部优雅之前，先评审所有权、失败形态、接口诚实性、并发生命周期、成本迁移，以及验证证据。他们先给改动分类，再围绕生产风险提问，并在仅靠代码本身不足以支撑结论的地方，坚持索要证据。

这种姿态，才是把本书其余内容转化成日常工程行为的关键。精确的所有权模型、显式的失败边界、有界并发、诚实的 API 和严格的诊断纪律之所以重要，并不是因为它们“看起来现代”，而是因为它们能让评审者用具体术语解释：这次改动为什么安全、为什么有风险，或者为什么仍然不完整。

Review questions:

- 这次改动引入的主导性生产风险是什么，评审是否先聚焦在那里？
- 哪些所有权、生命周期或借用假设现在跨越了时间、线程或 API 边界？
- 这次改动如何改变了失败报告、回滚保证或不变量保持方式？
- 哪个成本模型发生了变化，又有什么证据支持任何性能或效率声明？
- 现在由哪些测试、sanitizer lane、诊断或运维信号，来证明风险行为仍然可靠？
