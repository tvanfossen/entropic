---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260302-030
title: "Model candidate investigation: SSM/hybrid and specialized models"
priority: P2
component: models
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-03-02
updated: 2026-03-02
tags: [models, mamba, ssm, hybrid, falcon, evaluation]
completed_date: null
scoped_files: []
depends_on: [P1-20260302-029]
blocks: []
---

# Model Candidate Investigation: SSM/Hybrid and Specialized Models

## Context

Benchmark results from 2026-03-02 established the current model stack:

| Role | Model | tok/s | VRAM | Status |
|------|-------|-------|------|--------|
| Primary | Qwen3.5-35B-A3B Q2_K | 30-41 | 12,877 MB | Validated |
| Secondary/Fast | Qwen3-8B Q4_K_M | 62 | ~5,000 MB | Validated |
| Reasoning | Falcon-H1R-7B | TBD | ~4,600 MB | In use, not benchmarked |
| Code | (none — primary serves) | — | — | Gap |
| Infrastructure | Qwen3-0.6B Q8_0 | — | ~640 MB | In use |

This proposal tracks model candidates to evaluate via `entropic benchmark`
(P1-20260302-029) when the framework is ready. No model changes until
benchmark data supports them.

## Current Stack Strengths

- **Qwen3.5-35B-A3B Q2_K**: MoE, full GPU offload at 16GB, quality comparable
  to Q4 at 10x the speed. Nothing in the MoE space displaces it for 16GB VRAM.
- **Falcon-H1R-7B**: Only hybrid Mamba2+Attention model with full upstream
  llama.cpp support, reasoning-tuned via GRPO.

## Candidates to Benchmark

### Tier: Primary (MoE slot)

**Jamba-1.5-Mini** (AI21)
- 52B total / 12B active — Mamba + Transformer + MoE (16 experts, 2 active)
- The only released MoE+SSM model. Combines sparse expert activation with
  linear-scaling SSM layers.
- GGUF available, llama.cpp supported (PR #7531)
- Concern: 52B total params may not fit 16GB at usable quantization.
  Need to benchmark actual VRAM at Q2/Q3/Q4.
- **Test**: Does MoE+SSM beat pure MoE (Qwen3.5) on tok/s at same quality?

### Tier: Secondary/Fast

**Falcon-H1-3B**
- Mamba2+Attention hybrid, 3B params, ~2GB at Q4_K_M
- If SSM throughput advantage materializes, could match Qwen3-8B speed at
  less than half the VRAM. Frees VRAM for larger primary model.
- Full upstream llama.cpp support (same PR as H1R-7B)
- **Test**: tok/s vs Qwen3-8B. Quality on searcher/diagnoser identities.

**Falcon-H1-1.5B-Deep**
- Claims 7-10B Transformer parity. ~1GB at Q4_K_M.
- If true, this is a secondary model at infrastructure-tier VRAM cost.
- **Test**: Quality parity claim. Does it actually match 8B on focused tasks?

### Tier: Reasoning

**Falcon-H1R-7B** (current, needs formal benchmark)
- Already in use but never run through standardized benchmark.
- 88.1% AIME-24 math, Mamba2 hybrid gives better context scaling.
- **Test**: Baseline benchmark to establish comparison point.

### Tier: Code

**Qwen3-Coder-7B**
- Purpose-built code model, 262K context, ~5GB at Q4_K_M
- Same Qwen ecosystem (consistent tool calling, tokenizer)
- **Test**: code_writer and code_validator identity quality vs primary model
  doing code. Does a dedicated code model justify the swap cost?

### Tier: Infrastructure

**Falcon-H1-0.5B**
- Mamba2+Attention hybrid at 0.5B. Same architecture as H1R-7B, miniaturized.
- SSM routing: does linear scaling matter for classification? Probably not
  at the token counts routers see, but VRAM/speed characteristics may differ.
- **Test**: Classification accuracy vs Qwen3-0.6B. Throughput comparison.

**Falcon-H1-Tiny-90M**
- 90M params. Absolute minimum viable router.
- **Test**: Can a 90M hybrid model classify accurately enough for routing?

**Qwen3-1.7B**
- Dense, matches Qwen2.5-3B performance, same ecosystem as primary.
- Low-risk upgrade from 0.6B if classification accuracy is an issue.
- **Test**: Classification accuracy at 1.7B vs 0.6B.

## SSM/Hybrid Architecture Notes

### Why SSM Matters for Local Inference

| Property | Transformer | Hybrid (Falcon H1) |
|----------|------------|---------------------|
| KV cache | O(n) per layer | Proportional to attention head ratio only |
| Generation | Memory-bandwidth bound | More compute-bound (better GPU utilization) |
| Context scaling | Quadratic prefill | Linear for SSM layers |
| Practical crossover | — | 8K-16K tokens (below that, marginal) |

The real win for local inference is at longer contexts (16K+) and in VRAM
savings from reduced KV cache. At short contexts (2K-4K), the advantage
is modest (10-30%).

### llama.cpp SSM Support Status (March 2026)

- **Falcon-H1 (Mamba2+Attn)**: Full upstream support, GPU offload works
- **Pure Mamba1**: CPU only effectively (GPU kernels incomplete, issue #6758)
- **Jamba (Mamba+Transformer+MoE)**: Supported (PR #7531), mixed results
- **RWKV-6/7**: Limited support
- **Nemotron-H, Zamba**: Not supported / no GGUF

### MoE + SSM: The Theoretical Optimum

Combining MoE (sparse expert activation) with SSM (linear context scaling)
would give: few active params per token + no KV cache growth + efficient
GPU utilization. Jamba-1.5-Mini is the only shipped model testing this thesis.
Monitor for:
- Falcon H1 + MoE variants (not released)
- Qwen MoE + SSM hybrids (not released)
- Hunyuan-TurboS derivatives at smaller scale (560B is way over budget)

## Evaluation Order

When `entropic benchmark` is ready, evaluate in this order:

1. **Falcon-H1R-7B** — establish baseline for existing reasoning model
2. **Falcon-H1-3B** — quick test of SSM advantage at small scale
3. **Qwen3-Coder-7B** — assess dedicated code model value
4. **Jamba-1.5-Mini** — MoE+SSM thesis test (may not fit VRAM)
5. **Falcon-H1-1.5B-Deep** — infrastructure upgrade candidate
6. **Falcon-H1-0.5B / Tiny-90M** — router alternatives

## References

- P1-20260302-029: entropic-benchmark framework (evaluation tool)
- P1-20260302-024: Bundled identity library (identity definitions)
- Falcon-H1 family: https://huggingface.co/tiiuae
- Jamba-1.5: https://huggingface.co/ai21labs/AI21-Jamba-1.5-Mini
- llama.cpp Mamba support: https://github.com/ggml-org/llama.cpp/issues/6758
