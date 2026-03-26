# Ranges, Views, and Generators

Ranges and generators are attractive because they compress iteration into something that reads like data flow. Sometimes that is exactly what a production codebase needs. Sometimes it is how lifetime bugs, hidden work, and impossible-to-debug lazy behavior enter otherwise plain code.

The useful question is not whether range pipelines are elegant. The useful question is when lazy composition makes the structure of the work clearer than an ordinary loop, and when it obscures ownership, error handling, or cost. C++23 gives you powerful range machinery and `std::generator` for pull-based sequences. Neither should become the default shape for all iteration.

The sample domains here are realistic ones: filtering logs before export, transforming rows in a batch job, and exposing paged or coroutine-backed sources as pull-based sequences. The design pressure is the same in each case. Work arrives over time. The code wants to express a sequence of transformations. The risks are delayed execution, borrowed state, and confusion about where the data actually lives.

## Pipelines Earn Their Place When the Dataflow Is the Main Story

An ordinary loop is still the right tool for many jobs. It keeps sequencing obvious, makes side effects explicit, and is easy to step through. A range pipeline earns its place when the essential structure of the work is “take a sequence, discard some elements, transform the survivors, then materialize or consume.”

Suppose a log-export worker receives a batch of parsed records and needs to ship only security-relevant entries after projecting them into an export schema. First, consider the pre-ranges approach using manual iterators:

```cpp
// Pre-C++20: manual iterator loop with filter + transform
std::vector<ExportRow> export_rows;
for (auto it = records.begin(); it != records.end(); ++it) {
    if (it->severity >= Severity::warning && !it->redacted) {
        ExportRow row;
        row.timestamp = it->timestamp;
        row.service   = it->service;
        row.message   = it->message;
        export_rows.push_back(row);
    }
}
```

This version works, but the filtering logic and the transformation logic are fused into a single loop body. The reader must parse the `if` to understand what is kept and parse the body to understand what is produced. In more complex cases, these loops accumulate nested conditions, early `continue` statements, index arithmetic, and manual bookkeeping that obscure the data flow. Off-by-one errors in index-based variants (`for (size_t i = 0; i < records.size(); ++i)`) are a persistent source of bugs, especially when the loop body mutates the container or uses the index for more than one purpose.

The range version separates the concerns structurally:

```cpp
auto export_rows = records
    | std::views::filter([](const LogRecord& r) {
          return r.severity >= Severity::warning && !r.redacted;
      })
    | std::views::transform([](const LogRecord& r) {
          return ExportRow{
              .timestamp = r.timestamp,
              .service = r.service,
              .message = r.message,
          };
      })
    | std::ranges::to<std::vector>();
```

This is good range code because the pipeline is the business logic. There is no tricky mutation, no lifetime ambiguity in the source, and a clear materialization point at the end. The intermediate storage never existed conceptually, so not allocating it improves both clarity and cost.

Now compare that with a loop that updates shared counters, emits metrics, mutates records in place, and conditionally retries downstream writes. A pipeline there often hides the part that matters most: sequencing and side effects. Range syntax is not a readability win when the computation is stateful and effect-heavy.

Another common pre-ranges pattern worth examining is the "erase-remove" idiom for in-place filtering:

```cpp
// Pre-C++20 erase-remove idiom
records.erase(
    std::remove_if(records.begin(), records.end(),
                   [](const LogRecord& r) {
                       return r.severity < Severity::warning || r.redacted;
                   }),
    records.end());
```

This is correct but notoriously easy to get wrong. Forgetting the `.end()` argument to `erase` is a well-known bug that compiles but leaves the removed elements in the container. The logic is also inverted: you specify what to *remove* rather than what to *keep*, which is a common source of predicate errors. C++20 introduced `std::erase_if` to simplify this, and range pipelines avoid the problem entirely by producing a new view rather than mutating in place.

The companion project `examples/web-api/` contains a small but representative example of this pattern. In `repository.cppm`, the `find_completed` method filters tasks by completion status using a `views::filter` pipeline:

```cpp
// examples/web-api/src/modules/repository.cppm
[[nodiscard]] std::vector<Task> find_completed(bool completed) const {
    std::shared_lock lock{mutex_};
    auto view = tasks_
        | std::views::filter([completed](const Task& t) {
              return t.completed == completed;
          });
    return {view.begin(), view.end()};
}
```

The pipeline is the business logic — filter by a predicate, then materialize into an owning result before it crosses the API boundary. There is no lifetime ambiguity because the lock keeps the source alive for the duration and the result is an independent `vector`.

The rule is straightforward. Use range pipelines for linear dataflow over a sequence. Use loops when control flow, mutation, or operational steps are the story.

## Views Borrow, and Laziness Moves Bugs Later in Time

