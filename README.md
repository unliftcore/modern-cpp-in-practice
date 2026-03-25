# Modern C++23 in Practice

This repository is the manuscript workspace for a serious book on production modern C++23. The book is a hands-on engineering guide for experienced programmers who want to write modern C++ well from the start: clear ownership, stable interfaces, explicit failure boundaries, structured concurrency, measurable performance, and strong verification.

## Purpose

The project exists to produce a technically rigorous manuscript that teaches how modern C++ is applied in real systems. It is not a classroom introduction to the language and not a migration manual for legacy codebases. Chapters should teach design pressure, tradeoffs, failure modes, and operational consequences. Syntax matters only when it changes engineering choices.

## Intended Reader

The reader is already an experienced programmer. The manuscript does not assume a specific language background, only that the reader already knows how serious software gets designed, debugged, tested, deployed, and maintained. They may be new to C++, but they are not new to engineering judgment.

## Non-Goals

- No beginner onboarding, setup walkthroughs, or IDE tours.
- No control-flow or syntax tutorial progression.
- No "hello world" or toy examples used only to demonstrate language mechanics.
- No feature catalog organized around standard library headers.
- No migration-first framing where the default reader is a long-time C++ maintainer.
- No speculative C++26 coverage unless it changes present design decisions.

## Repository Layout

- `chapters/` — manuscript source, including the mdBook appendix pages.
- `examples/` — compilable or intentionally partial code samples tied to chapters.
- `appendices/` — compact reference material, checklists, and support content.
- `docs/plans/` — design and implementation plans for the book project.
- `style-guide.md` — editorial contract for prose, code samples, and version policy.

## Manuscript Workflow

Work in small editorial units:

1. Define or refine the chapter brief: production problem, scope boundary, sample domains, and the decisions the reader should leave with.
2. Draft the chapter using the repository style guide.
3. Review for technical rigor, overlap with adjacent chapters, and consistency with the book's audience.
4. Verify every substantial sample: compile and run it, or label it clearly as partial and explain why.
5. Tighten the prose so the chapter teaches choices and constraints rather than reciting mechanisms.

The expected sequence is outline -> chapter brief -> draft -> technical review -> sample verification -> language edit.

## Chapter Expectations

Every chapter should answer a production question. Start from a realistic engineering problem, show the naive or common approach that fails under pressure, then present the modern C++ approach with explicit tradeoffs. A strong chapter is decision-oriented: it explains where a technique helps, where it hurts, how it interacts with tooling and testing, and what reviewers should check in real code.

Use realistic sample domains: service backends, libraries, data pipelines, native tooling, memory-sensitive subsystems, concurrency-heavy components, or boundary layers. Do not build the manuscript around pedagogical toy programs.

Chapters target C++23 by default. C++26 notes belong in tightly scoped callouts or closing sections, and only when they materially alter recommended design or implementation choices.