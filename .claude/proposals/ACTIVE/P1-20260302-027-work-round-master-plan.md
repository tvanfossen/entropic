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
│       Engine is 1,552 lines / 69 methods. Decompose before B adds more.
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
  - [x] Engine decomposed: 4 subsystem files extracted, engine.py 1,552 → 680 lines — merged develop @ v1.2.0
  - [x] VRAM orchestration: three-state lifecycle implemented — merged develop @ v1.3.0
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

### 2026-03-02 — Pre-phase baseline (v1.1.0)
- Instance #2 committed Qwen3.5 adapter, `enable_thinking` pipeline,
  `inject_model_context` config, `entropic setup-cuda` CLI
- Engine grew: 1,528 → 1,552 lines, 68 → 69 methods (+_inject_model_context)
- Version bumped to 1.1.0
- Config fields already landed: `TierConfig.enable_thinking`, `LibraryConfig.inject_model_context`
- All proposals, examples, and test updates committed to develop
- Clean working tree — ready for Phase 1

### 2026-03-02 — Track 1: Engine Decomposition (P2-019)
- [x] Phase 1a: Extracted `engine_types.py` (143 lines) — dataclasses/enums
  - Commit: `b961a84`
  - Engine: 1,552 → 1,441 lines (re-exports preserved)
- [x] Phase 1b: Extracted `tool_executor.py` (442 lines) — 20 tool methods
  - Engine: 1,441 → 1,089 lines (-352 lines)
  - Refactored callbacks: individual attrs → shared `EngineCallbacks` container
  - `set_callbacks()` now updates fields in place (subsystems see changes)
  - ToolExecutor created lazily via `_ensure_tool_executor()` (server_manager may be None at init)
  - Engine hooks: `after_tool_hook` (compaction+warning), `directive_hook` (directive processing)
  - Updated 3 test files (test_engine, test_engine_auto_chain, test_engine_feedback_roles)
  - 647 unit + model tests pass
  - **Files changed:** engine.py, tool_executor.py (new), test_engine.py, test_engine_auto_chain.py, test_engine_feedback_roles.py
- [x] Phase 1c: Extracted `response_generator.py` (402 lines) — 13 response/tier methods
  - Engine: 1,089 → 805 lines (-284 lines)
  - ResponseGenerator constructor: (orchestrator, config, loop_config, callbacks, interrupt_event, pause_event)
  - `_directive_tier_change` delegates to `self._ensure_response_generator()` for prompt/logging methods
  - `create_assistant_message` promoted to static method
  - Updated 2 test files (test_engine.py, test_model_logger.py)
  - 647 unit + model tests pass
  - **Files changed:** engine.py, response_generator.py (new), test_engine.py, test_model_logger.py
- [x] Phase 1d: Extracted `context_manager.py` (201 lines) — 5 context methods
  - Commit: `d23af59`
  - Engine: 805 → 716 lines (-89 lines)
  - ContextManagerHooks: `after_compaction` hook → engine._reinject_context_anchors
  - `_ensure_context_manager()` lazy factory (all deps available at init)
  - ContextManager accesses token counter via compaction_manager.counter (same object)
  - Updated 1 test file (test_engine.py: TestAutoPruneToolResults, TestContextWarningInjection create CM directly; TestPruneDirective pre-sets engine._context_manager)
  - 647 unit + 26 model tests pass
  - **Files changed:** engine.py, context_manager.py (new), test_engine.py
  - **Note:** engine.py at 716 lines. Line count target (< 400/500) was over-optimistic — did not account for 9 new lazy-factory/delegation methods (~90 lines) or docstring overhead. User accepted this; the structural goal (all impl logic in subsystems) is met.
- [x] Phase 1e: Remove lazy factories + thin delegations (adversarial analysis 2026-03-03)
  - Commit: `6609ae2`
  - Engine: 716 → ~620 lines (-8 methods)
  - Removed `_ensure_response_generator()`, `_ensure_context_manager()` (cargo-cult lazy)
  - Removed all 6 thin delegation methods
  - Eager construction of RG + CM in `__init__` (non-nullable types); ToolExecutor stays lazy
  - Replaced `assert self.server_manager is not None` with explicit `RuntimeError`
  - Updated 3 test files: `_make_engine()`, `TestOverflowRecovery`, `test_engine_feedback_roles`
  - 647 unit + 26 model tests pass
  - **Files changed:** engine.py, test_engine.py, test_engine_feedback_roles.py
