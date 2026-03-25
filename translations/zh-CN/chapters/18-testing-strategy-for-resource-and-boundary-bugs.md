# 面向资源与边界缺陷的测试策略

C++ 里最昂贵的 bug，大多不是“算法返回了错误数字”这一类。它们是资源与边界缺陷：错误路径上未关闭的文件描述符、失败提交后仍然残留的临时文件、会泄漏工作的取消路径、在某个特定字节模式下才会在负载下爆炸的宽松解析器，或者一个把领域失败悄悄翻译成进程终止的库边界。

happy-path 单元测试无法对这些设计施加足够压力。它们往往验证了名义行为，却几乎没有覆盖生命周期转换、清理保证和边界契约。在现代 C++ 里，这是笔糟糕的交易。所有权和失败处理已经足够显式，你完全可以围绕它们来设计测试，而且你也应该这么做。如果一个组件拥有稀缺资源、跨越进程或 API 边界，或者在 timeout、取消、malformed input 或部分失败下表现不同，它的测试策略就应当围绕这些事实来构建。

本章讨论的是测试设计，而不是工具。目标是在代码发货前，决定你需要什么证据。Sanitizer、静态分析和构建诊断属于下一章。运行时日志、metrics、trace 和崩溃证据属于再下一章。这里的问题更简单：哪些测试能证明，在系统承压时，所有权、清理和边界行为仍然正确？

## 从失败形状开始，而不是从“测试金字塔”口号开始

通用测试建议在 C++ 里很快就会变弱，因为昂贵失败的分布并不均匀。如果一个服务的大部分风险预算都花在关闭、取消、临时文件替换、buffer 生命周期和外部协议翻译上，那么测试套件也应当把大部分精力花在那里。

这意味着要从失败形状开始。

对每个组件，问四个问题。

1. 哪些资源必须被恰好一次地释放、回滚或提交？
2. 哪些边界会在子系统之间转换错误、所有权或表示？
3. 哪些输入或调度情况大到无法枚举，但生成起来很便宜？
4. 哪些行为依赖时间、并发或取消，而不是简单的调用顺序？

这些问题会把你推向不同的测试形式。资源清理通常需要确定性的失败注入和后置条件检查。边界转换通常需要针对真实 payload 和错误类别的契约测试。巨大的输入空间通常需要性质和 fuzzing。时间敏感的并发通常需要可控的时钟、executor 和关闭编排，而不是基于 sleep 的测试。

覆盖率数字并不能回答这些问题。一行代码即使跑到了，也仍然无法证明确实发生了回滚、所有权依然有效，或者关闭路径在没有 use-after-free 风险的前提下排空了后台工作。把覆盖率当作滞后的完整性信号，而不是整个测试套件的组织原则。

## 在业务关心的层级上测试资源生命周期

针对资源 bug 的正确测试，几乎从来都不是去断言某个 helper 被调用了。它断言的是围绕获取、提交、回滚和释放的可观测契约。

考虑一个会以原子方式重写磁盘快照的服务。生产规则不是“先调用 `write`，再调用 `rename`，失败时再 `remove`”。生产规则是：“要么新的快照变得可见，要么旧快照保持不变，并且 staging file 被清理掉。” 有用的测试应当直接瞄准这条规则。

### 有意保留为 partial：一个让回滚可测试的接缝

```cpp
struct file_system {
	virtual ~file_system() = default;

	virtual auto write(std::filesystem::path const& path,
					   std::span<char const> bytes)
		-> std::expected<void, std::error_code> = 0;

	virtual auto rename(std::filesystem::path const& from,
						std::filesystem::path const& to)
		-> std::expected<void, std::error_code> = 0;

	virtual void remove(std::filesystem::path const& path) noexcept = 0;
};

enum class snapshot_error {
	staging_write_failed,
	commit_failed,
};

auto write_snapshot_atomically(file_system& fs,
							   std::filesystem::path const& target,
							   std::span<char const> bytes)
	-> std::expected<void, snapshot_error>
{
	auto staging = target;
	staging += ".tmp";

	if (auto r = fs.write(staging, bytes); !r) {
		return std::unexpected(snapshot_error::staging_write_failed);
	}

	if (auto r = fs.rename(staging, target); !r) {
		fs.remove(staging);
		return std::unexpected(snapshot_error::commit_failed);
	}

	return {};
}
```

