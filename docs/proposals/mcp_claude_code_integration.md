# Proposal: MCP Integration for Claude Code Delegation

**Status:** Draft (Refined)
**Branch:** `feature/mcp-claude-code-delegation`
**Author:** Generated via Claude Code session
**Date:** 2026-01-27
**Refined:** 2026-01-28

---

## Summary

Add an MCP server to Entropi that allows Claude Code to delegate tasks to local models. This extends the practical utility of Claude Pro subscriptions by offloading work to free local inference, while maintaining frontier-level orchestration.

**This is a secondary feature.** Entropi's primary purpose remains a standalone local AI coding assistant comparable to Claude Code.

---

## Motivation

- Claude Pro subscription ($20/month) has usage limits that active users hit quickly
- Many coding tasks don't require frontier-level reasoning
- Local models can handle implementation work adequately
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
│                   │ Orchestrat. │ ◄── Local models (configured)     │
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

### Message Priority & Preemption

Human input always preempts Claude Code. When human types during a Claude Code task:
1. Current generation **pauses** (not cancelled) - partial output saved
2. Human message processed to completion
3. Claude Code task **resumes** from saved state
4. If human's input invalidates the task, Claude Code can call `cancel`

**Preemption response to Claude Code:**
```json
{
  "status": "preempted",
  "task_id": "task_abc123",
  "partial_response": "Created Board class with...",
  "queue_position": 2,
  "hint": "Human interaction in progress. Poll for completion or cancel."
}
```

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

### Design: Async with Polling

Tasks are submitted asynchronously to avoid blocking Claude Code during long operations:
1. `chat` returns immediately with a `task_id`
2. Claude Code polls with `poll_task` or waits for MCP notification
3. On completion, full response with structured tool results returned

This allows Claude Code to:
- Submit multiple tasks in parallel
- Cancel tasks that are no longer needed
- Receive progress updates via notifications

### Tools Exposed to Claude Code

#### `chat`

Submit a message to Entropi. Returns immediately with task_id.

```json
{
  "name": "chat",
  "description": "Submit a task to Entropi, a local AI coding assistant. Use for tasks that don't require frontier-level reasoning: implementing functions, writing boilerplate, running tests, file operations. Runs locally (free). Returns immediately with task_id - use poll_task to get results. Conversation is shared with human user.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "message": {
        "type": "string",
        "description": "Your message to Entropi"
      },
      "context": {
        "type": "array",
        "description": "Optional file contents to include as context (avoids Entropi re-reading)",
        "items": {
          "type": "object",
          "properties": {
            "path": {"type": "string"},
            "content": {"type": "string"}
          }
        }
      },
      "hint": {
        "type": "string",
        "description": "Optional hint about task requirements",
        "enum": ["code_generation", "code_review", "refactoring", "testing", "documentation", "general"]
      },
      "wait": {
        "type": "boolean",
        "description": "If true, block until completion (default: false)",
        "default": false
      }
    },
    "required": ["message"]
  }
}
```

**Response (async):**
```json
{
  "task_id": "task_abc123",
  "status": "queued",
  "queue_position": 0
}
```

**Response (wait=true or completed):**
```json
{
  "task_id": "task_abc123",
  "status": "completed",
  "response": "Created Board class with 8x8 grid...",
  "tool_calls": [
    {"tool": "write_file", "path": "src/board.py", "status": "success", "lines": 145},
    {"tool": "bash", "command": "python -m pytest tests/", "status": "success", "exit_code": 0}
  ],
  "token_usage": {"input": 1250, "output": 3400}
}
```

#### `poll_task`

Check status of a submitted task.

```json
{
  "name": "poll_task",
  "description": "Check the status of a previously submitted task. Returns current status and results if complete.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "task_id": {
        "type": "string",
        "description": "Task ID returned from chat"
      }
    },
    "required": ["task_id"]
  }
}
```

**Response:**
```json
{
  "task_id": "task_abc123",
  "status": "in_progress",
  "progress": "Executing tool: write_file",
  "elapsed_seconds": 12
}
```

#### `cancel`

Cancel a queued or in-progress task.

