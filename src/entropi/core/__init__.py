"""Core module for Entropi."""

from typing import Any

from entropi.core.base import (
    GenerationResult,
    Message,
    ModelBackend,
    StorageBackend,
    ToolCall,
    ToolProvider,
    ToolResult,
)
from entropi.core.commands import (
    Command,
    CommandContext,
    CommandDefinition,
    CommandRegistry,
    CommandResult,
)
from entropi.core.context import (
    ContextBuilder,
    ContextCompactor,
    ProjectContext,
    TokenBudget,
)
from entropi.core.logging import get_logger, get_model_logger, setup_logging, setup_model_logger
from entropi.core.parser import ToolCallParser
from entropi.core.todos import TodoItem, TodoList, TodoStatus

# Lazy imports to avoid circular dependency with inference module
# Import AgentEngine, AgentState, etc. directly from entropi.core.engine when needed


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
        from entropi.core import engine as _engine_module

        return getattr(_engine_module, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


__all__ = [
    "AgentEngine",
    "AgentState",
    "Command",
    "CommandContext",
    "CommandDefinition",
    "CommandRegistry",
    "CommandResult",
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
    "get_logger",
    "get_model_logger",
    "setup_logging",
    "setup_model_logger",
]