```cpp
TEST(write_snapshot_atomically_cleans_up_staging_file_on_commit_failure)
{
	fake_file_system fs;
	fs.fail_rename_with(make_error_code(std::errc::device_or_resource_busy));

	auto result = write_snapshot_atomically(
		fs,
		"cache/index.bin",
		std::as_bytes(std::span{"new snapshot"sv.data(), "new snapshot"sv.size()}));

	ASSERT_FALSE(result.has_value());
	EXPECT_EQ(result.error(), snapshot_error::commit_failed);
	EXPECT_FALSE(fs.exists("cache/index.bin.tmp"));
	EXPECT_EQ(fs.read("cache/index.bin"), "old snapshot");
}
```

这种 seam 是对的，因为它正好落在业务边界上。测试不会把半个标准库都 mock 掉。它只是在外部效果周围建立一个可替换接口，然后检查调用方依赖的后置条件。

这种权衡很重要。对基础设施过度 mock，只会产生脆弱测试：它们确认的是 syscall 的实现顺序，而不是操作的安全属性。反过来，如果不在设计里做出 seam，失败路径除了靠大型集成测试几乎不可测。中间地带是：把资源边界隔离一次，然后围绕提交与回滚行为写测试。

### 过度 mock 何时会掩盖真实 bug

考虑一下：检查实现细节的测试，与检查安全属性的测试之间有什么差别。团队常常会写出这样的测试：

```cpp
// BAD: This test passes, but proves nothing about cleanup.
TEST(write_snapshot_calls_remove_on_rename_failure)
{
	strict_mock_file_system fs;
	EXPECT_CALL(fs, write(_, _)).WillOnce(Return(std::expected<void, std::error_code>{}));
	EXPECT_CALL(fs, rename(_, _)).WillOnce(Return(
		std::unexpected(make_error_code(std::errc::device_or_resource_busy))));
	EXPECT_CALL(fs, remove("cache/index.bin.tmp")).Times(1);

	write_snapshot_atomically(fs, "cache/index.bin", as_bytes("data"sv));
}
```

这个测试验证了 `remove` 被调用。它并没有验证 staging file 真的消失了，也没有验证原文件保持不变。如果有人把清理逻辑重构成 `std::filesystem::remove_all`，或者改了 staging 路径约定，这个测试就会失败——但如果 `remove` 静默失败、把 staging file 留了下来，这种真实 bug 却会通过。前面基于 `fake_file_system` 的测试更强，因为它断言的是可观测后置条件，而不是调用序列。

### 资源泄漏测试：验证清理，而不只是 happy-path 所有权

如果错误路径会跳过构造，或错误地移动所有权，那么作用域化 RAII 仍然不够。一个出人意料地常见的模式，是某个资源只会在特定失败路径上泄漏：

```cpp
class connection_pool {
public:
	auto acquire() -> std::expected<pooled_connection, pool_error>;
	void release(pooled_connection conn) noexcept;
};

// This function has a leak on the second acquire failure.
auto transfer(connection_pool& pool, transfer_request const& req)
	-> std::expected<receipt, transfer_error>
{
	auto src = pool.acquire();
	if (!src) return std::unexpected(transfer_error::no_connection);

	auto dst = pool.acquire();
	if (!dst) {
		// BUG: forgot to release src back to the pool.
		return std::unexpected(transfer_error::no_connection);
	}

	// ... perform transfer, release both on success ...
	pool.release(std::move(*src));
	pool.release(std::move(*dst));
	return receipt{};
}
```

一个只覆盖成功路径的测试看不到这个泄漏。一个只在失败时检查返回值的测试也会漏掉它。真正能抓住它的测试，会断言 pool 状态：

