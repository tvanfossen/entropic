"""
Entropi MCP server.

Provides entropi-internal tools: todo management, tier handoff,
and context pruning. Returns ``ServerResponse`` with native typed
directives for engine-level side effects.
"""

from __future__ import annotations

import asyncio
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


class EntropiServer(BaseMCPServer):
    """Entropi internal tools MCP server.

    Owns the TodoList instance and provides handoff, prune_context,
    and todo_write tools. Returns ``ServerResponse`` with native typed
    directives for engine-level side effects (tier changes, context pruning, etc.).
    """

    def __init__(self) -> None:
        """Initialize entropi server with internal todo list."""
        super().__init__("entropi")
        self._todo_list = TodoList()

    def get_tools(self) -> list[Tool]:
        """Get available entropi tools."""
        return [
            load_tool_definition("todo_write", "entropi"),
            load_tool_definition("handoff", "entropi"),
            load_tool_definition("prune_context", "entropi"),
        ]

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str | ServerResponse:
        """Execute an entropi tool.

        Args:
            name: Tool name (without server prefix)
            arguments: Tool arguments

        Returns:
            ServerResponse with native directives, or error string
        """
        handlers = {
            "todo_write": self._handle_todo_write,
            "handoff": self._handle_handoff,
            "prune_context": self._handle_prune_context,
        }
        handler = handlers.get(name)
        return handler(arguments) if handler else json.dumps({"error": f"Unknown tool: {name}"})

    def _handle_todo_write(self, arguments: dict[str, Any]) -> ServerResponse:
        """Handle todo_write tool call.

        Updates the internal todo list and returns two directives:
        - ``ContextAnchor``: text state for model context
        - ``NotifyPresenter``: structured data for TUI rendering
        """
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

    def _handle_handoff(self, arguments: dict[str, Any]) -> str | ServerResponse:
        """Handle handoff tool call.

        Validates todo requirements (execution todos must exist if list
        is non-empty), then returns directives for the engine to process:
        - Optional warning about incomplete self-todos
        - Clear self-directed todos
        - Tier change request
        - Stop processing remaining tool calls
        """
        target_tier = arguments.get("target_tier", "")
        reason = arguments.get("reason", "")

        directives = []

        # Check execution todos exist (if list is non-empty)
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

        # Warn about incomplete self-directed todos (soft gate)
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

        return ServerResponse(
            result=f"Handoff requested to {target_tier}. Reason: {reason}",
            directives=directives,
        )

    def _handle_prune_context(self, arguments: dict[str, Any]) -> ServerResponse:
        """Handle prune_context tool call.

        Returns a ``PruneMessages`` directive — the engine owns ``ctx.messages``
        and performs the actual pruning.
        """
        keep_recent = arguments.get("keep_recent", 2)
        return ServerResponse(
            result="Prune requested.",
            directives=[PruneMessages(keep_recent=keep_recent)],
        )


async def main() -> None:
    """Run the entropi server."""
    server = EntropiServer()
    await server.run()


if __name__ == "__main__":
    asyncio.run(main())
