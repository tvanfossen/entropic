---
version: 1.0.0
type: proposal
schema_version: 1
id: P3-20260226-023
title: "MoE expert-level VRAM caching with activation-aware placement"
priority: P3
component: inference
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-26
updated: 2026-02-26
tags: [vram, moe, cuda, expert-offloading, performance, exploratory]
completed_date: null
scoped_files:
  - "src/entropic/inference/llama_cpp.py"
  - "src/entropic/inference/base.py"
depends_on: ["P1-20260226-022"]
blocks: []
---

# MoE Expert-Level VRAM Caching with Activation-Aware Placement

## Problem Statement

MoE models (e.g., Qwen3.5-35B-A3B: 256 experts, 9 active per token) contain far more
expert weights than any single token uses. Current offloading strategies are binary:

- **All experts on GPU** (`-ngl 99`): Maximum speed, but requires VRAM for all 256
  experts per layer even though only 9 activate per token. A 35B MoE in Q4 needs
  ~21 GB — won't fit on a 16 GB GPU.
- **All experts on CPU** (`-ot exps=CPU`): Fits easily, but expert weight reads go
  through CPU RAM bandwidth (~80 GB/s) or worse, PCIe (~25 GB/s for results transfer).
  This creates a hard ceiling of ~40 tok/s from expert reads alone.

Neither strategy is optimal. The 9/256 = 3.5% active ratio per token means 96.5% of
GPU-resident expert weights are idle at any given moment. Meanwhile, expert activation
in trained MoE models is typically **skewed** — some experts activate far more
frequently than others.

An intelligent caching strategy that keeps frequently-activated experts on GPU and
evicts cold ones could approach GPU-bandwidth speeds for the majority of tokens while
fitting within constrained VRAM budgets.

### Bandwidth Analysis (Qwen3.5-35B-A3B, Q4_K_M)

Per-token expert weight read cost across 40 layers, 8 routed experts each:

| Expert Location | Memory Bandwidth | 8 Experts x 40 Layers | Tok/s Ceiling |
|---|---|---|---|
| GPU VRAM (GDDR7) | ~512 GB/s | ~1.3 ms | ~770 |
| CPU RAM (DDR5) | ~80 GB/s | ~8 ms | ~125 |
| CPU via PCIe 4.0 | ~25 GB/s | ~25 ms | ~40 |

GPU residency provides a 6-20x bandwidth advantage on the expert read path.
End-to-end tok/s improvement is smaller (attention, KV cache, other ops also
contribute) but expert reads are the dominant bottleneck during MoE generation.

## Proposed Solution (Exploratory)

### Expert LRU Cache in GPU VRAM

Maintain a fixed-size cache of expert weight slices in GPU VRAM. On each token:

```
┌────────────────────────────────────────────────────────┐
│  Per-Layer Expert Cache (GPU VRAM)                     │
│                                                        │
│  ┌──────────────────────────────────────────────┐      │
│  │  Cached: experts [3, 7, 12, 19, 42, 88, ...]│      │
│  │  Capacity: K experts (tunable per VRAM budget)│     │
│  │  Eviction: LRU or frequency-weighted          │     │
│  └──────────────────────────────────────────────┘      │
│                                                        │
│  Token N: router selects experts [7, 12, 42, 55, ...]  │
│    → [7, 12, 42] = cache HIT  → GPU matmul (fast)     │
│    → [55]        = cache MISS → CPU matmul (slow)      │
│    → Evict coldest, prefetch [55] for next token       │
│                                                        │
└────────────────────────────────────────────────────────┘
```

### Key Design Questions (Unresolved)

1. **Expert tensor packing**: llama.cpp stores all experts in a layer as a single
   3D tensor `{hidden, intermediate, n_experts}`. Caching individual experts means
   scatter-gather with `cudaMemcpy2DAsync` on strided slices. Is this practical at
   per-token frequency?

2. **Cache sizing**: With 256 experts and skewed activation, how many cached experts
   achieve 80%+ hit rate? This is model-specific and requires profiling real activation
   distributions. Literature (HOBBIT paper) suggests 20-30% of experts handle 80%+ of
   activations in models like Mixtral.

3. **ggml integration**: The compute graph expects tensors at known backend locations.
   A cache that moves expert slices between backends must either:
   - Hook into ggml's tensor dispatch (requires ggml fork or upstream support)
   - Maintain parallel GPU buffers and override tensor pointers before graph execution
   - Use a custom `MUL_MAT_ID` implementation that checks cache before dispatch

