"""
Textual-based TUI for Entropi.

Provides unified input handling that works during streaming,
replacing the Rich + prompt_toolkit approach.
"""

from __future__ import annotations

import asyncio
import difflib
import os
from collections.abc import Callable, Coroutine
from typing import TYPE_CHECKING, Any

from rich.panel import Panel
from rich.text import Text
from textual import events, on, work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import Header, Input, Static

from entropic.config.schema import EntropyConfig
from entropic.core.compaction import CompactionResult
from entropic.core.engine import AgentState, ToolApproval
from entropic.core.logging import get_logger
from entropic.ui.presenter import StatusInfo
from entropic.ui.themes import get_theme
from entropic.ui.widgets import (
    AssistantMessage,
    ContextBar,
    ProcessingIndicator,
    RouterInfoWidget,
    StatusFooter,
    ThinkingBlock,
    TodoWidget,
    ToolCallData,
    ToolCallWidget,
    UserMessage,
)

if TYPE_CHECKING:
    from entropic.core.queue import MessageQueue, QueuedMessage
    from entropic.core.todos import TodoList

logger = get_logger("ui.tui")


class ToolApprovalScreen(ModalScreen[ToolApproval | str]):
    """Modal screen for tool approval with arrow-key navigation."""

    BINDINGS = [
        Binding("up", "move_up", "Up", show=False),
        Binding("down", "move_down", "Down", show=False),
        Binding("k", "move_up", "Up", show=False),  # Vim-style
        Binding("j", "move_down", "Down", show=False),  # Vim-style
        Binding("enter", "select", "Select", show=True),
        Binding("escape", "cancel", "Cancel", show=True),
        # Keep shortcut keys as alternatives
        Binding("y", "quick_approve", "Yes", show=False),
        Binding("a", "quick_always_allow", "Always", show=False),
        Binding("n", "quick_deny", "No", show=False),
        Binding("d", "quick_always_deny", "Deny", show=False),
    ]

    # Options available for selection
    OPTIONS: list[tuple[str, ToolApproval | str]] = [
        ("Allow Once", ToolApproval.ALLOW),
        ("Always Allow", ToolApproval.ALWAYS_ALLOW),
        ("Deny Once", ToolApproval.DENY),
        ("Always Deny", ToolApproval.ALWAYS_DENY),
        ("Add Response + Deny", "feedback"),  # Special marker for feedback mode
    ]

    def __init__(
        self,
        tool_name: str,
        arguments: dict[str, Any],
        is_sensitive: bool = False,
    ) -> None:
        """
        Initialize tool approval screen.

        Args:
            tool_name: Name of the tool
            arguments: Tool arguments
            is_sensitive: Whether this is a sensitive operation
        """
        super().__init__()
        self._tool_name = tool_name
        self._arguments = arguments
        self._is_sensitive = is_sensitive
        self._selected_index = 0
        self._feedback_mode = False

    def compose(self) -> ComposeResult:
        """Compose the modal content."""
        color = "yellow" if self._is_sensitive else "cyan"
        icon = "!" if self._is_sensitive else "?"

        # Format arguments — show full content, scroll handles overflow
        args_lines = []
        for key, value in self._arguments.items():
            value_str = repr(value)
            args_lines.append(f"  [dim]{key}:[/] {value_str}")
        args_text = "\n".join(args_lines) if args_lines else "  (no arguments)"

        with Vertical():
            yield Static(
                f"[{color}][{icon}] Tool Approval Required[/]",
                classes="title",
            )
            yield Static(f"[bold]{self._tool_name}[/]", classes="tool-name")
            with VerticalScroll(id="approval-content"):
                yield Static(args_text, classes="args")
            yield Static("", id="options-display", classes="options")
            yield Input(
                placeholder="Type feedback and press Enter...",
                id="feedback-input",
                classes="hidden",
            )
            yield Static(
                "[dim]↑/↓ to select, Enter to confirm, Esc to cancel[/]",
                classes="hint",
            )

    def on_mount(self) -> None:
        """Update options display on mount."""
        self._update_options_display()
        # Ensure screen can receive key events
        self.can_focus = True
        self.focus()

    def on_key(self, event: events.Key) -> None:
        """Handle key events for arrow navigation."""
        if self._feedback_mode:
            return  # Let input handle keys

        if event.key in ("up", "k"):
            self._selected_index = (self._selected_index - 1) % len(self.OPTIONS)
            self._update_options_display()
            event.prevent_default()
            event.stop()
        elif event.key in ("down", "j"):
            self._selected_index = (self._selected_index + 1) % len(self.OPTIONS)
            self._update_options_display()
            event.prevent_default()
            event.stop()
        elif event.key == "enter":
            self.action_select()
            event.prevent_default()
            event.stop()
        elif event.key == "y":
            self.action_quick_approve()
            event.prevent_default()
            event.stop()
        elif event.key == "a":
            self.action_quick_always_allow()
            event.prevent_default()
            event.stop()
        elif event.key == "n":
            self.action_quick_deny()
            event.prevent_default()
            event.stop()
        elif event.key == "d":
            self.action_quick_always_deny()
            event.prevent_default()
            event.stop()

    def _update_options_display(self) -> None:
        """Update the options display with current selection."""
        lines = []
        for i, (label, _) in enumerate(self.OPTIONS):
            if i == self._selected_index:
                lines.append(f"  [bold cyan]> {label}[/]")
            else:
                lines.append(f"    [dim]{label}[/]")

        options_widget = self.query_one("#options-display", Static)
        options_widget.update("\n".join(lines))

    def action_move_up(self) -> None:
        """Move selection up."""
        if self._feedback_mode:
            return
        self._selected_index = (self._selected_index - 1) % len(self.OPTIONS)
        self._update_options_display()

    def action_move_down(self) -> None:
        """Move selection down."""
        if self._feedback_mode:
            return
        self._selected_index = (self._selected_index + 1) % len(self.OPTIONS)
        self._update_options_display()

    def action_select(self) -> None:
        """Select the current option."""
        if self._feedback_mode:
            return  # Let input handle Enter

        _, value = self.OPTIONS[self._selected_index]
        if value == "feedback":
            # Enter feedback mode
            self._feedback_mode = True
            input_widget = self.query_one("#feedback-input", Input)
            input_widget.remove_class("hidden")
            input_widget.focus()
        else:
            self.dismiss(value)

    def action_cancel(self) -> None:
        """Cancel and deny."""
        if self._feedback_mode:
            # Exit feedback mode without submitting
            self._feedback_mode = False
            input_widget = self.query_one("#feedback-input", Input)
            input_widget.add_class("hidden")
            input_widget.value = ""
        else:
            self.dismiss(ToolApproval.DENY)

    @on(Input.Submitted, "#feedback-input")
    def on_feedback_submitted(self, event: Input.Submitted) -> None:
        """Handle feedback submission."""
        feedback = event.value.strip()
        if feedback:
            # Return feedback string (engine will use as denial reason)
            self.dismiss(feedback)
        else:
            # Empty feedback = just deny
            self.dismiss(ToolApproval.DENY)

    # Quick shortcut actions (alternative to arrow navigation)
    def action_quick_approve(self) -> None:
        """Approve once (y shortcut)."""
        if not self._feedback_mode:
            self.dismiss(ToolApproval.ALLOW)

    def action_quick_always_allow(self) -> None:
        """Always allow (a shortcut)."""
        if not self._feedback_mode:
            self.dismiss(ToolApproval.ALWAYS_ALLOW)

    def action_quick_deny(self) -> None:
        """Deny once (n shortcut)."""
        if not self._feedback_mode:
            self.dismiss(ToolApproval.DENY)

    def action_quick_always_deny(self) -> None:
        """Always deny (d shortcut)."""
        if not self._feedback_mode:
            self.dismiss(ToolApproval.ALWAYS_DENY)


