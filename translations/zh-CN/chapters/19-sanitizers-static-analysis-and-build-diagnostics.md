# Sanitizer、静态分析与构建诊断

测试策略告诉你应当覆盖什么行为。本章讨论的是应当与这些测试并行运行的机械化找 bug 栈：编译器警告、sanitizer、静态分析，以及能保留有用诊断信息的构建配置。这些工具不会告诉你设计是否正确。它们会告诉你：程序是否踩进了那些人类在评审中经常漏掉的 bug 类别。

这个区分很重要。测试可以断言取消后不会留下可见的部分状态。AddressSanitizer 可以告诉你，清理路径为了遵守这项契约，却访问了已经释放的内存。契约测试可以证明一个解析器会拒绝 malformed input。UndefinedBehaviorSanitizer 可以告诉你，其中一条拒绝路径在计算 buffer 大小时发生了有符号溢出。可观测性随后还可以告诉你，生产构建正在一条你从未在 sanitizer 下执行过的路径里崩溃。每一层回答的都是不同问题。

对原生系统来说，跳过这一层的代价是可预测的。bug 会更晚被发现、复现更不稳定、诊断证据也更差。如果构建只能产出经过优化、符号被剥离、警告很弱、也没有 analyzer 或 sanitizer job 的二进制，那么团队实际上是在把更慢的调试选成制度。

## 把诊断当作构建产物，而不是开发者偏好

第一个错误是组织层面的，而不是技术层面的。团队常把警告和分析当成本地可选工具，于是它们会随编译器、机器和心情漂移。生产级 C++ 需要的是相反的姿态。诊断保真度应当是构建契约的一部分。

至少，仓库应当定义几个具名构建模式，各自回答不同问题。

| 构建模式 | 主要问题 | 典型特征 |
|---|---|---|
| 快速开发构建 | 我能快速迭代逻辑吗？ | 调试信息、断言、无优化或低优化 |
| Address/UB sanitizer 构建 | 执行过程中是否触发了内存或未定义行为 bug？ | `-O1`、调试信息、frame pointer、ASan 和 UBSan |
| Thread sanitizer 构建 | 并发执行时是否触发了 data race 或锁顺序问题？ | 独立 job、降低并行度、仅 TSan |
| 静态分析构建 | 代码是否在运行前就触发了警告模式或可分析缺陷？ | 编译器警告、clang-tidy、analyzer job |
| 带符号的发布构建 | 生产行为是否仍然可诊断？ | 发布优化、外部符号、build ID、稳定的源码映射 |

试图把这些折叠成一个通用配置，通常都会失败。TSan 的开销大到不适合每次构建。ASan 和 UBSan 会改变内存布局与时序。深度分析 job 比普通 edit-compile-run 循环更慢。正确答案不是一个神奇构建。正确答案是一个刻意设计的矩阵。

这个矩阵应当活在版本化的构建脚本或 preset 里，而不是活在部落记忆里。如果仓库无法准确告诉一个新工程师如何产出 sanitized binary，或者如何产出带符号的发布工件，那这个工作流就还不够成熟。

## 警告是策略表面

编译器警告是你手里最便宜的分析手段，而团队却仍在浪费它们。一种常见失败模式是警告膨胀：成千上万条既有警告会训练所有人忽视这个通道。另一种则是警告极简主义：因为害怕噪声，团队启用得太少，以至于可疑代码会静默通过。

实用目标更窄，也更严格。

- 在所有受支持编译器上启用一套严肃的警告集。
- 一旦警告基线得到控制，就把已拥有代码中的警告视为错误。
- 保持 suppression 局部化、可版本化，并附解释。
- 像评审代码变更一样评审新的 suppression，因为它本来就是代码变更。

这不是为了审美上的整洁。警告经常会暴露真正的评审问题：窄化转换、缺失的 override、被忽略的返回值、遮蔽了所有权状态的 shadowing、`switch` 穷尽性缺口，或热路径上的意外拷贝。有些警告偏风格化，应该保持关闭。这没问题。重点是让启用集合可辩护且稳定。

尤其要警惕在 target 级别做 blanket suppression。当某个第三方头文件或生成源码很吵时，把它隔离开，而不是把同一条诊断在整个仓库里静音。团队经常会为了修一个 vendor 问题，而通过项目级 suppression 创造未来的盲区。

## Sanitizer 把静默损坏变成可操作的失败

sanitizer 的价值在于它们改变了失败模式。内存 bug 不再表现为遥远的崩溃或不可能状态，而是会在接近违规点的地方停下来，附带栈跟踪和 bug 类别说明。

