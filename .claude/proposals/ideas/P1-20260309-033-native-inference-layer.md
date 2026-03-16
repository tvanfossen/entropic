---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260309-033
title: "Native C++ Inference Layer — Replace llama-cpp-python"
priority: P1
component: inference
author: claude
author_email: noreply@anthropic.com
created: 2026-03-09
updated: 2026-03-09
tags: [inference, performance, llama-cpp, native, multi-gpu, vram]
completed_date: null
scoped_files:
  - src/entropic/inference/native/     # New C++ shared library
  - src/entropic/inference/bridge.py   # cffi bindings
  - src/entropic/inference/llama_cpp.py # Replaced by bridge
  - src/entropic/inference/orchestrator.py # Simplified (no _model_pool hack)
---

# P1-20260309-033: Native C++ Inference Layer

## Problem

llama-cpp-python's abstraction model (one Llama object = one model + one context +
one CUDA device) is fundamentally mismatched with entropic's multi-identity
architecture. Every architectural workaround in the orchestrator — `_model_pool`,
backend deduplication, the warm/active state machine, adapter separation — exists
to fight this coupling.

Specific limitations:

1. **1.1GB VRAM per Llama instance** — CUDA compute buffers allocated regardless of
   `n_gpu_layers`. Multiple tiers = multiple instances = wasted VRAM.
2. **No multi-context per model** — identity swap requires full model lifecycle, not
   lightweight context switch.
3. **No KV cache control** — no quantized KV (Q4_0/Q8_0), no prefix sharing, no
   per-sequence manipulation. Auto-sized, opaque.
4. **No multi-GPU tensor parallelism** — `tensor_split` exists but is poorly tested.
   Multi-GPU = multi-process workaround (2 models, not 1 model across 2 GPUs).
5. **No speculative decoding** — 4B draft + 35B verify would yield 2x throughput.
   Not exposed.
6. **No continuous batching** — router + main model can't overlap in one forward pass.
7. **Upstream staleness** — wrapper releases lag llama.cpp by weeks/months. Features
   like flash attention, quantized KV, and split modes arrive late or broken.

## Proposal

Replace `llama-cpp-python` with a thin C++ shared library (`libentinfer.so`) that
wraps llama.cpp's C API directly. Python calls it via cffi. The application layer
(TUI, MCP, routing, identity library) stays Python.

### C API Surface (~20 functions)

```c
// Model lifecycle — one model, multiple contexts
inf_model_t*   inf_model_load(const char* path, inf_model_params params);
void           inf_model_free(inf_model_t* model);

// Context lifecycle — lightweight, shares model weights
inf_ctx_t*     inf_ctx_create(inf_model_t* model, inf_ctx_params params);
inf_ctx_t*     inf_ctx_fork(inf_ctx_t* src, int seq_id);  // KV prefix sharing
void           inf_ctx_free(inf_ctx_t* ctx);

// Generation
inf_result_t   inf_generate(inf_ctx_t* ctx, const int32_t* tokens, int n_tokens,
                            inf_sample_params params);
inf_result_t   inf_generate_speculative(inf_ctx_t* main_ctx, inf_ctx_t* draft_ctx,
                                        const int32_t* tokens, int n_tokens,
                                        inf_sample_params params);

// Multi-GPU
inf_device_t*  inf_enumerate_devices(int* count);
void           inf_model_set_split(inf_model_t* model, float* splits, int n_devices);

// VRAM management
size_t         inf_vram_used(int device_id);
size_t         inf_vram_available(int device_id);

// Grammar
inf_grammar_t* inf_grammar_load(const char* gbnf_str);
void           inf_grammar_free(inf_grammar_t* grammar);
```

### Architecture

```
src/entropic/
├── core/              # Python: engine, routing, MCP, TUI (unchanged)
├── inference/
│   ├── bridge.py      # cffi calls to libentinfer.so
│   ├── orchestrator.py # Simplified — no _model_pool, no state machine
│   └── native/
│       ├── CMakeLists.txt
│       ├── entinfer.h         # Public C API
│       ├── entinfer.cpp       # Implementation
│       ├── model_pool.cpp     # Multi-model, multi-context lifecycle
│       ├── batch_scheduler.cpp # Continuous batching + speculative
│       └── kv_cache.cpp       # Prefix sharing, quantized KV
```

### What Dies

| Component | Fate |
|-----------|------|
| `llama-cpp-python` dependency | Removed |
| `LlamaCppBackend` class | Replaced by `bridge.py` |
| `_model_pool` in orchestrator | Removed — native model pool |
| Warm/active state machine | Simplified — context create/destroy |
| CUDA compute buffer waste | Eliminated — one model, one buffer |

