# Chapter 15: Benchmarking, Profiling, and Regression Control

*Prerequisites: Chapter 14 (cost models provide the hypotheses that measurement validates).*

> **Prerequisites:** This chapter assumes you have a cost model worth testing. Chapter 14 covered allocation patterns, indirection costs, locality, and the reasoning that produces hypotheses like "this path allocates twice per request" or "switching from `std::map` to a flat container should cut lookup time by 4x for our working-set size." Without that reasoning, benchmarking degenerates into running timers around random code and producing numbers that feel scientific but prove nothing.
>
> You should also be comfortable with your toolchain's optimization levels and their effects on codegen. Measuring unoptimized builds is almost never useful for performance work and frequently misleading. The examples in this chapter assume `-O2` or equivalent as the baseline, with specific notes where `-O3`, LTO, or PGO change the picture.
>
> Familiarity with at least one profiler (perf, Instruments, VTune, or Tracy) is helpful but not required. The chapter explains interpretation, not installation.

## 15.1 The Production Problem

Performance regressions are among the most expensive bugs in production systems. They rarely crash anything. They show up as gradually increasing tail latency, growing memory footprints, or CI pipelines that slow down by a few percent per quarter until someone notices the build takes forty minutes instead of twelve. By the time the regression is visible, the commit that caused it is months old and buried under hundreds of changes.

The root causes are predictable:

- A benchmark existed but measured the wrong workload. It tested 100-element containers when production handles 50,000.
- A profiler was consulted but its output was misread. The hottest function in a flame graph is not always the function worth optimizing.
- A micro-benchmark showed a 2x improvement on an operation that accounts for 0.3% of wall-clock time, while the real bottleneck — memory allocation in a cold path — went unmeasured.
- No automated regression gate existed, so performance was checked manually before releases, which means it was checked inconsistently and eventually not at all.

The deeper problem is cultural. Performance evidence is treated as disposable — gathered during an optimization sprint, then abandoned. This chapter treats it as an engineering artifact: versioned, reviewed, automated, and tied to the same CI pipeline that enforces correctness.

## 15.2 The Naive Approach

The most common first attempt at benchmarking in C++ looks something like this:

```cpp
// Anti-pattern: naive wall-clock timing
#include <chrono>
#include <iostream>
#include <vector>

void benchmark_sort() {
    std::vector<int> data(100'000);
    std::iota(data.begin(), data.end(), 0);
    std::ranges::shuffle(data, std::mt19937{42});

    auto start = std::chrono::high_resolution_clock::now();
    std::ranges::sort(data);
    auto end = std::chrono::high_resolution_clock::now();

    // BUG: single iteration, no warmup, no statistics,
    // optimizer may reorder or eliminate the sort entirely
    std::cout << "sort took "
              << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()
              << " us\n";
}
```

This has several problems that compound in practice:

**Dead-code elimination.** If `data` is never read after the sort, an aggressive optimizer may remove the sort entirely. The benchmark then measures function-call overhead and clock resolution.

**No warmup.** The first iteration pays for page faults, instruction-cache misses, and branch-predictor training. Including it in the measurement biases the result upward.

**Single sample.** One run tells you nothing about variance. On a modern machine under load, wall-clock time for the same operation can vary by 10-30% between runs due to frequency scaling, context switches, and thermal throttling.

**No baseline comparison.** A number in microseconds is meaningless without context. Is it faster than last week? Faster than the alternative? The number alone cannot answer either question.

**Misleading clock choice.** `high_resolution_clock` is not guaranteed to be steady. On some platforms it can jump backward. `steady_clock` is the correct default for elapsed-time measurement.

Teams that outgrow this pattern often move to a framework but repeat the same mistakes at a higher level of abstraction — measuring unrealistic inputs, ignoring variance, and treating benchmark results as absolute rather than comparative.

## 15.3 The Modern Approach

### 15.3.1 Benchmark Design with Google Benchmark

