---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260302-026
title: "Runtime MCP server registration: hot-plug external tool servers"
priority: P2
component: mcp
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-03-02
updated: 2026-03-03
tags: [mcp, tools, runtime, integration, sse]
completed_date: null
scoped_files:
  - "src/entropic/mcp/manager.py"
  - "src/entropic/mcp/client.py"
  - "src/entropic/mcp/servers/external.py"
  - "src/entropic/config/schema.py"
  - "src/entropic/core/engine.py"
depends_on: []
blocks: []
---

# Runtime MCP Server Registration

## Problem Statement

External MCP servers can only be connected at startup via config
(`mcp.external_servers`) or pre-init registration (`register_server()` BEFORE
`ServerManager.initialize()`). There is no mechanism to connect an external
MCP server to a running engine.

Use cases requiring runtime registration:

1. **Claude → entropic via MCP**: Claude starts an MCP server, then tells the
   engine (via the external MCP socket) to connect to it. Currently impossible
   — the engine can't discover or connect to servers it didn't know about at
   startup.

2. **Dynamic tool providers**: A consumer's application spawns tool servers
   on demand (e.g., a database connection server created after the user
   provides credentials). These servers don't exist at config time.

3. **Development workflow**: Developer starts an MCP tool server in a
   separate terminal, wants the running engine to pick it up without restart.

## Current Architecture

```
Startup:
  config.yaml → mcp.external_servers → ServerManager.initialize()
                                            ↓
                                      MCPClient per server
                                            ↓
                                      tools registered

Pre-init:
  consumer code → server_manager.register_server(my_server)
                                            ↓
                                      InProcessProvider
                                            ↓
                                      tools registered

Runtime:  ← NO PATH EXISTS
```

### What Blocks Runtime Registration

1. **`ServerManager.initialize()` is one-shot.** It iterates config servers
   and starts clients. No method to add servers after this point.

2. **Tool list is cached.** `list_tools()` aggregates tools from all providers
   at call time, so new providers WOULD be discovered. But tier-level tool
   filtering (`allowed_tools`) is resolved at tier lock time from the cached
   tool list.

3. **No discovery/handshake protocol.** The engine has no way to discover an
   MCP server's transport (stdio, socket, SSE) and capabilities at runtime
   without config.

## Proposed Solution

### Transport Architecture: Native Multi-Transport via MCP SDK

MCP SDK 1.26.0 provides native transport implementations — no bridge
subprocess needed. `MCPClient` is extended to handle all transport types
directly through SDK context managers:

```
stdio:
  MCPClient ←stdio_client()→ subprocess

HTTP SSE:
  MCPClient ←sse_client()→ HTTP SSE endpoint

Streamable HTTP:
  MCPClient ←streamable_http_client()→ HTTP endpoint
```

**Why native SDK transport:**
- SDK already handles reconnection, event buffering, protocol negotiation
- Single `MCPClient` class covers all transports — one code path
- No additional subprocess per non-stdio server
- No additional dependencies (`httpx` already used by MCP SDK)
- New transports from SDK upgrades are free

### `.mcp.json` Support

Entropic reads `.mcp.json` at `ServerManager.initialize()` time (static,
no file watching). This is the same file Claude Code uses, enabling a single
source of truth for third-party MCP server configuration.

```json
{
  "mcpServers": {
    "pycommander": {
      "type": "sse",
      "url": "http://127.0.0.1:6277/sse"
    },
    "my-stdio-server": {
      "type": "stdio",
      "command": "my-mcp-server",
      "args": ["--verbose"]
    }
  }
}
```

**Discovery order** (last wins on name collision):
1. `mcp.external_servers` in YAML config
2. `.mcp.json` in `project_dir`
3. `.mcp.json` in `~/.entropic/` (global)

**Self-detection**: Entries whose resolved transport matches the engine's
own `ExternalMCPServer` socket are skipped automatically. This prevents
Entropic connecting to itself while still connecting to other Entropic
instances at different socket paths. See "Project-Derived Socket Path".

### Project-Derived Socket Path

`ExternalMCPServer` changes its default socket path from the fixed
`~/.entropic/mcp.sock` to a project-derived path:

```
~/.entropic/socks/{first8(sha256(abs(project_dir)))}.sock
```

**Why project-derived:**
- Stable across restarts (same project = same path)
- Unique per project directory — multiple Entropic instances don't
  collide
- Enables self-detection: Entropic reads its own hash from `project_dir`
  and skips any `.mcp.json` entry pointing to that socket
- Enables N-Entropic topology: multiple instances at different project
  dirs appear in `.mcp.json` with distinct paths; each connects to the
  others but not itself

```
Instance A (project=/home/user/proj-a):
  socket = ~/.entropic/socks/a1b2c3d4.sock
  reads .mcp.json → skips a1b2c3d4.sock → connects to b5c6d7e8.sock

Instance B (project=/home/user/proj-b):
  socket = ~/.entropic/socks/b5c6d7e8.sock
  reads .mcp.json → skips b5c6d7e8.sock → connects to a1b2c3d4.sock
```

