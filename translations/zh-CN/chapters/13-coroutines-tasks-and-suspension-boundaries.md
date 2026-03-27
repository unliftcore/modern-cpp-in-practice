# 协程、任务与挂起边界

本章假定你已经理解错误传递和并发共享状态设计。关注点很具体：协程（coroutine，一种可暂停和恢复的函数）拥有什么、什么能在挂起后继续存活，以及异步控制流掩盖生命周期时会出什么问题。

## 生产中的问题

协程让异步代码更好读，也更容易产生误导。

原本层层嵌套回调的请求处理器，可以写成顺序执行的直线代码。流式解析器可以逐个产出值。后台刷新任务可以用 `co_await`（协程挂起等待）等定时器和 I/O，不必再手写状态机。收益是真实的。但底层机制并没有消失，状态机依然存在，只是搬进了协程帧（coroutine frame，编译器为协程分配的存储区），而协程帧的生命周期、所有权和恢复上下文都需要刻意设计。

生产环境中协程引发的故障通常有四类：

1. 借用的数据在挂起后比其来源存活得更久。
2. 任务没有明确的所有者，导致工作比启动它的组件存活得更久。
3. 失败和取消路径是隐式的，挂起的工作恢复时所依赖的假设已经失效。
4. 执行在线程或 executor 之间跳转，而代码中看不出来。

本章的讨论范围限于局部。如何在取消压力下管理整棵任务树是第 14 章的内容。本章要回答的问题是：每个协程到底是什么？答案是一个拥有资源的对象，其挂起点划定了生命周期的边界。

## 协程替代了什么：回调地狱与手写状态机

要理解协程的设计取舍，先看看它们替代了什么。协程出现之前，异步代码依赖 continuation-passing style（续体传递风格），每一步都把回调串到下一步。一个简单的”获取、校验、存储”序列写出来是这样的：

```cpp
// Continuation-passing style — correct but unreadable at scale.
void handle_request(request req, std::function<void(response)> done) {
    fetch_profile(req.user_id, [req, done](std::expected<profile, error> prof) {
        if (!prof) { done(error_response(prof.error())); return; }
        validate_access(prof->role, req.resource,
            [req, prof = *prof, done](std::expected<bool, error> ok) {
                if (!ok || !*ok) { done(denied_response()); return; }
                store_audit_log(req, prof,
                    [req, prof, done](std::expected<void, error> result) {
                        if (!result) { done(error_response(result.error())); return; }
                        done(success_response(prof));
                    });
            });
    });
}
```

每一步都嵌套更深一层。错误处理在每一层重复。捕获变量的生命周期必须手动管理：按值捕获会带来大量拷贝，按引用捕获则可能悬空。加上超时、取消或重试逻辑，嵌套只会更深。这就是协程出现之前生产代码中异步 C++ 的真实面貌。

对应的协程版本：

```cpp
task<response> handle_request(request req) {
    auto prof = co_await fetch_profile(req.user_id);
    if (!prof) co_return error_response(prof.error());

    auto ok = co_await validate_access(prof->role, req.resource);
    if (!ok || !*ok) co_return denied_response();

    auto result = co_await store_audit_log(req, *prof);
    if (!result) co_return error_response(result.error());

    co_return success_response(*prof);
}
```

代码可以顺序阅读，每一步只有一条错误路径，没有嵌套。改进是真实的。但状态机并没有消失，它搬进了协程帧。本章剩余部分讨论的，就是这对所有权和生命周期意味着什么。

示例项目的 handler 层（`examples/web-api/src/modules/handlers.cppm`）用同步代码展示了同样的结构优势。每个 handler 接收 `const http::Request&`（借用）并返回 `http::Response`（拥有），控制流是直线式的：解析路径参数、校验输入、调用 repository、把结果转换为 HTTP 响应。错误处理在每一步本地完成，无需嵌套回调。这些 handler 不是协程，但体现了同一条原则：每一步顺序执行、错误路径扁平，业务逻辑就更易读、更好审查。

## 协程是带存储的状态机

把协程当语法糖来用，是最快制造生命周期 bug 的捷径。