class FileEditApprovalScreen(ModalScreen[ToolApproval | str]):
    """Specialized approval modal for file edit/write operations with content preview."""

    OPTIONS: list[tuple[str, ToolApproval | str]] = [
        ("Allow Once", ToolApproval.ALLOW),
        ("Always Allow", ToolApproval.ALWAYS_ALLOW),
        ("Deny Once", ToolApproval.DENY),
        ("Always Deny", ToolApproval.ALWAYS_DENY),
        ("Add Response + Deny", "feedback"),
    ]

    def __init__(
        self,
        tool_name: str,
        arguments: dict[str, Any],
        is_sensitive: bool = False,
    ) -> None:
        super().__init__()
        self._tool_name = tool_name
        self._arguments = arguments
        self._is_sensitive = is_sensitive
        self._selected_index = 0
        self._feedback_mode = False

    def compose(self) -> ComposeResult:
        """Compose the modal with file content preview."""
        color = "yellow" if self._is_sensitive else "cyan"
        path = self._arguments.get("path", "unknown")

        with Vertical(id="file-approval-container"):
            yield Static(
                f"[{color}]File Operation: {self._tool_name}[/]",
                classes="title",
            )
            yield Static(f"[bold]Path:[/] {path}", classes="file-path")

            # Scrollable content area — show full content, no truncation
            with VerticalScroll(id="approval-content"):
                if self._tool_name == "filesystem.edit_file":
                    old_str = self._arguments.get("old_string", "")
                    new_str = self._arguments.get("new_string", "")
                    insert_line = self._arguments.get("insert_line")

                    if insert_line is not None:
                        yield Static(
                            f"[dim]Insert at line {insert_line}:[/]",
                            classes="edit-label",
                        )
                        yield Static(
                            Panel(new_str, title="Content to Insert", border_style="green"),
                            classes="content-preview",
                        )
                    else:
                        diff_text = self._format_diff(old_str, new_str)
                        yield Static(
                            Panel(diff_text, title="Diff", border_style="cyan"),
                            classes="content-preview",
                        )

                elif self._tool_name == "filesystem.write_file":
                    content = self._arguments.get("content", "")
                    lines = content.count("\n") + 1
                    chars = len(content)
                    yield Static(f"[dim]{lines} lines, {chars} chars[/]", classes="file-stats")
                    preview_lines = content.split("\n")
                    numbered = "\n".join(
                        f"{i + 1:3} │ {line}" for i, line in enumerate(preview_lines)
                    )
                    yield Static(
                        Panel(numbered, title="Content Preview", border_style="cyan"),
                        classes="content-preview",
                    )

            yield Static("", id="options-display", classes="options")
            yield Input(
                placeholder="Type feedback and press Enter...",
                id="feedback-input",
                classes="hidden",
            )
            yield Static(
                "[dim]↑/↓ to select, Enter to confirm, Esc to cancel[/]",
                classes="hint",
            )

    @staticmethod
    def _format_diff(old: str, new: str) -> str:
        """Format old/new strings as a unified diff with Rich markup."""
        old_lines = old.splitlines(keepends=True)
        new_lines = new.splitlines(keepends=True)
        diff = difflib.unified_diff(
            old_lines,
            new_lines,
            lineterm="",
        )
        parts: list[str] = []
        for line in diff:
            stripped = line.rstrip("\n")
            if stripped.startswith("---") or stripped.startswith("+++"):
                continue
            if stripped.startswith("@@"):
                parts.append(f"[dim]{stripped}[/]")
            elif stripped.startswith("-"):
                parts.append(f"[red]{stripped}[/]")
            elif stripped.startswith("+"):
                parts.append(f"[green]{stripped}[/]")
            else:
                parts.append(f"[dim]{stripped}[/]")
        return "\n".join(parts) if parts else "[dim](no changes)[/]"

    def on_mount(self) -> None:
        self._update_options_display()
        self.can_focus = True
        self.focus()

    def on_key(self, event: events.Key) -> None:
        if self._feedback_mode:
            return
        if event.key in ("up", "k"):
            self._selected_index = (self._selected_index - 1) % len(self.OPTIONS)
            self._update_options_display()
            event.prevent_default()
            event.stop()
        elif event.key in ("down", "j"):
            self._selected_index = (self._selected_index + 1) % len(self.OPTIONS)
            self._update_options_display()
            event.prevent_default()
            event.stop()
        elif event.key == "enter":
            self._do_select()
            event.prevent_default()
            event.stop()
        elif event.key == "y":
            self.dismiss(ToolApproval.ALLOW)
        elif event.key == "a":
            self.dismiss(ToolApproval.ALWAYS_ALLOW)
        elif event.key == "n":
            self.dismiss(ToolApproval.DENY)
        elif event.key == "d":
            self.dismiss(ToolApproval.ALWAYS_DENY)
        elif event.key == "escape":
            self._do_cancel()

    def _update_options_display(self) -> None:
        lines = []
        for i, (label, _) in enumerate(self.OPTIONS):
            if i == self._selected_index:
                lines.append(f"  [bold cyan]> {label}[/]")
            else:
                lines.append(f"    [dim]{label}[/]")
        self.query_one("#options-display", Static).update("\n".join(lines))

    def _do_select(self) -> None:
        if self._feedback_mode:
            return
        _, value = self.OPTIONS[self._selected_index]
        if value == "feedback":
            self._feedback_mode = True
            input_widget = self.query_one("#feedback-input", Input)
            input_widget.remove_class("hidden")
            input_widget.focus()
        else:
            self.dismiss(value)

    def _do_cancel(self) -> None:
        if self._feedback_mode:
            self._feedback_mode = False
            input_widget = self.query_one("#feedback-input", Input)
            input_widget.add_class("hidden")
            input_widget.value = ""
        else:
            self.dismiss(ToolApproval.DENY)

    @on(Input.Submitted, "#feedback-input")
    def on_feedback_submitted(self, event: Input.Submitted) -> None:
        feedback = event.value.strip()
        self.dismiss(feedback if feedback else ToolApproval.DENY)


