"""
Textual widgets for Entropi TUI.

Provides reusable message and UI components for the Textual-based interface.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from rich.markdown import Markdown
from rich.panel import Panel
from rich.text import Text
from textual.widgets import Static

if TYPE_CHECKING:
    from entropi.core.todos import TodoList


class UserMessage(Static):
    """User message display widget."""

    DEFAULT_CSS = """
    UserMessage {
        margin: 0 1;
        padding: 0 1;
    }
    """

    def __init__(self, content: str, **kwargs: Any) -> None:
        """
        Initialize user message.

        Args:
            content: Message content
            **kwargs: Additional widget arguments
        """
        super().__init__(**kwargs)
        self._content = content

    def compose_content(self) -> Panel:
        """Compose the message panel."""
        return Panel(
            self._content,
            title="[bold blue]You[/]",
            title_align="left",
            border_style="blue",
            padding=(0, 1),
        )

    def on_mount(self) -> None:
        """Render on mount."""
        self.update(self.compose_content())


class AssistantMessage(Static):
    """
    Assistant message widget with streaming support.

    Supports real-time updates during generation.
    """

    DEFAULT_CSS = """
    AssistantMessage {
        margin: 0 1;
        padding: 0 1;
    }
    """

    def __init__(self, content: str = "", **kwargs: Any) -> None:
        """
        Initialize assistant message.

        Args:
            content: Initial content (can be empty for streaming)
            **kwargs: Additional widget arguments
        """
        super().__init__(**kwargs)
        self._content = content
        self._is_streaming = False

    @property
    def content(self) -> str:
        """Get current content."""
        return self._content

    @content.setter
    def content(self, value: str) -> None:
        """Set content and update display."""
        self._content = value
        self._update_display()

    def start_streaming(self) -> None:
        """Mark as streaming (shows cursor)."""
        self._is_streaming = True
        self._update_display()

    def stop_streaming(self) -> None:
        """Stop streaming mode."""
        self._is_streaming = False
        self._update_display()

    def append(self, chunk: str) -> None:
        """
        Append chunk during streaming.

        Args:
            chunk: Text chunk to append
        """
        self._content += chunk
        self._update_display()

    def _update_display(self) -> None:
        """Update the rendered display."""
        if self._is_streaming:
            # Show with cursor
            display = Text(self._content + "▌")
        else:
            # Render as markdown
            display = Markdown(self._content) if self._content else Text("")

        self.update(
            Panel(
                display,
                title="[bold green]Assistant[/]",
                title_align="left",
                border_style="green",
                padding=(0, 1),
            )
        )

    def on_mount(self) -> None:
        """Render on mount."""
        self._update_display()


class ToolCallWidget(Static):
    """Tool call display with status indicator."""

    DEFAULT_CSS = """
    ToolCallWidget {
        margin: 0 1;
        padding: 0;
    }
    """

    def __init__(
        self,
        name: str,
        arguments: dict[str, Any],
        status: str = "pending",
        result: str | None = None,
        duration_ms: float | None = None,
        **kwargs: Any,
    ) -> None:
        """
        Initialize tool call widget.

        Args:
            name: Tool name
            arguments: Tool arguments
            status: Status (pending, running, complete, error)
            result: Tool result (if complete)
            duration_ms: Execution duration in milliseconds
            **kwargs: Additional widget arguments
        """
        super().__init__(**kwargs)
        self._name = name
        self._arguments = arguments
        self._status = status
        self._result = result
        self._duration_ms = duration_ms

    def set_running(self) -> None:
        """Mark as running."""
        self._status = "running"
        self._update_display()

    def set_complete(self, result: str, duration_ms: float) -> None:
        """Mark as complete with result."""
        self._status = "complete"
        self._result = result
        self._duration_ms = duration_ms
        self._update_display()

    def set_error(self, error: str, duration_ms: float) -> None:
        """Mark as error."""
        self._status = "error"
        self._result = error
        self._duration_ms = duration_ms
        self._update_display()

    def _format_tool_info(self) -> str:
        """Format tool name and arguments for display."""
        tool_base = self._name.split(".")[-1]

        # Format based on common tool patterns
        if "read_file" in self._name:
            path = self._arguments.get("path", "")
            return f"Reading: {path}"
        elif "write_file" in self._name:
            path = self._arguments.get("path", "")
            return f"Writing: {path}"
        elif "edit_file" in self._name:
            path = self._arguments.get("path", "")
            return f"Editing: {path}"
        elif "list_directory" in self._name:
            path = self._arguments.get("path", ".")
            return f"Listing: {path}"
        elif "search_files" in self._name:
            pattern = self._arguments.get("pattern", "")
            return f"Searching: {pattern}"
        elif "execute" in self._name:
            cmd = self._arguments.get("command", "")[:60]
            return f"Running: {cmd}"
        elif "git" in self._name:
            return f"Git: {tool_base}"
        else:
            args_preview = ", ".join(f"{v}"[:20] for v in self._arguments.values())[:40]
            return f"{tool_base}: {args_preview}"

    def _update_display(self) -> None:
        """Update the rendered display."""
        status_icons = {
            "pending": "[dim]○[/]",
            "running": "[cyan]●[/]",
            "complete": "[green]✓[/]",
            "error": "[red]✗[/]",
        }
        icon = status_icons.get(self._status, "?")

        info = self._format_tool_info()

        if self._status in ("complete", "error"):
            duration = f" ({self._duration_ms:.0f}ms)" if self._duration_ms else ""
            result_preview = ""
            if self._result:
                if len(self._result) > 60:
                    result_preview = f" - {len(self._result)} chars"
                else:
                    result_preview = f" - {self._result[:40].replace(chr(10), ' ')}"
            text = f"{icon} {info}{duration}{result_preview}"
        else:
            text = f"{icon} {info}"

        self.update(Text.from_markup(text))

    def on_mount(self) -> None:
        """Render on mount."""
        self._update_display()


class ThinkingBlock(Static):
    """
    Collapsible thinking content display.

    Shows thinking content in a dimmed format while streaming,
    then collapses to 2 lines when complete.
    """

    DEFAULT_CSS = """
    ThinkingBlock {
        margin: 0 1;
        padding: 0;
        height: auto;
    }

    ThinkingBlock.collapsed {
        max-height: 2;
        overflow: hidden;
    }
    """

    # Class-level toggle for all thinking blocks
    _all_expanded: bool = False

    def __init__(self, content: str = "", **kwargs: Any) -> None:
        """
        Initialize thinking block.

        Args:
            content: Thinking content
            **kwargs: Additional widget arguments
        """
        super().__init__(**kwargs)
        self._content = content
        self._is_streaming = True  # Start in streaming mode

    @property
    def content(self) -> str:
        """Get full thinking content."""
        return self._content

    @classmethod
    def toggle_all_expanded(cls) -> bool:
        """Toggle expanded state for all thinking blocks. Returns new state."""
        cls._all_expanded = not cls._all_expanded
        return cls._all_expanded

    @classmethod
    def set_all_expanded(cls, expanded: bool) -> None:
        """Set expanded state for all thinking blocks."""
        cls._all_expanded = expanded

    def append(self, chunk: str) -> None:
        """Append thinking content during streaming."""
        self._content += chunk
        self._update_display()

    def finish(self) -> None:
        """Called when thinking block is complete - collapse it."""
        self._is_streaming = False
        if not ThinkingBlock._all_expanded:
            self.add_class("collapsed")
        self._update_display()

    def _update_display(self) -> None:
        """Update display."""
        # While streaming or expanded, show full content
        if self._is_streaming or ThinkingBlock._all_expanded:
            self.remove_class("collapsed")
            display = self._content + ("▌" if self._is_streaming else "")
        else:
            # Collapsed: show first 2 lines with hint
            self.add_class("collapsed")
            lines = self._content.split("\n")
            if len(lines) > 2:
                display = "\n".join(lines[:2]) + "..."
            else:
                display = self._content

        self.update(Text.from_markup(f"[dim italic]💭 {display}[/]"))

    def on_mount(self) -> None:
        """Render on mount."""
        self._update_display()


class TodoWidget(Static):
    """Todo list display widget."""

    DEFAULT_CSS = """
    TodoWidget {
        margin: 0 1;
        padding: 0;
    }
    """

    STATUS_SYMBOLS = {
        "pending": "○",
        "in_progress": "●",
        "completed": "✓",
    }

    STATUS_COLORS = {
        "pending": "dim",
        "in_progress": "cyan bold",
        "completed": "green",
    }

    def __init__(self, todo_list: TodoList | None = None, **kwargs: Any) -> None:
        """
        Initialize todo widget.

        Args:
            todo_list: Initial todo list
            **kwargs: Additional widget arguments
        """
        super().__init__(**kwargs)
        self._todo_list = todo_list

    def update_todos(self, todo_list: TodoList) -> None:
        """Update the todo list."""
        self._todo_list = todo_list
        self._update_display()

    def _update_display(self) -> None:
        """Update the rendered display."""
        if not self._todo_list or self._todo_list.is_empty:
            self.update("")
            return

        lines = []
        for item in self._todo_list.items:
            status_value = item.status.value
            symbol = self.STATUS_SYMBOLS.get(status_value, "?")
            color = self.STATUS_COLORS.get(status_value, "white")

            text = item.active_form if status_value == "in_progress" else item.content
            lines.append(f"[{color}]{symbol} {text}[/]")

        completed, total = self._todo_list.progress
        title = f"Todo ({completed}/{total})"

        self.update(
            Panel(
                "\n".join(lines),
                title=f"[bold]{title}[/]",
                border_style="blue",
                padding=(0, 1),
            )
        )

    def on_mount(self) -> None:
        """Render on mount."""
        self._update_display()


class ContextBar(Static):
    """Context usage bar widget."""

    DEFAULT_CSS = """
    ContextBar {
        height: 1;
        padding: 0 1;
    }
    """

    def __init__(self, used: int = 0, max_tokens: int = 16384, **kwargs: Any) -> None:
        """
        Initialize context bar.

        Args:
            used: Tokens used
            max_tokens: Maximum tokens
            **kwargs: Additional widget arguments
        """
        super().__init__(**kwargs)
        self._used = used
        self._max = max_tokens

    def update_usage(self, used: int, max_tokens: int | None = None) -> None:
        """Update context usage."""
        self._used = used
        if max_tokens is not None:
            self._max = max_tokens
        self._update_display()

    def _update_display(self) -> None:
        """Update the rendered display."""
        if self._max <= 0:
            self.update("")
            return

        percent = (self._used / self._max) * 100
        bar_width = 15
        filled = int((percent / 100) * bar_width)
        empty = bar_width - filled

        if percent > 90:
            color = "red"
        elif percent > 75:
            color = "yellow"
        else:
            color = "dim"

        bar = "█" * filled + "░" * empty
        self.update(
            Text.from_markup(
                f"[{color}]Context: [{bar}] {self._used:,}/{self._max:,} ({percent:.0f}%)[/]"
            )
        )

    def on_mount(self) -> None:
        """Render on mount."""
        self._update_display()


class SpinnerFrames:
    """
    Animation frame definitions.

    Swap out FRAMES to change the animation style.
    """

    # Braille spinner - smooth rotating dots
    BRAILLE = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]

    # Rotating segments
    SEGMENTS = ["◐", "◓", "◑", "◒"]

    # Pulsing dots
    DOTS = ["·", "··", "···", "····", "···", "··"]

    # Simple S with dots
    ENTROPY = ["S", "S·", "S··", "S···", "S··", "S·"]


class ProcessingIndicator(Static):
    """
    Animated processing indicator.

    Shows a subtle animation while the model is generating.
    Customize by changing FRAMES to any SpinnerFrames constant.
    """

    DEFAULT_CSS = """
    ProcessingIndicator {
        height: 1;
        padding: 0 1;
    }
    """

    # Active animation frames - change this to swap animation style
    FRAMES = SpinnerFrames.BRAILLE

    # Animation speed in seconds
    INTERVAL = 0.08

    def __init__(self, **kwargs: Any) -> None:
        """Initialize processing indicator."""
        super().__init__(**kwargs)
        self._frame_index = 0
        self._timer: Any = None
        self._message = "Processing"

    def start(self, message: str = "Processing") -> None:
        """Start the animation with optional message."""
        self._message = message
        self._frame_index = 0
        self._update_display()
        self.display = True
        if self._timer is None:
            self._timer = self.set_interval(self.INTERVAL, self._advance_frame)

    def stop(self) -> None:
        """Stop the animation and hide."""
        if self._timer is not None:
            self._timer.stop()
            self._timer = None
        self.display = False

    def _advance_frame(self) -> None:
        """Advance to next animation frame."""
        self._frame_index = (self._frame_index + 1) % len(self.FRAMES)
        self._update_display()

    def _update_display(self) -> None:
        """Render current frame with message."""
        frame = self.FRAMES[self._frame_index]
        self.update(Text.from_markup(f"[cyan]{frame}[/] [dim]{self._message}[/]"))

    def on_mount(self) -> None:
        """Hide by default until started."""
        self.display = False


class StatusFooter(Static):
    """Status footer with key hints and model info."""

    DEFAULT_CSS = """
    StatusFooter {
        height: 1;
        padding: 0 1;
    }
    """

    def __init__(
        self,
        model: str = "",
        thinking_mode: bool = False,
        **kwargs: Any,
    ) -> None:
        """
        Initialize status footer.

        Args:
            model: Current model name
            thinking_mode: Whether thinking mode is active
            **kwargs: Additional widget arguments
        """
        super().__init__(**kwargs)
        self._model = model
        self._thinking_mode = thinking_mode
        self._is_generating = False

    def set_generating(self, generating: bool) -> None:
        """Set generating state."""
        self._is_generating = generating
        self._update_display()

    def set_model(self, model: str) -> None:
        """Set current model."""
        self._model = model
        self._update_display()

    def set_thinking_mode(self, enabled: bool) -> None:
        """Set thinking mode."""
        self._thinking_mode = enabled
        self._update_display()

    def _update_display(self) -> None:
        """Update the rendered display."""
        parts = []

        if self._model:
            parts.append(f"[bold]{self._model}[/]")

        if self._thinking_mode:
            parts.append("[magenta]Think[/]")

        if self._is_generating:
            parts.append("[cyan bold]Esc[/]=Pause")
            parts.append("[dim]Ctrl+B[/]=Expand")
        else:
            parts.append("[bold]Ctrl+C[/]=Exit")

        parts.append("[dim]Ctrl+L[/]=Clear")

        self.update(Text.from_markup(" | ".join(parts)))

    def on_mount(self) -> None:
        """Render on mount."""
        self._update_display()
