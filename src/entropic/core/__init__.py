"""Core module for Entropic."""

from typing import Any

from entropic.core.base import (
    GenerationResult,
    Message,
    ModelBackend,
    ModelState,
    StorageBackend,
    ToolCall,
    ToolProvider,
    ToolResult,
)
from entropic.core.context import (
    ContextBuilder,
    ContextCompactor,
    ProjectContext,
    TokenBudget,
)
from entropic.core.logging import (
    get_display_logger,
    get_logger,
    get_model_logger,
    setup_display_logger,
    setup_logging,
    setup_model_logger,
)
from entropic.core.parser import ToolCallParser
from entropic.core.todos import TodoItem, TodoList, TodoStatus

# Lazy imports to avoid circular dependency with inference module
# Import AgentEngine, AgentState, etc. directly from entropic.core.engine when needed


def __getattr__(name: str) -> Any:
    """Lazy import for engine module to avoid circular imports."""
    if name in (
        "AgentEngine",
        "AgentState",
        "InterruptContext",
        "InterruptMode",
        "LoopConfig",
        "LoopContext",
        "LoopMetrics",
        "ToolApproval",
    ):
        from entropic.core import engine as _engine_module

        return getattr(_engine_module, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


__all__ = [
    "AgentEngine",
    "AgentState",
    "ContextBuilder",
    "ContextCompactor",
    "GenerationResult",
    "InterruptContext",
    "InterruptMode",
    "LoopConfig",
    "LoopContext",
    "LoopMetrics",
    "Message",
    "ModelBackend",
    "ModelState",
    "ProjectContext",
    "StorageBackend",
    "TodoItem",
    "TodoList",
    "TodoStatus",
    "TokenBudget",
    "ToolApproval",
    "ToolCall",
    "ToolCallParser",
    "ToolProvider",
    "ToolResult",
    "get_display_logger",
    "get_logger",
    "get_model_logger",
    "setup_display_logger",
    "setup_logging",
    "setup_model_logger",
]
