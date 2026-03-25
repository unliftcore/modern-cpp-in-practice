# Reviewer’s Checklist for Modern C++ Code

Most code review failures in C++ are not failures of intelligence. They are failures of review posture. The reviewer follows the diff mechanically, comments on naming or formatting, maybe spots one local bug, and misses the system question the change actually introduced: new ownership across threads, a hidden allocation in a hot path, an exception boundary leak, a silently widened API contract, a queue with no admission control, a sanitizer lane that can no longer exercise the risky path.

Modern C++ makes many expensive decisions visible, but only if reviewers ask the right questions. This closing chapter turns the rest of the book into a practical review routine. It is not a replacement for the appendix checklist, and it is not a style guide. It is the set of questions that should shape how an experienced reviewer reads a change in production C++.

The central idea is simple: review for failure economics before review for local elegance. A line that looks tidy can still expand lifetime, weaken invariants, or increase operational cost. The checklist below is organized around the places where those failures usually hide.

## First Pass: Identify What Kind of Change This Really Is

Before reading line by line, classify the change. A reviewer who misclassifies the change asks the wrong questions.

Is this primarily:

- a new ownership or lifetime path?
- an API or contract change?
- a concurrency or cancellation change?
- a data-layout or performance-sensitive change?
- a tooling, verification, or build-pipeline change?
- a service-behavior change with operational consequences?

Many pull requests contain more than one category, but one usually dominates. Start there. If a change adds a background queue, it is not mainly a refactor. If a function now returns `std::string_view`, it is not merely a micro-optimization. If a library starts exposing a templated callback type in public headers, it is not only a convenience improvement. Review should center on the dominant risk first.

This classification step also tells you what evidence to expect. An API change should come with contract and compatibility reasoning. A concurrency change should come with cancellation and shutdown evidence. A performance claim should come with measurement. A tooling change should explain which bug class becomes easier or harder to catch.

## Ownership and Lifetime Questions

Ownership review remains the highest-value pass in production C++ because lifetime bugs keep their talent for looking locally harmless. Ask these questions early.

Who owns each new resource, and where does that ownership end? If the answer depends on reading five files and inferring framework behavior, the design is already weak. Ownership should usually be obvious from types, object graph, and construction sites.

Did the change introduce borrowing across time? This includes storing `std::string_view`, `std::span`, iterators, references, or range views in state that may outlive the source object, request, frame, or container epoch. When the code crosses an async boundary, queue, callback, coroutine suspension, or detached thread, re-check every borrowed type aggressively.

Did the change replace clear ownership with shared ownership as a convenience? `std::shared_ptr` is sometimes the right tool. It is also a common way to postpone a design decision. Reviewers should ask what concrete lifetime problem shared ownership solves here and whether a moved value, owned work item, or explicit parent owner would make the design easier to reason about.

Were move and copy costs changed intentionally? Value semantics are powerful, but reviewers should still ask whether new copies are part of the contract or just accidental byproducts of interface design.

Good review comments in this area are concrete. "This looks risky" is weak. "This queue now stores `std::string_view` derived from request-local storage, so queued work can outlive the buffer" is strong.

## Invariants and Error Boundary Questions

The next pass is about invalid state and failure shape. Does the change make invalid state easier to create, easier to observe, or harder to recover from? Construction paths, configuration objects, partial initialization, and mutation APIs are common places where invariants become weaker silently.

Then ask how failure is reported. Are recoverable domain failures represented consistently, or did the change add a second error channel? Did a previously contained dependency error leak into a broader layer? Did a function marked `[[nodiscard]]` or returning `std::expected` gain call sites that ignore the result? If exceptions are involved, did the change widen the exception boundary accidentally?

Reviewers should also examine rollback and cleanup behavior, especially around resource-owning operations. If the new path fails halfway through, what remains true? Are partially written files removed, transactions canceled, temporary state discarded, background work stopped, and telemetry still emitted in the right category?

One useful discipline is to ask for the failure story in one sentence. "If dependency X times out after state Y is reserved, the request returns `dependency_timeout`, the reservation is released, and no background retry survives shutdown." If the author cannot say that succinctly, the error boundary may not be clear enough yet.

## Interface and Library Surface Questions

Any public or widely shared interface deserves a separate review pass because local implementation quality does not compensate for a weak contract.

Did parameter and return types become more honest or less honest? Returning `std::span<const std::byte>` may communicate borrowing clearly; returning a reference to internal mutable state may hide coupling. Accepting `std::string_view` may be right for read-only parse calls and wrong for objects that retain the string. Review should focus on what the signature now promises about ownership, cost, and failure.

If templates, concepts, callbacks, or type erasure were added, why this form? Concepts can improve diagnostics and prevent nonsense instantiations, but they also expand compile-time surface. Type erasure can stabilize call sites, but may add allocation or indirect-call cost. New genericity should pay for itself.

For library changes, ask whether the public surface leaked implementation detail. Did a new header expose a transport type, allocator strategy, synchronization primitive, or error type that callers should not have to know? Did a seemingly harmless inline helper change the ABI or source compatibility story? Did the docs or examples change with the contract, or is the new behavior discoverable only by reading the diff?

