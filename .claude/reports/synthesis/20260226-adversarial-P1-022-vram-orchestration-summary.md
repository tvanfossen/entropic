# Adversarial Analysis: P1-20260226-022 (VRAM-Aware Model Orchestration)

**Date:** 2026-02-26
**Scope:** Proposal review — design and feasibility analysis
**Personas:** Explorer, Security, Negative, DRY, KISS, Positive

---

## Critical Issues (3)

### C1. Abstract methods on ModelBackend break all consumers [Explorer, Negative, DRY]

Adding `warm()`, `activate()`, `deactivate()`, `state` as **abstract** methods to
`ModelBackend` (an ABC) is a breaking change. Every class inheriting from it must
implement all four, including:

- `LlamaCppBackend` (production)
- 3 `MockModelBackend` classes in tests (`test_orchestrator_loading.py:28`,
  `test_library_api.py:186`, `test_library_consumer.py:45`)
- Any third-party consumer using `BackendFactory` to inject custom backends

The acceptance criterion "All existing unit and model tests pass without modification"
(line 221) is impossible with new abstract methods.

**Fix:** Make `warm()`, `activate()`, `deactivate()` **concrete with default
implementations** in the base class. `warm()` → `await self.load()`.
`activate()` → `await self.load()`. `deactivate()` → `await self.unload()`.
`state` → concrete property returning ACTIVE when `is_loaded`, COLD otherwise.
Only `LlamaCppBackend` overrides with three-state behavior. Existing backends
work unchanged.

### C2. "1-3 seconds" is unvalidated — needs Phase 0 benchmark [Negative, KISS]

The proposal's value proposition rests on warm→active taking 1-3s. This number is
derived from theoretical PCIe bandwidth, not measurement. The `Llama()` constructor
does more than mmap:

1. GGUF header parsing and validation
2. Tokenizer initialization (BPE merge tables)
3. Context/KV cache allocation (hundreds of MB for n_ctx=16384)
4. CUDA context setup (cuCtxCreate, cuMemAlloc)
5. Tensor mapping and compute graph compilation

Steps 3-6 happen regardless of page cache. If actual warm→active is 4-5s, the
proposal's complexity buys minimal improvement over cold reload.

**Fix:** Add a Phase 0 benchmark gate:
1. Measure cold load from NVMe (baseline)
2. Measure second load immediately after unload (page cache hot)
3. Measure load with n_gpu_layers=0, destroy, reload with n_gpu_layers=N
4. If (2) is already under 3s, the WARM state is unnecessary
5. If (3) is not significantly faster than (2), the WARM state adds no value

This is a 2-hour investigation that de-risks the entire proposal.

### C3. `gpu_layers: -1` semantic change is silently breaking [Explorer, DRY]

Currently `-1` means "offload all layers to GPU" (passed directly to llama-cpp-python).
The proposal redefines it to mean "use VRAM budget allocator." Existing consumers with
`gpu_layers: -1` (the default) silently change from "all layers on GPU" to
"budget-allocated layers" — which may be fewer layers on VRAM-constrained hardware.

**Fix:** Change the field type to `int | Literal["all", "auto"]`:
- `"all"` (new default) — current behavior, pass -1 to llama-cpp-python
- `"auto"` — use VRAM budget allocator
- `N` (int) — fixed override

This makes the policy explicit and backward compatible.

---

## High Priority Issues (5)

### H1. Partial activate() failure corrupts state machine [Security]

If `activate()` destroys the WARM Llama instance then fails to create the GPU instance
(CUDA OOM), the model is in limbo — neither WARM nor ACTIVE nor cleanly COLD.

**Fix:** Two-phase activate:
1. Create new GPU instance first (in try block)
2. Only if successful, destroy old CPU instance
3. If creation fails, leave WARM instance intact and return error
4. Acceptance criterion: "If activate() fails, model remains in WARM state"

### H2. Absent GPU yields dangerous VRAM budget defaults [Security]

If `nvidia-smi` is missing or GPU is absent, `total_vram` could be 0 or undefined.
The formula `model_budget // per_layer_cost` with negative budget could yield a
negative layer count. Passing `n_gpu_layers=-N` to llama.cpp is interpreted as "all
layers" — attempting to load everything onto a nonexistent GPU.

**Fix:** If VRAM discovery fails, force `gpu_layers=0` (CPU-only) and log WARNING.
Never let absent GPU default to full offload.

### H3. TOCTOU between deactivate and activate [Security]

The transition `deactivate()` current → `activate(N)` target must be atomic under a
single lock acquisition. If the lock is released between the two calls, another
coroutine can race in.

**Fix:** Mandate that both calls happen within a single `async with self._lock:` block.
Add acceptance criterion: "No state transition releases the orchestrator lock between
deactivate and activate."

### H4. `unload_all_models()` and `shutdown()` not addressed [Explorer]

These existing methods call `model.unload()` on all tiers. Under the new state machine:
- Voice mode (`unload_all_models`): should go ACTIVE→COLD (free everything)
- Shutdown: should go ACTIVE→COLD
- Reactivation after voice mode: should go COLD→WARM→ACTIVE (or WARM→ACTIVE if
  models were kept warm)

The proposal must explicitly specify behavior for these paths.

### H5. GC race with surviving references during streaming [Security]

If a streaming generation is in progress when `deactivate()` is called, the background
thread holds a reference to `self._model` through the closure. `del self._model` does
not free C-level resources until the thread finishes. The 30-second stream timeout
means GPU memory could be held for up to 30s after deactivate request.

