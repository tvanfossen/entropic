"""Tests for tool definition loading from JSON files."""

import pytest
from entropic.mcp.servers.base import load_tool_definition
from mcp.types import Tool

# All JSON tool definition files that should exist
ALL_TOOLS = [
    ("read_file", "filesystem"),
    ("edit_file", "filesystem"),
    ("write_file", "filesystem"),
    ("execute", "bash"),
    ("status", "git"),
    ("diff", "git"),
    ("log", "git"),
    ("commit", "git"),
    ("branch", "git"),
    ("checkout", "git"),
    ("add", "git"),
    ("reset", "git"),
    ("handoff", "entropic"),
    ("prune_context", "entropic"),
    ("diagnostics", "diagnostics"),
    ("check_errors", "diagnostics"),
    ("todo_write", "entropic"),
]


class TestLoadToolDefinition:
    """Tests for load_tool_definition."""

    def test_returns_mcp_tool(self) -> None:
        """JSON loads as valid MCP Tool object."""
        tool = load_tool_definition("read_file", "filesystem")
        assert isinstance(tool, Tool)
        assert tool.name == "read_file"
        assert tool.description
        assert tool.inputSchema

    def test_nonexistent_tool_raises(self) -> None:
        """Missing JSON file raises FileNotFoundError."""
        with pytest.raises(FileNotFoundError):
            load_tool_definition("nonexistent_tool", "fake_server")

    @pytest.mark.parametrize("tool_name,server_prefix", ALL_TOOLS)
    def test_all_json_files_valid(self, tool_name: str, server_prefix: str) -> None:
        """Every JSON tool definition file loads and validates successfully."""
        tool = load_tool_definition(tool_name, server_prefix)
        assert isinstance(tool, Tool)
        assert tool.name == tool_name
        assert len(tool.description) > 0
        assert tool.inputSchema["type"] == "object"

    def test_todo_write_has_nested_schema(self) -> None:
        """todo_write JSON includes nested items schema with status enum."""
        tool = load_tool_definition("todo_write", "entropic")
        items = tool.inputSchema["properties"]["todos"]["items"]
        assert "status" in items["properties"]
        assert "enum" in items["properties"]["status"]
        assert "in_progress" in items["properties"]["status"]["enum"]
