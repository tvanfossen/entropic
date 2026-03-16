"""Tests for EntropicServer — todo, delegate, pipeline, complete, prune."""

import json

import pytest
from entropic.core.directives import (
    ContextAnchor,
    Delegate,
    NotifyPresenter,
    PruneMessages,
    StopProcessing,
)
from entropic.mcp.servers.base import ServerResponse
from entropic.mcp.servers.entropic import EntropicServer


class TestEntropicServerTodo:
    """entropic.todo returns context_anchor + notify_presenter directives."""

    @pytest.fixture
    def server(self) -> EntropicServer:
        return EntropicServer()

    @pytest.mark.asyncio
    async def test_add_returns_directives(self, server: EntropicServer) -> None:
        """Adding a todo returns context_anchor and notify_presenter directives."""
        result = await server.execute_tool(
            "todo",
            {"action": "add", "content": "Task A", "status": "pending"},
        )
        assert isinstance(result, ServerResponse)
        assert "Added" in result.result
        assert len(result.directives) == 2

        anchor = result.directives[0]
        assert isinstance(anchor, ContextAnchor)
        assert anchor.key == "todo_state"
        assert "[CURRENT TODO STATE]" in anchor.content

        notify = result.directives[1]
        assert isinstance(notify, NotifyPresenter)
        assert notify.key == "todo_update"
        assert notify.data["count"] == 1

    @pytest.mark.asyncio
    async def test_update_returns_directives(self, server: EntropicServer) -> None:
        """Updating a todo returns context_anchor and notify_presenter directives."""
        await server.execute_tool(
            "todo",
            {"action": "add", "content": "Task A", "status": "pending"},
        )
        result = await server.execute_tool(
            "todo",
            {"action": "update", "index": 0, "status": "in_progress"},
        )
        assert isinstance(result, ServerResponse)
        assert "Updated" in result.result
        assert isinstance(result.directives[0], ContextAnchor)
        assert isinstance(result.directives[1], NotifyPresenter)

    @pytest.mark.asyncio
    async def test_remove_returns_directives(self, server: EntropicServer) -> None:
        """Removing a todo returns directives."""
        await server.execute_tool(
            "todo",
            {"action": "add", "content": "Task A", "status": "pending"},
        )
        result = await server.execute_tool(
            "todo",
            {"action": "remove", "index": 0},
        )
        assert isinstance(result, ServerResponse)
        assert "Removed" in result.result

    @pytest.mark.asyncio
    async def test_add_without_content_returns_error(self, server: EntropicServer) -> None:
        """Add without content returns actionable error."""
        result = await server.execute_tool(
            "todo",
            {"action": "add"},
        )
        assert isinstance(result, ServerResponse)
        assert "Error" in result.result
        assert "content" in result.result

    @pytest.mark.asyncio
    async def test_unknown_action_returns_error(self, server: EntropicServer) -> None:
        """Unknown action returns error with valid actions listed."""
        result = await server.execute_tool(
            "todo",
            {"action": "bulk_update"},
        )
        assert isinstance(result, ServerResponse)
        assert "Error" in result.result
        assert "add" in result.result


class TestEntropicServerDelegate:
    """delegate returns directives for engine-level side effects."""

    @pytest.fixture
    def server(self) -> EntropicServer:
        return EntropicServer()

    @pytest.mark.asyncio
    async def test_delegate_succeeds(self, server: EntropicServer) -> None:
        """Delegate succeeds without requiring prior todos."""
        result = await server.execute_tool(
            "delegate",
            {"target": "code", "task": "Implement feature"},
        )
        assert isinstance(result, ServerResponse)
        assert "Delegation requested" in result.result

        directive_types = [type(d) for d in result.directives]
        assert Delegate in directive_types
        assert StopProcessing in directive_types

        delegate_d = next(d for d in result.directives if isinstance(d, Delegate))
        assert delegate_d.target == "code"
        assert delegate_d.task == "Implement feature"

    @pytest.mark.asyncio
    async def test_delegate_with_max_turns(self, server: EntropicServer) -> None:
        """Delegate passes max_turns to directive."""
        result = await server.execute_tool(
            "delegate",
            {"target": "code", "task": "Quick fix", "max_turns": 5},
        )
        assert isinstance(result, ServerResponse)
        delegate_d = next(d for d in result.directives if isinstance(d, Delegate))
        assert delegate_d.max_turns == 5


