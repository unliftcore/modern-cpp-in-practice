# Modern C++ Book Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Start a serious modern C++ book project for experienced programmers by establishing the manuscript structure, editorial contract, and first anchor chapters.

**Architecture:** The manuscript is organized as a topic-first handbook in Markdown. The repository separates authoring guidance, chapter content, appendices, and compilable examples so drafting can proceed incrementally without losing editorial consistency.

**Tech Stack:** Markdown, plain text, C++23-focused examples, future C++26 notes.

---

### Task 1: Create Manuscript Scaffold

**Files:**
- Create: `README.md`
- Create: `style-guide.md`
- Create: `chapters/.gitkeep`
- Create: `examples/.gitkeep`
- Create: `appendices/.gitkeep`

**Step 1: Create the directory structure**

Create the top-level folders for chapters, examples, appendices, and plans.

**Step 2: Write the project README**

Document the book premise, target audience, scope, and repository layout.

**Step 3: Write the style guide**

Define tone, chapter template, code sample rules, and C++ version policy.

**Step 4: Review for consistency**

Check that the README and style guide agree on audience, scope, and manuscript workflow.

### Task 2: Draft Front Matter And Table Of Contents

**Files:**
- Create: `chapters/00-preface.md`
- Create: `chapters/01-map.md`

**Step 1: Draft the preface**

Write the book promise, reader assumptions, and what the book intentionally does not cover.

**Step 2: Draft the book map**

Write the part structure and chapter list with one-paragraph descriptions for each chapter.

**Step 3: Review sequencing**

Verify that the chapter order progresses from foundations that matter in production toward scale, performance, tooling, and maintenance.

### Task 3: Draft The First Anchor Chapter

**Files:**
- Create: `chapters/02-ownership-lifetime-and-resource-management.md`

**Step 1: Draft the chapter**

Write a substantive chapter on ownership, lifetime, RAII, move semantics, handles, smart pointers, and resource boundaries in production code.

**Step 2: Include chapter structure**

Use the project chapter template: production problem, naive approach, modern solution, tradeoffs, testing/tooling implications, and review checklist.

**Step 3: Review technical depth**

Check that the chapter reads as senior-level guidance rather than tutorial material.

### Task 4: Draft The Second Anchor Chapter

**Files:**
- Create: `chapters/03-errors-results-and-failures.md`

**Step 1: Draft the chapter**

Write a chapter on exceptions, error codes, `std::expected`, failure domains, API boundaries, and production observability.

**Step 2: Integrate with the rest of the book**

Ensure the chapter prepares readers for later sections on concurrency, APIs, and tooling.

**Step 3: Review overlap**

Remove repetition with the ownership chapter and keep the center of gravity on decision-making.

### Task 5: Editorial Review Pass

**Files:**
- Modify: `README.md`
- Modify: `style-guide.md`
- Modify: `chapters/00-preface.md`
- Modify: `chapters/01-map.md`
- Modify: `chapters/02-ownership-lifetime-and-resource-management.md`
- Modify: `chapters/03-errors-results-and-failures.md`

**Step 1: Review spec compliance**

Check the files against the agreed design: experienced audience, topic-first structure, real-world framing, C++23 primary, selective C++26 mentions.

**Step 2: Review prose quality**

Tighten repetition, remove AI-sounding transitions, and sharpen claims so they are grounded in engineering tradeoffs.

**Step 3: Review structural quality**

Check that the first chapters establish the book’s technical level and make the rest of the manuscript easier to continue.