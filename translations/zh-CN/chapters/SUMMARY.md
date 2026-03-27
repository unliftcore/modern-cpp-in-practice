# 目录

[前言](preface.md)
[全书导图](map.md)

---

# 第一部分：核心思维模型

- [所有权、生命周期与 RAII](01-ownership-lifetime-and-raii.md)
- [值、身份与不变量](02-values-identity-and-invariants.md)
- [错误、结果与失败边界](03-errors-results-and-failure-boundaries.md)
- [参数传递、返回类型与 API 设计](04-parameter-passing-return-types-and-api-surface.md)

# 第二部分：编写现代 C++ 代码

- [影响设计决策的标准库类型](05-standard-library-types-that-change-design.md)
- [使用概念与约束编写泛型代码](06-generic-code-with-concepts-and-constraints.md)
- [范围、视图与生成器](07-ranges-views-and-generators.md)
- [编译期编程：保持清醒](08-compile-time-programming-without-losing-your-mind.md)

# 第三部分：接口、库与架构

- [接口设计与依赖方向](09-interface-design-and-dependency-direction.md)
- [运行时多态、类型擦除与回调](10-runtime-polymorphism-type-erasure-and-callbacks.md)
- [模块、库、打包与 ABI 的现实问题](11-modules-libraries-packaging-and-abi-reality.md)

# 第四部分：并发与异步系统

- [共享状态、同步与争用](12-shared-state-synchronization-and-contention.md)
- [协程、任务与挂起边界](13-coroutines-tasks-and-suspension-boundaries.md)
- [结构化并发、取消与背压](14-structured-concurrency-cancellation-and-backpressure.md)

# 第五部分：数据、内存与性能

- [数据布局、容器与内存行为](15-data-layout-containers-and-memory-behavior.md)
- [内存分配、数据局部性与开销模型](16-allocation-locality-and-cost-models.md)
- [基准测试与性能分析：避免自我欺骗](17-benchmarking-and-profiling-without-lying-to-yourself.md)

# 第六部分：验证与交付

- [针对资源管理与边界问题的测试策略](18-testing-strategy-for-resource-and-boundary-bugs.md)
- [Sanitizer、静态分析与构建诊断](19-sanitizers-static-analysis-and-build-diagnostics.md)
- [原生系统的可观测性](20-observability-for-native-systems.md)

# 第七部分：实战模式

- [用现代 C++ 构建小型服务](21-building-a-small-service-in-modern-cpp.md)
- [用现代 C++ 构建可复用库](22-building-a-reusable-library-in-modern-cpp.md)
- [现代 C++ 代码审查清单](23-reviewers-checklist-modern-cpp-code.md)

# 附录

- [术语对照表与翻译约定](appendices/terminology-reference.md)
- [C++23 特性索引（按工程用途分类）](appendices/cpp23-feature-index-by-engineering-use.md)
- [工具链基线](appendices/toolchain-baseline.md)
- [代码审查清单](appendices/code-review-checklists.md)
- [术语表](appendices/glossary.md)
