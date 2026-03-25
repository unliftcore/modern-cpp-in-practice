# 所有权、生命周期与 RAII

现代 C++ 的第一个生产问题，不是“这是否应该是一个类”，也不是“这里能不能零拷贝”。它更简单，也更危险：谁拥有这个资源，它会保持有效多久，以及当快乐路径不再快乐时，什么来保证清理。

这个问题适用于内存，但内存只是故事的一部分。真实系统拥有 socket、文件描述符、互斥量、线程 join、临时目录、遥测注册、进程句柄、映射文件、事务作用域和关闭钩子。语言给了你足够多的绳子，让你把所有这些都表示得很糟。所有权值得成为第一章，不是因为它在学术意义上是基础，而是因为不清晰的所有权会让其余设计无法被有把握地评审。

在生产环境中，昂贵的失败很少在调用点上表现得戏剧化。某个服务启动了后台 flush，捕获了一个指向请求状态的裸指针，并在部署期间偶发崩溃。某个连接池在错误的线程上关闭，因为最后一次 `shared_ptr` 释放发生在一个没人认为属于关闭流程的回调里。某条初始化路径构造了一半的三个资源，并在第四个抛异常时泄漏了第二个。这些不是语法问题，而是演变成运维事故的所有权问题。

RAII 仍然是现代 C++ 能够干净处理这些情况的主要原因。它不是一个因习惯而存活下来的旧习语。它是一种机制，让资源生命周期能够与作用域、异常、提前返回和部分构造组合。用得好时，RAII 会让清理变得无聊。而这正是你想要的。

## 所有权必须可读

所有权是一种契约，不是实现细节。评审者应该能够指着一个类型或成员，快速回答三个问题。

1. 这个对象拥有些什么？
2. 它可能暂时借用什么？
3. 什么事件会结束所拥有资源的生命周期？

如果答案需要阅读几个辅助函数，设计就已经过于隐式了。

这正是现代 C++ 偏爱那些所有权行为一眼可见的类型的原因。`std::unique_ptr<T>` 表示独占所有权。`std::shared_ptr<T>` 表示带引用计数生命周期的共享所有权。普通对象成员表示包含它的对象直接拥有这个子对象。`std::span<T>` 或 `std::string_view` 表示借用，而不是保留。这些不是风格偏好，而是程序传达生命周期的方式的一部分。

相反的风格也很熟悉，而且昂贵：一个原始指针成员，可能拥有、可能观察，而且有时还可能为 null，因为关闭正在进行中。这种设计写起来便宜，推理起来昂贵。

## RAII 关心的是资源，而不是 `new`

很多程序员第一次接触 RAII，是通过“用智能指针代替手工 `delete`”。这个方向大体没错，但规模小得多。

RAII 的含义是，把一个资源绑定到某个对象的生命周期上，并由该对象的析构函数释放它。这个资源可能是内存。它也完全可能是文件描述符、内核事件、事务锁，或者一个必须在关闭完成前注销的指标注册。

### 没有 RAII 会发生什么

在说明 RAII 模式之前，值得先完整看一下手工方式，因为生产代码库里仍然存在看起来完全像这样的代码。

```cpp
// Manual resource management: C-style socket handling.
void serve_request(const Config& config) {
socket_t sock = ::open_socket(config.port());
if (sock == invalid_socket) {
throw NetworkError{"bind failed"};
}

char* buffer = new char[config.buffer_size()]; // second resource

auto* sub = event_bus::subscribe("health_check"); // third resource

// -- any throw between here and the cleanup block leaks all three --

process(sock, buffer, sub); // may throw

event_bus::unsubscribe(sub);
delete[] buffer;
::close_socket(sock);
}
```

问题会迅速叠加：

1. **异常不安全。** 如果 `process` 抛异常，或者 `event_bus::subscribe` 在 buffer 分配后抛异常，前面获取的资源就会悄悄泄漏。为每次获取都添加 `try`/`catch` 会形成层层嵌套的清理梯子，既痛苦又容易出错。

2. **double-free 风险。** 如果一次维护性修改添加了一个提前返回或第二条调用路径，开发者可能会对 `buffer` 调用两次 `delete[]`，或者对已经关闭的句柄调用 `::close_socket`。这两类失败都不会在编译期被捕获。

3. **依赖顺序的拆除。** 每向函数里加入一个新资源，开发者就必须更新每一条退出路径。只有三个资源时，就已经存在多种部分获取状态的组合。到五六个资源时，清理逻辑会比函数本身的工作还显眼。

4. **在维护下很脆弱。** 代码评审无法局部验证清理正确性。评审者必须追踪函数中的每一条路径，确认每个资源都恰好释放一次。这无法扩展。

