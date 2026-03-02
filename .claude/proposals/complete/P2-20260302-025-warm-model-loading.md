---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260302-025
title: "Warm model loading: RAM-resident model cache for fast VRAM swaps"
priority: P2
component: inference
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-03-02
updated: 2026-03-02
tags: [inference, vram, performance, model-loading]
completed_date: 2026-03-02
scoped_files:
  - "src/entropic/inference/llama_cpp.py"
  - "src/entropic/inference/orchestrator.py"
depends_on: []
blocks: []
---

# Warm Model Loading: RAM-Resident Model Cache for Fast VRAM Swaps

## Problem Statement

Model swaps are cold: `unload()` destroys the Python object and frees RAM,
`load()` reads from disk → RAM → VRAM. A 7B Q4 model takes 2-5s for a cold
swap. With the identity library (P1-20260302-024) introducing 13 identities
across 3-4 model files, swap frequency increases and cold load latency becomes
a UX bottleneck.

## Current State

```
Cold swap (current):
  unload tier A: VRAM freed, Python object destroyed, RAM freed
  load tier B:   disk → RAM → VRAM  (~2-5s for 7B Q4 on NVMe)

Warm swap (goal):
  unload tier A: VRAM freed, weights stay in RAM
  load tier B:   RAM → VRAM only    (~0.3-0.8s for 7B Q4 over PCIe 4.0)
```

## Proposed Solution

### Phase 0: mlock (immediate, one-line change)

Add `use_mlock=True` to `Llama()` constructor in `llama_cpp.py`. This pins
mmap pages in RAM, preventing OS eviction. Subsequent loads of the same file
hit hot page cache instead of disk I/O.

Not a true warm cache (model object is still destroyed and recreated), but
eliminates the disk → RAM portion of the load. RAM → VRAM transfer is the
dominant cost.

```python
model = Llama(
    model_path=str(self.config.path),
    n_ctx=self.config.context_length,
    n_gpu_layers=self.config.gpu_layers,
    use_mlock=True,   # Pin mmap pages in RAM
    verbose=False,
)
```

### Phase 1: Multi-model RAM cache

Replace single `_loaded_main_tier` tracking with a model cache:

```python
self._vram_tier: ModelTier | None          # Currently on GPU
self._warm_cache: dict[Path, ModelBackend]  # In RAM, GPU layers = 0
```

On swap: current model offloads from VRAM (stays in RAM), new model loads
from RAM to VRAM. Requires llama-cpp-python to support dynamic GPU layer
changes, or a fast model reconstruction from mlocked pages.

### Phase 2: LRU eviction

With multiple models pinned in RAM, total system memory usage grows. Add LRU
eviction when RAM pressure is detected. Least-recently-used models get fully
unloaded. Configurable max warm cache size.

## VRAM Budget Analysis (16GB GPU, 32GB+ RAM)

| Model | VRAM when active | RAM when warm | Role |
|-------|-----------------|---------------|------|
| Qwen3 0.6B | ~0.5GB | ~0.5GB | Router (always loaded) |
| Qwen3.5 35A3B Q2 | ~8-10GB | ~8-10GB | Primary identities |
| Qwen3 8B | ~5GB | ~5GB | Secondary identities |
| Falcon H1R 7B | ~5GB | ~5GB | Thinker |
| **VRAM peak** | **~10.5GB** | | 0.6B + 35A3B |
| **RAM total (all warm)** | | **~20GB** | All 4 models pinned |

Only one main model in VRAM at a time. Others warm in RAM. 0.6B always
co-resident.

## Acceptance Criteria

- [ ] Phase 0: `use_mlock=True` added to Llama constructor
- [ ] Phase 0: Measured swap time reduction (cold vs mlock)
- [ ] Phase 1: Multi-model cache tracks warm models in RAM
- [ ] Phase 1: Swap time < 1s for warm models
- [ ] Phase 2: LRU eviction under memory pressure
- [ ] Phase 2: Configurable max warm cache size

## Risks & Considerations

- **mlock requires sufficient RAM.** Pinning 20GB of model files in RAM on a
  32GB system leaves 12GB for OS + applications. May cause swap pressure on
  systems with less RAM.
- **llama-cpp-python may not support dynamic GPU layer changes.** Phase 1 may
  require model reconstruction from mlocked pages rather than true
  offload/reload. Need to investigate llama-cpp-python internals.
- **mlock may require elevated privileges** on some Linux configurations
  (ulimit -l). Document in installation guide.

## Implementation Log

### 2026-03-02
- [x] Phase 0: `use_mlock=True` applied to Llama constructor in llama_cpp.py
- **Decision:** Phases 1-2 superseded by P1-20260226-022 (VRAM orchestration)
  which covers the same scope with a more complete three-state lifecycle.
  Moving to COMPLETE with Phase 0 done; remaining work lives in P1-022.

## References

- Identity library proposal: P1-20260302-024
- VRAM orchestration proposal: P1-20260226-022
- llama-cpp-python mmap/mlock documentation
