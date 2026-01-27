# Entropi as External MCP Server for Claude Code

## Executive Summary

This proposal outlines exposing Entropi as an MCP server that external AI assistants (specifically Claude Code) can connect to, enabling orchestration of local AI agents. This creates a powerful architecture where Claude Code can delegate tasks to locally-running models while maintaining security through Constitutional AI governance.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Claude Code (Remote)                         │
│                                                                      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐  │
│  │ User Request │→ │ Task Analysis│→ │ Delegate to Local Agent? │  │
│  └──────────────┘  └──────────────┘  └────────────┬─────────────┘  │
│                                                    │                 │
└────────────────────────────────────────────────────┼─────────────────┘
                                                     │ MCP Protocol
                                                     ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      Entropi MCP Server (Local)                      │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────────┐ │
│  │                    Constitutional AI Layer                      │ │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐ │ │
│  │  │   Policies  │  │  Principles │  │  Critique & Validation  │ │ │
│  │  └─────────────┘  └─────────────┘  └─────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────────┘ │
│                                │                                     │
│                                ▼                                     │
│  ┌────────────────────────────────────────────────────────────────┐ │
│  │                      Agent Orchestrator                         │ │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐ │ │
│  │  │ Task Queue  │  │ Agent Pool  │  │  Result Aggregation     │ │ │
│  │  └─────────────┘  └─────────────┘  └─────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────────┘ │
│                                │                                     │
│                                ▼                                     │
│  ┌────────────────────────────────────────────────────────────────┐ │
│  │                      Local Resources                            │ │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐ │ │
│  │  │ Local LLMs  │  │ Filesystem  │  │  Git / Bash / Tools     │ │ │
│  │  └─────────────┘  └─────────────┘  └─────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────────┘ │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

## Use Cases

### 1. Local Agent Delegation
Claude Code delegates compute-intensive or long-running tasks to Entropi:
```
Claude Code: "Analyze this codebase for security vulnerabilities"
    → Entropi spawns local agent with security analysis prompt
    → Agent runs locally, no API costs
    → Results returned to Claude Code
```

### 2. Privacy-Sensitive Operations
Keep sensitive code/data local:
```
Claude Code: "Refactor the authentication module"
    → Entropi handles locally (code never leaves machine)
    → Only high-level summary returned to Claude Code
```

### 3. Parallel Agent Execution
Spawn multiple local agents simultaneously:
```
Claude Code: "Run tests, lint, and build in parallel"
    → Entropi spawns 3 concurrent agents
    → Returns aggregated results
```

### 4. Hybrid Workflows
Combine Claude Code's capabilities with local execution:
```
Claude Code: "I'll design the API, you implement it locally"
    → Claude Code provides specifications
    → Entropi generates code with local models
    → Results reviewed by Claude Code
```

---

## Constitutional AI as Gatekeeper

### Why Constitutional AI Must Come First

When exposing Entropi to external control, Constitutional AI becomes **critical infrastructure**, not optional:

1. **Trust Boundary**: Claude Code is a remote service sending arbitrary commands
2. **Execution Context**: Commands execute with user's local permissions
3. **Attack Surface**: MCP protocol could be exploited if compromised
4. **Data Exfiltration**: Must prevent sensitive data leaving the machine

### Constitutional Layers for External Access

```
┌─────────────────────────────────────────────────────────────┐
│                    Layer 1: Protocol Security                │
│  - Authentication (API key, certificate)                     │
│  - Rate limiting                                             │
│  - Request validation                                        │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Layer 2: Policy Enforcement               │
│  - Allowed operations whitelist                              │
│  - Blocked patterns (rm -rf, etc.)                          │
│  - Path restrictions (project directory only)               │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Layer 3: Constitutional Principles        │
│  - No data exfiltration                                     │
│  - No destructive operations without confirmation           │
│  - Respect project boundaries                               │
│  - Audit logging                                            │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Layer 4: Output Validation                │
│  - Sensitive data redaction                                 │
│  - Response size limits                                     │
│  - Content classification                                   │
└─────────────────────────────────────────────────────────────┘
```

### External Access Constitution

```markdown
# External Access Constitutional Principles

## Scope Restrictions
- Operations MUST be limited to the configured project directory
- No access to parent directories or system paths
- No network operations except to localhost

## Data Protection
- NEVER include contents of .env, credentials, or secret files in responses
- REDACT API keys, tokens, and passwords from all outputs
- File contents stay local unless explicitly summarized

## Operation Limits
- No destructive filesystem operations (delete, overwrite) without user confirmation
- No git push, force operations, or remote modifications
- No package installation or system modifications
- No arbitrary code execution outside sandboxed context

## Transparency
- ALL operations MUST be logged with full audit trail
- User MUST be notified of external access attempts
- User can revoke access at any time

## Resource Limits
- Maximum concurrent agents: 3
- Maximum execution time per request: 5 minutes
- Maximum output size: 100KB per response
```

