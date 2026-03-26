# 所有权、生命周期与 RAII

写现代 C++ 时，首先要面对的生产问题不是”这该不该做成一个类”，也不是”这里能不能零拷贝”。问题更简单，也更危险：谁拥有这个资源？它能保持有效多久？当正常流程走不通了，谁来保证清理？

这个问题首先针对内存，但内存只是冰山一角。真实系统需要管理的资源远不止于此：socket、文件描述符、互斥量、线程 join、临时目录、遥测注册、进程句柄、映射文件、事务作用域、关闭钩子……C++ 给了你充分的自由，也给了你充分的机会把这些全都搞砸。之所以把所有权放在第一章，不是因为它在学术上很基础，而是因为所有权一旦不清晰，后面的设计就根本无法放心地做评审。

在生产环境中，代价高昂的故障往往不会在调用点上表现得很显眼。一个服务启动了后台 flush，捕获了指向请求状态的裸指针，部署时偶尔崩一下。连接池在错误的线程上被关闭，因为最后一个 `shared_ptr` 恰好在一个没人当成关闭流程的回调里被释放。初始化路径构建了三个资源才建了一半，第四个抛异常时第二个就泄漏了。这些都不是语法问题——它们是所有权问题，最终演变成了运维事故。

RAII 至今仍是现代 C++ 能把这些情况处理干净的核心原因。它不是靠惯性留下来的老古董，而是一种让资源生命周期能与作用域、异常、提前返回和部分构造自然组合的机制。用好了，RAII 会让清理变得毫无存在感——这正是你想要的效果。

## 所有权必须一目了然

所有权是一种契约，不是实现细节。评审者应该能指着任意一个类型或成员，迅速回答三个问题：

1. 这个对象拥有哪些东西？
2. 它暂时借用了什么？
3. 什么事件标志着所拥有资源的生命周期结束？

如果回答这些问题需要翻好几个辅助函数，说明设计已经过于隐式了。

正因如此，现代 C++ 偏爱那些所有权语义一目了然的类型。`std::unique_ptr<T>` 意味着独占所有权；`std::shared_ptr<T>` 意味着引用计数的共享所有权；普通对象成员意味着外层对象直接拥有这个子对象；`std::span<T>` 或 `std::string_view` 意味着借用而非持有。这些不是风格偏好，而是程序表达生命周期的手段。

反面的风格大家都见过，代价也不低：一个裸指针成员，可能代表拥有、可能代表观察，有时候还会因为正在关闭而变成 null。这种设计写起来省事，理解起来要命。

## RAII 关心的是资源，而不是 `new`

很多程序员第一次接触 RAII，是通过”用智能指针代替手写 `delete`”这句话。方向大致没错，但格局太小了。

RAII 的本质是：把资源绑定到一个对象的生命周期上，由该对象的析构函数负责释放。资源可以是内存，但也完全可以是文件描述符、内核事件、事务锁，或者必须在关闭完成前注销的指标注册。

### 没有 RAII 会发生什么

在展示 RAII 模式之前，有必要先完整看一遍更接近真实情况的手工方式。下面这个反面示例是故意写得有缺陷的，因为生产代码库里至今仍有长得一模一样的代码。

```cpp
socket_t create_server_socket(std::uint16_t port) {
	socket_t server = ::socket(AF_INET, SOCK_STREAM, 0);
	if (server == invalid_socket) {
		throw NetworkError{"socket failed"};
	}

	int opt = 1;
	if (::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		::close_socket(server);
		throw NetworkError{"setsockopt failed"};
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
		::close_socket(server);
		throw NetworkError{"bind failed"};
	}

	if (::listen(server, 16) < 0) {
		::close_socket(server);
		throw NetworkError{"listen failed"};
	}

	return server; // RISK: caller now owns the raw descriptor by convention
}

void serve_once(std::uint16_t port) {
	socket_t server = create_server_socket(port);
	socket_t client = invalid_socket;

	try {
		sockaddr_in client_addr{};
		socket_length addr_len = sizeof(client_addr);
		client = ::accept(server,
		                  reinterpret_cast<sockaddr*>(&client_addr),
		                  &addr_len);
		if (client == invalid_socket) {
			::close_socket(server); // BUG: server will be closed twice (here + in catch)
			throw NetworkError{"accept failed"};
		}

		std::array<char, 8192> buffer{};
		auto n = read_from_socket(client, buffer.data(), buffer.size());
		if (n <= 0) {
			::close_socket(client);
			::close_socket(server);
			return;
		}

		process_request(client, std::string_view{buffer.data(), static_cast<std::size_t>(n)}); // RISK: any throw must preserve cleanup correctness

		::close_socket(client);
		::close_socket(server);
	} catch (...) {
		if (client != invalid_socket) {
			::close_socket(client);
		}
		::close_socket(server);
		throw;
	}
}
```

