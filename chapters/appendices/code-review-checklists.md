# Appendix C: Code Review Checklists

This appendix is the short form of the review posture developed across the book and expanded in [Chapter 23](../23-reviewers-checklist-modern-cpp-code.md). Use it as a compact pass over a diff, not as a substitute for thinking about the dominant risk in the change.

## Ownership and Lifetime

- What new resources exist after this change, and who owns each one?
- Did the change introduce stored `std::string_view`, `std::span`, iterators, references, or views whose source may disappear first?
- Did a borrow start crossing time, threads, queues, callbacks, or coroutine suspension points?
- Is `std::shared_ptr` solving a real multi-owner lifetime, or compensating for the absence of a named owner?
- If move or copy behavior changed, is that part of the contract or an accidental cost shift?

## Invariants and Failure Boundaries

- What invariant must remain true before and after the new code runs?
- If the operation fails halfway through, what cleanup or rollback is guaranteed?
- Does the change keep the existing error model, or did it add a second error channel?
- Are `std::expected`, `[[nodiscard]]`, and status-bearing return values still checked consistently?
- Were low-level errors translated near the boundary, or did vendor detail leak upward?

## Interfaces and Library Surfaces

- Do parameter and return types now communicate ownership, retention, mutability, and cost more honestly or less honestly?
- Did the public surface widen in a way that commits callers to templates, callbacks, allocators, synchronization, or vendor types they should not need to know about?
- If concepts, type erasure, or callbacks were added, why is this the right seam?
- If the change affects a library boundary, what compatibility promise changed: source, ABI, serialized format, or semantics?
- Can a caller understand the contract from the signature and surrounding docs, or only from implementation detail?

## Concurrency, Cancellation, and Shutdown

- Who owns each new piece of asynchronous work?
- What stops it: parent completion, failure, deadline, explicit stop, or shutdown?
- Is concurrent work bounded at the scarce resource, or can overload turn into queue growth and latent failure?
- Do locks protect invariants, or only fields?
- Does any callback, logging path, I/O path, or foreign code now run while a lock is held?
- After this change, what work can still be running when destruction starts, and how does it stop?

## Data Layout and Performance

- Did the change add allocation, indirection, copying, or object-size growth on a meaningful path?
- If a container or representation changed, what workload assumption made that choice better?
- Did a ranges or view-based rewrite preserve lifetime and traversal clarity?
- If the change claims a performance win, where is the benchmark, profile, or production evidence?
- If the change claims the cost is negligible, under what input shape and load is that true?

## Verification and Delivery

- Which tests exercise the risky path introduced by the change?
- Which sanitizer, static-analysis, or build-diagnostics lanes still cover it?
- If the diff changes overload behavior, retries, shutdown, or failure classification, is that visible in logs, metrics, traces, or crash diagnostics?
- Did the build or CI configuration become more diagnosable or less diagnosable?
- Can the shipped artifact still be mapped back to exact symbols, source, and build identity if it fails in production?

## When the Checklist Should Block Approval

Block the change when ownership is unclear, borrowed state may outlive its source, failure policy is inconsistent, async work has no owner, overload behavior is implicit, a public boundary widened without compatibility reasoning, or a performance claim lacks evidence proportional to the claim.