# Proposal: MCP Integration for Claude Code Delegation

**Status:** Draft
**Branch:** `feature/mcp-claude-code-delegation`
**Author:** Generated via Claude Code session
**Date:** 2026-01-27

---

## Summary

Add an MCP server to Entropi that allows Claude Code to delegate tasks to local models. This extends the practical utility of Claude Pro subscriptions by offloading work to free local inference, while maintaining frontier-level orchestration.

**This is a secondary feature.** Entropi's primary purpose remains a standalone local AI coding assistant comparable to Claude Code.

---

## Motivation

- Claude Pro subscription ($20/month) has usage limits that active users hit quickly
- Many coding tasks don't require frontier-level reasoning
- Local models (Qwen 8B/14B) can handle implementation work adequately
- Users want to maximize value from their subscription

**Value proposition:** Claude Code handles orchestration and complex reasoning; Entropi handles grunt work for free.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Entropi Process                             │
│                                                                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌────────────┐  │
│  │     TUI     │  │   Agent     │  │   Session   │  │    MCP     │  │
│  │  (Textual)  │  │   Engine    │  │   Manager   │  │   Server   │  │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬─────┘  │
│         │                │                │                │        │
│         │                ▼                │                │        │
│         │         ┌─────────────┐         │                │        │
│         └────────►│   Message   │◄────────┘                │        │
│                   │    Queue    │◄──────────────────────────┘        │
│                   └─────────────┘                                   │
│                         │                                           │
│                         ▼                                           │
│                   ┌─────────────┐                                   │
│                   │ Orchestrat. │ ◄── Local models (Qwen, etc.)     │
│                   └─────────────┘                                   │
│                                                                     │
│  Socket: ~/.entropi/mcp.sock                                        │
└─────────────────────────────────────────────────────────────────────┘
         ▲
         │ stdio ↔ socket bridge
         │
┌────────┴────────┐
│ entropi         │
│ --mcp-bridge    │  ◄── Spawned by Claude Code
└────────┬────────┘
         │ stdio (MCP protocol)
         ▼
┌─────────────────┐
│   Claude Code   │
└─────────────────┘
```

---

## Design Decisions

### Shared Context

Human and Claude Code share the same conversation context. Both can see each other's messages in the TUI. This is intentional:
- Enables collaboration between human and Claude Code
- Human can observe what Claude Code delegates
- Claude Code can use `get_history` to see human's prior work

### Message Priority

Human input always preempts Claude Code. When human types during a Claude Code task:
1. Current generation pauses
2. Human message processed
3. Claude Code task resumes

### Failure Handling

Entropi does its best. If local model produces poor output:
- Claude Code decides whether to retry, take over, or accept
- Claude Code can call `report_issue` to log quality problems
- Entropi does not escalate automatically

### Session Persistence

Conversation state survives Entropi restarts:
- Messages stored in `~/.entropi/sessions/`
- Session restored on startup
- Claude Code can reconnect and see prior context via `get_history`

---

## MCP Interface

### Tools Exposed to Claude Code

#### `chat`

Send a message to Entropi and receive a response.

```json
{
  "name": "chat",
  "description": "Send a message to Entropi, a local AI coding assistant. Use for tasks that don't require frontier-level reasoning: implementing functions, writing boilerplate, running tests, file operations. Runs locally (free). Conversation is shared with human user.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "message": {
        "type": "string",
        "description": "Your message to Entropi"
      }
    },
    "required": ["message"]
  }
}
```

#### `get_history`

Retrieve recent conversation history.

```json
{
  "name": "get_history",
  "description": "Get recent conversation history from Entropi, including messages from both you and the human user.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "limit": {
        "type": "integer",
        "default": 20
      },
      "include_tool_results": {
        "type": "boolean",
        "default": true
      }
    }
  }
}
```

#### `report_issue`

Report quality issues for logging/improvement.

```json
{
  "name": "report_issue",
  "description": "Report a quality issue with Entropi's response.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "message_id": {"type": "string"},
      "issue_type": {
        "type": "string",
        "enum": ["incorrect", "incomplete", "hallucination", "off_topic", "other"]
      },
      "description": {"type": "string"}
    },
    "required": ["issue_type", "description"]
  }
}
```

#### `status`

Check Entropi's current state.

```json
{
  "name": "status",
  "description": "Check Entropi's current status (idle, busy, queue depth).",
  "inputSchema": {
    "type": "object",
    "properties": {}
  }
}
```

---

## Implementation Plan

### Phase 1: Core Infrastructure

1. **Message Queue** (`src/entropi/core/queue.py`)
   - Priority queue with HUMAN > CLAUDE_CODE
   - Preemption signaling
   - Async callback support for responses

2. **Session Manager** (`src/entropi/core/session.py`)
   - SQLite-based persistence
   - Save/load conversation state
   - History retrieval for MCP

3. **Engine Updates** (`src/entropi/core/engine.py`)
   - Accept messages from queue (not just direct input)
   - Handle preemption gracefully
   - Track message source in metadata

### Phase 2: MCP Server

4. **External MCP Server** (`src/entropi/mcp/servers/external.py`)
   - Implements chat, get_history, report_issue, status
   - Listens on Unix socket
   - Routes to message queue

5. **MCP Bridge** (`src/entropi/mcp/bridge.py`)
   - `entropi --mcp-bridge` mode
   - Bridges stdio ↔ socket
   - Minimal footprint, no TUI/agent

6. **Socket Server Integration** (`src/entropi/app.py`)
   - Start socket server on Entropi launch
   - Handle client connections
   - Cleanup on shutdown

### Phase 3: TUI Integration

7. **Source Attribution** (`src/entropi/ui/tui.py`)
   - Show `[you]` vs `[claude-code]` prefixes
   - Visual distinction (color/style)
   - Queue status indicator

8. **Configuration** (`src/entropi/config/schema.py`)
   - Add `mcp.external.enabled`
   - Add `mcp.external.socket_path`

---

## File Changes

| File | Type | Description |
|------|------|-------------|
| `src/entropi/core/queue.py` | New | Message queue with priority |
| `src/entropi/core/session.py` | New | Session persistence |
| `src/entropi/mcp/servers/external.py` | New | External MCP server |
| `src/entropi/mcp/bridge.py` | New | stdio ↔ socket bridge |
| `src/entropi/core/engine.py` | Modify | Queue integration, preemption |
| `src/entropi/app.py` | Modify | Start external MCP server |
| `src/entropi/ui/tui.py` | Modify | Source attribution display |
| `src/entropi/config/schema.py` | Modify | External MCP config |
| `src/entropi/__main__.py` | Modify | `--mcp-bridge` flag |

---

## Usage

### Starting Entropi

```bash
$ entropi
# TUI launches, MCP server starts automatically on ~/.entropi/mcp.sock
```

### Claude Code Configuration

Create `.mcp.json` in project directory:

```json
{
  "mcpServers": {
    "entropi": {
      "type": "stdio",
      "command": "entropi",
      "args": ["--mcp-bridge"]
    }
  }
}
```

### Example Interaction

```
TUI View:
─────────────────────────────────────────────────────
[you]: Help me build a chess application

