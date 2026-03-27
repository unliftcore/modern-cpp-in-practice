# 现代 C++ 代码评审检查清单

C++ 代码评审中的大多数失误，根源不在能力，而在评审方式。评审者机械地顺着 diff 读下去，对命名或格式提几句意见，也许能发现一个局部 bug，却漏掉了这次改动真正引入的系统级问题：跨线程的新所有权关系、热路径上的隐藏分配、异常边界泄漏、被悄悄放宽的 API 契约、没有准入控制的队列、再也无法覆盖风险路径的 sanitizer lane。

现代 C++ 让许多高成本决策变得可见，但前提是评审者问对了问题。作为本书的收官章节，这里要做的是把前面的内容收束成一套实用评审流程。它不是附录检查表的替代品，也不是风格指南，而是一组问题，用来引导有经验的评审者在审查生产级 C++ 变更时该怎么读代码。

核心思想很简单：先审失败代价，再审局部优雅。一行看起来整洁的代码仍然可能延长生命周期、削弱不变量或抬高运维成本。下面的检查清单围绕这些失败最常潜伏之处来组织。

## 第一遍：先识别这次改动真正属于哪一类

逐行阅读之前，先给改动分类。分类错了，后面问的问题就全跑偏了。

这次改动主要是：

- 一条新的所有权或生命周期路径？
- 一次 API 或契约变更？
- 一次并发或取消语义变更？
- 一次与数据布局或性能敏感路径相关的变更？
- 一次工具链、验证或构建流水线变更？
- 一次带有运维后果的服务行为变更？

很多 pull request 同时涉及多类变化，但通常有一类占主导地位，先从那里入手。新增了后台队列的改动本质上就不是重构。函数返回值改成 `std::string_view` 就不只是微优化。库开始在公开头文件中暴露模板化回调类型，就不只是”方便了一点”。评审应当先围绕主导风险展开。

分类的同时也就确定了应该期待什么证据。API 变更应附带契约与兼容性方面的论证；并发变更应附带取消与关闭行为的证据；性能声明应附带实测数据；工具链变更应说明它让哪类 bug 更容易或更难被发现。

## 所有权与生命周期问题

在生产级 C++ 中，所有权审查仍然是回报最高的一遍，生命周期 bug 天生擅长伪装成”局部看起来无害”。这些问题应当尽早问。

每个新资源由谁拥有？所有权在哪里终止？如果回答这些问题需要翻五个文件、再猜测框架行为，那设计本身就已经有问题了。所有权通常应当能从类型、对象图和构造点直接看出来。

改动是否引入了跨时间的借用？即把 `std::string_view`、`std::span`、迭代器、引用或范围视图存进了可能比源对象、请求、栈帧或容器 epoch 活得更久的状态。代码一旦跨越异步边界、队列、回调、协程挂起点或脱离控制的线程，就必须重新严格审查每一种借用类型。

改动是否只是图方便，就把清晰所有权换成了共享所有权？`std::shared_ptr` 有时确实是正确工具，但更多时候只是把设计决策往后拖。评审者应该追问：这里的共享所有权到底解决了什么具体的生命周期问题？换成 moved value、带所有权的工作项或显式的父级所有者，是不是更容易推理？

move 和 copy 的成本变化是刻意为之，还是无心之过？值语义固然强大，但评审者仍应甄别：新增的复制到底是契约的一部分，还是接口设计附带的意外开销？

这个领域的评审意见必须具体。”这看起来有风险”太弱了；”这个队列现在存的是来自请求局部存储的 `std::string_view`，排队中的工作可能比缓冲区活得更久”才够有力。

### 评审者应当标出的内容：悬空引用

悬空引用是评审者最值得抓住的一类 bug。类型系统对它无能为力，而且它往往能一路安然通过测试，直到竞态条件或重分配才把问题暴露出来。

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

评审者要问的始终是同一个问题：被引用的对象，是否可以被证明在每次使用时都仍然存活？如果回答这个问题需要推导框架调度、队列时序或调用者的行为规范，那代码就应该复制，而不是借用。

### 评审者应当标出的内容：move 操作缺少 noexcept

未标记 `noexcept` 的 move 构造函数或 move 赋值运算符会悄然拖慢标准库容器的性能。一旦 move 构造可能抛异常，`std::vector` 在重分配时就会退回到 copy 而非 move，因为强异常保证要求如此。

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

move 赋值同理。`std::move_if_noexcept` 和容器实现都会在编译期检查 `noexcept`。只要任一 move 操作可能抛异常，回退路径的开销就一定更大。

