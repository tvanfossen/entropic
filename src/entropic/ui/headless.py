"""
Headless presenter for programmatic/testing use.

Provides a Presenter implementation that runs without a TUI,
enabling automated testing and scripting.
"""

import asyncio
import logging
from collections.abc import Callable, Coroutine
from typing import TYPE_CHECKING, Any

from entropic.core.engine import ToolApproval
from entropic.ui.presenter import Presenter, StatusInfo

if TYPE_CHECKING:
    from entropic.core.compaction import CompactionResult
    from entropic.core.engine import AgentState
    from entropic.core.queue import MessageQueue, QueuedMessage
    from entropic.core.todos import TodoList


class HeadlessPresenter(Presenter):
    """Headless presenter for programmatic/testing use.

    Runs without a TUI, processing input from a queue and capturing
    output for testing/inspection.
    """

    def __init__(self, auto_approve: bool = True) -> None:
        """Initialize headless presenter.

        Args:
            auto_approve: Whether to auto-approve all tool calls
        """
        self._logger = logging.getLogger(__name__)

        # Callbacks
        self._input_callback: Callable[[str], Coroutine[Any, Any, None]] | None = None
        self._interrupt_callback: Callable[[], None] | None = None
        self._pause_callback: Callable[[], None] | None = None

        # State
        self._running = False
        self._generating = False
        self._auto_approve = auto_approve

        # Input/output queues for programmatic control
        self._input_queue: asyncio.Queue[str] = asyncio.Queue()
        self._exit_event = asyncio.Event()

        # Captured output for testing
        self._stream_buffer: list[str] = []
        self._messages: list[tuple[str, str]] = []  # (role, content)
        self._info_messages: list[str] = []
        self._warning_messages: list[str] = []
        self._error_messages: list[str] = []
        self._tool_calls: list[dict[str, Any]] = []

    # === Lifecycle ===

    async def run_async(self) -> None:
        """Process input queue until exit."""
        self._running = True
        self._exit_event.clear()

        while self._running:
            try:
                # Wait for input with timeout to check exit flag
                try:
                    user_input = await asyncio.wait_for(self._input_queue.get(), timeout=0.1)
                except TimeoutError:
                    # Check if we should exit
                    if self._exit_event.is_set():
                        break
                    continue

                # Process input through callback
                if self._input_callback:
                    await self._input_callback(user_input)

            except asyncio.CancelledError:
                break

        self._running = False

    def exit(self) -> None:
        """Signal the presenter to exit."""
        self._running = False
        self._exit_event.set()

    # === Callback Registration ===

    def set_input_callback(self, callback: Callable[[str], Coroutine[Any, Any, None]]) -> None:
        """Set callback for user input."""
        self._input_callback = callback

    def set_interrupt_callback(self, callback: Callable[[], None]) -> None:
        """Set callback for interrupt signal."""
        self._interrupt_callback = callback

    def set_pause_callback(self, callback: Callable[[], None]) -> None:
        """Set callback for pause signal."""
        self._pause_callback = callback

    def set_queue_consumer(
        self,
        queue: "MessageQueue",
        process_callback: Callable[["QueuedMessage"], Coroutine[Any, Any, None]],
    ) -> None:
        """Set up MCP message queue consumer.

        For headless mode, we don't start a background consumer by default.
        Tests can manually consume from the queue if needed.
        """
        _ = (queue, process_callback)

    def set_voice_callbacks(
        self,
        on_enter: Callable[[], Coroutine[Any, Any, None]],
        on_exit: Callable[[], Coroutine[Any, Any, None]],
    ) -> None:
        """Set voice mode callbacks - no-op for headless."""
        _ = (on_enter, on_exit)

    # === Tier Display ===

    def set_tier(self, tier: str) -> None:
        """Set the active model tier - logged only."""
        self._logger.debug(f"Tier: {tier}")

    def show_routing_info(self, info_text: str) -> None:
        """Display routing info - logged only."""
        self._logger.info(f"Routing: {info_text}")

    # === Generation Lifecycle ===

    def start_generation(self) -> None:
        """Mark generation as active."""
        self._generating = True
        self._stream_buffer.clear()
        self._logger.debug("Generation started")

    def end_generation(self) -> None:
        """Mark generation as complete."""
        self._generating = False
        self._logger.debug("Generation ended")

    # === Streaming ===

    def on_stream_chunk(self, chunk: str) -> None:
        """Handle streaming text chunk."""
        self._stream_buffer.append(chunk)

    # === Messages ===

    def add_message(self, role: str, content: str) -> None:
        """Add a message to the captured output."""
        self._messages.append((role, content))
        self._logger.debug(f"[{role}] {content[:50]}...")

    def print_info(self, message: str) -> None:
        """Print informational message."""
        self._info_messages.append(message)
        self._logger.info(message)

    def print_warning(self, message: str) -> None:
        """Print warning message."""
        self._warning_messages.append(message)
        self._logger.warning(message)

    def print_error(self, message: str) -> None:
        """Print error message."""
        self._error_messages.append(message)
        self._logger.error(message)

    # === Tool Feedback ===

    def print_tool_start(self, name: str, arguments: dict[str, Any]) -> None:
        """Display tool execution start."""
        self._tool_calls.append(
            {
                "name": name,
                "arguments": arguments,
                "status": "running",
            }
        )
        self._logger.debug(f"Tool start: {name}")

    def print_tool_complete(self, name: str, result: str, duration_ms: float) -> None:
        """Display tool execution complete."""
        # Update the last tool call with result
        for tc in reversed(self._tool_calls):
            if tc["name"] == name and tc["status"] == "running":
                tc["status"] = "complete"
                tc["result"] = result
                tc["duration_ms"] = duration_ms
                break
        self._logger.debug(f"Tool complete: {name} ({duration_ms:.0f}ms)")

    async def prompt_tool_approval(
        self, name: str, arguments: dict[str, Any], is_sensitive: bool
    ) -> ToolApproval | str:
        """Auto-approve tools in headless mode."""
        _ = (name, arguments, is_sensitive)
        if self._auto_approve:
            return ToolApproval.ALLOW
        return ToolApproval.DENY

    def is_sensitive_tool(self, name: str, arguments: dict[str, Any]) -> bool:
        """Check if tool invocation is sensitive."""
        sensitive_tools = {
            "bash.execute",
            "filesystem.write_file",
            "filesystem.delete",
            "git.commit",
            "git.push",
        }
        if name in sensitive_tools:
            return True
        dangerous_patterns = ["rm ", "sudo", "chmod", "chown", "> /", "| sh"]
        for value in arguments.values():
            if isinstance(value, str):
                for pattern in dangerous_patterns:
                    if pattern in value:
                        return True
        return False

    # === Context Feedback ===

    def update_state(self, state: "AgentState") -> None:
        """Update displayed agent state - logged only."""
        self._logger.debug(f"State: {state}")

    def print_context_usage(self, used: int, maximum: int) -> None:
        """Display context window usage - logged only."""
        pct = (used / maximum * 100) if maximum > 0 else 0
        self._logger.debug(f"Context: {used}/{maximum} ({pct:.1f}%)")

    def print_compaction_notice(self, result: "CompactionResult") -> None:
        """Display context compaction notice - logged only."""
        self._logger.info(
            f"Context compacted: {result.old_token_count} -> {result.new_token_count}"
        )

    def print_todo_panel(self, todo_list: "TodoList") -> None:
        """Display todo list panel - logged only."""
        if not todo_list.is_empty:
            self._logger.debug(f"Todos: {len(todo_list.items)} items")

    def print_status(self, status: StatusInfo) -> None:
        """Display system status - logged only."""
        self._logger.debug(
            f"Status: model={status.model}, thinking={status.thinking_mode}, "
            f"context={status.context_used}/{status.context_max}"
        )

    # === Injection ===

    async def prompt_injection(self, partial_content: str) -> str | None:
        """Prompt for mid-generation injection - returns None in headless."""
        _ = partial_content
        return None

    # === Testing Interface ===

    async def send_input(self, text: str) -> None:
        """Send input programmatically (for testing).

        Args:
            text: User input to process
        """
        await self._input_queue.put(text)

    def get_stream_content(self) -> str:
        """Get the accumulated stream content.

        Returns:
            Concatenated stream chunks
        """
        return "".join(self._stream_buffer)

    def get_messages(self) -> list[tuple[str, str]]:
        """Get captured messages.

        Returns:
            List of (role, content) tuples
        """
        return self._messages.copy()

    def get_info_messages(self) -> list[str]:
        """Get captured info messages."""
        return self._info_messages.copy()

    def get_warning_messages(self) -> list[str]:
        """Get captured warning messages."""
        return self._warning_messages.copy()

    def get_error_messages(self) -> list[str]:
        """Get captured error messages."""
        return self._error_messages.copy()

    def get_tool_calls(self) -> list[dict[str, Any]]:
        """Get captured tool calls."""
        return self._tool_calls.copy()

    def clear_captured(self) -> None:
        """Clear all captured output."""
        self._stream_buffer.clear()
        self._messages.clear()
        self._info_messages.clear()
        self._warning_messages.clear()
        self._error_messages.clear()
        self._tool_calls.clear()

    def interrupt(self) -> None:
        """Trigger interrupt callback (for testing)."""
        if self._interrupt_callback:
            self._interrupt_callback()

    def pause(self) -> None:
        """Trigger pause callback (for testing)."""
        if self._pause_callback:
            self._pause_callback()