RAII 替代方案从结构上消除这些问题。每个资源都由一个拥有对象持有，其析构函数执行释放。剩下的事交给栈展开。

考虑一个服务组件，它需要一个 socket 和一个对内部事件流的临时订阅。

```cpp
class SocketHandle {
public:
SocketHandle() = default;
explicit SocketHandle(socket_t fd) noexcept : fd_(fd) {}

SocketHandle(const SocketHandle&) = delete;
auto operator=(const SocketHandle&) -> SocketHandle& = delete;

SocketHandle(SocketHandle&& other) noexcept
: fd_(std::exchange(other.fd_, invalid_socket)) {}

auto operator=(SocketHandle&& other) noexcept -> SocketHandle& {
if (this != &other) {
reset();
fd_ = std::exchange(other.fd_, invalid_socket);
}
return *this;
}

~SocketHandle() { reset(); }

auto get() const noexcept -> socket_t { return fd_; }

void reset() noexcept {
if (fd_ != invalid_socket) {
::close_socket(fd_);
fd_ = invalid_socket;
}
}

private:
socket_t fd_ = invalid_socket;
};
```

这个类型的存在，不是为了展示 move 语义，而是为了让 socket 的所有权变得单一、显式且异常安全。析构函数就是清理策略。move 操作定义转移。复制被禁止，因为复制所有权是错误的。

同样的模式适用于许多非内存资源。作用域注册令牌在析构时注销。事务对象除非被显式提交，否则就回滚。joined-thread 包装器会在析构时 join，或者拒绝在仍可 join 时被销毁。一旦代码库按这种方式思考，清理路径就重新变得局部，而不是散落在错误处理里。

## 反模式：靠约定清理

RAII 的替代方案通常不是“显式手工清理并且做得完美”。通常是靠约定清理，而这意味着清理会在压力下被跳过。

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

这不是因为手工清理丑陋才有争议。它之所以错误，是因为清理策略现在和每一条退出路径交织在一起。一旦函数获得第二个或第三个资源，控制流就会变得比函数真正执行的工作更难审计。

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

现在这个函数只有一个关注点：它真正的逻辑。清理之所以不可见，是因为它已经被保证。增加第三、第四或第十条退出路径，都不会改变资源安全性。这种可组合性才是 RAII 的真正收益——不是代码更好看，而是在维护压力下依然正确。

RAII 通过把释放策略移入拥有对象，修复了“靠约定清理”的问题。于是错误路径就能重新专注于自己的主要职责：描述失败，而不是描述拆除过程。

## 独占所有权应当是默认值

设计良好的系统中，大多数资源在任意时刻都只有一个显而易见的所有者。请求对象拥有它解析后的载荷。连接对象拥有它的 socket。批处理拥有它的 buffer。这就是为什么独占所有权是正确的默认心智模型。

在实践中，这意味着当无法直接内嵌时，优先使用普通对象成员或 `std::unique_ptr`。`unique_ptr` 不是设计很复杂的信号，而是所有权转移和销毁都显式的信号。它也能很好地与容器、工厂和错误路径组合，因为 moved-from 状态是定义良好的，单一所有权仍然保持单一。

共享所有权应被视为刻意的例外。确实存在有效场景：异步扇出中，多个组件必须让同一份不可变状态保持存活；具有真实共享生命周期的图状结构；多个用户仍持有条目时条目仍需有效的缓存。但 `shared_ptr` 不是通用的安全毯。它会改变销毁时机，在许多实现中增加原子的引用计数流量，而且经常掩盖真正的问题：为什么没有哪个组件能指名这个所有者？

如果评审在某个边界上发现 `shared_ptr`，后续问题应该非常具体：这里是哪种生命周期关系让独占所有权不可能成立？如果答案很模糊，这种共享所有权多半是在为一个从未决定资源归属位置的设计打补丁。

一个常见症状是关闭时的非确定性。当资源的最后一个 `shared_ptr` 从不可预测的回调或线程中被释放时，析构函数也会在不可预测的时间和地点运行：

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

当销毁顺序很重要时——而在生产环境中它几乎总是重要——应优先使用 `unique_ptr` 配合显式生命周期作用域，并把非拥有的裸指针或引用传递给那些保证会在所有者生命周期内完成的工作。

## 借用比拥有需要更严格的约束

一个拥有关系清晰的系统仍然需要非拥有访问。算法检查调用方拥有的 buffer。验证逻辑读取请求元数据。迭代器和视图在不拷贝的前提下遍历存储。借用是正常的。错误在于让借用状态活得比所有者更久，或者让借用不可见。

现代 C++ 提供了有用的借用词汇：引用、明确作为观察者使用的指针、`std::span` 和 `std::string_view`。这些类型很有帮助，但它们本身并不能强制一个好设计。长生命周期对象里的 view 成员，如果所有者在别处，仍然是生命周期风险。回调若捕获了对栈状态的引用，在稍后运行时仍然是错误的。

