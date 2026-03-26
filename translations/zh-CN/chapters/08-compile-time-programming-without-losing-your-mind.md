# 不失控的编译期编程

编译期编程是 C++ 中最容易把专业能力变成自我伤害的领域之一。你可以把计算移到常量求值阶段、按类型分派、提前拒绝无效配置，还能在程序运行前就合成表格或元数据。这些能力千真万确。但代价同样真实：更长的构建时间、更差的诊断信息、逻辑向头文件扩散，以及诱使工程师把业务规则写成没人愿意调试的形式。

在生产环境中，正确的问题不是”这能不能在编译期做？”，而是”放到编译期做之后，什么会变得更安全、更省事、更难被误用？为此付出的构建时间和可维护性成本，值不值得？”

带着这个思路，编译期技术就能各就其位：它们是工程工具，用来消除无效状态、验证固定配置、在变化确实是静态的地方特化底层行为。它们并不比运行时代码高人一等。

## 优先选择看起来仍像普通代码的 `constexpr` 代码

最理想的现代编译期编程，往往就是普通代码——只不过写法上恰好也能在常量求值期间运行。如果一个解析辅助函数、小型查找表构建器或单位转换例程加上 `constexpr` 后依然清晰易读，那通常就是最好的状态。

这一点为什么重要？因为过去 C++ 元编程的大部分痛苦，都源于不得不把逻辑塞进类型层编码或模板递归——如果能用运行时代码，没人会自愿这么写。C++20 和 C++23 大幅缓解了这种压力：你现在可以直接在 `constexpr` 函数里写循环、分支和小型局部数据结构。

这改变了设计上的取舍。编译期例程如果读起来跟普通代码差不多，评审和调试就还在可接受的范围内。但如果为了在编译期运行，不得不写出一套更怪异的算法，那收益就必须大到足以弥补这些代价。

### 旧世界：递归模板和类型层算术

要感受 `constexpr` 带来了多大的变化，不妨看一个 C++11 之前的经典任务：在编译期计算阶乘。没有 `constexpr` 的年代，唯一的办法就是递归模板实例化：

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

这虽然能跑，但逻辑全部编码在类型系统里，而不是写成代码。没有循环、没有变量、没有调试器支持。一旦递归深度超限，报错就是一长串模板实例化的回溯。更复杂的场景——比如编译期字符串处理或表生成——需要的技巧更加晦涩：把变参模板包当值列表用，用递归 `struct` 层级模拟数组，再靠 SFINAE 技巧模拟条件分支。

现代写法就是一个普通函数：

```cpp
constexpr auto factorial(int n) -> int {
    int result = 1;
    for (int i = 2; i <= n; ++i)
        result *= i;
    return result;
}

// Usage: constexpr auto f = factorial(10);
```

结果一样，同样在编译期求值，但写出来就是任何 C++ 程序员都看得懂的普通代码；需要时还可以在运行时用调试器单步跟踪。这才是真正关键的转变：编译期编程不再需要一套截然不同的心智模型。

再看一个更贴近实际的例子：编译期查找表的构造。在旧写法中，生成一张 CRC 值表需要一个递归模板——每个表项实例化一次自身，通过嵌套类型别名逐步累积结果，几乎无法扩展或调试。而用 `constexpr`，只需要一个循环填充 `std::array`：

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

一个恰好在编译期运行的普通函数，就替代了原本可能长达几百行的模板机制。

适合这种做法的典型场景包括：固定翻译表、协议字段布局辅助函数、小型 enum 的经过验证的查找映射，以及根据常量输入组装的命令元数据。它们的共同特征是输入天然就是静态的，提前算好既能减少启动开销，又能让无效组合在源头上就不可能出现。

配套项目 `examples/web-api/` 中有几个简洁的实例。在 `error.cppm` 中，一个 `constexpr` 函数把错误码映射为 HTTP 状态码：

```cpp
// examples/web-api/src/modules/error.cppm
[[nodiscard]] constexpr int to_http_status(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::not_found:      return 404;
        case ErrorCode::bad_request:    return 400;
        case ErrorCode::conflict:       return 409;
        case ErrorCode::internal_error: return 500;
    }
    return 500;
}
```

