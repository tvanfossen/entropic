---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260226-022
title: "VRAM-aware model orchestration with warm/active state management"
priority: P1
component: inference
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-26
updated: 2026-03-02
tags: [vram, orchestration, performance, tier-management, resource-management, warm-loading]
completed_date: null
scoped_files:
  - "src/entropic/inference/orchestrator.py"
  - "src/entropic/inference/llama_cpp.py"
  - "src/entropic/inference/base.py"
  - "src/entropic/config/schema.py"
depends_on: []
blocks: []
supersedes: [P2-20260302-025]
---

# VRAM-Aware Model Orchestration with Warm/Active State Management

## Problem Statement

Entropic's current model lifecycle is binary: a model is either fully loaded (in GPU VRAM) or
fully unloaded (destroyed). When the orchestrator switches tiers, it destroys the current model
and loads the next one from disk. For large models (20+ GB GGUF), this takes 5-10 seconds —
long enough to break conversational flow and make multi-tier workflows impractical.

The root issue is that Entropic treats VRAM and system RAM as a single resource. In practice,
they are a two-level hierarchy:

```
┌──────────────────────────────────────┐
│  GPU VRAM (16 GB)                    │  Fast inference. Scarce.
│  ┌────────────────────────────────┐  │
│  │  Active model layers           │  │
│  │  KV cache                      │  │
│  └────────────────────────────────┘  │
├──────────────────────────────────────┤
│  System RAM (62 GB)                  │  Slow inference. Abundant.
│  ┌────────────────────────────────┐  │
│  │  Warm models (mmap + mlock)    │  │
│  │  OS page cache                 │  │
│  └────────────────────────────────┘  │
├──────────────────────────────────────┤
│  Disk (NVMe SSD)                     │  Cold storage. Cheapest.
│  ┌────────────────────────────────┐  │
│  │  All GGUF files                │  │
│  └────────────────────────────────┘  │
└──────────────────────────────────────┘
```

By keeping models warm in system RAM and only promoting/demoting GPU layers on demand,
tier switches drop from 5-10s to 1-3s — with no llama.cpp upstream changes required.

Additionally, the current GPU layer allocation (`n_gpu_layers`) is static per-tier config.
In practice, the optimal layer count depends on runtime state: available VRAM, KV cache
pressure from context length, which other tiers might need activation soon. This proposal
introduces a VRAM budget system that dynamically determines GPU layer allocation at
activation time.

## Proposed Solution

### Core Concept: Three-State Model Lifecycle

Replace the binary loaded/unloaded lifecycle with a three-state machine:

```
                    startup / config
                         │
                         ▼
               ┌──────────────┐
               │   COLD       │  On disk only. No RAM consumed.
               │   (default)  │
               └──────┬───────┘
                      │ warm()
                      ▼
               ┌──────────────┐
               │   WARM       │  mmap'd + mlock'd in system RAM.
               │   (CPU-only) │  n_gpu_layers=0. Inference possible
               │              │  but slow (~0.5-2 tok/s).
               └──────┬───────┘
                      │ activate(gpu_layers=N)
                      ▼
               ┌──────────────┐
               │   ACTIVE     │  N layers on GPU, rest in CPU.
               │   (GPU)      │  Full inference speed.
               └──────────────┘
                      │ deactivate()
                      ▼
               ┌──────────────┐
               │   WARM       │  GPU layers released. Model stays
               │   (CPU-only) │  in RAM. Ready for fast reactivation.
               └──────────────┘
```

**Key transitions:**
- `COLD → WARM`: mmap file, mlock pages. Cost: ~1-2s (demand paging, one-time).
- `WARM → ACTIVE`: Free current GPU model, reload from page cache with N GPU layers.
  Cost: ~1-3s (PCIe transfer + allocation, no disk I/O).
- `ACTIVE → WARM`: Free GPU-backed Llama instance, reload with `n_gpu_layers=0`.
  Cost: ~0.5-1s (fast, no PCIe transfer needed for CPU-only load).
- `WARM → COLD`: munlock, release mmap. Cost: negligible.