[entropi]: I'll help you design a chess application...

[claude-code]: Implement the Board class in src/board.py
               with 8x8 grid, get_piece, set_piece methods.

[entropi]: I'll create the Board class...
           [filesystem.write_file: src/board.py]
           Created Board class with 145 lines...

[you]: What did Claude Code ask for?

[entropi]: Claude Code requested I implement a Board class...
─────────────────────────────────────────────────────
```

---

## Open Questions

1. **MCP Notifications** - Does Claude Code support server-initiated notifications? If yes, we could return immediately from `chat` and notify on completion. If no, `chat` blocks until complete (current design).

2. **Preemption UX** - When human preempts, Claude Code's `chat` call blocks longer. Should we return a specific status? Timeout with retry hint?

3. **Socket Security** - Socket created with `chmod 600`. Sufficient for local-only access?

4. **Rate Limiting** - Should Entropi rate-limit Claude Code requests to prevent resource exhaustion?

---

## Testing Strategy

1. **Unit tests** for queue, session manager
2. **Integration tests** for MCP server (mock socket client)
3. **Manual testing** with actual Claude Code connection
4. **Load testing** for concurrent human + Claude Code interaction

---

## Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Local model quality insufficient | Medium | Medium | Claude Code can take over; report_issue logging |
| IPC complexity causes bugs | Medium | High | Thorough testing; simple protocol |
| Feature unused (not valuable) | Medium | Low | Isolated on branch; easy to abandon |
| Anthropic changes MCP support | Low | High | Monitor Claude Code updates |

---

## Success Criteria

- [ ] Entropi starts with MCP socket server automatically
- [ ] Claude Code can connect via `--mcp-bridge`
- [ ] `chat` tool works end-to-end
- [ ] Human and Claude Code messages appear in same TUI
- [ ] Human input preempts Claude Code tasks
- [ ] Session persists across restarts
- [ ] No performance regression for standalone usage

---

## Branch Strategy

Development on `feature/mcp-claude-code-delegation` branch.

```bash
git checkout -b feature/mcp-claude-code-delegation
```

Merge to main only after:
1. Core functionality complete
2. Manual testing with Claude Code successful
3. No regressions to primary TUI functionality

This feature is experimental. If it proves not useful, the branch can be abandoned without impacting main.
