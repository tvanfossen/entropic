"""
External MCP server for Claude Code integration.

Exposes tools that allow Claude Code to delegate tasks to Entropi's
local models. Listens on a Unix socket for connections.
"""

from __future__ import annotations

import asyncio
import json
import time
from collections.abc import Callable
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp.server import Server
from mcp.types import TextContent, Tool

from entropi.core.logging import get_logger
from entropi.core.queue import MessagePriority, MessageQueue, MessageRequest, MessageSource
from entropi.core.session import SessionManager
from entropi.core.tasks import Task, TaskManager

if TYPE_CHECKING:
    from entropi.config.schema import EntropyConfig

logger = get_logger("mcp.external")


class RateLimiter:
    """Simple token bucket rate limiter."""

    def __init__(self, requests_per_minute: int) -> None:
        self._rate = requests_per_minute
        self._tokens = float(requests_per_minute)
        self._last_update = time.time()
        self._lock = asyncio.Lock()

    async def acquire(self) -> bool:
        """Acquire a token. Returns True if allowed, False if rate limited."""
        async with self._lock:
            now = time.time()
            elapsed = now - self._last_update
            self._last_update = now

            # Refill tokens
            self._tokens = min(float(self._rate), self._tokens + elapsed * (self._rate / 60.0))

            if self._tokens >= 1.0:
                self._tokens -= 1.0
                return True
            return False