class ToolDetailScreen(ModalScreen[None]):
    """Read-only modal showing full tool call details."""

    BINDINGS = [
        Binding("escape", "close", "Close", show=True),
        Binding("q", "close", "Close", show=False),
    ]

    def __init__(
        self,
        name: str,
        arguments: dict[str, Any],
        result: str | None,
        duration_ms: float | None,
        status: str,
    ) -> None:
        super().__init__()
        self._tool_name = name
        self._arguments = arguments
        self._result = result
        self._duration_ms = duration_ms
        self._status = status

    def compose(self) -> ComposeResult:
        """Compose the detail modal."""
        icon = "[green]✓[/]" if self._status == "complete" else "[red]✗[/]"
        duration = f" ({self._duration_ms:.0f}ms)" if self._duration_ms else ""
        header = f"{icon} [bold]{self._tool_name}[/]{duration}"

        import json

        args_str = json.dumps(self._arguments, indent=2, default=str)

        with Vertical():
            with Horizontal(classes="detail-header"):
                yield Static(header, classes="title")
                yield Static(
                    "[bold]\\[x][/]",
                    classes="close-btn",
                    id="detail-close",
                )
            with VerticalScroll(id="detail-content"):
                yield Static(
                    Panel(args_str, title="Arguments", border_style="cyan"),
                    classes="detail-args",
                )
                if self._result:
                    result_panel = Panel(
                        self._result,
                        title="Result",
                        border_style="green",
                    )
                    yield Static(
                        result_panel,
                        classes="detail-result",
                    )
            yield Static("[dim]Esc or q to close[/]", classes="hint")

    @on(events.Click, "#detail-close")
    def on_close_click(self) -> None:
        """Close modal when clicking the close button."""
        self.dismiss(None)

    def action_close(self) -> None:
        """Close the detail modal."""
        self.dismiss(None)


