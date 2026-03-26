# 面向资源与边界缺陷的测试策略

C++ 中代价最高的 bug，大多不是”算法算错了数”这种。真正昂贵的是资源与边界缺陷：错误路径上忘关的文件描述符、提交失败后残留的临时文件、取消时泄漏了后台工作、解析器在某个特定字节模式下平时没事而一到高负载就炸，又或者某个库边界悄悄把业务错误变成了进程崩溃。

只测 happy path 的单元测试，对这些设计施加不了多少压力。它们往往只验证了正常行为，却把生命周期转换、清理保证和边界契约晾在一边。在现代 C++ 中，这笔账很不划算。所有权和错误处理已经足够显式，完全可以围绕它们来设计测试——而且理应如此。一个组件如果持有稀缺资源、跨越进程或 API 边界、又或者在超时、取消、畸形输入、部分失败等场景下有不同行为，那它的测试策略就应该围绕这些事实来构建。

本章谈的是测试设计，不是工具选型。目标是在代码上线之前，想清楚需要哪些证据。Sanitizer、静态分析和构建诊断是下一章的内容；运行时日志、metrics、trace 和崩溃证据是再下一章的内容。这里要回答的问题更简单：哪些测试能证明，在系统承压时所有权、清理和边界行为依然正确？

## 从失败形状开始，而不是从“测试金字塔”口号开始

泛泛的测试建议到了 C++ 这里很快就不够用了，因为高代价的故障分布极不均匀。如果一个服务的风险主要集中在关闭、取消、临时文件替换、buffer 生命周期和外部协议转换上，测试套件就应该在这些地方投入最多精力。

所以要从失败形状入手。

针对每个组件，问四个问题：

1. 哪些资源必须被恰好一次地释放、回滚或提交？
2. 哪些边界会在子系统之间转换错误、所有权或表示？
3. 哪些输入或调度情况大到无法枚举，但生成起来很便宜？
4. 哪些行为依赖时间、并发或取消，而不是简单的调用顺序？

这些问题会自然引出不同的测试形式。资源清理通常需要确定性的故障注入加后置条件检查；边界转换通常需要基于真实 payload 和错误类别的契约测试；输入空间巨大的场景通常需要属性测试（property testing）和 fuzzing；时间敏感的并发通常需要可控的时钟、executor 和关闭编排，而非基于 sleep 的测试。

覆盖率数字回答不了这些问题。一行代码跑到了，并不能证明回滚确实发生了、所有权依然有效、或者关闭路径在排空后台工作时没有 use-after-free 风险。应该把覆盖率视为一个滞后的完整性信号，而不是组织测试套件的核心原则。

## 在业务关心的层级上测试资源生命周期

针对资源 bug 的正确测试，几乎不会去断言某个 helper 被调用了。它断言的是获取、提交、回滚和释放这些环节的可观测契约。

假设有一个服务需要原子地重写磁盘快照。真正的生产规则不是”先调 `write`，再调 `rename`，失败了再 `remove`”，而是”要么新快照变得可见，要么旧快照保持不变且临时文件被清理掉”。有用的测试应该直接验证这条规则。

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

这个 seam 选得恰到好处，因为它正好落在业务边界上。测试没有把半个标准库都 mock 掉，而是只在外部副作用周围建了一个可替换接口，然后检查调用方真正关心的后置条件。

这个权衡很重要。过度 mock 基础设施只会产出脆弱的测试——它们验证的是 syscall 的调用顺序，而非操作的安全属性。但如果设计中完全不留 seam，失败路径就只能靠大型集成测试来覆盖。折中之道是：在资源边界做一次隔离，然后围绕提交与回滚行为来写测试。

### 过度 mock 何时会掩盖真实 bug

想想"检查实现细节的测试"和"检查安全属性的测试"之间的差别。团队常常会写出这样的测试：

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

这个测试验证了 `remove` 被调用了，但并没有验证临时文件真的消失了，也没有验证原文件保持完好。一旦有人把清理逻辑重构成 `std::filesystem::remove_all`，或者改了 staging 路径约定，这个测试就会挂掉——然而 `remove` 静默失败、临时文件残留这种真实 bug 反而能通过。前面基于 `fake_file_system` 的测试更强，因为它断言的是可观测的后置条件，而非调用序列。

### 资源泄漏测试：验证清理，而不只是 happy-path 所有权

