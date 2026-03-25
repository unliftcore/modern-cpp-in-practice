# 不失控的编译期编程

编译期编程是 C++ 专业能力最容易演变成自我伤害的地方之一。语言允许你把工作移到常量求值阶段、按类型分派、提前拒绝无效配置，并在运行时开始前合成表格或元数据。这些能力都是真实的。但它们也会消耗构建时间、损害诊断质量、把逻辑扩散到头文件里，并诱使工程师把业务规则编码成没人愿意调试的形式。

正确的生产问题不是“这能不能在编译期完成？”，而是“如果在编译期完成，什么东西会变得更安全、更便宜或更难被误用？这是否值得付出构建和可维护性的成本？”

这种表述把编译期技术放回它们应有的位置：它们是工程工具，用来消除无效状态、验证固定配置，以及在变化确实是静态的地方专门化低层行为。它们不是对运行时代码的道德升级。

## 优先选择看起来仍像普通代码的 `constexpr` 代码

最健康的现代编译期编程，往往只是写成也能在常量求值期间运行的普通代码。如果一个解析辅助函数、小型查找表构建器或者单位转换例程能在不变得晦涩的情况下成为 `constexpr`，那通常就是最佳点。

这很重要，因为旧时代 C++ 元编程的大部分痛苦，都来自于被迫把逻辑塞进类型层编码或模板递归中；如果允许运行时代码，没有任何人会自愿这样写。C++20 和 C++23 已经大幅减轻了这种压力。你现在常常可以直接在 `constexpr` 函数里写循环、分支和小型局部数据结构。

这改变了设计权衡。如果一个编译期例程看起来仍像普通代码，那么评审和调试都还能忍受。如果把工作移到编译期，需要你再造一个更怪异的算法版本，那收益就必须足够大。

### 旧世界：递归模板和类型层算术

要理解 `constexpr` 改变了多少，考虑一个常见的 pre-C++11 任务：在编译期计算阶乘。没有 `constexpr` 时，唯一选择是递归模板实例化：

```cpp
// Pre-C++11: compile-time factorial via template recursion
template <int N>
struct Factorial {
    static const int value = N * Factorial<N - 1>::value;
};

template <>
struct Factorial<0> {
    static const int value = 1;
};

// Usage: Factorial<10>::value
```

这能工作，但逻辑被编码在类型系统里，而不是代码里。没有循环，没有变量，也没有调试器支持。递归深度超限时的错误会产生长长的模板实例化回溯链。更复杂的计算，例如编译期字符串处理或表生成，则需要越来越晦涩的技巧：把变参模板包当作值列表，用递归 `struct` 层级模拟数组，再用 SFINAE 技巧模拟条件分支。

现代的等价物只是一个函数：

```cpp
constexpr auto factorial(int n) -> int {
    int result = 1;
    for (int i = 2; i <= n; ++i)
        result *= i;
    return result;
}

// Usage: constexpr auto f = factorial(10);
```

结果相同，也是在编译期求值，但写成了任何 C++ 程序员都能读懂的普通代码；必要时，调试器还可以在运行时单步执行它。真正重要的变化就在这里：编译期编程不再需要一套完全不同的心智模型。

一个更现实的例子是编译期查找表构造。在旧风格中，生成一个 CRC 值表之类的东西，需要写一个递归模板，它为每个表项实例化一次自身，通过嵌套类型别名累积结果，而且几乎不可能扩展或调试。有了 `constexpr`，你只需要写一个循环去填充 `std::array`：

```cpp
constexpr auto build_crc_table() -> std::array<std::uint32_t, 256> {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
        table[i] = crc;
    }
    return table;
}

constexpr auto crc_table = build_crc_table();
```

这用一个恰好在编译期运行的普通函数，替代了原本可能要写成几百行模板 machinery 的东西。

好的候选场景包括：固定翻译表、协议字段布局辅助函数、小型 enum 的已验证查找映射，以及由常量输入组装出来的命令元数据。这些场景的共同点是：输入天然是静态的，而更早计算结果可以减少启动工作，或者让无效组合根本不可能出现。

## 只有当延迟失败本身就是设计 bug 时，才使用 `consteval`

