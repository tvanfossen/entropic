"""Tests for InProcessProvider — in-process tool execution wrapper."""

from __future__ import annotations

from unittest.mock import AsyncMock, MagicMock

import pytest
from entropi.core.directives import ContextAnchor, NotifyPresenter
from entropi.mcp.provider import InProcessProvider
from entropi.mcp.servers.base import ServerResponse


def _make_mock_server(tools: list[dict] | None = None) -> MagicMock:
    """Create a mock BaseMCPServer with configurable tools."""
    server = MagicMock()

    if tools is None:
        tool_a = MagicMock()
        tool_a.name = "read_file"
        tool_a.description = "Read a file"
        tool_a.inputSchema = {"type": "object", "properties": {"path": {"type": "string"}}}

        tool_b = MagicMock()
        tool_b.name = "write_file"
        tool_b.description = "Write a file"
        tool_b.inputSchema = {"type": "object"}

        server.get_tools.return_value = [tool_a, tool_b]
    else:
        mock_tools = []
        for t in tools:
            mt = MagicMock()
            mt.name = t["name"]
            mt.description = t.get("description", "")
            mt.inputSchema = t.get("inputSchema", {})
            mock_tools.append(mt)
        server.get_tools.return_value = mock_tools

    server.execute_tool = AsyncMock(return_value='{"result": "ok"}')
    return server


class TestInProcessProviderLifecycle:
    """connect/disconnect state management."""

    def test_not_connected_initially(self) -> None:
        """Provider starts disconnected."""
        server = _make_mock_server()
        provider = InProcessProvider("test", server)
        assert provider.is_connected is False
        assert provider.name == "test"

    @pytest.mark.asyncio
    async def test_connect_sets_connected(self) -> None:
        """connect() loads tools and marks connected."""
        server = _make_mock_server()
        provider = InProcessProvider("test", server)
        await provider.connect()
        assert provider.is_connected is True

    @pytest.mark.asyncio
    async def test_disconnect_clears_state(self) -> None:
        """disconnect() clears tools and marks disconnected."""
        server = _make_mock_server()
        provider = InProcessProvider("test", server)
        await provider.connect()
        await provider.disconnect()
        assert provider.is_connected is False
        assert await provider.list_tools() == []


class TestInProcessProviderTools:
    """Tool listing with name prefixing."""

    @pytest.mark.asyncio
    async def test_connect_prefixes_tool_names(self) -> None:
        """connect() prefixes tool names with server name."""
        server = _make_mock_server()
        provider = InProcessProvider("filesystem", server)
        await provider.connect()

        tools = await provider.list_tools()
        assert len(tools) == 2
        assert tools[0]["name"] == "filesystem.read_file"
        assert tools[1]["name"] == "filesystem.write_file"

    @pytest.mark.asyncio
    async def test_tool_metadata_preserved(self) -> None:
        """connect() preserves description and inputSchema."""
        server = _make_mock_server()
        provider = InProcessProvider("fs", server)
        await provider.connect()

        tools = await provider.list_tools()
        assert tools[0]["description"] == "Read a file"
        assert "properties" in tools[0]["inputSchema"]


class TestInProcessProviderExecute:
    """Tool execution via server delegation."""

    @pytest.mark.asyncio
    async def test_execute_strips_prefix(self) -> None:
        """execute() strips server prefix before calling server."""
        server = _make_mock_server()
        provider = InProcessProvider("filesystem", server)
        await provider.connect()

        await provider.execute("filesystem.read_file", {"path": "/foo"})
        server.execute_tool.assert_called_once_with("read_file", {"path": "/foo"})

    @pytest.mark.asyncio
    async def test_execute_returns_tool_result(self) -> None:
        """execute() wraps result in ToolResult."""
        server = _make_mock_server()
        provider = InProcessProvider("filesystem", server)
        await provider.connect()

        result = await provider.execute("filesystem.read_file", {"path": "/foo"})
        assert result.result == '{"result": "ok"}'
        assert result.is_error is False
        assert result.name == "filesystem.read_file"
        assert result.duration_ms >= 0

    @pytest.mark.asyncio
    async def test_execute_handles_exception(self) -> None:
        """execute() catches exceptions and returns error ToolResult."""
        server = _make_mock_server()
        server.execute_tool = AsyncMock(side_effect=RuntimeError("disk full"))
        provider = InProcessProvider("filesystem", server)
        await provider.connect()

        result = await provider.execute("filesystem.write_file", {"path": "/foo"})
        assert result.is_error is True
        assert "disk full" in result.result

    @pytest.mark.asyncio
    async def test_execute_without_prefix(self) -> None:
        """execute() handles bare tool name (no prefix)."""
        server = _make_mock_server()
        provider = InProcessProvider("filesystem", server)
        await provider.connect()

        await provider.execute("read_file", {"path": "/foo"})
        server.execute_tool.assert_called_once_with("read_file", {"path": "/foo"})


class TestInProcessProviderDirectives:
    """Directive extraction from ServerResponse vs plain string."""

    @pytest.mark.asyncio
    async def test_server_response_extracts_native_directives(self) -> None:
        """ServerResponse directives are propagated natively to ToolResult."""
        server = _make_mock_server()
        directives = [
            ContextAnchor(key="test", content="state"),
            NotifyPresenter(key="update", data={"count": 1}),
        ]
        server.execute_tool = AsyncMock(
            return_value=ServerResponse(result="ok", directives=directives)
        )
        provider = InProcessProvider("entropi", server)
        await provider.connect()

        result = await provider.execute("entropi.todo_write", {"action": "add"})
        assert result.result == "ok"
        assert len(result.directives) == 2
        assert isinstance(result.directives[0], ContextAnchor)
        assert isinstance(result.directives[1], NotifyPresenter)

    @pytest.mark.asyncio
    async def test_plain_string_falls_back_to_extract(self) -> None:
        """Plain string return uses extract_directives fallback."""
        server = _make_mock_server()
        server.execute_tool = AsyncMock(return_value='{"result": "ok"}')
        provider = InProcessProvider("filesystem", server)
        await provider.connect()

        result = await provider.execute("filesystem.read_file", {"path": "/foo"})
        assert result.result == '{"result": "ok"}'
        # No directives embedded in plain JSON without _directives key
        assert result.directives == []
