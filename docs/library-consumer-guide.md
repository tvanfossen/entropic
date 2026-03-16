# Library Consumer Guide

How to embed entropic as an inference engine in your own application.
Reference implementation: `examples/pychess/`.

## Install

```bash
pip install entropic-engine          # Core inference engine
pip install entropic-engine[tui]     # Terminal UI (not needed for library use)
```

## Project Structure

```
my-app/
├── data/
│   ├── default_config.yaml          # App defaults (seeded to .myapp/)
│   ├── grammars/                    # GBNF grammar files
│   │   └── {tier_name}.gbnf
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
| `tiers.*.grammar` | No | Path to `.gbnf` file for output constraints |
| `tiers.*.auto_chain` | No | Chain to next tier on completion (default: false) |
| `tiers.*.enable_thinking` | No | Default true; false adds `/no-think` (Qwen3) |
| `tiers.*.identity` | No | Per-tier system prompt (None=bundled, False=disabled) |
| `tiers.*.allowed_tools` | No | Tool visibility filter (`server.tool` format) |
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

`tier_names` is passed so the `entropic.delegate` tool knows which tiers exist.

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

The engine handles routing, generation, tool execution, and delegation
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
engine instance. History is **not** stored internally — the consumer must capture
yielded messages and pass them back via the `history` parameter:

```python
history: list[Message] = []
async for msg in engine.run("Hello", history=history):
    history.append(msg)

# Next turn — pass accumulated history
async for msg in engine.run("Follow up", history=history):
    history.append(msg)
```

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
      - entropic.delegate       # built-in delegation tool
```

`None` (default) means all tools are visible to the tier.

## Delegation System

Tiers delegate work to other tiers using `entropic.delegate` (single role) or
`entropic.pipeline` (multi-stage chain). Delegation spawns an isolated child
inference loop — the child runs to completion with fresh context, then returns
its final message to the parent.

### Delegation Tools

| Tool | Purpose | Example |
|------|---------|---------|
| `entropic.delegate` | Single-role delegation | `delegate(target="eng", task="Fix the login bug")` |
| `entropic.pipeline` | Multi-stage chain | `pipeline(stages=["eng", "qa"], task="Implement and test feature")` |
| `entropic.todo_write` | Planning (required before delegation) | Create work plan with `target_tier` per item |

The delegate tool **rejects calls when no todos exist** — the delegating tier
must plan first using `todo_write`, then delegate.

### Child Loop Isolation

Each delegation creates:
- **Fresh context**: system prompt + task only — no parent conversation history
- **Git worktree** (if git repo): isolated filesystem for child modifications
- **Scoped state**: TodoList and context anchors saved/restored around child loop

On success, the worktree is merged back. On failure, it's discarded.

### Auto-Chain (Return to Parent)

Front-office tiers (eng, qa, arch, etc.) have `auto_chain: lead` in their
identity frontmatter. When a child loop completes (stop + no tool calls),
auto_chain fires — but in a delegation child, it signals `COMPLETE` instead
of performing a tier change. The child's final message returns to the parent.

### Grammar + Auto-Chain Interaction

Grammar and auto_chain compose with delegation:

| Config Combination | Behavior |
|---|---|
| `grammar` only | Structured output, tier is terminal |
| `auto_chain` only | Returns to parent on completion (delegation) or chains (root) |
| `grammar` + `auto_chain` | Chains on grammar completion (`stop`) |
| `auto_chain` at depth 0 | In-place tier change to `auto_chain` target |
| `auto_chain` at depth > 0 | Signals `COMPLETE`, returns to parent |

### Delegation Rules

```yaml
routing:
  handoff_rules:
    lead: [eng, qa, arch, ux, ui, analyst]  # Lead can delegate to these
    eng: [qa]                                # Eng can only delegate to QA
    qa: []                                   # QA is terminal
```

Empty rules (default) means all-to-all delegation is allowed.

### Delegation Callbacks

```python
callbacks = EngineCallbacks(
    on_delegation_start=lambda conv_id, tier, task: ...,
    on_delegation_complete=lambda conv_id, tier, summary, success: ...,
)
```

### Pipeline Pattern

Lead uses `entropic.pipeline` for multi-stage work. Each stage receives
the original task plus the previous stage's output:

```
pipeline(stages=["arch", "eng", "qa"], task="Design and implement login")

  arch receives: "Design and implement login"
  eng receives:  "Original task: ... Previous stage (arch) output: ..."
  qa receives:   "Original task: ... Previous stage (eng) output: ..."
```

If any stage fails, the pipeline stops and returns the failure to the parent.

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
      - entropic.delegate
  writer:
    allowed_tools:
      - myserver.get_data
      - myserver.write_data
      - entropic.delegate
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

## VRAM Model Lifecycle (P1-022)

Models transition through three states:

```
COLD → WARM → ACTIVE
         ↑       ↓
         └─ deactivate
```

| State | RAM | VRAM | Notes |
|-------|-----|------|-------|
| COLD | ✗ | ✗ | Not loaded |
| WARM | ✅ | ✗ | mlock pages in CPU RAM, fast GPU promotion |
| ACTIVE | ✅ | ✅ | Generating |

**`keep_warm`** enables the WARM state for a tier's model. At startup, the model is pre-loaded to CPU RAM. On swap-out, it deactivates to WARM (not fully unloaded), so GPU promotion (WARM→ACTIVE) takes ~1–3s instead of a full cold load from disk. Models without `keep_warm` go COLD on swap-out, freeing all VRAM and RAM.

