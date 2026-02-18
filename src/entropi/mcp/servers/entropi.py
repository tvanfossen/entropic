"""
Entropi MCP server.

Provides entropi-internal tools: todo management, tier handoff,
and context pruning. Returns ``ServerResponse`` with native typed
directives for engine-level side effects.
"""

from __future__ import annotations

import json
from typing import Any

from mcp.types import Tool

from entropi.core.directives import (
    ClearSelfTodos,
    ContextAnchor,
    InjectContext,
    NotifyPresenter,
    PruneMessages,
    StopProcessing,
    TierChange,
)
from entropi.core.todos import TodoList, TodoStatus
from entropi.mcp.servers.base import BaseMCPServer, ServerResponse, load_tool_definition
from entropi.mcp.tools import BaseTool


class TodoWriteTool(BaseTool):
    """Manage the internal todo list."""

    def __init__(self, todo_list: TodoList) -> None:
        super().__init__("todo_write", "entropi")
        self._todo_list = todo_list

    async def execute(self, arguments: dict[str, Any]) -> ServerResponse:
        """Update todo list, return context anchor + presenter notification."""
        result = self._todo_list.handle_tool_call(arguments)
        state = self._todo_list.format_for_context()
        return ServerResponse(
            result=result,
            directives=[
                ContextAnchor(key="todo_state", content=state),
                NotifyPresenter(
                    key="todo_update",
                    data={
                        "items": self._todo_list.to_dict(),
                        "count": len(self._todo_list.items),
                    },
                ),
            ],
        )


class HandoffTool(BaseTool):
    """Transfer the current task to a different model tier."""

    def __init__(self, todo_list: TodoList, tier_names: list[str] | None) -> None:
        definition = self._build_definition(tier_names) if tier_names else None
        if definition:
            super().__init__(definition=definition)
        else:
            super().__init__("handoff", "entropi")
        self._todo_list = todo_list
        self._tier_names = tier_names

    @staticmethod
    def _build_definition(tier_names: list[str]) -> Tool:
        """Build handoff tool definition with patched tier enum."""
        tool = load_tool_definition("handoff", "entropi")
        schema = dict(tool.inputSchema)
        props = dict(schema["properties"])
        target_prop = dict(props["target_tier"])
        target_prop["enum"] = list(tier_names)
        props["target_tier"] = target_prop
        schema["properties"] = props

        tier_list = ", ".join(f"`{t}`" for t in tier_names)
        description = (
            "Transfer the current task to a different model tier.\n\n"
            f"Available tiers: {tier_list}\n\n"
            "Use this tool when the task would be better handled "
            "by a different tier's capabilities."
        )
        return Tool(name=tool.name, description=description, inputSchema=schema)

    async def execute(self, arguments: dict[str, Any]) -> str | ServerResponse:
        """Validate and execute tier handoff."""
        target_tier = arguments.get("target_tier", "")
        reason = arguments.get("reason", "")

        error = self._validate_handoff(target_tier)
        if error:
            return error

        directives = self._build_directives(target_tier, reason)
        return ServerResponse(
            result=f"Handoff requested to {target_tier}. Reason: {reason}",
            directives=directives,
        )

    def _validate_handoff(self, target_tier: str) -> str | None:
        """Check tier validity and todo requirements. Returns error JSON or None."""
        if self._tier_names and target_tier not in self._tier_names:
            return json.dumps(
                {"error": f"Unknown tier: '{target_tier}'. Available tiers: {self._tier_names}"}
            )

        execution_todos = [t for t in self._todo_list.items if t.target_tier is not None]
        if not execution_todos and not self._todo_list.is_empty:
            return json.dumps(
                {
                    "error": (
                        "No execution todos (with target_tier) found. "
                        "Create todos with a target_tier before handing off."
                    ),
                }
            )
        return None

    def _build_directives(self, target_tier: str, reason: str) -> list:
        """Build handoff directives including optional warning."""
        directives = []

        incomplete_self = [
            t
            for t in self._todo_list.items
            if t.target_tier is None and t.status != TodoStatus.COMPLETED
        ]
        if incomplete_self:
            directives.append(
                InjectContext(
                    content=(
                        f"[SYSTEM] Warning: {len(incomplete_self)} self-directed "
                        f"todo(s) not yet completed. Proceeding with handoff."
                    ),
                )
            )

        directives.extend(
            [
                ClearSelfTodos(),
                TierChange(tier=target_tier, reason=reason),
                StopProcessing(),
            ]
        )
        return directives


class PruneContextTool(BaseTool):
    """Request context pruning to reduce message history."""

    def __init__(self) -> None:
        super().__init__("prune_context", "entropi")

    async def execute(self, arguments: dict[str, Any]) -> ServerResponse:
        """Return prune directive for the engine to process."""
        keep_recent = arguments.get("keep_recent", 2)
        return ServerResponse(
            result="Prune requested.",
            directives=[PruneMessages(keep_recent=keep_recent)],
        )


class EntropiServer(BaseMCPServer):
    """Entropi internal tools MCP server.

    Owns the TodoList instance and provides handoff, prune_context,
    and todo_write tools. Returns ``ServerResponse`` with native typed
    directives for engine-level side effects (tier changes, context pruning, etc.).
    """

    def __init__(self, tier_names: list[str] | None = None) -> None:
        """Initialize entropi server with internal todo list.

        Args:
            tier_names: Custom tier names for the handoff tool schema.
                When ``None``, uses the default tiers from ``handoff.json``.
                Consumer apps pass their own (e.g. ``["suggest", "validate", "execute"]``).
        """
        super().__init__("entropi")
        self._todo_list = TodoList()
        self._tier_names = tier_names
        self.register_tool(TodoWriteTool(self._todo_list))
        self.register_tool(HandoffTool(self._todo_list, tier_names))
        self.register_tool(PruneContextTool())

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str | ServerResponse:
        """Execute an entropi tool.

        Preserves JSON error format for unknown tools.
        """
        if not self._tool_registry.has_tool(name):
            return json.dumps({"error": f"Unknown tool: {name}"})
        return await self._tool_registry.dispatch(name, arguments)
