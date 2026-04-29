---
id: P1-20260429-002
title: "Requirements traceability via doxygen-guard impact reporting"
priority: 1
status: BACKLOG
created: 2026-04-29
updated: 2026-04-29
version_target: TBD (v2.2 candidate; Phase 1 landable on develop independently)
author: "@architect"
related: []
---

## Summary

Introduce a formal requirements catalog at `docs/requirements.yaml` and
wire it into doxygen-guard's existing impact-reporting pipeline so
every commit produces a precise, machine-readable changelog of which
requirements were touched. Existing functions and Catch2 BDD
SCENARIOs gain `@req REQ-NNNN` / `[req:REQ-NNNN]` tags. Acceptance
criteria for each requirement are stored as a literal block in the
YAML, which for most entries will be a verbatim mirror of the
verifying SCENARIO's Given/When/Then text (script-extractable),
falling back to free-form for non-BDD-verified requirements (build
gates, config invariants, release pipeline assertions).

## Motivation

The doxygen-guard hook has been emitting `WARNING: Requirements file
not found: docs/requirements.yaml` on every commit through v2.1.3.
The hook's impact-reporting machinery is fully built and ready to
produce per-commit changelogs at `docs/generated/impact/{impact.json,
impact.md}` — but without a requirements catalog, every report reads
"No requirements affected." We're paying for the machinery without
collecting its output.

Real consequences of the gap:

1. **No precise per-commit changelog.** Today a reader has to
   eyeball `git log` + commit-message prose to understand what
   shipped. Six bugs were closed across three releases this week and
   the only structured cross-reference is the GitHub issue numbers
   in commit footers. With requirements tagged, `impact.md` would
   list "this commit affects REQ-MCP-007 (POST_TOOL_CALL hook output
   contract), REQ-PYBIND-002 (AgentState IntEnum ABI conformance)"
   etc, with one line per touched requirement and links back to
   their `acceptance_criteria`.
2. **No formal verification matrix.** "Tests are specifications"
   (CLAUDE.md) but the specifications are scattered across ~770
   SCENARIOs with no canonical index. A future feature touching
   "the hook contract" can't easily list what behaviour is currently
   guaranteed.
3. **No BDD-to-requirement bidirectional check.** A SCENARIO can
   exist without a corresponding requirement (test for behaviour
   nobody documented), and a requirement can lack any test (claim
   without verification). doxygen-guard supports detecting both
   sides once the catalog and tags exist.
4. **Onboarding cost.** A new contributor (or future-Claude) reading
   the codebase has no top-down map. They start from a file and
   work outward. With requirements documented, they start from
   "what does the system claim to do?" and trace into the code.

## Schema (matching doxygen-guard's existing contract)

doxygen-guard's `impact.requirements` config already defines the
schema; we adopt it verbatim:

```yaml
# docs/requirements.yaml
- id: REQ-NNNN | REQ-<MODULE>-NNN | REQ-PROD-NNN
  name: Short human-readable title (≤ 80 chars)
  module: Functional group (matches a doxygen-guard module name)
  description: |
    Multi-line prose: what the system must do. The "why" lives here
    if non-obvious (constraint, deadline, incident driver).
  acceptance_criteria: |
    Multi-line literal block. For BDD-verified requirements, the
    verbatim Given/When/Then text from the corresponding Catch2
    SCENARIO. For non-BDD-verified requirements (build gates, config
    invariants, release pipeline), free-form imperative description
    of what the verifying check asserts.
  version: X.Y.Z   # Version this requirement was introduced
```

ID conventions:

- `REQ-NNNN` — Software-level requirement (general engine behaviour)
- `REQ-<MODULE>-NNN` — Module-scoped (e.g. `REQ-MCP-007`,
  `REQ-CTX-003`, `REQ-VAL-012`). Modules track architectural
  boundaries — `MCP`, `CTX`, `VAL`, `PYBIND`, `BRIDGE`,
  `BUILD`, `RELEASE`, `DOCS`, `HOOK`, `INFER`.
- `REQ-PROD-NNN` — Product-level requirement (system promises a user
  observes externally — e.g. `pip install entropic-engine` works,
  CLI accepts these arguments, etc.)

