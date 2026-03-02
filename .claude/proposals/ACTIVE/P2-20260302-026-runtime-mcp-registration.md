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
updated: 2026-03-02
tags: [mcp, tools, runtime, integration, bridge]
completed_date: null
scoped_files:
  - "src/entropic/mcp/manager.py"
  - "src/entropic/mcp/servers/external.py"
  - "src/entropic/mcp/bridge.py"
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

### Transport Architecture: stdio-native + SSE bridge

The engine speaks **one transport: stdio**. All MCP servers are managed as
subprocesses communicating over stdin/stdout JSON-RPC.

Non-stdio transports (HTTP SSE, WebSocket, Unix socket) are handled by a
**bundled bridge utility** (`entropic-mcp-bridge`) that translates between
the remote transport and stdio. The engine spawns the bridge as a regular
stdio subprocess — it doesn't know or care what's on the other end.

```
Native stdio:
  Engine ←stdin/stdout→ MCP Server (subprocess)

SSE via bridge:
  Engine ←stdin/stdout→ entropic-mcp-bridge ←HTTP SSE→ Remote Server

Socket via bridge:
  Engine ←stdin/stdout→ entropic-mcp-bridge ←Unix socket→ Local Server
```

**Why stdio-native:**
- Engine transport layer stays trivial: `subprocess.Popen` + pipe I/O
- One code path for subprocess lifecycle, error handling, health checks
- SSE complexity (reconnection, event buffering, HTTP auth, TLS, proxy)
  stays outside the engine in a standalone, independently testable utility
- Bridge is reusable outside entropic by any stdio-speaking MCP client
- Adding new transports = new bridge flags, zero engine changes

### API Surface

```python
# Direct stdio server
await server_manager.connect_server(
    name="my-server",
    command="my-mcp-server",
    args=["--verbose"],
)

# SSE server via bridge (engine doesn't know the difference)
await server_manager.connect_server(
    name="remote-server",
    command="entropic-mcp-bridge",
    args=["--sse", "http://localhost:8080/sse"],
)

# Unix socket via bridge
await server_manager.connect_server(
    name="local-server",
    command="entropic-mcp-bridge",
    args=["--socket", "/tmp/my-server.sock"],
)

# Convenience: SSE shorthand (resolves to bridge internally)
await server_manager.connect_server(
    name="remote-server",
    sse_url="http://localhost:8080/sse",
)

await server_manager.disconnect_server("my-server")
```

The `sse_url` and `socket_path` convenience parameters resolve to
`command="entropic-mcp-bridge"` internally — sugar over the same stdio
mechanism.

### `entropic-mcp-bridge` Utility

Bundled CLI tool, installed with `entropic-engine`. Translates non-stdio
MCP transports to stdio for any consumer.

```
Usage: entropic-mcp-bridge [OPTIONS]

Transports (mutually exclusive):
  --sse URL          Connect to HTTP SSE endpoint
  --socket PATH      Connect to Unix domain socket

Options:
  --header KEY=VAL   HTTP header (SSE only, repeatable)
  --reconnect-delay  Seconds between reconnection attempts (default: 2)
  --max-reconnects   Max reconnection attempts (default: 10, 0=infinite)
  --timeout          Connection timeout in seconds (default: 30)
  --verbose          Log bridge activity to stderr
```

**SSE bridge behavior:**
- Connects to SSE endpoint, reads event stream
- Translates SSE events → JSON-RPC messages on stdout
- Translates JSON-RPC messages on stdin → HTTP POST requests
- Automatic reconnection with exponential backoff
- Auth headers passed via `--header Authorization=Bearer:xxx`
- TLS/proxy handled by standard Python `httpx`/`aiohttp`
- Clean shutdown on stdin EOF or SIGTERM

**Socket bridge behavior:**
- Connects to Unix domain socket
- Bidirectional JSON-RPC relay between socket and stdio
- Reconnection on socket disconnect

### ExternalMCPServer Tool: `entropic.connect_server`

Exposed via the external MCP socket so Claude (or any MCP client) can tell
the engine to connect to a new server:

```json
{
  "name": "connect_server",
  "description": "Connect to an external MCP tool server at runtime.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "name": {"type": "string", "description": "Server name (unique ID)"},
      "command": {"type": "string", "description": "Command to spawn (stdio)"},
      "args": {"type": "array", "items": {"type": "string"}},
      "sse_url": {"type": "string", "description": "SSE endpoint URL (uses bridge)"},
      "socket_path": {"type": "string", "description": "Unix socket path (uses bridge)"}
    },
    "required": ["name"],
    "oneOf": [
      {"required": ["command"]},
      {"required": ["sse_url"]},
      {"required": ["socket_path"]}
    ]
  }
}
```

Engine resolves `sse_url`/`socket_path` to bridge commands internally.
Claude doesn't need to know about the bridge — just provides the endpoint.

### ServerManager Changes

```python
class ServerManager:
    async def connect_server(
        self,
        name: str,
        command: str | None = None,
        args: list[str] | None = None,
        sse_url: str | None = None,
        socket_path: str | None = None,
    ) -> list[str]:
        """Connect to an MCP server at runtime.

        Exactly one of command, sse_url, or socket_path must be provided.
        sse_url and socket_path are convenience parameters that resolve to
        entropic-mcp-bridge as the command.

        Returns list of tool names registered from the server.
        Raises if name conflicts with existing server.
        """

    async def disconnect_server(self, name: str) -> None:
        """Disconnect and remove a runtime-registered server."""

    def list_servers(self) -> dict[str, ServerInfo]:
        """List all connected servers with connection status."""
```

### Security Considerations

