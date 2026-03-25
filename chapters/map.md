# Book Map

The book is organized around the places where production C++ gets expensive when the design is vague. It starts with ownership, invariants, failure boundaries, and API shape because later architecture depends on those choices being reviewable. It then moves through the library and language tools that change everyday design, into interfaces and packaging, into concurrency and performance, into verification and observability, and finally into complete production patterns.

Undefined behavior is a cross-cutting concern rather than a standalone stop on the tour. The ownership chapters address lifetime and aliasing hazards. The concurrency chapters address data races, shutdown bugs, and work that outlives its owner. The data and performance chapters address invalidation, layout, locality, and false confidence from weak measurement. The verification chapters cover the tooling and runtime signals that catch what code review alone will miss.

## Part I: Core Mental Models

Part I establishes the vocabulary the rest of the manuscript depends on: ownership, lifetime, value semantics, invariants, failure boundaries, and how signatures communicate cost and retention. If these terms are fuzzy, later chapters become style arguments instead of engineering arguments.

## Part II: Writing Modern C++ Code

Part II covers the standard library and language tools that should change ordinary C++23 design: borrowing types, result and alternative types, concepts, ranges, generators, and compile-time work used with restraint. The focus is not feature coverage. It is which tools change contracts, review burden, and cost models.

## Part III: Interfaces, Libraries, and Architecture

Part III moves from local code into subsystem and package boundaries. The question becomes whether interfaces remain honest once callbacks, type erasure, modules, packaging, and ABI constraints enter the picture. This is where local elegance starts competing with long-term composability.

## Part IV: Concurrency and Asynchronous Systems

Part IV treats concurrency as ownership across time. Shared state, coroutine suspension, cancellation, and backpressure are presented as lifecycle and throughput problems, not as a catalog of primitives. The goal is to make async work bounded, owned, and stoppable.

## Part V: Data, Memory, and Performance

Part V focuses on how representation decisions become runtime behavior. Data layout, container choice, allocation policy, locality, and measurement discipline are treated as one connected cost story rather than as separate optimization tricks.

## Part VI: Verification and Delivery

Part VI covers the stack that keeps native systems honest after the design is chosen: tests aimed at boundary failures, sanitizer and static-analysis lanes, build diagnostics, and observability for running systems. The emphasis is on evidence quality, not on checklists for their own sake.

## Part VII: Production Patterns

Part VII applies the earlier chapters to complete engineering shapes. Chapter 21 shows the service boundary where ownership, admission control, shutdown, and telemetry all have to line up. Chapter 22 does the same for a reusable library whose contracts must survive other teams. Chapter 23 closes with a reviewer workflow that turns the rest of the book into day-to-day code review behavior.

## Appendices

The appendices are intentionally compact. They provide a decision-oriented feature index, a reference toolchain and diagnostics baseline, a short-form review checklist, and the glossary that anchors the book's core terminology. They are there to speed up applied work, not to become a second textbook.
