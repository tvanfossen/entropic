# Library Consumer Guide

How to embed entropic as an inference engine in your own application.
Reference implementation: `examples/pychess/`.

## Install

```bash
pip install entropic          # Core inference engine
pip install entropic[tui]     # Terminal UI (not needed for library use)
```

## Project Structure

```
my-app/
├── data/
│   ├── default_config.yaml          # App defaults (seeded to .myapp/)
│   └── tools/
│       └── {server_name}/
│           └── {tool_name}.json     # MCP tool definitions
├── prompts/
│   ├── constitution.md              # Shared system prompt (all tiers)
│   └── identity_{tier}.md           # Per-tier identity + frontmatter
└── src/
    ├── config.py                    # ConfigLoader setup
    ├── engine.py                    # Wiring: orchestrator + servers + engine
    ├── my_server.py                 # Custom MCP server + tools
    └── main.py                      # Application entry point
```

## Step-by-Step Setup

### 1. Configuration

Create `data/default_config.yaml` with your model tiers, routing, and server settings.
ConfigLoader seeds this to `.myapp/config.local.yaml` on first run.

```python
from pathlib import Path
from entropic import ConfigLoader

PROMPTS_DIR = Path(__file__).parent / "prompts"

loader = ConfigLoader(
    project_root=Path("."),
    app_dir_name=".myapp",                          # Your app's config dir
    default_config_path=Path("data/default_config.yaml"),
    global_config_dir=None,                          # Skip ~/.entropic/
)
config = loader.load(cli_overrides={
    "prompts_dir": str(PROMPTS_DIR),
    "use_bundled_prompts": False,                    # Don't fall back to entropic defaults
})
```

**Config YAML requirements:**

| Section | Required | Notes |
|---------|----------|-------|
| `models.tiers` | Yes | Dict of tier_name -> model config |
| `models.default` | Yes | Must reference a defined tier |
| `models.router` | If routing enabled | Small model for classification |
| `routing.fallback_tier` | If tiers defined | Must reference a defined tier |
| `routing.tier_map` | No | Auto-derived from tier order |
| `routing.handoff_rules` | No | Default: all-to-all |
| `mcp.enable_*` | No | Disable built-in servers you don't need |

**Cross-validation enforced at load time:**
- `models.default` must exist in `models.tiers`
- `routing.fallback_tier` must exist in `models.tiers`
- `routing.tier_map` values must reference defined tiers
- `routing.handoff_rules` keys and values must reference defined tiers
- `compaction.warning_threshold_percent` must be less than `threshold_percent`

### 2. Logging

```python
from entropic import setup_logging, setup_model_logger

setup_logging(config, project_dir=PROJECT_ROOT, app_dir_name=".myapp")
setup_model_logger(project_dir=PROJECT_ROOT, app_dir_name=".myapp")
```

Creates `.myapp/session.log` and `.myapp/session_model.log`.

### 3. Identity Files

Each tier needs `prompts/identity_{tier_name}.md` with YAML frontmatter:

```yaml
---
name: suggest
focus:
  - "analyzing positions"
  - "proposing candidate moves"
examples:
  - "What's the best move here?"
  - "Evaluate this position"
---

# Suggest Tier

You are the move suggestion engine. Be brief.
```

| Field | Required | Used For |
|-------|----------|----------|
| `name` | Yes | Must match config tier name |
| `focus` | Yes | Auto-generates router classification prompt |
| `examples` | No | Few-shot examples for router |
| Body | Yes | Tier-specific system prompt |

The orchestrator reads identity files automatically during `initialize()`.

### 4. Tool Definitions

Create JSON files at `data/tools/{server_name}/{tool_name}.json`:

```json
{
  "name": "get_board",
  "description": "Get the current board state and legal moves.",
  "inputSchema": {
    "type": "object",
    "properties": {},
    "required": []
  }
}
```

### 5. Custom MCP Server

Subclass `BaseTool` for each tool, `BaseMCPServer` for the server:

```python
from entropic import BaseMCPServer, BaseTool

class GetBoardTool(BaseTool):
    def __init__(self, board):
        super().__init__(
            tool_name="get_board",
            server_prefix="myserver",
            tools_dir=Path("data/tools"),
        )
        self._board = board

    async def execute(self, arguments: dict) -> str:
        return json.dumps({"state": str(self._board)})


class MyServer(BaseMCPServer):
    def __init__(self, board):
        super().__init__("myserver")
        self.register_tool(GetBoardTool(board))
```

`BaseMCPServer.register_tool()` handles `get_tools()` and `execute_tool()` dispatch
automatically. No dict-dispatch boilerplate needed.

