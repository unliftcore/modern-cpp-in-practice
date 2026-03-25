# 协程、任务与挂起边界

本章假定你已经理解失败传输和并发共享状态设计。这里的焦点更窄：协程拥有什么、什么能跨越挂起存活，以及当异步控制流隐藏生命周期时会出什么问题。

## 生产问题

协程让异步代码更容易读，也更容易撒谎。

一个原本嵌套回调的请求处理器可以变成直线式代码。一个流式解析器可以自然地 yield 值。后台刷新作业可以用 `co_await` 等待计时器和 I/O，而不是手写状态机。这些都是真实收益。但机制并没有消失。状态机依然存在。它现在住在协程帧里，而这个协程帧的生命周期、所有权和恢复上下文必须被有意设计。

生产环境中的协程故障通常有四种形状：

1. 借用的数据在挂起期间活得比它的来源更久。
2. 任务没有清晰所有者，因此工作活得比启动它的组件更久。
3. 失败路径和取消路径是隐式的，因此挂起的工作会在无效假设上恢复执行。
4. 执行会跨线程或 executor 跳转，而代码并没有让这一点足够明显。

本章把范围保持在局部。现在的问题还不是如何在取消压力下管理整棵任务树。那是第 14 章的内容。这里的问题是，每个协程到底是什么：一个带有挂起点、并以这些挂起点定义生命周期边界的资源拥有型对象。

## 协程替代了什么：回调地狱与手写状态机

要理解协程设计的权衡，先看看它们替代了什么。协程出现之前，异步代码依赖 continuation-passing style，也就是每一步都把回调链接到下一步。一个简单的“获取、校验、存储”序列会写成这样：

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

每一步都会嵌套得更深。错误处理在每一层重复。捕获值的生命周期必须手动管理：按值捕获会膨胀拷贝，按引用捕获则可能悬空。再加上超时、取消或重试逻辑，嵌套只会进一步放大。这不是稻草人；这就是生产代码库里真实的前协程异步 C++ 的形状。

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

阅读顺序是线性的，每一步只有一条失败路径，也没有嵌套。改进是真实的。但状态机并没有消失——它只是搬进了协程帧。本章剩余部分讨论的，就是这对所有权和生命周期意味着什么。

## 协程是带存储的状态机

把协程当作语法糖，是最快把生命周期 bug 带进生产环境的方法。

当一个函数变成协程时，一部分状态会搬进帧里。参数可能被拷贝或 move 到里面。那些跨越挂起存活的局部变量会驻留在里面。awaiter 的状态可能决定执行何时、以及在哪里恢复。析构可能发生在成功时、失败时、取消时，或者在拥有这个任务对象的对象被销毁时。这些都不是表面现象。

这很重要，因为普通的栈直觉不再可靠。对于非协程函数，局部变量会在控制流离开作用域时死亡。对于协程，一个跨越挂起的局部变量，可能会比调用方预期活得久得多；而一个指向调用方存储的借用 view，则可能在恢复之前很久就已经失效。

核心评审问题很简单：从一个挂起点到下一个挂起点，哪些数据必须保持有效？

## 挂起点就是生命周期边界

每个 `co_await` 都是一个应当重新检查日常假设的边界。

在挂起之前，先问：

1. 哪些引用、span、string view、迭代器和指针在之后仍然需要？
2. 它们引用的存储由谁拥有？
3. 被等待的操作是否可能比发起它的调用方、请求或组件活得更久？
4. 恢复会发生在哪个 executor 或线程上？

这是 API 生命周期评审在局部层面的对应物。如果协程在挂起期间保留借用数据，你就必须证明所有者活得比协程更久，或者把协程改成取得所有权。

这也是为什么协程 API 往往比同步 API 需要更严格的参数选择。一个同步 helper 安全地接收 `std::string_view` 是可能的，因为它会立刻完成。一个会挂起的异步任务通常不应该保留这个 view，除非所有权契约极其严格且有文档说明。

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

这段代码看起来很高效，因为它避免了一次拷贝。只有在调用方保证 `body` 在协程完成之前一直有效时，它才正确。在服务代码里，这通常意味着直到网络 I/O、认证查找、重试和超时处理全部结束为止。这不是一个小承诺。它通常就是错的承诺。

更安全的默认做法是：如果数据在挂起之后还要用，就把所有权 move 进协程。