class ExternalMCPServer:
    """
    MCP server for Claude Code integration.

    Provides tools for:
    - chat: Submit tasks to Entropi
    - poll_task: Check task status
    - cancel: Cancel tasks
    - get_history: Get conversation history
    - get_capabilities: Query available models/tools
    - report_issue: Report quality issues
    - status: Check Entropi status

    And notifications for:
    - task_progress: Progress updates
    - task_completed: Task completion
    - task_preempted: Task preemption
    """

    def __init__(
        self,
        config: EntropyConfig,
        message_queue: MessageQueue,
        task_manager: TaskManager,
        session_manager: SessionManager,
    ) -> None:
        """
        Initialize external MCP server.

        Args:
            config: Application configuration
            message_queue: Message queue for routing
            task_manager: Task manager for tracking
            session_manager: Session manager for history
        """
        self._config = config
        self._queue = message_queue
        self._tasks = task_manager
        self._sessions = session_manager

        self._server = Server("entropi-external")
        self._rate_limiter = RateLimiter(config.mcp.external.rate_limit)

        # Socket path - resolve relative paths from current working directory
        socket_path = config.mcp.external.socket_path
        if not socket_path.is_absolute():
            socket_path = Path.cwd() / socket_path
        self._socket_path = socket_path.resolve()

        # Connected clients for notifications
        self._clients: list[asyncio.StreamWriter] = []
        self._clients_lock = asyncio.Lock()

        # Callback to get capabilities from engine
        self._get_capabilities: Callable[[], dict[str, Any]] | None = None

        # History provider: returns live conversation messages
        self._history_provider: Callable[[], list[Any]] | None = None

        # Current session ID (legacy — prefer _history_provider)
        self._session_id: str | None = None

        # Register handlers
        self._register_handlers()

        # Set up task manager callbacks for notifications
        self._tasks.set_callbacks(
            on_progress=self._on_task_progress,
            on_completed=self._on_task_completed,
            on_preempted=self._on_task_preempted,
        )

    def set_session(self, session_id: str) -> None:
        """Set the current session ID."""
        self._session_id = session_id

    def set_history_provider(
        self,
        provider: Callable[[], list[Any]],
    ) -> None:
        """Set callback that returns live conversation messages.

        Args:
            provider: Callable returning list of Message objects
        """
        self._history_provider = provider

    def set_capabilities_callback(
        self,
        callback: Callable[[], dict[str, Any]],
    ) -> None:
        """Set callback to get capabilities."""
        self._get_capabilities = callback

    def _register_handlers(self) -> None:
        """Register MCP tool handlers."""

        @self._server.list_tools()
        async def list_tools() -> list[Tool]:
            return self._get_tools()

        @self._server.call_tool()
        async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
            result = await self._execute_tool(name, arguments)
            return [TextContent(type="text", text=json.dumps(result))]

    def _get_tools(self) -> list[Tool]:
        """Get list of available tools."""
        return [
            self._tool_chat(),
            self._tool_poll_task(),
            self._tool_cancel(),
            self._tool_get_history(),
            self._tool_get_capabilities(),
            self._tool_report_issue(),
            self._tool_status(),
        ]

    def _tool_chat(self) -> Tool:
        """Define chat tool."""
        return Tool(
            name="chat",
            description=(
                "Submit a task to Entropi, a local AI coding assistant. "
                "Use for tasks that don't require frontier-level reasoning: "
                "implementing functions, writing boilerplate, running tests, "
                "file operations. Runs locally (free). Returns immediately "
                "with task_id - use poll_task to get results. "
                "Conversation is shared with human user."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "message": {"type": "string", "description": "Your message to Entropi"},
                    "context": {
                        "type": "array",
                        "description": "Optional file contents to include as context",
                        "items": {
                            "type": "object",
                            "properties": {
                                "path": {"type": "string"},
                                "content": {"type": "string"},
                            },
                        },
                    },
                    "wait": {
                        "type": "boolean",
                        "description": "If true, block until completion (default: true)",
                        "default": True,
                    },
                },
                "required": ["message"],
            },
        )

    def _tool_poll_task(self) -> Tool:
        """Define poll_task tool."""
        return Tool(
            name="poll_task",
            description="Check the status of a previously submitted task.",
            inputSchema={
                "type": "object",
                "properties": {
                    "task_id": {"type": "string", "description": "Task ID returned from chat"}
                },
                "required": ["task_id"],
            },
        )

    def _tool_cancel(self) -> Tool:
        """Define cancel tool."""
        return Tool(
            name="cancel",
            description="Cancel a queued or in-progress task.",
            inputSchema={
                "type": "object",
                "properties": {
                    "task_id": {"type": "string", "description": "Task ID to cancel"},
                    "reason": {"type": "string", "description": "Optional reason for cancellation"},
                },
                "required": ["task_id"],
            },
        )

    def _tool_get_history(self) -> Tool:
        """Define get_history tool."""
        return Tool(
            name="get_history",
            description=(
                "Get recent conversation history from Entropi, "
                "including messages from both you and the human user."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "limit": {
                        "type": "integer",
                        "description": "Maximum messages to return",
                        "default": 20,
                    },
                    "include_tool_results": {
                        "type": "boolean",
                        "description": "Whether to include tool results",
                        "default": True,
                    },
                },
            },
        )

    def _tool_get_capabilities(self) -> Tool:
        """Define get_capabilities tool."""
        return Tool(
            name="get_capabilities",
            description=(
                "Query what models and tools Entropi has available. "
                "Use to understand what tasks can be delegated effectively."
            ),
            inputSchema={"type": "object", "properties": {}},
        )

    def _tool_report_issue(self) -> Tool:
        """Define report_issue tool."""
        return Tool(
            name="report_issue",
            description="Report a quality issue with Entropi's response.",
            inputSchema={
                "type": "object",
                "properties": {
                    "task_id": {
                        "type": "string",
                        "description": "Task ID of the problematic response",
                    },
                    "issue_type": {
                        "type": "string",
                        "enum": [
                            "incorrect",
                            "incomplete",
                            "hallucination",
                            "off_topic",
                            "slow",
                            "other",
                        ],
                    },
                    "description": {"type": "string", "description": "Description of the issue"},
                    "expected": {"type": "string", "description": "What you expected instead"},
                },
                "required": ["issue_type", "description"],
            },
        )

    def _tool_status(self) -> Tool:
        """Define status tool."""
        return Tool(
            name="status",
            description="Check Entropi's current status including queue depth and active tasks.",
            inputSchema={"type": "object", "properties": {}},
        )

    async def _execute_tool(
        self,
        name: str,
        arguments: dict[str, Any],
    ) -> dict[str, Any]:
        """Execute a tool and return result."""
        # Rate limiting
        if not await self._rate_limiter.acquire():
            return {
                "error": "rate_limited",
                "message": f"Rate limited. Max {self._config.mcp.external.rate_limit} requests/minute.",
            }

        handlers = {
            "chat": self._handle_chat,
            "poll_task": self._handle_poll_task,
            "cancel": self._handle_cancel,
            "get_history": self._handle_get_history,
            "get_capabilities": self._handle_get_capabilities,
            "report_issue": self._handle_report_issue,
            "status": self._handle_status,
        }

        handler = handlers.get(name)
        return await self._run_handler(name, handler, arguments)

    async def _run_handler(
        self,
        name: str,
        handler: Any,
        arguments: dict[str, Any],
    ) -> dict[str, Any]:
        """Run a handler with error handling."""
        if not handler:
            return {"error": "unknown_tool", "message": f"Unknown tool: {name}"}
        try:
            return await handler(arguments)
        except Exception as e:
            logger.error(f"Error executing {name}: {e}")
            return {"error": "execution_error", "message": str(e)}

    async def _handle_chat(self, args: dict[str, Any]) -> dict[str, Any]:
        """Handle chat tool."""
        message = args.get("message", "")
        if not message:
            return {"error": "invalid_args", "message": "message is required"}

        context = args.get("context")
        wait = args.get("wait", True)

        # Create task
        task = self._tasks.create_task(
            message=message,
            source=MessageSource.CLAUDE_CODE,
            context=context,
        )

        # Add to queue with callback for completion
        completion_event = asyncio.Event()
        task_result: dict[str, Any] = {}

        def on_complete(result: Any) -> None:
            nonlocal task_result
            task_result = result if isinstance(result, dict) else {"response": str(result)}
            completion_event.set()

        await self._queue.put(
            MessageRequest(
                content=message,
                source=MessageSource.CLAUDE_CODE,
                priority=MessagePriority.CLAUDE_CODE,
                context=context,
                callback=on_complete if wait else None,
                task_id=task.id,
            )
        )

        # Update queue position
        task.queue_position = self._queue.depth

        if wait:
            # Wait for completion
            await completion_event.wait()
            return self._tasks.get_task(task.id).to_poll_response()  # type: ignore

        return task.to_queued_response()

    async def _handle_poll_task(self, args: dict[str, Any]) -> dict[str, Any]:
        """Handle poll_task tool."""
        task_id = args.get("task_id", "")
        if not task_id:
            return {"error": "invalid_args", "message": "task_id is required"}

        task = self._tasks.get_task(task_id)
        if not task:
            return {"error": "not_found", "message": f"Task {task_id} not found"}

        return task.to_poll_response()

    async def _handle_cancel(self, args: dict[str, Any]) -> dict[str, Any]:
        """Handle cancel tool."""
        task_id = args.get("task_id", "")
        if not task_id:
            return {"error": "invalid_args", "message": "task_id is required"}

        reason = args.get("reason", "Cancelled by Claude Code")

        if self._tasks.cancel_task(task_id, reason):
            return {"status": "cancelled", "task_id": task_id}
        return {"error": "cannot_cancel", "message": f"Cannot cancel task {task_id}"}

    async def _handle_get_history(
        self,
        args: dict[str, Any],
    ) -> dict[str, Any]:
        """Handle get_history tool."""
        limit = args.get("limit", 20)
        include_tools = args.get("include_tool_results", True)

        # Prefer live history provider over session DB
        if self._history_provider is not None:
            return self._format_live_history(limit, include_tools)

        # Fallback to session manager
        if not self._session_id:
            return {
                "error": "no_session",
                "message": "No active session",
            }

        messages = self._sessions.get_history_for_mcp(
            self._session_id,
            limit=limit,
            include_tool_results=include_tools,
        )
        return {"messages": messages, "count": len(messages)}

    def _format_live_history(
        self,
        limit: int,
        include_tools: bool,
    ) -> dict[str, Any]:
        """Format live conversation messages for MCP response."""
        assert self._history_provider is not None
        all_msgs = self._history_provider()

        formatted = []
        for msg in all_msgs:
            if not include_tools and msg.role == "tool":
                continue
            formatted.append(
                {
                    "role": msg.role,
                    "content": msg.content[:4000],
                    "source": msg.metadata.get("source", "human"),
                    "has_tool_calls": len(msg.tool_calls) > 0,
                }
            )

        # Apply limit (most recent messages)
        trimmed = formatted[-limit:]
        return {"messages": trimmed, "count": len(trimmed)}

    async def _handle_get_capabilities(self, args: dict[str, Any]) -> dict[str, Any]:
        """Handle get_capabilities tool."""
        if self._get_capabilities:
            return self._get_capabilities()

        # Default capabilities
        return {
            "models": [{"name": "default", "context_length": 16384, "strengths": ["general"]}],
            "tools": [
                "filesystem.read_file",
                "filesystem.write_file",
                "bash.execute",
                "glob",
                "grep",
            ],
            "max_concurrent_tasks": 1,
            "rate_limit": {"requests_per_minute": self._config.mcp.external.rate_limit},
        }

    async def _handle_report_issue(self, args: dict[str, Any]) -> dict[str, Any]:
        """Handle report_issue tool."""
        issue_type = args.get("issue_type", "")
        description = args.get("description", "")

        if not issue_type or not description:
            return {
                "error": "invalid_args",
                "message": "issue_type and description are required",
            }

        # Log the issue for analysis
        task_id = args.get("task_id", "unknown")
        expected = args.get("expected", "")

        logger.warning(
            f"Quality issue reported: type={issue_type}, task={task_id}, "
            f"description={description[:100]}, expected={expected[:100]}"
        )

        # Could store in database for analysis
        return {"status": "reported", "message": "Thank you for the feedback"}

    async def _handle_status(self, args: dict[str, Any]) -> dict[str, Any]:
        """Handle status tool."""
        active_tasks = self._tasks.get_active_tasks()
        current = active_tasks[0] if active_tasks else None

        return {
            "state": "busy" if current else "idle",
            "active_task": (
                {
                    "task_id": current.id,
                    "elapsed_seconds": int(current.elapsed_seconds),
                }
                if current
                else None
            ),
            "queue_depth": self._queue.depth,
            "model_loaded": True,  # TODO: Get from orchestrator
        }

    # Notification callbacks

    def _on_task_progress(self, task: Task, message: str, percent: int) -> None:
        """Called when task progress updates."""
        asyncio.create_task(
            self._send_notification(
                "notifications/task_progress",
                {
                    "task_id": task.id,
                    "progress": message,
                    "percent": percent,
                },
            )
        )

    def _on_task_completed(self, task: Task) -> None:
        """Called when task completes."""
        asyncio.create_task(
            self._send_notification(
                "notifications/task_completed",
                task.to_completed_notification(),
            )
        )

    def _on_task_preempted(self, task: Task, reason: str) -> None:
        """Called when task is preempted."""
        asyncio.create_task(
            self._send_notification(
                "notifications/task_preempted",
                task.to_preempted_notification(reason),
            )
        )

    async def _send_notification(
        self,
        method: str,
        params: dict[str, Any],
    ) -> None:
        """Send notification to all connected clients."""
        notification = json.dumps(
            {
                "jsonrpc": "2.0",
                "method": method,
                "params": params,
            }
        )

        async with self._clients_lock:
            for writer in self._clients[:]:  # Copy to allow removal
                try:
                    writer.write(notification.encode() + b"\n")
                    await writer.drain()
                except Exception as e:
                    logger.warning(f"Failed to send notification: {e}")
                    self._clients.remove(writer)

    # Socket server

    async def start(self) -> None:
        """Start the socket server."""
        # Ensure socket directory exists
        self._socket_path.parent.mkdir(parents=True, exist_ok=True)

        # Remove existing socket
        if self._socket_path.exists():
            self._socket_path.unlink()

        server = await asyncio.start_unix_server(
            self._handle_client,
            path=str(self._socket_path),
        )

        # Set socket permissions (owner only)
        self._socket_path.chmod(0o600)

        logger.info(f"External MCP server listening on {self._socket_path}")

        async with server:
            await server.serve_forever()

    async def _handle_client(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        """Handle a client connection."""
        logger.info("Client connected to external MCP server")

        async with self._clients_lock:
            self._clients.append(writer)

        try:
            logger.info("Waiting for client messages...")
            while True:
                line = await reader.readline()
                if not line:
                    logger.info("Client sent empty line (EOF)")
                    break

                logger.info(f"Received from client: {len(line)} bytes")
                try:
                    request = json.loads(line.decode())
                    logger.info(
                        f"JSON-RPC request: method={request.get('method')}, id={request.get('id')}"
                    )
                    response = await self._handle_jsonrpc(request)
                    if response:
                        response_bytes = json.dumps(response).encode() + b"\n"
                        logger.info(f"Sending response: {len(response_bytes)} bytes")
                        writer.write(response_bytes)
                        await writer.drain()
                except json.JSONDecodeError as e:
                    logger.warning(f"Invalid JSON from client: {e}, data: {line[:100].decode()}")
                except Exception as e:
                    logger.error(f"Error handling request: {e}", exc_info=True)

        finally:
            async with self._clients_lock:
                if writer in self._clients:
                    self._clients.remove(writer)
            writer.close()
            await writer.wait_closed()
            logger.info("Client disconnected from external MCP server")

    async def _handle_jsonrpc(
        self,
        request: dict[str, Any],
    ) -> dict[str, Any] | None:
        """Handle a JSON-RPC request."""
        method = request.get("method", "")
        params = request.get("params", {})
        request_id = request.get("id")

        # Method handlers
        handlers: dict[str, Any] = {
            "tools/list": lambda: self._handle_tools_list(request_id),
            "tools/call": lambda: self._handle_tools_call(request_id, params),
            "initialize": lambda: self._handle_initialize(request_id),
        }

        if handler := handlers.get(method):
            result = handler()
            return await result if asyncio.iscoroutine(result) else result

        # Unknown method
        if request_id:
            return self._jsonrpc_error(request_id, -32601, f"Method not found: {method}")
        return None

    def _handle_tools_list(self, request_id: Any) -> dict[str, Any]:
        """Handle tools/list method."""
        tools = self._get_tools()
        response = {
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {"tools": [t.model_dump(exclude_none=True) for t in tools]},
        }
        response_json = json.dumps(response)
        logger.info(f"tools/list response ({len(response_json)} bytes): {response_json[:1000]}")
        return response

    async def _handle_tools_call(self, request_id: Any, params: dict[str, Any]) -> dict[str, Any]:
        """Handle tools/call method."""
        name = params.get("name", "")
        arguments = params.get("arguments", {})
        result = await self._execute_tool(name, arguments)
        return {
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {"content": [{"type": "text", "text": json.dumps(result)}]},
        }

    def _handle_initialize(self, request_id: Any) -> dict[str, Any]:
        """Handle initialize method."""
        tools_info = []
        if self._get_capabilities:
            caps = self._get_capabilities()
            tools_info = caps.get("tools", [])

        return {
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {
                "protocolVersion": "2024-11-05",
                "serverInfo": {"name": "entropi-external", "version": "1.0.0"},
                "capabilities": {"tools": {}},
                "instructions": (
                    "Entropi is a local AI coding assistant with its own tool access. "
                    f"Available tools: {', '.join(tools_info) if tools_info else 'filesystem, bash, git'}. "
                    "When delegating tasks: describe WHAT to do, not HOW. "
                    "Reference files by path - Entropi will read them itself. "
                    "Do not pass file contents. Let Entropi make its own tool decisions."
                ),
            },
        }

    def _jsonrpc_error(self, request_id: Any, code: int, message: str) -> dict[str, Any]:
        """Create a JSON-RPC error response."""
        return {"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}}

    async def stop(self) -> None:
        """Stop the socket server."""
        # Close all client connections
        async with self._clients_lock:
            for writer in self._clients:
                writer.close()
            self._clients.clear()

        # Remove socket file
        if self._socket_path.exists():
            self._socket_path.unlink()

        logger.info("External MCP server stopped")