Google Benchmark is the de facto standard for C++ micro-benchmarking. It handles iteration control, warmup, statistics, and optimizer-defeating scaffolding. But using the framework correctly still requires deliberate design.

```cpp
#include <benchmark/benchmark.h>
#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

static void BM_VectorSort(benchmark::State& state) {
    const auto n = state.range(0);
    std::mt19937 rng{42};

    for (auto _ : state) {
        // Setup: create and shuffle inside the loop so each
        // iteration sorts a fresh unsorted sequence.
        state.PauseTiming();
        std::vector<int> data(n);
        std::iota(data.begin(), data.end(), 0);
        std::ranges::shuffle(data, rng);
        state.ResumeTiming();

        std::ranges::sort(data);
        benchmark::DoNotOptimize(data.data());
    }

    state.SetItemsProcessed(state.iterations() * n);
    state.SetComplexityN(n);
}

BENCHMARK(BM_VectorSort)
    ->RangeMultiplier(4)
    ->Range(256, 1 << 20)
    ->Complexity(benchmark::oNLogN)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
```

Key design decisions here:

**`DoNotOptimize`** is a compiler barrier. It forces the compiler to treat `data.data()` as an observable side effect, preventing dead-code elimination of the sort. Use `benchmark::DoNotOptimize` on the output and `benchmark::ClobberMemory()` when you need to force memory to be written back.

**`PauseTiming` / `ResumeTiming`** excludes setup cost from the measurement. Use this sparingly — the pause/resume overhead is nonzero (typically tens of nanoseconds) and can distort results for very fast operations. For sub-microsecond work, prefer amortizing setup across a batch.

**Parameterized ranges** let you measure scaling behavior, not just a single point. A sort that is fast at N=1000 and quadratic at N=100,000 will show that in the complexity report.

**`SetItemsProcessed`** normalizes throughput reporting. Reviewers can compare "items per second" across different implementations even when iteration counts differ.

### 15.3.2 Avoiding Micro-Benchmark Traps

Micro-benchmarks are useful for comparing two implementations of the same operation under controlled conditions. They are not useful for predicting system-level performance. The gap between the two is where most measurement mistakes live.

**Cache state is unrealistic.** A micro-benchmark runs the same operation thousands of times. After the first few iterations, the entire working set is hot in L1/L2. In production, the data may be cold because other work evicted it between accesses. If cache behavior matters — and for data-structure selection it almost always does — you need a benchmark that interleaves other work or operates on data sets larger than the cache.

**Allocation patterns are hidden.** Google Benchmark measures wall-clock time by default. If your operation allocates, the allocator's thread-local cache, OS page state, and fragmentation level all affect timing but are invisible in the output. Pair benchmarks with allocation-tracking (see Section 3.4).

**Branch prediction is trained.** After hundreds of iterations with the same data distribution, the branch predictor is perfectly trained. Production workloads are rarely this predictable. Consider randomizing branch-affecting inputs across iterations.

### 15.3.3 Profiling: From Flame Graph to Actionable Insight

A profiler tells you where time is spent. It does not tell you why, and it does not tell you what to do about it. Misreading profiler output is as common as not profiling at all.

**Sampling profilers** (perf, Instruments, VTune) interrupt the program at regular intervals and record the call stack. The resulting flame graph shows which functions appear on the stack most often. This measures inclusive time — a function that appears hot may be hot because it calls something expensive, not because its own code is slow.

**Reading a flame graph correctly:**

1. Start from the widest bars at the bottom. These are the functions that dominate wall-clock time.
2. Distinguish self time from inclusive time. A function with high inclusive time but low self time is a caller, not a bottleneck.
3. Look for unexpected width. If `malloc` or `operator new` is 15% of your flame graph, the problem is allocation count, not sort algorithms.
4. Check for missing frames. Inlined functions do not appear in sampling profiles unless you compile with `-fno-omit-frame-pointer` (GCC/Clang) or use DWARF-based unwinding. Without frame pointers, flame graphs have collapsed stacks and misleading attribution.

