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
