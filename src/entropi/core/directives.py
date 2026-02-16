"""
Directive processing for tool-to-engine communication.

Tools return JSON results with an optional ``_directives`` key.
The engine extracts directives and delegates to DirectiveProcessor,
which dispatches each typed directive to a registered handler without
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


# ---------------------------------------------------------------------------
# Directive dataclass hierarchy
# ---------------------------------------------------------------------------


@dataclass
class Directive:
    """Base class for all typed directives."""

    pass


@dataclass
class StopProcessing(Directive):
    """Stop processing remaining tool calls."""

    pass


@dataclass
class TierChange(Directive):
    """Request a tier handoff."""

    tier: str
    reason: str = ""


@dataclass
class ClearSelfTodos(Directive):
    """Clear self-directed todos (handled server-side, engine no-op)."""

    pass


@dataclass
class InjectContext(Directive):
    """Inject a message into the conversation context."""

    content: str
    role: str = "user"


@dataclass
class PruneMessages(Directive):
    """Prune old tool results from context."""

    keep_recent: int = 2


@dataclass
class ContextAnchor(Directive):
    """Push state to a persistent context anchor.

    The engine maintains a keyed dict of anchors. Each anchor becomes a
    single message in ctx.messages, updated in-place on change, and
    re-injected after compaction or tier change. Empty content removes
    the anchor.
    """

    key: str
    content: str


@dataclass
class NotifyPresenter(Directive):
    """Generic UI notification — engine passes through, doesn't inspect.

    The engine fires a callback with (key, data). App.py dispatches
    to the appropriate presenter method based on the key.
    """

    key: str
    data: dict = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Wire format type names (used by MCP servers for JSON serialization)
# These go away when P1-018 (in-process tools) eliminates the subprocess
# boundary and servers return list[Directive] directly.
# ---------------------------------------------------------------------------

STOP_PROCESSING = "stop_processing"
TIER_CHANGE = "tier_change"
CLEAR_SELF_TODOS = "clear_self_todos"
INJECT_CONTEXT = "inject_context"
PRUNE_MESSAGES = "prune_messages"
CONTEXT_ANCHOR = "context_anchor"
NOTIFY_PRESENTER = "notify_presenter"


# ---------------------------------------------------------------------------
# Deserialization: JSON dict → typed Directive
# ---------------------------------------------------------------------------

_DIRECTIVE_REGISTRY: dict[str, type[Directive]] = {
    "stop_processing": StopProcessing,
    "tier_change": TierChange,
    "clear_self_todos": ClearSelfTodos,
    "inject_context": InjectContext,
    "prune_messages": PruneMessages,
    "context_anchor": ContextAnchor,
    "notify_presenter": NotifyPresenter,
}


def deserialize_directive(raw: dict[str, Any]) -> Directive:
    """Convert a raw directive dict to a typed Directive.

    Args:
        raw: Dict with "type" key and optional "params" dict.

    Returns:
        Typed Directive instance.

    Raises:
        KeyError: If the directive type is not in the registry.
        TypeError: If params don't match the dataclass fields.
    """
    d_type = raw.get("type", "")
    cls = _DIRECTIVE_REGISTRY.get(d_type)
    if cls is None:
        raise KeyError(f"Unknown directive type: {d_type!r}")
    params = raw.get("params", {})
    return cls(**params)


# ---------------------------------------------------------------------------
# Aggregate result
# ---------------------------------------------------------------------------


@dataclass
class DirectiveResult:
    """Aggregate result of processing a batch of directives."""

    stop_processing: bool = False
    tier_changed: bool = False
    injected_messages: list[Any] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Processor
# ---------------------------------------------------------------------------

# Type alias for directive handler functions.
# Uses Any for the directive param because Callable is contravariant:
# handlers accept specific subclasses but the registry stores them generically.
DirectiveHandler = Callable[["LoopContext", Any, DirectiveResult], None]


class DirectiveProcessor:
    """Processes tool directives generically.

    Swappable: the engine depends on DirectiveProcessor's interface,
    not its internals. Future alternatives (event bus, callbacks) can
    implement the same ``process()`` contract.
    """

    def __init__(self) -> None:
        self._handlers: dict[type[Directive], DirectiveHandler] = {}

    def register(
        self,
        directive_type: type[Directive],
        handler: DirectiveHandler,
    ) -> None:
        """Register a handler for a directive type.

        Args:
            directive_type: The directive dataclass type
            handler: Callable(ctx, directive, result) that processes it
        """
        self._handlers[directive_type] = handler

    def process(
        self,
        ctx: LoopContext,
        directives: list[Directive],
    ) -> DirectiveResult:
        """Process a list of directives, returning aggregate result.

        Args:
            ctx: Current loop context
            directives: List of typed Directive instances

        Returns:
            Aggregate result indicating engine-level side effects
        """
        result = DirectiveResult()
        for directive in directives:
            handler = self._handlers.get(type(directive))
            if handler:
                logger.debug(f"Processing directive: {type(directive).__name__}")
                handler(ctx, directive, result)
            else:
                logger.warning(f"No handler for directive: {type(directive).__name__}")
        return result

    @property
    def registered_types(self) -> list[type[Directive]]:
        """List registered directive types."""
        return list(self._handlers.keys())


# ---------------------------------------------------------------------------
# Extraction from JSON tool results
# ---------------------------------------------------------------------------


def extract_directives(tool_result_content: str) -> list[Directive]:
    """Extract and deserialize directives from a tool result JSON string.

    Tools embed directives in their JSON response under the
    ``_directives`` key. This function parses the JSON and
    deserializes each directive dict into a typed Directive.

    Args:
        tool_result_content: Raw tool result string (may or may not be JSON)

    Returns:
        List of typed Directive instances, or empty list if none found
    """
    if not tool_result_content:
        return []
    try:
        data = json.loads(tool_result_content)
        if isinstance(data, dict):
            raw_directives = data.get("_directives", [])
            if isinstance(raw_directives, list):
                result: list[Directive] = []
                for raw in raw_directives:
                    try:
                        result.append(deserialize_directive(raw))
                    except (KeyError, TypeError) as exc:
                        logger.warning(f"Skipping invalid directive: {exc}")
                return result
    except (json.JSONDecodeError, TypeError, ValueError):
        pass
    return []
