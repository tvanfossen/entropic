---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260216-017
title: "Generic Context Anchors"
priority: P1
component: architecture
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-16
updated: 2026-02-16
tags: [architecture, directives, context, refactor]
completed_date: null
scoped_files:
  - src/entropi/core/directives.py
  - src/entropi/core/engine.py
  - src/entropi/mcp/servers/entropi.py
  - tests/unit/test_engine.py
depends_on:
  - P1-20260216-016  # Typed directives (ContextAnchor dataclass)
blocks:
  - P1-20260216-018  # In-process tools can emit anchors natively
---

# Generic Context Anchors

## Problem Statement

The engine contains todo-specific domain knowledge that violates separation of concerns:

| Code | Location | Problem |
|------|----------|---------|
| `_cached_todo_state: str` | engine.py:197 | Engine caches tool-specific state |
| `_update_todo_anchor()` | engine.py:1317-1332 | Engine manages tool-specific message |
| `_directive_todo_state_changed()` | engine.py:342-352 | Engine handles tool-specific directive |
| `"is_todo_anchor"` metadata key | engine.py:1325 | Hardcoded domain key |

Called from 3 sites: `run()` (line 464), `_directive_tier_change()` (line 298),
and `_check_compaction()` (line 1315). Any new anchored state (diagnostics, memory,
etc.) would require adding more tool-specific code to the engine.

## Target Architecture

Replace with a generic mechanism. Any tool can push state to a named anchor.
The engine has zero domain knowledge — it just manages `{key: Message}`.

### New Directive Type

```python
@dataclass
class ContextAnchor(Directive):
    """Push state to a persistent context anchor."""
    key: str       # e.g., "todo_state", "diagnostics"
    content: str   # The anchored content (empty = remove anchor)
```

### Engine Changes

```python
# Replace _cached_todo_state with generic dict
_context_anchors: dict[str, str] = {}

def _directive_context_anchor(self, ctx, directive: ContextAnchor) -> None:
    """Handle context anchor from any tool."""
    if not directive.content:
        # Empty content = remove anchor
        self._context_anchors.pop(directive.key, None)
        ctx.messages = [
            m for m in ctx.messages
            if m.metadata.get("anchor_key") != directive.key
        ]
        return

    self._context_anchors[directive.key] = directive.content

    # Remove existing anchor with this key
    ctx.messages = [
        m for m in ctx.messages
        if m.metadata.get("anchor_key") != directive.key
    ]

    # Append at end (recency bias)
    anchor = Message(
        role="user",
        content=directive.content,
        metadata={"is_context_anchor": True, "anchor_key": directive.key},
    )
    ctx.messages.append(anchor)
```

### Re-inject After Compaction / Tier Change

The 3 call sites that currently call `_update_todo_anchor(ctx)` become:

```python
def _reinject_context_anchors(self, ctx: LoopContext) -> None:
    """Re-inject all context anchors (after compaction or tier change)."""
    for key, content in self._context_anchors.items():
        self._directive_context_anchor(
            ctx, ContextAnchor(key=key, content=content)
        )
```

### Server-Side Change

`EntropiServer.todo_write()` changes its directive emission from:

```python
# Before
{"type": "todo_state_changed", "params": {"state": formatted, "count": n, ...}}
```

to:

```python
# After
{"type": "context_anchor", "params": {"key": "todo_state", "content": formatted}}
```

The server controls the content — including empty-state usage instructions when
the todo list is empty. Engine never decides what to anchor.

### TodoStateChanged Directive — Remove or Keep?

The `TodoStateChanged` directive currently does two things:
1. Updates the context anchor (engine-side)
2. Fires `_on_todo_update` callback (UI notification)

Option A: Remove `TodoStateChanged` entirely. Use `ContextAnchor` for the anchor,
emit a separate lightweight directive for the UI callback.

Option B: Keep `TodoStateChanged` but have its handler call the generic
`_directive_context_anchor` internally. Less disruptive but leaves domain code.

**Recommendation: Option A** — clean separation. The UI callback can be triggered
by a new `NotifyPresenter` directive or by the `ContextAnchor` handler itself
(check if key == "todo_state" and fire callback). The latter is simpler but slightly
leaky. Decision point for implementation.

## Code to Remove from Engine

- `_cached_todo_state` instance variable
- `_update_todo_anchor()` method
- `_directive_todo_state_changed()` handler
- `TODO_STATE_CHANGED` registration in `_register_directive_handlers()`
- `"is_todo_anchor"` metadata checks

## Code to Add

- `ContextAnchor` dataclass (in directives.py, may already exist from P1-016)
- `_context_anchors: dict[str, str]` instance variable
- `_directive_context_anchor()` handler
- `_reinject_context_anchors()` method
- `CONTEXT_ANCHOR` registration
- Update 3 call sites to use `_reinject_context_anchors()`

## Risks

| Risk | Mitigation |
|------|------------|
| UI callback (`_on_todo_update`) breaks | Wire callback to ContextAnchor handler for key="todo_state" |
| Compaction strips anchor metadata | `_reinject_context_anchors` called after compaction (same pattern as today) |
| Multiple anchors bloat context | Each anchor is one message; current system already has this cost |

## Success Criteria

- [ ] Engine has zero todo-specific code
- [ ] `_context_anchors` dict manages all anchored state generically
- [ ] EntropiServer emits `ContextAnchor` directive for todo state
- [ ] Anchors survive compaction and tier changes
- [ ] Empty todo list shows usage instructions (server-controlled content)
- [ ] `_on_todo_update` callback still fires for UI updates
- [ ] All existing tests pass
