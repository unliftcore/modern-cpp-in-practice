# Style Guide

This guide defines the editorial contract for the manuscript. If a draft conflicts with this file, fix the draft.

## Prose Style

- Write for experienced programmers. Assume they understand software engineering, but not the sharp edges of modern C++ in production.
- Use direct, technical prose. Prefer specific claims over reassurance, slogans, or generic "best practice" language.
- Explain constraints and tradeoffs whenever recommending an approach.
- Introduce language features only in service of a design problem. Do not write syntax tours.
- Treat modern C++ as the default working language. Legacy code may appear as a constraint or anti-pattern, but it must not become the organizing frame of the book.
- Avoid filler, motivational framing, and tutorial scaffolding.
- Prefer short, declarative paragraphs. Long explanations must earn their length by carrying technical detail.
- Use defined terms consistently. If a chapter distinguishes ownership, lifetime, identity, value semantics, or failure domains, keep those terms stable. Core terms are defined in the glossary appendix; use those definitions as the canonical reference.

## Chapter Template

Use a two-layer model. The first layer is for the author while drafting. The second layer is what the reader sees in the finished chapter.

### Writing Skeleton

Before drafting, the author should be able to answer these questions clearly:

1. `Question` — what concrete production question does this chapter answer?
2. `Why this gets expensive` — why does this topic become costly or dangerous in real systems?
3. `Sample domain` — which realistic system or component carries the chapter?
4. `Core decisions` — what 3 to 5 decisions should the reader be able to make afterward?
5. `Modern C++ tools` — which language or library tools actually matter to those decisions?
6. `Failure modes and boundaries` — where does the design break, and where should the reader stop applying it?
7. `Verification` — what tests, diagnostics, benchmarks, sanitizers, or observability signals matter here?
8. `Takeaways` — what short heuristics or review questions should remain at the end?

These are drafting prompts, not mandatory visible section headings.

### Reader-Facing Structure

The finished chapter should usually include these elements, but not necessarily in this order and not necessarily with these exact headings:

1. A clear opening that states the production problem and why it matters.
2. One or more sections that analyze common or naive designs, modern C++ designs, and the tradeoffs between them.
3. A treatment of failure modes, limits, or boundary conditions where the recommendation stops being sound.
4. Verification or tooling discussion when it materially changes how the design should be implemented or reviewed.
5. A concise ending that leaves the reader with decisions, heuristics, or a review checklist.

Operational rules:

- Each chapter must answer a concrete production question, not merely cover a language feature.
- Do not force identical visible headings across every chapter.
- `Naive or common approach`, `verification`, and `review checklist` are common moves, not mandatory top-level section names.
- Choose the order of exposition that makes the argument strongest for that topic.
- Keep overlap with neighboring chapters low. If a topic is central elsewhere, reference it briefly and move on.
- End with clear decisions or heuristics the reader can apply.

## Code Sample Rules

- Prefer realistic domains: service backends, libraries, concurrent pipelines, memory-sensitive components, tooling, or API boundaries.
- Favor samples that look like code someone would actually review in a production repository.
- Every substantial sample must be one of two things:
  - `Compilable` — complete enough to build as shown or with obvious surrounding boilerplate kept outside the excerpt.
  - `Intentional partial` — incomplete on purpose, labeled as such, with the omitted context explained.
- Samples must demonstrate a design choice, tradeoff, or failure mode. Do not include code that exists only to restate syntax.
- Keep examples small enough to read in one pass, but large enough to show real constraints.
- Prefer C++23 library and language features when they improve clarity, safety, or maintainability.
- Avoid outdated idioms unless the point is to explain migration, interop, or a failure pattern.
- When presenting an anti-pattern, prefix the example title or heading with `Anti-pattern:` and mark the specific failure in the code with a `// BUG:` or `// RISK:` comment at the relevant line. Make the failure class explicit: lifetime bug, ownership confusion, exception boundary leak, data race risk, ABI fragility, hidden allocation, and so on.
- Use comments sparingly. Comments should explain why a design choice matters, not narrate the code line by line.

### Reference Compilers

Examples target C++23. For reproducibility, the reference toolchains are:

- GCC 14+
- Clang 18+
- MSVC 17.10+ (Visual Studio 2022)

If a sample relies on a feature not yet available in one of these compilers, note the restriction next to the example.

## Diagram Rules

- Use diagrams only when structure matters more than prose.
- Good diagram targets:
  - ownership and lifetime relationships
  - execution or concurrency flow
  - dependency or API boundaries
  - memory layout or data movement
- Do not add decorative figures, generic architecture boxes, or visuals that merely restate the surrounding text.
- Keep labels technical and minimal.
- If a diagram does not help the reader reason about a bug, cost, boundary, or invariant, cut it.

## C++ Version Policy

- Primary baseline: C++23.
- Write the main body so it remains correct and useful for readers working in present-day production codebases.
- Include C++26 material only when it materially changes a recommendation, simplifies an important pattern, or removes a meaningful drawback.
- Mark future-facing material explicitly as a note, sidebar, or end-of-chapter section. Do not let forward-looking content dominate the main flow.
- Do not pad chapters with tentative standard-evolution commentary.

## Draft Review Checklist

Before considering a chapter ready for review, check the following:

- The chapter targets experienced programmers rather than beginners.
- The argument is organized around a production problem and a decision, not a feature inventory.
- The visible chapter structure fits the topic instead of mechanically mirroring a template.
- Claims about style or design are justified with constraints and tradeoffs.
- Code samples are either buildable or clearly labeled as intentional partials.
- Anti-pattern examples are clearly labeled and the failure mode is marked in the code.
- Any C++26 material changes a real design decision and is clearly marked.
- The prerequisites section accurately reflects which earlier chapters are assumed.
- The prose is tight enough that removing a paragraph would lose technical meaning, not just tone.
