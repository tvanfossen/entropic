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