**Self-detection logic** in `initialize()`:
1. Compute own socket path from `project_dir`
2. For each `.mcp.json` entry with `type: socket`, resolve the socket path
3. Skip if resolved path == own socket path

### API Surface

```python
# Runtime connection — any transport (MCPClient picks based on config)
await server_manager.connect_server(
    name="pycommander",
    sse_url="http://127.0.0.1:6277/sse",
)

await server_manager.connect_server(
    name="my-stdio-tool",
    command="my-mcp-server",
    args=["--verbose"],
)

# Disconnect at runtime
await server_manager.disconnect_server("pycommander")

# Inspect connected servers
info = server_manager.list_servers()
# {"pycommander": ServerInfo(status="connected", tools=[...]), ...}
```

### ServerManager Changes

```python
class ServerManager:
    async def connect_server(
        self,
        name: str,
        command: str | None = None,
        args: list[str] | None = None,
        sse_url: str | None = None,
    ) -> list[str]:
        """Connect to an MCP server at runtime.

        Exactly one of command+args (stdio) or sse_url must be provided.
        Returns list of tool names registered from the server.
        Raises ValueError if name conflicts with existing server.
        """

    async def disconnect_server(self, name: str) -> None:
        """Disconnect and remove a runtime-registered server."""

    def list_servers(self) -> dict[str, ServerInfo]:
        """List all connected servers (static + runtime) with status."""
```

**`MCPClient` transport selection** (new `transport` parameter):

```python
class MCPClient:
    def __init__(
        self,
        name: str,
        command: str | None = None,
        args: list[str] | None = None,
        env: dict[str, str] | None = None,
        sse_url: str | None = None,   # NEW
        transport: str = "stdio",      # NEW: "stdio" | "sse" | "http"
    ) -> None: ...
```

The `connect()` method selects the SDK context manager based on `transport`:
- `"stdio"` → `stdio_client(StdioServerParameters(...))`
- `"sse"` → `sse_client(url, ...)`
- `"http"` → `streamable_http_client(url, ...)`

### Security Considerations

- **Permission check**: Runtime-connected tools inherit the engine's
  permission system. Unknown tools prompt for approval. `allowed_tools`
  per-tier filtering applies.
- **URL validation**: SSE/HTTP URLs validated for scheme (https preferred;
  http requires explicit opt-in for non-localhost via config).
- **Self-detection only**: No blanket blocklist of socket paths — only
  own socket is skipped. All other entries are treated as legitimate peers.
- **Namespace isolation**: External server tools prefixed with server name
  (`pycommander.tool_name`) to prevent collision with built-in tools.

## Acceptance Criteria

- [ ] `MCPClient` supports SSE transport via `sse_client()` from MCP SDK
- [ ] `MCPClient` supports stdio transport (existing behavior unchanged)
- [ ] `.mcp.json` read statically at `ServerManager.initialize()`
- [ ] Servers from `.mcp.json` merged with YAML `external_servers`
- [ ] `ExternalMCPServer` default socket path is project-derived hash
- [ ] Self-detection: entries matching own socket path are skipped
- [ ] N-Entropic topology: instance A connects to B, B connects to A
- [ ] `ServerManager.connect_server()` works post-`initialize()`
- [ ] `ServerManager.disconnect_server()` removes server + tools
- [ ] `list_servers()` returns all connected servers with transport info
- [ ] Runtime-registered tools appear in `list_tools()` immediately
- [ ] Per-tier `allowed_tools` filtering applies to runtime tools
- [ ] Unit tests: MCPClient SSE transport (mocked `sse_client`)
- [ ] Unit tests: `.mcp.json` parsing and merge with YAML config
- [ ] Unit tests: self-detection (own socket skipped, other socket connected)
- [ ] Unit tests: connect/disconnect lifecycle
- [ ] Integration test: SSE server connected via `.mcp.json`, call tool

## Implementation Plan

### Phase 1: Project-Derived Socket Path + `.mcp.json` Reading
- Change `ExternalMCPConfig.socket_path` default to
  `~/.entropic/socks/{first8(sha256(abs(project_dir)))}.sock`
- Add `project_dir`-aware default computation in `ExternalMCPServer`
- Add `.mcp.json` parser in `ServerManager.initialize()`:
  - Read `project_dir/.mcp.json` and `~/.entropic/.mcp.json`
  - Convert entries to `MCPClient` instances (stdio or SSE)
  - Skip entries matching own socket path (self-detection)
- Unit tests: path derivation, `.mcp.json` parsing, self-detection

### Phase 2: MCPClient Multi-Transport
- Add `sse_url` and `transport` parameters to `MCPClient`
- `connect()`: select `sse_client` or `stdio_client` based on transport
- Unit tests: SSE path (mock `sse_client`), existing stdio unchanged

### Phase 3: Runtime connect/disconnect on ServerManager
- `connect_server(name, command, args, sse_url)` — post-init registration
- `disconnect_server(name)` — teardown + tool removal
- `list_servers()` — status snapshot
- Lock protects `_clients` during concurrent access
- Unit tests: connect/disconnect lifecycle, name collision