对大多数生产级 C++ 代码库来说，有三类 sanitizer 配置价值最高。

### AddressSanitizer 与泄漏检测

AddressSanitizer 是标准的一线工具，因为它能发现一大类本来会浪费巨大时间的 bug：use-after-free、heap buffer overflow、某些配置下的 stack use-after-return、double free，以及相关的内存生命周期违规。泄漏检测（在可用的平台上）则为测试进程和短生命周期工具再增加一种有用信号。

当它与上一章的测试策略配合时，ASan 尤其有效。失败路径测试、fuzzer 和集成场景会把执行压进那些所有权错误藏身的分支。ASan 则把这些错误转成可复现的失败。

### 不开 ASan 时“看起来能工作”的 bug

这是跳过 sanitizer 构建的代码库里，最经典、也最浪费调试时间的案例：

```cpp
auto get_session_name(session_registry& registry, session_id id)
	-> std::string_view
{
	auto it = registry.find(id);
	if (it == registry.end()) return {};
	return it->second.name();  // Returns view into the session object.
}

void log_and_remove_session(session_registry& registry, session_id id)
{
	auto name = get_session_name(registry, id);
	registry.erase(id);             // Session destroyed. name is now dangling.
	audit_log("removed session: {}", name);  // Use-after-free.
}
```

没有 ASan 时，这段代码通常会通过测试，甚至在生产里正确运行几个月。被释放的内存会一直保留旧字符串数据，直到别的东西把它覆盖。测试通过。代码评审也未必能抓到它——这个函数看起来很直接。真正出问题时，症状却是乱码日志，或者在某个无关分配处崩溃，离实际 bug 很远。

在 ASan 下，它会立刻产生精确失败：

```
==41032==ERROR: AddressSanitizer: heap-use-after-free on address 0x6020000000d0
READ of size 12 at 0x6020000000d0 thread T0
    #0 0x55a3c1 in log_and_remove_session(session_registry&, session_id)
        src/session_manager.cpp:47
    #1 0x55a812 in handle_disconnect src/connection.cpp:103

0x6020000000d0 is located 0 bytes inside of 32-byte region
freed by thread T0 here:
    #0 0x4c1a30 in operator delete(void*)
    #1 0x55a7f1 in session_registry::erase(session_id)
        src/session_manager.cpp:31

previously allocated by thread T0 here:
    #0 0x4c1820 in operator new(unsigned long)
    #1 0x55a620 in session_registry::insert(session_id, session_info)
        src/session_manager.cpp:22
```

这份报告指出了确切的读取位置、确切的释放位置，以及确切的分配位置。把它和另一种情况比较一下：三周后某条损坏的日志记录，谁也想不到会和这条代码路径有关。

典型构建特征大致如下：

```bash
clang++ -std=c++23 -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined
```

具体 flag 会因工具链而异，但原则稳定：保留足够优化以维持真实结构，保留调试信息，并保留 frame pointer 以便栈可用。

### UndefinedBehaviorSanitizer

UBSan 是对应的搭档，用来捕获那些不一定表现为内存损坏的危险行为：未对齐访问、非法位移、错误的枚举值、某些上下文中的空指针解引用、取决于配置的有符号溢出，以及其他未定义或可疑操作。重要的操作性经验是：未定义行为往往对输入和构建都很敏感。同一段代码可能几个月都通过测试，却只会在新编译器下，或某次内联变化后才失败。UBSan 的价值，就是在 bug 还足够局部、还能理智修复的时候，把这些风险暴露出来。

但也不要过度解读它。UBSan 不是证明系统。它只会报告被当前执行走到、并且被启用检查所能看见的行为。

一个具体例子：尺寸计算中的有符号溢出，是安全 bug 的常见来源，而编译器完全有权利用它。

```cpp
auto compute_buffer_size(std::int32_t width, std::int32_t height, std::int32_t channels)
	-> std::int32_t
{
	return width * height * channels;  // Signed overflow if product exceeds INT32_MAX.
}
```

当 `width=4096, height=4096, channels=4` 时，乘积是 67,108,864——安全。当 `width=32768, height=32768, channels=4` 时，乘积是 4,294,967,296，会溢出 32 位有符号整数。没有 UBSan 时，编译器甚至可能把下游边界检查整体优化掉，因为有符号溢出属于未定义行为。UBSan 会在乘法处直接抓住它：

