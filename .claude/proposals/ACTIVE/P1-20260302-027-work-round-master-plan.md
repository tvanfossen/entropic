---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260302-027
title: "Work Round Master Plan: Identity Library + Infrastructure"
priority: P1
component: all
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-03-02
updated: 2026-03-02
tags: [master-plan, identities, vram, engine, mcp, tui, benchmark]
completed_date: null
scoped_files: []
depends_on: []
blocks: []
---

# Work Round Master Plan: Identity Library + Infrastructure

## Goal

Make `entropic-engine` the framework that turns unreliable local models
into reliable cognitive pipelines.

A consumer provides a GGUF file and gets:

1. **Narrow identities** that decompose broad tasks into focused steps a
   local model can actually do (P1-024)
2. **Grammar enforcement** that makes structured output physically
   guaranteed, not hoped for (P1-024)
3. **Fast model swaps** that make multi-stage pipelines practical, not
   theoretical (P1-022)
4. **Measurable quality** — benchmark any model against any identity,
   compare, decide (P1-029)
5. **Tunable resource usage** that works on a dedicated server or a shared
   laptop (P2-020, P2-021)
6. **Runtime extensibility** — connect external tool servers without
   restart (P2-026)
7. **A clean engine** where each subsystem is independently testable and
   overridable (P2-019)

The TUI (P1-028) proves all of this works together as a complete
application.

The benchmark (P1-029) replaces model tests as the quality gate: a model
that fails benchmarks is a candidate for replacement, not a test failure
to debug. The engine itself is validated by unit tests. Model behavior is
validated by benchmarks.

## Active Proposals

| Group | ID(s) | Title | Dependency |
|-------|--------|-------|------------|
| **A** | P1-022 | VRAM orchestration + warm loading | None |
| **B** | P1-024 | Bundled identity library | A, C |
| **C** | P2-019 | Engine decomposition | None |
| **D** | P2-026 | Runtime MCP registration + bridge | None |
| **E** | P2-020 + P2-021 | GPU resource mgmt + time-based cap | A |
| **F** | P1-029 | entropic-benchmark | Layer 1: None. Layer 2: B |
| **G** | P1-028 | TUI overhaul: full reference implementation | All |

## Execution Order

```
Phase 1: Foundation (parallel tracks)
├── Track 1: C (engine decomposition)
│   └── Extract ToolExecutor, ContextManager, ResponseGenerator
│       Engine is 1,528 lines / 68 methods. Decompose before B adds more.
│
├── Track 2: A (VRAM orchestration)
│   └── Three-state lifecycle (cold/warm/active), VRAM budget
│       Phase 0 (mlock) already applied.
│
├── Track 3: D (runtime MCP registration)
│   └── Independent. stdio-native + bridge. No inference overlap.
│
└── Track 4: F Phase 1 (benchmark Layer 1)
    └── Raw model perf: load times, tok/s, GPU sweep, swap latency
        Validates Track 2 (VRAM orchestration) as it's built.
        Produces model perf data for identity assignments in Phase 2.

Phase 1 complete when:
  - [ ] Engine decomposed: 3 subsystem files extracted, engine.py < 400 lines
  - [ ] VRAM orchestration: warm→active swap measured and < 3s for 21GB model
  - [ ] MCP runtime: connect/disconnect lifecycle works, bridge relays JSON-RPC
  - [ ] Benchmark Layer 1: `entropic benchmark run --layer1-only` produces results
  - [ ] All 4 tracks merged to develop with version bumps
  - [ ] docs/ updated for each track's new features

Phase 2: Core Feature
└── B (identity library)
    ├── 13 identities: prompts + grammars + allowed_tools
    ├── Router expansion (7 categories)
    ├── Pipeline integration (auto_chain defaults from frontmatter)
    ├── Interstitial pruner (experimental)
    └── Uses Layer 1 benchmark data to validate model → identity assignments

Phase 2 complete when:
  - [ ] All 13 identities have prompt + grammar + frontmatter
  - [ ] Router classifies 7 categories at ≥80% accuracy
  - [ ] 3+ pipeline compositions work end-to-end
  - [ ] Merged to develop with version bump
  - [ ] docs/ updated: identity library, pipeline composition, config overrides

Phase 2.5: Benchmark Layer 2
└── F Phases 2-4 (benchmark identity evaluation)
    ├── Identity benchmark.yaml discovery + evaluation
    ├── Shared check primitives (also used by tests/model/)
    ├── Comparison tool + historical records
    └── Depends on: B (identities must exist)

Phase 2.5 complete when:
  - [ ] `entropic benchmark run` evaluates model against all identities
  - [ ] `entropic benchmark compare` produces per-identity leaderboard
  - [ ] Shared check primitives imported by tests/model/
  - [ ] Merged to develop with version bump

Phase 3: Tuning
└── E (GPU resource management + time-based cap)
    ├── Compute caps, resource profiles (n_batch, n_threads)
    ├── Wall-clock generation limits (ThroughputTracker)
    └── Depends on: A (VRAM budget integration)

Phase 3 complete when:
  - [ ] Resource profiles (maximum/balanced/background/minimal) work
  - [ ] Time-based generation cap adjusts token budget from tok/s
  - [ ] Merged to develop with version bump
  - [ ] docs/ updated: resource profiles, time caps

Phase 4: Capstone
└── G (TUI overhaul)
    ├── Full reference implementation
    ├── Pipeline visualization + identity attribution
    ├── VRAM dashboard, model state indicators
    ├── Runtime MCP server management UI
    ├── Resource profile switching, tok/s readout
    └── Depends on: everything above

Phase 4 complete when:
  - [ ] TUI exercises every bundled identity in at least one workflow
  - [ ] Pipeline strip, resource panel, MCP server list functional
  - [ ] Merged to develop with version bump
  - [ ] docs/ updated: TUI features, keybindings
```

