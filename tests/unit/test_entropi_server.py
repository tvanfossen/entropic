"""Tests for EntropiServer — unified todo/handoff/prune server."""

import json

import pytest
from entropi.core.directives import (
    CLEAR_SELF_TODOS,
    INJECT_CONTEXT,
    PRUNE_MESSAGES,
    STOP_PROCESSING,
    TIER_CHANGE,
    TODO_STATE_CHANGED,
)
from entropi.mcp.servers.entropi import EntropiServer


class TestEntropiServerTodoWrite:
    """todo_write returns result + todo_state_changed directive."""

    @pytest.fixture
    def server(self) -> EntropiServer:
        return EntropiServer()

    @pytest.mark.asyncio
    async def test_add_returns_directive(self, server: EntropiServer) -> None:
        """Adding a todo returns todo_state_changed directive."""
        result = await server.execute_tool(
            "todo_write",
            {
                "action": "add",
                "todos": [{"content": "Task A", "active_form": "Doing A", "status": "pending"}],
            },
        )
        data = json.loads(result)
        assert "Added 1" in data["result"]
        directives = data["_directives"]
        assert len(directives) == 1
        assert directives[0]["type"] == TODO_STATE_CHANGED
        assert directives[0]["params"]["count"] == 1
        assert "[CURRENT TODO STATE]" in directives[0]["params"]["state"]

    @pytest.mark.asyncio
    async def test_update_returns_directive(self, server: EntropiServer) -> None:
        """Updating a todo returns todo_state_changed directive."""
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
        data = json.loads(result)
        assert "Updated" in data["result"]
        assert data["_directives"][0]["type"] == TODO_STATE_CHANGED

    @pytest.mark.asyncio
    async def test_empty_list_returns_empty_state(self, server: EntropiServer) -> None:
        """Empty todo list returns empty state string."""
        result = await server.execute_tool(
            "todo_write",
            {
                "action": "replace",
                "todos": [],
            },
        )
        data = json.loads(result)
        assert data["_directives"][0]["params"]["count"] == 0
        assert data["_directives"][0]["params"]["state"] == ""


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
        data = json.loads(result)
        assert "Handoff requested" in data["result"]

        directives = data["_directives"]
        types = [d["type"] for d in directives]
        assert CLEAR_SELF_TODOS in types
        assert TIER_CHANGE in types
        assert STOP_PROCESSING in types

        tier_d = next(d for d in directives if d["type"] == TIER_CHANGE)
        assert tier_d["params"]["tier"] == "code"

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
        data = json.loads(result)
        assert "Handoff requested" in data["result"]
        assert "_directives" in data

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
        data = json.loads(result)
        directives = data["_directives"]
        inject = next((d for d in directives if d["type"] == INJECT_CONTEXT), None)
        assert inject is not None
        assert "1 self-directed" in inject["params"]["content"]

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
        data = json.loads(result)
        directives = data["_directives"]
        inject = [d for d in directives if d["type"] == INJECT_CONTEXT]
        assert len(inject) == 0


class TestEntropiServerPrune:
    """prune_context returns prune_messages directive."""

    @pytest.mark.asyncio
    async def test_prune_returns_directive(self) -> None:
        server = EntropiServer()
        result = await server.execute_tool("prune_context", {"keep_recent": 5})
        data = json.loads(result)
        assert data["result"] == "Prune requested."
        directives = data["_directives"]
        assert len(directives) == 1
        assert directives[0]["type"] == PRUNE_MESSAGES
        assert directives[0]["params"]["keep_recent"] == 5

    @pytest.mark.asyncio
    async def test_prune_default_keep_recent(self) -> None:
        server = EntropiServer()
        result = await server.execute_tool("prune_context", {})
        data = json.loads(result)
        assert data["_directives"][0]["params"]["keep_recent"] == 2


class TestEntropiServerUnknownTool:
    """Unknown tool name returns error."""

    @pytest.mark.asyncio
    async def test_unknown_tool(self) -> None:
        server = EntropiServer()
        result = await server.execute_tool("nonexistent", {})
        data = json.loads(result)
        assert "error" in data
        assert "Unknown tool" in data["error"]
