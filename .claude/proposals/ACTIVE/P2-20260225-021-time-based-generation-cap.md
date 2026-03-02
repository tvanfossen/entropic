---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260225-021
title: "Time-Based Generation Cap"
priority: P2
component: inference
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-25
updated: 2026-03-02
tags: [inference, performance, ux, config, per-tier]
completed_date: null
scoped_files:
  - "src/entropic/inference/orchestrator.py"
  - "src/entropic/inference/llama_cpp.py"
  - "src/entropic/config/schema.py"
depends_on: []
blocks: []
---

# P2-20260225-021: Time-Based Generation Cap

## Problem

Consumers configure `max_output_tokens` per tier — a hardware-dependent
unit. A consumer who wants "respond within 10 seconds" must manually
measure tok/s and compute the right token budget. This changes across
hardware, models, and sessions as system load varies.

## Concept

Per-tier wall-clock time cap alongside token cap:

```yaml
tiers:
  thinker:
    max_generation_time_ms: 10000  # 10 second wall-clock cap
    max_output_tokens: 1024        # Hard ceiling (safety net)
```

Engine measures tok/s, converts time cap to dynamic token budget, passes
the lower of time-derived and explicit token budget to the backend.

## Identity Library Interaction (P1-024)

Time caps are especially useful for pipeline stages. Each identity can
have a time budget that keeps the total pipeline under a target:

```
Bug fix pipeline (14s target):
  diagnoser:  max_generation_time_ms: 3000
  pruner:     max_generation_time_ms: 1000
  planner:    max_generation_time_ms: 3000
  code_writer: max_generation_time_ms: 3000
  validator:  max_generation_time_ms: 2000
```

Without time caps, a thinker identity with `max_output_tokens: 1024` may
take 5s on an RTX 4090 or 30s on a laptop GPU — same config, wildly
different UX. Time caps auto-adapt.

## Design

### tok/s Measurement

```python
class ThroughputTracker:
    """Track per-tier tokens-per-second for time budget calculation."""

    def __init__(self, window_size: int = 5):
        self._measurements: dict[str, deque[float]] = {}
        self._window_size = window_size

    def record(self, tier: str, tokens: int, elapsed_ms: int) -> None:
        """Record a generation measurement."""
        if elapsed_ms <= 0:
            return
        tok_s = tokens / (elapsed_ms / 1000)
        if tier not in self._measurements:
            self._measurements[tier] = deque(maxlen=self._window_size)
        self._measurements[tier].append(tok_s)

    def estimate_tokens(self, tier: str, time_budget_ms: int) -> int | None:
        """Estimate token count for a time budget. None if no data."""
        if tier not in self._measurements or not self._measurements[tier]:
            return None
        avg_tok_s = sum(self._measurements[tier]) / len(self._measurements[tier])
        return int(avg_tok_s * (time_budget_ms / 1000))
```

Rolling window (last 5 generations per tier) smooths variance. First
generation for a tier has no measurement — falls back to `max_output_tokens`
only.

### Budget Resolution

```python
def resolve_token_budget(tier_config, throughput: ThroughputTracker) -> int:
    """Resolve effective token budget from time cap + token cap."""
    token_budget = tier_config.max_output_tokens

    if tier_config.max_generation_time_ms:
        time_derived = throughput.estimate_tokens(
            tier_config.name, tier_config.max_generation_time_ms
        )
        if time_derived is not None:
            token_budget = min(token_budget, time_derived)

    return token_budget
```

### Grammar Interaction

Grammar-constrained output may need to complete a structural requirement
that exceeds the time budget. Options:

1. **Hard cut** — interrupt at time cap, output may be invalid JSON
2. **Grammar priority** — let grammar complete, time cap is advisory
3. **Structural minimum** — time cap applies only after grammar's
   minimum structure is satisfied

**Recommendation: Grammar priority (option 2).** Grammar-constrained
identities have small `max_output_tokens` already (enforced by narrow
scope). The grammar will complete within a predictable time. The time cap
catches runaway unconstrained output, which is where it's most needed.

### What Time Cap Measures

The cap measures **decode time only** — time spent generating tokens.
Excludes:
- Prompt processing (prefill) — varies with context length, not a
  per-generation budget
- Model swap time — controlled by VRAM orchestration (P1-022)
- Tool execution time — controlled by tool timeouts

This matches the consumer's mental model: "how long is the model talking."

## Config Schema

```python
class TierConfig(BaseModel):
    max_generation_time_ms: int | None = None  # None = no time cap
    # Existing:
    max_output_tokens: int = 4096
```

## Acceptance Criteria

- [ ] `max_generation_time_ms` config field per tier (optional)
- [ ] ThroughputTracker measures tok/s per tier (rolling window)
- [ ] Time cap converted to dynamic token budget before generation
- [ ] Lower of time-derived and explicit token budget used
- [ ] First generation falls back to token-only budget (no measurement yet)
- [ ] Grammar-constrained tiers: grammar takes priority over time cap
- [ ] Works across different hardware without config changes
- [ ] Unit test: budget resolution logic
- [ ] Unit test: throughput tracker rolling window
- [ ] Documented in library-consumer-guide.md

## Implementation Plan

### Phase 1: ThroughputTracker
1. Add `ThroughputTracker` to orchestrator or new `inference/throughput.py`
2. Record measurements after each generation (tokens + elapsed_ms already
   available in `GenerationResult`)
3. Unit tests for measurement and estimation

### Phase 2: Budget Resolution + Config
1. Add `max_generation_time_ms` to TierConfig
2. Budget resolution in orchestrator before calling backend.generate()
3. Grammar priority: if tier has grammar, skip time cap adjustment
4. Unit tests for resolution logic

### Phase 3: Documentation
1. Consumer guide: time caps, interaction with grammar, pipeline budgets
2. Configuration docs: new field, examples

## Risks & Considerations

- **Cold start**: No tok/s data for first generation per tier. Falls back
  to token-only budget. After 1-2 generations, time cap is effective.
- **Throughput variance**: GPU thermal throttling, background load, context
  length all affect tok/s. Rolling window smooths but doesn't eliminate.
  Consumer should set time caps with ~20% headroom.
- **Grammar vs time cap**: Grammar priority means grammar-constrained
  tiers effectively ignore time caps. This is correct — grammar scope
  is already narrow. Document clearly.

## Implementation Log

{Entries added as work progresses}
