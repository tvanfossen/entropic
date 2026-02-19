"""Tests for EntropiServer — unified todo/handoff/prune server."""

import json

import pytest
from entropi.core.directives import (
    ClearSelfTodos,
    ContextAnchor,
    InjectContext,
    NotifyPresenter,
    PruneMessages,
    StopProcessing,
    TierChange,
)
from entropi.mcp.servers.base import ServerResponse
from entropi.mcp.servers.entropi import EntropiServer


class TestEntropiServerTodoWrite:
    """todo_write returns context_anchor + notify_presenter directives."""

    @pytest.fixture
    def server(self) -> EntropiServer:
        return EntropiServer()

    @pytest.mark.asyncio
    async def test_add_returns_directives(self, server: EntropiServer) -> None:
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
    async def test_update_returns_directives(self, server: EntropiServer) -> None:
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
    async def test_empty_list_returns_usage_reminder(self, server: EntropiServer) -> None:
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


class TestEntropiServerHandoff:
    """handoff returns directives for engine-level side effects."""

    @pytest.fixture
    def server(self) -> EntropiServer:
        return EntropiServer()

    @pytest.mark.asyncio
    async def test_handoff_with_execution_todos(self, server: EntropiServer) -> None:
        """Handoff succeeds when execution todos exist."""
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
            "handoff",
            {
                "target_tier": "code",
                "reason": "Implementation ready",
                "task_state": "plan_ready",
            },
        )
        assert isinstance(result, ServerResponse)
        assert "Handoff requested" in result.result

        directive_types = [type(d) for d in result.directives]
        assert ClearSelfTodos in directive_types
        assert TierChange in directive_types
        assert StopProcessing in directive_types

        tier_d = next(d for d in result.directives if isinstance(d, TierChange))
        assert tier_d.tier == "code"

    @pytest.mark.asyncio
    async def test_handoff_empty_list_succeeds(self, server: EntropiServer) -> None:
        """Handoff with empty todo list succeeds (no gate)."""
        result = await server.execute_tool(
            "handoff",
            {
                "target_tier": "normal",
                "reason": "Quick handoff",
                "task_state": "in_progress",
            },
        )
        assert isinstance(result, ServerResponse)
        assert "Handoff requested" in result.result
        assert len(result.directives) > 0

    @pytest.mark.asyncio
    async def test_handoff_blocked_without_execution_todos(self, server: EntropiServer) -> None:
        """Handoff blocked when todos exist but none have target_tier."""
        await server.execute_tool(
            "todo_write",
            {
                "action": "add",
                "todos": [{"content": "Self task", "active_form": "Working", "status": "pending"}],
            },
        )
        result = await server.execute_tool(
            "handoff",
            {
                "target_tier": "code",
                "reason": "test",
                "task_state": "in_progress",
            },
        )
        # Error case returns plain string
        assert isinstance(result, str)
        data = json.loads(result)
        assert "error" in data
        assert "No execution todos" in data["error"]

    @pytest.mark.asyncio
    async def test_handoff_warns_incomplete_self_todos(self, server: EntropiServer) -> None:
        """Handoff warns about incomplete self-directed todos."""
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
            "handoff",
            {
                "target_tier": "code",
                "reason": "test",
                "task_state": "in_progress",
            },
        )
        assert isinstance(result, ServerResponse)
        inject = next((d for d in result.directives if isinstance(d, InjectContext)), None)
        assert inject is not None
        assert "1 self-directed" in inject.content

    @pytest.mark.asyncio
    async def test_handoff_no_warning_when_self_todos_complete(self, server: EntropiServer) -> None:
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
            "handoff",
            {
                "target_tier": "code",
                "reason": "test",
                "task_state": "in_progress",
            },
        )
        assert isinstance(result, ServerResponse)
        inject = [d for d in result.directives if isinstance(d, InjectContext)]
        assert len(inject) == 0