### 评审者应当标出的内容：异常不安全的资源获取

在缺少 RAII 保护的前提下连续获取多个资源，且获取动作之间夹杂着可能抛异常的操作，这就是资源泄漏最爱潜伏的地方。

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

要盯住的模式是：资源获取和 RAII 接管之间的任何空档。哪怕中间只有一行可能抛异常的代码，就足以造成泄漏。评审者还应标出把 `new` 直接写在函数参数里的情况，如果另一个参数求值时抛了异常，智能指针还没来得及构造，这次分配就泄漏了。

## 不变量与失败边界问题

下一遍审查的重点是无效状态与失败形态。这个改动是否让无效状态更容易出现、更容易暴露给外部，或更难恢复？构造路径、配置对象、部分初始化和 mutation API，都是不变量容易被悄悄削弱的地方。

接着问：失败是怎么上报的？可恢复的业务错误是否仍然用统一方式表达，还是新增了第二条错误通道？之前被收口在底层的依赖错误，现在是否泄漏到了更上层？标了 `[[nodiscard]]` 或返回 `std::expected` 的函数，是否新增了忽略返回值的调用点？如果涉及异常，改动是否无意中扩大了异常边界？

评审者还应检查回滚与清理行为，尤其是涉及资源所有权的操作。如果新路径中途失败了，哪些状态仍然成立？部分写入的文件有没有删掉？事务有没有取消？临时状态有没有丢弃？后台工作有没有停下来？遥测指标有没有归到正确的分类里？

一个有效的做法是要求作者用一句话把失败场景讲清楚。比如：”如果依赖 X 在状态 Y 已预留后超时，请求返回 `dependency_timeout`，预留随即释放，且没有后台重试能存活到关闭流程之后。”如果作者说不清这句话，失败边界多半还不够明确。

## 接口与库表面问题

任何公开接口或广泛共享的接口都值得单独审一遍。局部实现质量再高，也弥补不了契约本身的薄弱。

参数和返回类型是变得更诚实了，还是更含糊了？返回 `std::span<const std::byte>` 也许能清晰表达借用语义，但返回内部可变状态的引用，就可能在暗中引入耦合。对只读的解析调用来说，接受 `std::string_view` 可能是对的；但如果对象会保留这个字符串，那就可能埋下隐患。评审的焦点在于：函数签名现在对所有权、成本与失败做出了什么样的承诺。

如果新增了模板、concept、回调或类型擦除，为什么选这种形式？concept 能改善诊断信息并阻止无意义的实例化，但也会扩大编译期表面积。类型擦除能稳定调用点，但可能带来额外的分配或间接调用开销。新引入的泛型机制必须值回票价。

对库变更，还要追问公开表面是否泄漏了实现细节。新头文件是否暴露了调用者本不该知道的传输类型、分配策略、同步原语或错误类型？一个看似无害的 inline helper，有没有改变 ABI 或源码兼容性？文档和示例有没有跟着契约一起更新，还是说新行为只有读 diff 才知道？

审好接口，就是要站在下一个调用者的角度思考，而不是站在当前作者的角度。

### 评审者应当标出的内容：API 中的隐式转换与窄化

如果公开接口会静默接受错误类型或窄化数值，bug 就很容易埋进去——单元测试里未必暴露，往往要等到生产数据才出问题。

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

并发评审本质上是加了”时间”维度的生命周期评审。关键不在于代码用了哪些并发原语，而在于工作在流转过程中所有权是否始终明确、规模是否始终可控。

要问的是：改动有没有引入脱管的工作、隐藏线程、执行器跳转，或归属不明的协程挂起点？停止请求怎么传播？超时和重试是显式设定的，还是藏在辅助层里？队列增长有没有上限，过载了怎么处理？

如果改动涉及锁或共享状态，就要把争用和不变量放在一起审查。锁保护的是完整的不变量，还是只管了几个字段？有没有在持锁时调用回调？统计更新或缓存更新有没有引入一场日后会被解释成”良性”的数据竞争？ThreadSanitizer 也许能捕获其中一部分，但评审仍应在运行之前尽量消除这类歧义。

几乎所有服务或工具的改动都值得单独追问关闭流程：打完这个 diff 之后，当析构开始时还有哪些工作可能仍在运行？它们怎么停下来？如果答案不清楚，评审就还没结束。

## 数据布局与成本模型问题

很多性能 bug 披着”无害抽象”的外衣混进代码库。评审者该问的是成本转移到了哪里，而不只是代码”看起来是不是高效”。