4. **Synchronization**: Expert caching adds conditional branches to the hot path.
   Cache miss → async CPU read + GPU compute for cached experts → sync before
   combining results. Pipelining this without stalling requires careful CUDA
   stream management.

5. **Activation profiling**: Do we profile at runtime (adaptive) or offline
   (static placement)? Runtime is more flexible but adds overhead. Offline requires
   a profiling pass per model, which is user-unfriendly.

### Implementation Complexity

This is a **custom CUDA component** — not achievable through llama-cpp-python's
public API. It requires either:

- Forking ggml to add expert-aware tensor dispatch (~1000-2000 lines of C/CUDA)
- Building a standalone CUDA layer that intercepts expert tensor reads
- Waiting for upstream support (llama.cpp [#11532](https://github.com/ggml-org/llama.cpp/issues/11532), [#19378](https://github.com/ggml-org/llama.cpp/pull/19378))

Estimated effort: 3-6 months for a working prototype. NVIDIA CUDA only.

### Alternative: Upstream Contribution Path

If the ggml tensor parallelism PR [#19378](https://github.com/ggml-org/llama.cpp/pull/19378) merges
and exposes `cpy_tensor_async` between backends, expert-level migration becomes
possible without a full fork. The caching logic could be implemented as a
higher-level layer on top of ggml's cross-backend copy primitives. This would
be the preferred path if upstream is receptive.

## Acceptance Criteria

- [ ] Profiling tool measures per-expert activation frequency for a given model + prompt set
- [ ] Expert cache achieves >80% hit rate on representative workloads
- [ ] Measurable tok/s improvement over `-ot exps=CPU` baseline (target: 2-4x)
- [ ] VRAM budget for expert cache is configurable and respects P1-022 VRAM budget system
- [ ] Cache operates transparently — no consumer-facing config beyond enable/disable + budget
- [ ] Graceful fallback: if CUDA unavailable, disable cache silently (CPU-only inference)

## Implementation Plan

### Phase 1: Activation Profiling & Feasibility

Profile expert activation distributions on representative MoE models to determine
whether caching is viable (skewed distribution required for cache effectiveness).

- Instrument llama.cpp's `MUL_MAT_ID` to log expert selection per layer per token
- Analyze activation distribution: entropy, top-K coverage, per-layer variance
- Determine minimum cache size for 80% hit rate per model family
- **Go/no-go decision**: If activation is uniform (high entropy), caching provides
  minimal benefit and this proposal should be shelved.

### Phase 2: Prototype Expert Cache (if Phase 1 is go)

Build a standalone CUDA expert cache that works alongside llama.cpp inference.

### Phase 3: ggml Integration

Integrate cache into the inference hot path, either via upstream hooks or fork.

## Risks & Considerations

- **Activation uniformity**: If expert routing is uniform (each expert equally likely),
  cache hit rates are low (~K/256) and the effort is wasted. Phase 1 profiling gates
  all subsequent work.
- **Model-specific behavior**: Cache effectiveness may vary wildly between MoE models.
  A cache tuned for Qwen3.5-35B-A3B may not help DeepSeek-R1. The engine must avoid
  model-specific assumptions.
- **ggml upstream churn**: llama.cpp's backend layer changes frequently. Any custom
  integration risks breakage on upstream updates. Minimize the integration surface.
- **NVIDIA lock-in**: CUDA-only implementation excludes AMD/Apple users. The proposal
  explicitly scopes this as an optional accelerator, not a requirement. Inference must
  work without it (falling back to llama.cpp's standard offloading).
- **Complexity budget**: This is the most complex feature Entropic would undertake.
  Must not destabilize the core inference path. The cache must be fully bypassable.

## Implementation Log

{Entries added as work progresses}

## References

- [P1-20260226-022 — VRAM-aware model orchestration (dependency)](../ACTIVE/P1-20260226-022-vram-orchestration.md)
- [llama.cpp #11532 — Only load activated experts to GPU](https://github.com/ggml-org/llama.cpp/issues/11532)
- [llama.cpp #19378 — Backend-agnostic tensor parallelism PR](https://github.com/ggml-org/llama.cpp/pull/19378)
- [llama.cpp #13386 — --no-op-offload for MoE](https://github.com/ggml-org/llama.cpp/pull/13386)
- [HOBBIT: Mixed-Precision Expert Offloading (arxiv 2411.01433)](https://arxiv.org/html/2411.01433v2)
- [ktransformers — Expert-level offloading for DeepSeek](https://github.com/kvcache-ai/ktransformers)