问题会迅速叠加起来：

1. **清理逻辑会重复。** `::close_socket(server)` 同时出现在设置 helper、正常路径、提前返回路径和异常路径里。退出点越多，重复就越多。

2. **重复最终会变成 bug。** `accept` 失败路径在抛异常前已经关闭了 `server`，而 `catch` 块又会再关一次。手工所有权逻辑在维护过程中很容易就会漂移成这样。

3. **异常安全依赖纪律。** `process_request` 可能抛异常。以后只要有人在“获取资源”和“手工清理”之间多插一段代码，就必须重新想一遍当时哪些描述符还活着。

4. **转移是隐式的。** `create_server_socket()` 返回的是裸 `socket_t`，所以所有权只能靠调用方和被调方之间的约定维持，而不是由类型系统表达。

5. **评审变成全局推理。** 想确认代码正确，评审者就得检查整段函数，确认每一条退出路径都把每个描述符恰好关闭一次。

RAII 方案从结构上消除了这些问题：每个资源都由一个拥有它的对象持有，析构函数负责释放，剩下的交给栈展开。

本书配套的 web-api 示例项目里已经有我们真正想讲的例子。`examples/web-api/src/modules/http.cppm` 中的 `Socket` 类包装了一个文件描述符，并把所有权规则直接写进了类型里：

```cpp
// From examples/web-api/src/modules/http.cppm
class Socket {
public:
    Socket() = default;
    explicit Socket(socket_handle fd) noexcept : fd_{fd} {}

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept
        : fd_{std::exchange(other.fd_, invalid_socket_handle)} {}

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close(); // release what this object currently owns
            fd_ = std::exchange(other.fd_, invalid_socket_handle);
        }
        return *this;
    }

    ~Socket() { close(); } // automatic release on every exit path

    [[nodiscard]] socket_handle fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ != invalid_socket_handle; }
    explicit operator bool() const noexcept { return valid(); }

    void close() noexcept {
        if (fd_ != invalid_socket_handle) {
            close_socket(fd_);
            fd_ = invalid_socket_handle;
        }
    }

private:
    socket_handle fd_{invalid_socket_handle};
};
```

光这个类就足够把 RAII 的核心讲清楚：

- **获取** 发生在构造时：`Socket sock{::socket(...)};`
- **所有权是独占的**，因为复制被禁用了。
- **转移是显式的**，因为移动通过 `std::exchange` 把源对象清空。
- **释放是自动的**，因为析构函数总会调用 `close()`。

同一个模块中的周边代码还展示了它在真实使用中是如何工作的。下面是一段局部摘录：只保留了与所有权相关的代码，辅助声明和无关的错误处理细节为了便于阅读被省略了。

```cpp
[[nodiscard]] Socket create_server_socket() const {
    Socket sock{::socket(AF_INET, SOCK_STREAM, 0)}; // ownership starts here
    if (!sock) return {};

    int opt = 1;
    if (::setsockopt(sock.fd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        return {}; // sock is destroyed here, so the descriptor closes automatically
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    if (::bind(sock.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return {}; // same: failure path still releases the descriptor
    }

    if (::listen(sock.fd(), 16) < 0) {
        return {};
    }

    return sock; // move or copy elision transfers ownership to the caller
}

Socket client{::accept(server_sock.fd(), ...)}; // accepted socket now has an owner
handle_connection(std::move(client));           // explicit ownership transfer

void handle_connection(Socket client) const {
    std::array<char, 8192> buf{};
    auto n = read_from_socket(client.fd(), buf.data(), buf.size());
    if (n <= 0) return;

    Response resp = handler_(req); // request parsing omitted here
    auto data = resp.serialize();
    (void)write_to_socket(client.fd(), data.data(), static_cast<int>(data.size()));
} // client goes out of scope here and closes automatically
```

