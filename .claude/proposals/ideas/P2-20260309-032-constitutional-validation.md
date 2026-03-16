---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260309-032
title: "Inference-Time Constitutional Validation Pipeline"
priority: P2
component: core/engine
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-03-09
updated: 2026-03-09
tags: [constitution, validation, safety, pipeline, inference]
completed_date: null
scoped_files:
  - src/entropic/data/prompts/constitution.md
  - src/entropic/data/grammars/constitution_critique.gbnf
  - src/entropic/core/engine.py
  - src/entropic/core/constitutional.py
  - src/entropic/config/schema.py
  - src/entropic/benchmark/checks.py
depends_on: [P1-20260302-024]
blocks: []
---

# Inference-Time Constitutional Validation Pipeline

## Problem Statement

The engine's constitution (`src/entropic/data/prompts/constitution.md`) is a system
prompt preamble — it tells the model what to do, but there is no mechanism to verify
compliance. The model may generate responses that violate constitutional principles
(fabricate facts, present biased information as truth, make unsafe recommendations)
and the engine has no way to catch this before the response reaches the consumer.

This matters because LLMs are statistical generators optimized for plausibility, not
truth. Training data bias (frequency bias, perspective bias, authority flattening)
means the model can confidently produce outputs that violate constitutional principles
while sounding correct. A system prompt alone cannot guard against this — the same
model generating the response is the one "following" the constitution, using the same
biased representations.

### What This Is NOT

This is **not** Anthropic's Constitutional AI (CAI). CAI operates at training time —
critique/revision cycles generate RLHF training data that bakes principles into model
weights. This proposal implements **inference-time constitutional filtering**: a
post-generation validation pass that critiques output against written principles
before returning it to the consumer. The mechanism is different; the goal is similar.

### Limitation Acknowledged

The validation model has the same biases as the generation model — it's the same
statistical distribution evaluating its own output. A structured critique prompt
forces a different reasoning path (evaluation vs generation), which helps but does
not eliminate blind spots. This is better than no validation, not a guarantee of
correctness.

## Design

### Architecture: Pipeline Stage, Not Identity

Constitutional validation is an **engine pipeline stage**, not an identity. It runs
after the entire identity chain resolves, before the response reaches the consumer.

```
User prompt
  → route (0.6B router)
  → identity A (generate, primary model)
  → identity B (auto_chain, interstitial)      ← identity-controlled
  → identity C (auto_chain, interstitial)      ← identity-controlled
  → constitutional validation                   ← engine-controlled
  → return to consumer
```

Why not an identity:
- Not routable — the router should never send a user prompt to the validator
- Not auto-chainable — no identity should chain into the validator
- Not consumer-overridable prompt — the critique mechanism is engine-internal
- Closer to the router (engine infrastructure) than to a cognitive mode

### Triage: Deterministic, Not Model-Based

The 0.6B router model cannot reliably evaluate constitutional compliance — that
requires understanding nuance, detecting bias, and evaluating epistemic claims.
Instead, triage is deterministic based on identity metadata:

```yaml
# In identity frontmatter
constitutional_review: true    # engine checks this flag
```

Decision tree (no model call, zero latency):

```
Should this response be validated?

├── Grammar-constrained output?       → NO  (structurally safe)
├── Identity is interstitial?         → NO  (not user-facing)
├── identity.constitutional_review?   → use identity's declaration
├── Consumer config scope: all?       → YES (override)
├── Consumer config scope: none?      → NO  (override)
└── default                           → NO  (opt-in, not opt-out)
```

### Validation Flow

```
generate (primary, free-form)
  → check identity.constitutional_review
        │
  ┌─────┴─────┐
  │ false     │ true
  ▼            ▼
return    critique (primary model, GBNF-constrained)
               │
         ┌─────┴─────┐
         │ pass      │ fail
         ▼            ▼
       return    revise (primary model, free-form)
                      → critique (primary model, GBNF-constrained)
                           │
                     ┌─────┴─────┐
                     │ pass      │ fail
                     ▼            ▼
                   return    return with warning
                             (max_revisions exceeded)
```

The primary model is already loaded after generation, so the validation pass does
not require a model swap — it's an additional inference call on the same model.

### Latency Cost

| Phase | Approx tokens | Estimated time |
|---|---|---|
| Critique (GBNF-constrained) | 100-200 output | 1-3 seconds |
| Revision (if needed) | same as original | same as original generation |
| Second critique (if revised) | 100-200 output | 1-3 seconds |

Common case (compliant): +1-3 seconds.
Revision needed: +original generation time + 2-6 seconds.
Max revisions exceeded: same as revision, returns with warning metadata.

### Constitution Layering

```
Engine default constitution (bundled, immutable floor)
  ↓ merged with
Consumer constitution (.entropic/constitution.md, additive only)
```

The engine's bundled constitution defines the **floor** — minimum principles the
engine always enforces (safety, harm avoidance, intellectual honesty). Consumers
can **add** principles but cannot remove floor principles.

Rationale: if a consumer could delete "don't assist with code intended to damage
systems," the engine becomes a liability. The engine's safety posture is the
engine's responsibility, not the consumer's option.

### Critique Output Grammar

The critique is GBNF-constrained to structured JSON:

```json
{
  "compliant": true,
  "violations": []
}
```

```json
{
  "compliant": false,
  "violations": [
    {
      "principle": "intellectual_honesty",
      "excerpt": "the offending text from the response",
      "reason": "stated as fact without qualification"
    }
  ]
}
```