```cpp
TEST(transfer_releases_source_connection_when_dest_acquire_fails)
{
	counting_connection_pool pool{.max_connections = 1};

	auto result = transfer(pool, make_request());

	ASSERT_FALSE(result.has_value());
	EXPECT_EQ(pool.available(), 1);  // Source connection must be returned.
}
```

模式就是这样：如果你拥有稀缺资源，那么失败路径测试就应当断言该资源被释放了，而不只是断言返回了一个错误。

### 异常安全：从“能编译”到“正确”之间的差距

即便是不带 `noexcept` 的代码路径，只要异常安全重要，也值得测试。一个提供强异常保证的容器或缓存，就应该为此写测试：

```cpp
TEST(cache_insert_preserves_existing_entries_on_allocation_failure)
{
	lru_cache<std::string, std::string> cache(/*capacity=*/4);
	cache.insert("key1", "value1");
	cache.insert("key2", "value2");

	failing_allocator::arm_failure_after(1);  // Fail during insert internals.

	auto result = cache.insert("key3", "value3");
	EXPECT_FALSE(result.has_value());

	// Strong guarantee: pre-existing entries are intact.
	EXPECT_EQ(cache.get("key1"), "value1");
	EXPECT_EQ(cache.get("key2"), "value2");
	EXPECT_EQ(cache.size(), 2);
}
```

如果缓存只提供基本保证，测试仍应验证没有资源泄漏，并且缓存处于有效状态（即使状态有所改变）。最糟糕的情况是根本没有测试——缓存在异常时静默破坏了自己的内部结构，而调用方是在生产环境的分配压力下才发现问题。

同样的模式也适用于 socket、事务、受锁保护的 registry、临时目录、子进程 handle，以及拥有线程的服务。问一问：成功、部分失败、重试和关闭时，稳定契约分别是什么。测试这些。

## 边界测试应证明转换，而不只是解析

现代 C++ 代码往往把复杂度预算花在边界上：网络协议、文件格式、进程边界、插件 API、数据库客户端，以及 C 接口。这里的 bug 很昂贵，因为它们会同时污染两边的假设。一个边界测试应当验证三件事。

第一，合法输入会在没有生命周期花招的前提下映射到内部表示。如果解析器把 `std::string_view` 存进更长生命周期的状态里，边界测试就应当证明：这个 view 指向的是稳定所有权，或者在必要时表示会执行复制。第二，非法或部分输入会以正确的错误类别失败。解析失败、传输失败和业务规则拒绝，除非这是 API 明确规定的契约，否则不应塌缩成同一条泛化错误路径。第三，从组件内部再格式化输出或翻译回外部时，应保持排序、转义、单位和版本等不变量。

这里要使用真实工件。对配置加载器来说，把真实示例文件放在测试旁边。对 HTTP 或 RPC 边界来说，保留有代表性的 payload，包括 malformed header、过大的 body、重复字段、错误编码和不支持的版本。对带 C API 的库来说，要在面向 ABI 的表面写测试，而不只是测内部 C++ wrapper。如果边界承诺不抛异常，就要在 allocator 压力和非法输入下验证这一承诺。

这些测试不必很大，但必须具体。“能 round-trip 一个 JSON 对象”很弱。“遇到重复的主键字段时会以 schema error 拒绝，并保持旧配置继续生效”很强。

### 精选示例容易漏掉的边界角落案例

要特别注意那些单独看似无害、但组合起来会出问题的边界条件：

```cpp
// A parser that stores string_view into a longer-lived config object.
// This test passes because the input string outlives the config.
TEST(config_parser_reads_server_name)
{
	std::string input = R"({"server": "prod-01"})";
	auto cfg = parse_config(input);
	EXPECT_EQ(cfg.server_name(), "prod-01");  // PASSES -- but fragile.
}

// This test exposes the dangling view.
TEST(config_survives_input_destruction)
{
	auto cfg = []{
		std::string input = R"({"server": "prod-01"})";
		return parse_config(input);
	}();
	// input is destroyed. If server_name() holds a string_view into it,
	// this is use-after-free. It may still "pass" without sanitizers.
	EXPECT_EQ(cfg.server_name(), "prod-01");
}
```

