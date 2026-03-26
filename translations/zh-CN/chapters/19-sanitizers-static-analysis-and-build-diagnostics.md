# Sanitizer、静态分析与构建诊断

测试策略决定了要覆盖哪些行为。本章讨论的是应当与测试并行运行的一套机械化查错手段：编译器警告、sanitizer、静态分析，以及能保留有用诊断信息的构建配置。这些工具不会告诉你设计对不对，但能告诉你程序是否踩进了人类在评审中经常漏掉的某类 bug。

这个区分很重要。测试可以断言：取消操作不会留下可见的部分状态。AddressSanitizer 能发现清理路径在履行该契约时访问了已释放的内存。契约测试可以证明解析器会拒绝畸形输入。UndefinedBehaviorSanitizer 能发现某条拒绝路径在计算 buffer 大小时发生了有符号溢出。可观测性则能揭示生产构建正在某条你从未用 sanitizer 执行过的路径上崩溃。每一层回答的问题各不相同。

对原生系统来说，跳过这一层的代价完全可以预见：bug 发现得更晚、复现更不稳定、诊断依据也更差。如果构建只能产出经过优化、符号被剥离、警告级别很低、又没有 analyzer 或 sanitizer job 的二进制文件，那团队实际上是在制度层面选择了更慢的调试方式。

## 把诊断当作构建产物，而不是开发者偏好

第一个错误出在组织层面，而非技术层面。团队常把警告和分析当成本地可选工具，结果它们就随着编译器、机器和个人心情各自漂移。生产级 C++ 需要的恰恰相反——诊断保真度应当是构建契约的一部分。

仓库至少应当定义几个具名构建模式，让每个模式回答不同的问题。

| 构建模式 | 主要问题 | 典型特征 |
|---|---|---|
| 快速开发构建 | 我能快速迭代逻辑吗？ | 调试信息、断言、无优化或低优化 |
| Address/UB sanitizer 构建 | 执行过程中是否触发了内存或未定义行为 bug？ | `-O1`、调试信息、frame pointer、ASan 和 UBSan |
| Thread sanitizer 构建 | 并发执行时是否触发了 data race 或锁顺序问题？ | 独立 job、降低并行度、仅 TSan |
| 静态分析构建 | 代码是否在运行前就触发了警告模式或可分析缺陷？ | 编译器警告、clang-tidy、analyzer job |
| 带符号的发布构建 | 生产行为是否仍然可诊断？ | 发布优化、外部符号、build ID、稳定的源码映射 |

试图把这些压缩成一个通用配置，往往行不通。TSan 开销太大，不适合每次构建都跑。ASan 和 UBSan 会改变内存布局与时序。深度分析 job 比日常的"编辑-编译-运行"循环慢得多。正确答案不是找到一个万能构建配置，而是刻意设计一个构建矩阵。

这个矩阵应当存放在版本化的构建脚本或 preset 中，而非靠口口相传。如果仓库无法明确告诉新工程师如何产出 sanitized binary 或带符号的发布工件，说明工作流还不够成熟。

示例项目 `examples/web-api/` 用具名的 CMake 选项直接对应了上面这些构建通道：

```cmake
# examples/web-api/CMakeLists.txt
option(ENABLE_ASAN  "Enable AddressSanitizer + UBSan"  OFF)
option(ENABLE_TSAN  "Enable ThreadSanitizer"            OFF)

add_library(project_sanitizers INTERFACE)
if(ENABLE_ASAN)
    target_compile_options(project_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(project_sanitizers    INTERFACE
        -fsanitize=address,undefined)
endif()
if(ENABLE_TSAN)
    target_compile_options(project_sanitizers INTERFACE -fsanitize=thread)
    target_link_options(project_sanitizers    INTERFACE -fsanitize=thread)
endif()
```

新工程师克隆仓库后，只需执行 `cmake -G Ninja -DENABLE_ASAN=ON` 或 `-DENABLE_TSAN=ON` 即可。通道是可发现的、受版本控制的，并且产出不同的二进制——这就是"具名构建模式"在实践中的样子。

## 警告是策略表面

