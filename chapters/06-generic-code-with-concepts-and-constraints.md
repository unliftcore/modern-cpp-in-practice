# Generic Code with Concepts and Constraints

Generic code is valuable when a family resemblance in the problem is real. It is destructive when authors use templates to postpone interface decisions. The production problem is not “how do I make this reusable?” It is “how do I remove duplication without making the call contract, diagnostics, and failure behavior opaque?”

Concepts are the first C++ feature in a long time that directly improves this situation at the boundary where readers need help most. They do not make generic code automatically simple. They make it possible to state what a template expects in terms close to the actual design. That is a major shift from the era of “instantiate it and hope the compiler error points at the right line.”

This chapter focuses on constrained generic code that ordinary product teams can maintain: reusable transforms, narrow extension points, policy objects, and algorithm families whose assumptions must remain reviewable.

## Start With the Variation, Not With the Template

Most bad generic code starts from a false premise: “these functions look similar, so I should template them.” Similar surface syntax is not enough. The real question is which parts of the design are allowed to vary and which invariants must stay fixed.

Imagine an internal observability library that writes metric batches to different sinks: in-memory test collectors, local files, and a network exporter. The invariant parts are straightforward: a batch has a schema, timestamps must be monotonic within a flush, serialization failures must be reported, and shutdown must not lose acknowledged data. The variable part is where bytes go.

That points to a narrow generic seam. It does not justify templating the entire pipeline.

If you template everything from parsing through retry logic through transport mechanics, you are no longer writing reusable code. You are building a second language inside the codebase. Concepts help only if the variation boundary was already honest.

## Constrain the Boundary So the Implementation Can Stay Ordinary

The main practical use of concepts is not clever overload ranking. It is telling callers, and the compiler, what operations your algorithm is allowed to assume.

Consider a batching helper that writes already-serialized records to some sink:

```cpp
template <typename Sink>
concept ByteSink = requires(Sink sink,
							std::span<const std::byte> bytes) {
	{ sink.write(bytes) } -> std::same_as<std::expected<void, WriteError>>;
	{ sink.flush() } -> std::same_as<std::expected<void, WriteError>>;
};

template <ByteSink Sink>
auto flush_batch(Sink& sink,
				 std::span<const EncodedRecord> batch)
	-> std::expected<void, WriteError>
{
	for (const auto& record : batch) {
		if (auto result = sink.write(record.bytes); !result) {
			return std::unexpected(result.error());
		}
	}
	return sink.flush();
}
```

This does several things well.

- The concept names the role, not the implementation technique.
- The required operations are few and operationally meaningful.
- Failure behavior is part of the contract.
- The function body is ordinary code; nothing about the implementation has become more abstract than the problem demands.

The alternative is the classic unconstrained template:

```cpp
template <typename Sink>
auto flush_batch(Sink& sink, const auto& batch) {
	for (const auto& record : batch) {
		sink.push(record.data(), record.size()); // RISK: hidden, undocumented assumptions
	}
	sink.commit();
}
```

This version looks shorter. It is worse in every production-relevant way. The assumptions are unstated. The error contract is unclear. The required record shape is accidental. A mismatch produces compiler noise at the use site rather than a crisp statement of the interface.

### The Error Message Problem, Concretely

To appreciate what concepts actually fix, consider what happens when someone passes a wrong type to the unconstrained version:

```cpp
struct BadSink {};
BadSink sink;
std::vector<EncodedRecord> batch = /* ... */;
flush_batch(sink, batch);
```

Without concepts, the compiler instantiates the template body and fails deep inside the implementation. A typical error from a major compiler looks something like:

```
error: 'class BadSink' has no member named 'push'
    in instantiation of 'auto flush_batch(Sink&, const auto&) [with Sink = BadSink; ...]'
    required from here
note: in expansion of 'sink.push(record.data(), record.size())'
error: 'class BadSink' has no member named 'commit'
note: in expansion of 'sink.commit()'
```

This is two errors for a simple case. In production, the template is rarely this shallow. The sink might be passed through three layers of adapters, each a template. The actual error appears at the bottom of a deep instantiation stack, and the programmer must mentally unwind the chain to figure out what went wrong. With heavily nested templates and standard library types involved, these diagnostics routinely span dozens of lines.

With the `ByteSink` concept, the same mistake produces a single, targeted error at the call site:

```
error: constraints not satisfied for 'auto flush_batch(Sink&, ...) [with Sink = BadSink]'
note: because 'BadSink' does not satisfy 'ByteSink'
note: because 'sink.write(bytes)' would be ill-formed
```

The error names the concept, names the unsatisfied requirement, and points at the call site rather than the implementation internals. The programmer knows immediately what interface `BadSink` needs to provide.

### What This Replaced: SFINAE

Before concepts, the standard technique for constraining templates was SFINAE (Substitution Failure Is Not An Error). The idea was to make the template signature itself ill-formed for wrong types, so the compiler would silently remove it from overload resolution rather than producing a hard error.

The equivalent of the `ByteSink` constraint in pre-C++20 code looked like this:

```cpp
// SFINAE approach (using enable_if to constrain the same interface)
template <typename Sink,
          std::enable_if_t<
              std::is_same_v<
                  decltype(std::declval<Sink&>().write(
                      std::declval<std::span<const std::byte>>())),
                  std::expected<void, WriteError>
              > &&
              std::is_same_v<
                  decltype(std::declval<Sink&>().flush()),
                  std::expected<void, WriteError>
              >,
              int> = 0>
auto flush_batch(Sink& sink, std::span<const EncodedRecord> batch)
    -> std::expected<void, WriteError>;
```