改动有没有在热路径上新增分配、增大容器中对象的体积、加深间接层次，或把局部值变成堆上的共享状态？ranges 管道的改写是在保住生命周期的前提下提升了可读性，还是暗中引入了隐藏迭代、临时物化或悬空视图的风险？容器选型的变化是否改变了内存和迭代器失效模型，而作者却没有提及？

评审标准是：声明多大，证据就应该多充分。宣称提升了性能，就要拿出 benchmark 或 profile 数据。说新增分配可以忽略不计，就要说清楚在什么负载下可以忽略。如果答案只是”应该没事”，那就得判断这段代码所在位置是否真的容得下”应该”二字。

不是每个改动都需要 benchmark，但性能敏感的改动必须有一个经得住基本追问的成本模型。

## 验证与交付问题

好的 C++ 评审不止于源码 diff。最后一遍要确认：仓库是否仍然有可信的手段来证明这次改动是可靠的。

哪些测试覆盖了风险路径？如果改动新增了回滚分支、过载行为或宿主-库边界交互，有没有测试在刻意触发它？如果改动影响了内存、并发或输入处理，sanitizer 或 fuzzing lane 还能覆盖到吗？如果构建或 CI 配置变了，诊断矩阵是变强了，还是变弱了？

运维层面的变化也需要可观测性评审。服务现在更早拒绝请求了，运维人员能把它和依赖故障区分开吗？库新增了诊断信息，这些信息是否足够稳定，能让宿主程序放心使用？崩溃处理或符号处理变了，交付出去的产物以后还能不能正常诊断？

这也是评审者该直接要求补充证据、而不是自己去翻历史代码的时候。评审不是义务考古。如果某个改动缺少必要的测试、benchmark、sanitizer 运行结果或迁移说明，就应当明确提出来。

## 如何写出有用的评审意见

好的评审意见会指出风险所在、点明被违反或不清楚的契约，并说明需要什么证据才能消除疑虑。它不是在表达个人口味。

强评审意见通常长这样：

- “这个回调捕获了 `this`，而它又被存进了可能活过关闭流程的工作里。`request_stop()` 之后，这个生命周期由谁拥有？”
- “公开 API 现在返回的是指向解析器拥有存储的 `std::string_view`。这块存储在什么地方被保证活得比调用者的使用更久？”
- “这个队列是有界的，但过载行为仍然是隐式的。我们是拒绝、阻塞，还是丢弃可选工作？运维上又如何体现？”
- “改动声称降低了延迟。哪次 benchmark 或 profile 运行证明新的分配模式在现实输入规模下更好？”

弱评审意见往往含混笼统、纠结于风格，或者明明问题出在语义层面，却包装成个人偏好。

评审者也应该在证据充分时明确表态。如果所有权清楚、测试覆盖了风险路径、契约确实变得更好了，就应当说出来。好的评审不只是为了拦住有问题的变更，也是为了让”为什么可以接受”变成一个显式的结论。

## 什么时候应当阻止变更

不是每个未解决的问题都值得强行拦下，但有些必须拦。

以下情况应该阻止变更：所有权不清楚；借用的状态可能比来源活得更久；失败契约前后不一致；并发无界或关闭语义未定义；公开接口变更缺少兼容性论证；性能声明缺乏证据；验证体系已经覆盖不到风险路径。

不要仅仅因为”换作是我会写成另一个样子”就拦住改动。现代 C++ 本身的偶然复杂度已经够高了，评审不应再叠加一层口味驱动的阻力。

## 要点总结

高效的 C++ 评审者会先审查所有权、失败形态、接口诚实性、并发生命周期、成本流向和验证证据，然后才看局部优雅。他们先给改动分类，围绕生产风险提问，在仅靠代码本身撑不起结论的地方坚持索要证据。

这种审查态度才能把本书的内容转化为日常工程实践。精确的所有权模型、显式的失败边界、有界并发、诚实的 API 和严格的诊断纪律之所以重要，不是因为它们”看起来现代”，而是因为它们让评审者能用具体的语言说清楚：这次改动为什么安全、为什么有风险，或者为什么还不完整。

复习问题：

- 这次改动引入的主导性生产风险是什么，评审是否先聚焦在那里？
- 哪些所有权、生命周期或借用假设现在跨越了时间、线程或 API 边界？
- 这次改动如何改变了失败报告、回滚保证或不变量保持方式？
- 哪个成本模型发生了变化，又有什么证据支持任何性能或效率声明？
- 现在由哪些测试、sanitizer lane、诊断或运维信号，来证明风险行为仍然可靠？
