---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260209-010
title: "Embed LSP Diagnostics in Filesystem Tool Responses"
priority: P2
component: mcp/servers/filesystem
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-09
updated: 2026-02-09
tags: [lsp, diagnostics, pyright, filesystem, tool-integration]
completed_date: null
scoped_files:
  - src/entropi/mcp/servers/filesystem.py
  - src/entropi/mcp/servers/diagnostics.py
  - src/entropi/config/schema.py
depends_on: []
blocks: []
---

# Embed LSP Diagnostics in Filesystem Tool Responses

## Problem Statement

When the model edits or writes a file, it has no immediate feedback on whether the change introduced type errors, import failures, or other static analysis issues. The model must explicitly call a separate `diagnostics` tool to check — which it often forgets to do, or calls too late.

Meanwhile, a separate diagnostics tool is exposed to the model, adding cognitive load and tool surface area that could be eliminated.

### Observed Failure Mode

During live testing, the model was asked to "add a bug to main.py" and executed `filesystem.edit_file` without any gate or feedback. The edit was syntactically valid (removed a conditional), so pyright would not catch it — but the general pattern of ungated edits without diagnostic feedback is the concern.

## Proposed Solution

Embed LSP diagnostic results directly into `filesystem.write_file` and `filesystem.edit_file` tool responses. After applying the edit, automatically run diagnostics and include **only newly introduced issues** in the response. This removes the need for a separate diagnostics tool.

### Design Decision: Embed, Don't Gate

An earlier design considered **gating** edits behind diagnostics (reject the edit if it introduces errors). This was rejected because:

1. Adds latency to every write/edit operation
2. Requires complex stash/rollback mechanics
3. Pre-existing errors create false negatives
4. The model benefits more from seeing the errors and self-correcting

Instead: apply the edit, report diagnostics, let the model react.

## Flow Diagrams

### Current Flow (No Diagnostic Feedback)

```
Model                    Filesystem MCP           File
  |                           |                     |
  |--- edit_file(old, new) -->|                     |
  |                           |--- apply edit ----->|
  |                           |<-- success ---------|
  |<-- {success: true} -------|                     |
  |                           |                     |
  | (model continues, unaware of introduced errors) |
```

### Proposed Flow (Diagnostics Embedded in Response)

```
Model                    Filesystem MCP       LSP/Pyright        File
  |                           |                    |               |
  |--- edit_file(old, new) -->|                    |               |
  |                           |-- snapshot diag -->|               |
  |                           |<- pre_diagnostics -|               |
  |                           |                    |               |
  |                           |--- apply edit -----|-------------->|
  |                           |                    |               |
  |                           |-- snapshot diag -->|               |
  |                           |<- post_diagnostics |               |
  |                           |                    |               |
  |                           |--- diff(pre,post) -|               |
  |                           |                    |               |
  |<-- {success, new_issues} -|                    |               |
  |                           |                    |               |
  | (model sees introduced    |                    |               |
  |  errors, self-corrects)   |                    |               |
```

### Diagnostic Diff Logic

```
                    Pre-Edit Diagnostics
                    ┌─────────────────────┐
                    │ warn: line 12       │
                    │ error: line 45      │
                    │ warn: line 78       │
                    └─────────────────────┘
                              │
                     apply edit to file
                              │
                              v
                    Post-Edit Diagnostics
                    ┌─────────────────────┐
                    │ warn: line 12       │  ← pre-existing (keep quiet)
                    │ error: line 45      │  ← pre-existing (keep quiet)
                    │ error: line 52      │  ← NEW (report to model)
                    │ warn: line 78       │  ← pre-existing (keep quiet)
                    └─────────────────────┘
                              │
                              v
                    Reported to Model
                    ┌─────────────────────┐
                    │ error: line 52      │  ← only new issues
                    └─────────────────────┘
```

### Write Tool Flow (Same Pattern)

```
Model                    Filesystem MCP       LSP/Pyright        File
  |                           |                    |               |
  |--- write_file(content) -->|                    |               |
  |                           |-- snapshot diag -->|               |
  |                           |<- pre_diagnostics -|  (may be      |
  |                           |   (empty if new    |   empty)      |
  |                           |    file)           |               |
  |                           |                    |               |
  |                           |--- write file -----|-------------->|
  |                           |                    |               |
  |                           |-- snapshot diag -->|               |
  |                           |<- post_diagnostics |               |
  |                           |                    |               |
  |<-- {success, new_issues} -|                    |               |
```

### Read Tool Flow (NOT Included)

Read operations do NOT include diagnostics. Rationale:
- Model reading a file to understand it shouldn't be distracted by pre-existing issues
- Risk of scope creep (model tries to fix unrelated problems)
- No "before/after" diff possible — all diagnostics are pre-existing

## Acceptance Criteria

- [ ] `filesystem.edit_file` response includes `new_diagnostics` field (empty array if no new issues)
- [ ] `filesystem.write_file` response includes `new_diagnostics` field
- [ ] `filesystem.read_file` response does NOT include diagnostics
- [ ] Only NEWLY INTRODUCED diagnostics are reported (pre-existing are filtered)
- [ ] Non-diagnosable file types (YAML, Markdown, etc.) skip diagnostic check silently
- [ ] Diagnostic check has configurable timeout (default 1s) — returns empty on timeout
- [ ] Diagnostics tool can be removed from model's tool set (no longer needed)
- [ ] Unit tests verify diff logic (pre-existing filtered, new reported)

## Implementation Plan

### Phase 1: Diagnostic Diff Engine

Build the core before/after snapshot and diff logic:
- Snapshot function: query LSP diagnostics for a file path
- Diff function: compare pre/post snapshots, return only new issues
- Handle async settle time (pyright may need a moment after file change)
- Handle timeout gracefully (return empty, don't block)

### Phase 2: Integrate with Filesystem MCP Server

Wire the diff engine into `edit_file` and `write_file` handlers:
- Snapshot before edit/write
- Apply the operation
- Snapshot after (with settle delay)
- Include diff in response
- Skip for non-diagnosable file types

### Phase 3: Remove Standalone Diagnostics Tool

Once diagnostics are embedded:
- Remove `diagnostics` from model's tool set
- Update system prompts (no need to mention diagnostic tool)
- Config toggle: `mcp.filesystem.diagnostics_on_edit: true/false`

## Risks & Considerations

- **Async settle time**: Pyright runs as a language server and may not have diagnostics ready immediately after a file change. Need to poll or wait with timeout.
- **Pre-existing vs introduced**: The diff logic must be robust. Comparing by (line, message) is fragile if the edit shifts line numbers. Consider comparing by (message_text, severity) and accepting some noise.
- **Latency**: Every write/edit pays a diagnostic cost. The 1s timeout caps worst case, but adds latency to the common path. Config flag allows disabling.
- **Language coverage**: Only Python (pyright) initially. C/C++ (clangd) could be added later. Non-covered languages silently skip.
- **Token cost**: Diagnostic output in every edit response inflates context over long sessions. Keep output concise (severity + line + message, no full diagnostic objects).

## Implementation Log

{Entries added as work progresses}

## References

- Discussion during feature/tui-tier-display-tool-panels implementation (2026-02-09)
- Existing config: `FilesystemConfig.diagnostics_on_edit` already in schema.py
- LSP config: `LSPConfig` in schema.py (pyright + clangd support defined)