### 6. Orchestrator

```python
from entropic import ModelOrchestrator

orchestrator = ModelOrchestrator(config)
await orchestrator.initialize()
```

The orchestrator:
- Reads tier names from `config.models.tiers` keys
- Loads `identity_{tier}.md` for each tier (extracts focus/examples from frontmatter)
- Builds `ModelTier` objects automatically
- Loads backends on demand (only one main model in VRAM at a time)

### 7. Server Manager

```python
from entropic import ServerManager

my_server = MyServer(board)
server_manager = ServerManager(config, tier_names=orchestrator.tier_names)
server_manager.register_server(my_server)   # BEFORE initialize()
await server_manager.initialize()
```

`tier_names` is passed so the `entropic.handoff` tool knows which tiers exist.

### 8. Agent Engine

```python
from entropic import AgentEngine, EngineCallbacks, LoopConfig

loop_config = LoopConfig(auto_approve_tools=True)
engine = AgentEngine(orchestrator, server_manager, config, loop_config)

callbacks = EngineCallbacks(
    on_stream_chunk=lambda chunk: print(chunk, end=""),
    on_tier_selected=lambda tier: logger.info("Tier: %s", tier),
)
engine.set_callbacks(callbacks)
```

All callbacks are optional. Wire only what your UI needs.

### 9. Run the Loop

```python
async for msg in engine.run(prompt, system_prompt=context):
    pass  # Messages yielded as conversation progresses
```

The engine handles routing, generation, tool execution, and tier handoffs
automatically. Returns when the model stops or max iterations reached.

### 10. Shutdown

```python
await server_manager.shutdown()
await orchestrator.shutdown()
```

## Initialization Order

```
ConfigLoader.load()
    |
setup_logging() + setup_model_logger()
    |
ModelOrchestrator(config)
await orchestrator.initialize()
    |
ServerManager(config, tier_names=orchestrator.tier_names)
server_manager.register_server(my_server)    <-- BEFORE initialize
await server_manager.initialize()
    |
AgentEngine(orchestrator, server_manager, config, loop_config)
engine.set_callbacks(callbacks)
    |
engine.run(prompt, system_prompt=...)
    |
server_manager.shutdown()
orchestrator.shutdown()
```

## engine.run() Semantics

```python
async for msg in engine.run(prompt, system_prompt=context):
    # msg is a Message(role, content)
    # Roles: "system", "user", "assistant", "tool"
    pass
```

**Return type:** `AsyncIterator[Message]`. Yields complete messages (not partial
chunks — use `on_stream_chunk` callback for streaming).

**Termination:** The iterator stops (`StopAsyncIteration`) when:
- The model emits a stop token (`finish_reason="stop"`)
- `LoopConfig.max_iterations` is reached
- `LoopConfig.max_consecutive_errors` consecutive tool errors occur

**Re-entrant calls:** `engine.run()` can be called multiple times on the same
engine instance. Conversation history carries forward between runs.

**Callback exceptions:** If a callback raises an exception, it propagates up
through the engine and terminates the current run. Wrap callback logic in
try/except if you want fault-tolerant callbacks.

**Tool errors during execution:** If a tool's `execute()` raises, the engine
catches the exception, records it as a tool error result, and continues the loop
(up to `max_consecutive_errors`). The exception is logged but not re-raised.

## Tool JSON Path Convention

Tool definition JSON files must be located at:

```
{tools_dir}/{server_prefix}/{tool_name}.json
```

For example, a server with `server_prefix="chess"` and tool `"get_board"`:

```
data/tools/chess/get_board.json
```

`BaseTool.__init__()` calls `load_tool_definition(tool_name, server_prefix, tools_dir)`
which constructs this path and raises `FileNotFoundError` if the file doesn't exist,
or `ToolValidationError` if the JSON is malformed.

## allowed_tools Format

The `allowed_tools` field in tier config uses fully-qualified tool names:
`{server_name}.{tool_name}`.

```yaml
tiers:
  suggest:
    allowed_tools:
      - chess.get_board       # server_name.tool_name
      - entropic.handoff       # built-in handoff tool
```

`None` (default) means all tools are visible to the tier.

## Security Considerations

### Tool Approval

`LoopConfig(auto_approve_tools=True)` bypasses ALL permission prompts. The model
can execute any registered tool without user confirmation. Only use this when all
registered tools are safe for unsupervised execution.

### Built-in Servers

By default, entropic enables built-in servers for bash, filesystem, and git. For
library consumers, disable servers you don't need:

