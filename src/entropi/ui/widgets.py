"""
Textual widgets for Entropi TUI.

Provides reusable message and UI components for the Textual-based interface.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from rich.markdown import Markdown
from rich.panel import Panel
from rich.text import Text
from textual import events
from textual.message import Message as TextualMessage
from textual.widgets import Static

if TYPE_CHECKING:
    from entropi.core.todos import TodoList


@dataclass
class ToolCallData:
    """Data for a tool call display."""

    name: str
    arguments: dict[str, Any]
    status: str = "pending"
    result: str | None = None
    duration_ms: float | None = None


class RouterInfoWidget(Static):
    """Compact router decision display widget."""

    DEFAULT_CSS = """
    RouterInfoWidget {
        margin: 0 1;
        padding: 0;
    }
    """

    def __init__(self, info_text: str, **kwargs: Any) -> None:
        """
        Initialize router info widget.

        Args:
            info_text: Formatted routing info string
            **kwargs: Additional widget arguments
        """
        super().__init__(**kwargs)
        self._info_text = info_text

    def on_mount(self) -> None:
        """Render on mount."""
        self.update(
            Panel(
                Text.from_markup(self._info_text),
                title="Router",
                border_style="yellow",
                padding=(0, 1),
            )
        )


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

    TIER_STYLES: dict[str, str] = {
        "code": "cyan",
        "thinking": "magenta",
        "normal": "green",
        "simple": "dim",
    }

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
        self._tier: str | None = None

    @property
    def message_text(self) -> str:
        """Get current message text."""
        return self._content

    @message_text.setter
    def message_text(self, value: str) -> None:
        """Set message text and update display."""
        self._content = value
        self._update_display()

    def set_tier(self, tier: str) -> None:
        """Set the model tier and update display.

        Args:
            tier: Tier name (e.g., 'code', 'thinking', 'normal', 'simple')
        """
        self._tier = tier
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

    def _build_title(self) -> str:
        """Build panel title with optional tier label."""
        if self._tier:
            tier_upper = self._tier.upper()
            color = self.TIER_STYLES.get(self._tier, "dim")
            return f"[bold green]Assistant[/] [{color}]{tier_upper}[/]"
        return "[bold green]Assistant[/]"

    def _update_display(self) -> None:
        """Update the rendered display."""
        display: Text | Markdown
        if self._is_streaming:
            # Show with cursor
            display = Text(self._content + "▌")
        else:
            # Render as markdown
            display = Markdown(self._content) if self._content else Text("")

        self.update(
            Panel(
                display,
                title=self._build_title(),
                title_align="left",
                border_style="green",
                padding=(0, 1),
            )
        )

    def on_mount(self) -> None:
        """Render on mount."""
        self._update_display()


class ToolCallWidget(Static):
    """Tool call display with status indicator. Click completed calls for details."""

    class ShowDetail(TextualMessage):
        """Posted when user clicks a completed/errored tool call."""

        def __init__(self, widget: ToolCallWidget) -> None:
            super().__init__()
            self.tool_widget = widget

    DEFAULT_CSS = """
    ToolCallWidget {
        margin: 0 1;
        padding: 0 1;
    }
    """

    def __init__(self, data: ToolCallData, **kwargs: Any) -> None:
        """
        Initialize tool call widget.

        Args:
            data: Tool call data
            **kwargs: Additional widget arguments
        """
        super().__init__(**kwargs)
        self._tool_name = data.name
        self._arguments = data.arguments
        self._status = data.status
        self._result = data.result
        self._duration_ms = data.duration_ms

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
        tool_base = self._tool_name.split(".")[-1]

        # Format based on common tool patterns
        formatters = [
            ("read_file", lambda: f"Reading: {self._arguments.get('path', '')}"),
            ("write_file", lambda: f"Writing: {self._arguments.get('path', '')}"),
            ("edit_file", lambda: f"Editing: {self._arguments.get('path', '')}"),
            ("list_directory", lambda: f"Listing: {self._arguments.get('path', '.')}"),
            ("search_files", lambda: f"Searching: {self._arguments.get('pattern', '')}"),
            ("execute", lambda: f"Running: {self._arguments.get('command', '')[:60]}"),
            ("git", lambda: f"Git: {tool_base}"),
        ]
        for pattern, formatter in formatters:
            if pattern in self._tool_name:
                return formatter()

        # Default: show tool base with args preview
        args_preview = ", ".join(f"{v}"[:20] for v in self._arguments.values())[:40]
        return f"{tool_base}: {args_preview}"

    def _format_result_preview(self) -> str:
        """Format tool result for display — show meaningful content."""
        clean = self._result.replace(chr(10), " ").strip() if self._result else ""
        if not clean:
            return ""
        max_len = 120
        truncated = clean if len(clean) <= max_len else f"{clean[:max_len]}..."
        return f" - {truncated}"

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
            result_preview = self._format_result_preview()
            text = f"{icon} {info}{duration}{result_preview}"
        else:
            text = f"{icon} {info}"

        tool_base = self._tool_name.split(".")[-1]
        self.update(
            Panel(
                Text.from_markup(text),
                title=f"[bold green]Tool[/] [dim]{tool_base}[/]",
                title_align="left",
                border_style="green",
                padding=(0, 1),
            )
        )

    def on_click(self, event: events.Click) -> None:
        """Open detail modal when clicking a completed/errored tool call."""
        if self._status in ("complete", "error"):
            self.post_message(self.ShowDetail(self))

    def on_mount(self) -> None:
        """Render on mount."""
        self._update_display()


class ThinkingBlock(Static):
    """
    Collapsible thinking content display.

    Shows thinking content in a dimmed format while streaming,
    then collapses to 2 lines when complete. Respects Ctrl+B toggle
    during streaming with rate-controlled updates.
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

    # Streaming rate control
    _EXPANDED_UPDATE_INTERVAL = 0.05  # 50ms - slower for readability
    _COLLAPSED_UPDATE_INTERVAL = 0.2  # 200ms - batch updates when collapsed

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
        self._pending_content: str = ""  # Buffer for batching
        self._last_update_time: float = 0.0

    @property
    def thinking_content(self) -> str:
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

    def append_content(self, chunk: str) -> None:
        """Append content with rate limiting based on toggle state."""
        import time

        self._content += chunk
        self._pending_content += chunk

        now = time.time()
        interval = (
            self._EXPANDED_UPDATE_INTERVAL
            if ThinkingBlock._all_expanded
            else self._COLLAPSED_UPDATE_INTERVAL
        )

        # Update display if enough time has passed
        if now - self._last_update_time >= interval:
            self._update_display()
            self._pending_content = ""
            self._last_update_time = now

    def append(self, chunk: str) -> None:
        """Append thinking content during streaming (alias for append_content)."""
        self.append_content(chunk)

    def finish_streaming(self) -> None:
        """Mark streaming complete and flush any pending content."""
        self._is_streaming = False
        self._update_display()  # Final update with all content

    def finish(self) -> None:
        """Called when thinking block is complete - collapse it."""
        self.finish_streaming()
        if not ThinkingBlock._all_expanded:
            self.add_class("collapsed")

    def _update_display(self) -> None:
        """Update display - respects toggle even during streaming."""
        cursor = "▌" if self._is_streaming else ""

        if ThinkingBlock._all_expanded:
            self.remove_class("collapsed")
            display = self._content + cursor
        else:
            self.add_class("collapsed")
            lines = self._content.split("\n")
            if len(lines) > 2:
                # Show last 2 lines as rolling window during streaming
                display = "..." + "\n".join(lines[-2:]) + cursor
            else:
                display = self._content + cursor

        # Build Text without markup parsing to avoid issues with brackets in content
        text = Text(f"💭 {display}")
        text.stylize("dim italic")
        self.update(text)

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
    """Status footer with key hints, model info, and GPU stats."""

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
        self._gpu_stats: dict[str, Any] = {}
        self._gpu_timer: Any = None

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

    def _get_gpu_stats(self) -> dict[str, Any]:
        """Fetch GPU stats from nvidia-smi."""
        import subprocess

        try:
            result = subprocess.run(
                [
                    "nvidia-smi",
                    "--query-gpu=memory.used,memory.total,temperature.gpu,utilization.gpu",
                    "--format=csv,noheader,nounits",
                ],
                capture_output=True,
                text=True,
                timeout=2,
            )
            if result.returncode == 0:
                parts = result.stdout.strip().split(", ")
                if len(parts) >= 4:
                    return {
                        "vram_used": int(parts[0]),
                        "vram_total": int(parts[1]),
                        "temp": int(parts[2]),
                        "util": int(parts[3]),
                    }
        except (FileNotFoundError, subprocess.TimeoutExpired, ValueError):
            pass
        return {}

    def _refresh_gpu_stats(self) -> None:
        """Refresh GPU stats periodically."""
        self._gpu_stats = self._get_gpu_stats()
        self._update_display()

    def _update_display(self) -> None:
        """Update the rendered display."""
        parts = []

        # GPU stats (if available)
        if self._gpu_stats:
            vram_used = self._gpu_stats.get("vram_used", 0)
            vram_total = self._gpu_stats.get("vram_total", 1)
            temp = self._gpu_stats.get("temp", 0)
            util = self._gpu_stats.get("util", 0)

            vram_gb = f"{vram_used / 1024:.1f}/{vram_total / 1024:.0f}GB"
            temp_color = "red" if temp > 80 else "yellow" if temp > 70 else "green"
            parts.append(f"[dim]VRAM:[/]{vram_gb}")
            parts.append(f"[{temp_color}]{temp}°C[/]")
            parts.append(f"[dim]GPU:[/]{util}%")

        if self._model:
            parts.append(f"[bold]{self._model}[/]")

        if self._thinking_mode:
            parts.append("[magenta]Think[/]")

        if self._is_generating:
            parts.append("[cyan bold]Esc[/]=Pause")
        else:
            parts.append("[bold]Ctrl+C[/]=Exit")

        # Always show these shortcuts
        parts.append("[dim]Ctrl+B[/]=Think")
        parts.append("[dim]Ctrl+L[/]=Clear")
        parts.append("[dim]F5[/]=Voice")

        display_text = " | ".join(parts)
        self.update(Text.from_markup(display_text))

    def on_mount(self) -> None:
        """Render on mount and start GPU refresh timer."""
        self._refresh_gpu_stats()
        # Refresh GPU stats every 5 seconds
        self._gpu_timer = self.set_interval(5.0, self._refresh_gpu_stats)

    def on_unmount(self) -> None:
        """Stop GPU refresh timer."""
        if self._gpu_timer:
            self._gpu_timer.stop()
