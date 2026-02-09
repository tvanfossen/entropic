"""
Abstract presenter interface for UI decoupling.

Defines the contract between Application layer and presentation layer,
enabling headless operation and swappable UI implementations.
"""

from abc import ABC, abstractmethod
from collections.abc import Callable, Coroutine
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from entropi.core.compaction import CompactionResult
    from entropi.core.engine import AgentState, ToolApproval
    from entropi.core.queue import MessageQueue, QueuedMessage
    from entropi.core.todos import TodoList


@dataclass
class StatusInfo:
    """System status information for display."""

    model: str
    vram_used: float
    vram_total: float
    tokens: int
    thinking_mode: bool
    context_used: int
    context_max: int


class Presenter(ABC):
    """Abstract interface for UI presentation layer.

    All UI interactions from Application go through this interface,
    enabling headless operation and alternative UI implementations.
    """

    # === Lifecycle ===

    @abstractmethod
    async def run_async(self) -> None:
        """Run the presenter's event loop.

        This blocks until the presenter exits.
        """
        ...

    @abstractmethod
    def exit(self) -> None:
        """Signal the presenter to exit."""
        ...

    # === Callback Registration ===

    @abstractmethod
    def set_input_callback(self, callback: Callable[[str], "Coroutine[Any, Any, None]"]) -> None:
        """Set callback for user input.

        Args:
            callback: Async function called with user input text
        """
        ...

    @abstractmethod
    def set_interrupt_callback(self, callback: Callable[[], None]) -> None:
        """Set callback for interrupt signal (hard cancel).

        Args:
            callback: Function called when user requests interrupt
        """
        ...

    @abstractmethod
    def set_pause_callback(self, callback: Callable[[], None]) -> None:
        """Set callback for pause signal (soft interrupt).

        Args:
            callback: Function called when user requests pause
        """
        ...

    def set_queue_consumer(
        self,
        queue: "MessageQueue",
        process_callback: Callable[["QueuedMessage"], Coroutine[Any, Any, None]],
    ) -> None:
        """Set up MCP message queue consumer.

        Optional - implementations may no-op if MCP integration not needed.

        Args:
            queue: Message queue to consume from
            process_callback: Callback to process each message
        """
        _ = (queue, process_callback)

    def set_voice_callbacks(
        self,
        on_enter: Callable[[], Coroutine[Any, Any, None]],
        on_exit: Callable[[], Coroutine[Any, Any, None]],
    ) -> None:
        """Set voice mode VRAM management callbacks.

        Optional - implementations may no-op if voice mode not supported.

        Args:
            on_enter: Called when entering voice mode (unload models)
            on_exit: Called when exiting voice mode (reload models)
        """
        _ = (on_enter, on_exit)

    # === Tier Display ===

    def set_tier(self, tier: str) -> None:
        """Set the active model tier for display.

        Optional - implementations may no-op if tier display not supported.

        Args:
            tier: Tier name (e.g., 'code', 'thinking', 'normal', 'simple')
        """
        _ = tier

    # === Generation Lifecycle ===

    @abstractmethod
    def start_generation(self) -> None:
        """Mark generation as active.

        Called when the model starts generating a response.
        """
        ...

    @abstractmethod
    def end_generation(self) -> None:
        """Mark generation as complete.

        Called when the model finishes generating (success or error).
        """
        ...

    # === Streaming ===

    @abstractmethod
    def on_stream_chunk(self, chunk: str) -> None:
        """Handle streaming text chunk from model.

        Args:
            chunk: Text chunk from model response
        """
        ...

    # === Messages ===

    @abstractmethod
    def add_message(self, role: str, content: str) -> None:
        """Add a message to the display.

        Args:
            role: Message role ('user', 'assistant', 'system')
            content: Message content
        """
        ...

    @abstractmethod
    def print_info(self, message: str) -> None:
        """Print informational message.

        Args:
            message: Info message to display
        """
        ...

    @abstractmethod
    def print_warning(self, message: str) -> None:
        """Print warning message.

        Args:
            message: Warning message to display
        """
        ...

    @abstractmethod
    def print_error(self, message: str) -> None:
        """Print error message.

        Args:
            message: Error message to display
        """
        ...

    # === Tool Feedback ===

    @abstractmethod
    def print_tool_start(self, name: str, arguments: dict[str, Any]) -> None:
        """Display tool execution start.

        Args:
            name: Tool name
            arguments: Tool arguments
        """
        ...

    @abstractmethod
    def print_tool_complete(self, name: str, result: str, duration_ms: float) -> None:
        """Display tool execution complete.

        Args:
            name: Tool name
            result: Tool result
            duration_ms: Execution time in milliseconds
        """
        ...

    @abstractmethod
    async def prompt_tool_approval(
        self, name: str, arguments: dict[str, Any], is_sensitive: bool
    ) -> "ToolApproval | str":
        """Prompt user for tool approval.

        Args:
            name: Tool name
            arguments: Tool arguments
            is_sensitive: Whether this is a sensitive operation

        Returns:
            ToolApproval enum value or feedback string for denial
        """
        ...

    @abstractmethod
    def is_sensitive_tool(self, name: str, arguments: dict[str, Any]) -> bool:
        """Check if tool invocation is sensitive.

        Args:
            name: Tool name
            arguments: Tool arguments

        Returns:
            True if the tool call should require approval
        """
        ...

    # === Context Feedback ===

    @abstractmethod
    def update_state(self, state: "AgentState") -> None:
        """Update displayed agent state.

        Args:
            state: Current agent state
        """
        ...

    @abstractmethod
    def print_context_usage(self, used: int, maximum: int) -> None:
        """Display context window usage.

        Args:
            used: Tokens used
            maximum: Maximum tokens
        """
        ...

    @abstractmethod
    def print_compaction_notice(self, result: "CompactionResult") -> None:
        """Display context compaction notice.

        Args:
            result: Compaction result with token counts
        """
        ...

    @abstractmethod
    def print_todo_panel(self, todo_list: "TodoList") -> None:
        """Display todo list panel.

        Args:
            todo_list: Current todo list
        """
        ...

    @abstractmethod
    def print_status(self, status: StatusInfo) -> None:
        """Display system status.

        Args:
            status: Status information to display
        """
        ...

    # === Injection ===

    @abstractmethod
    async def prompt_injection(self, partial_content: str) -> str | None:
        """Prompt for mid-generation injection.

        Called when generation is paused to allow user to inject context.

        Args:
            partial_content: The partial response generated so far

        Returns:
            User's injection text, empty string to resume, or None to cancel
        """
        ...