**Fix:** `deactivate()` must block until all in-flight generations complete before
destroying the model instance. Add a generation-in-progress guard.

---

## Medium Priority Issues (5)

### M1. `warm_on_startup` belongs on TierConfig, not ModelConfig [Explorer, DRY]

The router uses `ModelConfig`, not `TierConfig`. Adding `warm_on_startup` to
`ModelConfig` means the router config gains a semantically meaningless field (the
router is "permanently active, never demoted to warm").

**Fix:** New fields go on `TierConfig`. Or better: centralize in a `ResourceConfig`
sub-config with a `warm_tiers: list[str]` field.

### M2. MoE per-layer cost estimation is unreliable [Negative, Security]

`per_layer_mb = file_size / n_layers` assumes uniform layer sizes. MoE layers can be
4-8x larger than attention layers. Over-estimating leads to wasted VRAM headroom.
Under-estimating leads to OOM → retry loops → 10-15s activation.

**Fix:** Read GGUF header metadata at warm time for per-layer tensor sizes. If
unavailable, use the flat estimate but with a 20% safety margin and a single retry
at N-2 layers on failure.

### M3. Three sources of model readiness [DRY]

`is_loaded` (backend), `state` (new), `_loaded_main_tier` (orchestrator) all track
overlapping aspects of "is this model ready?"

**Fix:** `state` is the single source of truth. `is_loaded` becomes a concrete
non-overridable `@property` returning `self.state == ACTIVE`. Rename
`_loaded_main_tier` to `_active_main_tier` and validate against backend state.

### M4. Warm Llama instance has hidden memory overhead [Negative]

A `Llama(n_gpu_layers=0)` allocates KV cache buffers, tokenizer state, and compute
graph in addition to model weights. Estimated 500 MB - 1 GB per warm model beyond
the mmap'd weight file. With 3-4 warm models this is 2-4 GB of invisible overhead.

**Fix:** Factor `n_ctx` into RAM budget calculation. Or: reduce `n_ctx` for warm
instances to minimum (512) since warm inference is "possible but slow" anyway.
Resize context at activation time.

### M5. GC+CUDA cleanup duplicated across transition paths [DRY]

`gc.collect()` + `torch.cuda.empty_cache()` + `torch.cuda.synchronize()` appears
in `unload_all_models()` and will be needed in `deactivate()`, `activate()`, and
`shutdown()`.

**Fix:** Extract `release_gpu_memory()` utility function. Call from all transition
points. One implementation, one place to add Metal/Vulkan cleanup later.

---

## Low Priority / Suggestions (3)

### L1. Phase 3 should be a separate proposal [KISS, Negative]

Router resource hints (context_estimate, generation_estimate, followup_likely) triple
the router's output complexity for unproven value. A 0.6B model doing multi-field
prediction is fundamentally different from single-digit classification. Error rates
likely 40-60%.

**Suggestion:** Extract to a separate IDEAS proposal. Remove from P1-022 to keep
scope focused.

### L2. scoped_files path is wrong [Explorer]

Proposal lists `src/entropic/inference/base.py`. Actual path is
`src/entropic/core/base.py`.

### L3. Config field grouping [DRY]

New VRAM/memory fields should be grouped into a `ResourceConfig` sub-config rather
than flattened onto ModelConfig/TierConfig. Prevents config sprawl as more resource
management features are added.

---

## Strengths Identified (5)

### S1. Memory hierarchy mapping is sound [Positive]

The three-state lifecycle (COLD/WARM/ACTIVE) maps 1:1 to the physical hardware
hierarchy (disk/RAM/VRAM). This alignment between software abstraction and physical
reality is strong systems engineering.

### S2. Advisor/decider separation is clean [Positive]

Router advises on semantics, orchestrator decides on resources. Neither needs the
other's internals. If router predictions are bad, orchestrator falls back to reactive
allocation with zero degradation.

### S3. Phase ordering by value/risk is correct [Positive]

Each phase can ship independently. Phase N doesn't break without Phase N+1. Phase 1
alone delivers 80% of the value. Exploratory phases are explicitly marked.

### S4. Unique differentiator [Positive]

No other local inference tool (llama.cpp CLI, LM Studio, ollama, koboldcpp) does
intelligent model pre-warming with dynamic GPU layer allocation. This directly
amplifies Entropic's multi-tier routing — the defining feature of the engine.

### S5. Builds cleanly on existing architecture [Positive]

BackendFactory, orchestrator lock, tier reuse detection, router as separate
always-loaded model — all extend naturally without redesign.

---

## Recommendations

### Quick Wins (before implementation)
1. **Phase 0 benchmark** — Measure actual cold/warm/hot reload times. 2-hour effort
   that validates or invalidates the entire proposal.
2. **Fix abstract → concrete** — Use default implementations on base class.
3. **Fix `gpu_layers` sentinel** — `int | Literal["all", "auto"]`.
4. **Fix scoped_files** — Correct the file path.

### Implementation Changes
5. **Two-phase activate** with rollback on failure.
6. **Atomic deactivate→activate** under single lock acquisition.
7. **Handle absent GPU** — force cpu-only, never assume VRAM.
8. **Address unload_all_models/shutdown** — specify state transitions.
9. **Extract release_gpu_memory()** utility.
10. **Move warm_on_startup to TierConfig** (or centralize in ResourceConfig).

### Scope Changes
11. **Extract Phase 3** to a separate IDEAS proposal.
12. **Add Phase 0** (benchmark gate) as prerequisite to Phase 1.
