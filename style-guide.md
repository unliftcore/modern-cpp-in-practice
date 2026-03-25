# Style Guide

This guide defines the editorial contract for the manuscript. If a draft conflicts with this file, fix the draft.

## Prose Style

- Write for experienced programmers. Assume they understand software engineering, but not the sharp edges of modern C++ in production.
- Use direct, technical prose. Prefer specific claims over reassurance, slogans, or generic "best practice" language.
- Explain constraints and tradeoffs whenever recommending an approach.
- Introduce language features only in service of a design problem. Do not write syntax tours.
- Avoid filler, motivational framing, and tutorial scaffolding.
- Prefer short, declarative paragraphs. Long explanations must earn their length by carrying technical detail.
- Use defined terms consistently. If a chapter distinguishes ownership, lifetime, identity, value semantics, or failure domains, keep those terms stable.

## Chapter Template

Use this structure unless a chapter has a strong reason to deviate:

1. `Production problem` — the real engineering pressure or failure mode.
2. `Naive or legacy approach` — what teams often do first and why it breaks down.
3. `Modern C++ approach` — the main design and language tools that address the problem.
4. `Tradeoffs and boundaries` — costs, edge cases, operational risks, and when not to use the approach.
5. `Testing and tooling implications` — what sanitizers, static analysis, benchmarks, fuzzing, logging, or build checks matter.
6. `Review checklist` — concise items a reviewer can apply to real code.

Operational rules:

- Each chapter must answer a concrete production question, not merely cover a language feature.
- Keep overlap with neighboring chapters low. If a topic is central elsewhere, reference it briefly and move on.
- End with clear decisions or heuristics the reader can apply.

## Code Sample Rules

- Prefer realistic domains: service backends, libraries, concurrent pipelines, memory-sensitive components, tooling, or API boundaries.
- Every substantial sample must be one of two things:
  - `Compilable` — complete enough to build as shown or with obvious surrounding boilerplate kept outside the excerpt.
  - `Intentional partial` — incomplete on purpose, labeled as such, with the omitted context explained.
- Samples must demonstrate a design choice, tradeoff, or failure mode. Do not include code that exists only to restate syntax.
- Keep examples small enough to read in one pass, but large enough to show real constraints.
- Prefer C++23 library and language features when they improve clarity, safety, or maintainability.
- Avoid outdated idioms unless the point is to explain migration, interop, or a failure pattern.
- When presenting a "bad" example, make the failure explicit: lifetime bug, ownership confusion, exception boundary leak, data race risk, ABI fragility, hidden allocation, and so on.
- Use comments sparingly. Comments should explain why a design choice matters, not narrate the code line by line.

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
- Claims about style or design are justified with constraints and tradeoffs.
- Code samples are either buildable or clearly labeled as intentional partials.
- Any C++26 material changes a real design decision and is clearly marked.
- The prose is tight enough that removing a paragraph would lose technical meaning, not just tone.