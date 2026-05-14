# entropic v2.1.11

Patch release introducing **SecondaryModelLoader (gh#27)** and the
**speculative-decoding infrastructure (gh#36)**. Closes out the four-
patch sequence (v2.1.8 multimodal → v2.1.9 registry → v2.1.10 mid-gen
queue → v2.1.11 speculative) bundled to the v2.2.0 milestone tag.

The speculative-decoding *kernel* lands in-tree but is gated INERT
at this llama.cpp pin (`253ba110b`) — see "Speculative kernel status"
below. All v2.1.11-listed infrastructure (compat check, draft slot,
config schema, C ABI, hybrid+recurrent arch guard, orchestrator
routing) is in place and consumer-reachable.

## Highlights

- **gh#27 (MEDIUM):** SecondaryModelLoader — unified role-keyed
  lifecycle for non-primary inference backends.
  - Replaces the per-role `router_` shared_ptr on `ModelOrchestrator`
    with composition over a slot map keyed by role name. Today's
    roles: `"router"` (digit classifier) and `"draft"` (speculative
    proposer); thinking-model (gh#25) lands the same way.
  - API: `ensure_loaded(role, ModelConfig)`, `get(role)`,
    `get_shared(role)`, `release_role(role)`, `is_loaded(role)`,
    `loaded_roles()`, `clear_all_prompt_caches()`, `shutdown()`.
  - No observable change for router consumers — existing classify
    path, diagnostics, and cache management behave identically.
  - Single-class internal helper (no interface layer), following the
    AdapterManager (#29) and GrammarRegistry (#31) precedent.

- **gh#36 (LARGE — infrastructure landed, kernel staged):**
  Speculative decoding scaffolding.
  - New config schema (off by default, additive):
    ```yaml
    inference:
      speculative:
        enabled: false
        draft_model: <bundled key or path>
        n_draft: 16
        draft_n_gpu_layers: 0
        draft_cpu_threads: 4
    ```
    `draft_model` accepts a bundled-registry key (e.g.
    `qwen3_5_0_8b`) or a literal path; resolution happens at
    config-parse time via `BundledModels::resolve()`.
  - New helper `entropic::speculative::check_compat(target, draft)`
    mirrors the file-private `common_speculative_are_compatible`
    rules from `extern/llama.cpp/common/speculative.cpp` plus an
    explicit recurrent-architecture gate (target must NOT be
    Mamba/RWKV/hybrid — upstream's speculative layer does not
    self-disable at pin `253ba110b`).
  - New C ABI: `entropic_speculative_compat(handle, *compatible,
    **diagnostic)` — metadata-only query, no model state allocation.
    Returns a heap-allocated diagnostic on rejection (caller frees
    with `entropic_free_string`).
  - New `BackendCapability::SPECULATIVE_DECODING` reporting
    (existing enum value, wired with a dynamic check).
  - New backend virtual `do_generate_speculative` + public
    `generate_speculative` wrapper. Default impl returns
    `ENTROPIC_ERROR_NOT_SUPPORTED`.
  - Orchestrator routing: when `inference.speculative.enabled` is
    true AND the configured pair is compatible, attempts
    `generate_speculative`; on `NOT_SUPPORTED`, logs and falls back
    to plain streaming.

## Speculative kernel status (gated inert at this pin)

The v2.1.11 kernel is fully implemented in `LlamaCppBackend::
generate_speculative_with_draft` and integrates `common_speculative_*`
plus a `common_sampler`-based accept-N path that mirrors upstream's
`speculative-simple.cpp`. The kernel + arch guard + tests all SHIP.

**Empirical state at the v2.1.11 llama.cpp pin (`253ba110b`):**
At this pin, **no bundled primary delivers both bit-identical
correctness AND positive speedup**. The Session 5 forensics document
why (see `.claude/proposals/ACTIVE/v2.1.11-speculative-decoding.md`
Gate A and `docs/architecture-cpp.md` decision log entry #41):

- **Bit-identical correctness fails on every bundled primary.**
  Upstream's `speculative-simple.cpp` splits the prefill into two
  ubatches (`prefill[0..N-2]` then a batched decode of `[id_last +
  drafts]`). This implicitly assumes pure-transformer state
  continuity across the ubatch boundary. At this pin, every bundled
  primary has at least partial recurrent state that does NOT carry
  across the boundary correctly. Gate A confirmed empirically on
  both Qwen3.5 and Gemma 4 — identical input tokens produce
  catastrophically different top-1 logits at sequence position
  N-1 (e.g., on Qwen3.5: `<think>` @ logit 27 vs `#` @ logit 12.7;
  on Gemma 4: `'2'` @ 28 vs `'\n'` @ 7.9). The kernel itself is
  correct against upstream's API.

- **Speedup is negative even where the kernel runs.** On Gemma 4
  E4B (target) + E2B (CPU draft) the partial-recurrent
  classification lets the kernel through but the CPU draft is too
  slow on a 16 GB GPU to amortize. Measured 0.46× speedup
  (15.5 tok/s plain vs 7.1 tok/s spec) on the bundled hardware
  profile.

**Validation matrix:**

| # | Criterion | Status |
|---|---|---|
| 1 | SecondaryModelLoader replaces router code | ✅ unchanged |
| 2 | `inference.speculative.enabled: false` shipping default | ✅ |
| 3 | `entropic_speculative_compat` returns false + diagnostic on mismatch | ✅ |
| 4 | Compatible draft + enabled=true → kernel runs, bit-identical on rejection | ⏸ **gated inert** at this pin (no bundled combo qualifies) |
| 5 | Model test gate ≥1.8× speedup | ⏸ **gated inert** (CPU draft too slow + correctness unmet) |
| 6 | CPU-resident draft works without extra VRAM | ✅ kernel runs; output diverges per #4 |
| 7 | GPU-resident draft works | ⏸ untested (VRAM-tight, deferred with #5) |
| 8 | Doxygen `@version 2.1.11` on every new public symbol | ✅ |
| 9 | Pre-commit gates pass | ✅ |
| 10 | `docs/architecture-cpp.md` decision log updated | ✅ entries #38–#41 |

**What ships and why anyway:**

- **Hybrid+recurrent arch guard.** `check_compat` now refuses targets
  classified as `llama_model_is_recurrent` OR `llama_model_is_hybrid`
  via the new `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_ARCH` code.
  This is the conservative subset — additional primaries with partial
  recurrent state (Gemma 4) still pass the classification check; the
  model tests SKIP at this pin to keep CI green.
- **`spec_trim_*` off-by-one fix.** Pre-Session-5 the kernel used
  `seq_rm(n_past + 1, -1)` after each iteration, leaving the first
  rejected-draft slot polluted. Upstream uses `seq_rm(n_past, -1)`.
  Fix lands in this version so a future pin bump can rely on
  correct trim semantics.
- **`entropic_speculative_compat`** is callable today — consumers
  validate a planned pair without booting the kernel.
- **SecondaryModelLoader** is consumer-reachable for the router slot
  and ready to absorb the thinking-model slot (gh#25).
- **Off-by-default** means existing deployments see zero behavior
  change.

**What unblocks the feature in a future patch:**
- A llama.cpp pin where the chunked SSM scan correctly continues
  from the recurrent state of the prior ubatch (this would make
  speculative bit-identical on Qwen3.5/3.6/Gemma 4/Nemotron-H), OR
- A pure-transformer primary added to the bundled registry, OR
- A faster draft path (e.g., GPU draft when VRAM permits, or
  llama.cpp's `DRAFT_EAGLE3` / `NGRAM_*` variants).

## C ABI additions (strictly additive)

- `entropic_speculative_compat(handle, int* compatible, char** diagnostic)`
- `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_ARCH` (error code 52) — returned
  when the target arch is recurrent OR hybrid at the current pin.

## Internal API additions

- `entropic::SecondaryModelLoader`
- `entropic::speculative::check_compat(...)` + `CompatResult`
- `entropic::InferenceBackend::do_generate_speculative(...)`
- `entropic::InferenceBackend::generate_speculative(...)` (public wrapper)
- `entropic::ModelOrchestrator::check_speculative_compat()`
- `entropic::ModelOrchestrator::activate_draft(...)`

## New tests

- `entropic-speculative-compat-tests` (11 cases, 23 assertions) —
  mock-vocab coverage of every compat rule.
- `entropic-secondary-loader-tests` (6 cases, 14 assertions) —
  loader bookkeeping invariants.

## Documentation updates

- `docs/architecture-cpp.md` decision log entries #38 (speculative
  decoupled from router), #39 (entropic-side recurrent gate vs
  upstream non-self-disable), #40 (compat check is metadata-only +
  mirrors upstream `static common_speculative_are_compatible`),
  **#41** (kernel ships gated inert at this pin; Session 5
  forensics localize the structural cause to cross-ubatch SSM
  state continuity in `common_speculative_*` + an entropic-side
  `spec_trim_*` off-by-one fix landed unconditionally).
- `data/bundled_models.yaml`: Gemma 4 / Nemotron 3 sections flag
  that no `mmproj_key` ships for those families at this version
  (only Qwen3.5 primary and Qwen3.6-A3B have bundled mmproj). Image
  inputs to non-Qwen tiers fall through to text-only via the
  existing image-strip path.
- `include/entropic/types/config.h`: `SpeculativeConfig` docstring
  includes a per-bundled-key compatibility matrix so consumers can
  see at a glance which combinations the arch guard refuses.

# entropic v2.1.10

Patch release adding a mid-generation user-message queue — a UX
primitive for long-thinking interactive sessions where a turn can
expand into 30+ tool calls running for minutes. Lands a single issue
(**gh#40**) as the third of four patches bundling to the v2.2.0
milestone. No inference-path, adapter, or vision changes.

## Highlights

- **gh#40 (MEDIUM):** mid-generation user-message queue.
  - New C ABI: `entropic_queue_user_message`,
    `entropic_user_message_queue_depth`,
    `entropic_clear_user_message_queue`, and
    `entropic_set_queue_observer`. New error code
    `ENTROPIC_ERROR_QUEUE_FULL`. All additions are strictly
    additive — no existing symbol changes signature or semantics.
  - Consumers can call `entropic_queue_user_message(handle, text)`
    from any thread while a run is in flight. The queued message
    becomes a fresh next turn the moment the current top-level turn
    reaches `AgentState::COMPLETE`, under the same per-call
    `on_token` callback. Streaming protocol is unchanged — tokens
    fire for the active turn, then for the queued turn, with no
    multiplexing.
  - Bounded FIFO (default cap 8, runtime-configurable via
    `AgentEngine::set_message_queue_capacity`). Past capacity,
    enqueue returns `ENTROPIC_ERROR_QUEUE_FULL`. Calling
    `entropic_queue_user_message` when no run is in flight returns
    `ENTROPIC_ERROR_INVALID_STATE` — the consumer should just call
    `entropic_run_streaming` directly in that case.

## Design decisions (locked at proposal time)

- **Boundary = top-level COMPLETE only.** Queued messages do NOT
  inject at parent-resume-after-child-delegation boundaries. The
  drain hook lives in `AgentEngine::run_turn` after `run()` returns,
  not in the state observer — this structurally guarantees the
  property (child loops invoked through `run_loop` cannot reach the
  drain).
- **No interruption.** The queue does NOT cancel the current turn.
  Consumers wanting that already have `entropic_interrupt`.
- **FIFO append.** Oldest queued message becomes next turn.
  Prioritization, newest-wins, coalescing, and dedup are deferred —
  no v1 consumer demand.
- **Thread-safe via a dedicated queue mutex** that is disjoint from
  the facade's per-handle `api_mutex`. The facade thunks for the
  three new ABI calls do not take `api_mutex`, so consumer threads
  can enqueue while another thread is inside `entropic_run_streaming`.

## Engineering changes

- `LoopConfig` gains `message_queue_capacity` (default 8).
- `AgentEngine` gains a dedicated queue observer slot (persistent
  across `set_callbacks()` reassignments) — modeled on the existing
  `set_stream_observer` pattern so the streaming entry points'
  per-call EngineCallbacks installation does not lose the
  registration.
- Facade adds a `rewire_queue_observer` companion to
  `rewire_stream_observer`, called from `configure_common` so
  pre-configure registrations survive engine construction.

## Out of scope (deferred)

- Boundary policies beyond top-level COMPLETE (parent-resume-
  after-child, per-call boundary policy).
- Queue prioritization / coalescing / dedup.
- Cross-turn message editing or per-message dequeue by id.
- Queue persistence across engine restarts.

## Tests

- `tests/unit/core/message_queue_test.cpp` — queue semantics
  (enqueue / depth / clear / cap), top-level COMPLETE boundary
  property, FIFO drain order, and a thread-safety stress test
  (concurrent enqueues + depth queries).
- `tests/unit/api/message_queue_abi_test.cpp` — C ABI surface
  validation (NULL handle / arg, INVALID_STATE when idle, depth
  no-op on unconfigured handle, pre-configure observer registration,
  `ENTROPIC_ERROR_QUEUE_FULL` name lookup).

## Compatibility

Strictly additive. No ABI break. Existing consumers compile and
run unchanged.

---

# entropic v2.1.9

Patch release expanding the bundled model registry and adding three
new chat-adapter classes — strictly additive, no facade ABI changes.
Bundles four issues as one cohesive registry-and-adapters slice
toward the v2.2.0 milestone: registry expansion (**gh#44**), new
adapter classes for Qwen 3.6 (**gh#45**), Gemma 4 (**gh#46**), and
Nemotron 3 (**gh#47**); partial ratchet of the gh#17 umbrella with
the remaining families (Llama 3.x, Gemma 2/3, Mistral / Mixtral,
Phi-3/4, DeepSeek, Granite, Command-R) deferred to 2.3.x.

> **Nemotron architecture-verification gate (gh#47): PASSES.** The
> Nemotron-3-Nano-4B model is a hybrid Mamba-Transformer with GGUF
> arch tag `nemotron_h`. llama.cpp ships full integration via
> `llm_build_nemotron_h : public llm_build_mamba_base`, so state
> handling is on the stable Mamba path rather than experimental.
> Adapter proceeds; gate documentation lives in
> `nemotron3_adapter.h`.

## Highlights

- **gh#44 (MEDIUM):** `data/bundled_models.yaml` adds 9 model-keyed
  entries using a `<family>_<size_or_variant>` naming convention:
  `qwen3_5_{0_8b,2b,4b,9b}`, `qwen3_6_a3b` (+ paired
  `qwen3_6_a3b_mmproj` for vision), `gemma4_{a4b,e4b,e2b}`,
  `nemotron3_nano_4b`. Tier-role keys (`primary`, `mid`,
  `lightweight`) are preserved for backward compatibility; new
  configs should reference the model-keyed entries directly. Each
  entry carries per-entry comments documenting license footprint
  (Apache-2.0 / Gemma Terms of Use / NVIDIA Open Model License) and
  the quant choice rationale.
- **gh#45 (MEDIUM):** `Qwen36Adapter` (registry key `qwen36`) —
  distinct adapter class for the Qwen 3.6 generation. Implements
  the qwen3_coder XML tool-call format (`<tool_call><function=name>
  <parameter=key>value</parameter></function></tool_call>`),
  `<tool_response>` result wrapping, OpenAI-native content-array for
  multimodal inputs, vision-aware system prompt extension. Kept
  structurally distinct from `Qwen35Adapter` per the "no version
  lumping" rule so future template divergence across the Qwen 3.x
  line lands without churning Qwen 3.5 callers.
- **gh#46 (MEDIUM):** `Gemma4Adapter` (registry key `gemma4`) —
  single adapter covers Gemma 4 26B-A4B, E4B, and E2B variants.
  Uses the GGUF-embedded chat template (`chat_format=""`) and a
  permissive parser: tagged JSON primary, bare-JSON fallback. The
  exact native tool-call syntax is to be refined empirically via
  the new model tests — the adapter header documents the open
  question and the decision criteria.
- **gh#47 (MEDIUM):** `Nemotron3Adapter` (registry key `nemotron3`)
  — first-class support for the Nemotron 3 family. Adapter header
  records the arch-verification outcome (hybrid Mamba-Transformer,
  `nemotron_h` GGUF arch, qwen3_coder XML tool-call format,
  `<think>` reasoning trace via separate special tokens). Tool
  parsing mirrors qwen3_coder; reasoning-trace stripping reuses the
  base-class primitives so no Nemotron-specific override is needed.

## Engineering changes

- **Adapter registry refactored to a dispatch table.** Going from
  two to five registered adapter families pushed
  `create_adapter()` past the knots 3-return ceiling. Lookup is
  now a `static const std::array<AdapterEntry, 4>` of
  `{key, factory}` pairs; the function stays at three returns and
  adding a sixth family is a one-row edit.
- **Tests:** 24+ new unit-test scenarios across
  `qwen36_adapter_test.cpp`, `gemma4_adapter_test.cpp`,
  `nemotron3_adapter_test.cpp` — render round-trip, well-formed +
  malformed tool-call parse, multi-turn with think+tool
  interleaving, reasoning-trace stripping, fallback ordering,
  vision content-parts (Qwen 3.6), vision system-prompt extension,
  and the `chat_format` / `format_tool_result` contracts. Full
  suite stays at 874 passing scenarios.
- **Developer-run model tests:** new
  `test_v219_{qwen36,gemma4,nemotron3}_family.cpp` exercise each
  bundled GGUF end-to-end (smoke generation + tool-call fixture)
  and SKIP cleanly when the model isn't on disk. Shared
  infrastructure in `tests/model/v219_family_test_helpers.h`
  lets a v2.1.9 family test override the default tier to load a
  specific bundled key. Run via `entropic download <key>` then
  `ctest -L model -R v219`.

## New features
- 9 new bundled model registry entries (gh#44)
- `Qwen36Adapter` (gh#45)
- `Gemma4Adapter` (gh#46)
- `Nemotron3Adapter` (gh#47)

## Breaking changes
- None. Registry additions only; adapter keys are new; existing
  tier-role keys (`primary`, `mid`, `lightweight`) and the
  `qwen35` / `generic` adapter keys all behave as before.

## Distribution
- CPU tarball: `entropic-2.1.9-linux-x86_64-cpu.tar.gz` (sha256 in
  companion file)
- CUDA tarball: `entropic-2.1.9-linux-x86_64-cuda.tar.gz` (sha256
  in companion file)
- Python wrapper: `pip install entropic-engine==2.1.9` then
  `entropic install-engine`

## Empirical validation (v2.1.9 model-test closure)

All 35 model tests pass (30 pre-existing + 5 new v2.1.9 family
tests), validated end-to-end against actual GGUFs on GPU:

- `qwen3_6_a3b` loaded; **Qwen 3.6 emits qwen3_coder XML** —
  `<tool_call><function=fs.read><parameter=path>...</parameter>
  </function></tool_call>`. `Qwen36Adapter` parsed it cleanly.
- `gemma4_e2b`, `gemma4_e4b`, `gemma4_a4b` all loaded; **Gemma 4
  emits tagged JSON tool calls** —
  `<tool_call>{"name":"fs.read","arguments":{...}}</tool_call>`.
  The permissive `Gemma4Adapter::parse_tool_calls` handles it via
  base-class `parse_tagged_tool_calls`. The "open question" flagged
  in `gemma4_adapter.h` is resolved: tagged JSON.
- `nemotron3_nano_4b` loaded (hybrid Mamba via `nemotron_h` arch);
  emits `<think>...</think>` reasoning then a qwen3_coder XML call.
  `Nemotron3Adapter` + base-class think-block stripping work as
  designed.

## Build / infrastructure changes bundled with v2.1.9

- **`extern/llama.cpp` bumped** from `7f2cbd9a4` (Mar 19) to
  `253ba110b` (May 14, 732 commits) — adds `LLM_ARCH_GEMMA4` which
  was required to load Gemma 4 GGUFs (arch tag `'gemma4'`). All 30
  pre-existing model tests still pass against the new pin.
- **Root `CMakeLists.txt`** now pre-defaults `CMAKE_CUDA_ARCHITECTURES`
  to `native` before `enable_language(CUDA)`. Previously CMake's
  CUDA module assigned `sm_52` (lowest supported for CUDA 12) when
  the variable wasn't set, and llama.cpp's own native-detection
  skipped its path because the value was already defined. After the
  llama.cpp bump, the new MoE-GEMV kernel could not execute on
  `sm_52` binary against `sm_120` (Blackwell) hardware. Release
  builds (`tasks.py::_build_and_stage`) still override with the
  comprehensive multi-arch list — only the dev/full preset path
  inherits the new `native` default.
- **`inv test --cpu` hardening** — the cpu-only ctest lane now
  passes `-LE model` so a stale model-test registration in
  `build/dev` (e.g., left over from a `--debug` cmake reconfigure)
  can't cause pre-commit to load a real GGUF on the CPU lane and
  hang.

## Known limitations

- **Adapter coverage** for the remaining gh#17 families (Llama 3.x,
  Gemma 2 / 3, Mistral / Mixtral, Phi-3 / Phi-4, DeepSeek,
  Granite, Command-R) is deferred to 2.3.x per the gh#17 triage —
  v2.1.9 is the first ratchet, not the close-out.
- **Gemma 4 A4B requires 24+ GB VRAM for full-context inference.**
  Its KV cache is fundamentally larger per token than Qwen 3.6 A3B
  (8 KV heads × 512 head_dim vs 2 × 256), so the same nominal MoE
  GGUF size hides a ~3× memory profile difference. The v2.1.9
  model-test harness overrides `tier.context_length = 4096` so the
  smoke + tool-call fixtures fit on a 16 GB dev GPU; production
  deployments load the full context via config.