**Instrumentation profilers** (Tracy, Optick, hand-rolled) add explicit markers around regions of interest. They have higher overhead but provide exact timing and custom annotations. Tracy is particularly effective for frame-based workloads (games, simulations, real-time systems) because it correlates timing with visual output.

```cpp
#include <tracy/Tracy.hpp>

void process_batch(std::span<const Request> batch) {
    ZoneScoped;  // Tracy: names zone after the function

    for (const auto& req : batch) {
        ZoneScopedN("process_single");
        // ... per-request work
    }
}
```

Tracy zones have approximately 5-20 ns overhead per zone entry/exit. For functions called millions of times per second, that overhead is measurable. Use zones selectively — instrument the outer loop, not the inner arithmetic.

### 15.3.4 Tracking Allocations as a First-Class Metric

Allocation count and allocation size are often more informative than wall-clock time for regression detection. A change that adds one allocation per request may not show up in a micro-benchmark but can degrade throughput by 10-20% under contention when multiple threads compete for the allocator.

Instrumenting allocations can be done at several levels:

**Global operator new replacement** for coarse counting:

```cpp
#include <atomic>
#include <cstdlib>
#include <new>

struct AllocationStats {
    std::atomic<std::size_t> count{0};
    std::atomic<std::size_t> bytes{0};

    void reset() {
        count.store(0, std::memory_order_relaxed);
        bytes.store(0, std::memory_order_relaxed);
    }
};

inline AllocationStats g_alloc_stats;

void* operator new(std::size_t size) {
    g_alloc_stats.count.fetch_add(1, std::memory_order_relaxed);
    g_alloc_stats.bytes.fetch_add(size, std::memory_order_relaxed);
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc{};
    return p;
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
```

This is a blunt instrument. It counts all allocations in the process, including those from third-party libraries and the standard library itself. But for regression detection, that is often exactly what you want: if the total allocation count for a request-processing benchmark jumps from 12 to 47 after a commit, something changed that deserves investigation.

Use this technique only in isolated benchmark or test binaries. A global `operator new` replacement conflicts with other allocator interposition mechanisms, including some sanitizer and allocator runtimes, so it is the wrong default for general-purpose application builds.

**`pmr` allocators** for scoped tracking in production code:

```cpp
#include <memory_resource>
#include <vector>
#include <cstdio>

class counting_resource : public std::pmr::memory_resource {
    std::pmr::memory_resource* upstream_;
    std::size_t alloc_count_ = 0;
    std::size_t total_bytes_ = 0;

protected:
    void* do_allocate(std::size_t bytes, std::size_t align) override {
        ++alloc_count_;
        total_bytes_ += bytes;
        return upstream_->allocate(bytes, align);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
        upstream_->deallocate(p, bytes, align);
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }

public:
    explicit counting_resource(std::pmr::memory_resource* up = std::pmr::get_default_resource())
        : upstream_(up) {}

    std::size_t alloc_count() const { return alloc_count_; }
    std::size_t total_bytes() const { return total_bytes_; }
};

// Usage in a benchmark or test:
void measure_request_allocations(const Request& req) {
    counting_resource counter;
    std::pmr::vector<Result> results(&counter);

    process(req, results);

    std::printf("allocations: %zu, bytes: %zu\n",
                counter.alloc_count(), counter.total_bytes());
}
```

The `pmr` approach is composable and scoped. It tracks only the allocations routed through it, making attribution precise. The tradeoff is that existing code must already use `pmr` containers, or be refactored to accept an allocator — which is nontrivial in large codebases.

### 15.3.5 Workload Selection

The most important decision in benchmark design is not the framework or the statistics. It is the workload.

A benchmark's workload must represent the access pattern, data distribution, and size that the code encounters in production. If your hash map serves lookups from a Zipfian-distributed key space, benchmarking with uniformly random keys will produce misleading throughput numbers — the real workload hits the same buckets repeatedly, stressing collision handling and cache behavior in ways uniform input does not.

Principles for workload design:

- **Match the size.** If production operates on 10,000-element containers, benchmark at 10,000. Also benchmark at 100 and 1,000,000 to understand scaling, but anchor decisions on the production size.
- **Match the ratio.** If the production mix is 95% reads and 5% writes, a 50/50 benchmark will overweight write-path performance.
- **Match the contention.** Single-threaded benchmarks of concurrent data structures are meaningless. Use the thread count and access pattern that production exhibits.
- **Use real data when possible.** Sanitized production traces, anonymized request logs, or recorded workload replays eliminate guesswork about distributions. Synthetic data is a fallback, not a first choice.
- **Document the workload.** Every benchmark should have a comment or companion document explaining why this workload was chosen, what production scenario it represents, and what its known limitations are.

### 15.3.6 Noise Control

Benchmark variance is the enemy of regression detection. If your benchmark has 15% run-to-run variance, you cannot detect a 5% regression. Reducing noise is prerequisite to useful automation.

**CPU frequency scaling.** On Linux, set the governor to `performance` before benchmarking:

```bash
# Set all CPUs to fixed frequency
sudo cpupower frequency-set -g performance
```

On CI machines, this should be a provisioning step, not a per-benchmark action.

**CPU pinning.** Pin the benchmark process to a specific core to avoid migration:

```bash
taskset -c 4 ./my_benchmark
```

Avoid core 0, which typically handles interrupts.

**Thermal throttling.** Long-running benchmarks on laptops and some CI machines will hit thermal limits. Either use short benchmarks with sufficient iterations for statistics, or use machines with adequate cooling.

**Background processes.** CI runners shared with other jobs produce noisy results. Dedicated benchmark machines or isolated containers with reserved CPU cores are worth the infrastructure cost if regression detection matters to the team.

**Statistical repetition.** Google Benchmark supports `--benchmark_repetitions=N` to run each benchmark multiple times and report mean, median, and standard deviation. Use at least 5 repetitions. Report median, not mean — it is more robust to outliers from OS scheduling jitter.

```bash
./my_benchmark --benchmark_repetitions=9 \
               --benchmark_report_aggregates_only=true \
               --benchmark_out=results.json \
               --benchmark_out_format=json
```

## 15.4 Tradeoffs and Boundaries

### 15.4.1 Micro-Benchmarks vs. System Benchmarks

Micro-benchmarks are cheap, fast, and deterministic. They answer "which implementation is faster for this operation in isolation?" They do not answer "will this make the system faster?" because they cannot model cache contention with other subsystems, allocator fragmentation from prior operations, or the interaction between this code and the scheduler under real load.

System benchmarks (load tests, integration benchmarks, replay-based benchmarks) answer the system question but are expensive, slow, and noisy. They require realistic infrastructure, representative data, and long enough runs to capture steady-state behavior.

Most teams need both. Micro-benchmarks gate individual PRs. System benchmarks gate releases.

### 15.4.2 Measurement Overhead

Every measurement technique has overhead. Tracy zones cost 5-20 ns. Allocation counting via global `operator new` adds an atomic increment per allocation. Sampling profilers consume 1-5% of CPU depending on sample rate. Instrumented builds can be 2-10x slower than production builds.

The consequence: never measure with the same binary you ship. Performance-measurement builds and production builds serve different purposes. Treating them as interchangeable produces either slow production binaries or meaningless measurements.

### 15.4.3 The Danger of Premature Automation

Automating benchmark regression detection before noise is under control produces false positives. False positives erode trust. After a few weeks of ignoring flaky benchmark alerts, teams stop looking at them entirely — and then real regressions pass through unnoticed.

Get the noise floor below 2-3% before automating gates. If you cannot achieve that with your infrastructure, use automation for alerts (informational) rather than gates (blocking), and investigate manually.

## 15.5 Testing and Tooling Implications

### 15.5.1 CI Integration: Continuous Benchmarking

The goal is to make performance regression as visible as a failing test. The mechanism depends on infrastructure constraints, but the pattern is consistent:

1. **Run benchmarks on every commit** (or every PR, if per-commit is too expensive). Store results in a time-series database or structured file (JSON output from Google Benchmark).

2. **Compare against a baseline.** The baseline is either the merge-base commit or a rolling window of recent results. Percentage thresholds (e.g., "fail if any benchmark regresses by more than 5% relative to baseline median") are simple and effective.

3. **Report results in the PR.** A comment or CI check that shows before/after numbers for affected benchmarks makes performance visible during review, not after merge.

A minimal comparison script:

```python
#!/usr/bin/env python3
"""Compare two Google Benchmark JSON outputs and flag regressions."""

import json
import sys

THRESHOLD = 0.05  # 5% regression threshold

def load_benchmarks(path):
    with open(path) as f:
        data = json.load(f)
    return {
        b["name"]: b["real_time"]
        for b in data["benchmarks"]
        if b.get("run_type") == "aggregate" and b.get("aggregate_name") == "median"
    }

def compare(baseline_path, current_path):
    baseline = load_benchmarks(baseline_path)
    current = load_benchmarks(current_path)
    regressions = []

    for name, base_time in baseline.items():
        curr_time = current.get(name)
        if curr_time is None:
            continue
        ratio = (curr_time - base_time) / base_time
        if ratio > THRESHOLD:
            regressions.append((name, base_time, curr_time, ratio))

    return regressions

if __name__ == "__main__":
    regressions = compare(sys.argv[1], sys.argv[2])
    if regressions:
        print("Performance regressions detected:")
        for name, base, curr, ratio in regressions:
            print(f"  {name}: {base:.1f} -> {curr:.1f} ({ratio:+.1%})")
        sys.exit(1)
    else:
        print("No regressions detected.")
        sys.exit(0)
```

This is deliberately simple. Production CI systems often add: results archival to a database, graphing over time, separate thresholds for different benchmark categories, and notification routing. But the core logic — compare medians, flag regressions above threshold — stays the same.

### 15.5.2 Benchmark Hygiene Rules

Benchmarks rot faster than tests. A test either passes or fails; a benchmark that drifts 1% per month is technically passing while becoming useless. Maintenance rules:

- **Benchmarks live in the same repository as the code they measure.** If they are in a separate repo, they fall out of sync within weeks.
- **Benchmarks are reviewed like production code.** Workload assumptions, parameter ranges, and `DoNotOptimize` placement all deserve review scrutiny.
- **Benchmarks have owners.** When the team responsible for a subsystem changes the data layout, they update the corresponding benchmarks. Orphaned benchmarks are worse than no benchmarks because they provide false confidence.
- **Dead benchmarks are deleted.** A benchmark for a code path that no longer exists is noise in the CI pipeline and confusion in results.

### 15.5.3 Hardware Counters and perf stat

When a micro-benchmark shows a regression but the algorithmic complexity has not changed, hardware counters reveal what the CPU is actually doing:

```bash
perf stat -e cache-misses,cache-references,branch-misses,instructions,cycles \
    ./my_benchmark --benchmark_filter=BM_VectorSort/16384
```

Typical interpretation:

| Counter | What it reveals |
|---|---|
| `instructions` / `cycles` (IPC) | IPC drop without instruction count change means stalls — memory, branch, or resource contention. |
| `cache-misses` / `cache-references` | High miss rate points to layout or access-pattern changes. |
| `branch-misses` | Elevated branch misses suggest data-dependent branching that the predictor cannot learn. |
| `L1-dcache-load-misses` | Distinguishes L1 from LLC misses. L1 misses to L2 are cheap (~4 ns); L1 misses to memory are not (~60-100 ns). |

On Windows, VTune provides equivalent counters through its GUI or `vtune -collect uarch-exploration` command line.

Hardware counters are deterministic in a way that wall-clock time is not. Two runs on the same data with the same binary will produce nearly identical instruction counts even if wall-clock time varies by 5%. This makes instruction count a useful secondary metric for regression detection in noisy CI environments.