编译器警告是最廉价的分析手段，团队却经常白白浪费。一种常见的失败模式是警告泛滥：成千上万条历史遗留警告让所有人习惯性地忽略。另一种则是警告不足：因为怕噪声，团队开启的警告太少，可疑代码得以悄悄通过。

务实的做法是范围更窄、要求更严。

- 在所有支持的编译器上开启一组认真挑选的警告。
- 警告基线清理干净后，对自有代码启用"警告即错误"。
- suppression 要保持局部化、纳入版本管理，并附上说明。
- 新增 suppression 要像评审代码变更一样评审——因为它本身就是代码变更。

这不是追求审美整洁。警告经常能暴露真正的问题：窄化转换、缺失的 override、被忽略的返回值、因变量遮蔽而隐藏的所有权状态、`switch` 穷举性缺口，以及热路径上的意外拷贝。有些警告确实偏风格化，关掉就好。关键是让已启用的警告集合经得起推敲，并且保持稳定。

尤其要警惕在 target 级别做大面积 suppression。如果某个第三方头文件或生成代码噪声太多，应该把它隔离出来，而不是在整个仓库里关掉同一条诊断。团队常常为了解决一个第三方库的问题，用项目级 suppression 给未来埋下盲区。

## Sanitizer 把静默损坏变成可操作的失败

sanitizer 之所以有价值，是因为它们改变了失败的表现形式。内存 bug 不再表现为远处的崩溃或匪夷所思的状态，而是在紧邻违规点的地方停下来，并给出栈跟踪和 bug 类别说明。

对大多数生产级 C++ 代码库来说，有三类 sanitizer 配置价值最高。

### AddressSanitizer 与泄漏检测

AddressSanitizer 是标准的一线工具，因为它能发现一大类原本会耗费大量调试时间的 bug：use-after-free、heap buffer overflow、某些配置下的 stack use-after-return、double free，以及其他内存生命周期违规。泄漏检测（在支持的平台上）还能为测试进程和短生命周期工具提供额外信号。

ASan 与上一章介绍的测试策略搭配使用时尤其有效。失败路径测试、fuzzer 和集成场景会把执行推入所有权错误潜伏的分支，ASan 则把这些错误转化为可复现的失败。

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

没有 ASan 时，这段代码通常能通过测试，甚至在生产环境里正确运行好几个月。被释放的内存仍然保留着旧的字符串数据，直到被其他内容覆盖。测试通过了，代码评审也未必能发现问题——函数看起来很简单。等到真正出问题时，症状却是乱码日志或某个毫不相干的分配处崩溃，离实际 bug 的位置很远。

在 ASan 下，它会立刻报出精确的错误：

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

报告精确指出了读取位置、释放位置和分配位置。对比一下另一种情况：三周后出现一条损坏的日志，谁也不会联想到是这条代码路径的问题。

典型构建特征大致如下：

```bash
clang++ -std=c++23 -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined
```

具体 flag 因工具链而异，但原则不变：保留足够的优化以维持真实的代码结构，保留调试信息，保留 frame pointer 以确保栈回溯可用。

### UndefinedBehaviorSanitizer

UBSan 是 ASan 的搭档，专门捕获那些不一定表现为内存损坏的危险行为：未对齐访问、非法位移、无效枚举值、某些上下文中的空指针解引用、有符号溢出（取决于配置），以及其他未定义或可疑操作。一条重要的实践经验是：未定义行为往往对输入和构建环境都很敏感。同一段代码可能连续几个月都能通过测试，换了编译器或者发生一次内联变化就暴露出来。UBSan 的价值在于：趁 bug 还局限在局部、还能从容修复时，就把这些隐患揭示出来。

但也不要过度解读。UBSan 不是形式化证明系统，它只能报告当前执行实际走到的、且已启用检查能覆盖到的行为。

举个具体例子：尺寸计算中的有符号溢出是安全漏洞的常见来源，而编译器完全有权基于"有符号溢出属于未定义行为"这一前提做优化。

```cpp
auto compute_buffer_size(std::int32_t width, std::int32_t height, std::int32_t channels)
    -> std::int32_t
{
    return width * height * channels;  // Signed overflow if product exceeds INT32_MAX.
}
```