The hardest production bug class with ranges is not algorithmic. It is lifetime. Views are often non-owning, and lazy pipelines delay work until iteration. That means the place where a view is constructed and the place where its bug becomes visible may be far apart.

Consider an anti-pattern that shows up in request processing code:

```cpp
auto tenant_ids() {
    return load_tenants()
        | std::views::transform([](const Tenant& t) {
              return std::string_view{t.id};
          }); // BUG: returned view depends on destroyed temporary container
}
```

The code looks tidy. It is wrong. `load_tenants()` returns an owning container temporary. The view pipeline borrows from that container. Returning the view returns a delayed lifetime bug.

This is the central design rule for views: the owner must outlive the view, and that fact must stay obvious to readers. If the lifetime relationship is subtle, the abstraction is already too clever.

There are several safe patterns.

- Build and consume the pipeline in one local scope where the owner is visibly alive.
- Materialize an owning result before crossing a boundary.
- Return an owning range or domain object when the caller cannot reasonably track the source lifetime.
- Use `std::generator` or another owning abstraction when the sequence is produced over time rather than borrowed from existing storage.

Borrowing is not a defect. Hidden borrowing is.

## Do Not Export Deep Lazy Pipelines as Casual APIs

Internally, ranges are often excellent glue. At subsystem boundaries, they deserve more caution. Returning a deep view stack from a public API exports not just a sequence but a bundle of lifetime assumptions, iterator category behavior, evaluation timing, and sometimes surprising invalidation rules.

That is a lot of semantic surface for a caller who may only want “the filtered records.”

In a library or large service boundary, ask what the caller really needs.

- If they need an owned result, return one.
- If they need pull-based traversal over expensive or paged data, consider a generator.
- If they need customizable traversal with local control, expose a callback-based visitor or a dedicated iterator abstraction.

Returning a lazily composed view is strongest inside a local implementation region where one team owns both sides of the contract and the lifetime story is visually short.

The companion project shows two range-aware designs that stay within safe boundaries. First, `json.cppm` defines a function template that accepts any `input_range` whose elements satisfy `JsonSerializable`, combining range constraints with concepts:

```cpp
// examples/web-api/src/modules/json.cppm
template <std::ranges::input_range R>
    requires JsonSerializable<std::ranges::range_value_t<R>>
[[nodiscard]] std::string serialize_array(R&& range) {
    std::string result = "[";
    bool first = true;
    for (const auto& item : range) {
        if (!first) result += ',';
        result += item.to_json();
        first = false;
    }
    result += ']';
    return result;
}
```

The function returns an owned `std::string` — no lazy view escapes the boundary. The range constraint ensures that only collections of serializable types are accepted, and the error message from a constraint violation names the concept rather than pointing at the loop body.

Second, `middleware.cppm` uses `std::ranges::rbegin` and `std::ranges::rend` to iterate a middleware collection in reverse, so that the first middleware in the list wraps outermost:

```cpp
// examples/web-api/src/modules/middleware.cppm
template <std::ranges::input_range R>
    requires std::same_as<std::ranges::range_value_t<R>, Middleware>
[[nodiscard]] http::Handler
chain(R&& middlewares, http::Handler base) {
    http::Handler current = std::move(base);
    for (auto it = std::ranges::rbegin(middlewares);
         it != std::ranges::rend(middlewares); ++it)
    {
        current = apply(*it, std::move(current));
    }
    return current;
}
```

This is a good use of range utilities inside a local algorithm: the reverse iteration intent is expressed through `rbegin`/`rend` rather than index arithmetic, and the function produces an owned result.

The same caution applies to `string_view` and `span` inside pipelines. A transform that produces borrowed slices is fine if the source lifetime stays local and obvious. It is risky if those slices are smuggled across threads, queued for later work, or cached.

## `std::generator` Is for Pull-Based Sources, Not for Replacing Every Loop

C++23's `std::generator` is useful because some sequences are not naturally “stored then traversed.” They are produced incrementally: paged database scans, directory walks, chunked file reads, retry-aware polling, or protocol decoders that yield complete messages as bytes arrive.

This is where generators change design. They let the producer keep state between elements without forcing the caller into callback inversion or hand-written iterator machinery.

A batch job that reads pages from a remote API is a good example. Before generators, expressing an incremental page-fetching sequence required either callback inversion or a hand-written iterator class:

