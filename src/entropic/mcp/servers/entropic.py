"""
Entropic MCP server.

Provides entropic-internal tools: todo management, task delegation,
and context pruning. Returns ``ServerResponse`` with native typed
directives for engine-level side effects.
"""

from __future__ import annotations

import json
import logging
from typing import Any

from mcp.types import Tool

from entropic.core.directives import (
    Complete,
    ContextAnchor,
    Delegate,
    NotifyPresenter,
    PhaseChange,
    Pipeline,
    PruneMessages,
    StopProcessing,
)
from entropic.core.todos import TodoList
from entropic.mcp.servers.base import BaseMCPServer, ServerResponse, load_tool_definition
from entropic.mcp.tools import BaseTool

logger = logging.getLogger(__name__)


def _parse_json_string(value: Any, expected_type: type) -> Any:
    """Parse a stringified JSON value if it matches the expected type.

    Models sometimes emit JSON arrays/objects as strings rather than
    native structures. This normalizes them before validation.
    """
    if not isinstance(value, str):
        return value
    try:
        parsed = json.loads(value)
        if isinstance(parsed, expected_type):
            return parsed
    except (json.JSONDecodeError, TypeError):
        pass
    return value


class TodoTool(BaseTool):
    """Minimal todo tracking for child roles.

    Simplified interface: add, update, remove. No target_tier,
    no bulk_update, no replace. Active_form computed from content.
    """

    def __init__(self, todo_list: TodoList) -> None:
        super().__init__("todo", "entropic")
        self._todo_list = todo_list

    async def execute(self, arguments: dict[str, Any]) -> ServerResponse:
        """Update todo list via simplified interface."""
        result = self._todo_list.handle_simple_call(arguments)
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


class DelegateTool(BaseTool):
    """Delegate a task to a different identity tier."""

    def __init__(self, tier_names: list[str] | None) -> None:
        definition = self._build_definition(tier_names) if tier_names else None
        if definition:
            super().__init__(definition=definition)
        else:
            super().__init__("delegate", "entropic")
        self._tier_names = tier_names

    @staticmethod
    def _build_definition(tier_names: list[str]) -> Tool:
        """Build delegate tool definition with patched tier enum."""
        tool = load_tool_definition("delegate", "entropic")
        schema = dict(tool.inputSchema)
        props = dict(schema["properties"])
        target_prop = dict(props["target"])
        target_prop["enum"] = list(tier_names)
        props["target"] = target_prop
        schema["properties"] = props

        tier_list = ", ".join(f"`{t}`" for t in tier_names)
        description = (
            "This is your PRIMARY tool. Use it to assign ALL implementation "
            "work to the appropriate role. You do not write code, files, or "
            "artifacts yourself — you delegate.\n\n"
            f"Available tiers: {tier_list}"
        )
        return Tool(name=tool.name, description=description, inputSchema=schema)

    async def execute(self, arguments: dict[str, Any]) -> str | ServerResponse:
        """Validate and execute delegation."""
        target = arguments.get("target", "")
        task = arguments.get("task", "")
        max_turns = arguments.get("max_turns")

        if self._tier_names and target not in self._tier_names:
            return json.dumps(
                {"error": f"Unknown tier: '{target}'. Available tiers: {self._tier_names}"}
            )

        return ServerResponse(
            result=f"Delegation requested to {target}. Task: {task}",
            directives=[
                Delegate(target=target, task=task, max_turns=max_turns),
                StopProcessing(),
            ],
        )