当错误路径跳过了构造，或者所有权被错误地 move 走，单靠 RAII 的作用域管理是不够的。有一种出乎意料地常见的模式：资源只在某条特定的失败路径上才会泄漏：

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

只走成功路径的测试看不到这个泄漏；只在失败时检查返回值的测试同样会漏掉。真正能抓住它的测试，断言的是连接池的状态：

```cpp
TEST(transfer_releases_source_connection_when_dest_acquire_fails)
{
    counting_connection_pool pool{.max_connections = 1};

    auto result = transfer(pool, make_request());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(pool.available(), 1);  // Source connection must be returned.
}
```

规律就是：如果你持有稀缺资源，失败路径的测试就该断言资源已被归还，而不只是断言返回了一个错误。

### 异常安全：从“能编译”到“正确”之间的差距

即便代码路径没有标 `noexcept`，只要异常安全性很重要，就值得为它写测试。提供强异常保证的容器或缓存尤其如此：

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

如果缓存只提供基本保证，测试仍然应当验证没有资源泄漏，且缓存处于有效状态（哪怕内容已变）。最坏的情况是压根没有测试——缓存在抛异常时悄悄破坏了内部结构，直到生产环境的内存分配压力下才被发现。

同样的思路也适用于 socket、事务、受锁保护的 registry、临时目录、子进程 handle，以及持有线程的服务。问自己：在成功、部分失败、重试和关闭各种场景下，稳定的契约分别是什么？把这些契约测出来。

## 边界测试应证明转换，而不只是解析

现代 C++ 代码的复杂度往往集中在边界上：网络协议、文件格式、进程边界、插件 API、数据库客户端，以及 C 接口。边界上的 bug 代价高昂，因为它们会同时破坏两侧的假设。一个边界测试应当验证三件事。

第一，合法输入能正确映射到内部表示，不依赖生命周期上的取巧。如果解析器把 `std::string_view` 存进了生命周期更长的状态里，边界测试就要证明这个 view 指向的数据有稳定的所有权，或者在必要时做了拷贝。第二，非法或不完整的输入要以正确的错误类别失败。解析失败、传输失败、业务规则拒绝——除非 API 明确就是这么约定的，否则不应混为一谈、走同一条笼统的错误路径。第三，从组件内部格式化输出或转换回外部表示时，排序、转义、单位、版本等不变量要得到保持。

这里要用真实的测试工件。配置加载器——把真实的示例文件放在测试旁边。HTTP 或 RPC 边界——保留有代表性的 payload，包括畸形 header、超大 body、重复字段、错误编码和不支持的版本号。带 C API 的库——在 ABI 层面写测试，而不只是测内部的 C++ wrapper。如果边界承诺不抛异常，就要在 allocator 压力和非法输入下验证这个承诺。

这些测试不必很大，但必须足够具体。”能 round-trip 一个 JSON 对象”——太弱。”遇到重复主键字段时以 schema error 拒绝，并保持旧配置继续生效”——这才有力。

### 手工精选示例容易遗漏的边界情况

要留意那些单独看上去无害、组合起来却会出问题的边界条件：

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

大多数团队只会写第一个测试，但真正能暴露 bug 的是第二个。再配合 AddressSanitizer（下一章介绍），就能把悄无声息的内存损坏变成确定性的测试失败。

其他常被遗漏、值得专门写测试的边界极端情况还包括：

- 空输入、单字节输入，以及恰好落在 buffer 边界上的输入。
- 字符串字段中包含嵌入式 null 的 payload——`std::string_view::size()` 和 C 的 `strlen()` 对此会给出不同结果。
- 依赖方返回的错误响应：协议帧本身是合法的，但状态码出人意料，不只是简单的连接失败。
- 在某个 schema 版本里合法、在另一个版本里非法的输入，尤其是涉及版本协商的场景。

示例项目中有这几种模式的具体实践。在 `examples/web-api/tests/test_http.cpp` 中，`test_parse_request_malformed()` 向解析器喂入字符串 `"not a valid request"`，断言 `parse_request()` 返回 `std::nullopt` 而非崩溃或产出半初始化的 `Request`。这正是那种能抓住"默认假设输入格式正确"的解析器 bug 的畸形输入边界测试。同一文件还测试了 header 缺失的情况（`test_header_missing()`），确认返回 `std::optional` 的 `header()` 方法在 header 不存在时能正确处理缺失，而非返回悬空 view 或默认构造的字符串。