在 `handle_connection(std::move(client))` 之后，调用方那边的 `client` 就不再拥有这个描述符了。移动构造函数已经把它的文件描述符交换成 `invalid_socket_handle`，所以之后这个被移动过的对象析构时也不会出事。任意时刻，所有权都只存在于一个对象里。

注意那些消失掉的东西：没有清理阶梯，没有主要职责是收尾的 `try`/`catch`，也没有“这个描述符现在到底归谁管”的口头约定。类型本身就携带了策略。这才是 RAII 的现实价值。

同样的模式适用于各种非内存资源。作用域注册令牌在析构时注销；事务对象不显式提交就回滚；joined-thread 包装器在析构时 join，或者拒绝在仍可 join 的状态下被销毁。一旦代码库建立起这种思维方式，清理路径就重新回归局部，不再散落于各处错误处理中。

## 反模式：靠约定清理

RAII 的替代方案通常不是”手工清理但做到完美无缺”，而是靠约定来清理——这就意味着一有压力，清理就会被跳过。

```cpp
// Anti-pattern: ownership and cleanup are split across control flow.
void publish_snapshot(Publisher& publisher, std::string_view path) {
auto* file = ::open_config(path.data());
if (file == nullptr) {
throw ConfigError{"open failed"};
}

auto payload = read_payload(file);
if (!payload) {
::close_config(file); // BUG: one exit path remembered cleanup
throw ConfigError{"parse failed"};
}

publisher.send(*payload); // BUG: if this throws, file leaks
::close_config(file);
}
```

问题不在于手工清理难看，而在于它根本就是错的——清理策略被拆散到了每条退出路径里。函数一旦要管第二个、第三个资源，控制流就会变得比函数实际要做的工作更难审计。

RAII 版本消除了每一次手工释放和每一条条件清理路径：

```cpp
void publish_snapshot(Publisher& publisher, std::string_view path) {
auto file = ConfigFile::open(path); // RAII: destructor calls ::close_config
if (!file) {
throw ConfigError{"open failed"};
}

auto payload = read_payload(*file);
if (!payload) {
throw ConfigError{"parse failed"};
// file releases automatically -- no manual cleanup needed
}

publisher.send(*payload);
// file releases automatically at scope exit, whether normal or exceptional
}
```

现在函数只需关心一件事：业务逻辑本身。清理不可见，因为它已经被保证了。哪怕再加第三、第四、甚至第十条退出路径，资源安全性也不受影响。这种可组合性才是 RAII 的真正价值所在——不是代码更好看，而是在持续维护的压力下依然正确。

RAII 把释放策略移入了拥有资源的对象，从根源上消除了”靠约定清理”的隐患。错误路径于是可以回归本职：描述失败本身，而不是操心拆除过程。

## 独占所有权应当是默认选择

在设计良好的系统中，大多数资源在任意时刻都只有一个显而易见的所有者。请求对象拥有解析后的载荷，连接对象拥有它的 socket，批处理拥有它的 buffer。独占所有权理应成为默认的思维方式。

落实到代码中，就是优先使用普通对象成员，无法直接内嵌时再用 `std::unique_ptr`。`unique_ptr` 并不意味着设计有多复杂，它只是说明所有权的转移和销毁都是显式的。它与容器、工厂和错误路径的组合也很自然，因为 moved-from 状态是良定义的，单一所有权自始至终保持单一。

共享所有权应该是有意为之的例外。确实有合理的场景：异步扇出中多个组件需要让同一份不可变状态保持存活；图状结构中确实存在共享生命周期；缓存条目在多个使用者持有期间必须有效。但 `shared_ptr` 不是万能的安全毯。它会改变销毁时机，在许多实现中带来原子引用计数的开销，还经常掩盖真正的问题：为什么没有任何一个组件能明确充当所有者？

评审中如果在边界处发现了 `shared_ptr`，应该追问一个具体的问题：究竟是什么样的生命周期关系，让独占所有权行不通？如果答案含糊不清，那这个共享所有权多半只是在给一个从未想清楚资源归属的设计打补丁。