```yaml
models:
  tiers:
    lead:
      path: ~/models/Qwen3.5-35B-A3B-Q2_K.gguf
      keep_warm: true    # Stay in WARM state when swapped out
      use_mlock: true    # Lock pages (default). Prevents OS swap.
```

**`vram_reserve_mb`** keeps headroom free to avoid OOM during swaps:

```yaml
library:
  vram_reserve_mb: 512   # Keep 512MB free (default)
```

`ModelState` is exported from `entropic.core` for consumers that need to inspect tier state:

```python
from entropic.core import ModelState

state = orchestrator.get_tier("thinking").backend.state
if state == ModelState.WARM:
    print("Thinking tier is pre-loaded, next request will be fast")
```

## Subsystem Injection (P2-019)

The engine's internal subsystems (`ToolExecutor`, `ResponseGenerator`, `ContextManager`) are constructed eagerly but can be replaced via post-construction assignment for testing or custom behavior:

```python
from entropic import AgentEngine
from entropic.core.tool_executor import ToolExecutor

engine = AgentEngine(orchestrator, server_manager, config, loop_config)

# Replace with a custom subclass after construction
engine._tool_executor = MyToolExecutor(
    server_manager=server_manager,
    orchestrator=orchestrator,
    config=config,
    callbacks=engine._callbacks,
)
```

This pattern is used in tests to inject mock subsystems:

```python
from unittest.mock import AsyncMock
from entropic.core.context_manager import ContextManager

engine._context_manager = AsyncMock(spec=ContextManager)
engine._context_manager.check_compaction.return_value = False
```

`EngineCallbacks` is a shared mutable dataclass — all subsystems hold a reference to the same instance. Call `engine.set_callbacks()` to update callbacks in place; all subsystems see the change immediately.

## Runtime MCP Registration (P2-026)

### `.mcp.json` Auto-Discovery

Entropic reads `.mcp.json` at `ServerManager.initialize()` time and connects any listed MCP servers automatically. This is the same file Claude Code uses, so third-party tool servers can be declared once:

```json
{
  "mcpServers": {
    "my-tool-server": {
      "type": "sse",
      "url": "http://127.0.0.1:9000/sse"
    },
    "my-stdio-server": {
      "type": "stdio",
      "command": "my-mcp-server",
      "args": ["--verbose"]
    }
  }
}
```

Supported transports: `sse`, `stdio`. YAML `mcp.external_servers` takes priority on name collision.

**Self-detection:** If an entry's socket path matches Entropic's own socket (derived from `project_dir`), it is skipped. This prevents circular connections when Entropic is listed in its own `.mcp.json`.

### Runtime connect/disconnect

Connect an external MCP server after the engine is running:

```python
# SSE server (HTTP)
tool_names = await engine.connect_server(
    name="pycommander",
    sse_url="http://127.0.0.1:6277/sse",
)
print(f"Connected: {tool_names}")

# stdio server (subprocess)
tool_names = await engine.connect_server(
    name="my-cli-tool",
    command="my-mcp-server",
    args=["--verbose"],
)

# Disconnect
await engine.disconnect_server("pycommander")
```

Runtime-connected tools appear in `list_tools()` immediately and follow per-tier `allowed_tools` filtering on the next turn.

```python
# Inspect all connected servers
servers = engine.server_manager.list_servers()
for name, info in servers.items():
    print(f"{name}: {info.transport} {info.status} ({info.source})")
```

`ServerInfo.source` is one of `"config"`, `"mcp_json"`, or `"runtime"`.

### Tool namespacing

All external MCP server tools are prefixed with the server name: `{server}.{tool}`. Add them explicitly to `allowed_tools` for tiers that need them:

```yaml
tiers:
  eng:
    allowed_tools:
      - filesystem.write_file
      - pycommander.device_info.query   # Runtime server tool, explicitly permitted
```

## Benchmark CLI (P1-029)

Layer 1 benchmarks measure raw model performance: load times, token/s, swap latency, and GPU sweep.

```bash
# Full Layer 1 benchmark
entropic benchmark run /path/to/model.gguf --layer1-only

# GPU layer sweep only
entropic benchmark sweep /path/to/model.gguf --max-layers 40

# Save results to JSON
entropic benchmark run /path/to/model.gguf --layer1-only --output results.json
```

**Layer 1 measures:**

| Metric | Description |
|--------|-------------|
| Cold load time | COLD → ACTIVE (disk read + GPU transfer) |
| Warm load time | WARM → ACTIVE (RAM → GPU only, validates `keep_warm`) |
| Token/s | Inference throughput at target GPU layers |
| Swap latency | ACTIVE → WARM → ACTIVE round-trip (validates <3s target) |
| GPU sweep | Token/s vs layers curve, OOM detection |

Results are printed as tables and optionally exported as JSON for comparison across model versions.

The benchmark bypasses the engine identity system — it uses `LlamaCppBackend` directly with no routing, tool execution, or context management. This gives clean baseline measurements unaffected by engine overhead.

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
| `ModelState` | Core | COLD/WARM/ACTIVE enum |
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
| `setup_model_logger` | Logging | Model logging |\n| `setup_display_logger` | Logging | Display mirror logging |
