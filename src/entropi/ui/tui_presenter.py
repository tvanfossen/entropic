"""
TUI Presenter implementation.

Wraps the existing EntropiApp (Textual TUI) to implement the Presenter interface.
Pure delegation - no behavior changes from existing TUI.
"""

from collections.abc import Callable, Coroutine
from typing import TYPE_CHECKING, Any

from entropi.config.schema import EntropyConfig
from entropi.ui.presenter import Presenter, StatusInfo
from entropi.ui.tui import EntropiApp

if TYPE_CHECKING:
    from entropi.core.compaction import CompactionResult
    from entropi.core.engine import AgentState, ToolApproval
    from entropi.core.queue import MessageQueue, QueuedMessage
    from entropi.core.todos import TodoList


class TUIPresenter(Presenter):
    """TUI presenter that delegates to EntropiApp.

    This is a thin wrapper that implements the Presenter ABC by
    delegating all calls to the underlying Textual TUI application.
    """

    def __init__(
        self,
        config: EntropyConfig,
        version: str = "0.1.0",
        models: list[str] | None = None,
    ) -> None:
        """Initialize TUI presenter.

        Args:
            config: Application configuration
            version: Application version
            models: Available model names
        """
        self._app = EntropiApp(
            config=config,
            version=version,
            models=models,
        )

    # === Lifecycle ===

    async def run_async(self) -> None:
        """Run the TUI event loop."""
        await self._app.run_async()

    def exit(self) -> None:
        """Exit the TUI."""
        self._app.exit()

    # === Callback Registration ===

    def set_input_callback(self, callback: Callable[[str], Coroutine[Any, Any, None]]) -> None:
        """Set callback for user input."""
        self._app.set_input_callback(callback)

    def set_interrupt_callback(self, callback: Callable[[], None]) -> None:
        """Set callback for interrupt signal."""
        self._app.set_interrupt_callback(callback)

    def set_pause_callback(self, callback: Callable[[], None]) -> None:
        """Set callback for pause signal."""
        self._app.set_pause_callback(callback)

    def set_queue_consumer(
        self,
        queue: "MessageQueue",
        process_callback: Callable[["QueuedMessage"], Coroutine[Any, Any, None]],
    ) -> None:
        """Set up MCP message queue consumer."""
        self._app.set_queue_consumer(queue, process_callback)

    def set_voice_callbacks(
        self,
        on_enter: Callable[[], Coroutine[Any, Any, None]],
        on_exit: Callable[[], Coroutine[Any, Any, None]],
    ) -> None:
        """Set voice mode callbacks."""
        self._app.set_voice_callbacks(on_enter, on_exit)

    # === Generation Lifecycle ===

    def start_generation(self) -> None:
        """Mark generation as active."""
        self._app.start_generation()

    def end_generation(self) -> None:
        """Mark generation as complete."""
        self._app.end_generation()

    # === Streaming ===

    def on_stream_chunk(self, chunk: str) -> None:
        """Handle streaming text chunk."""
        self._app.on_stream_chunk(chunk)

    # === Messages ===

    def add_message(self, role: str, content: str) -> None:
        """Add a message to the display."""
        self._app.add_message(role, content)

    def print_info(self, message: str) -> None:
        """Print informational message."""
        self._app.print_info(message)

    def print_warning(self, message: str) -> None:
        """Print warning message."""
        self._app.print_warning(message)

    def print_error(self, message: str) -> None:
        """Print error message."""
        self._app.print_error(message)

    # === Tool Feedback ===

    def print_tool_start(self, name: str, arguments: dict[str, Any]) -> None:
        """Display tool execution start."""
        self._app.print_tool_start(name, arguments)

    def print_tool_complete(self, name: str, result: str, duration_ms: float) -> None:
        """Display tool execution complete."""
        self._app.print_tool_complete(name, result, duration_ms)

    async def prompt_tool_approval(
        self, name: str, arguments: dict[str, Any], is_sensitive: bool
    ) -> "ToolApproval | str":
        """Prompt user for tool approval."""
        return await self._app.prompt_tool_approval(name, arguments, is_sensitive)

    def is_sensitive_tool(self, name: str, arguments: dict[str, Any]) -> bool:
        """Check if tool invocation is sensitive."""
        return self._app.is_sensitive_tool(name, arguments)

    # === Context Feedback ===

    def update_state(self, state: "AgentState") -> None:
        """Update displayed agent state."""
        self._app.update_state(state)

    def print_context_usage(self, used: int, maximum: int) -> None:
        """Display context window usage."""
        self._app.print_context_usage(used, maximum)

    def print_compaction_notice(self, result: "CompactionResult") -> None:
        """Display context compaction notice."""
        self._app.print_compaction_notice(result)

    def print_todo_panel(self, todo_list: "TodoList") -> None:
        """Display todo list panel."""
        self._app.print_todo_panel(todo_list)

    def print_status(self, status: StatusInfo) -> None:
        """Display system status."""
        self._app.print_status(status)

    # === Injection ===

    async def prompt_injection(self, partial_content: str) -> str | None:
        """Prompt for mid-generation injection."""
        return await self._app.prompt_injection(partial_content)