`module` field carries the same name in long-form (e.g.
`MCP Server Integration`, `Context Management`).

## Tagging Convention

**Doxygen comments on functions / classes:**

```c
/**
 * @brief Replace invalid UTF-8 byte sequences with U+FFFD.
 * @req REQ-MCP-014
 * @req REQ-INFER-006
 * @version 2.1.3
 */
ENTROPIC_EXPORT std::string sanitize_utf8(std::string_view input);
```

A function may carry multiple `@req` tags when it implements multiple
requirements. doxygen-guard's parser already handles this.

**Catch2 SCENARIOs:**

```cpp
SCENARIO("Async ask emits notifications/progress",
         "[external_bridge][regression][2.1.2][issue-4][req:REQ-BRIDGE-008]")
{ ... }
```

Tag added alongside the existing tag set; doxygen-guard cross-checks
by parsing tag strings for the `req:` prefix.

## Phased rollout

### Phase 1 — Catalog scaffolding (landable on develop now)

- Create empty `docs/requirements.yaml` with a header comment
  explaining the schema and conventions.
- Create `docs/doxygen-guard.yaml` (or extend existing config) wiring
  the impact pipeline:
  ```yaml
  impact:
    requirements:
      file: docs/requirements.yaml
      format: yaml
      id_column: id
      name_column: name
    output:
      format: markdown
      file: docs/generated/impact/impact.md
  ```
- Document the `@req` / `[req:...]` tag conventions in
  `docs/contributing.md`.
- Verify `inv test` and pre-commit still pass with the empty
  catalog (no behaviour change yet).

Phase 1 is non-disruptive and can land in a single commit. After it,
the impact reports start generating into `docs/generated/impact/` —
empty until Phase 2/3 begins tagging.

### Phase 2 — Bulk script-extraction from existing BDD tests

Write a one-shot script `scripts/extract_requirements.py` that:

1. Walks `tests/unit/` and `tests/regression/` for Catch2 source
   files.
2. Parses every `SCENARIO("title", "[tags]")` block.
3. For each: emits a candidate `requirements.yaml` entry with:
   - `id`: synthesized from the file path + scenario index
     (e.g. `REQ-EXTRACT-0042` placeholder for human curation)
   - `name`: the SCENARIO title
   - `module`: derived from the test file's directory
     (`tests/unit/mcp/` → `MCP`)
   - `description`: TODO marker for human prose
   - `acceptance_criteria`: verbatim text of the GIVEN/WHEN/THEN
     block (the literal-block mirror)
   - `version`: extracted from existing `[2.1.X]` tags or `unknown`

4. Emits the candidate entries to a separate
   `docs/requirements-candidates.yaml` for human review. Curation
   merges into the canonical `requirements.yaml` after IDs and
   modules are normalised.

This produces ~150-250 candidate entries from the ~770 existing
SCENARIOs (multiple THENs typically map to one requirement). Each
candidate already has its acceptance_criteria populated — humans
only need to assign canonical IDs and write the prose description.

### Phase 3 — Tag the C-ABI public surface

Smallest-surface, highest-leverage tagging target. The C ABI is
~80 functions in `include/entropic/entropic.h` and supporting type
headers. Each gets one or more `@req` tags pointing at the
requirements that justify its existence. This is pure annotation —
zero behaviour change.

### Phase 4 — Tag BDD SCENARIOs

Add `[req:REQ-NNNN]` tags to every Catch2 SCENARIO. The
script-extraction in Phase 2 already correlated SCENARIO ↔
candidate-requirement, so this is mechanical: write the tag matching
the curated ID.

After Phase 4, doxygen-guard's CI gate can be tightened:

- Every requirement must have ≥ 1 SCENARIO tagged with its ID
- Every SCENARIO with `[req:...]` must reference an existing
  catalog entry
- Every public function must carry ≥ 1 `@req` tag
- (optional) BDD acceptance_criteria text in the YAML must equal the
  SCENARIO's Given/When/Then text verbatim — drift detector

### Phase 5 — Tag interior code rolling sweep