class PipelineTool(BaseTool):
    """Execute a multi-stage delegation pipeline."""

    def __init__(self, tier_names: list[str] | None) -> None:
        definition = self._build_definition(tier_names) if tier_names else None
        if definition:
            super().__init__(definition=definition)
        else:
            super().__init__("pipeline", "entropic")
        self._tier_names = tier_names

    @staticmethod
    def _build_definition(tier_names: list[str]) -> Tool:
        """Build pipeline tool definition with patched tier enum."""
        tool = load_tool_definition("pipeline", "entropic")
        schema = dict(tool.inputSchema)
        props = dict(schema["properties"])
        stages_prop = dict(props["stages"])
        items = dict(stages_prop["items"])
        items["enum"] = list(tier_names)
        stages_prop["items"] = items
        props["stages"] = stages_prop
        schema["properties"] = props
        return Tool(name=tool.name, description=tool.description, inputSchema=schema)

    async def execute(self, arguments: dict[str, Any]) -> str | ServerResponse:
        """Validate and execute pipeline."""
        stages = _parse_json_string(arguments.get("stages", []), list)
        task = arguments.get("task", "")

        if len(stages) < 2:
            return json.dumps({"error": "Pipeline requires at least 2 stages."})

        if self._tier_names:
            invalid = [s for s in stages if s not in self._tier_names]
            if invalid:
                return json.dumps(
                    {"error": f"Unknown tier(s): {invalid}. Available: {self._tier_names}"}
                )

        return ServerResponse(
            result=f"Pipeline requested: {' → '.join(stages)}. Task: {task}",
            directives=[Pipeline(stages=stages, task=task), StopProcessing()],
        )


class CompleteTool(BaseTool):
    """Signal explicit completion of a delegated task."""

    def __init__(self) -> None:
        super().__init__("complete", "entropic")

    async def execute(self, arguments: dict[str, Any]) -> ServerResponse:
        """Emit Complete directive with summary."""
        summary = arguments.get("summary", "(no summary provided)")
        return ServerResponse(
            result=f"Completion signaled: {summary}",
            directives=[Complete(summary=summary), StopProcessing()],
        )


class PhaseChangeTool(BaseTool):
    """Switch the active phase within the current role."""

    def __init__(self) -> None:
        super().__init__(
            definition=Tool(
                name="phase_change",
                description="Switch to a different phase within your current role. "
                "Phases change inference parameters and available bash commands.",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "phase": {
                            "type": "string",
                            "description": "Target phase name (e.g., 'execute')",
                        },
                    },
                    "required": ["phase"],
                },
            )
        )

    async def execute(self, arguments: dict[str, Any]) -> ServerResponse:
        """Emit PhaseChange directive."""
        phase = arguments.get("phase", "")
        if not phase:
            return ServerResponse(result="Error: phase name is required.")
        return ServerResponse(
            result=f"Phase changed to: {phase}",
            directives=[PhaseChange(phase=phase)],
        )


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

    Owns the TodoList instance and provides delegate, prune_context,
    and todo tools. Returns ``ServerResponse`` with native typed
    directives for engine-level side effects (delegation, context pruning, etc.).
    """

    def __init__(self, tier_names: list[str] | None = None) -> None:
        """Initialize entropic server with internal todo list.

        Args:
            tier_names: Custom tier names for the delegate tool schema.
                When ``None``, uses the default tiers from ``delegate.json``.
                Single-tier lists skip delegate registration (delegating to
                yourself is meaningless). Consumer apps pass their own
                (e.g. ``["suggest", "validate", "execute"]``).
        """
        super().__init__("entropic")
        self._todo_list = TodoList()
        self._tier_names = tier_names
        self.register_tool(TodoTool(self._todo_list))
        if not tier_names or len(tier_names) > 1:
            self.register_tool(DelegateTool(tier_names))
            self.register_tool(PipelineTool(tier_names))
        self.register_tool(CompleteTool())
        self.register_tool(PhaseChangeTool())
        self.register_tool(PruneContextTool())

    @staticmethod
    def skip_duplicate_check(tool_name: str) -> bool:
        """Delegate must always execute — validation depends on runtime state."""
        return tool_name in ("delegate", "pipeline")

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str | ServerResponse:
        """Execute an entropic tool.

        Preserves JSON error format for unknown tools.
        """
        if not self._tool_registry.has_tool(name):
            return json.dumps({"error": f"Unknown tool: {name}"})
        return await self._tool_registry.dispatch(name, arguments)
