"""
MCP client implementation using official SDK.

Provides a unified interface for communicating with MCP servers.
"""

import time
from typing import Any

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from entropi.core.base import ToolResult
from entropi.core.logging import get_logger

logger = get_logger("mcp.client")


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
        if self._session:
            await self._session.__aexit__(None, None, None)
            self._session = None

        if self._stdio_context:
            await self._stdio_context.__aexit__(None, None, None)
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

            result = await self._session.call_tool(local_name, arguments)

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
            )

        except Exception as e:
            logger.error(f"Tool execution failed: {e}")
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
