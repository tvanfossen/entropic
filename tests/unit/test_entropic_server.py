"""Tests for EntropicServer — unified todo/delegate/prune server."""

import json

import pytest
from entropic.core.directives import (
    ClearSelfTodos,
    ContextAnchor,
    Delegate,
    InjectContext,
    NotifyPresenter,
    PruneMessages,
    StopProcessing,
)
from entropic.mcp.servers.base import ServerResponse
from entropic.mcp.servers.entropic import EntropicServer


class TestEntropicServerTodoWrite:
    """todo_write returns context_anchor + notify_presenter directives."""

    @pytest.fixture
    def server(self) -> EntropicServer:
        return EntropicServer()

    @pytest.mark.asyncio
    async def test_add_returns_directives(self, server: EntropicServer) -> None:
        """Adding a todo returns context_anchor and notify_presenter directives."""
        result = await server.execute_tool(
            "todo_write",
            {
                "action": "add",
                "todos": [{"content": "Task A", "active_form": "Doing A", "status": "pending"}],
            },
        )
        assert isinstance(result, ServerResponse)
        assert "Added 1" in result.result
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
            "todo_write",
            {
                "action": "add",
                "todos": [{"content": "Task A", "active_form": "Doing A", "status": "pending"}],
            },
        )
        result = await server.execute_tool(
            "todo_write",
            {
                "action": "update",
                "index": 0,
                "status": "in_progress",
            },
        )
        assert isinstance(result, ServerResponse)
        assert "Updated" in result.result
        assert isinstance(result.directives[0], ContextAnchor)
        assert isinstance(result.directives[1], NotifyPresenter)

    @pytest.mark.asyncio
    async def test_empty_list_returns_usage_reminder(self, server: EntropicServer) -> None:
        """Empty todo list returns context_anchor with usage reminder."""
        result = await server.execute_tool(
            "todo_write",
            {
                "action": "replace",
                "todos": [],
            },
        )
        assert isinstance(result, ServerResponse)
        anchor = result.directives[0]
        assert isinstance(anchor, ContextAnchor)
        assert "No active todos" in anchor.content
        assert "todo_write" in anchor.content

        notify = result.directives[1]
        assert isinstance(notify, NotifyPresenter)
        assert notify.data["count"] == 0


class TestEntropicServerDelegate:
    """delegate returns directives for engine-level side effects."""

    @pytest.fixture
    def server(self) -> EntropicServer:
        return EntropicServer()

    @pytest.mark.asyncio
    async def test_delegate_with_execution_todos(self, server: EntropicServer) -> None:
        """Delegate succeeds when execution todos exist."""
        await server.execute_tool(
            "todo_write",
            {
                "action": "add",
                "todos": [
                    {
                        "content": "Fix bug",
                        "active_form": "Fixing",
                        "status": "pending",
                        "target_tier": "code",
                    }
                ],
            },
        )
        result = await server.execute_tool(
            "delegate",
            {
                "target": "code",
                "task": "Implementation ready",
            },
        )
        assert isinstance(result, ServerResponse)
        assert "Delegation requested" in result.result

        directive_types = [type(d) for d in result.directives]
        assert ClearSelfTodos in directive_types
        assert Delegate in directive_types
        assert StopProcessing in directive_types

        delegate_d = next(d for d in result.directives if isinstance(d, Delegate))
        assert delegate_d.target == "code"
        assert delegate_d.task == "Implementation ready"

    @pytest.mark.asyncio
    async def test_delegate_empty_list_rejected(self, server: EntropicServer) -> None:
        """Delegate with empty todo list is rejected — must plan first."""
        result = await server.execute_tool(
            "delegate",
            {
                "target": "normal",
                "task": "Quick delegation",
            },
        )
        assert isinstance(result, str)
        data = json.loads(result)
        assert "error" in data
        assert "todo_write" in data["error"]

    @pytest.mark.asyncio
    async def test_delegate_succeeds_without_execution_todos(self, server: EntropicServer) -> None:
        """Delegate succeeds with warning when todos exist but none have target_tier."""
        await server.execute_tool(
            "todo_write",
            {
                "action": "add",
                "todos": [{"content": "Self task", "active_form": "Working", "status": "pending"}],
            },
        )
        result = await server.execute_tool(
            "delegate",
            {
                "target": "code",
                "task": "test",
            },
        )
        # Now succeeds (warning logged, not rejected)
        assert isinstance(result, ServerResponse)
        assert any(isinstance(d, Delegate) and d.target == "code" for d in result.directives)

    @pytest.mark.asyncio
    async def test_delegate_warns_incomplete_self_todos(self, server: EntropicServer) -> None:
        """Delegate warns about incomplete self-directed todos."""
        await server.execute_tool(
            "todo_write",
            {
                "action": "add",
                "todos": [
                    {"content": "Self task", "active_form": "Self", "status": "pending"},
                    {
                        "content": "Code task",
                        "active_form": "Coding",
                        "status": "pending",
                        "target_tier": "code",
                    },
                ],
            },
        )
        result = await server.execute_tool(
            "delegate",
            {
                "target": "code",
                "task": "test",
            },
        )
        assert isinstance(result, ServerResponse)
        inject = next((d for d in result.directives if isinstance(d, InjectContext)), None)
        assert inject is not None
        assert "1 self-directed" in inject.content

    @pytest.mark.asyncio
    async def test_delegate_no_warning_when_self_todos_complete(
        self, server: EntropicServer
    ) -> None:
        """No warning when all self-directed todos are completed."""
        await server.execute_tool(
            "todo_write",
            {
                "action": "add",
                "todos": [
                    {"content": "Self done", "active_form": "Done", "status": "completed"},
                    {
                        "content": "Code task",
                        "active_form": "Coding",
                        "status": "pending",
                        "target_tier": "code",
                    },
                ],
            },
        )
        result = await server.execute_tool(
            "delegate",
            {
                "target": "code",
                "task": "test",
            },
        )
        assert isinstance(result, ServerResponse)
        inject = [d for d in result.directives if isinstance(d, InjectContext)]
        assert len(inject) == 0


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
        """Delegate description mentions custom tier names, not defaults."""
        tools = server.get_tools()
        delegate = next(t for t in tools if t.name == "delegate")
        assert "`suggest`" in delegate.description
        assert "`validate`" in delegate.description
        assert "`execute`" in delegate.description
        assert "simple" not in delegate.description

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
            {
                "target": "thinking",
                "task": "deep analysis",
            },
        )
        assert isinstance(result, str)
        data = json.loads(result)
        assert "error" in data
        assert "Unknown tier" in data["error"]

    @pytest.mark.asyncio
    async def test_delegate_accepts_valid_custom_tier(self, server: EntropicServer) -> None:
        """Delegate succeeds with a valid custom tier name (after planning)."""
        await server.execute_tool(
            "todo_write",
            {
                "action": "add",
                "todos": [
                    {
                        "content": "Validate move",
                        "active_form": "Validating",
                        "status": "pending",
                        "target_tier": "validate",
                    }
                ],
            },
        )
        result = await server.execute_tool(
            "delegate",
            {
                "target": "validate",
                "task": "check the move",
            },
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
        assert "todo_write" in tool_names
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