一个常见的症状是关闭时机的不确定性。当最后一个持有资源的 `shared_ptr` 从某个不可预期的回调或线程中被释放，析构函数就会在不可预期的时间和位置执行：

```cpp
// Risky: destruction timing depends on which callback finishes last.
void start_fanout(std::shared_ptr<Connection> conn) {
for (auto& shard : shards_) {
shard.post([conn] {           // each lambda extends lifetime
conn->send(shard_ping()); // last lambda to finish destroys conn
});
}
// conn may already be destroyed here, or may live much longer --
// depends on thread scheduling. Destructor side effects (logging,
// metric flush, socket close) now happen at an uncontrolled point.
}
```

当销毁顺序很重要时——在生产环境中几乎总是如此——应优先使用 `unique_ptr` 配合显式的生命周期作用域，把非拥有的裸指针或引用传给那些确保在所有者存活期内完成的工作。

## 借用比拥有需要更严格的纪律

所有权关系再清晰的系统，也少不了非拥有式的访问。算法需要检查调用方拥有的 buffer，验证逻辑需要读取请求元数据，迭代器和视图需要在不拷贝的前提下遍历存储。借用是正常的，错误在于让借用的状态活过了所有者，或者让借用关系变得不可见。

现代 C++ 提供了表达借用的类型工具：引用、明确用作观察者的指针、`std::span` 和 `std::string_view`。它们很有帮助，但光靠类型本身不能保证设计正确。一个长生命周期对象里的 view 成员，如果真正的所有者在别处，仍然是生命周期隐患。回调捕获了栈上状态的引用，延后执行时照样出问题。

并发场景下风险更大。被捕获进后台工作的裸指针或 `string_view` 绝非无害的小优化——它是跨越时间的借用，有效性取决于调度时序和关闭顺序。

由此得出一条简明的所有权准则：拥有类型可以自由跨越时间边界；借用类型只有在所有者明显比使用方更强壮、更长命时，才可以跨越时间边界。如果你无法迅速证明这一点，就应该选择拷贝或转移所有权。

## 移动语义定义的是转移，而非单纯的优化

移动语义通常被当作性能话题来讲，但在实践中，它首先是一个所有权话题。

对一个对象执行移动，就是宣告资源换了主人：源对象依然有效，但不再对原来的资源负责。正因如此，工厂、容器和流水线各阶段才能在不为每个类型另造一套转移 API 的前提下组合起来。

对于资源拥有类型，良好的移动行为是其正确性保障的一部分：

- 移动目标成为新的所有者。
- 移动源仍然可以析构和赋值。
- 不会发生重复释放。

这也是为什么专门写一层薄薄的资源包装器是值得的。所有权转移规则一旦内化到类型中，调用方就不用再手工转移裸句柄、然后祈祷约定能对上号了。

并非所有类型都该可移动，也并非每次移动都很便宜。互斥量通常既不可复制也不可移动，因为移动会让不变量和平台语义变得复杂。一个直接持有大 buffer 的聚合类型可能是可移动的，但在热路径上开销仍然不小。设计时该问的不是”能不能给移动操作加 `= default`”，而是”这个类型应该允许怎样的所有权语义”。

## 生命周期 Bug 往往藏在关闭和部分构造中

程序员习惯在正常工作路径上思考生命周期，但生产中的 Bug 却往往出现在启动失败和关闭阶段。

部分构造就是典型的例子。如果一个对象需要获取三个资源，第二个获取时抛了异常，第一个资源仍然必须正确释放。只要把所有权分层到各个成员中，而不是在构造函数体内靠清理标志手动处理，RAII 就能自动搞定这件事。

手工方式的脆弱性一目了然：

```cpp
// Anti-pattern: manual multi-resource construction with cleanup flags.
class Pipeline {
public:
Pipeline(const Config& cfg) {
db_ = ::open_db(cfg.db_path().c_str());
if (!db_) throw InitError{"db open failed"};

cache_ = ::create_cache(cfg.cache_size());
if (!cache_) {
::close_db(db_); // must remember to clean up db_
throw InitError{"cache alloc failed"};
}

listener_ = ::bind_listener(cfg.port());
if (listener_ == invalid_socket) {
::destroy_cache(cache_); // must remember both prior resources
::close_db(db_);
throw InitError{"bind failed"};
}
}

~Pipeline() {
::close_listener(listener_);
::destroy_cache(cache_);
::close_db(db_);
}

private:
db_handle_t db_ = nullptr;
cache_handle_t cache_ = nullptr;
socket_t listener_ = invalid_socket;
};
```