### VRAM Budget System

Instead of static `gpu_layers` per tier, introduce a VRAM budget allocator:

```
┌─────────────────────────────────────────────────┐
│                  VRAM Budget                     │
│                                                  │
│  Total VRAM:          16,384 MB                  │
│  Reserved (driver):    ~500 MB                   │
│  Router (permanent):   ~200 MB  (0.6B Q4)       │
│  ─────────────────────────────                   │
│  Available budget:   ~15,684 MB                  │
│                                                  │
│  Allocated to active model:                      │
│    Model layers:      variable (N layers)        │
│    KV cache:          variable (context-dependent)│
│                                                  │
│  gpu_layers = f(model_size, context_needed,      │
│                 vram_budget, layer_vram_cost)     │
└─────────────────────────────────────────────────┘
```

The orchestrator calculates `gpu_layers` at activation time:

```
available_vram = total_vram - reserved - router_vram
kv_cache_budget = estimate_kv_cache(context_length, model_config)
model_vram_budget = available_vram - kv_cache_budget
gpu_layers = min(model.n_layers, model_vram_budget // per_layer_cost)
```

This allows the same model to activate with different GPU layer counts depending on
context requirements — more layers for short contexts, fewer for long ones.

### Router-Informed Resource Policy (Phase 3 — Exploratory)

The router already classifies prompt complexity to select tiers. Extending it to
inform resource allocation is a natural progression:

```
┌─────────────┐     "Complex reasoning task"     ┌──────────────────┐
│   Router     │ ──────────────────────────────► │  Policy Engine    │
│  (classify)  │     tier=thinking               │                  │
│              │     est_context=high             │  Decision:       │
│              │     est_tokens=long              │  - Activate 35B  │
│              │                                  │  - gpu_layers=20 │
│              │                                  │  - (long context │
│              │                                  │    needs KV room)│
└─────────────┘                                  └──────────────────┘
```

**What the router ADVISES (not decides):**
- Tier selection (existing behavior)
- Expected context utilization (short/medium/long)
- Expected generation length
- Whether follow-up turns are likely (keep model active vs. preemptively demote)

**What the orchestrator DECIDES (using router advice + system state):**
- GPU layer count for activation
- Whether to preemptively warm the next likely tier
- When to demote an active model to warm

The router is an advisor, not the resource manager. The orchestrator holds the
VRAM budget and makes allocation decisions. This separation keeps the router
focused on semantic classification and the orchestrator focused on resource
management.

**Risk assessment:** This phase is exploratory. The value depends on whether router
predictions are accurate enough to justify preemptive resource allocation. If the
router is wrong 30% of the time, preemptive warming wastes RAM bandwidth. The
safest approach is reactive (activate on demand) with the router integration as
an optimization layer that can be enabled/disabled per config.

### Why NOT Custom Cache Management

Taking over llama.cpp's internal tensor/buffer cache would require:

- Reimplementing ggml backend memory allocation (~3000+ lines of C across CUDA/Vulkan/Metal)
- Maintaining a fork that diverges from upstream on core memory management
- Handling device-specific memory semantics (CUDA unified memory, Vulkan device-local, etc.)
- Breaking on every llama.cpp update that touches the backend layer

The warm-RAM approach achieves ~80% of the benefit:

| Approach | Swap Latency | Implementation Cost | Maintenance Cost |
|---|---|---|---|
| Current (cold reload) | 5-10s | None | None |
| **Warm-RAM (this proposal)** | **1-3s** | **Medium (~2-3 weeks)** | **Low** |
| Custom cache management | ~300ms | Very high (~2-3 months) | Very high (fork) |
| Upstream migration API | ~300ms | High (~1 month C work) | Medium (upstream PR) |

The warm-RAM approach works entirely within llama-cpp-python's public API. No C code,
no fork, no upstream dependency. If llama.cpp eventually ships a migration API, the
`activate()` implementation can adopt it transparently — the Entropic-level architecture
doesn't change.

The upstream migration API remains an option for a future proposal if the warm-RAM
latency proves insufficient. It would upgrade `activate()` internals without changing
Entropic's public interface.