当 `width=4096, height=4096, channels=4` 时，乘积为 67,108,864，安全。但当 `width=32768, height=32768, channels=4` 时，乘积为 4,294,967,296，超出了 32 位有符号整数的范围。没有 UBSan 时，编译器甚至可能把下游的边界检查整个优化掉——因为有符号溢出本身就是未定义行为。UBSan 则会在乘法处直接报错：

```
runtime error: signed integer overflow: 32768 * 32768 cannot be
represented in type 'int'
```

修复方式是改用无符号运算，或在乘法前做溢出检查。关键在于：这类 bug 毫无征兆、对优化器敏感、而且往往涉及安全——正是 UBSan 大显身手的场景。

### ThreadSanitizer

TSan 开销很大，而且在自定义同步原语、lock-free 代码以及某些协程或外部运行时集成的场景下往往噪声较多。但它仍然值得跑，因为 data race 至今仍是事后诊断成本最高的原生 bug 之一。

### 测试永远抓不到的 data race

没有 TSan 的话，data race 对测试来说是不可见的，因为它取决于线程调度。来看一个请求处理线程和后台报告线程共享的 metrics counter：

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

这段代码能通过你写的所有测试，在 x86 上甚至可以正确运行好几个月——因为 x86 的内存模型相对宽容。问题会在编译器重排写入、优化器把读取提升到寄存器、或代码被移植到 ARM 时才暴露出来。bug 今天就在那里，只不过症状被推迟了。

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

修复方式是使用带合适内存序的 `std::atomic<std::int64_t>`；如果这些字段必须作为整体一致读取，则用 mutex 保护整个 struct。关键在于：无论写多少传统测试都抓不到这个问题——必须借助 TSan，才能把一个依赖调度的数据损坏转化为确定性失败。

TSan 的运行方式通常和 ASan 不同。把它放在更窄的 CI lane 或 nightly job 中运行，喂给它那些刻意对共享状态路径、关闭流程、重试和取消施压的测试。suppression file 要保持简短，每条都要有充分理由。如果 TSan 在看似无害的统计代码中报告了 race，不要下意识地忽略——所谓"无害 race"往往会在下一个功能上线后变成真正的问题。

不要在同一个构建里把 TSan 和其他重量级 sanitizer 叠加使用。分开跑可以让失败更容易定位，时序失真也更可控。

示例项目中的 `TaskRepository`（见 `examples/web-api/src/modules/repository.cppm`）就是 TSan 验证正确同步模式的典型案例。该仓储用 `std::shared_mutex` 保护内部的 `std::vector<Task>`，读路径（`find_by_id`、`find_all`）使用 `std::shared_lock`，写路径（`create`、`update`、`remove`）使用 `std::scoped_lock`。用 `-DENABLE_TSAN=ON` 构建并对并发读写施压，即可确认该加锁纪律不存在 data race——这正是常规测试无法提供的证据。

## 静态分析会放大评审注意力

静态分析最有价值的状态，是精准且平淡无奇。如果 analyzer 一次吐出好几页风格噪声，团队很快就不看了。但如果它专注于代码库中真正重要的模式，就能成为评审效率的倍增器。

现代 C++ 中值得关注的典型检查目标包括：

- 因临时对象生命周期错误导致的悬空 view 或悬空引用。
- API 契约依赖 `override`、`noexcept` 或 `[[nodiscard]]` 时，这些标注的缺失或误用。
- 涉及裸指针、moved-from 对象或智能指针别名的可疑所有权转移。
- 错误处理方面的疏漏，比如忽略返回结果、吞掉状态值、或在边界处做了不一致的错误转换。
- 热路径或高流量接口上代价高昂的意外拷贝。
- 加锁不一致或不安全地捕获共享状态等并发隐患。

编译器集成分析、`clang-tidy`、Clang static analyzer 以及 MSVC `/analyze` 等平台专属工具，各自擅长的检查领域不尽相同。工具链支持的话可以同时用多个，但要保持输出整洁。一套规模小、强制执行、能稳定抓到真实问题的规则集，远胜于一个人人绕道走的庞杂配置。

这也是把项目专属知识注入分析的好地方。如果服务代码绝不应忽略来自 transport adapter 的 `std::expected` 结果，就加上检查和封装，让这种忽略难以悄无声息地发生。如果库在 ABI 边界上禁止异常，就直接针对这条策略做分析，或通过构建与 API 结构来强制执行。静态分析一旦了解了你的契约，威力会大幅提升。

