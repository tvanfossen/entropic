---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260225-020
title: "GPU Resource Management"
priority: P2
component: inference
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-25
updated: 2026-03-02
tags: [gpu, performance, resource-management, cuda, laptop]
completed_date: null
scoped_files:
  - "src/entropic/inference/orchestrator.py"
  - "src/entropic/inference/llama_cpp.py"
  - "src/entropic/config/schema.py"
depends_on: [P1-20260226-022]
blocks: []
---

# P2-20260225-020: GPU Resource Management

## Problem

During model inference, llama.cpp saturates GPU compute and memory bandwidth.
On laptops with shared thermal/power budgets, this starves other processes
(display compositor, video decode, input handling) making the system unusable
during generation. Affects both TUI and library consumers.

No engine-level mechanism exists to cap GPU utilization — the consumer must
manually tune llama.cpp kwargs or configure external tools.

## Relationship to P1-022 (VRAM Orchestration)

P1-022 manages VRAM allocation (how many layers on GPU). This proposal
manages GPU compute pressure (how hard the GPU works per step). Orthogonal:

| Concern | P1-022 | P2-020 |
|---------|--------|--------|
| VRAM allocation | Dynamic `gpu_layers` | Not touched |
| Compute saturation | Not addressed | `n_batch`, thread limits |
| System responsiveness | Not addressed | Resource profiles |

## Proposed Solution

### Phase 1: Expose llama.cpp Knobs as Config

Promote impactful params from opaque kwargs to validated config fields:

```yaml
inference:
  n_batch: 256          # Tokens per eval step (lower = less compute spike)
  n_threads: 4          # CPU threads for prompt processing
  n_threads_batch: 4    # CPU threads for batch processing
```

These already work via tier kwargs. The value is validation, documentation,
and applying them consistently across all tiers.

**Config schema changes:**

```python
class InferenceConfig(BaseModel):
    n_batch: int = 512          # llama.cpp default
    n_threads: int | None = None  # None = llama.cpp auto-detect
    n_threads_batch: int | None = None
```

**LlamaCppBackend changes:**

Pass config values to `Llama()` constructor. Currently only `n_ctx`,
`n_gpu_layers`, `chat_format`, `use_mlock`, `verbose` are passed.

### Phase 2: Resource Profiles

Predefined profiles that set multiple knobs at once:

```yaml
inference:
  resource_profile: balanced  # or: maximum, background, minimal
```

| Profile | n_batch | n_threads | Use Case |
|---------|---------|-----------|----------|
| `maximum` | 512 | auto | Dedicated inference machine |
| `balanced` | 256 | auto | Laptop with other apps running |
| `background` | 128 | 2 | Background service, minimal impact |
| `minimal` | 64 | 1 | Development/testing, GPU shared heavily |

Profile sets defaults; explicit field values override. Profile is sugar
over individual knobs.

### Phase 3: CUDA MPS Integration (Optional)

`CUDA_MPS_ACTIVE_THREAD_PERCENTAGE` limits GPU SM utilization. Must be
set before CUDA context initialization.

```yaml
inference:
  gpu_compute_percent: 80  # MPS thread percentage (10-100)
```

Engine sets env var in orchestrator startup (before first model load).
MPS daemon must be running externally — engine logs warning if not.

**This phase is optional.** MPS is powerful but requires external daemon
setup. Phases 1-2 provide meaningful control without MPS.

## Acceptance Criteria

- [ ] `inference.n_batch` config field, validated, passed to Llama constructor
- [ ] `inference.n_threads` config field, validated, passed to Llama constructor
- [ ] `inference.resource_profile` config field, resolves to knob values
- [ ] Explicit values override profile defaults
- [ ] System remains responsive during inference on laptop hardware
  (qualitative — measurable via compositor frame drops or input latency)
- [ ] Config validated at load time (range checks)
- [ ] Documented in library-consumer-guide.md
- [ ] Optional: `gpu_compute_percent` sets CUDA_MPS_ACTIVE_THREAD_PERCENTAGE

## Implementation Plan

### Phase 1: Config + Backend Plumbing
1. Add `InferenceConfig` fields to schema.py
2. Pass `n_batch`, `n_threads` to `Llama()` constructor in llama_cpp.py
3. Validate ranges at config load
4. Unit test: config values flow through to backend

### Phase 2: Resource Profiles
1. Add `resource_profile` enum to InferenceConfig
2. Profile → knob resolution logic
3. Explicit values override profile
4. Document profiles in consumer guide

### Phase 3: CUDA MPS (Optional)
1. Add `gpu_compute_percent` to InferenceConfig
2. Set env var in orchestrator startup
3. Log warning if MPS daemon not detected
4. Document MPS setup in consumer guide

## Risks & Considerations

- **n_batch impact varies by model.** Lower n_batch reduces compute spikes
  but increases per-token latency. The tradeoff is hardware-dependent.
  Profiles provide sane defaults; consumers tune from there.
- **Thread count vs performance.** Limiting threads helps responsiveness
  but slows prompt processing (prefill). Generation (decode) is primarily
  GPU-bound, so thread limits mainly affect first-token latency.
- **MPS daemon is external.** Engine can't start it. Must be documented
  as an optional system-level setup step.

## Implementation Log

{Entries added as work progresses}