## Acceptance Criteria

- [ ] `ModelBackend` supports three states: COLD, WARM, ACTIVE
- [ ] Models can be warmed at startup via config (`warm_on_startup: true`)
- [ ] Tier switch via WARM→ACTIVE path completes in <3s for a 21 GB Q4 model
- [ ] Multiple models can be warm simultaneously without exceeding RAM budget
- [ ] VRAM budget allocator calculates `gpu_layers` dynamically at activation time
- [ ] Config supports per-tier `gpu_layers` override (bypass dynamic allocation)
- [ ] Orchestrator tracks VRAM usage and prevents over-allocation
- [ ] Router tier is permanently active (never demoted to warm)
- [ ] Auto-chain handoffs use warm→active path (no cold loads mid-chain)
- [ ] All existing unit and model tests pass without modification
- [ ] New unit tests cover: state transitions, VRAM budget calculation, concurrent warm models
- [ ] Graceful degradation: if mlock fails (insufficient privilege), fall back to mmap-only with warning

## Identity Library Interaction (P1-024)

The identity library introduces 13 identities across 3-4 model files. Most
identities share the same model — identity swap (prompt + grammar) is free,
only model swap costs time. This has direct implications for VRAM orchestration:

### Model Reuse Is Critical

| Model | Identities | Swap type |
|-------|-----------|-----------|
| Qwen3 0.6B | router, quick, pruner | Never swaps (always co-resident) |
| Qwen3.5 35A3B | planner, code_writer, test_writer, code_validator, tool_runner, extractor, conversational | Identity swap only (free) |
| Qwen3 8B | searcher, diagnoser, compactor | Model swap from primary (~1-2s warm) |
| Falcon H1R 7B | thinker | Model swap (rare, ~1-2s warm) |

The orchestrator MUST detect when a tier change requires the same model file
and skip the deactivate/activate cycle entirely. Only prompt + grammar change.

### Pipeline Latency Budget

A typical pipeline (diagnoser → pruner → planner → code_writer → validator)
involves at most 2 model swaps. With warm loading, total swap overhead is
~2-3s across the entire pipeline. The warm state is what makes multi-stage
pipelines viable.

## Implementation Plan

### Phase 0: mlock (COMPLETE)

`use_mlock=True` added to `Llama()` constructor in `llama_cpp.py`. Pins
mmap pages in RAM, preventing OS eviction. Subsequent loads of the same file
skip disk I/O. Applied in current working tree.

**Status: Done.** Remaining phases build on this foundation.

### Phase 1: Three-State Model Lifecycle

Extend `ModelBackend` and `LlamaCppBackend` with the warm/active state machine.

**1a. Backend interface changes (`base.py`)**

Add state enum and new abstract methods:

```python
class ModelState(Enum):
    COLD = "cold"        # On disk only
    WARM = "warm"        # In CPU RAM (mmap + mlock)
    ACTIVE = "active"    # GPU layers allocated

class ModelBackend(ABC):
    @property
    @abstractmethod
    def state(self) -> ModelState: ...

    @abstractmethod
    async def warm(self) -> None: ...

    @abstractmethod
    async def activate(self, gpu_layers: int = -1) -> None: ...

    @abstractmethod
    async def deactivate(self) -> None: ...
```

Preserve backward compat: `is_loaded` returns `True` when state is `ACTIVE`.
`load()` and `unload()` remain as convenience methods mapping to the new states.

**1b. LlamaCppBackend implementation (`llama_cpp.py`)**

- `warm()`: Load `Llama(n_gpu_layers=0, use_mmap=True)`. Model lives in CPU RAM.
  Pin with mlock if available.
- `activate(gpu_layers)`: If WARM, free current Llama instance, reload with
  `n_gpu_layers=gpu_layers`. Mmap page cache ensures no disk I/O.
- `deactivate()`: Free current Llama instance, reload with `n_gpu_layers=0`.
  Returns to WARM state.
- Track `_state: ModelState` internally.

**1c. Config schema changes (`schema.py`)**