```json
{
  "name": "cancel",
  "description": "Cancel a queued or in-progress task. Use when requirements change or task is no longer needed.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "task_id": {
        "type": "string",
        "description": "Task ID to cancel"
      },
      "reason": {
        "type": "string",
        "description": "Optional reason for cancellation (logged for analysis)"
      }
    },
    "required": ["task_id"]
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

#### `get_capabilities`

Query Entropi's available models and tools.

```json
{
  "name": "get_capabilities",
  "description": "Query what models and tools Entropi has available. Use to understand what tasks can be delegated effectively.",
  "inputSchema": {
    "type": "object",
    "properties": {}
  }
}
```

**Response:**
```json
{
  "models": [
    {"name": "current-model", "context_length": 32768, "strengths": ["code_generation", "refactoring"]}
  ],
  "tools": ["read_file", "write_file", "bash", "glob", "grep", "lsp_diagnostics"],
  "max_concurrent_tasks": 1,
  "rate_limit": {"requests_per_minute": 10}
}
```
*Note: Actual model details provided dynamically by Entropi based on loaded configuration.*

#### `report_issue`

Report quality issues for logging/improvement.

```json
{
  "name": "report_issue",
  "description": "Report a quality issue with Entropi's response. Logged for analysis and model improvement.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "task_id": {"type": "string"},
      "issue_type": {
        "type": "string",
        "enum": ["incorrect", "incomplete", "hallucination", "off_topic", "slow", "other"]
      },
      "description": {"type": "string"},
      "expected": {"type": "string", "description": "What you expected instead"}
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
  "description": "Check Entropi's current status including queue depth, active tasks, and system health.",
  "inputSchema": {
    "type": "object",
    "properties": {}
  }
}
```

**Response:**
```json
{
  "state": "busy",
  "active_task": {"task_id": "task_abc123", "elapsed_seconds": 5},
  "queue_depth": 2,
  "model_loaded": true,
  "gpu_utilization": 0.85
}
```

### MCP Notifications (Server → Client)

Entropi sends notifications for real-time updates:

#### `task_progress`
```json
{
  "method": "notifications/task_progress",
  "params": {
    "task_id": "task_abc123",
    "progress": "Writing file src/board.py...",
    "percent": 60
  }
}
```

#### `task_completed`
```json
{
  "method": "notifications/task_completed",
  "params": {
    "task_id": "task_abc123",
    "status": "completed",
    "response": "...",
    "tool_calls": [...]
  }
}
```

#### `task_preempted`
```json
{
  "method": "notifications/task_preempted",
  "params": {
    "task_id": "task_abc123",
    "reason": "human_input",
    "partial_response": "...",
    "will_resume": true
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

2. **Task Manager** (`src/entropi/core/tasks.py`)
   - Task lifecycle: queued → in_progress → completed/cancelled/failed
   - Task state persistence (survives preemption)
   - Progress tracking and elapsed time
   - Structured tool call result collection

3. **Session Manager** (`src/entropi/core/session.py`)
   - SQLite-based persistence
   - Save/load conversation state
   - History retrieval for MCP

4. **Engine Updates** (`src/entropi/core/engine.py`)
   - Accept messages from queue (not just direct input)
   - Handle preemption gracefully (pause/resume task state)
   - Track message source in metadata
   - Report tool calls back to task manager

### Phase 2: MCP Server

5. **External MCP Server** (`src/entropi/mcp/servers/external.py`)
   - Implements 7 tools: chat, poll_task, cancel, get_history, get_capabilities, report_issue, status
   - Implements 3 notifications: task_progress, task_completed, task_preempted
   - Listens on Unix socket
   - Routes to message queue
   - Rate limiting (token bucket)

6. **MCP Bridge** (`src/entropi/mcp/bridge.py`)
   - `entropi --mcp-bridge` mode
   - Bridges stdio ↔ socket
   - Minimal footprint, no TUI/agent

7. **Socket Server Integration** (`src/entropi/app.py`)
   - Start socket server on Entropi launch
   - Handle client connections
   - Cleanup on shutdown

### Phase 3: TUI Integration

8. **Source Attribution** (`src/entropi/ui/tui.py`)
   - Show `[you]` vs `[claude-code]` prefixes
   - Visual distinction (color/style)
   - Queue status indicator
   - Active task display

9. **Configuration** (`src/entropi/config/schema.py`)
   - Add `mcp.external.enabled`
   - Add `mcp.external.socket_path`
   - Add `mcp.external.rate_limit`

---

## File Changes

| File | Type | Description |
|------|------|-------------|
| `src/entropi/core/queue.py` | New | Message queue with priority, task tracking |
| `src/entropi/core/session.py` | New | Session persistence |
| `src/entropi/core/tasks.py` | New | Task state management (async task lifecycle) |
| `src/entropi/mcp/servers/external.py` | New | External MCP server (7 tools + notifications) |
| `src/entropi/mcp/bridge.py` | New | stdio ↔ socket bridge |
| `src/entropi/core/engine.py` | Modify | Queue integration, preemption, task callbacks |
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

1. ~~**MCP Notifications**~~ **RESOLVED**: Using async design with notifications. `chat` returns immediately with task_id; notifications sent for progress/completion/preemption.

2. ~~**Preemption UX**~~ **RESOLVED**: Return structured preemption response with partial_response, queue_position, and will_resume flag.

3. **Socket Security** - Socket created with `chmod 600`. Also verify `SO_PEERCRED` to ensure connecting UID matches socket owner.

4. ~~**Rate Limiting**~~ **RESOLVED**: Yes, implement token bucket. Configurable via `mcp.external.rate_limit`.

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

- [x] Entropi starts with MCP socket server automatically (when `mcp.external.enabled: true`)
- [x] Claude Code can connect via `entropi mcp-bridge`
- [x] `chat` tool works end-to-end (async with poll_task)
- [x] Human and Claude Code messages share context (via MessageSource tracking)
- [x] Human input preempts Claude Code tasks (via MessageQueue priority)
- [x] Session persists across restarts (via SessionManager with SQLite)
- [x] No performance regression for standalone usage (external MCP is opt-in)

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
