"""
Base class for MCP servers.

Provides common functionality for implementing MCP servers
using the official Python SDK.
"""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import TextContent, Tool

from entropi.core.tool_validation import ToolValidationError, validate_tool_definition

if TYPE_CHECKING:
    from entropi.core.directives import Directive
    from entropi.mcp.tools import BaseTool, ToolRegistry

logger = logging.getLogger(__name__)

# Tool definitions directory
_TOOLS_DIR = Path(__file__).parent.parent.parent / "data" / "tools"


@dataclass
class ServerResponse:
    """Structured result from server tool execution.

    Servers that emit directives return this instead of a plain string.
    ``result`` is the text shown to the model; ``directives`` are typed
    objects processed by the engine.
    """

    result: str
    directives: list[Directive] = field(default_factory=list)


def load_tool_definition(
    tool_name: str,
    server_prefix: str = "",
    tools_dir: Path | None = None,
) -> Tool:
    """Load and validate a tool definition from JSON.

    Args:
        tool_name: Tool name (e.g., "read_file", "execute")
        server_prefix: Server directory name (e.g., "filesystem", "git")
        tools_dir: Base directory for tool JSON files. Defaults to
            entropi's bundled ``data/tools/``. Consumer apps pass their
            own directory (e.g. ``Path("data/tools")``).

    Returns:
        Validated MCP Tool object.

    Raises:
        FileNotFoundError: If JSON file doesn't exist.
        ToolValidationError: If tool definition is invalid.
    """
    base = tools_dir or _TOOLS_DIR

    if server_prefix:
        json_path = base / server_prefix / f"{tool_name}.json"
    else:
        json_path = base / f"{tool_name}.json"

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


class BaseMCPServer:
    """Base class for MCP servers.

    Two usage patterns:

    **Tool registration (preferred for new servers):**
    Subclass, create ``BaseTool`` instances, call ``register_tool()``
    in ``__init__``.  ``get_tools()`` and ``execute_tool()`` work
    automatically via the internal ``ToolRegistry``.

    **Override (legacy / complex servers):**
    Override ``get_tools()`` and ``execute_tool()`` directly.
    The registry is bypassed when these methods are overridden.
    """

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

        from entropi.mcp.tools import ToolRegistry as _ToolRegistry

        self._tool_registry: ToolRegistry = _ToolRegistry()

        # Register handlers
        self._register_handlers()

    def register_tool(self, tool: BaseTool) -> None:
        """Register a BaseTool instance with this server.

        Registered tools are automatically included in ``get_tools()``
        and dispatched by ``execute_tool()``.

        Args:
            tool: Tool instance to register.
        """
        self._tool_registry.register(tool)

    def _register_handlers(self) -> None:
        """Register MCP handlers."""

        @self.server.list_tools()
        async def list_tools() -> list[Tool]:
            return self.get_tools()

        @self.server.call_tool()
        async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
            response = await self.execute_tool(name, arguments)
            text = response.result if isinstance(response, ServerResponse) else response
            return [TextContent(type="text", text=text)]

    def get_tools(self) -> list[Tool]:
        """Get list of available tools.

        Default: returns definitions from all registered tools.
        Override for custom behavior in legacy servers.
        """
        return self._tool_registry.get_tools()

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str | ServerResponse:
        """Execute a tool.

        Default: dispatches to the registered tool's ``execute()`` method.
        Override for custom behavior in legacy servers.

        Args:
            name: Tool name
            arguments: Tool arguments

        Returns:
            Plain string or ServerResponse with directives
        """
        return await self._tool_registry.dispatch(name, arguments)

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
