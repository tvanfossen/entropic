---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260216-018
title: "In-Process Tools"
priority: P1
component: architecture
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-16
updated: 2026-02-16
tags: [architecture, mcp, tools, refactor, performance]
completed_date: null
scoped_files:
  - src/entropi/mcp/manager.py
  - src/entropi/mcp/client.py
  - src/entropi/mcp/servers/base.py
  - src/entropi/mcp/servers/filesystem.py
  - src/entropi/mcp/servers/bash.py
  - src/entropi/mcp/servers/git.py
  - src/entropi/mcp/servers/entropi.py
  - src/entropi/mcp/servers/diagnostics.py
  - src/entropi/core/engine.py
  - src/entropi/config/schema.py
  - tests/unit/test_mcp_servers.py
  - tests/unit/test_engine.py
depends_on:
  - P1-20260216-016  # Typed directives (tools return list[Directive] natively)
  - P1-20260216-017  # Context anchors (ContextAnchor directive exists)
blocks: []
related:
  - P1-20260211-011  # Library extraction benefits from clean tool interface
---

# In-Process Tools

## Problem Statement

Built-in tools (filesystem, bash, entropi, diagnostics, git) run as MCP servers
in separate subprocesses via `MCPClient(command=sys.executable, args=["-m", module])`.
They communicate over JSON-RPC stdio. This adds:

- **Serialization overhead** for every tool call/result
- **Process management complexity** (startup, shutdown, crash handling)
- **No native Python objects** across process boundary (directives must be JSON-encoded)
- **File leak (#82)**: MCP subprocess working directory defaults to `Path.cwd()` instead
  of `project_dir`, causing model tests to create files in the repo root

These are internal tools that should run in-process. Only external MCP connections
(3rd party servers, Claude Code integration) need the MCP protocol.

### Current Architecture

```
Engine ──► ServerManager ──► MCPClient ──► subprocess (stdio JSON-RPC)
                                              └── BashServer / FilesystemServer / ...
```

Every tool call, even `entropi.todo_write`, crosses a process boundary.

## Proposed Architecture

### ToolProvider Protocol

```python
class ToolProvider(Protocol):
    """Uniform interface for tool execution — in-process or remote."""
    async def list_tools(self) -> list[ToolDefinition]: ...
    async def call_tool(self, name: str, arguments: dict) -> ToolResult: ...
    async def connect(self) -> None: ...
    async def disconnect(self) -> None: ...
```

### Two Implementations

```python
class InProcessToolProvider(ToolProvider):
    """Wraps BaseMCPServer instances for direct in-process execution."""
    def __init__(self, servers: dict[str, BaseMCPServer]): ...

    async def call_tool(self, name: str, arguments: dict) -> ToolResult:
        prefix, tool_name = name.split(".", 1)
        server = self.servers[prefix]
        result = await server.handle_call(tool_name, arguments)
        return ToolResult(name=name, result=result, directives=result.directives)

class MCPToolProvider(ToolProvider):
    """Wraps MCPClient for remote MCP server connections."""
    # Existing MCPClient, adapted to ToolProvider interface
```

### ServerManager Changes

```python
class ServerManager:
    def __init__(self):
        self._providers: dict[str, ToolProvider] = {}

    def _register_builtin_servers(self, config):
        # Create server instances IN-PROCESS
        servers = {}
        if config.mcp.enable_filesystem:
            servers["filesystem"] = FilesystemServer(root_dir=project_dir)
        if config.mcp.enable_bash:
            servers["bash"] = BashServer(working_dir=project_dir)
        # ...
        self._providers["builtin"] = InProcessToolProvider(servers)

    def _register_external_servers(self, config):
        # External servers still use MCPClient (subprocess)
        for name, ext_config in config.mcp.external_servers.items():
            client = MCPClient(name, ext_config.command, ext_config.args)
            self._providers[name] = MCPToolProvider(client)
```

### Native Directives

With in-process execution, tool results return `list[Directive]` directly — no
JSON serialization/deserialization. The `InProcessToolProvider` passes typed
directive objects straight to the engine. Only `MCPToolProvider` needs the
JSON serde layer (for external MCP servers).

### File Leak Fix

In-process tools receive `project_dir` directly via constructor. No more
`Path.cwd()` fallback. The filesystem server's `root_dir` and bash server's
`working_dir` are set explicitly from the Application's `project_dir`. This
resolves #82 without any workaround.

## Migration Strategy

Migrate one server at a time, keeping MCPClient as fallback:

### Phase 1: InProcessToolProvider + EntropiServer

Start with the entropi server — it's the most tightly coupled to engine
internals (todo state, directives, tier changes). Highest benefit from
in-process execution.

### Phase 2: FilesystemServer + BashServer + GitServer

These are the most commonly called tools. In-process execution removes the
per-call serialization overhead. FilesystemServer gets `project_dir` directly,
fixing the file leak.

### Phase 3: DiagnosticsServer

Lower priority — less frequently called.

### Phase 4: External MCP Gateway (future)

If external services need to access entropi's tools, wrap `InProcessToolProvider`
in an MCP server. This is the reverse of today — today everything is MCP, future
is in-process with MCP as an optional gateway. Out of scope for this proposal.

## Open Questions from Original P1-015

| Question | Proposed Answer |
|----------|----------------|
| Permission model for in-process tools? | Same `get_permission_pattern()` on server classes. ServerManager already delegates to class methods, not instances. |
| Tool discovery for in-process tools? | `InProcessToolProvider.list_tools()` reads tool JSON from `data/tools/`. Same source as today. |
| Config-driven enable/disable? | `InProcessToolProvider` constructor respects config flags. Same as today's `_register_builtin_servers`. |
| Error isolation? | `try/except` at `InProcessToolProvider.call_tool()` boundary. Catches exceptions, returns `ToolResult(is_error=True)`. No process crash risk. |

## Risks

| Risk | Mitigation |
|------|------------|
| Large refactor touches all tool execution | Migrate one server at a time; MCPClient fallback during transition |
| In-process exception crashes engine | try/except at ToolProvider boundary |
| Breaking existing tool behavior | Test reports (P2-013) provide baseline comparison |
| External MCP servers need different path | MCPToolProvider preserves existing MCPClient behavior |

## Success Criteria

- [ ] `ToolProvider` protocol defined with `list_tools` + `call_tool`
- [ ] `InProcessToolProvider` wraps built-in servers for direct execution
- [ ] `MCPToolProvider` wraps `MCPClient` for external MCP connections
- [ ] All 5 built-in servers run in-process (no subprocess)
- [ ] File leak (#82) resolved — tools use `project_dir` from Application
- [ ] Directives returned as native `list[Directive]` (no JSON serde for in-process)
- [ ] External MCP servers still work via MCPToolProvider
- [ ] Permission model unchanged
- [ ] All existing tests pass
- [ ] Model test reports show no behavioral regression
