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

from entropi.mcp.servers.base import ServerResponse, load_tool_definition

if TYPE_CHECKING:
    pass

logger = logging.getLogger(__name__)


class BaseTool(ABC):
    """Individual MCP tool — owns its definition and execution logic.

    Subclass this to create tools. The JSON definition is loaded on
    construction; implement ``execute()`` with the tool's behavior.

    Dependencies (board state, file tracker, etc.) are injected via
    the subclass constructor.

    Args:
        tool_name: Tool name matching the JSON filename (e.g., "read_file").
        server_prefix: Server directory under tools_dir (e.g., "filesystem").
        tools_dir: Base directory for tool JSON files.  Defaults to
            entropi's bundled ``data/tools/``.
    """

    def __init__(
        self,
        tool_name: str,
        server_prefix: str = "",
        tools_dir: Path | None = None,
    ) -> None:
        self._definition = load_tool_definition(tool_name, server_prefix, tools_dir)

    @property
    def name(self) -> str:
        """Tool name from the JSON definition."""
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
