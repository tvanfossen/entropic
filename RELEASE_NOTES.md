# entropic v2.1.11

Patch release introducing **SecondaryModelLoader (gh#27)** and the
**speculative-decoding infrastructure (gh#36)**. Closes out the four-
patch sequence (v2.1.8 multimodal → v2.1.9 registry → v2.1.10 mid-gen
queue → v2.1.11 speculative) bundled to the v2.2.0 milestone tag.

The speculative-decoding *kernel itself* is staged for a follow-up
developer session with GPU validation — see "Speculative kernel
deferral" below. All v2.1.11-listed infrastructure (compat check,
draft slot, config schema, C ABI, recurrent gate, orchestrator
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

## Speculative kernel deferral (load-bearing context)

The v2.1.11 proposal calls out "output distribution bit-identical to
plain decode on rejection cases" as the speculative correctness
contract. Validating that contract requires GPU runs against real
model pairs (the proposal's model-test gate at ≥1.8× speedup on >500
token generations).

Two additional implementation challenges surfaced during the v2.1.11
verification gates that the proposal had not anticipated:

1. **API shift at the v2.1.11 llama.cpp pin.** When v2.1.9 bumped
   the submodule from `7f2cbd9a4` → `253ba110b`,
   `common_speculative_is_compat` moved from a public symbol to a
   file-private `static common_speculative_are_compatible` inside
   `common/speculative.cpp` — the new shape *throws* from the
   draft-simple ctor on incompatible vocabs rather than returning a
   boolean. The proposal's pseudocode against the older surface no
   longer compiles. **Action:** mirrored the upstream rules inside
   entropic (`speculative_compat.cpp`) — metadata-only, unit-
   testable, plus an entropic-side recurrent-target gate that
   upstream does not provide.
2. **Sampler-type bridge.** The new
   `common_sampler_sample_and_accept_n` operates on
   `common_sampler*`; entropic's existing decode path uses
   `llama_sampler*` (lower-level). The kernel needs either a sampler
   bridge or a reimplementation of accept-N against
   `llama_sampler_*` primitives.

The infrastructure landed here is independently useful even before
the kernel:

- **`entropic_speculative_compat`** is callable today — consumers
  validate a planned pair (Qwen3.5-Small + Qwen3.6-A3B, etc.)
  without booting the kernel.
- **SecondaryModelLoader** is consumer-reachable for the router slot
  and ready to absorb the thinking-model slot (gh#25).
- **Off-by-default** means existing deployments see zero behavior
  change.

The next session takes the kernel from `NOT_SUPPORTED` stub to
working `common_speculative_*`-driven generation against the actual
v2.1.11 pin, with the model-test gate as the binding correctness
check.

## C ABI additions (strictly additive)

- `entropic_speculative_compat(handle, int* compatible, char** diagnostic)`

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
  mirrors upstream `static common_speculative_are_compatible`).

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