---

## MCP Server Implementation

### Tool Definitions

```python
# src/entropi/mcp/external_server.py

EXTERNAL_TOOLS = [
    {
        "name": "entropi.spawn_agent",
        "description": "Spawn a local AI agent to perform a task",
        "inputSchema": {
            "type": "object",
            "properties": {
                "task": {
                    "type": "string",
                    "description": "Task description for the agent"
                },
                "context": {
                    "type": "string",
                    "description": "Additional context (file paths, requirements)"
                },
                "model_tier": {
                    "type": "string",
                    "enum": ["thinking", "normal", "code", "simple"],
                    "description": "Which model tier to use"
                },
                "max_iterations": {
                    "type": "integer",
                    "default": 10,
                    "description": "Maximum agentic loop iterations"
                },
                "timeout_seconds": {
                    "type": "integer",
                    "default": 300,
                    "description": "Maximum execution time"
                }
            },
            "required": ["task"]
        }
    },
    {
        "name": "entropi.query_agent",
        "description": "Ask a question answered by local AI (no tools, just inference)",
        "inputSchema": {
            "type": "object",
            "properties": {
                "question": {
                    "type": "string",
                    "description": "Question to answer"
                },
                "context_files": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Files to include as context"
                }
            },
            "required": ["question"]
        }
    },
    {
        "name": "entropi.list_agents",
        "description": "List currently running agents and their status",
        "inputSchema": {
            "type": "object",
            "properties": {}
        }
    },
    {
        "name": "entropi.get_agent_result",
        "description": "Get results from a completed or running agent",
        "inputSchema": {
            "type": "object",
            "properties": {
                "agent_id": {
                    "type": "string",
                    "description": "Agent ID from spawn_agent"
                },
                "wait": {
                    "type": "boolean",
                    "default": false,
                    "description": "Wait for agent to complete"
                }
            },
            "required": ["agent_id"]
        }
    },
    {
        "name": "entropi.cancel_agent",
        "description": "Cancel a running agent",
        "inputSchema": {
            "type": "object",
            "properties": {
                "agent_id": {
                    "type": "string",
                    "description": "Agent ID to cancel"
                }
            },
            "required": ["agent_id"]
        }
    },
    {
        "name": "entropi.project_summary",
        "description": "Get a summary of the current project structure and context",
        "inputSchema": {
            "type": "object",
            "properties": {
                "include_git_status": {
                    "type": "boolean",
                    "default": true
                },
                "include_file_tree": {
                    "type": "boolean",
                    "default": true
                },
                "max_depth": {
                    "type": "integer",
                    "default": 3
                }
            }
        }
    }
]
```

### Server Implementation