```
runtime error: signed integer overflow: 32768 * 32768 cannot be
represented in type 'int'
```

修复方式是使用无符号运算，或者在乘法前检查溢出。重点在于：这一类 bug 是静默的、对优化器敏感，而且通常与安全相关——这正是 UBSan 的用武之地。

### ThreadSanitizer

TSan 很昂贵，而且在自定义同步、lock-free 代码，以及某些协程或外部运行时集成周围往往会比较吵。它仍然值得运行，因为 data race 仍然是那些事后诊断成本最高的原生 bug 之一。

### 测试永远抓不到的 data race

没有 TSan，data race 对测试来说是不可见的，因为它们依赖调度。考虑一个被请求处理器和后台报告器共享的 metrics counter：

```cpp
struct service_stats {
	std::int64_t requests_handled = 0;   // No synchronization.
	std::int64_t bytes_processed = 0;
};

// Thread 1: request handler
void handle_request(service_stats& stats, request const& req) {
	process(req);
	stats.requests_handled++;    // Data race: unsynchronized write.
	stats.bytes_processed += req.size();
}

// Thread 2: periodic reporter
void report_stats(service_stats const& stats) {
	log_metrics("requests", stats.requests_handled);   // Data race: unsynchronized read.
	log_metrics("bytes", stats.bytes_processed);
}
```

这段代码会通过你写的每一个测试。它甚至可能在 x86 上正确运行几个月，因为那里的内存模型相对宽容。真正的问题会在编译器重排写入、优化器把读取提到寄存器里，或者有人把代码移植到 ARM 时暴露。bug 今天就存在，只是症状被推迟了。

TSan 会立刻抓住它：

```
WARNING: ThreadSanitizer: data race (pid=28511)
  Write of size 8 at 0x7f8e3c000120 by thread T1:
    #0 handle_request(service_stats&, request const&)
        src/handler.cpp:24
    #1 worker_loop src/server.cpp:88

  Previous read of size 8 at 0x7f8e3c000120 by thread T2:
    #0 report_stats(service_stats const&)
        src/reporter.cpp:12
    #1 reporter_loop src/server.cpp:102

  Location is global 'g_stats' of size 16 at 0x7f8e3c000120

  Thread T1 (tid=28513, running) created by main thread at:
    #0 pthread_create
    #1 start_workers src/server.cpp:71

  Thread T2 (tid=28514, running) created by main thread at:
    #0 pthread_create
    #1 start_reporter src/server.cpp:76
```

修复方式是使用带合适内存序的 `std::atomic<std::int64_t>`，或者如果这些字段必须被一致地一起读取，就用 mutex 保护整个 struct。重点在于：传统测试再多，也抓不到这个问题——测试需要借助 TSan，才能把一个依赖调度的损坏转成确定性失败。

操作模式通常和 ASan 不同。让 TSan 运行在更窄的 CI lane 或 nightly job 里。给它喂那些会刻意施压共享状态路径、关闭、重试和取消的测试。让 suppression file 保持简短且有充分理由。如果 TSan 在所谓无害的统计代码里报告 race，不要习惯性 dismiss。所谓 benign race，往往会在下一个功能到来后变成真实 race。

不要在同一个构建里把 TSan 和其他重量级 sanitizer 叠在一起。把它们分开跑，失败更容易解释，时序失真也更可控。

## 静态分析会放大评审注意力

静态分析最有价值的时候，是它足够有选择性，而且足够无聊。如果 analyzer 一次吐出几页风格噪声，团队很快就不看了。如果它围绕代码库里真正重要的模式做了调优，它就会成为评审的倍增器。

在现代 C++ 里，有用的目标通常包括：

- 由临时对象生命周期错误导致的悬空 view 或引用。
- 在 API 契约依赖它们时，缺失或误用 `override`、`noexcept` 或 `[[nodiscard]]`。
- 涉及原始指针、moved-from 对象或智能指针别名的可疑所有权转移模式。
- 错误处理失误，例如被忽略的结果、被吞掉的状态值，或边界上的不一致转换。
- 穿越热或高流量接口时昂贵的意外拷贝。
- 锁使用不一致或对共享状态的不安全 capture 这类并发隐患。

编译器集成分析、`clang-tidy`、Clang static analyzer，以及像 MSVC `/analyze` 这样的特定平台工具，各自能抓到的东西略有不同。如果工具链支持，可以同时用多个，但要把输出整理干净。一套小而受强制执行、且稳定抓住真实问题的规则，比一个所有人都绕过去的庞杂配置更好。