大多数团队写的是第一个测试。真正能抓住 bug 的是第二个。再配上 AddressSanitizer（下一章），就能把静默损坏变成硬失败。

其他那些常被漏掉、值得明确写测试的边界角落案例还包括：

- 空输入、单字节输入，以及恰好落在 buffer 边界上的输入。
- 字符串字段中包含内嵌 null 的 payload，因为 `std::string_view::size()` 与 C 的 `strlen()` 会给出不同结论。
- 来自依赖方的错误响应，它们是合法的协议帧，但状态码出乎意料，而不仅仅是连接失败。
- 在一个 schema 版本里合法、在另一个版本里非法的输入，尤其是在存在版本协商时。

## 失败注入比更多 mock 更有价值

C++ 的错误路径，正是所有权错误变成生产事故的地方。如果你只测试成功路径，就等于宣布错误处理代码没有被评审过。

实用的答案是确定性的失败注入。在组件跨越资源边界或调度边界的地方引入失败：文件打开、rename、有界组件内部的内存分配、任务提交、timer 到期、下游 RPC 调用，或持久化提交。然后验证这次操作让系统保持在有效状态。

关键字是“确定性”。随机让 syscall 失败，在混沌环境里也许有用，但作为回归测试就很弱。回归测试应当能够准确指出：是哪个操作失败了，以及之后必须保持什么状态。

因此要相应地设计 seam。

- 文件和网络 adapter 应当在操作边界上可替换。
- 时钟和 timer source 应当可注入，这样 timeout 测试就不用 sleep。
- 任务调度应允许测试 executor 以可控方式推进工作。
- 关闭和取消应暴露出一个测试可以等待的完成点。

这种设计压力是健康的。如果一个组件不靠全局 monkey-patching 就无法被强行推进到各种失败模式里，它通常就是和环境耦合得太深了。

要避免一种常见的过度行为：到处模拟 allocator 失败。分配失败测试对硬实时或具备强恢复保证的基础设施组件也许有用，但在许多代码库里，它只会制造噪声和不真实的控制流。只有当契约真的依赖于低内存存活能力时，才去做它。对大多数服务代码来说，I/O 失败、timeout、取消和部分提交行为才是更高价值的目标。

## 性质测试与 fuzzing 适合输入丰富的边界

有些边界太大，不能只靠精选示例。解析器、解码器、压缩器、类 SQL 查询片段、二进制消息读取器、路径规范化器和命令行解释器，都接受巨大的输入空间。在这里，基于性质的测试和 fuzzing 会物有所值。

重点不是新奇，而是把应当跨越大量输入仍然成立的不变量编码出来。

好性质的例子：

- 解析合法配置后再序列化再解析，语义相等性仍然保持。
- 非法 UTF-8 永远不会产生成功归一化后的 identifier。
- 一个消息解码器要么返回一个完全构造好的值，要么返回结构化错误；它绝不会把部分初始化的输出暴露出去。
- 对已规范化、并位于接受域内的相对路径，路径规范化是幂等的。

fuzzing 对 native code 尤其强，因为 malformed input 经常会把控制流带进那些很少被测试的分支，而生命周期错误和未定义行为正藏在那里。但要把章节边界分清：fuzzing 仍然是一种测试策略。它的价值来自于对契约和不变量施压。下一章会解释，sanitizer 如何通过把静默内存损坏变成可操作的失败，让 fuzzing 变得高效得多。

使用看起来像生产流量的 seed corpus，而不是任意字节。否则 fuzzer 会把太多时间花在探索那些真实系统早在外层就会拒绝的输入形状上。对协议读取器来说，种子里应包括截断消息、重复字段、错误长度、不支持的版本，以及压缩边界情况。对文本格式来说，应包括超长 token、非法转义和混合换行符。

## 并发与取消测试需要可控时间