```python
# src/entropi/mcp/external_server.py

import asyncio
import json
import uuid
from dataclasses import dataclass, field
from enum import Enum
from typing import Any

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import Tool, TextContent

from entropi.config.loader import get_config
from entropi.constitutional import ConstitutionalGuard
from entropi.core.engine import AgentEngine, LoopConfig


class AgentStatus(Enum):
    PENDING = "pending"
    RUNNING = "running"
    COMPLETED = "completed"
    FAILED = "failed"
    CANCELLED = "cancelled"


@dataclass
class AgentTask:
    """Represents a spawned agent task."""
    id: str
    task: str
    status: AgentStatus = AgentStatus.PENDING
    result: str | None = None
    error: str | None = None
    created_at: float = field(default_factory=time.time)
    completed_at: float | None = None


class EntropiExternalServer:
    """MCP Server exposing Entropi to external AI assistants."""

    def __init__(self):
        self.config = get_config()
        self.constitutional = ConstitutionalGuard(self.config)
        self.agents: dict[str, AgentTask] = {}
        self.server = Server("entropi")
        self._setup_handlers()

    def _setup_handlers(self):
        """Register MCP tool handlers."""

        @self.server.list_tools()
        async def list_tools() -> list[Tool]:
            return [Tool(**t) for t in EXTERNAL_TOOLS]

        @self.server.call_tool()
        async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
            # Constitutional check FIRST
            violation = await self.constitutional.check_external_request(name, arguments)
            if violation:
                return [TextContent(
                    type="text",
                    text=f"Constitutional violation: {violation.reason}"
                )]

            # Route to handler
            handler = getattr(self, f"_handle_{name.replace('.', '_')}", None)
            if not handler:
                return [TextContent(type="text", text=f"Unknown tool: {name}")]

            try:
                result = await handler(arguments)
                return [TextContent(type="text", text=result)]
            except Exception as e:
                return [TextContent(type="text", text=f"Error: {e}")]

    async def _handle_entropi_spawn_agent(self, args: dict[str, Any]) -> str:
        """Spawn a new agent task."""
        agent_id = str(uuid.uuid4())[:8]

        task = AgentTask(
            id=agent_id,
            task=args["task"],
            status=AgentStatus.PENDING,
        )
        self.agents[agent_id] = task

        # Start agent in background
        asyncio.create_task(self._run_agent(task, args))

        return json.dumps({
            "agent_id": agent_id,
            "status": "spawned",
            "message": f"Agent spawned. Use entropi.get_agent_result with id '{agent_id}' to check status."
        })

    async def _run_agent(self, task: AgentTask, args: dict[str, Any]) -> None:
        """Run agent in background."""
        task.status = AgentStatus.RUNNING

        try:
            engine = AgentEngine(
                config=self.config,
                loop_config=LoopConfig(
                    max_iterations=args.get("max_iterations", 10),
                    timeout_seconds=args.get("timeout_seconds", 300),
                ),
            )
            await engine.initialize()

            # Run the agent loop
            result_parts = []
            async for msg in engine.run(task.task):
                if msg.role == "assistant" and msg.content:
                    result_parts.append(msg.content)

            task.result = "\n".join(result_parts)
            task.status = AgentStatus.COMPLETED

        except asyncio.CancelledError:
            task.status = AgentStatus.CANCELLED
        except Exception as e:
            task.status = AgentStatus.FAILED
            task.error = str(e)
        finally:
            task.completed_at = time.time()

    async def _handle_entropi_get_agent_result(self, args: dict[str, Any]) -> str:
        """Get agent results."""
        agent_id = args["agent_id"]
        task = self.agents.get(agent_id)

        if not task:
            return json.dumps({"error": f"No agent found with id '{agent_id}'"})

        if args.get("wait") and task.status == AgentStatus.RUNNING:
            # Wait for completion (with timeout)
            timeout = 60
            start = time.time()
            while task.status == AgentStatus.RUNNING and (time.time() - start) < timeout:
                await asyncio.sleep(1)

        # Redact sensitive data from result
        result = self.constitutional.redact_sensitive(task.result) if task.result else None

        return json.dumps({
            "agent_id": agent_id,
            "status": task.status.value,
            "result": result,
            "error": task.error,
            "duration_seconds": (task.completed_at - task.created_at) if task.completed_at else None,
        })

    async def run(self):
        """Run the MCP server."""
        async with stdio_server() as (read_stream, write_stream):
            await self.server.run(
                read_stream,
                write_stream,
                self.server.create_initialization_options(),
            )


def main():
    """Entry point for entropi-server command."""
    server = EntropiExternalServer()
    asyncio.run(server.run())
```

### Constitutional Guard for External Access