```python
class ModelConfig(BaseModel):
    warm_on_startup: bool = False    # Pre-warm into CPU RAM at init
    gpu_layers: int = -1             # -1 = dynamic (use VRAM budget), N = fixed override
    use_mlock: bool = True           # Pin warm models in RAM
```

**1d. Orchestrator changes (`orchestrator.py`)**

- Startup: warm all tiers with `warm_on_startup=True`
- Tier switch: `deactivate()` current → `activate(N)` target (not unload → load)
- Router tier: `activate()` at startup, never `deactivate()`

**Acceptance criteria for Phase 1:**
- [ ] State machine transitions work correctly (cold→warm→active→warm→cold)
- [ ] Warm models consume system RAM but no VRAM
- [ ] Tier switches use deactivate/activate path
- [ ] Model reuse detected: identity swap on same model skips deactivate/activate
- [ ] Cold load path still works as fallback
- [ ] Benchmark: warm→active < 3s for 21 GB model

### Phase 2: VRAM Budget Allocator

Dynamic GPU layer calculation based on available resources.

**2a. VRAM discovery**

Query GPU VRAM at startup via `nvidia-smi` or llama.cpp's backend reporting.
Store as `total_vram_mb` in orchestrator state.

**2b. Per-layer cost estimation**

At warm time, the model's total size and layer count are known. Estimate
per-layer VRAM cost:

```
per_layer_mb = (model_file_size_mb - embedding_overhead_mb) / n_layers
```

Embedding/output head overhead is typically ~5-10% of total size. This estimate
is approximate but sufficient for budgeting — over-allocating by 1-2 layers just
means a failed load that falls back to fewer layers.

**2c. KV cache estimation**

```
kv_cache_mb = n_layers * n_kv_heads * head_dim * 2 * context_length * dtype_bytes / 1e6
```

For MoE models, KV heads are shared across experts — use the model's actual
`n_kv_heads`, not `n_experts * n_kv_heads`.

**2d. Budget allocation**

```python
def calculate_gpu_layers(self, model: ModelBackend, context_hint: int) -> int:
    available = self.total_vram - self.reserved_vram - self.router_vram
    kv_budget = estimate_kv_cache(context_hint, model.config)
    model_budget = available - kv_budget
    max_layers = int(model_budget / model.per_layer_cost)
    return min(max_layers, model.n_layers)
```

**2e. Config integration**

- `gpu_layers: -1` (default): use dynamic budget allocation
- `gpu_layers: N` (explicit): bypass budget, use fixed layer count
- New global config: `vram_reserve_mb: int = 512` (headroom for driver/other processes)

**Acceptance criteria for Phase 2:**
- [ ] VRAM total discovered at startup, logged
- [ ] Per-layer cost estimated at warm time
- [ ] Dynamic gpu_layers calculation matches manual estimates within ±2 layers
- [ ] Explicit gpu_layers override still works
- [ ] KV cache budget adjusts layer count for long contexts

### Phase 3: Router-Informed Resource Policy (Exploratory)

Extend the router's output to include resource hints that inform activation decisions.

**3a. Router resource hints**

Add optional fields to routing output:

```python
@dataclass
class RoutingResult:
    tier: ModelTier
    # ... existing fields ...
    context_estimate: str = "unknown"   # "short" | "medium" | "long"
    generation_estimate: str = "unknown" # "brief" | "moderate" | "extended"
    followup_likely: bool = False        # Hint for preemptive warming
```

The router's classification prompt gains a secondary instruction to estimate
resource usage. This can be a coarse 3-way classification — precision isn't
critical since it only tunes `gpu_layers` within a range, not binary decisions.

**3b. Context-aware activation**

When the router signals `context_estimate="long"`, the budget allocator reduces
GPU layers to leave VRAM headroom for KV cache. When `"short"`, it maximizes GPU
layers for speed.

**3c. Preemptive warming**

When `followup_likely=True`, the orchestrator can preemptively warm the predicted
next tier (e.g., after a thinking tier, the normal tier is likely next).

