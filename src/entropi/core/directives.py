"""
Directive processing for tool-to-engine communication.

Tools return JSON results with an optional ``_directives`` key.
The engine extracts directives and delegates to DirectiveProcessor,
which dispatches each directive to a registered handler without
inspecting tool names.

This keeps the engine tool-agnostic: it processes generic directives
rather than hard-coding behavior per tool.
"""

from __future__ import annotations

import json
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from entropi.core.logging import get_logger

if TYPE_CHECKING:
    from entropi.core.engine import LoopContext

logger = get_logger("core.directives")

# Standard directive type constants
STOP_PROCESSING = "stop_processing"
TIER_CHANGE = "tier_change"
CLEAR_SELF_TODOS = "clear_self_todos"
INJECT_CONTEXT = "inject_context"
PRUNE_MESSAGES = "prune_messages"
TODO_STATE_CHANGED = "todo_state_changed"


@dataclass
class DirectiveResult:
    """Aggregate result of processing a batch of directives."""

    stop_processing: bool = False
    tier_changed: bool = False
    injected_messages: list[Any] = field(default_factory=list)


# Type alias for directive handler functions
DirectiveHandler = Callable[["LoopContext", dict[str, Any], DirectiveResult], None]


class DirectiveProcessor:
    """Processes tool directives generically.

    Swappable: the engine depends on DirectiveProcessor's interface,
    not its internals. Future alternatives (event bus, callbacks) can
    implement the same ``process()`` contract.
    """

    def __init__(self) -> None:
        self._handlers: dict[str, DirectiveHandler] = {}

    def register(self, directive_type: str, handler: DirectiveHandler) -> None:
        """Register a handler for a directive type.

        Args:
            directive_type: The directive type string (e.g., "stop_processing")
            handler: Callable(ctx, params, result) that processes the directive
        """
        self._handlers[directive_type] = handler

    def process(
        self,
        ctx: LoopContext,
        directives: list[dict[str, Any]],
    ) -> DirectiveResult:
        """Process a list of directives, returning aggregate result.

        Args:
            ctx: Current loop context
            directives: List of directive dicts with "type" and optional "params"

        Returns:
            Aggregate result indicating engine-level side effects
        """
        result = DirectiveResult()
        for directive in directives:
            d_type = directive.get("type", "")
            params = directive.get("params", {})
            handler = self._handlers.get(d_type)
            if handler:
                logger.debug(f"Processing directive: {d_type}")
                handler(ctx, params, result)
            else:
                logger.warning(f"Unknown directive type: {d_type}")
        return result

    @property
    def registered_types(self) -> list[str]:
        """List registered directive types."""
        return list(self._handlers.keys())


def extract_directives(tool_result_content: str) -> list[dict[str, Any]]:
    """Extract directives from a tool result JSON string.

    Tools embed directives in their JSON response under the
    ``_directives`` key. This function parses the JSON and
    extracts the directive list.

    Args:
        tool_result_content: Raw tool result string (may or may not be JSON)

    Returns:
        List of directive dicts, or empty list if none found
    """
    if not tool_result_content:
        return []
    try:
        data = json.loads(tool_result_content)
        if isinstance(data, dict):
            directives = data.get("_directives", [])
            if isinstance(directives, list):
                return directives
    except (json.JSONDecodeError, TypeError, ValueError):
        pass
    return []
