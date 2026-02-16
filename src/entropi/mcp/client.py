"""
MCP client implementation using official SDK.

Provides a unified interface for communicating with MCP servers.
"""

import asyncio
import time
from typing import Any

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from entropi.core.base import ToolResult
from entropi.core.directives import extract_directives
from entropi.core.logging import get_logger

logger = get_logger("mcp.client")

# Default timeout for MCP tool calls (seconds)
DEFAULT_TOOL_TIMEOUT = 30.0


class MCPClient:
    """Client for communicating with a single MCP server."""

    def __init__(
        self,
        name: str,
        command: str,
        args: list[str] | None = None,
        env: dict[str, str] | None = None,
    ) -> None:
        """
        Initialize MCP client.

        Args:
            name: Server name
            command: Command to start server
            args: Command arguments
            env: Environment variables
        """
        self.name = name
        self.command = command
        self.args = args or []
        self.env = env or {}

        self._session: ClientSession | None = None
        self._stdio_context: Any = None
        self._tools: list[dict[str, Any]] = []

    async def connect(self) -> None:
        """Connect to the MCP server."""
        logger.info(f"Connecting to MCP server: {self.name}")

        server_params = StdioServerParameters(
            command=self.command,
            args=self.args,
            env=self.env if self.env else None,
        )

        # Create stdio client context
        self._stdio_context = stdio_client(server_params)
        read_stream, write_stream = await self._stdio_context.__aenter__()

        # Create and initialize session
        self._session = ClientSession(read_stream, write_stream)
        await self._session.__aenter__()
        await self._session.initialize()

        # Cache available tools
        tools_response = await self._session.list_tools()
        self._tools = [
            {
                "name": f"{self.name}.{tool.name}",
                "description": tool.description or "",
                "inputSchema": tool.inputSchema if hasattr(tool, "inputSchema") else {},
            }
            for tool in tools_response.tools
        ]

        logger.info(f"Connected to {self.name}, {len(self._tools)} tools available")

    async def disconnect(self) -> None:
        """Disconnect from the MCP server."""
        # Close session first
        if self._session:
            try:
                await self._session.__aexit__(None, None, None)
            except Exception as e:
                logger.debug(f"Session cleanup error (non-critical): {e}")
            self._session = None

        # Close stdio context - may fail if called from different task
        if self._stdio_context:
            try:
                await self._stdio_context.__aexit__(None, None, None)
            except Exception as e:
                # This is expected when closing from a different task than opened
                logger.debug(f"Stdio cleanup error (non-critical): {e}")
            self._stdio_context = None

        self._tools = []
        logger.info(f"Disconnected from {self.name}")

    async def list_tools(self) -> list[dict[str, Any]]:
        """List available tools."""
        return self._tools

    async def execute(
        self,
        tool_name: str,
        arguments: dict[str, Any],
    ) -> ToolResult:
        """
        Execute a tool.

        Args:
            tool_name: Full tool name (server.tool)
            arguments: Tool arguments

        Returns:
            Tool execution result
        """
        if not self._session:
            raise RuntimeError(f"Not connected to {self.name}")

        # Extract local tool name (remove server prefix)
        if tool_name.startswith(f"{self.name}."):
            local_name = tool_name[len(self.name) + 1 :]
        else:
            local_name = tool_name

        logger.debug(f"Executing {self.name}.{local_name}")

        try:
            start = time.time()

            try:
                result = await asyncio.wait_for(
                    self._session.call_tool(local_name, arguments),
                    timeout=DEFAULT_TOOL_TIMEOUT,
                )
            except TimeoutError:
                logger.error(
                    f"Tool {self.name}.{local_name} timed out after {DEFAULT_TOOL_TIMEOUT}s"
                )
                return ToolResult(
                    call_id=tool_name,
                    name=tool_name,
                    result=f"Tool execution timed out after {DEFAULT_TOOL_TIMEOUT} seconds",
                    is_error=True,
                )

            duration = int((time.time() - start) * 1000)

            # Extract text content from result
            content = ""
            if result.content:
                for item in result.content:
                    if hasattr(item, "text"):
                        content += item.text

            is_error = getattr(result, "isError", False)

            return ToolResult(
                call_id=tool_name,
                name=tool_name,
                result=content,
                is_error=is_error,
                duration_ms=duration,
                directives=extract_directives(content),
            )

        except Exception as e:
            logger.exception(f"Tool execution failed: {e}")
            return ToolResult(
                call_id=tool_name,
                name=tool_name,
                result=str(e),
                is_error=True,
            )

    @property
    def is_connected(self) -> bool:
        """Check if connected to server."""
        return self._session is not None