class TestEntropiServerPrune:
    """prune_context returns prune_messages directive."""

    @pytest.mark.asyncio
    async def test_prune_returns_directive(self) -> None:
        server = EntropiServer()
        result = await server.execute_tool("prune_context", {"keep_recent": 5})
        assert isinstance(result, ServerResponse)
        assert result.result == "Prune requested."
        assert len(result.directives) == 1

        prune = result.directives[0]
        assert isinstance(prune, PruneMessages)
        assert prune.keep_recent == 5

    @pytest.mark.asyncio
    async def test_prune_default_keep_recent(self) -> None:
        server = EntropiServer()
        result = await server.execute_tool("prune_context", {})
        assert isinstance(result, ServerResponse)
        prune = result.directives[0]
        assert isinstance(prune, PruneMessages)
        assert prune.keep_recent == 2


class TestEntropiServerDynamicTiers:
    """Custom tier_names patch handoff tool schema and validate at runtime."""

    @pytest.fixture
    def server(self) -> EntropiServer:
        return EntropiServer(tier_names=["suggest", "validate", "execute"])

    def test_handoff_enum_patched(self, server: EntropiServer) -> None:
        """Handoff tool's target_tier enum reflects custom tier names."""
        tools = server.get_tools()
        handoff = next(t for t in tools if t.name == "handoff")
        enum = handoff.inputSchema["properties"]["target_tier"]["enum"]
        assert enum == ["suggest", "validate", "execute"]

    def test_handoff_description_updated(self, server: EntropiServer) -> None:
        """Handoff description mentions custom tier names, not defaults."""
        tools = server.get_tools()
        handoff = next(t for t in tools if t.name == "handoff")
        assert "`suggest`" in handoff.description
        assert "`validate`" in handoff.description
        assert "`execute`" in handoff.description
        assert "simple" not in handoff.description

    def test_default_server_uses_static_tiers(self) -> None:
        """Server without tier_names uses the static handoff.json enum."""
        server = EntropiServer()
        tools = server.get_tools()
        handoff = next(t for t in tools if t.name == "handoff")
        enum = handoff.inputSchema["properties"]["target_tier"]["enum"]
        assert enum == ["simple", "normal", "code", "thinking"]

    @pytest.mark.asyncio
    async def test_handoff_rejects_invalid_tier(self, server: EntropiServer) -> None:
        """Handoff rejects a tier name not in the custom set."""
        result = await server.execute_tool(
            "handoff",
            {
                "target_tier": "thinking",
                "reason": "deep analysis",
                "task_state": "in_progress",
            },
        )
        assert isinstance(result, str)
        data = json.loads(result)
        assert "error" in data
        assert "Unknown tier" in data["error"]

    @pytest.mark.asyncio
    async def test_handoff_accepts_valid_custom_tier(self, server: EntropiServer) -> None:
        """Handoff succeeds with a valid custom tier name."""
        result = await server.execute_tool(
            "handoff",
            {
                "target_tier": "validate",
                "reason": "check the move",
                "task_state": "in_progress",
            },
        )
        assert isinstance(result, ServerResponse)
        assert "Handoff requested" in result.result
        tier_d = next(d for d in result.directives if isinstance(d, TierChange))
        assert tier_d.tier == "validate"


class TestSingleTierSkipsHandoff:
    """Single-tier server omits handoff tool (handoff to yourself is meaningless)."""

    def test_single_tier_no_handoff(self) -> None:
        """Server with one tier does not register handoff."""
        server = EntropiServer(tier_names=["normal"])
        tool_names = [t.name for t in server.get_tools()]
        assert "handoff" not in tool_names
        assert "todo_write" in tool_names
        assert "prune_context" in tool_names

    def test_multi_tier_has_handoff(self) -> None:
        """Server with multiple tiers registers handoff."""
        server = EntropiServer(tier_names=["normal", "thinking"])
        tool_names = [t.name for t in server.get_tools()]
        assert "handoff" in tool_names


class TestEntropiServerUnknownTool:
    """Unknown tool name returns error."""

    @pytest.mark.asyncio
    async def test_unknown_tool(self) -> None:
        server = EntropiServer()
        result = await server.execute_tool("nonexistent", {})
        assert isinstance(result, str)
        data = json.loads(result)
        assert "error" in data
        assert "Unknown tool" in data["error"]
