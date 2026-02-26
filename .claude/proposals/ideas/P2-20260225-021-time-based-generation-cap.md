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
updated: 2026-02-25
tags: [inference, performance, ux, config, per-tier]
completed_date: null
scoped_files: []
---

# P2-20260225-021: Time-Based Generation Cap

## Problem

Consumers currently configure `max_output_tokens` per tier — a hardware unit
that varies wildly in wall-clock time depending on GPU, model size,
quantization, context length, and system load. A consumer who wants "respond
within 10 seconds" must manually measure tok/s and compute the right token
budget. This changes across hardware, across models, and across sessions
as system load varies.

## Concept

Allow tiers to specify a time cap instead of (or alongside) a token cap:

```yaml
tiers:
  thinker:
    max_generation_time: 10s     # Wall-clock cap
    max_output_tokens: 1024      # Hard ceiling (safety net)
```

The engine measures tok/s during the session (rolling average or per-tier
measurement), converts the time cap to a dynamic token budget before each
generation call, and passes the lower of `time-derived budget` and
`max_output_tokens` to the backend.

## Why This Matters

- **Consumer UX**: Consumers think in wall-clock time ("the chess thinker
  should respond in under 10 seconds"), not tokens. Time is the natural
  unit for user-facing latency budgets.
- **Hardware portability**: A config that works on an RTX 4090 (fast tok/s)
  also works on an RTX 4000 Laptop (slower tok/s) without reconfiguring
  token budgets. The time cap auto-adjusts.
- **System load adaptation**: Under heavy GPU load (other apps, thermal
  throttling), tok/s drops. A time cap naturally produces fewer tokens
  rather than taking longer — graceful degradation.

## Analogy

`max_output_tokens` = "use this much fuel per cycle" (fuel injector)
`max_generation_time` = "run for X seconds at whatever throughput" (governor)

The engine acts as governor — it knows the current throughput and limits
generation duration, letting the hardware determine how much output that
produces.

## Open Questions

- How to measure tok/s: rolling window? per-tier? warmup calibration?
- Prompt processing time (prefill) vs generation time (decode) — the
  consumer probably means decode time, but prefill can be significant
  on large contexts
- Interaction with grammar: grammar may force the model to produce output
  past the time cap to complete a structural requirement. Should the
  engine interrupt mid-grammar?
- Streaming: does the time cap apply to first-token latency, total
  generation, or just decode phase?

## Dependencies

- tok/s measurement infrastructure (may already exist in backend stats)
- Timer integration in generate/generate_stream paths

## Acceptance Criteria

- [ ] Per-tier `max_generation_time` config field (optional, alongside max_output_tokens)
- [ ] Engine measures tok/s and converts time cap to dynamic token budget
- [ ] Lower of time-derived and explicit token budget is used
- [ ] Documented in library-consumer-guide.md
- [ ] Works across different hardware without config changes
