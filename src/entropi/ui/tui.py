"""
Textual-based TUI for Entropi.

Provides unified input handling that works during streaming,
replacing the Rich + prompt_toolkit approach.
"""

from __future__ import annotations

import asyncio
from collections.abc import Callable, Coroutine
from typing import TYPE_CHECKING, Any

from rich.panel import Panel
from rich.text import Text
from textual import events, on, work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import Header, Input, Static

from entropi.config.schema import EntropyConfig
from entropi.core.compaction import CompactionResult
from entropi.core.engine import AgentState, ToolApproval
from entropi.core.logging import get_logger
from entropi.ui.themes import get_theme
from entropi.ui.widgets import (
    AssistantMessage,
    ContextBar,
    ProcessingIndicator,
    StatusFooter,
    ThinkingBlock,
    TodoWidget,
    ToolCallWidget,
    UserMessage,
)

if TYPE_CHECKING:
    from entropi.core.todos import TodoList

logger = get_logger("ui.tui")


class ToolApprovalScreen(ModalScreen[ToolApproval | str]):
    """Modal screen for tool approval."""

    BINDINGS = [
        Binding("y", "approve", "Yes", show=True),
        Binding("a", "always_allow", "Always", show=True),
        Binding("n", "deny", "No", show=True),
        Binding("d", "always_deny", "Deny Always", show=True),
        Binding("escape", "deny", "Cancel", show=False),
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

    def compose(self) -> ComposeResult:
        """Compose the modal content."""
        color = "yellow" if self._is_sensitive else "cyan"
        icon = "!" if self._is_sensitive else "?"

        # Format arguments
        args_lines = []
        for key, value in self._arguments.items():
            value_str = repr(value)
            if len(value_str) > 100:
                value_str = value_str[:50] + " ... " + value_str[-30:]
            args_lines.append(f"  [dim]{key}:[/] {value_str}")
        args_text = "\n".join(args_lines) if args_lines else "  (no arguments)"

        with Vertical():
            yield Static(
                f"[{color}][{icon}] Tool Approval Required[/]",
                classes="title",
            )
            yield Static(f"[bold]{self._tool_name}[/]", classes="tool-name")
            yield Static(args_text, classes="args")
            yield Static(
                "[bold]y[/]=yes once  [bold]a[/]=always allow  "
                "[bold]n[/]=no  [bold]d[/]=always deny\n"
                "Or type feedback and press Enter",
                classes="options",
            )
            yield Input(placeholder="Feedback (optional)...", id="feedback-input")

    @on(Input.Submitted, "#feedback-input")
    def on_feedback_submitted(self, event: Input.Submitted) -> None:
        """Handle feedback submission."""
        if event.value.strip():
            self.dismiss(event.value)
        else:
            self.dismiss(ToolApproval.DENY)

    def action_approve(self) -> None:
        """Approve once."""
        self.dismiss(ToolApproval.ALLOW)

    def action_always_allow(self) -> None:
        """Always allow."""
        self.dismiss(ToolApproval.ALWAYS_ALLOW)

    def action_deny(self) -> None:
        """Deny once."""
        self.dismiss(ToolApproval.DENY)

    def action_always_deny(self) -> None:
        """Always deny."""
        self.dismiss(ToolApproval.ALWAYS_DENY)


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

    CSS_PATH = "entropi.tcss"
    ENABLE_COMMAND_PALETTE = False

    BINDINGS = [
        Binding("escape", "pause_generation", "Pause", show=False, priority=True),
        Binding("ctrl+c", "interrupt", "Interrupt", show=False, priority=True),
        Binding("ctrl+l", "clear_screen", "Clear", show=True),
        Binding("ctrl+b", "toggle_thinking_blocks", "Toggle Thinking", show=False),
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
        self.entropi_config = config
        self._version = version
        self._models = models or []
        self.theme_obj = get_theme(config.ui.theme)

        # State
        self._is_generating = False
        self._generation_id = 0  # Incremented each generation for stale callback detection
        self._current_message: AssistantMessage | None = None
        self._current_thinking: ThinkingBlock | None = None
        self._in_think_block = False
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

    @work(exclusive=True, thread=True)
    async def _run_generation_worker(self, user_input: str) -> None:
        """Run generation in background thread."""
        try:
            if self._on_user_input:
                await self._on_user_input(user_input)
        except asyncio.CancelledError:
            # Worker was cancelled (e.g., by Esc or new input) - this is expected
            logger.debug("Generation worker cancelled")
            self.call_from_thread(self._end_generation_ui)
        except Exception as e:
            logger.exception("Error in generation worker")
            self.call_from_thread(self._add_error, f"Error: {e}")
            self.call_from_thread(self._end_generation_ui)

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
        if self._is_generating:
            logger.debug("Pause requested via Escape")
            # Show feedback but DON'T end generation UI - let the worker finish
            # The worker's finally block will call end_generation()
            self.notify("Interrupting...", timeout=2)
            if self._on_pause:
                self._on_pause()
            # Don't call _end_generation_ui() - worker handles that

    def action_interrupt(self) -> None:
        """Handle Ctrl+C - interrupt."""
        if self._is_generating:
            logger.debug("Interrupt requested via Ctrl+C")
            # Show feedback but DON'T end generation UI - let the worker finish
            self.notify("Interrupting...", timeout=2)
            if self._on_interrupt:
                self._on_interrupt()
            # Don't call _end_generation_ui() - worker handles that
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

    # === Generation State ===

    def start_generation(self) -> None:
        """Mark generation as started. Thread-safe."""
        self.call_from_thread(self._start_generation_ui)

    def _start_generation_ui(self) -> None:
        """UI updates for start_generation - must run on main thread."""
        self._generation_id += 1  # Invalidate any stale callbacks
        self._is_generating = True
        self._in_think_block = False
        self._current_thinking = None
        self._current_message = None  # Don't create yet - wait for non-thinking content

        # Start processing animation
        processing = self.query_one("#processing", ProcessingIndicator)
        processing.start("Generating")

        # Update footer
        footer = self.query_one("#status-footer", StatusFooter)
        footer.set_generating(True)

    def end_generation(self) -> None:
        """Mark generation as ended. Thread-safe."""
        self.call_from_thread(self._end_generation_ui)

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
        """Handle streaming chunk from engine. Thread-safe."""
        self.call_from_thread(self._on_stream_chunk_ui, chunk)

    def _on_stream_chunk_ui(self, chunk: str) -> None:
        """Process streaming chunk - must run on main thread."""
        # Guard against stale callbacks after generation stopped
        if not self._is_generating:
            return

        chat_log = self.query_one("#chat-log", VerticalScroll)

        # Process chunk for think blocks
        i = 0
        while i < len(chunk):
            remaining = chunk[i:]

            # Check for <think> start tag
            if remaining.startswith("<think>"):
                self._in_think_block = True
                # Create thinking block widget if needed
                if not self._current_thinking:
                    self._current_thinking = ThinkingBlock()
                    chat_log.mount(self._current_thinking)
                i += 7
                continue

            # Check for </think> end tag
            if remaining.startswith("</think>"):
                self._in_think_block = False
                if self._current_thinking:
                    self._current_thinking.finish()
                self._current_thinking = None
                i += 8
                continue

            # Regular character
            if self._in_think_block and self._current_thinking:
                self._current_thinking.append(chunk[i])
            else:
                # Create assistant message on first non-thinking content
                if not self._current_message:
                    self._current_message = AssistantMessage("")
                    self._current_message.start_streaming()
                    chat_log.mount(self._current_message)
                self._current_message.append(chunk[i])
            i += 1

        # Keep scrolled to bottom
        chat_log.scroll_end(animate=False)

    # === Message Display ===

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
        """Print info message. Thread-safe."""
        self.call_from_thread(self._print_info_ui, message)

    def _print_info_ui(self, message: str) -> None:
        """Print info UI - must run on main thread."""
        chat_log = self.query_one("#chat-log", VerticalScroll)
        chat_log.mount(Static(Text.from_markup(f"[cyan]{message}[/]")))
        chat_log.scroll_end(animate=False)

    def print_warning(self, message: str) -> None:
        """Print warning message. Thread-safe."""
        self.call_from_thread(self._print_warning_ui, message)

    def _print_warning_ui(self, message: str) -> None:
        """Print warning UI - must run on main thread."""
        chat_log = self.query_one("#chat-log", VerticalScroll)
        chat_log.mount(Static(Text.from_markup(f"[yellow]Warning: {message}[/]")))
        chat_log.scroll_end(animate=False)

    def print_error(self, message: str) -> None:
        """Print error message. Thread-safe."""
        self.call_from_thread(self._add_error, message)

    # === Tool Handling ===

    def print_tool_start(self, name: str, arguments: dict[str, Any]) -> None:
        """Print tool execution start. Thread-safe."""
        self.call_from_thread(self._print_tool_start_ui, name, arguments)

    def _print_tool_start_ui(self, name: str, arguments: dict[str, Any]) -> None:
        """Tool start UI - must run on main thread."""
        # Guard against stale callbacks
        if not self._is_generating:
            return

        # End current streaming message temporarily
        if self._current_message:
            self._current_message.stop_streaming()

        chat_log = self.query_one("#chat-log", VerticalScroll)
        tool_widget = ToolCallWidget(name, arguments, status="running")
        chat_log.mount(tool_widget)
        chat_log.scroll_end(animate=False)

    def print_tool_complete(self, name: str, result: str, duration_ms: float) -> None:
        """Print tool execution complete. Thread-safe."""
        self.call_from_thread(self._print_tool_complete_ui, name, result, duration_ms)

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

        # Resume streaming on current message if still generating
        if self._is_generating and self._current_message:
            self._current_message.start_streaming()

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

        # Push modal screen and wait for result
        result = await self.push_screen_wait(ToolApprovalScreen(name, arguments, is_sensitive))

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
        return result

    # === Context & Status ===

    def print_context_usage(self, used: int, max_tokens: int) -> None:
        """Update context usage display. Thread-safe."""
        self.call_from_thread(self._print_context_usage_ui, used, max_tokens)

    def _print_context_usage_ui(self, used: int, max_tokens: int) -> None:
        """Context usage UI - must run on main thread."""
        context_bar = self.query_one("#context-bar", ContextBar)
        context_bar.update_usage(used, max_tokens)

    def update_state(self, state: AgentState) -> None:
        """Update agent state display."""
        _ = state  # Currently unused
        pass

    def print_compaction_notice(self, result: CompactionResult) -> None:
        """Print compaction notification. Thread-safe."""
        self.call_from_thread(self._print_compaction_notice_ui, result)

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
        """Update todo display. Thread-safe."""
        self.call_from_thread(self._print_todo_panel_ui, todo_list)

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

    def print_status(
        self,
        model: str,
        vram_used: float,  # noqa: ARG002 - reserved for future VRAM display
        vram_total: float,  # noqa: ARG002 - reserved for future VRAM display
        tokens: int,  # noqa: ARG002 - reserved for future token display
        thinking_mode: bool = False,
        context_used: int = 0,
        context_max: int = 0,
    ) -> None:
        """Print status information."""
        _ = (vram_used, vram_total, tokens)  # Reserved for future use
        footer = self.query_one("#status-footer", StatusFooter)
        footer.set_model(model)
        footer.set_thinking_mode(thinking_mode)

        if context_max > 0:
            context_bar = self.query_one("#context-bar", ContextBar)
            context_bar.update_usage(context_used, context_max)

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