`consteval` 比 `constexpr` 更强：它要求一定在编译期求值。当接受运行时回退会掩盖某种你永远不想在生产里放行的配置错误时，这就很有用。

想象一个 wire protocol 子系统，它有一组固定的消息描述符，并且这些描述符必须拥有唯一 opcode 和有界 payload 大小。这些约束不是动态业务逻辑。它们是程序静态形状的一部分。在编译期发现重复 opcode，显然比在启动时才发现、或者更糟——通过集成环境中的路由 bug 才发现，要好得多。

```cpp
struct MessageDescriptor {
std::uint16_t opcode;
std::size_t max_payload;
};

template <std::size_t N>
consteval auto validate_descriptors(std::array<MessageDescriptor, N> table)
-> std::array<MessageDescriptor, N>
{
for (std::size_t i = 0; i < N; ++i) {
if (table[i].max_payload > 64 * 1024) {
throw "payload limit exceeded";
}
for (std::size_t j = i + 1; j < N; ++j) {
if (table[i].opcode == table[j].opcode) {
throw "duplicate opcode";
}
}
}
return table;
}

constexpr auto descriptors = validate_descriptors(std::array{
MessageDescriptor{0x10, 1024},
MessageDescriptor{0x11, 4096},
MessageDescriptor{0x12, 512},
});
```

具体错误文本和机制还可以继续打磨，但设计点是稳固的。这些描述符属于程序的静态结构。在编译期间拒绝无效表格，值得付出相应成本。

错误在于：对那些本质上并不静态的逻辑，也用 `consteval` 强制求值。如果某个值完全可能合法地来自部署配置、用户输入或外部数据，那么硬把它拖进编译期，通常只会得到一个别扭而脆弱的设计。

## `if constexpr` 应当区分真实家族，而不是编码任意业务逻辑

`if constexpr` 是现代泛型代码中最有用的工具之一，因为它让基于类型的分支保持局部且可读。用得好时，它允许同一个实现适配少量真正有意义的模型差异，而不必拆成一片特化森林。

用得差时，它会把一个函数模板变成无关行为的垃圾场。

正确的使用场景，像是：平凡可复制 payload 与非平凡领域对象之间的存储策略差异；或者某个格式化辅助函数在保持单一公共契约的同时，对字节 buffer 和结构化记录做不同处理。这里的变化属于表示形式或能力差异。

在 `if constexpr` 出现之前，这类按类型分支的写法要么依赖 tag dispatch，要么依赖 SFINAE 重载集：

```cpp
// Pre-C++17 tag dispatch: two overloads selected by a type trait
template <typename T>
void serialize_impl(const T& val, Buffer& buf, std::true_type /*trivially_copyable*/) {
    buf.append(reinterpret_cast<const std::byte*>(&val), sizeof(T));
}

template <typename T>
void serialize_impl(const T& val, Buffer& buf, std::false_type /*trivially_copyable*/) {
    val.serialize(buf); // requires a member function
}

template <typename T>
void serialize(const T& val, Buffer& buf) {
    serialize_impl(val, buf, std::is_trivially_copyable<T>{});
}
```

这能工作，但会把一个逻辑上统一的函数打散到多个重载里。读者必须跟着 tag dispatch 追踪，才能理解分支逻辑。有了 `if constexpr`，相同逻辑就能保持局部且线性：

```cpp
template <typename T>
void serialize(const T& val, Buffer& buf) {
    if constexpr (std::is_trivially_copyable_v<T>) {
        buf.append(reinterpret_cast<const std::byte*>(&val), sizeof(T));
    } else {
        val.serialize(buf);
    }
}
```

两个分支都在同一个函数里。被丢弃的分支不会被实例化，所以它不需要对当前类型也能编译。意图立刻可见。

错误的使用场景，是因为“编译器能把它优化掉”，就把每一条产品特定规则都编码成另一个编译期分支。这种做法把应用层策略绑死在类型结构上，而且每增加一个条件，函数就更难评审。当分支真正关心的是运行时业务含义，而不是静态类型能力时，普通运行时代码通常更清晰。

编译期分支最适合表达稳定的家族关系。如果它存在的主要原因只是为了省掉第二个同样直白的函数，那么它往往就是个错误。

