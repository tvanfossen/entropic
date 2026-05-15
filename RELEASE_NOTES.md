# entropic v2.2.1

Patch release. Closes the pip-wrapper coverage gap surfaced after the
v2.2.0 cut and ships defense-in-depth that prevents recurrence. The
v2.2.0 C ABI was correct; the wrapper-sync miss was a Python-layer
gap that left 9 newly-added symbols + 2 error codes unreachable to
PyPI consumers. **Strictly additive at both the C ABI and the
wrapper API** — no symbol removed, no signature changed.

## Highlights

**Full auto-generation of the Python bindings (gh#54, gh#55).**
The hand-curated `_bindings.py` was the recurring source of drift
across v2.1.10, v2.1.11, and v2.1.12 — each patch added new
`ENTROPIC_EXPORT` symbols without the corresponding Python wrapper
update, and v2.2.0 shipped with 3 of those symbols still unbound.
v2.2.1 replaces the partial regex generator with a full-fidelity
bracket-aware C declaration parser (`scripts/gen_bindings.py`) that
covers every header-declared export by construction. The previous
"hand-curated subset, ~50 KB wheel" philosophy is retired; the
generator output is the wrapper.

**New pre-commit hook `gen-bindings-check` (gh#55).** Re-runs the
generator and diffs against the committed output; drift exits 1
with a precise diagnostic. The hook is exercised by a unit-level
self-test that synthesizes a fake `ENTROPIC_EXPORT` and asserts the
generator emits a binding for it, so the defense-in-depth itself
stays honest as the project evolves.

## New (now-reachable) wrapper bindings

The C ABI for these shipped in v2.2.0; v2.2.1 adds the Python
wrapper coverage:

- **Mid-generation message queue (v2.1.10, gh#40):**
  `entropic_queue_user_message`, `entropic_user_message_queue_depth`,
  `entropic_clear_user_message_queue`, `entropic_set_queue_observer`,
  `QUEUE_OBSERVER_CB`, `ENTROPIC_ERROR_QUEUE_FULL`.
- **Critique-visibility callbacks (v2.1.12, gh#50):**
  `entropic_set_critique_callbacks`, `CRITIQUE_START_CB`,
  `CRITIQUE_END_CB`.
- **Speculative-decoding compat probe (v2.1.11, gh#36):**
  `entropic_speculative_compat`,
  `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_ARCH`.

Plus mechanical coverage of every other `ENTROPIC_EXPORT` previously
unreachable through the wrapper (grammar registry, adapter swap,
identity management, MCP key auth, audit, profile registry, logprob
APIs, throughput metrics, compactor registration, etc.). Wheel
binding count: 86 (was 25).

## C ABI compatibility

**Unchanged.** Zero C/C++ source modifications in this patch. SONAME
stays at 2. Consumers built against 2.2.0 dynamically link to 2.2.1
without recompilation.

## Wrapper API compatibility

**Strictly additive.** Every pre-2.2.1 symbol name, signature, and
import path is preserved. The internal `_bindings.py` file becomes
auto-generated and now lives alongside an auto-generated
`_bindings_manifest.py`; `__init__.py` imports the lazy-export
frozenset from the manifest instead of carrying a hand-curated
literal. Consumer imports of the form `from entropic import X`
continue to work unchanged.

The two gh#22 backward-compat CFUNCTYPE aliases (`HOOK_CALLBACK_CB`,
`TOKEN_STREAM_CB`) are emitted by the generator alongside the
canonical names.

## Release-process improvements

`docs/releasing.md` gains three items capturing v2.2.0 lessons:
- Re-run model tests AFTER the VERSION rebuild (test #683
  `version-match` fails on stale dev builds when run via `--no-build`).
- Close referenced issues AFTER the release ships, so closure
  comments can name the live release URL and the PyPI artifact.
- Verify the pip wrapper covers every new `ENTROPIC_EXPORT` before
  publish. From v2.2.1 onward this is mechanical via the new
  pre-commit drift check; the human-readable item stays as
  belt-and-suspenders.

## Distribution

- CPU tarball: `entropic-2.2.1-linux-x86_64-cpu.tar.gz` (sha256 in companion file)
- CUDA tarball: `entropic-2.2.1-linux-x86_64-cuda.tar.gz` (sha256 in companion file)
- Python wrapper: `pip install entropic-engine==2.2.1` then `entropic install-engine`

After upgrading, bissell-llm-studio (and any other consumer) can
adopt the mid-gen queue (gh#40) and critique callbacks (gh#50) on a
pip-installed wrapper without ctypes-bypassing the wrapper.

---

# entropic v2.2.0

Minor release. The v2.1.x bundle (v2.1.7 → v2.1.12) lands together at
this tag. **Strictly additive at the C ABI** — no removals, no behavioural
changes for existing 2.1.x consumers. Six patches under one milestone:

| Patch | Theme |
|---|---|
| v2.1.7 | `mcp-bridge` becomes a pure relay (VRAM-orphan fix) |
| v2.1.8 | Multimodal end-to-end (facade ABI + tier routing + bundled VLM + mtmd integration) |
| v2.1.9 | Bundled-model registry expansion + 3 new chat adapter families |
| v2.1.10 | Mid-generation user-message queue ABI |
| v2.1.11 | Speculative decoding infrastructure + SecondaryModelLoader (kernel gated inert — see Known Limitations) |
| v2.1.12 | Consumer-reported bug fixes + Gemma 4 mmproj registry closure + critique-visibility callbacks |

## Headline features

**Multimodal end-to-end (v2.1.8).** `entropic_run_messages` accepts
content_parts with text + image; the orchestrator routes image-bearing
messages to a vision-capable tier automatically (via the new
`capabilities:` config field). The bundled primary ships an mmproj.
The OpenAI server example handles `data:` URLs, `file://`, and
absolute paths. `http(s)://` returns HTTP 400 (SSRF surface) by
design. The text-only fast path (`entropic_run`) is preserved
unchanged.

**3 new chat adapters + 11-key registry (v2.1.9).** Qwen 3.6
(`Qwen36Adapter`), Gemma 4 (`Gemma4Adapter` covering A4B / E4B /
E2B), and Nemotron 3 (`Nemotron3Adapter`) join the existing
`Qwen35Adapter`. Registry uses the `<family>_<size_or_variant>`
convention; tier role assignment is purely a config concern, so any
registry key can serve as primary / mid / draft / router. The
bundled-model fetch tooling (`entropic download <key>`) handles
mmproj pairing where applicable. License compatibility documented
per entry.

**Mid-generation user-message queue (v2.1.10).** New ABI lets
interactive consumers enqueue follow-up messages mid-turn without
interrupting the active generation. Queued messages drain at
top-level `AgentState::COMPLETE` only — never at
parent-resume-after-child boundaries, which is enforced
structurally by the drain hook's location inside `run_turn`. Cap
default 8, runtime-tunable. Token-stream protocol unchanged.

**Context-pressure API (v2.1.8).**
`entropic_context_usage(handle, *used, *capacity)` exposes the
same numbers `core.context_manager` logs (`Context: N/M tokens`).
Two scalars, cheap to poll at 1Hz for a UI gauge. No JSON, no heap
allocation.

**Critique-visibility callbacks (v2.1.12).**
`entropic_set_critique_callbacks(handle, start_cb, end_cb, ud)`
fires before and after the constitutional validator's critique
generation — the 20-30s "validating response…" window that
previously showed `AgentState::EXECUTING` with no consumer-visible
signal. Persistent-slot pattern (survives `set_callbacks()`
shuffles, matches the v2.1.10 state-observer precedent).

**Speculative decoding infrastructure (v2.1.11).** `inference.speculative.*` config schema, `SecondaryModelLoader` unifying router /
draft / future thinking-model lifecycle, `LlamaCppBackend::do_generate_speculative` wired to `common_speculative_*`,
capability query `entropic_speculative_compat`, acceptance-rate
metrics, and a new `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_ARCH`
guard that refuses recurrent OR hybrid targets at
config-activation time. **Kernel gated inert** — see Known
Limitations.

**Storage correctness fix (v2.1.12 gh#48).** Pre-v2.1.12 the root
`LoopContext` defaulted to an empty `conversation_id`. Every
delegation copied the empty string into `parent_conversation_id`
and the resulting INSERT FK-failed silently against
`conversations(id)`. Net effect: `entropic.followup` returned the
"no matches" sentinel for every session regardless of how many
delegations had run. The `delegations` table stayed empty across
entire sessions. Fixed at engine init; defense-in-depth in the
storage backend.

## Known limitations

**Speculative decoding kernel is gated inert.** The infrastructure
ships and the arch guard refuses every unsafe combo cleanly, but
the bit-identical correctness contract
(`test_speculative_correctness`) cannot be met at llama.cpp pin
`253ba110b` for any bundled primary. Catastrophic logit divergence
at the speculative split-prefill boundary on Qwen3.5, Qwen3.6,
Nemotron-3, AND Gemma 4. Root cause: upstream
`speculative-simple.cpp`'s split-prefill assumes pure-transformer
state continuity across ubatch boundaries; every bundled primary
carries recurrent state that doesn't carry across. Most modern
frontier model families released in 2025–2026 are hybrid and
therefore refused by the arch guard. Path forward (post-2.2.0):
upstream fix, entropic-side state-management shim, or relaxation
to statistical-equivalence test. Architecture decision log #41
captures the full forensics.

**Empirical demo numbers (Gemma 4 E4B target + E2B CPU draft,
122-token prompt → 600 tokens):** plain decode 15.5 tok/s,
speculative 7.1 tok/s = 0.46× (slower) despite `accept_rate=0.99`.
CPU draft is too slow on the dev hardware to amortize. Both-on-GPU
was not yet tried at the v2.1.11 freeze and may change the
calculus; tracked as a v2.2.x followup.

**Nemotron-3-Nano-4B is text-only at this pin.** llama.cpp at
`253ba110b` only exposes `PROJECTOR_TYPE_NEMOTRON_V2_VL` (for the
v2 Nemotron-Nano family). No v3 projector upstream. Documented in
`data/bundled_models.yaml`.

**Gemma 4 audio capability is wired but not surfaced.** llama.cpp
ships `PROJECTOR_TYPE_GEMMA4A` + `mtmd_audio_preprocessor_gemma4a`;
mmproj is bundled. The tier `capabilities:` field currently only
defines `text` and `vision`. Audio surfacing is a v2.2.x followup
when a consumer pulls on it.

**Adapter coverage is partial.** 4 of ~13 families land here:
Qwen3.5 / Qwen3.6 / Gemma 4 / Nemotron 3. Llama 3.x, Gemma 2/3,
Mistral / Mixtral, Phi-3/4, DeepSeek, Granite, Command-R remain
deferred to 2.3.x (umbrella gh#17). Note that some of these
(Granite-Hybrid, Phi-MoE variants) are classified hybrid at the
current llama.cpp pin and will be refused by v2.1.11's speculative
arch guard regardless of adapter availability.

## C ABI compatibility

**Strictly additive.** ABI SONAME unchanged at `2`. Consumers
compiled against 2.1.x dynamically link to 2.2.0 without
recompilation. No public symbol removed or repurposed; no error
code renumbered; no config key removed.

### New C ABI symbols

- `entropic_run_messages(handle, messages_json, *out_result)` (v2.1.8)
- `entropic_run_messages_streaming(handle, messages_json, cb, ud, *out_result)` (v2.1.8)
- `entropic_context_usage(handle, *used, *capacity)` (v2.1.8)
- `entropic_queue_user_message(handle, message)` (v2.1.10)
- `entropic_user_message_queue_depth(handle, *count)` (v2.1.10)
- `entropic_clear_user_message_queue(handle)` (v2.1.10)
- `entropic_set_queue_observer(handle, cb, ud)` (v2.1.10)
- `entropic_speculative_compat(handle, *compatible, **diagnostic)` (v2.1.11)
- `entropic_set_critique_callbacks(handle, start_cb, end_cb, ud)` (v2.1.12)

### New error codes

- `ENTROPIC_ERROR_NO_VISION_TIER` (v2.1.8)
- `ENTROPIC_ERROR_QUEUE_FULL` (v2.1.10)
- `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_ARCH` (v2.1.11)

### New config schema (additive, safe defaults)

- `tiers.<name>.capabilities: [text, vision, ...]` — defaults to
  `[text]` when absent (preserves all existing tier configs)
- `tiers.<name>.mmproj` / `tiers.<name>.mmproj_key` — defaults to
  unset (text-only behaviour unchanged)
- `inference.speculative.*` section — `enabled` defaults to `false`
  (no behavioural change when absent or off)
- `loop.message_queue_capacity` — defaults to 8

### Persistent-slot observer pattern

Three observer slots now live on `AgentEngine` independent of
`EngineCallbacks`:

- `set_stream_observer` (pre-existing)
- `set_queue_observer` (v2.1.10)
- `set_state_observer` (v2.1.10; replaces the buggy
  `EngineCallbacks` route on streaming paths)
- `set_critique_callbacks` (v2.1.12; on `ConstitutionalValidator`,
  same pattern)

**Consumer note:** if you previously registered an observer via
`entropic_set_callbacks()` and saw it stop firing on streaming
runs in 2.1.7, that was the bug v2.1.10 fixed via the persistent-
slot migration. New observers added in your own code should follow
the persistent-slot pattern; `set_callbacks()` is documented as a
footgun for any persistent-style observer.

## Bundled model registry

14 keys total. Naming convention `<family>_<size_or_variant>`.
Tier role assignment is config-level.

| Key | Family | mmproj | Notes |
|---|---|---|---|
| `primary` / `primary_mmproj` | Qwen3.5-35B-A3B + F16 mmproj | ✅ | The v2.1.8 vision-capable default |
| `qwen3_5_0_8b` / `_2b` / `_4b` / `_9b` | Qwen 3.5 family | — | New v2.1.9 |
| `qwen3_6_a3b` + `qwen3_6_a3b_mmproj` | Qwen 3.6 | ✅ | New v2.1.9 |
| `gemma4_e2b` / `_e4b` / `_a4b` + paired mmproj | Gemma 4 | ✅ | New v2.1.9 + v2.1.12 mmproj |
| `nemotron3_nano_4b` | Nemotron 3 | ❌ | Text-only at this pin |
| (router) `qwen3_0_6b` | Qwen 3 | — | Existing |

## Per-patch reference

For per-issue commit detail and the implementation logs, see the
per-patch entries below in this file (v2.1.12 → v2.1.7) and the
STAGED proposals at `.claude/proposals/STAGED/v2.1.7-*` through
`v2.1.12-*`.

## Migration notes

**For consumers compiled against 2.1.x:** dynamic-link to 2.2.0
without recompilation. No source changes required.

**For consumers wanting new features:** add the relevant new
symbols (`entropic_run_messages`, `entropic_context_usage`, etc.)
to your bindings. Existing function signatures are unchanged.

**For consumers running observers:** if you have an observer
registered via the legacy `entropic_set_callbacks()` route, verify
it still fires on streaming paths. The persistent-slot setters
(`set_stream_observer`, `set_queue_observer`, `set_state_observer`,
`set_critique_callbacks`) are the recommended path going forward.

**For consumers configuring tiers:** existing tier configs
continue to work unchanged. Adding `capabilities:` is optional;
default is `[text]`.

## Model-test results

Full model/benchmark suite results attached to this GitHub Release
as `model-results-v2.2.0.json`. Per the project's test-gating
policy, this is the audit record gating the minor bump. Generated
on the maintainer's dev hardware (NVIDIA RTX PRO 4000 Blackwell,
16 GB VRAM); consumers with different hardware will see different
absolute throughput but the same correctness pass/fail matrix.

## Acknowledgements

Three of the six patches are direct responses to consumer-reported
issues. Special thanks to the bissell-llm-studio team for the
detailed root-cause analyses on gh#48, gh#49, and gh#50.

---

# entropic v2.1.12

Patch release closing the gap between v2.1.11 and the v2.2.0 milestone
tag. Four consumer-feedback-and-registry items land together as one
patch (gh#48, gh#49, gh#50, gh#51). gh#48 / gh#49 / gh#50 were
reported by bissell-llm-studio in a single 2026-05-13 session.

After this patch the v2.1.x bundle is complete; v2.2.0 is a
planner-driven milestone tag (release-notes consolidation +
model-results JSON, no new code).

## Highlights

- **gh#48 (BUG — silent storage data loss):** Root `LoopContext`
  now creates a conversation row at `AgentEngine::run` init when
  storage is wired. Pre-v2.1.12 the root context defaulted to an
  empty `conversation_id`; every delegation copied that empty string
  into `parent_conversation_id` and the resulting INSERT FK-failed
  silently against `conversations(id)`. The unconditional "Created
  delegation" info log masked the failure, and `entropic.followup`
  returned the 31-char "no matches" sentinel for every session
  regardless of how many delegations had run. Primary fix at the
  engine init point; defense-in-depth on the storage backend
  (empty-parent guard + log-only-on-ok + paired error log on FK
  failure).
- **gh#49 (BUG — streaming interrupt regression):** The
  `entropic_interrupt()` -> per-token cancel-flag chain on develop
  is correctly wired (verified by an end-to-end regression test
  added in this patch). A diagnostic log line now fires the first
  time the per-token poll observes the engine flag ("Stream
  interrupt observed at token N; raising backend cancel_flag"), so
  any future "interrupt didn't propagate" report has a consumer-
  side confirmation channel. The v2.1.7 repro that motivated the
  issue did not reproduce on current develop directly — most
  likely incidentally fixed by the v2.1.10 EngineCallbacks-shuffle
  fix (gh#40 fallout); the diagnostic + regression test ensure any
  future regression is observable and caught.
- **gh#50 (FEATURE — critique visibility):** New
  `entropic_set_critique_callbacks(handle, start_cb, end_cb, ud)`
  C ABI surfaces the 20-30s constitutional-critique generation
  window to consumer UIs. Each callback takes only `void* user_data`
  — consumers correlate timing themselves to drive a "validating
  response (Ns)…" indicator. Persistent-slot pattern matches the
  v2.1.10 state observer precedent (slot lives on the handle and
  on `ConstitutionalValidator`, survives every `set_callbacks()`
  shuffle and configure-reconstruction).
- **gh#51 (REGISTRY — Gemma 4 mmproj closure):** All three Gemma 4
  variants (E2B / E4B / 26B-A4B) now ship their matching
  `mmproj-F16.gguf` entries plus `mmproj_key:` populated on each
  base. llama.cpp at the current pin has full Gemma 4 projector
  support (`PROJECTOR_TYPE_GEMMA4V` for vision,
  `PROJECTOR_TYPE_GEMMA4A` for audio); vision is wired through the
  orchestrator's mmproj routing path (v2.1.8 gh#41) automatically.
  Audio capability surfacing in tier `capabilities:` is flagged as
  a follow-up. Nemotron-3 is now documented explicitly as
  text-only-at-this-pin — no `PROJECTOR_TYPE_NEMOTRON_V3_*` exists
  upstream.

## C ABI additions (strictly additive)

- `entropic_set_critique_callbacks(handle, start_cb, end_cb, user_data)`

## Internal API additions

- `StorageInterface::create_conversation` (engine_types.h) — required
  for gh#48; orchestrator-side trampoline `si_create_conversation`
  in the facade.
- `ConstitutionalValidator::set_critique_callbacks(start, end, ud)`
  + `CritiqueCallbacks` struct + `critique_cbs_mutex_`.

## New tests

- `engine_test.cpp`: "Root conversation created at run() init when
  storage wired (gh#48)" + "Streaming interrupt propagates to backend
  cancel chain (gh#49)".
- `storage/backend_test.cpp`: "create_delegation refuses empty
  parent (gh#48)".
- `constitutional_validator_test.cpp`: "Critique start/end callbacks
  fire around the generate call (gh#50)".

All tests run under pre-commit (CPU lane). No new model tests.

## Compatibility

- Strictly additive C ABI: existing consumers see no behavior change.
- `entropic_set_critique_callbacks` is OFF by default (slot is null
  until the consumer registers callbacks).
- gh#48 fix changes only the silent-storage-failure path; consumers
  that pre-created conversations manually were never affected.
- Registry additions in #51 only fetch when a Gemma 4 tier is
  configured with `mmproj_key:` resolved (existing fetch tooling
  picks them up automatically).

## Documentation updates

- `.claude/proposals/STAGED/v2.1.12-consumer-fixes-and-mmproj.md`
  — implementation log per-issue.
- `data/bundled_models.yaml`: Gemma 4 multimodal-coverage-gap
  comment replaced with the closure note; Nemotron-3 text-only-at-
  this-pin note expanded.

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