这就是"恰好也能在编译期运行"的普通代码。它读起来像运行时函数，可以在运行时测试，也可以在 `constexpr` 上下文中求值——例如，在验证映射一致性的 `static_assert` 中使用。配套函数 `to_reason()` 对人可读的原因字符串做了同样的事情，返回 `std::string_view` 字面量。

类似地，`http.cppm` 提供了用于解析和格式化 HTTP 方法字符串的 `constexpr` 函数：

```cpp
// examples/web-api/src/modules/http.cppm
[[nodiscard]] constexpr Method parse_method(std::string_view sv) noexcept {
    if (sv == "GET")    return Method::GET;
    if (sv == "POST")   return Method::POST;
    if (sv == "PUT")    return Method::PUT;
    if (sv == "PATCH")  return Method::PATCH;
    if (sv == "DELETE") return Method::DELETE_;
    return Method::UNKNOWN;
}
```

这两个函数都是用普通控制流表达的编译期查找表，不需要任何模板机制，误用时的诊断信息清晰，运行时也可以正常调试。这正是 `constexpr` 的甜蜜点：输入来自一个小的静态集合，映射关系足够稳定，编译期求值增加了安全性而没有带来复杂性。

## 只有当延迟失败本身就是设计 bug 时，才使用 `consteval`

`consteval` 比 `constexpr` 更严格：它强制要求在编译期求值。如果放任运行时回退，会把某种你绝不想让它混进生产环境的配置错误掩盖掉，那就应该用 `consteval`。

举个例子：某个线路协议子系统有一组固定的消息描述符，每个描述符必须有唯一的 opcode 和受限的 payload 大小。这些约束不是动态业务逻辑，而是程序静态结构的一部分。在编译期就发现重复 opcode，显然好过启动时才暴露问题——更别说等到集成测试中因为路由 bug 才发现了。

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

具体的错误文本和机制还可以继续打磨，但设计思路是站得住脚的：这些描述符本身就是程序的静态结构，在编译期拒绝无效表格，完全值得付出相应成本。

常见的误用是：对那些本质上并非静态的逻辑也用 `consteval` 强制求值。如果某个值完全可能来自部署配置、用户输入或外部数据，硬把它拖进编译期，通常只会得到一个别扭又脆弱的设计。

## `if constexpr` 应当区分真正的类型族群，而不是塞进任意业务逻辑

`if constexpr` 是现代泛型代码中最有用的工具之一，因为它让基于类型的分支既局部又清晰。用得好，一套实现就能适配少量真正有意义的模型差异，不必拆成满地开花的特化版本。

用得不好，它会把一个函数模板变成无关行为的垃圾堆。

适用场景举例：trivially copyable 的 payload 与非平凡领域对象之间的存储策略差异；或者某个格式化辅助函数在对外保持统一接口的同时，对字节 buffer 和结构化记录分别处理。这些变化本质上是表示形式或类型能力的差异。

`if constexpr` 出现之前，这种按类型分支的需求只能靠 tag dispatch 或 SFINAE 重载集来实现：

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

这虽然可行，但会把逻辑上完整的一个函数打散到多个重载里。读者必须顺着 tag dispatch 一路追踪才能理清分支。有了 `if constexpr`，同样的逻辑可以写在一处，一目了然：

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

两条分支都在同一个函数里。未命中的分支不会被实例化，因此不需要对当前类型也能编译通过。意图一目了然。

反面案例是：仅仅因为”编译器能把它优化掉”，就把每一条产品特定规则都塞进编译期分支。这样做会把应用层策略绑死在类型结构上，每多加一个条件，函数就更难审查。如果分支真正关心的是运行时的业务含义而非静态的类型能力，普通运行时代码通常更清晰。

编译期分支最适合用来表达稳定的类型族群关系。如果用它只是为了省掉另写一个同样简单的函数，那多半就是用错了地方。

## 主要成本：构建开销、诊断质量和组织拖累