在 `examples/web-api/tests/test_task.cpp` 中，边界验证测试同样值得关注。`test_task_validation_rejects_empty_title()` 和 `test_task_validation_rejects_long_title()`（后者构造了一个 257 字符的字符串）验证了领域不变量在极端值处依然成立。这些不是验证调用顺序的测试——它们断言的是业务规则：task 标题必须非空且在长度上限内，并且错误通过 `std::expected` 以正确的 `ErrorCode::bad_request` 报告，而非被吞掉或转化为异常。

## 失败注入比更多 mock 更有价值

C++ 的错误路径，恰恰是所有权错误演变为生产事故的温床。只测成功路径，就等于默认错误处理代码无需审查。

务实的做法是确定性故障注入。在组件跨越资源边界或调度边界的地方引入故障：文件打开、rename、有界组件内部的内存分配、任务提交、定时器到期、下游 RPC 调用、持久化提交。然后验证操作结束后系统仍处于有效状态。

关键词是”确定性”。随机让 syscall 失败在混沌测试环境里或许有用，但作为回归测试太弱了。回归测试应当能精确说明：是哪个操作失败了，以及失败之后系统必须保持什么状态。

据此来设计 seam：

- 文件和网络适配器应当在操作边界上可替换。
- 时钟和定时器源应当可注入，让 timeout 测试无需 sleep。
- 任务调度应当允许测试用 executor 按步推进工作。
- 关闭和取消应当暴露一个完成点，供测试等待。

这种设计压力是良性的。如果一个组件非得靠全局 monkey-patching 才能走进各种失败模式，说明它和运行环境耦合得太紧了。

有一种常见的过度做法要避免：到处模拟 allocator 失败。分配失败测试对硬实时系统或具备强恢复保证的基础设施组件或许有意义，但在大多数代码库里只会制造噪声和不切实际的控制流。只在契约确实依赖于低内存存活能力时才做这件事。对大多数服务代码而言，I/O 失败、超时、取消和部分提交行为才是更值得投入的目标。

## 属性测试与 fuzzing 适用于输入丰富的边界

有些边界的输入空间太大，光靠精选示例远远不够。解析器、解码器、压缩器、类 SQL 查询片段、二进制消息读取器、路径规范化器、命令行解释器——它们接受的输入空间都极为庞大。在这些场景下，属性测试和 fuzzing 物有所值。

关键不在于追求新颖，而在于把那些应该在海量输入下始终成立的不变量明确编码出来。

好的属性示例：

- 合法配置经过"解析 -> 序列化 -> 再解析"后，语义保持一致。
- 非法 UTF-8 永远不会产出一个成功归一化的标识符。
- 消息解码器要么返回一个完全构造好的值，要么返回结构化错误；绝不允许部分初始化的输出被外部观察到。
- 对已规范化且位于接受域内的相对路径，路径规范化是幂等的。

Fuzzing 对 native code 尤其有效，因为畸形输入经常把控制流引入那些极少被测试到的分支——生命周期错误和未定义行为往往就藏在那里。不过要注意章节分工：fuzzing 仍然属于测试策略的范畴，它的价值在于对契约和不变量施加压力。下一章会介绍 sanitizer 如何把悄无声息的内存损坏变成可定位的失败，从而让 fuzzing 的效果大幅提升。

Seed corpus 要尽量贴近生产流量，而不是随机字节。否则 fuzzer 会把大量时间浪费在探索那些真实系统在外层就会拒掉的输入上。对协议读取器，种子应包括截断消息、重复字段、错误长度、不支持的版本号，以及压缩边界情况。对文本格式，应包括超长 token、非法转义序列和混合换行符。

## 并发与取消测试需要可控时间

许多 C++ 团队明知基于 sleep 的测试不稳定，却还是照写不误，原因无非是生产代码把真实时钟和线程池写死了。结果是一种虚假的节约：测试在本地能过，CI 上就挂，而真正的关闭 bug 照样漏过去。

如果组件依赖 deadline、重试、stop request 或后台排空（drain），就应该在设计上让测试能控制时间和调度。`std::stop_token` 和 `std::jthread` 有助于表达取消意图，但并不能取代确定性编排。运行在可注入 executor 上的任务队列，远比那种一来就 spawn detached 工作的队列容易验证；接受时钟和 sleep 策略注入的重试循环，也远比直接调用 `std::this_thread::sleep_for` 的好测。

好的并发测试通常断言以下某类行为：