## Deferred (IDEAS)

| ID | Title | Why deferred |
|----|-------|-------------|
| P3-023 | MoE expert-level VRAM caching | Needs P1-022 + feasibility study (3-6 month effort) |
| P1-012 | Per-tier model fine-tuning | Needs identity library to define training targets |
| P2-005 | Tool abandonment detection | Engine-behavioral, not architectural. After B. |
| P2-010 | LSP diagnostics in filesystem | Engine-behavioral. After B. |

## Cross-Cutting Concerns

### Config Schema Evolution

Multiple proposals add config fields. Track all additions:

| Proposal | New config fields |
|----------|-------------------|
| A (P1-022) | `warm_on_startup`, `gpu_layers` dynamic mode, `vram_reserve_mb` |
| B (P1-024) | Identity frontmatter defaults, `interstitial` flag, `default_model`, pipeline config |
| D (P2-026) | `mcp.runtime_servers` (optional persistence) |
| E (P2-020) | `n_batch`, `n_threads`, `resource_profile` |
| E (P2-021) | `max_generation_time_ms` per tier |

All must validate at load time. No silent degradation on invalid config.

### Test Strategy

The engine is validated by unit tests. Model behavior is validated by
benchmarks. The existing `tests/model/` tests will be revisited as
benchmark Layer 2 matures — shared check primitives unify what "correct"
means across both.

Each proposal must deliver:
- Unit tests for all new code paths
- Integration tests for cross-component interactions
- Benchmark definitions (Layer 2) for any new identities

### Documentation Checklist

Documentation is maintained throughout — not deferred to the end.
Each proposal updates these files as part of its merge:

| Proposal | library-consumer-guide.md | configuration.md | README.md |
|----------|--------------------------|-------------------|-----------|
| A (P1-022) | Warm/active states, model lifecycle | `warm_on_startup`, `vram_reserve_mb` | VRAM management bullet |
| B (P1-024) | Identity library, pipelines, config overrides | Full tier config with all identity fields | Identity system bullet |
| C (P2-019) | Constructor injection, custom subsystems | — | — |
| D (P2-026) | `connect_server()` API, bridge usage | `mcp.runtime_servers` | Runtime MCP bullet |
| E (P2-020) | Resource profiles, compute tuning | `resource_profile`, `n_batch`, `n_threads` | GPU tuning bullet |
| E (P2-021) | Time caps, grammar interaction | `max_generation_time_ms` | — |
| F (P1-029) | Benchmark CLI, identity benchmark.yaml | — | Benchmark bullet |
| G (P1-028) | TUI features, commands | — | TUI overhaul bullet |

### Git Commit Structure

Clean, atomic commits per logical unit of work:
- One feature branch per proposal (per CLAUDE.md branching strategy)
- Commits within a branch are logical units, not checkpoint saves
- Merge to develop on completion with version bump
- Commit messages reference proposal IDs
- Examples created or modified as needed to demonstrate features

### Version Bumps

One version bump per merge to develop (per CLAUDE.md). This work round
spans multiple merges — each is a minor bump since they add features and
config fields.

## Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| Engine decomposition breaks model tests | High | Run full pre-commit after each extraction phase. Subsystem boundaries are well-defined — methods move, signatures don't change. |
| Warm→active swap still too slow (>3s) | Medium | Benchmark Layer 1 measures this. If mlock + reconstruct is insufficient, investigate llama-cpp-python internals for GPU layer migration. Documented in P1-022 risks. |
| Identity library bloats config | Medium | Zero-config defaults from frontmatter. Consumer sets ONE model path, everything works. Config is overrides only. |
| Bridge subprocess management complexity | Low | stdio is trivial (Popen + pipes). Bridge is independently testable. Health check via pipe EOF + periodic ping. |
| Grammar maintenance burden (10+ .gbnf files) | Medium | Each grammar is small (structural skeleton). Benchmark Layer 2 catches grammar/output mismatches automatically. |

## Superseded Proposals

| ID | Status | Reason |
|----|--------|--------|
| P2-025 | COMPLETE | Phase 0 (mlock) applied. Phases 1-2 superseded by P1-022. |

## Implementation Log

Log format: date, proposal ID, action taken, files changed.

### 2026-03-02 — Work round setup
- Created master plan P1-20260302-027
- Created TUI overhaul P1-20260302-028
- Reviewed and updated all active proposals:
  - P1-022: merged P2-025, added Phase 0 (mlock done), identity library interaction
  - P1-024: grammar sketches, allowed_tools, consumer config, router expansion,
    interstitial pruner, tightened acceptance criteria
  - P2-019: full rewrite — 68-method inventory, extraction plan, constructor injection
  - P2-026: bridge architecture, identity interaction, health checking, ServerInfo
  - P2-020: rewritten — 3 phases, config schema, resource profiles
  - P2-021: rewritten — ThroughputTracker, grammar interaction, pipeline budgets
- P1-029 (benchmark): added swap latency to Layer 1
- Moved P2-025 to COMPLETE (superseded by P1-022)
- Moved P1-011, P1-014 to COMPLETE (already done)
- Moved P3-023 to IDEAS (deferred)
- All active proposals reference `src/entropic/` (old `src/entropi/` paths fixed)