## 在发布工件中保留诊断质量

原生开发中最有害的习惯之一，就是把可调试性视为 debug 构建的专属。生产故障发生在 release 构建中。如果发布工件没有保留足够的信息来把崩溃和延迟问题追溯到代码，后续的可观测性就会被严重削弱。

发布工件通常至少应保留以下属性：

- 外部符号文件或 symbol server，确保部署后仍可对栈做符号化。
- build ID 或等价的版本指纹，确保 dump 或 trace 能无歧义地对应到具体二进制。
- 源码版本信息，嵌入工件本身或附在部署元数据中。
- 足够的 unwind 支持，以获得可用的原生栈回溯。
- 编译器和 linker 设置，以可复现的方式记录下来。

视平台与安全敏感程度不同，还可能需要 frame pointer、split DWARF 或 PDB 处理、map file 以及归档的链接命令。具体机制因工具链而异，但策略是通用的：如果你无法重现已发布二进制的诊断条件，事故响应速度会立刻下降。

这也是构建诊断被放在 sanitizer 和分析这一章、而非可观测性章节的原因。可观测性在后续阶段会消费这些工件，但决定是否生产它们，本质上是构建层面的决策。

## CI 应当分阶段承担成本，而不是假装成本不存在

成熟的 pipeline 不会在每次编辑时都运行全部昂贵检查，而是按成本和 bug 类别分阶段安排。

例如：

- Pull request gate：快速构建、严肃警告、针对性测试，以及至少一个针对变更目标的 ASan/UBSan 配置。
- Scheduled 或 nightly job：更广的 sanitizer 覆盖、TSan、更深的静态分析，以及开启 sanitizer 的 fuzz target。
- 发布资格验证：干净的带符号发布构建、打包检查，以及对符号发布与构建元数据成功产出的验证。

取舍很明显：检查越慢，bug 被发现得就越晚。解决办法不是砍掉这些检查，而是把它们安排在可持续、可见的位置上。

不要让 sanitizer 或 analyzer 的失败沦为”仅供参考”的噪声。如果某条 lane 不稳定到无法作为门禁，就修掉不稳定的根因，或缩小它的覆盖范围。一条永远亮红灯的分析 job，在组织效果上等于没有。

## 这些工具做不到什么

这套工具很强大，但边界也需要明确认识。

- sanitizer 不能证明正确性，它们只对实际执行到的路径做插桩检测。
- 静态分析无法理解每一个项目特定的不变量，除非你把它们编码到代码和配置中。
- 警告全部消除并不代表 API 设计或错误处理就是好的。
- 构建完全可诊断，不等于它不会产生错误行为。

这正是第六部分分为三章而非一章的原因。测试策略定义了必须覆盖哪些行为；机械化工具在执行过程中捕获特定类别的 bug；可观测性则解决最后一个问题：那些仍然逃逸到生产环境的故障，该如何理解和诊断。

## 要点总结

在生产级 C++ 中，诊断能力必须经过设计，不能寄希望于运气。维护一个版本化的构建矩阵，为快速迭代、sanitizer、静态分析和带符号的发布分别设置独立 job。把警告当作策略面来管理。ASan 和 UBSan 要例行运行，TSan 要有针对性地运行，静态分析则要收敛到团队仍然会认真阅读输出的程度。在发布工件中保留符号化能力和构建标识。

核心权衡在于成本与收益。sanitizer 和静态分析会拖慢 pipeline，偶尔也需要 suppression。但不用它们就发布，等原生 bug 逃逸到生产环境后，代价只会更大。趁代码还在本地，主动承担这份成本。

评审问题：

- 对当前目标而言，哪些 sanitizer 配置是必须的？它们是否被有意义的测试真正覆盖了？
- 哪些警告在仓库范围内强制执行？suppression 在哪里接受评审？
- 哪些 analyzer 检查反映的是真实的项目契约，而非泛泛的风格偏好？
- 发布版崩溃或 dump 能否追溯到确切的二进制、符号集和源码版本？
- 哪些昂贵检查是有意推迟运行的？这种分阶段是刻意设计的，还是偶然形成的？