```python
# src/entropi/constitutional.py

import re
from dataclasses import dataclass
from typing import Any

from entropi.config.schema import EntropyConfig
from entropi.core.logging import get_logger

logger = get_logger("constitutional")


@dataclass
class Violation:
    """Represents a constitutional violation."""
    principle: str
    reason: str
    severity: str  # "block", "warn", "log"


class ConstitutionalGuard:
    """Guards external access according to constitutional principles."""

    # Patterns that should NEVER appear in external requests
    BLOCKED_PATTERNS = [
        (r"rm\s+(-rf?|--recursive)\s+[/~]", "Destructive recursive delete"),
        (r">\s*/dev/sd[a-z]", "Direct disk write"),
        (r"curl.*\|\s*(ba)?sh", "Pipe to shell execution"),
        (r"wget.*\|\s*(ba)?sh", "Pipe to shell execution"),
        (r"mkfs\.", "Filesystem format"),
        (r"dd\s+if=", "Direct disk manipulation"),
        (r":(){ :|:& };:", "Fork bomb"),
        (r"chmod\s+777", "Overly permissive chmod"),
        (r"sudo\s+", "Sudo elevation"),
    ]

    # Patterns for sensitive data redaction
    REDACT_PATTERNS = [
        (r'(?i)(api[_-]?key|apikey)\s*[=:]\s*["\']?[\w-]+["\']?', "[REDACTED_API_KEY]"),
        (r'(?i)(password|passwd|pwd)\s*[=:]\s*["\']?[^\s"\']+["\']?', "[REDACTED_PASSWORD]"),
        (r'(?i)(secret|token)\s*[=:]\s*["\']?[\w-]+["\']?', "[REDACTED_SECRET]"),
        (r'(?i)(aws_access_key_id)\s*[=:]\s*["\']?[\w]+["\']?', "[REDACTED_AWS_KEY]"),
        (r'-----BEGIN [A-Z ]+ PRIVATE KEY-----[\s\S]*?-----END [A-Z ]+ PRIVATE KEY-----', "[REDACTED_PRIVATE_KEY]"),
    ]

    # Files that should never be read or returned
    FORBIDDEN_FILES = [
        ".env",
        ".env.local",
        ".env.production",
        "credentials.json",
        "secrets.yaml",
        ".git/config",  # May contain credentials
        "id_rsa",
        "id_ed25519",
    ]

    def __init__(self, config: EntropyConfig):
        self.config = config
        self.project_root = config.config_dir.parent

    async def check_external_request(
        self,
        tool_name: str,
        arguments: dict[str, Any],
    ) -> Violation | None:
        """
        Check an external request against constitutional principles.

        Returns Violation if request should be blocked, None if allowed.
        """
        # Check for blocked patterns in task/command text
        task_text = str(arguments.get("task", "")) + str(arguments.get("command", ""))

        for pattern, description in self.BLOCKED_PATTERNS:
            if re.search(pattern, task_text, re.IGNORECASE):
                logger.warning(f"Blocked external request: {description}")
                return Violation(
                    principle="No destructive operations",
                    reason=f"Blocked pattern detected: {description}",
                    severity="block",
                )

        # Check file access restrictions
        context_files = arguments.get("context_files", [])
        for file_path in context_files:
            if any(forbidden in file_path for forbidden in self.FORBIDDEN_FILES):
                return Violation(
                    principle="No access to sensitive files",
                    reason=f"Access to sensitive file forbidden: {file_path}",
                    severity="block",
                )

            # Ensure within project root
            if not self._is_within_project(file_path):
                return Violation(
                    principle="Operations limited to project directory",
                    reason=f"Path outside project root: {file_path}",
                    severity="block",
                )

        return None

    def _is_within_project(self, path: str) -> bool:
        """Check if path is within project root."""
        from pathlib import Path
        try:
            resolved = Path(path).resolve()
            return str(resolved).startswith(str(self.project_root))
        except Exception:
            return False

    def redact_sensitive(self, content: str | None) -> str | None:
        """Redact sensitive information from content."""
        if not content:
            return content

        result = content
        for pattern, replacement in self.REDACT_PATTERNS:
            result = re.sub(pattern, replacement, result)

        return result
```

---

## Configuration

### Claude Code MCP Configuration

Add to `~/.claude/claude_desktop_config.json` (or equivalent):

```json
{
  "mcpServers": {
    "entropi": {
      "command": "entropi-server",
      "args": ["--project", "/path/to/project"],
      "env": {
        "ENTROPI_EXTERNAL_ACCESS": "true"
      }
    }
  }
}
```

### Entropi Configuration

```yaml
# .entropi/config.yaml

external_access:
  enabled: true

  # Authentication (future: certificate-based)
  require_auth: false

  # Resource limits
  max_concurrent_agents: 3
  max_execution_time: 300  # seconds
  max_output_size: 102400  # bytes

  # Allowed operations
  allowed_tools:
    - "entropi.spawn_agent"
    - "entropi.query_agent"
    - "entropi.list_agents"
    - "entropi.get_agent_result"
    - "entropi.cancel_agent"
    - "entropi.project_summary"

  # Constitutional settings for external access
  constitutional:
    enabled: true  # REQUIRED for external access
    strict_mode: true  # More aggressive filtering
    audit_log: true  # Log all external requests

  # Notification settings
  notifications:
    on_agent_spawn: true
    on_agent_complete: true
    on_violation: true
```

---

## CLI Commands

```bash
# Start Entropi as MCP server (for Claude Code to connect)
entropi server

# Start with specific project
entropi server --project /path/to/project

# Start with verbose logging
entropi server --log-level DEBUG

# Check server status
entropi server status

# View audit log
entropi server audit-log --tail 50
```

---

## Security Considerations

### Authentication (Future Enhancement)

