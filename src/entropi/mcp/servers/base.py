"""
Base class for MCP servers.

Provides common functionality for implementing MCP servers
using the official Python SDK.
"""

import json
import logging
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Any

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import TextContent, Tool

from entropi.core.tool_validation import ToolValidationError, validate_tool_definition

logger = logging.getLogger(__name__)

# Tool definitions directory
_TOOLS_DIR = Path(__file__).parent.parent.parent / "data" / "tools"


def load_tool_definition(tool_name: str, server_prefix: str = "") -> Tool:
    """Load and validate a tool definition from JSON.

    Args:
        tool_name: Tool name (e.g., "read_file", "execute")
        server_prefix: Server directory name (e.g., "filesystem", "git")

    Returns:
        Validated MCP Tool object.

    Raises:
        FileNotFoundError: If JSON file doesn't exist.
        ToolValidationError: If tool definition is invalid.
    """
    if server_prefix:
        json_path = _TOOLS_DIR / server_prefix / f"{tool_name}.json"
    else:
        json_path = _TOOLS_DIR / f"{tool_name}.json"

    if not json_path.exists():
        raise FileNotFoundError(f"Tool definition not found: {json_path}")

    tool_data = json.loads(json_path.read_text())

    errors = validate_tool_definition(tool_data)
    if errors:
        raise ToolValidationError(tool_name, errors)

    return Tool(
        name=tool_data["name"],
        description=tool_data.get("description", ""),
        inputSchema=tool_data["inputSchema"],
    )


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

    @staticmethod
    def get_permission_pattern(
        tool_name: str,
        arguments: dict[str, Any],
    ) -> str:
        """Generate permission pattern for 'Always Allow/Deny'.

        Default: tool-level (e.g., "filesystem.read_file").
        Override in subclasses for finer granularity.

        Args:
            tool_name: Fully-qualified tool name
            arguments: Tool call arguments

        Returns:
            Permission pattern string
        """
        return tool_name

    @staticmethod
    def skip_duplicate_check(tool_name: str) -> bool:
        """Check if a tool should skip duplicate detection.

        Override in subclasses for tools with side effects that must
        always execute (e.g., read_file updates FileAccessTracker).

        Args:
            tool_name: Local tool name (without server prefix)

        Returns:
            True if duplicate check should be skipped
        """
        return False

    async def run(self) -> None:
        """Run the server."""
        async with stdio_server() as (read_stream, write_stream):
            await self.server.run(
                read_stream,
                write_stream,
                self.server.create_initialization_options(),
            )