函数变成协程后，部分状态会搬进协程帧。参数可能被拷贝或移动到帧中。跨越挂起点存活的局部变量驻留在帧中。awaiter（等待器，控制挂起和恢复行为的对象）的状态可能决定执行何时恢复、在哪里恢复。析构可能发生在成功、失败、取消，或者持有该任务对象的外部对象被销毁时。

基于栈的常规直觉在这里不再可靠。非协程函数中，局部变量在控制流离开作用域时销毁。协程中，一个跨越挂起点的局部变量可能比调用方预想的活得久得多；反过来，一个借用调用方存储的 view 可能在恢复之前早已失效。

核心评审问题很简单：从一个挂起点到下一个挂起点之间，哪些数据必须保持有效？

## 挂起点就是生命周期边界

每个 `co_await` 都是一道边界，跨过它时应当重新审视之前的假设。

挂起之前，先问自己：

1. 哪些引用、span、string view、迭代器和指针在恢复后仍然需要？
2. 它们所引用的存储归谁所有？
3. 被等待的操作有没有可能比发起它的调用方、请求或组件活得更久？
4. 恢复会发生在哪个 executor 或线程上？

这相当于把 API 生命周期评审搬到了局部层面。协程在挂起期间保留了借用数据，你就必须证明数据的所有者比协程活得更久，否则就让协程接管所有权。

这也解释了为什么协程 API 在参数选择上比同步 API 更严格。同步的辅助函数可以放心地接收 `std::string_view`，因为它立即返回。会挂起的异步任务通常不该保留这个 view，除非所有权契约非常严格且有明确文档。

示例项目的 `Request::path_param_after()`（`examples/web-api/src/modules/http.cppm`）展示了满足这条边界约束时的安全做法。它返回 `std::optional<std::string_view>`，指向请求对象的 `path` 成员。当前设计中这是安全的，因为 handler 同步执行，`Request` 对象在整个 handler 调用期间都存活。但如果这些 handler 变成会在执行中途挂起的协程，同一个 `string_view` 就会在请求缓冲区被回收时变为悬空引用。设计成立的前提很简单：请求在 handler 执行期间存活，handler 不会挂起。

## 反模式：借用的请求状态跨越挂起存活

```cpp
// Anti-pattern: borrowed data may dangle after suspension.
task<parsed_request> parse_and_authorize(
    std::string_view body,
    const auth_context& auth) {

    auto token = co_await fetch_access_token(auth.user_id());
    co_return parse_request(body, token); // BUG: body may refer to caller-owned storage.
}
```

这段代码看上去很高效，省去了一次拷贝。但它只在调用方保证 `body` 在协程完成前始终有效时才正确。在服务端代码中，"协程完成"意味着网络 I/O、认证查找、重试和超时处理全部结束。这个承诺很难兑现，往往也是错误的。

更安全的默认做法是：凡是挂起后还要用的数据，都把所有权移交给协程。

```cpp
task<parsed_request> parse_and_authorize(
    std::string body,
    auth_context auth) {

    auto token = co_await fetch_access_token(auth.user_id());
    co_return parse_request(body, token);
}
```

拷贝或移动的开销清晰可见，便于评审。协程帧现在拥有了它所需的一切。分配开销确实构成问题时，去测量，再围绕消息边界或存储复用来重新设计。不要悄悄地跨越时间借用数据。

## 更多生命周期陷阱：局部变量、临时对象和 lambda 捕获

借用参数反模式是最常见的情况，但协程生命周期 bug 还有其他几种常见形式。

### 指向调用方局部变量的悬空引用

```cpp
// BUG: coroutine captures a reference to a local that dies when the caller returns.
task<void> start_processing(dispatcher& d) {
    std::vector<record> batch = build_batch();
    co_await d.schedule([&batch] {     // lambda captures batch by reference
        process(batch);                // batch may be destroyed if start_processing
    });                                // is suspended and its caller exits
}
```

当 `start_processing` 在 `co_await` 处挂起时，协程帧会让 `batch` 继续存活，但前提是帧本身还活着。一旦任务被 detach 或父作用域退出，协程帧随之销毁，lambda 中的引用便悬空。修复方法：按值捕获，或通过结构化所有权确保父作用域的生命周期长于被调度的工作。

