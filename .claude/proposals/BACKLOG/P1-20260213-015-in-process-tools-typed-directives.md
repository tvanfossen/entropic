---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260213-015
title: "In-Process Tools, Typed Directives, and Context Anchor"
priority: P1
component: architecture
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-13
updated: 2026-02-13
tags: [architecture, mcp, directives, tools, refactor]
completed_date: null
scoped_files:
  - src/entropi/mcp/
  - src/entropi/core/engine.py
  - src/entropi/core/directives.py
  - src/entropi/config/schema.py
  - src/entropi/app.py
  - tests/unit/test_engine.py
  - tests/unit/test_mcp_servers.py
depends_on: []
blocks: []
---

# In-Process Tools, Typed Directives, and Context Anchor

## Problem Statement

Three related architectural issues identified during smoke testing and design review:

### 1. Built-in MCP Servers Are Unnecessary Subprocesses

Built-in tools (filesystem, bash, entropi, diagnostics, git) run as MCP servers in
separate subprocesses via `MCPClient(command=sys.executable, args=["-m", module])`.
They communicate over JSON-RPC stdio. This adds:

- Serialization overhead for every tool call/result
- Process management complexity (startup, shutdown, crash handling)
- Inability to pass native Python objects (directives must be JSON-encoded)

These are internal tools that should run in-process. Only the **external-facing MCP
server** (where outside services connect to entropi) needs the MCP protocol.

### 2. Directives Are Freeform Dicts

Tool results carry `_directives` as `dict[str, Any]` — no type safety, no validation,
no IDE support. As directives expand (context anchors, future capabilities), freeform
dicts become error-prone.

### 3. Engine Has Todo-Specific Knowledge

`AgentEngine` contains `_cached_todo_state`, `_update_todo_anchor()`, and
`_directive_todo_state_changed()` — all specific to the todo tool. Engine should be
tool-agnostic. Any tool should be able to push state to a context anchor.

## Proposed Architecture

### Three-Layer Tool Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│ Layer 1: Internal Tools (in-process)                            │
│ ├── BaseTool → FilesystemTool, BashTool, EntropiTool, etc.     │
│ ├── Direct Python calls, native directive objects               │
│ └── Wrapped by LocalToolClient implementing ToolProvider        │
├─────────────────────────────────────────────────────────────────┤
│ Layer 2: External MCP Server (entropi exposes tools)            │
│ ├── MCP gateway wraps internal tools for external clients       │
│ ├── JSON-RPC over stdio (existing MCPServer infrastructure)     │
│ └── External services connect to entropi via this server        │
├─────────────────────────────────────────────────────────────────┤
│ Layer 3: 3rd Party MCP (entropi as client)                      │
│ ├── Entropi connects to external MCP servers                    │
│ ├── Configured via external_servers in config                   │
│ └── Already partially supported via external_servers config     │
└─────────────────────────────────────────────────────────────────┘
```

### ToolProvider Protocol

```python
class ToolProvider(Protocol):
    """Uniform interface for tool execution."""
    async def list_tools(self) -> list[ToolDefinition]: ...
    async def call_tool(self, name: str, arguments: dict) -> ToolResult: ...

class LocalToolClient(ToolProvider):
    """Adapter wrapping in-process BaseTool instances."""
    def __init__(self, tools: list[BaseTool]): ...

class MCPToolClient(ToolProvider):
    """Adapter wrapping remote MCP server connections."""
    # Existing MCPClient, refactored to implement ToolProvider
```

Engine interacts only with `ToolProvider` — doesn't know or care whether the tool is
in-process or remote.

### Typed Directive Hierarchy

```python
from dataclasses import dataclass

@dataclass
class Directive:
    """Base directive — all directives inherit from this."""
    pass

@dataclass
class StopProcessing(Directive):
    """Stop processing remaining tool calls."""
    pass

@dataclass
class TierChange(Directive):
    """Request a tier handoff."""
    target_tier: str

@dataclass
class ContextAnchor(Directive):
    """Push state to a persistent context anchor."""
    key: str          # e.g., "todo_state", "diagnostic_summary"
    content: str      # The anchored content

@dataclass
class TodoStateChanged(Directive):
    """Notify that todo state changed (triggers anchor update)."""
    formatted_state: str
```

Tools return `list[Directive]` directly (in-process) or JSON-serialized equivalents
(MCP boundary). `DirectiveProcessor` handles typed objects instead of parsing dicts.

### Generic Context Anchor

Replace engine's todo-specific anchor with a generic mechanism:

```python
# Engine stores anchors by key
_context_anchors: dict[str, str] = {}

def _update_context_anchor(self, ctx: LoopContext, key: str, content: str) -> None:
    """Update or create a named context anchor."""
    self._context_anchors[key] = content
    # Remove existing anchor with this key
    ctx.messages = [
        m for m in ctx.messages
        if m.metadata.get("anchor_key") != key
    ]
    # Append at end (recency bias)
    anchor = Message(
        role="user",
        content=content,
        metadata={"is_context_anchor": True, "anchor_key": key},
    )
    ctx.messages.append(anchor)
```

Any tool can push a `ContextAnchor` directive. The entropi server pushes todo state,
the diagnostics server could push LSP findings, a future memory tool could push
retrieved context — all through the same mechanism.

## Migration Path

### Phase 1: Typed Directives

- Create `Directive` base class and concrete subclasses
- Update `DirectiveProcessor` to accept typed directives
- Keep JSON fallback for MCP boundary (serialize/deserialize at adapter layer)
- Update existing directive handlers

### Phase 2: Generic Context Anchor

- Replace `_cached_todo_state` + `_update_todo_anchor()` with `_context_anchors` dict
- Replace `_directive_todo_state_changed` with generic `_directive_context_anchor`
- Entropi server emits `ContextAnchor(key="todo_state", ...)` instead of
  `{"todo_state_changed": ...}`

### Phase 3: In-Process Tools

- Create `BaseTool` base class with `execute()` method
- Create `LocalToolClient` adapter implementing `ToolProvider`
- Migrate built-in servers one at a time (entropi server first, then filesystem, etc.)
- Keep `MCPToolClient` for external MCP connections
- Remove subprocess launching for migrated tools

### Phase 4: External MCP Gateway

- Wrap `LocalToolClient` tools in an MCP server for external access
- External services connect to entropi's MCP gateway
- Internal tools never go through MCP protocol

## Risks

| Risk | Mitigation |
|------|------------|
| Large refactor scope | Phase incrementally; each phase independently testable |
| Breaking tool execution during migration | ToolProvider protocol ensures uniform interface |
| External MCP compatibility | Gateway layer preserves MCP protocol for external clients |
| Directive serialization at MCP boundary | JSON serde adapter at MCPToolClient level |

## Dependencies

- Should be done AFTER rename (P1-20260213-014) to avoid conflicts
- Should be done BEFORE library extraction (P1-20260211-011) — cleaner API surface

## Open Questions

- **Permission model:** Internal tools currently use MCP permission patterns. How does
  permission work for in-process tools? Same `get_permission_pattern()` on `BaseTool`?
- **Tool discovery:** MCP servers self-register tools via `list_tools`. In-process tools
  need the same discovery mechanism.
- **Config-driven enable/disable:** Tools can be disabled in config. `LocalToolClient`
  needs to respect this.
- **Error isolation:** MCP subprocess crashes don't take down the engine. In-process tool
  exceptions need equivalent isolation (try/catch at ToolProvider boundary).
