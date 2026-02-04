"""
Base class for MCP servers.

Provides common functionality for implementing MCP servers
using the official Python SDK.
"""

from abc import ABC, abstractmethod
from pathlib import Path
from typing import Any

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import TextContent, Tool

# Tool descriptions directory
_TOOLS_DIR = Path(__file__).parent.parent.parent / "data" / "prompts" / "tools"


def load_tool_description(tool_name: str, server_prefix: str = "") -> str:
    """
    Load tool description from markdown file.

    Args:
        tool_name: Tool name (e.g., "read_file", "status")
        server_prefix: Optional server prefix (e.g., "git", "bash")

    Returns:
        Tool description text, or fallback if file not found
    """
    # Try with prefix first (e.g., git_status.md), then without (e.g., status.md)
    if server_prefix:
        prefixed_path = _TOOLS_DIR / f"{server_prefix}_{tool_name}.md"
        if prefixed_path.exists():
            return prefixed_path.read_text().strip()

    # Try direct name
    direct_path = _TOOLS_DIR / f"{tool_name}.md"
    if direct_path.exists():
        return direct_path.read_text().strip()

    # Fallback
    return f"Tool: {tool_name}"


class BaseMCPServer(ABC):
    """Base class for MCP servers."""

    def __init__(self, name: str, version: str = "1.0.0") -> None:
        """
        Initialize server.

        Args:
            name: Server name
            version: Server version
        """
        self.name = name
        self.version = version
        self.server = Server(name)

        # Register handlers
        self._register_handlers()

    def _register_handlers(self) -> None:
        """Register MCP handlers."""

        @self.server.list_tools()
        async def list_tools() -> list[Tool]:
            return self.get_tools()

        @self.server.call_tool()
        async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
            result = await self.execute_tool(name, arguments)
            return [TextContent(type="text", text=result)]

    @abstractmethod
    def get_tools(self) -> list[Tool]:
        """Get list of available tools."""
        pass

    @abstractmethod
    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """
        Execute a tool.

        Args:
            name: Tool name
            arguments: Tool arguments

        Returns:
            Result string
        """
        pass

    async def run(self) -> None:
        """Run the server."""
        async with stdio_server() as (read_stream, write_stream):
            await self.server.run(
                read_stream,
                write_stream,
                self.server.create_initialization_options(),
            )


def create_tool(
    name: str,
    description: str,
    properties: dict[str, dict[str, Any]],
    required: list[str] | None = None,
) -> Tool:
    """
    Helper to create a Tool definition.

    Args:
        name: Tool name
        description: Tool description
        properties: Parameter properties
        required: Required parameters

    Returns:
        Tool definition
    """
    return Tool(
        name=name,
        description=description,
        inputSchema={
            "type": "object",
            "properties": properties,
            "required": required or [],
        },
    )