### Phase 4: Engine Consumer API
- `AgentEngine.connect_server(...)` / `disconnect_server(name)` delegates
  to `ServerManager`
- Tool list refresh (already works — `list_tools()` aggregates on call)
- Integration test: SSE server via `.mcp.json`, call tool end-to-end

## Identity Library Interaction (P1-024)

Runtime-connected tools follow the same `allowed_tools` filtering as
config-defined tools:

- `tool_runner` identity (`allowed_tools: all`) sees all runtime tools
- Other identities with restricted `allowed_tools` do NOT see runtime
  tools unless the consumer adds them to the identity's allow list
- Runtime tools are namespaced (`server-name.tool_name`) — consumers
  add the namespaced name to `allowed_tools`

```yaml
tiers:
  code_writer:
    allowed_tools:
      - filesystem.write_file
      - pycommander.run_command     # Runtime tool, explicitly permitted
```

## Health Checking

External server crashes are detected via the MCP SDK session lifecycle —
session methods raise when the underlying transport closes. The `MCPClient`
wraps session calls in try/except; a transport failure marks `is_connected`
False and logs at WARNING. No active ping loop is added for the initial
implementation (SDK handles session-level errors).

```python
@dataclass
class ServerInfo:
    name: str
    transport: str              # "stdio" | "sse" | "http"
    url: str | None             # Set for SSE/HTTP servers
    command: str | None         # Set for stdio servers
    status: Literal["connected", "disconnected", "error"]
    tools: list[str]            # Tool names registered from this server
    connected_at: datetime
    source: str                 # "config" | "mcp_json" | "runtime"
```

## Risks & Considerations

- **Tool list invalidation**: `list_tools()` already aggregates from all
  providers at call time. New providers ARE discovered. Per-tier
  `allowed_tools` is resolved at tier lock time — a runtime server
  connected mid-generation won't be visible until the next tier lock.
  This is acceptable (tools appear on next turn, not mid-turn).
- **`.mcp.json` name collisions**: If YAML `external_servers` and
  `.mcp.json` define the same server name, YAML wins (first write wins,
  mcp.json processed second). Log at WARNING when overridden.
- **Self-detection hash stability**: The hash is computed from
  `abs(project_dir)`, so symlinks or different paths to the same directory
  would produce different hashes. Document: use canonical paths. This is
  acceptable — consumers control what they put in `.mcp.json`.
- **SSE server unavailable at startup**: If an SSE server in `.mcp.json`
  is unreachable, `_safe_connect()` logs the error and skips. Tools from
  that server are simply absent. Same behavior as unreachable stdio servers.
- **N-Entropic topology**: Each instance reads its own socket from
  `project_dir`. If both instances include the other's socket in `.mcp.json`,
  connection is symmetric. No special handling needed beyond self-detection.

## Implementation Log

### 2026-03-03
- [x] Proposal rewritten: bridge subprocess approach dropped, native MCP SDK
  transports adopted, `.mcp.json` reading added, project-derived socket path
  and self-detection designed
- **Decision**: MCP SDK 1.26.0 has native `sse_client`, `stdio_client`,
  `streamable_http_client` — no bridge utility needed
- **Decision**: `.mcp.json` read statically at `initialize()` (no file watching)
- **Decision**: Socket path = `~/.entropic/socks/{first8(sha256(abs(project_dir)))}.sock`
  for stable-per-project, unique-per-instance identity
- **Decision**: Self-detection by socket path comparison (not command name)
- [x] `mcp/client.py`: SSE transport via `sse_client`, transport inferred from `sse_url`
- [x] `mcp/servers/external.py`: `compute_socket_path(project_dir)` utility; project_dir param
- [x] `config/schema.py`: `socket_path: OptionalExpandedPath = None` (was fixed path)
- [x] `mcp/manager.py`: `_load_mcp_json_servers()`, `connect_server()`, `disconnect_server()`,
  `list_servers()`, `ServerInfo` dataclass, `asyncio.Lock` for `_clients`
- [x] `core/engine.py`: `connect_server()` / `disconnect_server()` consumer API
- [x] `app.py`: pass `project_dir` to `ExternalMCPServer`
- [x] 24 unit tests in `tests/unit/test_mcp_runtime.py` — all green
- [x] Merged to develop at v1.4.0

## References

- MCP SDK 1.26.0: `mcp.client.stdio.stdio_client`, `mcp.client.sse.sse_client`,
  `mcp.client.streamable_http.streamable_http_client`
- Current `MCPClient`: `src/entropic/mcp/client.py`
- Current `ServerManager`: `src/entropic/mcp/manager.py`
- Current `ExternalMCPServer`: `src/entropic/mcp/servers/external.py`
- Current socket default: `ExternalMCPConfig.socket_path` in `config/schema.py:226`
- `.mcp.json` format: Claude Code MCP server configuration spec