## 主要成本是构建成本、诊断质量和组织拖累

运行时代码有可见的执行成本。编译时代码有可见的团队成本。

大块常量求值表、重度实例化模板以及定义在头文件中的辅助框架，会拖慢增量构建，并让依赖图更加脆弱。常量求值失败时的诊断，依然可能很难解释，尤其当多个模板和概念叠在一起时。而且因为编译期 machinery 往往住在头文件里，实现细节会比运行时等价物更远地泄漏到整个代码库中。

这就是为什么生产环境中的编译期编程，应当靠近少数几个反复出现的收益点。

- 尽早拒绝静态上无效的程序结构。
- 为固定数据消除少量启动工作。
- 基于静态能力专门化低层操作。
- 让生成的表和元数据与声明的类型保持一致。

离开这些区域后，投资回报率会迅速下降。

这里还有组织成本。一旦团队把复杂的编译期基础设施常态化，更多工程师就会因为“它已经存在”，而不是因为“它是最清晰的解法”，继续在其上构建。抽象表面不断扩张。能有信心评审它的人越来越少。最终，项目里会出现两层复杂性：运行时代码，以及塑造运行时代码的编译期框架。

这就是为什么在现代 C++ 里，几乎没有哪个地方比这里更需要克制。

## 代码生成有时比元编程更好

如果事实来源是外部的，或者规模很大，那么代码生成通常是更好的工程权衡。对于协议 schema、遥测目录、SQL 查询清单，或者从外部定义提取出的命令注册表，用生成器来验证和演化，往往比搭一个复杂的模板塔和 `constexpr` 解析器更容易管理。

这不是承认失败，而是承认：有些复杂性更适合放在构建工具链里管理，而不是放进 C++ 类型系统。生成出来的 C++ 仍然可以暴露干净的强类型接口。区别只在于复杂性住在哪里，以及失败模式有多可见。

一个经验规则是：当源数据很小、静态，并且天然适合直接写在代码里时，优先在 C++ 内做编译期编程。当源数据很大、来自外部，或者本来就维护在另一种格式中时，优先用代码生成。这个收支平衡点通常比模板爱好者愿意承认的来得更早。

## 失败模式与边界

编译期编程往往会以一些熟悉的方式失败。

一种失败模式，是为了节省一个从未测量过的启动成本，用密集的模板 machinery 取代本来可读的运行时代码。另一种，是把部署期配置拉进编译期，导致那些本应是运维选择的变更，变成必须重新构建才能完成。还有一种，是把 `constexpr` 能通过求值，误当成整体设计更好的证据，哪怕构建时间和诊断质量都已经显著恶化。

这里还存在一个边界：编译期到底能证明什么。它可以验证固定形状、常量关系和类型层能力。它不能替代集成测试、资源边界测试或运维验证。一个在编译期验证过的分发表，仍然可能指向运行时副作用错误的 handler。

让编译期逻辑紧贴设计中真正静态的那一部分。不要让它扩散成一种通用架构风格。

## 验证与评审

这里的验证同时包括正确性和成本。

- 当核心编译期辅助函数编码了你不想回归的规则时，为它们添加聚焦的 `static_assert` 检查。
- 即使表格和元数据是在编译期构建的，也保留有代表性的运行时测试；常量求值并不能证明动态正确性。
- 当你引入以头文件为中心的编译期基础设施时，关注增量构建时间。
- 评审失败场景下的错误信息。如果诊断不可用，这个抽象就还没准备好进入生产环境。
- 问一问：同样结果是否能用更简单的运行时代码，或者用代码生成来达到。

最后这个问题，是团队最常跳过的，而它通常也是最有价值的。

## 要点

- 优先选择看起来仍像普通代码的 `constexpr` 代码。
- 只有当运行时回退代表真实设计错误时，才使用 `consteval`。
- 把 `if constexpr` 用在稳定的能力差异上，而不是任意业务分支上。
- 把构建时间、诊断质量和可评审性当作一等成本。
- 一旦编译期 machinery 不再让程序的静态结构更清晰，就退回到更简单的运行时代码或代码生成工具链。