- [x] Phase 2: Integration verification, version bump 1.2.0, merge to develop
  - Commit: `1bd3834` — merged develop @ v1.2.0

### 2026-03-03 — Track 2: VRAM Orchestration (P1-022)
- [x] Phase 1a: `ModelState` enum + `ModelBackend` abstract interface
  - Added `ModelState` (COLD/WARM/ACTIVE) enum to `core/base.py`
  - Added abstract `state`, `warm()`, `activate()`, `deactivate()` to `ModelBackend`
  - Made `is_loaded` concrete (returns `state == ACTIVE`)
  - Exported `ModelState` from `entropic.core` public API
- [x] Phase 1b: `LlamaCppBackend` three-state implementation
  - `warm()`: COLD → WARM (n_gpu_layers=0, mmap+mlock)
  - `activate()`: WARM/COLD → ACTIVE (GPU layers)
  - `deactivate()`: ACTIVE → WARM (GPU released, CPU pages retained)
  - `_swap_model()`: gc + reload helper
  - `_load_model_sync()` accepts `gpu_layers` param, uses `config.use_mlock`
  - `_detect_template_support()` extracted from old `load()`
- [x] Phase 1c: Config schema new fields
  - `ModelConfig.warm_on_startup: bool = False`
  - `ModelConfig.use_mlock: bool = True`
  - `LibraryConfig.vram_reserve_mb: int = 512`
- [x] Phase 1d: Orchestrator integration
  - `_deactivate_current_if_needed()` replaces `_unload_current_if_needed()`
  - `_warm_on_startup_tiers()` helper for initialize()
  - `set_thinking_mode()` uses `deactivate()` not `unload()`
- [x] Phase 1e: Tests updated
  - `test_orchestrator_loading.py`: `MockModelBackend` three-state, `MockTierConfig` new fields, swap assertion → `_deactivate_called`
  - `test_vram_state_machine.py` (new): 8 tests for swap/warm_on_startup/thinking
  - `test_llama_cpp_grammar.py`: `_make_backend()` sets `_state=ACTIVE`
  - `test_library_api.py`: `MockTierConfig` new fields
  - 654 unit + 26 model tests pass
  - **Files changed:** core/base.py, core/__init__.py, inference/llama_cpp.py, inference/orchestrator.py, config/schema.py, 4 test files, 1 new test file
- [x] Phase 2: Merge to develop @ v1.3.0
  - Commit: `e3bb08f` — merged develop @ v1.3.0

### 2026-03-03 — Track 3: Benchmark Layer 1 (P1-029)
- [x] Package structure: `src/entropic/benchmark/` (7 files: __init__, types, gpu, runner, layer1, report, cli)
- [x] `types.py`: ModelSpec, LoadResult, InferenceResult, SwapResult, SweepPoint, Layer1Results
- [x] `gpu.py`: get_vram_mb(), get_gpu_info(), free_model() (GC + VRAM polling), get_block_count()
- [x] `runner.py`: BenchmarkRunner — timed_cold_load, timed_swap, timed_inference, gpu_sweep
- [x] `layer1.py`: run_layer1() + save_results(); SWAP_TARGET_MS = 3000ms
- [x] `report.py`: Rich tables + plain-text fallback, results_to_json()
- [x] `cli.py`: `entropic benchmark run --layer1-only` + `entropic benchmark sweep`
- [x] Unit tests: test_benchmark_gpu.py, test_benchmark_runner.py
- [x] Pre-commit: all hooks pass (668 unit + 26 model tests pass)
- [x] Merge to develop @ v1.3.1 (patch bump — diagnostic tool, not engine API)
  - Commit: `8d9b75d` (bump), `827997b` (merge)
- **Files changed:** src/entropic/benchmark/ (7 new), src/entropic/cli.py, tests/unit/ (2 new)