```cpp
task<parsed_request> parse_and_authorize(
	std::string body,
	auth_context auth) {

	auto token = co_await fetch_access_token(auth.user_id());
	co_return parse_request(body, token);
}
```

拷贝或 move 的成本是可见、可评审的。协程帧现在拥有它所需的内容。如果这种分配成本真的重要，那就去测量，并围绕消息边界或存储复用重构。不要默默地跨时间借用。

## 更多生命周期陷阱：局部变量、临时对象和 lambda 捕获

上面的借用参数反模式是最常见的情况，但协程生命周期 bug 还有其他值得明确关注的形状。

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

当 `start_processing` 在 `co_await` 处挂起时，协程帧会让 `batch` 继续存活——但前提是协程帧本身还活着。如果任务被 detach，或者父作用域退出，协程帧就会被销毁，lambda 中的引用也会悬空。修复方式是：按值捕获，或者通过结构化所有权保证父作用域活得比被调度工作更久。

### 临时对象生命周期坍塌

```cpp
// BUG: temporary string destroyed before coroutine body executes.
task<void> log_message(std::string_view msg);

void caller() {
	log_message("request started"s + request_id()); // temporary std::string
	// temporary is destroyed here, before the coroutine even begins if lazy-start
}
```

对于 lazy-start 协程，这个临时 `std::string` 会在分号处销毁，而协程甚至还没有开始执行。即使是 eager-start 协程，如果帧把 `msg` 存成 `string_view`，那么在第一次挂起之后，它仍会指向已释放内存。解决方案是让协程签名按值接收 `std::string`，这样帧就拥有了一份副本。

### 挂起边界上的 `this` 指针

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

如果 `connection` 对象在 `run()` 挂起期间被移动进另一个容器，或者被销毁，那么恢复时 `this` 就无效了。成员协程只有在对象生命周期保证长于协程时才安全。在实践中，这通常意味着协程应该持有 `shared_ptr<connection>`，或者拥有者作用域必须被结构化设计，以防止挂起期间对象被销毁。

这些都不是异想天开的边角情况。它们就是生产环境里协程生命周期的常见失败模式。每个挂起点，都是调用方世界可能已经发生变化的时刻。

## 任务类型就是所有权契约

协程的返回类型不是装饰。它定义了所有权、结果传输和析构语义。

一个有用的任务类型，至少要回答这些问题：

1. 销毁任务会取消工作、detach 工作、阻塞等待，还是泄漏它？
2. 结果会被恰好观察一次、多次，还是根本不观察？
3. 异常如何传输？
4. 任务可以 eager start，还是只有被等待时才开始？
5. 取消是否有显式表示？

很多协程 bug 其实是任务类型 bug。一个 detached 的“fire-and-forget”协程，不是一种异步风格选择。它是一种所有权声明：后续代码不需要知道工作何时完成、是否失败，或者是否应在关闭时被取消。这个声明在生产服务里很少成立。

保守的默认做法很简单：每个已启动的任务都应有清晰所有者和可见完成路径。如果你说不出所有者是谁，你就在构建孤儿工作。

## Eager start 与 lazy start 会改变失败时机

一个协程是在创建后立刻开始运行，还是只有被等待时才开始，会影响正确性，而不只是性能。

eager task 可能在调用方存储 handle 之前就开始产生副作用。lazy task 则可能把工作推迟到编排代码决定其何时、何地运行的时候。两者都可能有效。重要的是，这种行为要一致，并且有文档说明。

这会影响失败边界。如果任务构造就可能启动工作，那么在任何父作用域认为该任务“已激活”之前，异常和取消就可能已经变得可观察。如果工作只在第一次 await 或显式调度时启动，所有权通常更容易推理。

对生产代码的建议不是一刀切地偏向某一种策略，而是：任务抽象必须让这种策略足够明显，明显到评审者不需要去检查 promise type 内部实现，就能知道副作用何时开始。

## 恢复上下文是正确性的一部分

协程代码经常读起来像是始终待在同一个线程上。这是一种错觉。

一个被等待的操作可能会在 I/O 线程、调度器线程池、UI 亲和线程，或 awaiter 选择的 executor 上恢复。如果代码在恢复后访问线程限制的状态，或者它要求继续在某个特定 executor 上运行，那么这个要求就必须在抽象中显式体现。