```yaml
mcp:
  enable_bash: false
  enable_filesystem: false
  enable_git: false
  enable_diagnostics: false
```

The bash server uses a blacklist (not allowlist) — it blocks known-dangerous
commands but cannot anticipate all risks. The filesystem server restricts
operations to the project root but can read any file within it.

### Per-Tier Tool Restrictions

Use `allowed_tools` to limit which tools each tier can see. This follows the
principle of least privilege — a tier that only needs to read data shouldn't
have access to write or execute tools:

```yaml
tiers:
  reader:
    allowed_tools:
      - myserver.get_data
      - entropic.handoff
  writer:
    allowed_tools:
      - myserver.get_data
      - myserver.write_data
      - entropic.handoff
```

### Error Sanitization

Tool errors (`str(exception)`) flow to the model unfiltered by default. If your
tools interact with sensitive data (credentials, PII, internal paths), use the
`error_sanitizer` callback to filter error text before the model sees it:

```python
def redact_secrets(error: str) -> str:
    """Strip sensitive patterns from error messages."""
    import re
    error = re.sub(r"password=\S+", "password=***", error)
    error = re.sub(r"/internal/\S+", "/***", error)
    return error

callbacks = EngineCallbacks(
    error_sanitizer=redact_secrets,
)
```

The raw error is always logged unfiltered for diagnostics — only the model-facing
message is sanitized.

### Config File Permissions

`config.local.yaml` is NOT gitignored by default. Saved permission patterns
(allow/deny lists from "Always Allow" prompts) persist to this file. Review
before committing to version control.

## Error Handling

**Config validation errors:** `ConfigLoader.load()` raises `pydantic.ValidationError`
with specific field paths and messages when config is invalid.

**Missing identity files:** When `use_bundled_prompts=False`, a missing
`identity_{tier}.md` file raises `FileNotFoundError` at generation time (not at
init). Ensure all configured tiers have identity files.

**Resource cleanup on partial failure:**

```python
orchestrator = ModelOrchestrator(config)
try:
    await orchestrator.initialize()
    server_manager = ServerManager(config, tier_names=orchestrator.tier_names)
    server_manager.register_server(my_server)
    await server_manager.initialize()
except Exception:
    await orchestrator.shutdown()  # Free GPU memory
    raise
```

If `orchestrator.initialize()` succeeds but later steps fail, you must still call
`orchestrator.shutdown()` to release GPU memory.

## Public API Surface

Exported from `entropic`:

| Symbol | Category | Purpose |
|--------|----------|---------|
| `AgentEngine` | Engine | Agentic loop |
| `AgentState` | Engine | Engine state enum |
| `EngineCallbacks` | Engine | Event handler dataclass |
| `LoopConfig` | Engine | Loop behavior config |
| `ModelOrchestrator` | Orchestrator | Model/tier management |
| `BackendFactory` | Orchestrator | Custom backend injection |
| `RoutingResult` | Orchestrator | Routing decision result |
| `ConfigLoader` | Config | Config hierarchy loader |
| `EntropyConfig` | Config | Root config schema |
| `ModelConfig` | Config | Per-model config |
| `ModelsConfig` | Config | All models config |
| `TierConfig` | Config | Per-tier config |
| `RoutingConfig` | Config | Routing config |
| `CompactionConfig` | Config | Context compaction config |
| `GenerationConfig` | Config | Generation params config |
| `Message` | Core | Chat message |
| `GenerationResult` | Core | Model output |
| `ModelBackend` | Core | Backend ABC |
| `ModelTier` | Core | Tier metadata |
| `ToolCall` | Core | Tool invocation |
| `ToolProvider` | Core | Tool provider ABC |
| `ToolResult` | Core | Tool execution result |
| `BaseMCPServer` | MCP | Server base class |
| `BaseTool` | MCP | Tool base class |
| `InProcessProvider` | MCP | In-process tool provider |
| `ServerManager` | MCP | Server lifecycle manager |
| `ServerResponse` | MCP | Structured tool response |
| `ToolRegistry` | MCP | Tool collection + dispatch |
| `ToolValidationError` | MCP | Invalid tool definition |
| `load_tool_definition` | MCP | Load tool JSON |
| `ChatAdapter` | Adapters | Chat format adapter ABC |
| `get_adapter` | Adapters | Look up adapter by name |
| `register_adapter` | Adapters | Register custom adapter |
| `TierIdentity` | Prompts | Identity file schema |
| `load_tier_identity` | Prompts | Parse identity frontmatter |
| `setup_logging` | Logging | Session logging |
| `setup_model_logger` | Logging | Model logging |
