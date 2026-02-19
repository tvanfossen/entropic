"""
In-process tool provider.

Wraps a BaseMCPServer instance to match MCPClient's duck-type
interface, allowing ServerManager to treat in-process and
subprocess-backed servers uniformly.
"""

from __future__ import annotations

import time
from typing import Any

from entropic.core.base import ToolResult
from entropic.core.directives import extract_directives
from entropic.core.logging import get_logger
from entropic.mcp.servers.base import BaseMCPServer, ServerResponse

logger = get_logger("mcp.provider")


class InProcessProvider:
    """Wraps a BaseMCPServer for direct in-process tool execution.

    Implements the same interface as MCPClient so ServerManager
    can store both in ``_clients`` without type discrimination.
    """

    def __init__(self, name: str, server: BaseMCPServer) -> None:
        """Initialize in-process provider.

        Args:
            name: Server name (used as tool name prefix)
            server: BaseMCPServer instance to wrap
        """
        self.name = name
        self._server = server
        self._tools: list[dict[str, Any]] = []
        self._connected = False

    @property
    def is_connected(self) -> bool:
        """Check if provider is connected (tools loaded)."""
        return self._connected

    async def connect(self) -> None:
        """Load tool definitions from the server."""
        raw_tools = self._server.get_tools()
        self._tools = [
            {
                "name": f"{self.name}.{tool.name}",
                "description": tool.description or "",
                "inputSchema": tool.inputSchema if hasattr(tool, "inputSchema") else {},
            }
            for tool in raw_tools
        ]
        self._connected = True
        logger.info(f"In-process provider {self.name}: {len(self._tools)} tools available")

    async def disconnect(self) -> None:
        """Mark provider as disconnected."""
        self._connected = False
        self._tools = []
        logger.info(f"In-process provider {self.name}: disconnected")

    async def list_tools(self) -> list[dict[str, Any]]:
        """List available tools (prefixed with server name)."""
        return self._tools

    async def execute(
        self,
        tool_name: str,
        arguments: dict[str, Any],
    ) -> ToolResult:
        """Execute a tool directly, no subprocess or JSON-RPC.

        Args:
            tool_name: Full tool name (server.tool)
            arguments: Tool arguments

        Returns:
            Tool execution result
        """
        # Strip server prefix (mirrors MCPClient.execute lines 128-131)
        if tool_name.startswith(f"{self.name}."):
            local_name = tool_name[len(self.name) + 1 :]
        else:
            local_name = tool_name

        start = time.time()
        try:
            response = await self._server.execute_tool(local_name, arguments)
            duration = int((time.time() - start) * 1000)

            # ServerResponse carries native directives; plain str uses fallback
            if isinstance(response, ServerResponse):
                result_str = response.result
                directives = response.directives
            else:
                result_str = response
                directives = extract_directives(result_str)

            return ToolResult(
                call_id=tool_name,
                name=tool_name,
                result=result_str,
                duration_ms=duration,
                directives=directives,
            )
        except Exception as e:
            duration = int((time.time() - start) * 1000)
            logger.error(f"In-process tool {self.name}.{local_name} failed: {e}")
            return ToolResult(
                call_id=tool_name,
                name=tool_name,
                result=str(e),
                is_error=True,
                duration_ms=duration,
            )
