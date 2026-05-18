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