- 发出 stop request 后，新工作不再启动。
- 正在执行的工作能在预定义的挂起点感知到取消。
- 关闭流程会等待自身持有的工作完成，完成后不再访问已释放的状态。
- 背压机制能限制队列增长，而非把过载变成无上限的内存膨胀。
- 超时路径返回一致的错误类别，并释放所持有的资源。

注意，以上没有一条是”在 callback Y 之前调用了 callback X”。它们都是生命周期保证。并发 bug 代价高昂，正是因为出问题的地方在这里。

示例项目中 `examples/web-api/tests/test_repository.cpp` 的 `test_concurrent_access()` 提供了一个简洁的例子。它启动 8 个 `std::jthread`，每个线程并发创建 100 个 task，最后断言 repository 的最终大小等于 800。这测试的是一个不变量：受 `shared_mutex` 保护的 `TaskRepository` 在并发写入下既不丢数据也不产生重复。同一文件中的 `test_update_validates()` 也值得注意：它在 `update()` 回调内把 task 标题改为空字符串，断言 repository 以 `ErrorCode::bad_request` 拒绝了这次修改。这是一个边界与并发交叉的测试——它验证了写锁保护下的 `update()` 路径中，即使 updater callable 由调用方提供，重新验证步骤仍能捕获不变量违反。

项目的 CMake 配置（`examples/web-api/CMakeLists.txt`）还通过 `ENABLE_ASAN` 和 `ENABLE_TSAN` 选项支持在 sanitizer 下运行这些测试。在 ThreadSanitizer 下跑并发测试，能提供锁协议正确性的机械化证据，而不是依赖”在某种特定调度交错下恰好通过”的运气。

## 集成测试应验证完整的清理故事

并非每种资源 bug 都能靠隔离测试来证明。有些故障只有在真实文件系统、进程模型、socket 或线程调度参与时才会浮现。聚焦的单元测试和属性测试仍然需要，但同时也需要一小组集成测试来验证端到端的清理行为。

对服务而言，可能意味着：用临时数据目录启动进程，发送真实请求，在存储层强制注入故障，然后验证重启行为和磁盘状态。对库而言，可能意味着：写一个小型宿主程序调用公共 API，加载配置、启动后台工作、取消它、干净卸载。对命令行工具而言，可能意味着：用 fixture 目录树调用真实可执行文件，检查退出码、stderr 和文件系统后置条件。

这类测试要以场景为核心，数量上保持克制。它们比单元测试慢，也更难排查问题。它们的职责是验证完整的清理流程：部分写入不会变成已提交状态，重复启动不会继承上次失败关闭留下的垃圾，外部契约在故障下依然稳定。

## 什么测试该停掉

弱测试消耗评审时间，却不能提升信心。

不要再写那些只是在重述当前实现结构的测试了：

- 逐个验证 helper 是否被调用，却从不检查任何对外有意义的后置条件。
- mock 太重，合并两个内部函数就会挂，哪怕契约完全没变。
- 基于 sleep 的异步测试，真正断言的不过是”今天机器恰好够空闲”。
- 对日志或错误字符串做 snapshot 的测试——实际契约是错误类别和结构化字段，不是措辞本身。
- 用大而全的集成测试替代精确的失败路径测试。

核心原则是：把测试预算花在 bug 高发区。在 C++ 中，这些高发区集中在所有权、边界、取消和畸形输入上。要为它们做有针对性的设计。

## 要点总结

现代 C++ 的测试策略应当跟着失败的经济学走，而不是跟着泛泛的分层口号走。持有资源的代码需要确定性的失败路径测试；边界密集的代码需要基于真实工件的契约测试；输入丰富的代码需要属性测试和 fuzzing；并发代码需要可控的时间与调度。集成测试应验证完整的清理流程，而不是替代聚焦测试。

用本章来确定：上线前哪些行为必须得到验证。用下一章来确定：在这些测试运行时，应当让哪些编译器、sanitizer、分析器和构建诊断自动帮你找 bug。

评审问题：

- 这个组件对资源的提交、回滚和释放保证是什么？
- 哪些边界转换需要配合真实 fixture 的具体契约测试？
- 目前有哪些故障点可以做确定性注入，哪些需要重构之后才可测？
- 哪些输入面更适合属性测试或 fuzzing，而非仅靠手写示例？
- 目前有哪些涉及时间、取消或关闭的行为，还在靠 sleep 而非受控调度来测试？