这也是仓库特定知识该放进来的地方。如果你的服务代码绝不该忽略来自 transport adapter 的 `std::expected` 结果，那就加上检查和 wrapper，让这种忽略不容易静默发生。如果你的库在 ABI 边界上禁止异常，那就直接为这个策略做分析，或者通过构建和 API 结构来强制执行。静态分析一旦知道你的契约是什么，就会变得强得多。

## 在发布工件中保留诊断质量

原生开发里最具破坏性的习惯之一，就是把可调试性当成只属于 debug 构建的问题。生产失败发生在 release 构建里。如果这些工件没有保留足够信息，无法把崩溃和延迟问题映射回代码，那么你就是在主动削弱后续可观测性。

发布工件通常至少应保留这些属性。

- 外部符号文件或 symbol server，使部署后仍可对栈做符号化。
- build ID 或等价的版本指纹，使 dump 或 trace 能无歧义地映射到精确二进制。
- 嵌入工件或附着在部署元数据中的源码版本信息。
- 足够的 unwind 支持，以得到可用的原生栈追踪。
- 以某种可复现方式记录下来的稳定编译器和 linker 设置。

根据平台与敏感性，这还可能包括 frame pointer、split DWARF 或 PDB 处理、map file，以及归档过的 link 命令。具体机制取决于工具链。策略则不取决于工具链：如果你无法复现已发货二进制的诊断形状，事故响应会立刻变慢。

这也是为什么构建诊断放在 sanitizer 与分析这一章里，而不是放在可观测性章节里。可观测性稍后会消费这些工件，但生产这些工件的决定，本质上是构建决策。

## CI 应当分阶段承担成本，而不是假装成本不存在

成熟的 pipeline 不会在每次编辑上都运行所有昂贵检查。它会按成本和 bug 类别来分阶段。

例如：

- Pull request gate：快速构建、严肃警告、针对性测试，以及至少一个针对变更目标的 ASan/UBSan 配置。
- Scheduled 或 nightly job：更广的 sanitizer 覆盖、TSan、更深的静态分析，以及开启 sanitizer 的 fuzz target。
- 发布资格验证：干净的带符号发布构建、打包检查，以及对符号发布与构建元数据成功产出的验证。

权衡非常明显：更慢的检查会让 bug 在一天中更晚被发现。答案不是删掉它们。答案是把它们放在可持续且可见的位置。

不要让 sanitizer 或 analyzer 失败退化成“仅供参考”的噪声。如果某条 lane 抖到无法 gate 任何东西，就修掉抖动，或者缩小它的范围。一条永久红着的分析 job，在组织层面上等同于根本没有这条 job。

## 这些工具做不到什么

这套工具链很强，但它的边界也应当保持明确。

- sanitizer 不会证明正确性；它们只会对被执行到的路径插桩。
- 静态分析无法理解每一个项目特定不变量，除非你把这些不变量编码进代码和配置里。
- 警告干净并不意味着 API 设计或失败处理就做得好。
- 一个完全可诊断的构建，仍然可能发出错误行为。

这就是为什么第六部分有三章，而不是一章。测试策略定义了哪些东西必须被执行到。机械化工具会在这些执行发生时抓住一类 bug。可观测性则解释了：那些仍然逃到生产环境里的失败，该如何理解。

## 要点总结

在生产级 C++ 里，诊断必须被设计，而不能靠希望。维护一个版本化的构建矩阵，为快速迭代、sanitizer、分析以及带符号的发布分别提供独立 job。把警告当作策略表面。例行运行 ASan 和 UBSan，有意识地运行 TSan，并把静态分析收敛到一个大家仍会阅读输出的程度。在发布工件里保留符号化能力和构建身份。

核心权衡是成本与信号。sanitizer 和分析会拖慢 pipeline，偶尔也需要 suppression。但如果在没有它们的情况下发货，等原生 bug 逃逸之后，付出的代价会大得多。趁代码还在本地时，就选择承担这份成本。

评审问题：

- 对这个目标来说，哪些 sanitizer 配置是强制的，它们是否真的被有意义的测试覆盖到了？
- 哪些警告在仓库范围内被强制执行，suppression 又是在什么地方被评审的？
- 哪些 analyzer 检查反映的是真实项目契约，而不是泛泛的风格偏好？
- 一个发布崩溃或 dump，能否映射回精确的二进制、符号集和源码版本？
- 哪些昂贵检查是被有意放到更后面运行的，这种分阶段是显式设计出来的，而不是偶然形成的？