class PauseScreen(ModalScreen[str | None]):
    """Modal screen for pause/inject during generation."""

    BINDINGS = [
        Binding("escape", "cancel", "Cancel", show=True),
    ]

    def __init__(self, partial_content: str) -> None:
        """
        Initialize pause screen.

        Args:
            partial_content: Partial response generated so far
        """
        super().__init__()
        self._partial_content = partial_content

    def compose(self) -> ComposeResult:
        """Compose the modal content."""
        # Show last 300 chars of partial content
        preview = self._partial_content[-300:]
        if len(self._partial_content) > 300:
            preview = "..." + preview

        with Vertical():
            yield Static("[yellow]Generation Paused[/]", classes="title")
            yield Static(f"[dim]{preview}[/]", classes="preview")
            yield Static("Type to add context, [bold]Enter[/] to resume, [bold]Esc[/] to cancel")
            yield Input(placeholder="Inject context...", id="inject-input")

    @on(Input.Submitted, "#inject-input")
    def on_inject_submitted(self, event: Input.Submitted) -> None:
        """Handle injection submission."""
        self.dismiss(event.value)  # Empty string means just resume

    def action_cancel(self) -> None:
        """Cancel and interrupt."""
        self.dismiss(None)


class EntropiApp(App[None]):
    """
    Main Entropi TUI application.

    Handles the entire UI lifecycle with unified input handling
    that works during streaming.
    """

    CSS_PATH = "entropic.tcss"
    ENABLE_COMMAND_PALETTE = False

    BINDINGS = [
        Binding("escape", "pause_generation", "Pause", show=False, priority=True),
        Binding("ctrl+c", "interrupt", "Interrupt", show=False, priority=True),
        Binding("ctrl+l", "clear_screen", "Clear", show=True),
        Binding("ctrl+b", "toggle_thinking_blocks", "Toggle Thinking", show=False),
        Binding("f5", "voice_mode", "Voice Mode", show=True),
    ]

    def __init__(
        self,
        config: EntropyConfig,
        version: str = "0.1.0",
        models: list[str] | None = None,
    ) -> None:
        """
        Initialize Entropi app.

        Args:
            config: Application configuration
            version: Application version
            models: Available model names
        """
        super().__init__()
        self.entropic_config = config
        self._version = version
        self._models = models or []
        self.theme_obj = get_theme(config.ui.theme)

        # State
        self._is_generating = False
        self._generation_id = 0  # Incremented each generation for stale callback detection
        self._interrupt_count = 0  # Track repeated interrupts for force exit
        self._current_message: AssistantMessage | None = None
        self._current_thinking: ThinkingBlock | None = None
        self._in_think_block = False
        self._in_tool_call_block = False  # Filter tool_call XML from display
        self._current_tier: str | None = None  # Active model tier for display
        self._auto_approve_all = False
        self._input_history: list[str] = []
        self._history_index = -1

        # Callbacks
        self._on_user_input: Callable[[str], Coroutine[Any, Any, None]] | None = None
        self._on_interrupt: Callable[[], None] | None = None
        self._on_pause: Callable[[], None] | None = None
        self._on_thinking_toggle: Callable[[], Coroutine[Any, Any, None]] | None = None

        # For tool approval synchronization
        self._tool_approval_result: ToolApproval | str | None = None
        self._tool_approval_event: asyncio.Event | None = None

        # For pause/inject synchronization
        self._pause_result: str | None = None
        self._pause_event: asyncio.Event | None = None

        # Voice mode callbacks (set by app to unload/reload models)
        self._voice_on_enter: Callable[[], Coroutine[Any, Any, None]] | None = None
        self._voice_on_exit: Callable[[], Coroutine[Any, Any, None]] | None = None

        # Queue consumer for MCP messages (runs as worker for proper app context)
        self._mcp_queue: MessageQueue | None = None
        self._queue_process_callback: (
            Callable[[QueuedMessage], Coroutine[Any, Any, None]] | None
        ) = None

    def compose(self) -> ComposeResult:
        """Compose the application layout."""
        yield Header()
        yield VerticalScroll(id="chat-log")
        yield ProcessingIndicator(id="processing")
        yield Input(placeholder="Type your message... (Ctrl+C to exit)", id="input")
        yield ContextBar(id="context-bar")
        yield StatusFooter(id="status-footer")

    def on_mount(self) -> None:
        """Handle mount event."""
        self._show_welcome()
        # Initialize footer with idle state
        footer = self.query_one("#status-footer", StatusFooter)
        footer.set_generating(False)
        self.query_one("#input", Input).focus()

        # Start queue consumer if configured (runs as worker for app context)
        if self._mcp_queue is not None and self._queue_process_callback is not None:
            self._run_queue_consumer_worker()

    def _show_welcome(self) -> None:
        """Show welcome message."""
        models_str = ", ".join(self._models) if self._models else "None loaded"
        welcome = Panel(
            f"[bold cyan]Entropi[/] v{self._version}\n"
            f"[dim]Local AI Coding Assistant[/]\n\n"
            f"Models: {models_str}\n"
            f"Type [bold]/help[/] for commands, [bold]/exit[/] to quit",
            title="Welcome",
            border_style="cyan",
        )
        chat_log = self.query_one("#chat-log", VerticalScroll)
        chat_log.mount(Static(welcome, id="welcome-panel"))

    # === Input Handling ===

    @on(Input.Submitted, "#input")
    async def on_input_submitted(self, event: Input.Submitted) -> None:
        """Handle user input submission."""
        user_input = event.value.strip()
        if not user_input:
            return

        # Ignore if already generating - user needs to wait or Esc first
        if self._is_generating:
            self.notify("Generation in progress. Press Esc to pause.", timeout=2)
            return

        # Clear input
        input_widget = self.query_one("#input", Input)
        input_widget.value = ""

        # Add to history
        self._input_history.append(user_input)
        self._history_index = -1

        # Check for exit
        if user_input.lower() in ("/exit", "/quit", "/q"):
            self.exit()
            return

        # Display user message
        self._add_user_message(user_input)

        # Run generation in background worker
        if self._on_user_input:
            self._run_generation_worker(user_input)

    @work(exclusive=True)
    async def _run_generation_worker(self, user_input: str) -> None:
        """Run generation as async background task."""
        try:
            if self._on_user_input:
                await self._on_user_input(user_input)
        except asyncio.CancelledError:
            # Worker was cancelled (e.g., by Esc or new input) - this is expected
            logger.debug("Generation worker cancelled")
            self._end_generation_ui()
        except Exception as e:
            logger.exception("Error in generation worker")
            self._add_error(f"Error: {e}")
            self._end_generation_ui()

    @work(exclusive=False, exit_on_error=False, group="queue-consumer")
    async def _run_queue_consumer_worker(self) -> None:
        """Run queue consumer as Textual worker for proper app context.

        This runs continuously, processing messages from the MCP queue.
        Running as a worker ensures tool approval modals work correctly.
        """
        assert self._mcp_queue is not None
        assert self._queue_process_callback is not None

        logger.info("Queue consumer worker started")

        try:
            while True:
                # Block until message available
                queued_msg = await self._mcp_queue.get()

                logger.info(
                    f"Processing queued message: {queued_msg.task_id} from {queued_msg.source}"
                )

                try:
                    await self._queue_process_callback(queued_msg)
                except Exception as e:
                    logger.error(f"Error processing queued message: {e}")
                    # Note: Task failure handling is done in the callback
                finally:
                    # Mark message as complete in queue
                    self._mcp_queue.mark_complete(queued_msg.id)

        except asyncio.CancelledError:
            logger.info("Queue consumer worker cancelled")
            raise

    def on_key(self, event: events.Key) -> None:
        """Handle key events for history navigation."""
        input_widget = self.query_one("#input", Input)

        if not input_widget.has_focus:
            return

        if event.key == "up" and self._input_history:
            # Navigate up in history
            if self._history_index < len(self._input_history) - 1:
                self._history_index += 1
                input_widget.value = self._input_history[-(self._history_index + 1)]
                input_widget.cursor_position = len(input_widget.value)
            event.prevent_default()

        elif event.key == "down":
            # Navigate down in history
            if self._history_index > 0:
                self._history_index -= 1
                input_widget.value = self._input_history[-(self._history_index + 1)]
                input_widget.cursor_position = len(input_widget.value)
            elif self._history_index == 0:
                self._history_index = -1
                input_widget.value = ""
            event.prevent_default()

    # === Actions ===

    def action_pause_generation(self) -> None:
        """Handle Escape key - interrupt generation."""
        # Let VoiceScreen handle its own Escape (check by method existence)
        if hasattr(self.screen, "action_exit_voice"):
            # action_exit_voice is async, so schedule it properly
            self.run_worker(self.screen.action_exit_voice())
            return

        if self._is_generating:
            self._interrupt_count += 1
            logger.info(f"User pause via Escape (count={self._interrupt_count})")

            if self._interrupt_count >= 3:
                # Hard kill - nothing else worked
                logger.warning("Hard exit via os._exit")
                os._exit(1)

            if self._interrupt_count >= 2:
                # Force exit on repeated interrupt
                self.notify("Force exit (press again for hard kill)", timeout=1)
                self._end_generation_ui()
                self.exit()
                return

            self.notify("Interrupting... (press again to exit)", timeout=2)
            if self._on_pause:
                self._on_pause()
            # End UI state so user can continue or exit
            self._end_generation_ui()

    def action_interrupt(self) -> None:
        """Handle Ctrl+C - interrupt."""
        if self._is_generating:
            self._interrupt_count += 1
            logger.info(f"User interrupt via Ctrl+C (count={self._interrupt_count})")

            if self._interrupt_count >= 3:
                # Hard kill - nothing else worked
                logger.warning("Hard exit via os._exit")
                os._exit(1)

            if self._interrupt_count >= 2:
                # Force exit on repeated interrupt
                self.notify("Force exit (press again for hard kill)", timeout=1)
                self._end_generation_ui()
                self.exit()
                return

            self.notify("Interrupting... (press again to exit)", timeout=2)
            if self._on_interrupt:
                self._on_interrupt()
            # End UI state so user can continue or exit
            self._end_generation_ui()
        else:
            # Not generating - exit
            self.exit()

    def action_clear_screen(self) -> None:
        """Clear the chat log."""
        chat_log = self.query_one("#chat-log", VerticalScroll)
        chat_log.remove_children()
        self._show_welcome()

    def action_toggle_thinking_blocks(self) -> None:
        """Toggle visibility of thinking blocks (Ctrl+B)."""
        expanded = ThinkingBlock.toggle_all_expanded()

        # Update all existing thinking blocks
        chat_log = self.query_one("#chat-log", VerticalScroll)
        for block in chat_log.query(ThinkingBlock):
            block._update_display()

        # Show feedback
        state = "expanded" if expanded else "collapsed"
        self.notify(f"Thinking blocks {state}", timeout=1.5)

    # === Callback Setters ===

    def set_input_callback(self, callback: Callable[[str], Coroutine[Any, Any, None]]) -> None:
        """Set callback for user input."""
        self._on_user_input = callback

    def set_interrupt_callback(self, callback: Callable[[], None]) -> None:
        """Set callback for interrupt handling."""
        self._on_interrupt = callback

    def set_pause_callback(self, callback: Callable[[], None]) -> None:
        """Set callback for pause handling."""
        self._on_pause = callback

    def set_thinking_toggle_callback(
        self, callback: Callable[[], Coroutine[Any, Any, None]]
    ) -> None:
        """Set callback for thinking mode toggle."""
        self._on_thinking_toggle = callback

    def set_queue_consumer(
        self,
        queue: MessageQueue,
        process_callback: Callable[[QueuedMessage], Coroutine[Any, Any, None]],
    ) -> None:
        """Set up queue consumer for MCP messages.

        The queue consumer will be started as a Textual worker in on_mount,
        ensuring it runs with proper app context for modal dialogs.

        Args:
            queue: Message queue to consume from
            process_callback: Callback to process each message
        """
        self._mcp_queue = queue
        self._queue_process_callback = process_callback

    # === Tier Display ===

    def set_tier(self, tier: str) -> None:
        """Set the active model tier, updating current and future messages."""
        self._current_tier = tier
        if self._current_message:
            self._current_message.set_tier(tier)

    def show_routing_info(self, info_text: str) -> None:
        """Display routing decision in a compact yellow panel."""
        chat_log = self.query_one("#chat-log", VerticalScroll)
        chat_log.mount(RouterInfoWidget(info_text))
        chat_log.scroll_end(animate=False)

    # === Generation State ===

    def start_generation(self) -> None:
        """Mark generation as started."""
        self._start_generation_ui()

    def _start_generation_ui(self) -> None:
        """UI updates for start_generation - must run on main thread."""
        self._generation_id += 1  # Invalidate any stale callbacks
        self._is_generating = True
        self._interrupt_count = 0  # Reset interrupt counter
        self._in_think_block = False
        self._in_tool_call_block = False
        self._current_tier = None  # Reset tier - will be set by on_tier_selected callback
        self._current_thinking = None
        self._current_message = None  # Don't create yet - wait for non-thinking content

        # Start processing animation
        processing = self.query_one("#processing", ProcessingIndicator)
        processing.start("Generating")

        # Update footer
        footer = self.query_one("#status-footer", StatusFooter)
        footer.set_generating(True)

    def end_generation(self) -> None:
        """Mark generation as ended."""
        self._end_generation_ui()

    def _end_generation_ui(self) -> None:
        """UI updates for end_generation - must run on main thread. Idempotent."""
        # Guard against multiple calls
        was_generating = self._is_generating
        self._is_generating = False

        if self._current_message:
            self._current_message.stop_streaming()
            self._current_message = None

        # Finish any open thinking block
        if self._current_thinking:
            self._current_thinking.finish()
            self._current_thinking = None
        self._in_think_block = False

        # Only update UI elements if we were actually generating
        if was_generating:
            # Stop processing animation
            try:
                processing = self.query_one("#processing", ProcessingIndicator)
                processing.stop()
            except Exception:
                pass

            # Update footer
            try:
                footer = self.query_one("#status-footer", StatusFooter)
                footer.set_generating(False)
            except Exception:
                pass

            # Scroll to end
            try:
                chat_log = self.query_one("#chat-log", VerticalScroll)
                chat_log.scroll_end(animate=False)
            except Exception:
                pass

    # === Streaming ===

    def on_stream_chunk(self, chunk: str) -> None:
        """Handle streaming chunk from engine."""
        self._on_stream_chunk_ui(chunk)

    def stream_chunk(self, chunk: str) -> None:
        """Alias for on_stream_chunk - use for MCP queue processing."""
        self._on_stream_chunk_ui(chunk)

    def finalize_stream(self) -> None:
        """Finalize the current stream (end any open message blocks)."""
        self._end_generation_ui()

    def _on_stream_chunk_ui(self, chunk: str) -> None:
        """Process streaming chunk - must run on main thread."""
        if not self._is_generating:
            return

        chat_log = self.query_one("#chat-log", VerticalScroll)
        i = 0
        while i < len(chunk):
            skip = self._process_chunk_char(chunk, i, chat_log)
            i += skip
        chat_log.scroll_end(animate=False)

    def _process_chunk_char(self, chunk: str, i: int, chat_log: VerticalScroll) -> int:
        """Process a single character position in chunk. Returns chars to skip."""
        remaining = chunk[i:]

        # Check for tag transitions
        tag_skip = self._check_tag_transition(remaining, chat_log)
        if tag_skip > 0:
            return tag_skip

        # Skip content inside tool_call blocks
        if self._in_tool_call_block:
            return 1

        # Regular character handling
        self._append_char_to_output(chunk[i], chat_log)
        return 1

    def _check_tag_transition(self, remaining: str, chat_log: VerticalScroll) -> int:
        """Check for XML tag transitions. Returns chars to skip, 0 if no tag."""
        tags = [
            ("<think>", 7, lambda: self._start_think_block(chat_log)),
            ("</think>", 8, self._end_think_block),
            ("<tool_call>", 11, lambda: setattr(self, "_in_tool_call_block", True)),
            ("</tool_call>", 12, lambda: setattr(self, "_in_tool_call_block", False)),
        ]
        for tag, skip_len, handler in tags:
            if remaining.startswith(tag):
                handler()
                return skip_len
        return 0

    def _start_think_block(self, chat_log: VerticalScroll) -> None:
        """Start a thinking block."""
        self._in_think_block = True
        if not self._current_thinking:
            self._current_thinking = ThinkingBlock()
            chat_log.mount(self._current_thinking)

    def _end_think_block(self) -> None:
        """End current thinking block."""
        self._in_think_block = False
        if self._current_thinking:
            self._current_thinking.finish()
        self._current_thinking = None

    def _append_char_to_output(self, char: str, chat_log: VerticalScroll) -> None:
        """Append character to appropriate output widget."""
        if self._in_think_block and self._current_thinking:
            self._current_thinking.append(char)
        else:
            if not self._current_message:
                # Skip leading whitespace — avoids empty Assistant panels
                # between </think> and <tool_call> tags
                if char.isspace():
                    return
                self._current_message = AssistantMessage("")
                if self._current_tier:
                    self._current_message.set_tier(self._current_tier)
                self._current_message.start_streaming()
                chat_log.mount(self._current_message)
            self._current_message.append(char)

    # === Message Display ===

    def add_message(self, role: str, content: str) -> None:
        """Add a message to the chat log.

        Args:
            role: Message role ('user', 'assistant', 'system')
            content: Message content
        """
        if role == "user":
            self._add_user_message(content)
        elif role == "system":
            self._print_info_ui(content)
        else:
            # For assistant messages, just print as info for now
            self._print_info_ui(content)

    def _add_user_message(self, content: str) -> None:
        """Add a user message to the chat log."""
        chat_log = self.query_one("#chat-log", VerticalScroll)
        chat_log.mount(UserMessage(content))
        chat_log.scroll_end(animate=False)

    def _add_error(self, message: str) -> None:
        """Add an error message."""
        chat_log = self.query_one("#chat-log", VerticalScroll)
        chat_log.mount(
            Static(
                Panel(
                    message,
                    title="[red]Error[/]",
                    border_style="red",
                ),
                classes="error-panel",
            )
        )
        chat_log.scroll_end(animate=False)

    def print_info(self, message: str) -> None:
        """Print info message."""
        self._print_info_ui(message)

    def _print_info_ui(self, message: str) -> None:
        """Print info UI - must run on main thread."""
        chat_log = self.query_one("#chat-log", VerticalScroll)
        chat_log.mount(Static(Text.from_markup(f"[cyan]{message}[/]")))
        chat_log.scroll_end(animate=False)

    def print_warning(self, message: str) -> None:
        """Print warning message."""
        self._print_warning_ui(message)

    def _print_warning_ui(self, message: str) -> None:
        """Print warning UI - must run on main thread."""
        chat_log = self.query_one("#chat-log", VerticalScroll)
        chat_log.mount(Static(Text.from_markup(f"[yellow]Warning: {message}[/]")))
        chat_log.scroll_end(animate=False)

    def print_error(self, message: str) -> None:
        """Print error message."""
        self._add_error(message)

    # === Tool Handling ===

    def print_tool_start(self, name: str, arguments: dict[str, Any]) -> None:
        """Print tool execution start."""
        self._print_tool_start_ui(name, arguments)

    def _print_tool_start_ui(self, name: str, arguments: dict[str, Any]) -> None:
        """Tool start UI - must run on main thread."""
        # Guard against stale callbacks
        if not self._is_generating:
            return

        # End current streaming message temporarily
        if self._current_message:
            self._current_message.stop_streaming()

        chat_log = self.query_one("#chat-log", VerticalScroll)
        tool_widget = ToolCallWidget(ToolCallData(name, arguments, status="running"))
        chat_log.mount(tool_widget)
        chat_log.scroll_end(animate=False)

    def print_tool_complete(self, name: str, result: str, duration_ms: float) -> None:
        """Print tool execution complete."""
        self._print_tool_complete_ui(name, result, duration_ms)

    def _print_tool_complete_ui(self, name: str, result: str, duration_ms: float) -> None:
        """Tool complete UI - must run on main thread."""
        _ = name  # Used for logging if needed
        # Don't guard this - tool completion should show even if generation stopped
        try:
            chat_log = self.query_one("#chat-log", VerticalScroll)
            tool_widgets = chat_log.query(ToolCallWidget)
            if tool_widgets:
                last_tool = list(tool_widgets)[-1]
                last_tool.set_complete(result, duration_ms)
        except Exception:
            pass

        # Force a new green box for post-tool content by clearing current message.
        # _append_char_to_output will create a fresh AssistantMessage that inherits
        # the current tier.
        if self._is_generating and self._current_message:
            self._current_message.stop_streaming()
            self._current_message = None

    @on(ToolCallWidget.ShowDetail)
    def on_tool_detail(self, event: ToolCallWidget.ShowDetail) -> None:
        """Show full tool call details when clicking a completed tool widget."""
        w = event.tool_widget
        self.push_screen(
            ToolDetailScreen(
                name=w._tool_name,
                arguments=w._arguments,
                result=w._result,
                duration_ms=w._duration_ms,
                status=w._status,
            )
        )

    async def prompt_tool_approval(
        self,
        name: str,
        arguments: dict[str, Any],
        is_sensitive: bool = False,
    ) -> ToolApproval | str:
        """
        Prompt user for tool approval.

        Args:
            name: Tool name
            arguments: Tool arguments
            is_sensitive: Whether this is a sensitive operation

        Returns:
            ToolApproval enum or feedback string
        """
        if self._auto_approve_all:
            return ToolApproval.ALLOW

        # Select appropriate modal based on tool type
        screen: ToolApprovalScreen | FileEditApprovalScreen
        if name in ("filesystem.edit_file", "filesystem.write_file"):
            screen = FileEditApprovalScreen(name, arguments, is_sensitive)
        else:
            screen = ToolApprovalScreen(name, arguments, is_sensitive)

        # Push modal screen and wait for result
        result = await self.push_screen_wait(screen)

        # Handle session-wide auto-approve
        if result == ToolApproval.ALWAYS_ALLOW:
            # Don't set auto_approve_all - save to config instead
            pass

        return result if result is not None else ToolApproval.DENY

    # === Pause/Inject ===

    async def prompt_injection(self, partial_content: str) -> str | None:
        """
        Prompt user for injection during pause.

        Args:
            partial_content: Partial response so far

        Returns:
            Injection text, empty string to resume, or None to cancel
        """
        result = await self.push_screen_wait(PauseScreen(partial_content))
        return str(result) if result is not None else None

    # === Context & Status ===

    def print_context_usage(self, used: int, max_tokens: int) -> None:
        """Update context usage display."""
        self._print_context_usage_ui(used, max_tokens)

    def _print_context_usage_ui(self, used: int, max_tokens: int) -> None:
        """Context usage UI - must run on main thread."""
        context_bar = self.query_one("#context-bar", ContextBar)
        context_bar.update_usage(used, max_tokens)

    def update_state(self, state: AgentState) -> None:
        """Update agent state display."""
        _ = state  # Currently unused
        pass

    def print_compaction_notice(self, result: CompactionResult) -> None:
        """Print compaction notification."""
        self._print_compaction_notice_ui(result)

    def _print_compaction_notice_ui(self, result: CompactionResult) -> None:
        """Compaction notice UI - must run on main thread."""
        reduction = result.old_token_count - result.new_token_count
        reduction_pct = (
            (reduction / result.old_token_count * 100) if result.old_token_count > 0 else 0
        )

        chat_log = self.query_one("#chat-log", VerticalScroll)
        chat_log.mount(
            Static(
                Panel(
                    f"Context compacted to free space\n"
                    f"  [dim]Before:[/] {result.old_token_count:,} tokens\n"
                    f"  [dim]After:[/]  {result.new_token_count:,} tokens\n"
                    f"  [dim]Freed:[/]  {reduction:,} tokens ({reduction_pct:.0f}%)\n"
                    f"  [dim]Kept:[/]   {result.preserved_messages} recent messages",
                    title="[yellow]Context Compacted[/]",
                    border_style="yellow",
                ),
                classes="compaction-notice",
            )
        )
        chat_log.scroll_end(animate=False)

    def print_todo_panel(self, todo_list: TodoList) -> None:
        """Update todo display."""
        self._print_todo_panel_ui(todo_list)

    def _print_todo_panel_ui(self, todo_list: TodoList) -> None:
        """Todo panel UI - must run on main thread."""
        try:
            todo_widget = self.query_one("#todo-widget", TodoWidget)
            todo_widget.update_todos(todo_list)
            if not todo_list.is_empty:
                todo_widget.add_class("visible")
            else:
                todo_widget.remove_class("visible")
        except Exception:
            # Todo widget might not exist in layout
            pass

    def print_status(self, status: StatusInfo) -> None:
        """Print status information."""
        footer = self.query_one("#status-footer", StatusFooter)
        footer.set_model(status.model)
        footer.set_thinking_mode(status.thinking_mode)

        if status.context_max > 0:
            context_bar = self.query_one("#context-bar", ContextBar)
            context_bar.update_usage(status.context_used, status.context_max)

    # === Utility Methods ===

    def is_sensitive_tool(self, name: str, arguments: dict[str, Any]) -> bool:
        """Check if a tool call is sensitive."""
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

    def reset_auto_approve(self) -> None:
        """Reset session auto-approve mode."""
        self._auto_approve_all = False

    def set_voice_callbacks(
        self,
        on_enter: Callable[[], Coroutine[Any, Any, None]] | None = None,
        on_exit: Callable[[], Coroutine[Any, Any, None]] | None = None,
    ) -> None:
        """
        Set callbacks for voice mode entry/exit.

        Args:
            on_enter: Called when entering voice mode (e.g., unload chat models)
            on_exit: Called when exiting voice mode (e.g., reload chat models)
        """
        self._voice_on_enter = on_enter
        self._voice_on_exit = on_exit

    def action_voice_mode(self) -> None:
        """Launch voice mode screen (F5)."""
        if not self.entropic_config.voice.enabled:
            self.notify(
                "Voice mode not enabled. Set voice.enabled: true in config.",
                severity="warning",
                timeout=3,
            )
            return

        # Import here to avoid circular imports and allow graceful fallback
        try:
            from entropic.ui.voice_screen import VoiceScreen, VoiceScreenCallbacks

            self.push_screen(
                VoiceScreen(
                    self.entropic_config.voice,
                    callbacks=VoiceScreenCallbacks(
                        on_enter=self._voice_on_enter,
                        on_exit=self._voice_on_exit,
                    ),
                )
            )
        except ImportError as e:
            self.notify(
                f"Voice mode dependencies not installed: {e}",
                severity="error",
                timeout=3,
            )
