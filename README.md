# Modern C++

This repository is the manuscript workspace for a serious book on production modern C++. The book is a topic-first handbook for experienced programmers who need to make sound engineering decisions in C++23 codebases: ownership, interfaces, failure handling, concurrency, performance, tooling, and long-term maintainability.

## Purpose

The project exists to produce a technically rigorous manuscript that explains how modern C++ is used in real systems rather than how the language is introduced in a classroom. Chapters should teach design pressure, tradeoffs, and failure modes. Syntax matters only when it changes engineering choices.

## Intended Reader

The reader is already an experienced programmer. They may come from C, Rust, Java, Go, C#, Python, or another systems-adjacent environment, but they are new to writing and reviewing production modern C++. The manuscript assumes they can already reason about APIs, memory, concurrency, testing, and deployment constraints.

## Non-Goals

- No beginner onboarding, setup walkthroughs, or IDE tours.
- No "hello world" progression through the language.
- No generic object-oriented programming refreshers.
- No feature catalog organized around standard library headers.
- No speculative C++26 coverage unless it changes present design decisions.

## Repository Layout

- `chapters/` — manuscript source, one chapter per file.
- `examples/` — compilable or intentionally partial code samples tied to chapters.
- `appendices/` — compact reference material, checklists, and support content.
- `docs/plans/` — design and implementation plans for the book project.
- `style-guide.md` — editorial contract for prose, code samples, diagrams, and version policy.

## Manuscript Workflow

Work in small editorial units:

1. Define or refine the chapter brief: production problem, scope boundary, and decisions the reader should leave with.
2. Draft the chapter using the repository style guide.
3. Review for technical rigor, overlap with adjacent chapters, and consistency with the book's audience.
4. Verify every substantial sample: compile and run it, or label it clearly as partial and explain why.
5. Tighten the prose so the chapter teaches choices and constraints rather than reciting mechanisms.

The expected sequence is outline -> chapter brief -> draft -> technical review -> sample verification -> language edit.

## Chapter Expectations

Every chapter should answer a production question. Start from a realistic problem, show the naive or legacy approach that fails under pressure, then present the modern C++ approach with explicit tradeoffs. A strong chapter is decision-oriented: it explains where a technique helps, where it hurts, how it interacts with tooling and testing, and what reviewers should check in real code.

Chapters are expected to target C++23 by default. C++26 notes belong in tightly scoped callouts or closing sections, and only when they materially alter recommended design or implementation choices.