```python
class AuthenticatedServer(EntropiExternalServer):
    """MCP Server with authentication."""

    async def authenticate(self, credentials: dict) -> bool:
        """Verify client credentials."""
        # Option 1: Shared secret
        if credentials.get("api_key") == self.config.external_access.api_key:
            return True

        # Option 2: Certificate verification
        # if self._verify_certificate(credentials.get("certificate")):
        #     return True

        return False
```

### Audit Logging

```python
class AuditLogger:
    """Logs all external access for security audit."""

    def log_request(
        self,
        tool: str,
        arguments: dict,
        client_id: str,
        result: str,
        violation: Violation | None,
    ):
        """Log an external request."""
        entry = {
            "timestamp": datetime.now().isoformat(),
            "client_id": client_id,
            "tool": tool,
            "arguments": self._redact(arguments),
            "result_size": len(result),
            "violation": violation.reason if violation else None,
        }

        # Append to audit log
        audit_path = self.config.config_dir / "audit.jsonl"
        with open(audit_path, "a") as f:
            f.write(json.dumps(entry) + "\n")
```

### Sandboxing (Future Enhancement)

```python
class SandboxedExecution:
    """Execute agent tasks in sandboxed environment."""

    async def run_sandboxed(self, task: str) -> str:
        """Run task in container or restricted environment."""
        # Option 1: Docker container
        # Option 2: firejail sandbox
        # Option 3: seccomp restrictions
        pass
```

---

## Implementation Phases

### Phase 0: Constitutional AI Foundation (Prerequisite)

Before exposing external access, implement:
1. Tool-level policies (destructive command detection)
2. Sensitive file protection
3. Output redaction
4. Audit logging infrastructure

**Estimated effort:** 1-2 days

### Phase 1: Basic MCP Server

1. Create `entropi-server` command
2. Implement basic tool handlers (spawn_agent, get_result)
3. Integrate constitutional guard
4. Add stdio transport

**Estimated effort:** 2-3 days

### Phase 2: Agent Management

1. Concurrent agent pool
2. Task queue with priorities
3. Resource limits enforcement
4. Proper cancellation handling

**Estimated effort:** 2-3 days

### Phase 3: Claude Code Integration

1. Documentation for Claude Code configuration
2. Usage examples and prompts
3. Error handling and recovery
4. Performance optimization

**Estimated effort:** 1-2 days

### Phase 4: Security Hardening

1. Authentication mechanism
2. Rate limiting
3. Enhanced audit logging
4. Optional sandboxing

**Estimated effort:** 3-5 days

---

## Usage Examples

### From Claude Code

```
User: "Use Entropi to analyze the codebase for security issues"

Claude Code:
1. Calls entropi.spawn_agent with task: "Analyze all Python files for security vulnerabilities including SQL injection, XSS, and authentication issues. Report findings with file paths and line numbers."
2. Receives agent_id
3. Periodically calls entropi.get_agent_result
4. Presents summarized findings to user
```

### Parallel Execution

```
User: "Run the full CI pipeline locally"

Claude Code:
1. Calls entropi.spawn_agent for "pytest" task
2. Calls entropi.spawn_agent for "ruff check" task
3. Calls entropi.spawn_agent for "mypy" task
4. Aggregates results from all three
5. Presents unified report
```

### Hybrid Workflow

```
User: "Help me implement this feature"

Claude Code:
1. Designs the feature architecture
2. Calls entropi.spawn_agent: "Implement the database models according to this spec: ..."
3. Reviews Entropi's implementation
4. Calls entropi.spawn_agent: "Write tests for the models"
5. Integrates and refines
```

---

## Open Questions

1. **Transport Protocol**: stdio vs HTTP/SSE for MCP communication?
2. **Session Persistence**: Should agent results persist across sessions?
3. **Multi-Project**: Support multiple projects simultaneously?
4. **Model Selection**: Let Claude Code specify which local model to use?
5. **Cost Tracking**: Track local compute usage for comparison with API costs?

---

## Summary

This proposal enables Claude Code to orchestrate local AI agents through Entropi, creating a powerful hybrid architecture. Constitutional AI serves as the critical security layer, ensuring that external access cannot compromise the user's system or data.

**Key principles:**
1. Constitutional AI is a **prerequisite**, not optional
2. All external requests pass through multiple security layers
3. Sensitive data never leaves the local machine
4. Full audit trail of all operations
5. User maintains ultimate control via configuration

**Recommended implementation order:**
1. Constitutional AI foundation (policies, redaction, audit)
2. Basic MCP server with spawn_agent
3. Agent management and concurrency
4. Claude Code integration and documentation
5. Security hardening and authentication