### 15.5.4 Allocation Regression Tests

Allocation counts can be asserted in unit tests, not just benchmarks:

```cpp
#include <gtest/gtest.h>

TEST(JsonParser, AllocationCount) {
    AllocationStats snapshot;
    g_alloc_stats.reset();

    auto result = parse_json(test_payload);

    // This operation should allocate exactly once (the result buffer).
    // If this assertion breaks, a code change introduced unexpected allocations.
    EXPECT_LE(g_alloc_stats.count.load(), 3u)
        << "parse_json allocated more than expected; "
           "check for unnecessary copies or container growth";

    EXPECT_TRUE(result.has_value());
}
```

This is a coarse-grained but effective regression gate. The exact count may change intentionally — the point is that it changes visibly and requires an explicit update to the test, which triggers review discussion.

### 15.5.5 Profiler-Guided Optimization Workflow

The disciplined workflow for performance optimization is:

1. **Hypothesize.** Use the cost model from Chapter 14 to predict where time goes. "I expect 60% of time in deserialization, 30% in sorting, 10% in output."
2. **Profile.** Run a sampling profiler under a representative workload. Compare the profile to your hypothesis.
3. **Identify the gap.** If deserialization is 40% and allocation is 25% (not in your model), the allocation cost is the finding — not a confirmation of what you already knew.
4. **Benchmark the candidate change in isolation.** Write or update a micro-benchmark that exercises the specific operation.
5. **Measure the system effect.** After the micro-benchmark confirms improvement, run the system benchmark or load test to verify the improvement survives integration.
6. **Record the evidence.** The PR should include before/after benchmark numbers, the profiler finding that motivated the change, and the workload used. This is not bureaucracy — it is the only way future maintainers can evaluate whether the optimization is still valid when the workload changes.

## 15.6 Review Checklist

Use this checklist when reviewing benchmarks, profiling-driven optimizations, or performance regression infrastructure.

### 15.6.1 Benchmark Design
- [ ] The workload matches production data sizes, distributions, and access patterns. Deviations are documented.
- [ ] `DoNotOptimize` or `ClobberMemory` prevents dead-code elimination of the measured operation.
- [ ] Setup and teardown costs are excluded from timing (via `PauseTiming`/`ResumeTiming` or batched setup).
- [ ] The benchmark is parameterized across relevant sizes to show scaling behavior, not just a single data point.
- [ ] Multiple repetitions are used and results report median, not just mean.

### 15.6.2 Profiling
- [ ] The profiled binary was compiled with optimizations enabled (`-O2` or equivalent) and frame pointers retained (`-fno-omit-frame-pointer`).
- [ ] The profile was taken under a representative workload, not a unit test or trivial input.
- [ ] Flame graph interpretation distinguishes self time from inclusive time.
- [ ] Findings are stated as specific claims ("allocation accounts for 23% of wall time") rather than vague impressions ("allocation seems high").

### 15.6.3 Regression Detection
- [ ] Benchmark noise floor is documented. Regression threshold is set above the noise floor.
- [ ] Benchmarks run on isolated or dedicated hardware, not shared CI runners without CPU pinning.
- [ ] Results are stored in a format that supports historical comparison (JSON, database).
- [ ] Regression alerts have been validated: at least one synthetic regression has been introduced and detected by the pipeline before trusting it with real changes.

### 15.6.4 Allocation Tracking
- [ ] Allocation counts are tracked for performance-critical paths, either via benchmarks or unit-test assertions.
- [ ] Changes to allocation counts require explicit acknowledgment in review (updated test expectations or documented justification).

### 15.6.5 Optimization PRs
- [ ] The PR states the hypothesis and the profiling evidence that motivated the change.
- [ ] Before/after benchmark results are included, with workload description.
- [ ] The micro-benchmark improvement is validated by a system-level benchmark or load test.
- [ ] The optimization does not depend on a specific compiler version, platform, or hardware feature without documenting that dependency.