运行时代码的成本体现在执行开销上，编译期代码的成本则体现在团队身上。

大段常量求值表、大量实例化的模板，以及定义在头文件中的辅助框架，都会拖慢增量构建、让依赖图更加脆弱。常量求值失败时的诊断信息依然可能难以理解，尤其是多层模板和 concept 叠加的时候。而且编译期机制往往放在头文件里，实现细节的泄漏范围比运行时代码大得多。

正因如此，生产环境中的编译期编程应当紧贴少数几个反复验证过的收益点。

- 尽早拒绝静态上无效的程序结构。
- 为固定数据消除少量启动工作。
- 基于静态能力专门化低层操作。
- 让生成的表和元数据与声明的类型保持一致。

超出这些范围，投入产出比会迅速下降。

还有一层组织成本。一旦团队把复杂的编译期基础设施视为常态，更多工程师就会在上面继续搭建——不是因为”这是最清晰的解法”，而仅仅因为”它已经在那儿了”。抽象的表面积不断膨胀，能放心审查的人越来越少。到最后，项目里就出现了两层复杂性：运行时代码，以及塑造运行时代码的编译期框架。

在现代 C++ 中，几乎没有哪个领域比这里更需要克制。

## 代码生成有时比元编程更好

如果数据源来自外部或者规模很大，代码生成通常是更划算的工程选择。协议 schema、遥测目录、SQL 查询清单，或者从外部定义提取的命令注册表——用生成器来验证和演进，往往比搭一座复杂的模板高塔外加 `constexpr` 解析器更容易管理。

这不是认输，而是认清现实：有些复杂性放在构建工具链里管理比塞进 C++ 类型系统更合适。生成出来的 C++ 照样可以暴露干净的强类型接口，区别只是复杂性放在哪里、失败模式有多容易被看见。

经验法则：源数据规模小、本身就是静态的、天然适合直接写在代码里——这时优先在 C++ 内做编译期编程。源数据规模大、来自外部、或者本来就维护在另一种格式中——这时优先用代码生成。两者的平衡点，通常比模板爱好者愿意承认的来得更早。

## 失败模式与边界

编译期编程的失败模式往往大同小异。

一种常见的情况是：为了节省一个从未测量过的启动开销，用密密麻麻的模板机制取代了本来可读性很好的运行时代码。另一种是把部署期配置拉进了编译期，结果那些本应是运维层面的调整，变成了必须重新构建才能生效。还有一种是把"`constexpr` 能求值通过"当成了整体设计更优的证据，哪怕构建时间和诊断质量都已经明显恶化。

编译期能证明的东西也有边界。它可以验证固定形状、常量关系和类型层面的能力，但无法替代集成测试、资源边界测试或运维验证。一张在编译期通过校验的分发表，其指向的 handler 在运行时照样可能产生错误的副作用。

让编译期逻辑紧贴设计中真正静态的部分，不要让它蔓延成一种通用架构风格。

## 验证与评审

这里的验证既关乎正确性，也关乎成本。

- 对核心编译期辅助函数中不希望退化的规则，用有针对性的 `static_assert` 加以守护。
- 即使表格和元数据是在编译期构建的，也要保留有代表性的运行时测试——常量求值并不能证明动态行为的正确性。
- 引入以头文件为中心的编译期基础设施时，持续关注增量构建时间的变化。
- 审查失败场景下的错误信息。如果报错信息让人看不懂，说明这个抽象还没准备好投入生产。
- 问自己一句：同样的效果能不能用更简单的运行时代码或代码生成来达到？

最后这个问题是团队最常略过的，但往往也是最有价值的。

## 要点

- `constexpr` 代码应该看起来跟普通代码没什么两样。
- 只有当运行时回退本身就意味着设计错误时，才使用 `consteval`。
- `if constexpr` 用在稳定的类型能力差异上，不要拿来编码任意的业务分支。
- 构建时间、诊断质量和可审查性，都是一等成本。
- 一旦编译期机制不再让程序的静态结构更清晰，就退回到更简单的运行时代码或代码生成工具链。