### 临时对象生命周期坍塌

```cpp
// BUG: temporary string destroyed before coroutine body executes.
task<void> log_message(std::string_view msg);

void caller() {
    log_message("request started"s + request_id()); // temporary std::string
    // temporary is destroyed here, before the coroutine even begins if lazy-start
}
```

如果是 lazy-start 协程，这个临时 `std::string` 在分号处就销毁了，而协程甚至还没开始执行。即使是 eager-start 协程，如果帧把 `msg` 存为 `string_view`，第一次挂起之后它就指向了已释放的内存。解决方案是在协程签名中按值接收 `std::string`，让帧持有自己的副本。

### 跨越挂起的 `this` 指针

```cpp
// BUG: 'this' may dangle if the object is moved or destroyed while suspended.
class connection {
    std::string peer_addr_;
public:
    task<void> run() {
        auto data = co_await read_socket();    // suspended here
        log("received from " + peer_addr_);    // 'this' may be invalid
    }
};
```

如果 `connection` 对象在 `run()` 挂起期间被移动到另一个容器或被销毁，恢复时 `this` 就失效了。成员协程只有在对象生命周期保证长于协程时才安全。实践中，这意味着协程应持有 `shared_ptr<connection>`，或者所有者作用域做结构化设计，防止对象在挂起期间被销毁。

这些不是边角案例，而是生产环境中协程生命周期最常见的失败模式。每个挂起点都是调用方的世界可能已经变了样的时刻。

## 任务类型就是所有权契约

协程的返回类型定义了所有权、结果传递和析构语义。

一个合格的任务类型至少要回答以下问题：

1. 销毁任务时会取消工作、detach 工作、阻塞等待，还是泄漏？
2. 结果会被消费一次、多次，还是完全不消费？
3. 异常如何传递？
4. 任务是立即启动（eager），还是等到被 await 时才开始（lazy）？
5. 取消是否有显式表示？

很多协程 bug 本质上是任务类型 bug。detached 的”发后即忘”协程不是异步风格偏好，而是一个所有权声明：后续代码不需要知道工作何时完成、是否失败、是否该在关闭时取消。这个声明在生产服务中很少站得住脚。

保守的默认做法很简单：每个已启动的任务都应有明确的所有者和可见的完成路径。如果你说不出所有者是谁，那你就是在制造孤儿工作。

## 立即启动与延迟启动影响的是正确性

协程是创建后立刻运行，还是等到被 await 时才启动，这影响的是正确性，不只是性能。

eager task（立即启动的任务）可能在调用方保存 handle 之前就已产生副作用。lazy task（延迟启动的任务）把工作推迟到编排代码决定何时、何地运行时才开始。两种方式都有合理的场景，行为必须一致且有文档说明。

这直接影响失败边界。构造任务就可能触发工作的话，异常和取消可能在父作用域认为任务”已激活”之前就可以被观察到。工作只在第一次 await 或显式调度时启动的话，所有权关系更容易推理。

建议不是一刀切地偏向某种策略，而是：任务抽象必须让启动策略足够明显，明显到评审者无需深入 promise type 的实现就能知道副作用何时开始。

## 恢复上下文是正确性的一部分

协程代码读起来往往像是一直在同一个线程上执行。这是错觉。

一个被等待的操作，恢复时可能跑在 I/O 线程、调度器线程池、UI 亲和线程，或 awaiter 选定的 executor（执行器，负责调度任务运行的组件）上。恢复后的代码如果会访问线程绑定的状态，或者必须在特定 executor 上继续执行，这个要求必须在抽象层面显式体现。

这正是团队用更漂亮的语法重蹈回调时代覆辙的地方。控制流看起来是顺序的，评审者便不再追问 continuation 在哪个线程上运行。结果协程在线程池线程上恢复，访问了只在发起 executor 上才安全的请求局部状态。

恢复策略应通过以下三种方式之一变得可见：

