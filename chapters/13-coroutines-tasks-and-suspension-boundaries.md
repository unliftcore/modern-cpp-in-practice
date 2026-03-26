# Coroutines, Tasks, and Suspension Boundaries

This chapter assumes you already understand failure transport and concurrent shared-state design. The focus here is narrower: what a coroutine owns, what survives suspension, and what goes wrong when asynchronous control flow hides lifetime.

## The Production Problem

Coroutines make asynchronous code easier to read and easier to lie about.

A request handler that previously nested callbacks can become straight-line code. A streaming parser can yield values naturally. A background refresh job can `co_await` timers and I/O instead of hand-writing a state machine. Those are real gains. But the machinery did not disappear. The state machine still exists. It now lives in a coroutine frame whose lifetime, ownership, and resumption context must be designed on purpose.

Production failures with coroutines usually have one of four shapes:

1. Borrowed data outlives its source across suspension.
2. A task has no clear owner, so work outlives the component that started it.
3. Failure and cancellation paths are implicit, so suspended work resumes into invalid assumptions.
4. Execution hops across threads or executors in ways the code does not make obvious.

This chapter keeps the scope local. The question is not yet how a whole task tree should be managed under cancellation pressure. That is Chapter 14. The question here is what each coroutine actually is: a resource-owning object with suspension points that define lifetime boundaries.

## What Coroutines Replace: Callback Hell and Manual State Machines

To appreciate coroutine design tradeoffs, see what they displace. Pre-coroutine asynchronous code relies on continuation-passing style, where each step chains a callback into the next. A simple "fetch, validate, store" sequence looks like this:

```cpp
// Continuation-passing style — correct but unreadable at scale.
void handle_request(request req, std::function<void(response)> done) {
    fetch_profile(req.user_id, [req, done](std::expected<profile, error> prof) {
        if (!prof) { done(error_response(prof.error())); return; }
        validate_access(prof->role, req.resource,
            [req, prof = *prof, done](std::expected<bool, error> ok) {
                if (!ok || !*ok) { done(denied_response()); return; }
                store_audit_log(req, prof,
                    [req, prof, done](std::expected<void, error> result) {
                        if (!result) { done(error_response(result.error())); return; }
                        done(success_response(prof));
                    });
            });
    });
}
```

Every step nests deeper. Error handling is duplicated at each level. Lifetime of captured values must be managed manually — capture by value inflates copies, capture by reference invites dangling. Adding timeout, cancellation, or retry logic multiplies the nesting further. This is not a strawman; it is the shape of real pre-coroutine async C++ in production codebases.

The coroutine equivalent:

```cpp
task<response> handle_request(request req) {
    auto prof = co_await fetch_profile(req.user_id);
    if (!prof) co_return error_response(prof.error());

    auto ok = co_await validate_access(prof->role, req.resource);
    if (!ok || !*ok) co_return denied_response();

    auto result = co_await store_audit_log(req, *prof);
    if (!result) co_return error_response(result.error());

    co_return success_response(*prof);
}
```

Sequential reading, single error path per step, no nesting. The improvement is real. But the state machine did not vanish — it moved into the coroutine frame. The rest of this chapter is about what that means for ownership and lifetime.

The example project's handler layer (`examples/web-api/src/modules/handlers.cppm`) shows a synchronous version of this same structural benefit. Each handler is a function that accepts `const http::Request&` (borrowed) and returns `http::Response` (owned). The control flow is straight-line: parse the path parameter, validate input, call the repository, translate the result to HTTP. Error handling is local to each step, not nested inside callbacks. While these handlers are not coroutines, they demonstrate the same principle — when each step is sequential and error paths are flat, business logic stays readable and reviewable.

## A Coroutine Is a State Machine With Storage

Treating coroutines as syntax sugar is the fastest way to ship lifetime bugs.

When a function becomes a coroutine, some state moves into a frame. Parameters may be copied or moved there. Locals that live across suspension reside there. Awaiter state may influence when and where execution resumes. Destruction may happen on success, on failure, on cancellation, or when the owning task object is destroyed. None of that is cosmetic.

This matters because ordinary stack intuition stops being reliable. In a non-coroutine function, a local dies when control exits the scope. In a coroutine, a local that spans suspension may live much longer than the caller expects, while a borrowed view into caller-owned storage may become invalid long before resumption.

The core review question is simple: what data must remain valid from one suspension point to the next?

## Suspension Points Are Lifetime Boundaries

Every `co_await` is a boundary where ordinary assumptions should be re-checked.

Before suspension, ask:

1. Which references, spans, string views, iterators, and pointers will still be needed afterward?
2. Who owns the storage they refer to?
3. Can the awaited operation outlive the caller, request, or component that initiated it?
4. On which executor or thread will resumption occur?

This is the local equivalent of API lifetime review. If the coroutine keeps borrowed data across suspension, you must prove that the owner outlives the coroutine or change the coroutine to take ownership.

