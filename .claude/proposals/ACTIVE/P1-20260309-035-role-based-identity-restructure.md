---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260309-035
title: "Role-Based Identity Restructure"
priority: P1
component: identities
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-03-09
updated: 2026-03-09
tags: [identities, roles, architecture, constitutional-ai]
completed_date: null
scoped_files:
  - "src/entropic/prompts/**"
  - "src/entropic/data/prompts/**"
  - "src/entropic/data/grammars/**"
  - "src/entropic/config/schema.py"
  - "src/entropic/core/engine_types.py"
  - "src/entropic/inference/orchestrator.py"
depends_on: []
blocks: [P2-20260309-034]
---

# Role-Based Identity Restructure

## Problem Statement

P1-024 shipped 15 task-oriented identities (code_writer, test_writer, searcher, etc.) that create excessive handoff friction, saturate the 0.6B router at 9 classes, and fragment tool permissions so that identities can't complete natural work units without delegating. TUI testing revealed:

- **Grammars block tool calls on front-office identities.** GBNF constraints physically prevent XML tool-call emission, making grammar-constrained tool-using identities a contradiction.
- **Models hallucinate tool names** when capabilities are missing from an identity's allowed set.
- **Self-handoff loops** occur when the taxonomy forces unnecessary delegation between micro-task identities.