1. 任务类型自身携带明确的调度器或 executor 契约。
2. 代码在使用线程亲和资源前显式切换上下文。
3. 组件的设计使得 await 之后的代码不依赖特定 executor。

如果三者都不满足，这个协程就在依赖隐式的环境行为。而环境行为一旦重构就会失效。

## 协程不会消除错误边界设计

`co_await` 并不能回答”失败应该抛异常、返回 `std::expected`、请求取消，还是终止更大操作”这个问题。它只改变了控制流的形状。

第 3 章讨论的错误边界决策在任务 API 中依然适用，而且要保持一致：

1. 在内部层间栈展开可接受且被充分理解的领域，使用异常。
2. 当失败是预期中的、可组合的、属于正常业务流程时，使用结果类型。
3. 明确取消以何种形式呈现——在值空间、异常空间，还是任务状态中。
4. 让超时处理保持显式，而不是把它埋进一个行为出人意料的 awaiter 里。

失败模型应当在调用点可读。”这个协程可能挂起”远远不够，调用方还需要知道完成意味着什么、失败时会是什么样子。

## 生成器与任务解决的是不同问题

C++23 的协程支持同时涵盖了 pull 风格的生成器（generator，通过 `co_yield` 逐个产出值的协程）和异步任务，不要混淆二者。

生成器解决的是向本地消费者分阶段产出值的问题，适用于流式解析管线、分词、批量遍历或增量转换。核心关注点是迭代器有效性、生产者生命周期，以及 `co_yield` 出去的引用是否仍然有效。

任务解决的是异步工作最终完成的问题。核心关注点是所有权、调度、取消和结果传递。

二者共享底层机制，但需要不同的评审视角。生成器 bug 往往是”这个 yield 出去的引用到底指向哪里？”任务 bug 往往是”挂起之后这份工作归谁管？”区分这两类问题，代码评审就能更有针对性。

## 析构与取消必须能组合

协程的清理路径很容易被忽略，因为正常路径看起来就是一条直线。

试想：拥有者作用域在协程挂起时退出了，会怎样？析构会请求取消吗？会等待子操作完成吗？会不会和正常完成产生竞争？未完成的注册、文件描述符、定时器或缓冲区能否被恰好释放一次？

这些是任务抽象的语义契约，不是实现细节。

如果协程析构只是丢掉了 handle，而底层操作仍在别处继续，那就是"析构即 detach"。有时这确实是有意为之，但更多时候，它只是一个等着在关闭时爆发的 bug。

## 协程代码的验证

正常路径的单元测试不足以验证协程逻辑。验证应着重覆盖边界行为：

1. 生命周期测试——强制让调用方拥有的数据在恢复前消失。
2. 取消测试——在多个挂起点中断协程。
3. 调度器测试——让协程在非预期的 executor 上恢复，以暴露线程亲和性假设。
4. 失败路径测试——覆盖异常、错误结果和超时竞争场景。
5. Sanitizer 运行——当协程状态与共享对象交互时，检测 use-after-free 和数据竞争。

高价值组件通常值得编写确定性的测试 awaiter 或 fake scheduler，让恢复顺序可控。

## 协程的评审问题

在批准协程代码之前，问一问：

1. 协程帧里存了什么，由谁拥有？
2. 哪些借用的 view 或引用会跨越挂起点存活？
3. 任务启动后归谁所有？
4. 副作用何时开始——构造时、调度时，还是首次 await 时？
5. 恢复发生在什么上下文中？
6. 失败、超时和取消分别如何表达？
7. 如果发起方在挂起期间关闭了，会怎样？

如果这些问题的答案都含含糊糊，那这个协程并不比回调代码更简单，只是更容易被误读。

## 要点

协程改善的是控制流的清晰度，而非免除生命周期设计的责任。

把每个挂起点都视为一道边界，跨过它之后，所有权、恢复上下文和失败语义都必须依然成立。优先选择所有权和完成行为明确的任务类型。数据需要跨越挂起存活时，就把它移入协程帧。在概念上把生成器和异步任务区分开来。不要把看似顺序的源码等同于顺序的生命周期。协程的正确性取决于什么会跨越时间持续存在，而不是 `co_await` 链写得有多整齐。
