"""
TUI Presenter implementation.

Wraps the existing EntropiApp (Textual TUI) to implement the Presenter interface.
Mirrors display events to session_display.log for debugging.
"""

from collections.abc import Callable, Coroutine
from typing import TYPE_CHECKING, Any

from entropic.config.schema import EntropyConfig
from entropic.core.logging import get_display_logger
from entropic.ui.presenter import Presenter, StatusInfo
from entropic.ui.tui import EntropiApp

if TYPE_CHECKING:
    from entropic.core.compaction import CompactionResult
    from entropic.core.engine import AgentState, ToolApproval
    from entropic.core.queue import MessageQueue, QueuedMessage
    from entropic.core.todos import TodoList


class TUIPresenter(Presenter):
    """TUI presenter that delegates to EntropiApp.

    This is a thin wrapper that implements the Presenter ABC by
    delegating all calls to the underlying Textual TUI application.
    Display events are mirrored to session_display.log.
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
        self._display = get_display_logger()
        self._current_tier: str | None = None

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

    # === Tier Display ===

    def set_tier(self, tier: str) -> None:
        """Set the active model tier for display."""
        self._current_tier = tier
        self._display.info("[TIER] %s", tier.upper())
        self._app.set_tier(tier)

    def show_routing_info(self, info_text: str) -> None:
        """Display routing decision info."""
        self._display.info("[ROUTING] %s", info_text)
        self._app.show_routing_info(info_text)

    # === Delegation Display ===

    def on_delegation_start(self, child_conv_id: str, target_tier: str, task: str) -> None:
        """Push child tier onto display when delegation starts."""
        _ = child_conv_id
        self._display.info("[DELEGATION START] %s → %s", self._current_tier, target_tier)
        self._app.on_delegation_start(target_tier, task)

    def on_delegation_complete(
        self, child_conv_id: str, tier: str, summary: str, success: bool
    ) -> None:
        """Pop child tier and restore parent when delegation completes."""
        _ = (child_conv_id, summary)
        status = "OK" if success else "FAILED"
        self._display.info("[DELEGATION COMPLETE] %s %s", tier, status)
        self._app.on_delegation_complete(tier, success)

    # === Generation Lifecycle ===

    def start_generation(self) -> None:
        """Mark generation as active."""
        self._display.info("[GEN START]")
        self._app.start_generation()

    def end_generation(self) -> None:
        """Mark generation as complete."""
        self._display.info("[GEN END]")
        self._app.end_generation()

    # === Streaming ===

    def on_stream_chunk(self, chunk: str) -> None:
        """Handle streaming text chunk."""
        self._app.on_stream_chunk(chunk)

    # === Messages ===

    def add_message(self, role: str, content: str) -> None:
        """Add a message to the display."""
        tier_tag = f" ({self._current_tier})" if self._current_tier else ""
        self._display.info("[%s%s] %s", role.upper(), tier_tag, content)
        self._app.add_message(role, content)

    def print_info(self, message: str) -> None:
        """Print informational message."""
        self._display.info("[INFO] %s", message)
        self._app.print_info(message)

    def print_warning(self, message: str) -> None:
        """Print warning message."""
        self._display.info("[WARN] %s", message)
        self._app.print_warning(message)

    def print_error(self, message: str) -> None:
        """Print error message."""
        self._display.info("[ERROR] %s", message)
        self._app.print_error(message)

    # === Tool Feedback ===

    def print_tool_start(self, name: str, arguments: dict[str, Any]) -> None:
        """Display tool execution start."""
        self._display.info("[TOOL START] %s(%s)", name, arguments)
        self._app.print_tool_start(name, arguments)

    def print_tool_complete(self, name: str, result: str, duration_ms: float) -> None:
        """Display tool execution complete."""
        self._display.info("[TOOL DONE] %s (%.0fms) → %s", name, duration_ms, result)
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
        self._display.info("[STATE] %s", state)
        self._app.update_state(state)

    def print_context_usage(self, used: int, maximum: int) -> None:
        """Display context window usage."""
        self._app.print_context_usage(used, maximum)

    def print_compaction_notice(self, result: "CompactionResult") -> None:
        """Display context compaction notice."""
        self._display.info(
            "[COMPACTION] %d → %d tokens", result.old_token_count, result.new_token_count
        )
        self._app.print_compaction_notice(result)

    def print_todo_panel(self, todo_list: "TodoList") -> None:
        """Display todo list panel."""
        self._display.info("[TODOS] %d items", len(todo_list.items))
        self._app.print_todo_panel(todo_list)

    def print_status(self, status: StatusInfo) -> None:
        """Display system status."""
        self._app.print_status(status)

    # === Injection ===

    async def prompt_injection(self, partial_content: str) -> str | None:
        """Prompt for mid-generation injection."""
        return await self._app.prompt_injection(partial_content)