这正是团队用更漂亮的语法重现回调时代 bug 的地方。控制流看起来是顺序的，于是评审者不再问 continuation 会在哪儿运行。然后协程就在一个线程池线程上恢复，并访问了只对发起 executor 安全的请求局部状态。

让恢复策略通过以下三种方式之一可见：

1. 任务类型携带清晰的调度器或 executor 契约。
2. 代码在使用线程亲和资源前显式切换上下文。
3. 组件被设计成 post-await 代码与 executor 无关。

如果三者都不成立，这个协程就在依赖环境行为。环境行为一旦重构就会失效。

## 协程不会消除错误边界设计

`co_await` 并不能回答失败应当抛异常、返回 `std::expected`、请求取消，还是终止更大操作。它只改变了控制流形状。

这意味着第 3 章里的错误边界决策在任务 API 中依然适用：

1. 对那些可以接受且已经充分理解在内部层之间进行栈展开的领域，使用异常。
2. 当失败是预期的、可组合的，并属于普通业务流程时，使用结果类型。
3. 决定取消如何出现在值空间、异常空间或任务状态中。
4. 让超时处理保持显式，而不是把它埋进一个带有意外策略的 awaiter。

失败模型应当在调用点可读。“这个协程可能挂起”并不是足够信息。调用方还需要知道，完成意味着什么，以及失败完成是什么样。

## 生成器与任务解决的是不同问题

C++23 协程支持同时启用了 pull 风格生成器和异步任务。不要把它们混在一起。

生成器关心的是，在本地消费者存在的情况下分阶段产出值。它们通常适合流式解析 pipeline、tokenization、批量遍历或增量转换。它们主要关注迭代器有效性、生产者生命周期，以及 yield 出去的引用是否仍然有效。

任务关心的是异步工作的最终完成。它们关注的是所有权、调度、取消和结果传输。

它们共享一套机制，却需要不同的评审问题。生成器 bug 常常是“这个 yield 出去的引用到底指向什么？”任务 bug 常常是“挂起之后谁拥有这份工作？”把这两类问题分开，会让代码评审更锋利。

## 析构与取消必须能组合

协程清理路径很容易被忽略，因为成功路径看起来是线性的。

要问：如果拥有者作用域在协程挂起时退出，会发生什么？析构是否会请求取消？它是否等待子操作？它会不会与完成竞争？未完成的注册、文件描述符、计时器或 buffer 是否会被恰好释放一次？

这些都不是实现细节。它们是任务抽象的语义契约。

如果协程析构只是丢掉 handle，而底层操作仍在别处继续进行，那就是通过析构进行 detach。有时这是刻意设计。更多时候，这只是一个等待发生的关闭 bug。

## 协程代码的验证

只用成功路径单元测试来测试协程逻辑是不够的。验证应针对边界行为：

1. 生命周期测试：强制让调用方拥有的数据在恢复前消失。
2. 取消测试：在多个挂起点中断协程。
3. 调度器测试：让协程在意外 executor 上恢复，以捕获线程亲和假设。
4. 针对异常、错误结果和超时竞争的失败路径测试。
5. 当协程状态与共享对象交互时，运行 use-after-free 和竞争检测 sanitizer。

对于高价值组件，通常值得编写可确定的测试 awaiter 或 fake scheduler，这样恢复顺序就可以被控制，而不是靠猜。

## 协程的评审问题

在批准协程代码之前，先问：

1. 协程帧里活着什么，由谁拥有？
2. 哪些借用 view 或引用会跨越挂起存活？
3. 任务启动后由谁拥有？
4. 副作用从何时开始：构造时、调度时，还是第一次 await 时？
5. 恢复发生在什么上下文上？
6. 失败、超时和取消是如何表示的？
7. 如果发起组件在挂起中途关闭，会发生什么？

如果这些答案都很模糊，那么这个协程并不比回调代码更简单。它只是更容易被误读。

## 要点

协程改善的是控制流清晰度。它们不会消除生命周期设计。

把每个挂起点都当作边界，在那里所有权、恢复上下文和失败语义都必须仍然讲得通。优先选择那些拥有明确所有权和完成行为的任务类型。当数据必须跨挂起存活时，把它 move 进协程帧。把生成器和异步任务在概念上分开。最重要的是，不要把看起来顺序的源码误认为顺序的生命周期。协程正确性取决于什么东西会跨时间持续存在，而不是 `co_await` 链看起来有多整洁。