That is why coroutine APIs often need stricter parameter choices than synchronous ones. A synchronous helper might safely accept `std::string_view` because it finishes immediately. An asynchronous task that suspends usually should not keep that view unless the ownership contract is extremely tight and documented.

The example project's `Request::path_param_after()` (`examples/web-api/src/modules/http.cppm`) illustrates the safe side of this boundary. It returns `std::optional<std::string_view>` pointing into the request's `path` member. That is safe here because the handler executes synchronously — the `Request` object outlives the entire handler call. If these handlers were coroutines that suspended mid-execution, the same `string_view` would become a dangling reference the moment the request buffer was recycled. The design works precisely because the lifetime contract is simple: the request lives for the duration of the handler, and handlers do not suspend.

## Anti-pattern: Borrowed Request State Survives Suspension

```cpp
// Anti-pattern: borrowed data may dangle after suspension.
task<parsed_request> parse_and_authorize(
    std::string_view body,
    const auth_context& auth) {

    auto token = co_await fetch_access_token(auth.user_id());
    co_return parse_request(body, token); // BUG: body may refer to caller-owned storage.
}
```

This code looks efficient because it avoids a copy. It is only correct if the caller guarantees that `body` remains valid until the coroutine completes. In service code that often means until network I/O, authentication lookup, retries, and timeout handling have all finished. That is not a small promise. It is usually the wrong one.

The safer default is to move ownership into the coroutine when the data is needed after suspension.

```cpp
task<parsed_request> parse_and_authorize(
    std::string body,
    auth_context auth) {

    auto token = co_await fetch_access_token(auth.user_id());
    co_return parse_request(body, token);
}
```

The copy or move is visible and reviewable. The coroutine frame now owns what it needs. If that allocation cost matters, measure it and redesign around message boundaries or storage reuse. Do not silently borrow across time.

## More Lifetime Traps: Locals, Temporaries, and Lambda Captures

The borrowed-parameter anti-pattern above is the most common case, but coroutine lifetime bugs take other forms that deserve explicit attention.

### Dangling reference to a caller's local

```cpp
// BUG: coroutine captures a reference to a local that dies when the caller returns.
task<void> start_processing(dispatcher& d) {
    std::vector<record> batch = build_batch();
    co_await d.schedule([&batch] {     // lambda captures batch by reference
        process(batch);                // batch may be destroyed if start_processing
    });                                // is suspended and its caller exits
}
```

When `start_processing` suspends at `co_await`, the coroutine frame keeps `batch` alive — but only if the frame itself is alive. If the task is detached or the parent scope exits, the frame is destroyed, and the lambda's reference dangles. The fix: capture by value, or ensure the parent scope outlives the scheduled work through structured ownership.

### Temporary lifetime collapse

```cpp
// BUG: temporary string destroyed before coroutine body executes.
task<void> log_message(std::string_view msg);

void caller() {
    log_message("request started"s + request_id()); // temporary std::string
    // temporary is destroyed here, before the coroutine even begins if lazy-start
}
```

With a lazy-start coroutine, the temporary `std::string` is destroyed at the semicolon, but the coroutine has not yet executed. Even with eager-start coroutines, if the frame stores `msg` as a `string_view`, it points to freed memory after the first suspension. The solution is to accept `std::string` by value in the coroutine signature so the frame owns a copy.

### The `this` pointer across suspension

```cpp
// BUG: 'this' may dangle if the object is moved or destroyed while suspended.
class connection {
    std::string peer_addr_;
public:
    task<void> run() {
        auto data = co_await read_socket();    // suspended here
        log("received from " + peer_addr_);    // 'this' may be invalid
    }
};
```

If a `connection` object is moved into another container, or destroyed while `run()` is suspended, `this` becomes invalid at resumption. Member coroutines are safe only when the object's lifetime is guaranteed to exceed the coroutine's. In practice, this often means the coroutine should take a `shared_ptr<connection>` or the owning scope must be structured to prevent destruction during suspension.

These are not exotic edge cases. They are the normal failure modes of coroutine lifetime in production code. Every suspension point is a moment where the caller's world may have changed.

## Task Types Are Ownership Contracts

The return type of a coroutine is not decoration. It defines ownership, result transport, and destruction semantics.

A useful task type answers at least these questions:

1. Does destroying the task cancel work, detach it, block, or leak it?
2. Is the result observed exactly once, many times, or not at all?
3. How are exceptions transported?
4. Can the task start eagerly, or only when awaited?
5. Is cancellation represented explicitly?

Many coroutine bugs are really task-type bugs. A detached "fire-and-forget" coroutine is not an asynchronous style choice. It is an ownership claim that no later code needs to know when the work finishes, whether it failed, or whether it should be canceled during shutdown. That claim is rarely true in production services.

The conservative default is simple: every started task should have a clear owner and a visible completion path. If you cannot name the owner, you are building orphaned work.

## Eager Versus Lazy Start Changes Failure Timing

Whether a coroutine begins running immediately or only when awaited affects correctness, not just performance.

