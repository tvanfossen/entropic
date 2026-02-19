---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260219-019
title: "Engine Decomposition — Extract Cohesive Subsystems"
priority: P2
component: core
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-19
updated: 2026-02-19
tags: [refactoring, engine, separation-of-concerns, architecture]
completed_date: null
scoped_files:
  - src/entropi/core/engine.py
  - src/entropi/core/
  - tests/unit/test_engine*.py
depends_on: []
blocks: []
---

# Engine Decomposition — Extract Cohesive Subsystems

## Problem Statement

`engine.py` is 1,459 lines with 55 methods spanning multiple responsibilities:
agentic loop control, tool execution + approval, context/memory management,
directive processing, streaming, and tier selection. This makes the engine
difficult to understand, test in isolation, and extend for library consumers
who may want to customize one subsystem without understanding the whole.

Identified during adversarial analysis of P1-011 library extraction.

## Candidate Subsystems

| Subsystem | Methods (~) | Responsibility |
|-----------|-------------|----------------|
| **ToolExecutor** | 15 | Execute tool calls, check approval, handle permissions, duplicate detection |
| **ContextManager** | 8 | Prune tool results, inject context warnings, compaction, context anchor re-injection |
| **DirectiveProcessor** | 8 | Register + dispatch directive handlers, context anchor lifecycle |
| **ResponseGenerator** | 5 | Streaming/non-streaming generation, tier locking, tool filtering |
| **Engine (core loop)** | ~19 | Orchestration: run, _loop, _execute_iteration, state management, pause/resume/interrupt |

## Constraints

- **Library consumers depend on `Engine` as the entry point** — public API must not break
- **EngineCallbacks contract** — extracted subsystems must still funnel events through callbacks
- **Single LoopContext** — shared mutable state flows through the loop; subsystems receive it by reference
- **No premature abstraction** — extract concrete classes, not interfaces. ABC only if two implementations exist.

## Open Questions

- Should extracted classes be Engine inner classes or top-level in `core/`?
- Does ToolExecutor own the ServerManager reference, or does Engine pass it per-call?
- How do library consumers override subsystem behavior (subclass Engine, or inject custom subsystem)?

## Risk

Medium. Engine is the heart of the system. Incorrect extraction could introduce
subtle ordering bugs in the agentic loop. Strong test coverage needed before
and after.

## Recommendation

P2 priority — not blocking library extraction merge, but should be addressed
before the API stabilizes for 1.0. The current engine works; this is about
long-term maintainability.