This is the same constraint expressed in a form that nobody wants to read. `std::enable_if_t` with `decltype` and `std::declval` is not describing a design intent; it is exploiting a compiler mechanism. The resulting error messages when SFINAE rejects the overload are typically just "no matching function for call to `flush_batch`" with no indication of which requirement was not met. When multiple SFINAE-guarded overloads exist, the compiler may list every candidate it rejected without explaining why any individual one failed. The concept version is better in every dimension: readability, error quality, and maintainability.

Constrain the public surface aggressively so the implementation can remain boring. That is the right trade.

## Concepts Should Describe Semantics, Not Just Syntax

A concept that merely checks for the existence of member names is better than nothing, but it can still be a weak design. Production generic code becomes maintainable when the concept corresponds to a semantic role in the system.

`SortableRange` is better than `HasBeginEndAndLessThan`. `ByteSink` is better than `HasWriteAndFlush`. `RetryPolicy` is better than `CallableWithErrorAndAttemptCount`. The more the concept reads like a design term the team already uses, the more useful it becomes in code review and diagnostics.

This matters because concepts serve two audiences.

1. The compiler uses them to select and reject instantiations.
2. Humans use them to understand what kind of thing the algorithm expects.

If the name and structure only satisfy the compiler, half the value is gone.

That does not mean concepts should try to prove every semantic law. Most useful invariants remain testable rather than statically enforceable. A `RetryPolicy` concept can require a call signature and result type. It cannot prove that the policy is idempotency-safe for a specific operation. Accept that limit. State what can be checked in the interface and verify the rest in tests and review.

## Prefer Narrow Customization Points Over Template Sprawl

Many reusable components do not need a giant concept hierarchy. They need one or two carefully chosen extension points.

Suppose a storage subsystem wants to support several record types that can be serialized into a wire buffer. A common bad move is to define a primary template, invite specializations scattered across the codebase, and let argument-dependent lookup or implicit conversions decide what happens. This makes behavior hard to discover and easy to break.

The cleaner design is usually a narrow customization point with an explicit required signature. That can be a member function if the type owns the behavior, or a non-member operation if the type should stay decoupled from the serialization library. Either way, concepts should constrain the shape and result.

The key is locality. A reviewer should be able to answer three questions quickly.

- What exactly may vary?
- What invariants remain fixed?
- Where does a new model type opt in?

If the answers span ten headers and depend on incidental overload resolution rules, the generic design is already too implicit.

## Generic Code Is Not a License to Hide Costs

Templates are notorious for hiding allocation, copying, and code-size growth behind pretty call sites. Concepts do not solve that. They only make the allowed shapes clearer.

When designing generic code, force yourself to state the operational costs that stay fixed across all models and the costs that vary by model. Does the algorithm require contiguous storage? Does it materialize intermediate buffers? Does a policy type get inlined into every translation unit? Does a concept accept both throwing and non-throwing operations, thereby smearing failure handling across the interface?

These are design questions, not optimization trivia. A template that looks abstract but only performs well for one category of types is often an unstable abstraction. Either narrow the concept or provide distinct overloads for materially different cost models.

This is especially important for headers used across large codebases. Every additional instantiation increases compile cost and potentially code size. If a component crosses shared-library or plugin boundaries, an ordinary virtual interface or type-erased callable may be the better trade, even if it gives up some inlining. Stable boundaries are often worth more than theoretical zero-overhead purity.

## When Ordinary Overloads Beat Concepts

There is a persistent temptation to use concepts as a proof that a design is modern. Resist it. If you have three known input types and no evidence the set should grow, overloads are often clearer. If you need a runtime-polymorphic boundary, use one. If the variability matters only in tests, a function object or small mockable interface may be easier to maintain than a generic subsystem.

Concepts are strongest when all of the following are true:

- the algorithm genuinely applies to a family of types,
- the required operations can be stated narrowly,
- callers benefit from compile-time rejection,
- and the implementation cost model stays legible.

When those conditions fail, templates become a liability quickly.

## Failure Modes and Boundary Conditions

Generic code tends to fail in recurring ways.

The first is unconstrained seepage: one generic function accepts “anything range-like,” another expects “anything writable,” and soon the codebase has accidental compatibility between components that were never designed to work together. Concepts should narrow those seams, not widen them.

The second is constraint duplication. Authors copy nearly identical `requires` clauses across helpers until the interface becomes impossible to evolve. Prefer named concepts for recurring requirements. A named concept is documentation, not just syntax compression.

The third is semantics drift. A concept originally built for a clean role gradually accumulates unrelated requirements because one more caller needed one more operation. When that happens, split the concept or split the algorithm. Do not let one abstraction become the dumping ground for vaguely related use cases.

The fourth is diagnostic theater: elaborate concept stacks that look principled but still produce unreadable compiler output. If users cannot tell why their type failed to model the concept, simplify. Good generic design includes failure messages people can act on.

## Verification and Review

Verification for generic code is not just “instantiate it once.”

- Add `static_assert` checks for representative positive and negative models when the concept is central enough to deserve stable examples.
- Test the algorithm with a small number of materially different model types, not a parade of cosmetic variants.
- Review whether the concept names a real role in the system or merely a bundle of operations.
- Measure compile time and code size if the generic component sits in a hot header path.
- Confirm that error behavior, allocation behavior, and ownership assumptions are visible at the constrained boundary.

Compile-time rejection is helpful only if the rejected program was actually wrong according to a design the team can explain. Otherwise you have built a sophisticated gate around an unclear interface.

## Takeaways

- Write generic code only when the variation in the problem is real and durable.
- Use concepts to constrain public boundaries so implementations can remain ordinary and readable.
- Name concepts after semantic roles, not after incidental syntax.
- Prefer narrow customization points over open-ended specialization schemes.
- If compile-time polymorphism makes costs, diagnostics, or boundaries worse, use overloads or runtime abstraction instead.