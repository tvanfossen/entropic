# PyChess

Play chess against a local LLM via entropic's multi-tier pipeline.

## What This Demonstrates

PyChess is a reference consumer that exercises the engine's grammar + multi-tier
pipeline. It is **not** about chess quality — it demonstrates:

- **Multi-tier pipeline** — thinker tier analyzes, executor tier acts
- **GBNF grammar constraints** — each tier's output conforms to a structural grammar
- **Grammar-embedded handoff** — thinker grammar includes `entropic.handoff` tool call for explicit tier transition
- **Per-tier tool restrictions** — thinker can only handoff, executor can only make moves
- **Per-tier identity prompts** — separate system prompts per tier with frontmatter metadata
- **Auto-chain fallback** — if grammar handoff fails (all tool calls blocked), engine auto-chains as safety net

## Architecture

```
Human (White)
    │
    ▼
┌──────────────────────────────────────────────────┐
│  Thinker Tier                                    │
│  grammar: chess_thinker.gbnf                     │
│  Output: Threats → Candidates → Best move        │
│  Tools:  entropic.handoff (required, always last) │
│                                                   │
└──────────────┬───────────────────────────────────┘
               │ handoff tool call → stop_processing
               ▼
┌──────────────────────────────────────────────────┐
│  Executor Tier                                   │
│  grammar: chess_executor.gbnf                    │
│  Output: <tool_call>chess.make_move(uci)</tool_call> │
└──────────────┬───────────────────────────────────┘
               │ tool execution
               ▼
         Board updated
```

### Grammar Files

| File | Tier | Structure |
|------|------|-----------|
| `data/grammars/chess_thinker.gbnf` | thinker | Analysis → todo_writes (0+) → handoff (required last) |
| `data/grammars/chess_executor.gbnf` | executor | `<tool_call>{"name":"chess.make_move","arguments":{"move":"uci"}}</tool_call>` |

### Config → Tier Mapping

See `data/default_config.yaml` for the full configuration. Key fields:

```yaml
tiers:
  thinker:
    grammar: data/grammars/chess_thinker.gbnf
    auto_chain: true          # Fallback: chains on grammar stop if handoff fails
    enable_thinking: false    # /no-think for structured output
    allowed_tools:
      - entropic.handoff      # Explicit tier transition (grammar-embedded)
  executor:
    grammar: data/grammars/chess_executor.gbnf
    enable_thinking: false
    allowed_tools: [chess.make_move]
routing:
  handoff_rules:
    thinker: [executor]       # Handoff + auto-chain target
```

## Prerequisites

- A GGUF model file (the config defaults to `~/models/gguf/Qwen3-8B-Q4_K_M.gguf`)
- `entropic-engine` installed (the example uses an editable install)
- `python-chess` package

## Running

```bash
cd examples/pychess

# Create venv and install dependencies
python -m venv .venv
.venv/bin/pip install -e ../..       # entropic-engine (editable)
.venv/bin/pip install python-chess

# First run seeds .pychess/config.local.yaml — edit model paths if needed
.venv/bin/python main.py
```

You play as White (UCI notation, e.g. `e2e4`). The AI plays as Black.

## Files

| File | Purpose |
|------|---------|
| `main.py` | Game loop — human input, board display |
| `engine.py` | Wiring — orchestrator + server manager + agent engine |
| `config.py` | ConfigLoader setup with pychess-specific paths |
| `chess_server.py` | MCP server exposing `chess.make_move` tool |
| `data/default_config.yaml` | Model tiers, routing, grammar paths |
| `data/grammars/*.gbnf` | GBNF grammar files for each tier |
| `data/tools/chess/make_move.json` | Tool definition JSON |
| `prompts/constitution.md` | Shared system prompt |
| `prompts/identity_thinker.md` | Thinker tier identity + frontmatter |
| `prompts/identity_executor.md` | Executor tier identity + frontmatter |
