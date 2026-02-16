---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260216-016
title: "Typed Directives"
priority: P1
component: architecture
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-16
updated: 2026-02-16
tags: [architecture, directives, refactor, type-safety]
completed_date: null
scoped_files:
  - src/entropi/core/directives.py
  - src/entropi/core/engine.py
  - src/entropi/mcp/servers/entropi.py
  - src/entropi/mcp/servers/base.py
  - src/entropi/mcp/client.py
  - tests/unit/test_engine.py
  - tests/unit/test_directives.py
depends_on: []
blocks:
  - P1-20260216-017  # Context anchors uses typed ContextAnchor directive
  - P1-20260216-018  # In-process tools return list[Directive] natively
---

# Typed Directives

## Problem Statement

Tool results carry directives as `dict[str, Any]` — string type constants dispatched
through `DirectiveProcessor.register()`. No type safety, no IDE support, no validation.
As directives expand (context anchors, future capabilities), freeform dicts become
error-prone and hard to reason about.

### Current Wire Format

```json
{
  "result": "Todo updated",
  "_directives": [
    {"type": "todo_state_changed", "params": {"state": "...", "count": 2}}
  ]
}
```

### Current Code

```python
# directives.py — string constants
STOP_PROCESSING = "stop_processing"
TIER_CHANGE = "tier_change"
TODO_STATE_CHANGED = "todo_state_changed"
# ... 6 total

# DirectiveProcessor — dict-based dispatch
class DirectiveProcessor:
    def register(self, directive_type: str, handler: Callable): ...
    def process(self, ctx, directives: list[dict]) -> DirectiveResult: ...
```

## Proposed Approach

### 1. Directive Dataclass Hierarchy

```python
from dataclasses import dataclass

@dataclass
class Directive:
    """Base — all directives inherit from this."""
    pass

@dataclass
class StopProcessing(Directive):
    """Stop processing remaining tool calls."""
    pass

@dataclass
class TierChange(Directive):
    """Request a tier handoff."""
    target_tier: str
    reason: str = ""
    task_state: str = ""

@dataclass
class ClearSelfTodos(Directive):
    pass

@dataclass
class InjectContext(Directive):
    content: str
    role: str = "user"

@dataclass
class PruneMessages(Directive):
    keep_recent: int

@dataclass
class TodoStateChanged(Directive):
    state: str
    count: int = 0
    items: list[dict] | None = None
```

### 2. Update DirectiveProcessor

Replace string-keyed dispatch with type-keyed dispatch:

```python
class DirectiveProcessor:
    def register(self, directive_type: type[Directive], handler: Callable): ...
    def process(self, ctx, directives: list[Directive]) -> DirectiveResult: ...
```

Handlers receive typed objects instead of `(ctx, params, result)`.

### 3. Deserialization at MCP Boundary

Since MCP servers still run as subprocesses (in-process is P1-018), directives
cross the process boundary as JSON. Add deserialization in `extract_directives()`:

```python
_DIRECTIVE_REGISTRY: dict[str, type[Directive]] = {
    "stop_processing": StopProcessing,
    "tier_change": TierChange,
    # ...
}

def deserialize_directive(raw: dict) -> Directive:
    cls = _DIRECTIVE_REGISTRY[raw["type"]]
    return cls(**raw.get("params", {}))
```

MCP servers continue to emit JSON dicts (they're separate processes). The
deserialization happens in `_process_directives()` in the engine, immediately
after `extract_directives()` parses the JSON.

### 4. Update Engine Handlers

All `_directive_*` handlers in engine.py change signature from
`(self, ctx, params, result)` to `(self, ctx, directive)` where `directive`
is the typed dataclass. Engine code changes from `params.get("state", "")`
to `directive.state`.

### 5. Server-Side — No Change Yet

MCP servers (entropi.py, etc.) continue to construct directives as dicts for
JSON serialization. The typed hierarchy lives engine-side for now. When in-process
tools land (P1-018), servers can return `list[Directive]` directly.

## Migration Strategy

1. Create directive dataclasses in `directives.py`
2. Add `_DIRECTIVE_REGISTRY` and `deserialize_directive()`
3. Update `DirectiveProcessor` to type-keyed dispatch
4. Update `extract_directives()` to deserialize
5. Update all engine handlers to accept typed directives
6. Update unit tests

Each step is independently testable. No behavioral change — pure refactor.

## Risks

| Risk | Mitigation |
|------|------------|
| Handler signature change breaks tests | Update tests alongside handlers |
| Missing directive type in registry | `deserialize_directive` raises KeyError — loud failure, easy to debug |
| MCP servers emit unknown directive | Registry miss is an error, not silent drop |

## Success Criteria

- [ ] All 6 directive types have dataclass definitions
- [ ] DirectiveProcessor dispatches on type, not string
- [ ] Engine handlers receive typed directive objects
- [ ] MCP boundary deserialization works (JSON → dataclass)
- [ ] All existing tests pass with no behavioral change
- [ ] No string-based directive dispatch remains in engine
