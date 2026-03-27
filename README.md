# Modern C++ in Practice

[English](README.md) | [中文](README.zh-CN.md)

I started this project because I wanted to learn C++ properly, and I couldn't find a course that did it right. Everything out there is either stuck on C++11/14 patterns, aimed at beginners who've never seen a pointer, or organized as a feature catalog of standard library headers. Nothing actually teaches you how to write modern C++23 the way working engineers need to.

So I built one myself, with help from Claude Opus and GPT-5.4. The AI did a lot of the drafting and iteration. I reviewed, corrected, restructured, and stress-tested the material. The result is this book project: a hands-on guide to production C++23 for people who already know how to write software but are picking up C++ (or picking it back up after years away).

## What this book covers

Ownership, lifetime, RAII. Value semantics and invariants. Error handling at failure boundaries. Concurrency that doesn't fall apart under load. Performance you can actually measure.

Each chapter starts from a real engineering problem, shows the common approach that breaks, then walks through the modern C++ way with tradeoffs stated plainly. If a technique has sharp edges, the chapter says so.

## What this book doesn't cover

This is not a beginner intro. No "hello world", no IDE setup guides, no syntax tutorials. It also isn't a migration manual for people maintaining 20-year-old codebases.

The target language standard is C++23. C++26 comes up only when it changes a recommendation you'd act on today.

## Who it's for

You already know how serious software gets built, tested, and shipped. You might be new to C++, but you're not new to engineering.

## Repository layout

- `chapters/` contains the manuscript source (mdBook format).
- `examples/` has compilable code samples tied to specific chapters.
- `style-guide.md` is the editorial contract for prose and code conventions.

## How chapters get written

1. Write a chapter brief: what production problem does this chapter answer, what's in scope, what domains do the examples use.
2. Draft against the style guide.
3. Review for technical accuracy, overlap with adjacent chapters, and audience fit.
4. Compile and run every substantial code sample (or mark it partial and explain why).
5. Edit until the chapter teaches decisions, not just mechanisms.

Sample domains include service backends, libraries, data pipelines, concurrency-heavy components, and memory-sensitive subsystems. No toy programs built only to demonstrate syntax.