### What Stays

| Component | Reason |
|-----------|--------|
| `ChatAdapter` classes | Tokenization templates are Python string ops |
| Identity library | Frontmatter parsing, grammar loading |
| Orchestrator routing logic | Tier selection, lock management |
| Benchmark framework | Measures the new layer same as old |

## Key Capabilities Unlocked

### Multi-Context Identity Swap (immediate win)
One `llama_model` for the 35B. 14 `llama_context` objects with different system
prompt caches. Identity swap = switch context pointer (~0ms). No model reload,
no VRAM churn, no compute buffer duplication.

### KV Cache Quantization (immediate win)
Q4_0 KV cache halves VRAM for KV storage. On 8GB GPU: fits ~29 layers instead of
~26. On 16GB: frees ~750MB for longer context or bigger models.

### Speculative Decoding (high value)
4B generates candidate tokens (42 tok/s). 35B verifies in batch. Expected
throughput: 50-60 tok/s on current hardware (vs 30 tok/s today). Gives 4B's
speed with 35B's quality.

### Multi-GPU Tensor Parallelism (future hardware)
`LLAMA_SPLIT_MODE_LAYER` distributes layers across GPUs. 2×16GB = 32GB total VRAM.
Enables 80B+ MoE models that don't fit on any single consumer card. One model
instance, one forward pass, NVLink/PCIe layer crossing.

Performance: ~85-90% of equivalent single-GPU (one PCIe crossing per token at
the split boundary). NVLink-equipped cards approach parity.

### Continuous Batching (optimization)
Router classification + main model inference overlap in same forward pass.
Eliminates the sequential router→swap→generate pipeline for first messages.

## Hardware Context

Current reference: RTX PRO 4000 Blackwell Laptop (16GB VRAM).

| Scenario | Current (wrapper) | Native layer |
|----------|-------------------|-------------|
| 16GB, single GPU | 30 tok/s, 14GB used | 50+ tok/s (speculative), ~13.5GB (no dup buffers) |
| 8GB, single GPU | Not viable (14GB model) | 8-15 tok/s (partial offload + KV quant) |
| 2×16GB, multi-GPU | Two separate models | One 80B+ model, tensor parallel |
| 4×8GB, multi-GPU | Not possible | One 35B+ model, layer-distributed |

## Risks

1. **Build complexity** — CUDA toolkit + CMake in build chain. Need prebuilt
   wheels or compile-from-source path. Solvable but real work.
2. **Debug across FFI boundary** — Segfaults produce unhelpful Python tracebacks.
   Need separate C++ test harness + sanitizer builds.
3. **Upstream tracking** — llama.cpp moves fast. Pin to tagged releases, update
   deliberately. No wrapper buffer.
4. **Scope** — This is a significant rewrite of the inference layer. Must be
   phased to avoid breaking the working system.

## Implementation Phases (sketch)

1. **Spike** — Build libentinfer.so against llama.cpp, expose model_load +
   ctx_create + generate. Python cffi bridge. Prove tok/s parity.
2. **Single-model migration** — Replace LlamaCppBackend with bridge for one tier.
   Benchmark validates no regression.
3. **Multi-context** — One model, multiple contexts. Identity swap via context
   switch. Benchmark validates VRAM savings + swap elimination.
4. **KV cache quantization** — Expose Q4_0/Q8_0 KV. Benchmark validates VRAM
   savings.
5. **Speculative decoding** — 4B draft + 35B verify. Benchmark validates 2x
   throughput target.
6. **Multi-GPU** — tensor_split + split_mode. Requires hardware to test.
7. **Remove llama-cpp-python** — Final cleanup once all paths migrated.

## Decision Criteria

Proceed if:
- Spike demonstrates tok/s parity with current wrapper
- Multi-context proves VRAM savings (eliminate 1.1GB/instance overhead)
- Build chain works on Linux (primary target) without manual CUDA setup

## Benchmark Validation

Layer 1 + Layer 2 benchmark framework (P1-029) measures before/after at each phase.
GPU sweep validates partial offload curves. Quality benchmark validates no
regression in output quality. Existing framework, no new tooling needed.

---

@architect: This is a significant infrastructure investment. The immediate wins
(multi-context, KV quant) justify the spike. Speculative decoding and multi-GPU
are the long-term payoff. Recommend spike → single-model migration as first
gate, then decide on full commitment.