class TestEntropicServerPrune:
    """prune_context returns prune_messages directive."""

    @pytest.mark.asyncio
    async def test_prune_returns_directive(self) -> None:
        server = EntropicServer()
        result = await server.execute_tool("prune_context", {"keep_recent": 5})
        assert isinstance(result, ServerResponse)
        assert result.result == "Prune requested."
        assert len(result.directives) == 1

        prune = result.directives[0]
        assert isinstance(prune, PruneMessages)
        assert prune.keep_recent == 5

    @pytest.mark.asyncio
    async def test_prune_default_keep_recent(self) -> None:
        server = EntropicServer()
        result = await server.execute_tool("prune_context", {})
        assert isinstance(result, ServerResponse)
        prune = result.directives[0]
        assert isinstance(prune, PruneMessages)
        assert prune.keep_recent == 2


class TestEntropicServerDynamicTiers:
    """Custom tier_names patch delegate tool schema and validate at runtime."""

    @pytest.fixture
    def server(self) -> EntropicServer:
        return EntropicServer(tier_names=["suggest", "validate", "execute"])

    def test_delegate_enum_patched(self, server: EntropicServer) -> None:
        """Delegate tool's target enum reflects custom tier names."""
        tools = server.get_tools()
        delegate = next(t for t in tools if t.name == "delegate")
        enum = delegate.inputSchema["properties"]["target"]["enum"]
        assert enum == ["suggest", "validate", "execute"]

    def test_delegate_description_updated(self, server: EntropicServer) -> None:
        """Delegate description mentions custom tier names."""
        tools = server.get_tools()
        delegate = next(t for t in tools if t.name == "delegate")
        assert "`suggest`" in delegate.description
        assert "`validate`" in delegate.description
        assert "`execute`" in delegate.description

    def test_default_server_uses_empty_enum(self) -> None:
        """Server without tier_names uses delegate.json with empty enum."""
        server = EntropicServer()
        tools = server.get_tools()
        delegate = next(t for t in tools if t.name == "delegate")
        enum = delegate.inputSchema["properties"]["target"]["enum"]
        assert enum == []

    @pytest.mark.asyncio
    async def test_delegate_rejects_invalid_tier(self, server: EntropicServer) -> None:
        """Delegate rejects a tier name not in the custom set."""
        result = await server.execute_tool(
            "delegate",
            {"target": "thinking", "task": "deep analysis"},
        )
        assert isinstance(result, str)
        data = json.loads(result)
        assert "error" in data
        assert "Unknown tier" in data["error"]

    @pytest.mark.asyncio
    async def test_delegate_accepts_valid_custom_tier(self, server: EntropicServer) -> None:
        """Delegate succeeds with a valid custom tier name."""
        result = await server.execute_tool(
            "delegate",
            {"target": "validate", "task": "check the move"},
        )
        assert isinstance(result, ServerResponse)
        assert "Delegation requested" in result.result
        delegate_d = next(d for d in result.directives if isinstance(d, Delegate))
        assert delegate_d.target == "validate"


class TestSingleTierSkipsDelegate:
    """Single-tier server omits delegate tool (delegate to yourself is meaningless)."""

    def test_single_tier_no_delegate(self) -> None:
        """Server with one tier does not register delegate."""
        server = EntropicServer(tier_names=["normal"])
        tool_names = [t.name for t in server.get_tools()]
        assert "delegate" not in tool_names
        assert "todo" in tool_names
        assert "prune_context" in tool_names

    def test_multi_tier_has_delegate(self) -> None:
        """Server with multiple tiers registers delegate."""
        server = EntropicServer(tier_names=["normal", "thinking"])
        tool_names = [t.name for t in server.get_tools()]
        assert "delegate" in tool_names


class TestEntropicServerUnknownTool:
    """Unknown tool name returns error."""

    @pytest.mark.asyncio
    async def test_unknown_tool(self) -> None:
        server = EntropicServer()
        result = await server.execute_tool("nonexistent", {})
        assert isinstance(result, str)
        data = json.loads(result)
        assert "error" in data
        assert "Unknown tool" in data["error"]