Benefits:
- Eliminates parsing failures
- Reduces output tokens (no preamble or hedging)
- Makes pass/fail decision mechanical
- Faster generation (fewer tokens)

The revision pass is NOT grammar-constrained — it regenerates free-form text.

### Strengthened Constitution

The current constitution lacks epistemic principles. The bundled default should
be extended:

**Current (behavioral):**
- Privacy First
- User Agency
- Transparency
- Safety
- Intellectual Honesty (vague)
- Harm Avoidance
- Balanced Judgment
- Error Handling

**Additions (epistemic):**
- **Epistemic Calibration**: Distinguish between "this is commonly stated" and
  "this is verified." When uncertain, qualify claims rather than asserting them.
- **Source Awareness**: Do not present statistical consensus as ground truth.
  Acknowledge when an answer reflects popular opinion rather than established fact.
- **Bias Transparency**: When a topic is known to have perspective bias in
  common discourse, acknowledge the limitation rather than presenting one
  perspective as default.

These additions give the critique pass concrete principles to evaluate against,
rather than the current vague "acknowledge uncertainty" language.

## Consumer Configuration

```yaml
# .entropic/config.local.yaml
constitution:
  path: .entropic/constitution.md       # consumer additions (optional)
  validation:
    enabled: true                        # opt-in (default: false)
    scope: identity                      # identity | all | none
    max_revisions: 1                     # cap revision loop (default: 1)
    model_preference: primary            # which tier validates
```

| scope value | behavior |
|---|---|
| `identity` | Respect each identity's `constitutional_review` flag (default) |
| `all` | Validate every response regardless of identity flags |
| `none` | Disable validation entirely (overrides identity flags) |

## Relationship to Existing Systems

### Benchmark (P1-029)

Constitutional compliance becomes a benchmark check type — not just "did the
identity produce the right format" but "did it violate constitutional principles."
This fits as a Layer 2 check:

```yaml
# New check type in benchmark checks.py
- type: constitutional
  constitution: default    # or path to custom
```

This doesn't fix individual responses but reveals which identities and prompt
types are most prone to constitutional violations, enabling targeted prompt
iteration.

### Auto-Chain

Constitutional validation is orthogonal to auto-chain. Auto-chain is
identity-to-identity flow controlled by identity frontmatter. Constitutional
validation is an engine gate after the chain resolves. They do not interfere
with each other.

### Identity Library (P1-024)

Each identity gains a `constitutional_review` frontmatter field:

| Identity | constitutional_review | Rationale |
|---|---|---|
| conversational | true | Free-form prose, highest bias risk |
| planner | true | Recommendations, reasoning claims |
| diagnoser | true | Diagnostic assertions |
| searcher | true | Search result synthesis |
| code_writer | false | Code output, structurally constrained |
| code_validator | false | Structured validation output |
| design_validator | false | Structured validation output |
| wireframer | false | Grammar-constrained output |
| extractor | false | Grammar-constrained output |
| compactor | false | Interstitial |
| pruner | false | Interstitial |
| quick | false | Short responses, low risk |
| test_writer | false | Code output |
| tool_runner | false | Tool execution, not prose |
| benchmark_judge | false | Engine-internal |

## Acceptance Criteria

- [ ] Engine pipeline runs constitutional validation after identity chain resolves
- [ ] Validation only fires when identity declares `constitutional_review: true`
- [ ] Consumer can override scope via config (`all`, `none`, `identity`)
- [ ] Consumer can add constitutional principles but cannot remove engine floor
- [ ] Critique output is GBNF-constrained to structured JSON
- [ ] Revision pass regenerates free-form (not grammar-constrained)
- [ ] `max_revisions` cap prevents infinite loops
- [ ] Response metadata includes validation result (pass/fail/skipped)
- [ ] Bundled constitution includes epistemic principles
- [ ] `constitutional` check type available in benchmark framework
- [ ] No model swap required for validation (reuses loaded primary model)
- [ ] Validation adds < 3 seconds for compliant responses

## Implementation Plan

### Phase 1: Constitution Strengthening
- Extend `src/entropic/data/prompts/constitution.md` with epistemic principles
- Define constitution merging logic (engine floor + consumer additions)
- Add `constitutional_review` field to identity frontmatter schema
- Update bundled identities with appropriate flags

### Phase 2: Critique Infrastructure
- Create `src/entropic/data/grammars/constitution_critique.gbnf`
- Create `src/entropic/core/constitutional.py` — critique prompt template,
  response parsing, compliance decision logic
- Unit tests for parsing, merging, decision logic

### Phase 3: Pipeline Integration
- Add validation stage to engine pipeline (post-chain, pre-return)
- Wire config schema (`constitution.validation.*`)
- Revision loop with `max_revisions` cap
- Response metadata includes validation result
- Integration tests

### Phase 4: Benchmark Integration
- Add `constitutional` check type to `src/entropic/benchmark/checks.py`
- Identity benchmarks can include constitutional compliance checks
- Report which identities/prompts are most prone to violations

## Implementation Log

{Entries added as work progresses}

## References

- Bai et al., "Constitutional AI: Harmlessness from AI Feedback" (2022) — arxiv.org/abs/2212.08073
- P1-20260302-024: Bundled identity library (identity frontmatter schema)
- P1-20260302-029: Benchmark framework (check type integration)
- Conversation context: data bias discussion → CAI applicability → engine design