- **Permission check**: Runtime-connected tools inherit the engine's
  permission system. Unknown tools prompt for approval. `allowed_tools`
  per-tier filtering applies.
- **Command validation**: Stdio commands checked against permission deny
  list. Bridge URLs validated for scheme (https preferred, http requires
  explicit opt-in for non-localhost).
- **Rate limiting**: Runtime servers subject to same rate limits as
  config-defined servers.
- **Namespace isolation**: Runtime server tools prefixed with server name
  (`my-server.tool_name`) to prevent collision with built-in tools.
- **Bridge isolation**: Bridge runs as a subprocess with no access to
  engine internals. Compromise of a remote SSE server is contained to
  the tool responses it can produce.

## Acceptance Criteria

- [ ] `ServerManager.connect_server()` spawns stdio subprocess, discovers tools
- [ ] `ServerManager.disconnect_server()` terminates subprocess, removes tools
- [ ] `sse_url` convenience parameter resolves to bridge command
- [ ] `socket_path` convenience parameter resolves to bridge command
- [ ] Runtime-registered tools appear in `list_tools()` immediately
- [ ] Per-tier `allowed_tools` filtering applies to runtime tools
- [ ] `entropic.connect_server` tool exposed via ExternalMCPServer
- [ ] Permission system applies to runtime-registered tools
- [ ] `entropic-mcp-bridge --sse` connects and relays JSON-RPC
- [ ] `entropic-mcp-bridge --socket` connects and relays JSON-RPC
- [ ] Bridge reconnects on SSE disconnect with backoff
- [ ] Bridge exits cleanly on stdin EOF / SIGTERM
- [ ] Unit tests for connect/disconnect lifecycle
- [ ] Unit tests for bridge transport translation
- [ ] Integration test: start MCP server, connect at runtime, call tool
- [ ] Integration test: SSE endpoint via bridge, call tool

## Implementation Plan

### Phase 1: ServerManager Runtime API (stdio only)
- Add `connect_server()` / `disconnect_server()` to ServerManager
- Subprocess lifecycle: spawn, pipe wiring, health check, kill
- Tool discovery from newly connected server via `tools/list`
- `list_servers()` with connection status

### Phase 2: `entropic-mcp-bridge` Utility
- CLI entry point bundled with `entropic-engine`
- SSE transport: `httpx`-based SSE client ↔ stdio relay
- Socket transport: Unix domain socket ↔ stdio relay
- Reconnection with exponential backoff
- Clean shutdown handling

### Phase 3: Convenience Parameters + ExternalMCPServer Tool
- `sse_url` / `socket_path` → bridge command resolution in `connect_server()`
- `entropic.connect_server` tool on ExternalMCPServer
- Parameter validation, error messages

### Phase 4: Security & Permissions
- Runtime tool permission enforcement
- Command deny list validation
- URL scheme validation (https preferred)
- Namespace collision prevention

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
      - my-server.custom_lint     # Runtime tool, explicitly permitted
```

## Health Checking

Runtime servers need liveness monitoring since they can crash independently
of the engine.

### Mechanism

**Pipe EOF detection** (primary): Engine reads stdout pipe in a background
task. If pipe closes (process exit), server is marked dead and removed.

**Periodic ping** (secondary): Every 30s, engine sends JSON-RPC `ping`
to each runtime server. If no response within 5s, server is marked
unhealthy. After 3 consecutive failures, server is disconnected.

```python
@dataclass
class ServerInfo:
    name: str
    command: str
    args: list[str]
    status: Literal["connected", "unhealthy", "disconnected"]
    tools: list[str]           # Tool names registered from this server
    connected_at: datetime
    last_ping: datetime | None
    pid: int | None            # Subprocess PID (None if bridge/indirect)
```

### Config Persistence

Runtime servers are **ephemeral by default** — they don't survive engine
restart. This is correct for the primary use cases (Claude spawning servers,
dev workflow).

Optional persistence via config for servers that should auto-reconnect:

```yaml
mcp:
  runtime_servers:
    - name: my-server
      command: my-mcp-server
      args: ["--verbose"]
      auto_reconnect: true     # Reconnect on engine restart
```

Servers connected via `connect_server()` tool/API are NOT auto-persisted.
Consumer must explicitly add to config if persistence is desired.

## Risks & Considerations

- **Dangling connections**: Runtime server crashes → detected via pipe EOF
  or ping failure. Server removed from tool list. Consumer notified via
  callback if registered.
- **Tool list invalidation**: `list_tools()` already aggregates from all
  providers at call time. New providers ARE discovered. Per-tier
  `allowed_tools` is resolved at tier lock time — a runtime server
  connected mid-generation won't be visible until the next tier lock.
  This is acceptable (tools appear on next turn, not mid-turn).
- **Startup order**: If Claude starts an MCP server and immediately calls
  `connect_server`, there's a race. `connect_server()` retries connection
  up to 3 times with 1s backoff before failing.
- **Bridge as dependency**: Bridge adds `httpx` (or `aiohttp`) as a
  dependency for SSE support. Consider making it optional
  (`entropic-engine[bridge]`) to keep base install minimal.
- **Bridge process count**: Each bridged server = one additional subprocess.
  Multiple remote servers = multiple bridge processes. Acceptable for
  typical usage (1-3 servers), but document the scaling characteristic.
- **Namespace collisions**: Two runtime servers with the same name →
  `connect_server()` raises. Two servers providing a tool with the same
  name → namespace prefix prevents collision (`server-a.read` vs
  `server-b.read`).

## Implementation Log

{Entries added as work progresses}

## References

- MCP specification: transport types (stdio, SSE, socket)
- Current ExternalMCPServer: `src/entropic/mcp/servers/external.py`
- Library consumer guide: ServerManager initialization order