An eager task may start side effects before the caller stores the handle. A lazy task may delay work until orchestration code decides where and when it should run. Both are valid. What matters is that the behavior is consistent and documented.

This influences failure boundaries. If task construction can start work, exceptions and cancellation may become observable before any parent scope thinks the task is "live." If work starts only on first await or explicit scheduling, ownership is usually easier to reason about.

The recommendation for production code is not one universal policy. It is that the task abstraction must make the policy obvious enough that reviewers do not need to inspect promise-type internals to know when side effects begin.

## Resumption Context Is Part of Correctness

Coroutine code often reads like it stays on one thread. That is an illusion.

An awaited operation may resume on an I/O thread, a scheduler pool, a UI-affine thread, or an executor chosen by the awaiter. If the code touches thread-confined state after resumption, or if it expects continuation on a specific executor, that requirement must be explicit in the abstraction.

This is where teams recreate callback-era bugs with prettier syntax. The control flow looks sequential, so reviewers stop asking where the continuation runs. Then a coroutine resumes on a pool thread and touches request-local state that was only safe on the initiating executor.

Make resumption policy visible in one of three ways:

1. The task type carries a clear scheduler or executor contract.
2. The code explicitly switches context before using thread-affine resources.
3. The component is designed so post-await code is executor-agnostic.

If none is true, the coroutine is relying on ambient behavior. Ambient behavior breaks under refactoring.

## Coroutines Do Not Remove Error-Boundary Design

A `co_await` does not answer whether failure should throw, return `std::expected`, request cancellation, or terminate a larger operation. It merely changes the control-flow shape.

That means the error-boundary decisions from Chapter 3 still apply. Keep them consistent inside task APIs:

1. Use exceptions for domains where stack unwinding across internal layers is acceptable and well understood.
2. Use result types when failure is expected, compositional, and part of ordinary business flow.
3. Decide how cancellation appears in the value space, exception space, or task state.
4. Make timeout handling explicit rather than burying it in an awaiter with surprising policy.

The failure model should be readable at the call site. "This coroutine may suspend" is not enough information. The caller also needs to know what completion means and what failed completion looks like.

## Generators and Tasks Solve Different Problems

C++23 coroutine support enables both pull-style generators and asynchronous tasks. Do not blur them.

Generators are about staged production of values with a local consumer. They often work well for streaming parse pipelines, tokenization, batched traversal, or incremental transformation. Their main concerns are iterator validity, producer lifetime, and whether yielded references remain valid.

Tasks are about eventual completion of asynchronous work. Their concerns are ownership, scheduling, cancellation, and result transport.

They share machinery and deserve different review questions. A generator bug is often "what does this yielded reference point to?" A task bug is often "who owns this work after suspension?" Keeping those categories separate makes code review sharper.

## Destruction and Cancellation Must Compose

Coroutine cleanup paths are easy to ignore because the happy path looks linear.

Ask what happens if the owning scope exits while the coroutine is suspended. Does destruction request cancellation? Does it wait for child operations? Can it race with completion? Are outstanding registrations, file descriptors, timers, or buffers released exactly once?

These are not implementation details. They are the semantic contract of the task abstraction.

If coroutine destruction merely drops the handle while the underlying operation continues somewhere else, that is detach-by-destruction. Sometimes that is intentional. More often it is a shutdown bug waiting to happen.

## Verification for Coroutine Code

Testing coroutine logic with only success-path unit tests is not enough. Verification should target boundary behavior:

1. Lifetime tests that force the caller-owned data to disappear before resumption.
2. Cancellation tests that interrupt the coroutine at multiple suspension points.
3. Scheduler tests that resume on unexpected executors to catch thread-affinity assumptions.
4. Failure-path tests for exceptions, error results, and timeout races.
5. Sanitizer runs for use-after-free and race detection when coroutine state interacts with shared objects.

For high-value components, it is often worth writing deterministic test awaiters or fake schedulers so resumption order can be controlled instead of guessed.

## Review Questions for Coroutines

Before approving coroutine code, ask:

1. What lives in the coroutine frame, and who owns it?
2. Which borrowed views or references survive across suspension?
3. Who owns the task after it is started?
4. When do side effects begin: on construction, on scheduling, or on first await?
5. On what context does resumption happen?
6. How are failure, timeout, and cancellation represented?
7. What happens if the initiating component shuts down mid-suspension?

If those answers are vague, the coroutine is not simpler than callback code. It is just easier to misread.

## Takeaways

Coroutines improve control-flow clarity. They do not remove lifetime design.

Treat every suspension point as a boundary where ownership, resumption context, and failure semantics must still make sense. Prefer task types with explicit ownership and completion behavior. Move data into the coroutine frame when it must survive suspension. Keep generators and asynchronous tasks conceptually separate. Most importantly, do not confuse sequential-looking source code with sequential lifetime. Coroutine correctness depends on what persists across time, not on how tidy the `co_await` chain looks.