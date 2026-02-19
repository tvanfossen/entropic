"""
Base tool abstraction for MCP servers.

Provides BaseTool (individual tool with definition + execution) and
ToolRegistry (collection with dispatch) to eliminate boilerplate in
server implementations.
"""

from __future__ import annotations

import logging
from abc import ABC, abstractmethod
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp.types import Tool

from entropic.core.tool_validation import ToolValidationError, validate_tool_definition
from entropic.mcp.servers.base import ServerResponse, load_tool_definition

if TYPE_CHECKING:
    pass

logger = logging.getLogger(__name__)


class BaseTool(ABC):
    """Individual MCP tool — owns its definition and execution logic.

    Subclass this to create tools. Implement ``execute()`` with the
    tool's behavior.

    Two ways to provide the tool definition:

    **From JSON file (default):**
    Pass ``tool_name``, ``server_prefix``, ``tools_dir``.  The JSON
    file is loaded and validated on construction.

    **Inline definition:**
    Pass ``definition`` as a ``Tool`` object or a dict matching MCP
    tool format.  Dicts are validated the same as JSON files.

    Dependencies (board state, file tracker, etc.) are injected via
    the subclass constructor.
    """

    def __init__(
        self,
        tool_name: str = "",
        server_prefix: str = "",
        tools_dir: Path | None = None,
        definition: Tool | dict[str, Any] | None = None,
    ) -> None:
        if definition is not None:
            self._definition = self._resolve_definition(definition)
        elif tool_name:
            self._definition = load_tool_definition(tool_name, server_prefix, tools_dir)
        else:
            raise ValueError("BaseTool requires either tool_name or definition")

    @staticmethod
    def _resolve_definition(definition: Tool | dict[str, Any]) -> Tool:
        """Convert definition to a validated Tool object."""
        if isinstance(definition, Tool):
            return definition
        errors = validate_tool_definition(definition)
        if errors:
            name = definition.get("name", "<unknown>")
            raise ToolValidationError(str(name), errors)
        return Tool(
            name=definition["name"],
            description=definition.get("description", ""),
            inputSchema=definition["inputSchema"],
        )

    @property
    def name(self) -> str:
        """Tool name from the definition."""
        return self._definition.name

    @property
    def definition(self) -> Tool:
        """Full MCP Tool definition."""
        return self._definition

    @abstractmethod
    async def execute(self, arguments: dict[str, Any]) -> str | ServerResponse:
        """Execute this tool with the given arguments.

        Returns:
            Plain string or ServerResponse with directives.
        """


class ToolRegistry:
    """Manages a collection of BaseTool instances.

    Replaces the dict-dispatch boilerplate repeated across servers.
    Servers call ``register()`` during ``__init__`` and the registry
    handles ``get_tools()`` and ``dispatch()`` automatically.
    """

    def __init__(self) -> None:
        self._tools: dict[str, BaseTool] = {}

    def register(self, tool: BaseTool) -> None:
        """Register a tool instance.

        Args:
            tool: BaseTool subclass instance to register.
        """
        if tool.name in self._tools:
            logger.warning("Tool '%s' already registered — replacing", tool.name)
        self._tools[tool.name] = tool

    def has_tool(self, name: str) -> bool:
        """Check if a tool is registered by name."""
        return name in self._tools

    def get_tools(self) -> list[Tool]:
        """Return MCP Tool definitions for all registered tools."""
        return [t.definition for t in self._tools.values()]

    async def dispatch(self, name: str, arguments: dict[str, Any]) -> str | ServerResponse:
        """Route a tool call to the registered tool's execute method.

        Args:
            name: Tool name.
            arguments: Tool call arguments.

        Returns:
            Tool execution result, or error string if unknown.
        """
        tool = self._tools.get(name)
        if tool is None:
            return f"Unknown tool: {name}"
        return await tool.execute(arguments)
