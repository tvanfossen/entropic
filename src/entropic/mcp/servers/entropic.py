"""
Entropic MCP server.

Provides entropic-internal tools: todo management, tier handoff,
and context pruning. Returns ``ServerResponse`` with native typed
directives for engine-level side effects.
"""

from __future__ import annotations

import json
import logging
from typing import Any

from mcp.types import Tool

from entropic.core.directives import (
    ClearSelfTodos,
    ContextAnchor,
    InjectContext,
    NotifyPresenter,
    PruneMessages,
    StopProcessing,
    TierChange,
)
from entropic.core.todos import TodoList, TodoStatus
from entropic.mcp.servers.base import BaseMCPServer, ServerResponse, load_tool_definition
from entropic.mcp.tools import BaseTool

logger = logging.getLogger(__name__)


class TodoWriteTool(BaseTool):
    """Manage the internal todo list."""

    def __init__(self, todo_list: TodoList, tier_names: list[str] | None = None) -> None:
        definition = self._build_definition(tier_names) if tier_names else None
        if definition:
            super().__init__(definition=definition)
        else:
            super().__init__("todo_write", "entropic")
        self._todo_list = todo_list

    @staticmethod
    def _build_definition(tier_names: list[str]) -> Tool:
        """Build todo_write definition with patched target_tier enums."""
        tool = load_tool_definition("todo_write", "entropic")
        schema = dict(tool.inputSchema)
        props = dict(schema["properties"])

        # Patch target_tier enum in todos[].items.properties
        todos_prop = dict(props["todos"])
        items = dict(todos_prop["items"])
        item_props = dict(items["properties"])
        tt = dict(item_props["target_tier"])
        tt["enum"] = list(tier_names)
        item_props["target_tier"] = tt
        items["properties"] = item_props
        todos_prop["items"] = items
        props["todos"] = todos_prop

        # Patch top-level target_tier enum (for 'update' action)
        top_tt = dict(props["target_tier"])
        top_tt["enum"] = list(tier_names)
        props["target_tier"] = top_tt

        schema["properties"] = props
        return Tool(name=tool.name, description=tool.description, inputSchema=schema)

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
            super().__init__("handoff", "entropic")
        self._todo_list = todo_list
        self._tier_names = tier_names

    @staticmethod
    def _build_definition(tier_names: list[str]) -> Tool:
        """Build handoff tool definition with patched tier enum."""
        tool = load_tool_definition("handoff", "entropic")
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
            logger.warning(
                "[HANDOFF] %d todo(s) exist but none have target_tier set. "
                "Proceeding with handoff — todos are self-directed.",
                len(self._todo_list.items),
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
        super().__init__("prune_context", "entropic")

    async def execute(self, arguments: dict[str, Any]) -> ServerResponse:
        """Return prune directive for the engine to process."""
        keep_recent = arguments.get("keep_recent", 2)
        return ServerResponse(
            result="Prune requested.",
            directives=[PruneMessages(keep_recent=keep_recent)],
        )


class EntropicServer(BaseMCPServer):
    """Entropic internal tools MCP server.

    Owns the TodoList instance and provides handoff, prune_context,
    and todo_write tools. Returns ``ServerResponse`` with native typed
    directives for engine-level side effects (tier changes, context pruning, etc.).
    """

    def __init__(self, tier_names: list[str] | None = None) -> None:
        """Initialize entropic server with internal todo list.

        Args:
            tier_names: Custom tier names for the handoff tool schema.
                When ``None``, uses the default tiers from ``handoff.json``.
                Single-tier lists skip handoff registration (handoff to
                yourself is meaningless). Consumer apps pass their own
                (e.g. ``["suggest", "validate", "execute"]``).
        """
        super().__init__("entropic")
        self._todo_list = TodoList()
        self._tier_names = tier_names
        self.register_tool(TodoWriteTool(self._todo_list, tier_names))
        if not tier_names or len(tier_names) > 1:
            self.register_tool(HandoffTool(self._todo_list, tier_names))
        self.register_tool(PruneContextTool())

    @staticmethod
    def skip_duplicate_check(tool_name: str) -> bool:
        """Handoff must always execute — validation depends on runtime state."""
        return tool_name == "handoff"

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str | ServerResponse:
        """Execute an entropic tool.

        Preserves JSON error format for unknown tools.
        """
        if not self._tool_registry.has_tool(name):
            return json.dumps({"error": f"Unknown tool: {name}"})
        return await self._tool_registry.dispatch(name, arguments)
