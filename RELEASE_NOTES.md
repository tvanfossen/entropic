# entropic v2.2.8

Patch release. **Bugfix — `LlamaCppBackend` GPU buffer leak on
backend destruction (gh#58 v2.2.7 follow-up).** Caused
"Failed to reload model with GPU layers" on the next GPU model load
in the same process — after either a handle destroy or any
preceding test/scenario that loaded a GPU model and tore it down.

## Root cause

`InferenceBackend::~InferenceBackend()` was defaulted, and no
concrete backend (`LlamaCppBackend` included) defined its own
destructor. The class holds raw `llama_model*`, `llama_context*`,
and `mtmd_context*` pointers. On backend destruction those pointers
were silently dropped — `llama_model_free` / `llama_free` /
`mtmd_free` never ran. GPU memory stayed reserved by the process
but llama.cpp's CUDA pool lost track of it. The next
`llama_model_load_from_file` with `gpu_layers != 0` failed.

Reproduced locally with the test added at
`tests/unit/api/multi_handle_test.cpp` (tagged `[.gpu][.realmodel]`,
needs a real GGUF). Two handles configure cleanly the first
iteration; the second iteration of the same scenario (Catch2
re-runs the SCENARIO per THEN block) failed on the second handle.
With the fix, both iterations succeed.

This matches the consumer's gh#58 v2.2.7 verification where
`[.multihandle][gpu]` failed at "first" configure: their
`[.singlehandle][gpu]` runs first in the same process, leaks GPU
buffers on test teardown, and the multi-handle suite's first
configure then hits the corrupt CUDA pool.

## Fix

Added `~LlamaCppBackend()` that calls `do_unload()`. Routes through
the existing teardown that frees `mtmd_ctx_`, `ctx_`, and `model_`
in the correct order. No new resource management — just the
destructor that should have been there since v1.8.2.

Calling `do_unload()` from a derived destructor is the correct C++
pattern; calling it from the base destructor would be unsafe (the
derived vtable is already gone by then). Future backends must
follow the same pattern in their own destructors.

## Improved diagnostic

`do_activate()`'s "Failed to reload model with GPU layers" error
now includes the model path, the requested `gpu_layers`, and
points the operator at `llama_ggml.log` for the underlying
llama.cpp/CUDA error. Independent of the destructor fix; useful
for any future GPU activation failure.

## Tests

`tests/unit/api/multi_handle_test.cpp` — new
`[.gpu][.realmodel]` scenario that exercises two GPU handles
across two iterations. Tag dot-prefix excludes it from the default
test run; opt in via `entropic-api-tests "[.gpu]"`. Override the
model paths via `ENTROPIC_TEST_GPU_MODEL` and
`ENTROPIC_TEST_GPU_MODEL_B` env vars (defaults to the
`~/.entropic/models/` paths used during fix validation).

## Still open

- gh#59 — per-handle spdlog logger isolation (v2.3.0 target).

---

# entropic v2.2.7

Patch release. **Bugfix — `cache_type_k`/`cache_type_v` finally wired
to llama.cpp (gh#61).** Documented config fields since v1.8.0 that
were never connected to the inference layer.

## Symptom

Configurations declaring `cache_type_k: q8_0` / `cache_type_v: q8_0`
were silently ignored. The KV cache always ran F16, doubling-to-
quadrupling its VRAM footprint vs. the declared quantization. At
large `context_length`, this caused `llama_init_from_model failed`
in `do_activate()` — the GPU model loads fine but the F16 KV cache
puts total VRAM over the device's capacity.

Reporter case: Qwen3.5-4B-Q8_0 with `context_length: 65536`,
`gpu_layers: -1`, `cache_type_k/v: q8_0` on a 1080 Ti (11 GB). F16
KV at 65k context ≈ 8 GB + ~4.5 GB weights = ~12.5 GB > 11 GB =
OOM. With the fix, q8_0 KV ≈ 2 GB, total ~7 GB, fits.

## Not a regression

Investigation framed gh#61 as a v2.2.x regression vs. v2.2.1. Git
archaeology confirms `src/inference/llama_cpp_backend.cpp` is
byte-identical between v2.2.1 and v2.2.6 (only the license header
changed). The two-phase warm→activate split existed at v2.1.8.
The reporter's recollection that this config previously worked
likely conflates a smaller `context_length` or a different model.

The underlying defect is real regardless: `ModelConfig::cache_type_k`
and `cache_type_v` are public, documented (`include/entropic/
types/config.h:158`), parsed by the YAML loader, and have been
silently dead-code since v1.8.0. Wiring them is the right fix.

## What changed

`src/inference/llama_cpp_backend.cpp::do_activate()` now sets:

```cpp
cparams.type_k = parse_kv_cache_type(config().cache_type_k);
cparams.type_v = parse_kv_cache_type(config().cache_type_v);
```

Supported strings: `f16` (default), `f32`, `bf16`, `q8_0`, `q4_0`.
Unknown values log a warning and fall back to F16.

The `Context created:` log line now reports `type_k`/`type_v` so
the active KV quantization is observable in session logs.

## Tests

No new unit tests — exercising this requires a real GGUF on the
GPU. The fix is validated by the reporter's gh#61 repro (will be
confirmed when the consumer runs v2.2.7 against their probe).

## Still open

- gh#59 — per-handle spdlog logger isolation (v2.3.0 target).

---

# entropic v2.2.6

Patch release. **Bugfix follow-up to gh#58** — fixes the SIGSEGV in
`run()` on handle #1 after handle #2 configures, and fixes a 5-year-
old latent bug where `entropic_last_error()` ignored its handle
parameter and returned a thread-local global. Both bugs surfaced as
"empty `what()`, null `last_error`" in the consumer's v2.2.5
verification of gh#58.

## Root causes

### Use-after-free on inference interface (the SEGV)

`src/inference/interface_factory.cpp` held the C-callback `user_data`
in a **process-global static** `s_ctx`:

```cpp
static InterfaceContext* s_ctx = nullptr;  // "Leaked intentionally"
```

Every `configure_common` call did `delete s_ctx; s_ctx = new ...`,
which silently freed the previous handle's context. The previous
handle's `inference_iface.{backend_data, orchestrator_data,
adapter_data}` then pointed at freed memory. The next `run()` on
handle #1 hit `iface_route()`, dereferenced the dangling
`InterfaceContext*`, read a garbage `orchestrator` pointer, and
SEGV'd inside `ModelOrchestrator::route()`.

**Fix:** `build_orchestrator_interface()` now accepts an
`InterfaceContext** out_context` and the engine handle owns the
context (`engine_handle::inference_iface_ctx`). `entropic_destroy()`
calls the new `destroy_orchestrator_interface()` before freeing the
handle. No process-global state remains.

### `entropic_last_error()` ignored its handle

`src/types/error.cpp` v1.8.0 left a TODO:

```cpp
extern "C" const char* entropic_last_error(entropic_handle_t handle) {
    // TODO(v1.8.4): Look up per-handle error state.
    (void)handle;
    return s_global_error;
}
```

That TODO was never done. Every `handle->last_error = e.what()` in
the facade (14+ sites) wrote to the handle but the public reader
returned a thread-local string the handle never touched.

**Fix:** Moved the implementation to `src/facade/entropic.cpp` where
the private `engine_handle` struct is visible. The reader now copies
`handle->last_error` under `api_mutex` into a thread-local cache and
returns the cache pointer (preserving the v1.8.0 contract: valid
until the same thread's next `entropic_last_error()` call).
`handle == nullptr` still falls back to a thread-local global for
pre-create errors.

## Tests

`tests/unit/api/multi_handle_test.cpp` — two new scenarios:
- `entropic_last_error` returns per-handle state (h1's error
  doesn't leak into h2's reader).
- `h1.run() after h2.configure()` does not segfault and
  `entropic_last_error(h1)` is readable.

The second test reproduced the consumer's SEGV pre-fix and now
passes. Full ensemble-with-real-models still lives in
sassafras-class's `test_multi_handle_probe`.

## Still open (filed separately)

- **Per-handle session logger.** v2.2.5 deduped attached sinks but
  the spdlog logger tree is still process-global, so cpu1/session.log
  receives cpu2's lines when both handles share a process. End-state
  fix is per-handle named logger trees touching ~72 `log::get()`
  sites — out of scope for a patch, targeted at v2.3.0.
- **GPU activation regression** on 1080 Ti with single 3GB model and
  10GB free. Probably a v2.2.4 (gh#57) VRAM-aware tier lifecycle
  side-effect, unrelated to multi-handle.

---

# entropic v2.2.5

Patch release. **Bugfix — N `entropic_handle_t` per process (gh#58).**
Three process-global redirect points were silently clobbering each
other when a consumer instantiated more than one handle. Fixes them,
adds a re-entry guard on the same handle, and ships a two-handle probe
test at the C-API level.

## What changed

Consumer pull (sassafras-class ensemble voting): three distinct model
architectures need to run in one process for anti-hallucination
cross-architecture voting. The orchestrator was already per-handle and
already multi-model on a single GPU (speculative draft + main share
VRAM today). The remaining barrier was three process-globals that
were not guarded against a second handle.

### Fixes

- **Re-entry guard** on `configure_common` — second `entropic_configure*`
  call on an already-configured handle now returns
  `ENTROPIC_ERROR_INVALID_STATE` instead of silently replacing the
  orchestrator/engine/mcp_auth (which left dangling raw pointers on
  any subsystem that captured them during the first configure).

- **spdlog session log dedup** — `setup_session()` now tracks attached
  session.log paths and skips duplicate attach. Two handles in the
  same project_dir previously caused every log line to be written
  twice.

- **ggml log redirect** — `entropic_inference_log_to_file()` first call
  wins. Same-path subsequent calls are idempotent no-ops. Different-path
  subsequent calls log a warning and decline (llama.cpp's
  `llama_log_set` is a process-wide slot — only one redirect can be
  active).

- **External bridge socket** — `ExternalBridge::start()` checks a
  process-wide set of bound socket paths before calling
  `create_listen_socket`. Pre-fix, a second handle whose `project_dir`
  hashed to the same socket path would unlink the live socket out
  from under handle #1. Now the second bridge declines to start;
  consumers needing per-handle external MCP must set distinct
  `external.socket_path` per handle.

### Out of scope (unchanged from issue)

- Cross-handle KV cache sharing.
- Multi-handle batched inference.
- Multi-GPU tensor parallel.

### Tests

`tests/unit/api/multi_handle_test.cpp` — three scenarios:
- Two handles configure independently → both OK
- Re-configure on one handle → `INVALID_STATE`
- Three handles coexist → all OK

Full ensemble-with-real-models coverage requires a GPU and lives in
the consumer's test suite (sassafras-class `test_multi_handle_probe`).

---

# entropic v2.2.4

Patch release. **Bugfix — VRAM-aware tier model lifecycle.** Adds a
small, strictly additive C ABI surface so consumers can observe
residency transitions and a distinct typed error when a single tier's
model alone exceeds the engine's VRAM budget.

## What changed

Engine v2.2.1 framed any aggregate-fit failure during multi-tier
configuration as `ENTROPIC_ERROR_LOAD_FAILED`. The engine's stated
role is to abstract VRAM distribution from consumers — they declare
per-tier model bindings, and the engine decides what to load and when.
The v1.8.2 single-active-tier swap already implements this for the
main slot, but had no observable surface for consumers and no
distinct error code for "this single tier's model alone exceeds total
VRAM, no eviction will help". v2.2.4 fixes that gap.

### New C ABI (strictly additive)

- `ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE` — distinct from
  `ENTROPIC_ERROR_OUT_OF_MEMORY` and `ENTROPIC_ERROR_LOAD_FAILED`.
- `entropic_residency_event_t` — `LOADED` / `EVICTED` /
  `ACTIVATION_SWAP`.
- `entropic_residency_observer_t` — callback typedef receiving event,
  tier name, model path, and tracked footprint bytes.
- `entropic_set_residency_observer(handle, cb, ud)` — observer
  registration.
- `entropic_residency_snapshot(handle, **out_json)` — JSON snapshot
  of the currently-resident set, per-model footprints, the engine's
  VRAM budget, and headroom. Caller frees with
  `entropic_free_string`.

No symbols renamed or removed.

### Engine behavior

- Per-tier VRAM footprint accounting: GGUF weights file size +
  `context_length` × per-token KV estimate + configured
  `vram_reserve_mb` headroom.
- `ModelOrchestrator::get_model` refactored into three small helpers
  (`residency_admits`, `record_activation_reuse`, `activate_and_track`)
  to stay under the project's cognitive-complexity and SLOC gates.
- `ResidencyEvent::Loaded` fires on every fresh activation;
  `ResidencyEvent::Evicted` fires on the unload-path of the existing
  cold swap; `ResidencyEvent::ActivationSwap` fires when an already-
  loaded tier is reactivated. Every event is INFO-logged with full
  state-after (no truncation per project log policy).
- VRAM budget source: `ENTROPIC_VRAM_BUDGET_BYTES` environment
  override. CUDA `cudaMemGetInfo` discovery and Metal/ROCm equivalents
  are intentionally deferred — env var is the supported override
  surface today and what consumer tests inject. When the budget is 0
  (unknown), the gate is disabled and the engine preserves prior
  behavior.
- `configure_dir` remains lazy at the orchestrator level — only the
  `default` tier is activated at init; alternate tiers load on first
  routing/lock through `get_model` (now residency-gated).

### State provider surface

The introspection surface that backs the engine's `inspect` and
`context_inspect` MCP tools gains a new `get_residency` callback
mirroring `entropic_residency_snapshot` JSON exactly. This extends
the `entropic_state_provider_t` interface struct — flagged as an
interface change per the repo's session protocol.

### Files changed

- `include/entropic/types/error.h`,
  `src/types/error.cpp` — new error enum + name-table entry.
- `include/entropic/entropic.h` — new typedef + two exported
  functions (`entropic_set_residency_observer`,
  `entropic_residency_snapshot`).
- `include/entropic/interfaces/i_mcp_server.h` —
  `entropic_state_provider_t.get_residency` (interface change).
- `include/entropic/inference/orchestrator.h`,
  `src/inference/orchestrator.cpp` — residency-set state,
  footprint estimator, VRAM gate, observer hook, snapshot JSON,
  `get_model` refactor into three helpers.
- `src/facade/entropic.cpp` — facade wrappers for the new C ABI
  entries and the new `sp_get_residency` state provider.
- `scripts/gen_bindings.py`,
  `python/src/entropic/_bindings.py`,
  `python/src/entropic/_bindings_manifest.py` — Python ctypes
  wrapper regenerated for the new symbols.
- `tests/unit/inference/residency_test.cpp` — empty-snapshot shape,
  observer registration/clear, last-error default+clear, error-name
  stability, ResidencyEvent enum value pinning vs the C ABI enum.

## Compatibility

- C ABI: additive only. `entropic_error_t` gains a new tail value;
  existing values keep their positions. The new functions, typedef,
  and enum are new symbols; no existing symbol changed signature.
- Python wrapper: regenerated; the new functions appear automatically
  in `entropic._bindings`. Pre-2.2.4 consumers that don't reference
  the new symbols see no behavior change.
- Behavior: when `ENTROPIC_VRAM_BUDGET_BYTES` is unset (the default),
  the residency gate is disabled and the engine behaves identically
  to v2.2.3.

## Consumer impact

`bissell-llm-studio` and similar multi-tier consumers can revert
"Apply propagates to all tiers" workarounds: per-tier model
assignment now works as the engine originally intended, and consumers
can subscribe to residency events to mirror the engine's
currently-loaded set in their UIs without polling.

## Issue

- [gh#57](https://github.com/tvanfossen/entropic/issues/57)

---

# entropic v2.2.3

Patch release. **Bugfix — UTF-8-safe history truncation.** No ABI
changes, no behavioral changes outside the fixed code path.

## What changed

`sp_get_history` (the state-provider entry point that builds the
conversation snapshot consumed by `context_inspect` / `inspect
--target history`) previewed each message with
`m.content.substr(0, 200) + "..."`. Byte-indexed truncation slices
through any multi-byte UTF-8 codepoint whose bytes straddle the
200-byte boundary, producing invalid UTF-8 at the seam. The
subsequent `arr.dump()` (nlohmann/json) raised `type_error.316`,
which bubbled out `run_streaming` as
`ENTROPIC_ERROR_GENERATE_FAILED`. Consumers that did not surface
the rc visibly saw the turn end silently with no model output.

The fix adds `entropic::facade::utf8_safe_substr` in
`src/facade/utf8_safe.h`: walk back over UTF-8 continuation bytes
(`0x80..0xBF`) before the cut so the result is always valid UTF-8
when the input is. `sp_get_history` uses the helper in place of the
raw `substr`.

## Why it matters

The bug fires whenever message history contains non-ASCII content
near the 200-byte preview boundary — most commonly U+FFFD runs
from `docs.db` builds that decoded non-UTF-8 source files with
`errors="replace"`. Filed by `bissell-llm-studio` after three
identical `type_error.316 at index 200: 0x2E` failures in one
session (the `0x2E` is the first `.` of `"..."` appended after the
truncated substr).

## Files changed

- `src/facade/utf8_safe.h` — new internal header exposing
  `entropic::facade::utf8_safe_substr`.
- `src/facade/entropic.cpp` — `sp_get_history` routes preview
  truncation through the new helper.
- `tests/unit/api/utf8_safe_substr_test.cpp` — regression coverage:
  ASCII boundary, gh#56 repro (199 `a` + 5 `é`), 3-byte U+FFFD,
  4-byte U+1F600. The fixed `+ "..."` concatenation is asserted
  parseable by `nlohmann::json`.
- `tests/unit/CMakeLists.txt` — wires the new test into
  `entropic-api-tests`.

## Compatibility

- C ABI: unchanged.
- Python wrapper API: unchanged.
- Behavior change limited to the preview truncation code path in
  `sp_get_history`. Pre-fix valid-UTF-8 inputs (no codepoint
  straddle at byte 200) are unaffected.

No downstream consumer action is required. Consumers that worked
around the bug by scrubbing non-ASCII content from message history
may revert that workaround.

## Issue

- [gh#56](https://github.com/tvanfossen/entropic/issues/56)

---

# entropic v2.2.2

Patch release. **License change only — no code, ABI, or behavioral
changes.** Entropic returns to the **Apache License, Version 2.0**.

## Why

v2.0.0 moved from Apache-2.0 to LGPL-3.0-or-later + a project-named
linking exception in an attempt to solve a problem that, on review,
did not exist for this project's use cases. The linking exception
preserved permissive use for downstream linkers, which is exactly
what Apache-2.0 provides natively — without the LGPL §4d obligations
on engine modifications, without a custom-named exception clause,
and without the ongoing license-compatibility friction Apache avoids
by default.

This is framed as evolving understanding of open-source licensing,
not a reversal of any prior intent. The Apache-2.0 stance used for
v1.x is the right fit for an engine library and is restored here.

## Scope of the relicense

- v2.2.2 and all future releases: **Apache-2.0**.
- v2.0.0 through v2.2.1: remain under LGPL-3.0-or-later with the
  linking exception they originally shipped under. Licenses do not
  apply retroactively to copies already distributed.
- v1.x: was Apache-2.0; unchanged.

## Files changed

- `LICENSE` — replaced with canonical Apache-2.0 text.
- `NOTICE` — reduced to the minimum required: copyright line,
  license reference, and third-party attribution. Linking-exception
  text, employer acknowledgement, and version-history prose removed.
- `pyproject.toml` — `license = "Apache-2.0"`, SPDX header updated.
- `README.md`, `docs/dist-README.md`, `docs/roadmap.md` — license
  sections rewritten.
- `CONTRIBUTING.md`, `AUTHORS` — DCO/copyright wording updated.
- `data/bundled_models.yaml` — license-footprint comment updated.
- ~400 source/test/build files — `SPDX-License-Identifier` headers
  flipped from `LGPL-3.0-or-later` to `Apache-2.0`.

## Compatibility

- C ABI: unchanged.
- Python wrapper API: unchanged.
- On-disk layout, configuration, model registry: unchanged.
- Wheel / sdist contents: identical except for license metadata and
  in-file SPDX headers.

No downstream consumer action is required. Consumers already
permitted by Apache-2.0 (which is strictly more permissive than
LGPL+linking-exception for the linking case) continue to be
permitted.