许多 C++ 团队明知基于 sleep 的测试很脆弱，却仍在写，只因为生产代码把真实时钟和线程池写死了。结果是一种虚假的节省：测试在本地通过，在 CI 里失败，却仍然漏掉真正的关闭 bug。

如果组件依赖 deadline、重试、stop request 或后台 drain，就把它设计成让测试能够控制时间与调度。`std::stop_token` 和 `std::jthread` 有助于表达取消意图，但它们并不能消除对确定性编排的需求。运行在可注入 executor 上的任务队列，比起那种立刻 spawn detached 工作的队列，更容易验证。接收时钟和 sleep 策略的重试循环，比直接调用 `std::this_thread::sleep_for` 的重试循环更容易测试。

好的并发测试通常会断言以下某一类行为：

- stop request 会阻止新工作开始。
- in-flight 工作会在定义好的挂起点观察到取消。
- 关闭会等待已拥有的工作完成，并且之后不会再使用已释放状态。
- 背压会限制队列增长，而不是把过载转成无界的内存增长。
- timeout 路径返回一致的错误类别，并释放所拥有的资源。

注意，这些都不是“在 callback Y 之前调用了 callback X”。它们是生命周期保证。并发 bug 之所以昂贵，正是因为它们发生在这里。

## 集成测试应验证完整的清理故事

并不是每一种资源 bug 都能靠隔离测试证明。有些失败只有在真实文件系统、真实进程模型、socket 或线程调度参与时才会浮现。你仍然需要聚焦的单元测试和性质测试，但你同样需要一小组集成测试，用来验证端到端的清理行为。

对一个服务来说，这可能意味着：用临时数据目录启动进程，发送真实请求，在存储层强制制造失败，然后验证重启行为与磁盘状态。对一个库来说，这可能意味着：从一个小型 host 程序里调用公共 API，加载配置、启动后台工作、取消它，然后干净地卸载。对工具来说，这可能意味着：用 fixture tree 调用真实可执行文件，并检查退出码、stderr 和文件系统后置条件。

让这些测试以场景为中心，并保持克制。它们比单元测试更慢，也更难诊断。它们的任务，是验证完整的清理故事：部分写入不会变成已提交状态，重复启动不会继承失败关闭留下的垃圾，外部契约在失败下仍保持稳定。

## 什么测试该停掉

弱测试会消耗评审时间，却不会提升信心。

停止编写那些只是在重述当前实现结构的测试。

- 验证每个 helper 是否被调用，却从不检查任何对外有意义后置条件的测试。
- mock 很重，以至于只要你把两个内部函数合并就会失败，即使契约仍然正确的测试。
- 基于 sleep 的异步测试，它们真正断言的是“今天机器刚好够空闲”。
- 把日志或错误字符串做 snapshot 的测试；真正的契约明明是错误类别和结构化字段，而不是文案。
- 用宽泛的集成测试去替代精确的失败路径测试。

真正的纪律，是把测试预算花在 bug 类别聚集的地方。在 C++ 里，这些 bug 类别集中在所有权、边界、取消和 malformed input 周围。要为它们做显式设计。

## 要点总结

现代 C++ 中的测试策略应当追随失败经济学，而不是追随泛泛的分层口号。拥有资源的代码需要确定性的失败路径测试。边界密集的代码需要带真实工件的契约测试。输入丰富的代码需要性质测试与 fuzzing。并发代码需要可控的时间和调度。集成测试应验证完整的清理故事，而不是取代聚焦测试。

用本章来决定：发货之前，哪些行为必须被证明。用下一章来决定：在这些测试运行期间，哪些编译器、sanitizer、分析器和构建诊断应当机械地搜索 bug。

评审问题：

- 这个组件对资源的提交、回滚和释放保证是什么？
- 哪些边界转换需要配合真实 fixture 的具体契约测试？
- 现在有哪些失败点可以被确定性注入，哪些则需要通过重构才能变得可测试？
- 哪些输入表面更适合性质测试或 fuzzing，而不是只靠示例覆盖？
- 现在有哪些时间、取消或关闭行为，仍然是通过 sleep 而不是受控调度来测试的？