**@architect: This phase needs validation.** The value depends on router prediction
accuracy. Recommend implementing Phase 1-2 first, gathering real swap latency data,
then deciding if Phase 3 optimization is worth the added router complexity.

**Acceptance criteria for Phase 3:**
- [ ] Router emits resource hints without degrading classification accuracy
- [ ] Context estimate influences gpu_layers calculation
- [ ] Preemptive warming is config-gated (`preemptive_warm: bool = false`)
- [ ] Benchmark: preemptive warm reduces observed swap latency in multi-turn chains

### Phase 4: Observability & Diagnostics

**4a. VRAM dashboard data**

Expose resource state for TUI/logging consumption:

```python
@dataclass
class ResourceSnapshot:
    total_vram_mb: int
    used_vram_mb: int
    active_model: str | None
    active_gpu_layers: int
    warm_models: list[str]
    warm_ram_mb: int
    kv_cache_mb: int
```

**4b. Tier swap metrics**

Log and expose timing for each transition:

```
[VRAM] Deactivating thinking (28 layers) → warm: 0.8s
[VRAM] Activating normal (32 layers) from warm: 1.2s
[VRAM] Budget: 15684 MB total, 12400 MB model, 3284 MB KV cache
```

**Acceptance criteria for Phase 4:**
- [ ] Resource snapshot available via diagnostics MCP server
- [ ] Swap timing logged at INFO level
- [ ] VRAM budget breakdown logged at activation time

## Risks & Considerations

- **mlock privileges**: `mlock` may require `CAP_IPC_LOCK` or elevated `RLIMIT_MEMLOCK`.
  If unavailable, fall back to mmap-only (pages may be evicted under memory pressure,
  degrading warm→active performance). Log a warning.

- **RAM overcommit**: Warming all models simultaneously assumes sufficient RAM. If total
  warm model size exceeds available RAM, mlock will fail. The orchestrator must validate
  RAM budget at startup and warn/refuse if insufficient. Config should support marking
  which tiers to warm vs. leave cold.

- **GC timing**: Python's garbage collector may not immediately free the Llama instance
  on deactivate/activate transitions. Between freeing the old instance and creating the
  new one, both may briefly coexist in memory. Use explicit `del` + `gc.collect()` +
  CUDA cache clearing in the transition path.

- **llama-cpp-python mmap behavior**: The Python bindings may not expose `use_mmap` and
  `use_mlock` parameters directly. Verify these are passthrough to the C API. If not,
  investigate patching or using ctypes to set them.

- **Page cache eviction**: Without mlock, warm models rely on the OS page cache. Under
  memory pressure, pages may be evicted, degrading warm→active back to cold-load speed.
  mlock prevents this but consumes committed memory.

- **MoE layer cost estimation**: MoE models have unequal layer sizes (expert layers vs.
  attention layers). Per-layer cost estimation may be less accurate. Consider using
  the model's metadata (if available in GGUF header) for more precise estimates.

- **Concurrent activation during auto-chain**: Auto-chain may trigger rapid
  deactivate→activate sequences. Ensure the lock prevents TOCTOU races where two
  coroutines attempt simultaneous activation.

## Implementation Log

{Entries added as work progresses}

## References

- [llama.cpp #18491 — Runtime GPU layer change request (closed/stale)](https://github.com/ggml-org/llama.cpp/issues/18491)
- [llama.cpp #11703 — VRAM release when idle (closed/stale)](https://github.com/ggml-org/llama.cpp/issues/11703)
- [llama.cpp #19378 — Backend-agnostic tensor parallelism (open PR)](https://github.com/ggml-org/llama.cpp/pull/19378)
- [P2-20260225-020 — GPU Resource Management (backlog)](../backlog/P2-20260225-020-gpu-resource-management.md)
- [P3-20260226-023 — MoE expert-level VRAM caching (exploratory)](P3-20260226-023-moe-expert-vram-caching.md)
- Qwen3.5-35B-A3B: 35B total / 3B active MoE, 40 layers, 21 GB Q4_K_M
- PCIe 4.0 x16 bandwidth: ~12-14 GB/s theoretical