每往构造函数里多加一个资源，前面所有失败分支都得跟着改。一旦有人调整了获取顺序，清理逻辑就会悄悄坏掉。

RAII 版本用成员包装器解决问题，依赖的是一条语言规则：构造函数抛出异常时，已经构造好的成员会被自动销毁：

```cpp
class Pipeline {
public:
Pipeline(const Config& cfg)
: db_(DbHandle::open(cfg.db_path()))       // destroyed automatically if
, cache_(Cache::create(cfg.cache_size()))   // a later member throws
, listener_(Listener::bind(cfg.port())) {}

private:
DbHandle db_;
Cache cache_;
Listener listener_;
};
```

没有清理标志，没有级联 `if`，没有依赖顺序的手工拆除。语言替你完成了这一切。

本书配套的 web-api 示例项目中，`main.cpp` 展示了这一原则在完整服务启动中的应用。每一层都作为 `main()` 中的局部变量构造，栈的自然析构顺序负责拆除：

```cpp
// 摘自 examples/web-api/src/main.cpp（简化）
int main() {
    webapi::TaskRepository repo;                       // 1. 领域对象
    webapi::Router router;                             // 2. 路由表
    router.get("/tasks", webapi::handlers::list_tasks(repo));

    auto handler = webapi::middleware::chain(           // 3. 中间件
        pipeline, router.to_handler());

    webapi::http::Server server{port, std::move(handler)}; // 4. 服务器
    server.run_until(shutdown_requested);
    // 析构按反序展开：server, handler, router, repo
}
```

整段代码中没有任何显式的拆除逻辑。如果任何一步构造抛出异常，之前已构造的对象都会按反序自动销毁——这正是 RAII `Pipeline` 模式所依赖的保证。

关闭阶段是另一个主要压力点。析构函数运行时，系统往往已经处于状态切换之中——后台工作可能还持有引用，日志基础设施可能已经部分拆除。如果一个析构函数会无限阻塞、回调到不稳定的子系统、或者依赖某种从未写进文档的线程亲和性，就可能把原本整洁的所有权模型变成部署期故障。

教训不是要害怕析构函数，而是要让析构函数的职责尽可能窄、尽可能明确。释放你拥有的资源，不要搞出意外的跨子系统操作。如果拆除工作需要比单纯析构更复杂的协议，就提供显式的 `stop` 或 `close` 方法，把析构函数作为最后的安全兜底，而不是唯一的清理手段。

## 验证与评审

所有权设计需要有意识地纳入评审流程，因为很多生命周期 Bug 在跑任何工具之前，就已经能从结构上看出端倪。

评审时值得关注的问题：

1. 每个资源是否都有一个单一、明确的所有者？
2. 借用的引用和视图，生命周期是否明显短于所有者？
3. `shared_ptr` 是在解决真实的共享生命周期需求，还是在回避所有权决策？
4. 移动操作是否保持了单一所有权和安全销毁？
5. 关闭流程是否依赖了超出资源释放范围的析构副作用？

动态工具仍然不可或缺。AddressSanitizer 能捕获大量 use-after-free 问题；Leak Sanitizer 和平台诊断工具能发现遗漏的释放路径；ThreadSanitizer 在生命周期错误因关闭期间的竞态条件而暴露时尤其有用。不过，只有当类型系统本身已经把所有权表达清楚时，这些工具才最有效。

## 要点

- 把所有权当作契约来对待——它必须在类型和对象结构中清晰可见。
- 对每一种有意义的资源都使用 RAII，而不仅仅是堆内存。
- 默认优先独占所有权；选择共享所有权时，要给出明确的理由。
- 先把移动语义理解为所有权转移规则，再把它当作性能优化手段。
- 对关闭路径和部分构造路径的审查力度，应该和对稳态运行路径一样。

如果一个资源可能泄漏、重复释放、销毁后仍被访问，或在错误的线程上销毁，那问题往往早在崩溃发生之前就埋下了——埋在所有权被留为隐式的那一刻。