```cpp
// Pre-C++23: hand-written iterator for paged results
class PagedResultIterator {
public:
    using value_type = Row;
    using difference_type = std::ptrdiff_t;

    PagedResultIterator() = default; // sentinel
    explicit PagedResultIterator(Client& client)
        : client_(&client) { fetch_next_page(); }

    const Row& operator*() const { return rows_[index_]; }
    PagedResultIterator& operator++() {
        if (++index_ >= rows_.size()) {
            if (next_token_.empty()) { client_ = nullptr; return *this; }
            fetch_next_page();
        }
        return *this;
    }
    bool operator==(const PagedResultIterator& other) const {
        return client_ == other.client_;
    }

private:
    void fetch_next_page() {
        auto page = client_->fetch(next_token_);
        rows_ = std::move(page.rows);
        next_token_ = std::move(page.next_token);
        index_ = 0;
    }

    Client* client_ = nullptr;
    std::vector<Row> rows_;
    std::string next_token_;
    std::size_t index_ = 0;
};
```

This is roughly forty lines of boilerplate to express "fetch pages and yield rows." The same logic with `std::generator`:

```cpp
std::generator<Row> paged_rows(Client& client) {
    std::string token;
    do {
        auto page = client.fetch(token);
        for (auto& row : page.rows)
            co_yield std::move(row);
        token = std::move(page.next_token);
    } while (!token.empty());
}
```

The generator version keeps all state (page token, current buffer, position) implicit in the coroutine frame. The control flow is obvious. The hand-written iterator version is error-prone: the sentinel comparison, the index bookkeeping, and the fetch-on-boundary logic are all manual and easy to get subtly wrong.

Materializing all rows before processing may waste memory and delay first useful work. A generator can express a sequence of yielded rows while keeping page tokens, buffers, and retry state local to the producer.

That said, generators are coroutine machinery. They come with suspension points, frame lifetime, and sometimes allocation cost depending on implementation and optimization. They are not free. They are also harder to debug than a local vector and a loop. Use them when incremental production is the real structure of the problem, not as a fashionable replacement for ordinary containers.

Another boundary question matters here. Does the generator yield owned values or borrowed references into internal buffers? Yielding borrowed references can be correct, but only when the lifetime across suspension points is explicit and easy to reason about. In many cases, yielding small owning values is the safer trade.

If your standard library support for `std::generator` is incomplete on a target toolchain, the same design guidance still applies to an equivalent coroutine-backed generator type. The question is structural, not vendor-specific.

## Laziness Helps Until Observability and Error Handling Matter More

Lazy pipelines are appealing because they delay work until needed. That is often useful. It also means instrumentation, exceptions or `expected` propagation, and failure attribution may happen later than readers expect.

In a log processing path, a pipeline that filters, parses, enriches, and serializes on demand may look elegant, but operationally it can smear a failure across the eventual consumer. When parsing fails, where does the error belong? When metrics should count discarded records, where is the increment performed? When tracing needs stage-level timing, what exactly is a stage in a fused lazy pipeline?

This is where materialization points earn their keep. Breaking a long pipeline into named phases with owned intermediate results can make the system easier to observe and reason about, even if it costs a little memory. Not every temporary allocation is waste. Some are what let you attach metrics, isolate faults, and put the debugger in the right place.

Do not confuse laziness with efficiency. Sometimes fusing operations reduces work. Sometimes it blocks parallelization, complicates branch prediction, or simply makes the expensive part harder to see. Benchmark the whole path rather than assuming the pipeline form is faster.

## Where Ranges and Generators Stop Being the Right Tool

Ranges are a poor fit when mutation is central, when control flow is irregular, when early exits carry important side effects, or when the algorithm is already dominated by external I/O or locking. Generators are a poor fit when a plain container result is cheaper and simpler, when the sequence must be revisited multiple times, or when coroutine lifetime across subsystems would be harder to reason about than a local buffer.

Another common failure is turning pipelines into performance theater. A chain of five adaptors over unstable borrowed state is not better engineering than a loop with three well-named variables. The winning design is the one whose ownership, cost, and failure behavior remain easy to explain.

## Verification and Review

Ranges and generators deserve specific review questions.

- What owns the data a view is traversing, and is that owner visibly alive for the full iteration?
- Where does lazy work actually execute, and is that timing acceptable for error handling and metrics?
- Is there a deliberate materialization point where ownership or observability should become explicit?
- Would an ordinary loop communicate control flow and side effects more clearly?
- Does a generator yield owned values or borrowed ones, and is that lifetime valid across suspension?

Dynamic tools matter here. AddressSanitizer and UndefinedBehaviorSanitizer are good at exposing view lifetime mistakes once exercised. Benchmarks help when pipelines are adopted for throughput claims. But review still carries most of the burden because many lazy-lifetime bugs are structurally obvious if the ownership story is traced carefully.

## Takeaways

- Use range pipelines when linear dataflow is the main story and side effects are secondary.
- Treat every view as a borrowing abstraction whose owner must remain obvious and alive.
- Avoid exporting deep lazy pipelines across broad API boundaries unless the lifetime contract is genuinely clear.
- Use generators for incrementally produced sequences, not as a replacement for simple stored results.
- Insert materialization points when observability, error boundaries, or lifetime clarity matter more than maximal laziness.