在并发下，这种风险会更严重。被捕获进后台工作的裸指针或 `string_view`，并不是什么小的局部捷径。它是跨时间的借用，它的有效性现在取决于调度和关闭顺序。

因此，一条有用的所有权规则很简单：拥有类型可以自由跨越时间；借用类型只有在所有者显然更强、生命周期也显然长于使用借用的工作时，才应该跨越时间。如果你无法快速讲清这个论证，就应该拷贝或转移所有权。

## move 语义定义的是转移，而不只是优化

move 语义通常作为性能话题被介绍。在实践中，它首先是一个所有权话题。

move 一个对象，就是声明资源更换了所有者，而源对象仍然有效，但不再对旧资源负责。正因如此，工厂、容器和流水线阶段才能在不为每个类型发明定制转移 API 的前提下组合起来。

对于资源拥有类型，良好的 move 行为是该类型正确性叙事的一部分。

- 接收 move 的对象成为所有者。
- 被 move 的对象仍然可以析构和赋值。
- 不会发生重复释放。

这也是为什么直接的资源包装器值得付出那一点代码量。一旦所有权转移规则存在于类型里，调用方就不再需要手工转移原始句柄，并祈祷约定能对得上。

并非每个类型都应该可移动，也并非每次 move 都便宜。互斥量通常既不可复制也不可移动，因为移动它会让不变量和平台语义复杂化。一个拥有直接 buffer 的大型聚合体可能可移动，但在热路径上仍不便宜。设计问题不是“我能不能默认 move 操作”，而是“这个类型应该允许什么样的所有权故事”。

## 生命周期缺陷经常隐藏在关闭和部分构造中

程序员倾向于在主工作路径上思考生命周期。生产缺陷却常常出现在启动失败和关闭阶段。

部分构造就是一个例子。如果一个对象获取三个资源，而第二次获取抛出异常，第一个资源仍然必须被正确释放。当所有权被分层到成员中，而不是在构造函数体内借助清理标志手工处理时，RAII 会自动完成这件事。

手工方式很脆弱：

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

每向这个构造函数添加一个新资源，都要求更新前面所有失败分支。一次重新排序获取顺序的维护修改，就会悄悄破坏清理逻辑。

RAII 版本使用成员包装器，并依赖这样一条语言规则：当构造函数抛出时，已经构造完成的成员会被销毁：

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

没有清理标志，没有级联的 `if` 块，没有依赖顺序的手工拆除。语言本身完成了工作。

关闭是另一个主要压力点。析构函数运行时，系统往往已经处于状态转换中。后台工作可能仍然持有引用。日志基础设施可能已经部分拆除。一个会无限阻塞、回调进入不稳定子系统，或者依赖某种该类型从未文档化的线程亲和性的析构函数，可能会把整洁的所有权模型变成部署期失败。

教训不是害怕析构函数，而是保持析构工作狭窄且显式。释放你拥有的资源。避免令人意外的跨子系统行为。如果拆除需要比单纯析构更丰富的协议才能安全提供，就暴露一个显式的 stop 或 close 操作，并把析构函数当作最后的安全网，而不是唯一的清理路径。

## 验证与评审

所有权设计需要被显式评审，因为许多生命周期缺陷在任何工具运行之前，就已经能从结构上看出来。

有用的评审问题：

1. 每个资源是否都有一个单一且显而易见的所有者？
2. 借用的引用和视图，是否明显比所有者活得短？
3. `shared_ptr` 是在解决真实的共享生命周期问题，还是在逃避所有权决策？
4. move 操作是否保持了单一所有权和安全销毁？
5. 关闭是否依赖于超出资源释放范围的析构副作用？

动态工具仍然重要。AddressSanitizer 能捕获许多 use-after-free 缺陷。泄漏 Sanitizer 和平台诊断能捕获被遗忘的释放路径。ThreadSanitizer 在生命周期错误通过关闭期间的竞争暴露出来时会很有帮助。但只有当类型系统本身已经让所有权可读时，这些工具才最有力。

## 要点

- 把所有权视为一种必须在类型和对象结构中可见的契约。
- 对每一种有意义的资源都使用 RAII，而不仅仅是堆内存。
- 默认优先独占所有权，并对共享所有权给出显式理由。
- 在把 move 语义当作性能特性之前，先把它当作所有权转移规则。
- 对关闭路径和部分构造路径的审查，要和对稳定态路径一样认真。

如果某个资源可能被泄漏、被重复释放、在销毁后使用，或者在错误的线程上销毁，问题通常早在崩溃之前就开始了。它开始于所有权被留在隐式状态的时候。
