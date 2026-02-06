---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260206-009
title: "Dynamic Generation Window for Tool-Biased Output"
priority: P2
component: inference/orchestrator
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-06
updated: 2026-02-06
tags: [generation, performance, tool-calling, context-management]
completed_date: null
scoped_files:
  - src/entropi/inference/orchestrator.py
  - src/entropi/core/engine.py
  - src/entropi/config/schema.py
depends_on: []
blocks: []
---

# Dynamic Generation Window for Tool-Biased Output

## Problem

Fixed `max_output_tokens` creates a tension between two competing needs:

- **Small window**: Encourages tool calling, fast turns, responsive agent loop
- **Large window**: Allows complete code output, deep reasoning, file synthesis

A static value forces a compromise. Too large → runaway generation, model monologues
instead of acting. Too small → truncated code output, incomplete explanations after
tool results are ingested.

### Prior Bug

The orchestrator previously defaulted `max_tokens` to `context_length` (32768),
allowing the model to attempt filling the entire context window as output each turn.
This caused:

- Runaway generation on code/reasoning prompts (>30s per turn)
- Falcon thinking tier consuming excessive time per agentic turn
- Effectively limiting sessions to 1-2 turns before context exhaustion

Fixed by adding `max_output_tokens` per-tier config field (P2-20260206, commit TBD).

## Proposal

Make `max_output_tokens` dynamic per turn based on whether the model has pending
tool context to synthesize.

### Behavior

```
Turn type               max_output_tokens    Rationale
─────────────────────   ─────────────────    ─────────────────────────────
Initial (no tool ctx)   small (512-1024)     Bias toward tool calling
Post-tool-result        large (2048-4096)    Model has data, needs room
Continuation            medium (1024-2048)   Follow-up reasoning
```

### Sequence

```
User: "Find and fix the bug in sensor_driver.c"

Turn 1 [max_out=512]:
  Model: "I'll read the file." → <tool_call>read_file</tool_call>
  (stopped early, tool biased)

Turn 2 [max_out=4096]:
  [sensor_driver.c ~3500 tok in context]
  Model: "The bug is on line 47. Here's the fix: ..."
  → <tool_call>write_file</tool_call>
  (has data, uses larger window for analysis + fix)

Turn 3 [max_out=512]:
  [write result in context]
  Model: "Let me verify." → <tool_call>bash: make test</tool_call>

Turn 4 [max_out=1024]:
  [test output in context]
  Model: "All tests pass. The issue was an off-by-one in the DMA
   buffer index."
```

### Context Budget Awareness

Dynamic window should also consider remaining context budget:

```
available = context_length - input_tokens_used
max_output = min(dynamic_cap, available - reserve)
```

Where `reserve` ensures room for at least 1-2 more turns.

## Implementation Sketch

```python
# In orchestrator or engine
def _compute_max_output(self, messages: list[Message], tier: ModelTier) -> int:
    """Compute dynamic max_output_tokens based on turn context."""
    base = self._models[tier].config.max_output_tokens
    last_msg = messages[-1] if messages else None

    # After tool result → larger window for synthesis
    if last_msg and last_msg.role == "tool":
        return base  # full configured cap (4096)

    # Initial or continuation → smaller window, bias tool use
    return base // 4  # e.g., 1024
```

## Open Questions

- @architect: Should the dynamic scaling be configurable per-tier, or use a
  universal ratio (e.g., 1/4 base for non-tool turns)?
- @architect: Should the engine track turn type (initial/post-tool/continuation)
  explicitly, or infer from message history?
- @architect: Does the small initial window need prompt reinforcement ("use tools
  to gather information before responding") to be effective?

## Success Criteria

- Model calls tools within first 1-2 turns instead of monologuing
- Agentic sessions use 6-8+ turns within 32k context (vs 1-2 before)
- No generation exceeds 30 seconds per turn
- Code/reasoning quality maintained when synthesis window is used