Reviewing interfaces well means thinking like the next caller, not like the current author.

## Concurrency, Time, and Shutdown Questions

Concurrency review is mostly lifecycle review with time added. The key question is not whether the code uses the right primitive names. It is whether work remains owned and bounded as it moves.

Ask whether the change introduced detached work, hidden threads, executor hops, or coroutine suspension points whose owner is not obvious. Ask how stop requests propagate. Ask whether deadlines and retries are explicit or hidden behind helper layers. Ask whether queue growth is bounded and what overload policy now applies.

If a change touches locks or shared state, review contention and invariants together. Does the lock protect the full invariant or only some fields? Did a callback get invoked while a lock is held? Did statistics or cache updates introduce a race that will be rationalized later as benign? ThreadSanitizer may catch some of this. Review should still try to eliminate the ambiguity before runtime.

Shutdown deserves its own question in almost every service or tool change: after this diff, what work can still be running when destruction begins, and how does it stop? If the answer is unclear, the review is not done.

## Data Layout and Cost Model Questions

Many performance bugs enter code review disguised as harmless abstraction. Reviewers should therefore ask where cost moved, not just whether the code "looks efficient."

Did the change add allocations on a hot path, widen objects that live in large containers, increase indirection, or turn a local value into heap-managed shared state? Did a range pipeline improve clarity while preserving lifetime, or did it create hidden iteration, temporary materialization, or dangling-view risk? Did a container choice change the memory and invalidation model in ways the author has not discussed?

The standard for review here is evidence proportional to the claim. If the change says it improves performance, ask for benchmark or profile data. If it says the extra allocation is negligible, ask under which workload. If the answer is "it probably doesn't matter," decide whether this code is actually in a place where probably is acceptable.

Not every change needs a benchmark. Performance-sensitive changes do need a cost model that survives basic questioning.

## Verification and Delivery Questions

Good C++ review continues past the source diff. The final pass is about whether the repository still has a credible way to prove the change sound.

Which tests now cover the risky path? If the change adds a rollback branch, overload behavior, or host-library boundary, is there a test that exercises it deliberately? If the change affects memory, concurrency, or input handling, do sanitizer or fuzzing lanes still cover the path? If the build or CI configuration changed, did the diagnostic matrix become stronger or weaker?

Operational changes deserve observability review as well. If the service now rejects work earlier, can operators distinguish that from dependency failure? If a library added new diagnostics, are they stable enough for hosts to consume? If crash or symbol handling changed, can the shipped artifact still be diagnosed later?

This is also where reviewers should challenge missing evidence instead of reverse-engineering it themselves. Review is not unpaid archaeology. If a change requires a new test, benchmark, sanitizer run, or migration note, ask for it plainly.

## How To Write Useful Review Comments

A good review comment identifies the risk, the violated or unclear contract, and the evidence needed to resolve it. It does not merely express taste.

Strong comments tend to look like this:

- "This callback captures `this` and is stored in work that can outlive shutdown. Who owns that lifetime after `request_stop()`?"
- "The public API now returns a `std::string_view` into parser-owned storage. Where is that storage guaranteed to outlive the caller's use?"
- "This queue is bounded, but the overload behavior is still implicit. Do we reject, block, or drop optional work, and where is that surfaced operationally?"
- "The change claims a latency win. Which benchmark or profile run demonstrates that the new allocation pattern is better under realistic input sizes?"

Weak comments are vague, purely stylistic, or framed as personal preference when the issue is actually semantic.

Reviewers should also say when evidence is sufficient. If ownership is clear, tests hit the risky path, and the contract is improved, state that. Good review is not only about blocking changes. It is about making the reasoning around acceptance explicit.

## When To Block the Change

Not every unresolved issue deserves a hard stop. Some do.

Block the change when ownership is unclear, when borrowed state may outlive its source, when the failure contract is inconsistent, when concurrency is unbounded or shutdown is undefined, when a public interface change lacks compatibility reasoning, when a performance claim lacks required evidence, or when the verification story no longer exercises the risky path.

Do not block merely because you would have written the code differently. Modern C++ already has enough accidental complexity. Review should not add another layer of taste-driven friction.

## Takeaways

The most effective C++ reviewers read for ownership, failure shape, interface honesty, concurrency lifecycle, cost movement, and verification evidence before they read for local elegance. They classify the change first, ask questions tied to production risk, and insist on evidence where the code alone is not enough.

That posture is what turns the rest of this book into day-to-day engineering behavior. The point of precise ownership models, explicit failure boundaries, bounded concurrency, honest APIs, and diagnostic discipline is not that they look modern. It is that they let a reviewer explain why a change is safe, risky, or incomplete in concrete terms.

Review questions:

- What is the dominant production risk introduced by this change, and did the review focus there first?
- Which ownership, lifetime, or borrowing assumptions now cross time, threads, or API boundaries?
- How did the change alter failure reporting, rollback guarantees, or invariant preservation?
- What cost model changed, and what evidence supports any performance or efficiency claim?
- Which tests, sanitizer lanes, diagnostics, or operational signals now prove the risky behavior remains sound?