The root cause: identities were decomposed by micro-task (what you're doing) instead of by role/perspective (who you are). This mirrors function-level decomposition applied to personas — the handoff overhead exceeds the isolation benefit.

## Design Principles

1. **Identities are personas, not procedures** — system prompt defines perspective/mindset, not step-by-step operations.
2. **Tools are the constraint, not grammar** — permissions bound action space. Grammar on tool-using identities is a contradiction.
3. **Grammars only on back office** — output consumed by engine, not humans. Structured output justified only when the engine parses it.
4. **Single model architecture** — all VRAM for one model + KV cache. No tier swaps needed.
5. **Router removed** — lead routes via handoff, frees ~600MB VRAM.
6. **State machinery embedded from day one** — ships with one phase per role. Adding phases later = adding states to existing machine, not retrofitting state into a stateless system.
7. **Procedure lives in system prompt as behavioral rules** — not output format constraints.

## The Team (7 Front Office + 2 Back Office)

### Front Office (user-facing, routable via lead)

#### lead — Technical Team Lead

- **Perspective:** Triages, delegates, reviews output, final gate before human.
- **Absorbs:** conversational, quick.
- **Tools:** All (read, search, bash) — needs to assess before routing.
- **Params:** temp 0.3, thinking ON, max_tokens 4096.
- **Key behavior:** Answers simple queries directly. Only delegates when specialized perspective is needed. Receives from human, delegates to any role, synthesizes results back.

#### arch — Architect

- **Perspective:** System design, decomposition, technical decisions, tradeoffs.
- **Absorbs:** planner.
- **Tools:** File read, search, bash (analysis). Limited write (design docs, not implementation).
- **Params:** temp 0.5, thinking ON (always), max_tokens 4096.
- **Key behavior:** Produces designs not code. Challenges engineering decisions. Reviews for architectural concerns.

#### eng — Engineer

- **Perspective:** Build, fix, document, implement — the full lifecycle.
- **Absorbs:** code_writer, test_writer, searcher, wireframer, extractor.
- **Tools:** Full toolset — file read/write, bash execute, search, git.
- **Params:** temp 0.15, thinking ON for complex logic, max_tokens 8192.
- **Key behavior:** The workhorse. Searches, writes, tests, documents in one flow. No handoff needed for a natural work unit.

#### qa — QA Engineer

- **Perspective:** Adversarial — break, test, diagnose, validate, secure.
- **Absorbs:** code_validator, design_validator, benchmark_judge, diagnoser.
- **Tools:** File read/write (test files), bash execute (run tests, linters), search.
- **Params:** temp 0.4, thinking ON, max_tokens 4096.
- **Key behavior:** Finds problems, not confirms correctness. Security review folded in.

#### ux — UX Designer

- **Perspective:** User flows, interaction patterns, accessibility, cognitive load.
- **New role** (partially from design_validator).
- **Tools:** File read, search, bash (accessibility tools).
- **Params:** temp 0.5, thinking ON, max_tokens 4096.

#### ui — UI Designer

- **Perspective:** Visual design, layout, component consistency, responsive.
- **New role** (partially from wireframer, design_validator).
- **Tools:** File read/write, search.
- **Params:** temp 0.5, thinking ON, max_tokens 4096.

#### analyst — Analyst

- **Perspective:** Research, investigate, summarize findings.
- **New role.**
- **Tools:** File read, search, web_search, web_fetch.
- **Params:** temp 0.4, thinking ON, max_tokens 4096.

### Back Office (engine-triggered, not user-facing)

#### compactor — Context Compactor

- Same model, different system prompt + GBNF grammar.
- Engine-triggered at context limits.
- Grammar: justified (output consumed by engine, must be structured).

#### scribe — Session Scribe

- Engine-triggered: after handoffs, after significant events, periodic intervals.
- Records: decisions made, delegation results, task completions.
- Output survives compaction, available to any role for context recovery.
- Grammar: justified (structured logs for engine consumption).

### Eliminated

| Identity | Reason |
|----------|--------|
| searcher | Capability, not role — all roles search. |
| quick | Parameter config on lead, not a separate perspective. |
| tool_runner | Capability, not role — all front-office roles call tools. |
| pruner | Unnecessary with single model architecture. |
| extractor | Folded into eng. |
| benchmark_judge | Folded into qa. |
| wireframer | Split across ui/ux. |
| design_validator | Split across ui/ux/qa. |

## State Machinery

Each role has a `phases` field that maps phase names to inference params. V1 ships with one phase ("default") per role. The architecture supports adding phases later (e.g., eng: understand -> implement -> fix) without retrofitting.

### PhaseConfig Dataclass

```python
@dataclass
class PhaseConfig:
    temperature: float = 0.7
    max_output_tokens: int = 4096
    enable_thinking: bool = False
    repeat_penalty: float = 1.1
    # Future: tool_permissions, context_priority
```

### IdentityFrontmatter Schema Addition

```python
class IdentityFrontmatter(PromptFrontmatter):
    # ... existing fields ...
    phases: dict[str, PhaseConfig] = {"default": PhaseConfig()}
    role_type: Literal["front_office", "back_office"] = "front_office"
```

### Identity Frontmatter Example (eng)

```yaml
phases:
  default:
    temperature: 0.15
    max_output_tokens: 8192
    enable_thinking: true
role_type: front_office
```

V1: every role has only `default` phase. The `phases` key exists in schema from day one.

## Router Changes

- Router disabled by default in config: `routing: { enabled: false }` (was `true`).
- Lead identity routes via handoff tool calls.
- Router remains as opt-in engine capability for consumers with many identities.
- Disabling router frees ~600MB VRAM (0.6B model no longer co-resident).

## Config Changes

- `models.tiers` simplified — single model entry + router (if enabled).
- Identity frontmatter gains `phases` field and `role_type` field.
- `routing.enabled` defaults to `false`.
- Existing `temperature`, `max_output_tokens`, `enable_thinking`, `repeat_penalty` fields remain on IdentityFrontmatter for backward compatibility. `phases.default` is the canonical location; top-level fields become sugar that populates phases.default if phases is absent.

## Delegation Flow

```
Human ──→ lead
           │
           ├──→ eng (build/fix/implement)
           │     └──→ returns results to lead
           │
           ├──→ arch (design/decompose)
           │     └──→ returns results to lead
           │
           ├──→ qa (test/validate/diagnose)
           │     └──→ returns results to lead
           │
           ├──→ ux / ui (design review)
           │     └──→ returns results to lead
           │
           └──→ analyst (research/investigate)
                 └──→ returns results to lead
```

- Human talks to lead only.
- Lead delegates with context: `handoff(to="eng", task="implement X")`.
- Delegated role works, returns results to lead.
- Lead reviews before presenting to human.
- Lead manages reloops (QA finds bug -> eng fixes -> QA re-verifies).

### Constitutional AI Pattern

Each role is a critic for specific principles:

| Role | Enforces |
|------|----------|
| qa | Correctness, security, edge cases |
| ux | Usability, accessibility, cognitive load |
| ui | Visual consistency, responsive design |
| arch | Scalability, maintainability, coherence |
| lead | Alignment with human intent |

Reloops = constitutional revision cycles. Stronger than single-agent because different perspectives create genuinely different evaluation.

## Implementation Phases

### Phase 1: Schema + New Identity Files

- Add `PhaseConfig` dataclass to `engine_types.py`.
- Update `IdentityFrontmatter` schema with `phases` and `role_type` fields.
- Write 7 new front office identity `.md` files (lead, arch, eng, qa, ux, ui, analyst).
- Write scribe identity `.md` file + grammar.
- Update compactor identity if needed.
- Delete old identity files (all 15 current ones).

### Phase 2: Router Disable + Config

- Default `routing.enabled` to `false`.
- Update `default_config.yaml` for single model.
- Update orchestrator to skip router initialization when disabled.
- Lead identity handles routing via existing handoff mechanism.

### Phase 3: Engine Support for Scribe

- Add scribe trigger points in engine loop (after handoff, periodic, on events).
- Scribe output stored in context-recoverable location.
- Integration with compactor (scribe notes survive compaction).

### Phase 4: Tests + Verification

- Update all unit tests for new identity set.
- Update model tests.
- TUI verification with new roles.
- Pre-commit green.

### Phase 5: Cleanup

- Delete unused GBNF grammars (keep compactor, scribe, remove rest).
- Update examples (hello-world, pychess).
- Version bump: 1.5.0 -> 1.6.0 (minor: new identity architecture, new config defaults).

## What This Does NOT Include

- TUI separation (P2-034, future).
- Multi-phase state tuning (architecture supports, v1 doesn't exercise).
- Concurrent agents / multi-context (future).
- VRAM heterogeneous scheduling (separate track).
- C++ rewrite (2.0.0).

## Acceptance Criteria

- [ ] 7 front office + 2 back office identity files written.
- [ ] Old 15 identity files deleted.
- [ ] `PhaseConfig` dataclass in `engine_types.py`.
- [ ] `IdentityFrontmatter` gains `phases` and `role_type` fields.
- [ ] Router disabled by default, lead routes via handoff.
- [ ] Single model config (no tier swaps required).
- [ ] Grammars only on compactor + scribe (back office).
- [ ] Scribe triggers in engine loop.
- [ ] All tests pass (unit + model).
- [ ] TUI functional with new roles.

## Risks & Considerations

- **Lead as bottleneck.** All traffic flows through lead. If the model is slow at triage, every interaction pays a latency tax. Mitigation: lead's system prompt must be tuned for fast delegation decisions — not deep analysis.
- **Scribe quality.** Unproven that the same model produces useful structured logs while also doing primary work. Mitigation: scribe runs as a separate inference call with its own system prompt, not inline with primary generation.
- **Backward compatibility.** Consumers referencing old identity names (code_writer, planner, etc.) will break. Mitigation: version bump to 1.6.0, document migration in changelog.
- **Delegation depth.** Lead -> eng -> (needs arch input) creates multi-hop delegation. V1 keeps delegation flat (lead delegates, role returns). Deep delegation is a future concern.
- **Phase complexity creep.** The phases architecture is extensible by design. Risk is premature addition of states before evidence supports them. Mitigation: V1 ships with one phase per role. Phases are added only when TUI testing reveals a need.

## Supersedes

**P1-024** (Bundled Identity Library) — the identity *system* stays, the *taxonomy* changes entirely. P1-024's infrastructure (frontmatter parsing, grammar loading, prompt manager discovery) remains. The 15 task-oriented identities and their grammars are replaced by 9 role-based identities.

## Implementation Log

(Empty — work not yet started.)