Lower-priority. Interior helpers and private methods get `@req` tags
as a rolling effort during normal feature work. Not blocking; the
public surface (Phase 3) gives the bulk of the impact-report value.

## Constraints

- **No new tooling.** doxygen-guard already implements all the
  parsing, validation, and report generation. We only write the
  catalog content and add tags.
- **Sequence-diagram tracing is explicitly OUT OF SCOPE.**
  doxygen-guard's `trace` mode isn't reliable enough yet (per
  user feedback 2026-04-29). The impact-reporting subsystem is a
  separate code path and works.
- **Catalog is the source of truth.** When schema and code disagree,
  the catalog wins; tests / code update to match.

## Dependencies

- doxygen-guard hook (already in `.pre-commit-config.yaml`)
- No external deps; pure YAML + tags

## Open Questions

1. **`acceptance_criteria` drift detection.** Should CI enforce that
   the YAML's `acceptance_criteria` block exactly equals the
   referenced SCENARIO's Given/When/Then text? Pro: ironclad
   single-source guarantee. Con: requires a parser that handles
   Catch2 macros and YAML indentation precisely; whitespace
   variations cause false positives. Lean toward "Phase 4
   experiment" — try it, accept if it stays green for a release
   cycle, otherwise downgrade to advisory warning.
2. **Granularity of module names.** `MCP` covers ~5 sub-areas
   (server manager, tool executor, hooks, external bridge,
   permissions). Single module label or sub-modules? Lean
   sub-modules: `MCP-EXEC`, `MCP-HOOK`, `MCP-BRIDGE`. Costs more
   structure to maintain but precisely-scoped impact reports are
   the whole point.
3. **Pre-existing v2.1.x bug fixes — backfill or forward-only?**
   Backfilling adds ~12 requirements (the six bugs × ~2
   acceptance criteria each). Forward-only starts the catalog
   from "next commit ships". Lean backfill — the recent bugs are
   exactly the kind of "the system shall NOT regress this" entry
   that benefits most from being in the catalog explicitly.
4. **Visibility of the impact report.** Just commit
   `docs/generated/impact/` per-commit, or also surface in
   `docs/roadmap.md` per-release? Latter gives a structured
   release changelog at the cost of needing a release-time
   aggregation step.

## Verification

For Phase 1 (the part landable on develop now):

1. `docs/requirements.yaml` exists and parses as valid YAML
2. `pre-commit run --all-files` passes with no
   "Requirements file not found" warning
3. `docs/generated/impact/impact.json` regenerates per-commit
   (currently `[]` because no tags exist yet — that's correct)
4. `docs/contributing.md` documents the schema and tagging
   conventions

For Phases 2-4:

1. Phase 2 script runs to completion against the current test tree
   and produces N candidate entries; N approximates the SCENARIO
   count after dedup (target 150-250)
2. Phase 3: every `ENTROPIC_EXPORT` function in `entropic.h`
   carries ≥ 1 `@req` tag
3. Phase 4: every SCENARIO carries ≥ 1 `[req:...]` tag, and CI
   enforces that no `[req:REQ-X]` references a non-existent ID
4. (optional) acceptance_criteria drift check passes

## Out of Scope

- Sequence diagram generation (doxygen-guard `trace` mode unreliable)
- Migrating to a different traceability tool (Jira, Linear, etc.)
- Auto-generating release notes from the impact reports (could be a
  follow-up but not part of this proposal)
- Any architectural refactor; this is pure documentation +
  annotation work

## Notes

This proposal is sized so Phase 1 is small enough to land on develop
in one commit alongside the v2.1.3 release ceremony, while Phases
2-5 stage as their own commits / proposals as appetite allows.
Phase 1 alone gets us out of the "Requirements file not found" warning
state and starts the impact report machinery generating real output;
each subsequent phase increases the precision of those reports.

The user's framing on 2026-04-29: "BDD test cases to requirements as
a baseline ... pre-commit hooks that must pass, model behavior
enforcements, engine requirements, linkage to llama, release to pypi,
documentation structure used with doxygen guard ... this also
produces a very clear change log for every commit."
