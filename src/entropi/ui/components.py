"""
Reusable UI components.

Provides building blocks for the terminal interface.
"""

from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from rich.console import RenderableType
from rich.markdown import Markdown
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

from entropi.ui.themes import Theme

if TYPE_CHECKING:
    from entropi.core.todos import TodoList


@dataclass
class StatusBar:
    """Status bar component."""

    model: str
    vram_used: float
    vram_total: float
    tokens: int
    theme: Theme
    thinking_mode: bool = False
    context_used: int = 0
    context_max: int = 16384

    def render(self) -> RenderableType:
        """Render status bar."""
        vram_percent = (self.vram_used / self.vram_total * 100) if self.vram_total > 0 else 0

        # Choose color based on VRAM usage
        if vram_percent > 90:
            vram_color = self.theme.error_color
        elif vram_percent > 75:
            vram_color = self.theme.warning_color
        else:
            vram_color = self.theme.success_color

        # Context usage color
        context_percent = (
            (self.context_used / self.context_max * 100) if self.context_max > 0 else 0
        )
        if context_percent > 90:
            context_color = self.theme.error_color
        elif context_percent > 75:
            context_color = self.theme.warning_color
        else:
            context_color = self.theme.success_color

        # Thinking mode indicator
        if self.thinking_mode:
            mode_indicator = "[bold magenta]Thinking[/]"
        else:
            mode_indicator = "[bold yellow]Fast[/]"

        # Context bar visual
        context_bar = self._render_context_bar(context_percent, context_color)

        table = Table.grid(padding=1)
        table.add_column(justify="left")
        table.add_column(justify="center")
        table.add_column(justify="center")
        table.add_column(justify="right")

        table.add_row(
            f"[bold]Model:[/] {self.model}",
            f"[bold]Mode:[/] {mode_indicator}",
            f"[{vram_color}]VRAM: {self.vram_used:.1f}/{self.vram_total:.1f} GB[/]",
            f"[{context_color}]Context: {context_bar} {context_percent:.0f}%[/]",
        )

        return Panel(
            table,
            border_style=self.theme.border_color,
            padding=(0, 1),
        )

    def _render_context_bar(self, percent: float, color: str) -> str:
        """Render a mini progress bar for context usage."""
        bar_width = 10
        filled = int((percent / 100) * bar_width)
        empty = bar_width - filled
        return "█" * filled + "░" * empty


@dataclass
class ToolCallDisplay:
    """Tool call display component."""

    name: str
    arguments: dict[str, Any]
    theme: Theme

    def render(self) -> RenderableType:
        """Render tool call."""
        args_text = "\n".join(f"  {k}: {v}" for k, v in self.arguments.items())

        content = f"[bold]{self.name}[/]\n{args_text}" if args_text else f"[bold]{self.name}[/]"

        return Panel(
            content,
            title=f"[{self.theme.tool_color}]Tool Call[/]",
            border_style=self.theme.tool_color,
            padding=(0, 1),
        )


class StreamingText:
    """Streaming text display component."""

    def __init__(self, theme: Theme) -> None:
        """Initialize streaming text."""
        self.theme = theme
        self._cursor_visible = True

    def render(self, text: str) -> RenderableType:
        """
        Render streaming text with cursor.

        Args:
            text: Current text

        Returns:
            Renderable
        """
        cursor = "|" if self._cursor_visible else " "
        self._cursor_visible = not self._cursor_visible

        return Panel(
            Text(text + cursor),
            title="[bold]Assistant[/]",
            title_align="left",
            border_style=self.theme.assistant_color,
            padding=(0, 1),
        )


class HelpDisplay:
    """Help display component."""

    def __init__(self, theme: Theme) -> None:
        """Initialize help display."""
        self.theme = theme

    def render(self, commands: list[tuple[str, str]]) -> RenderableType:
        """
        Render help display.

        Args:
            commands: List of (command, description) tuples

        Returns:
            Renderable
        """
        table = Table(
            show_header=True,
            header_style=f"bold {self.theme.accent_color}",
            border_style=self.theme.border_color,
        )
        table.add_column("Command", style="bold")
        table.add_column("Description")

        for cmd, desc in commands:
            table.add_row(cmd, desc)

        return Panel(
            table,
            title="[bold]Available Commands[/]",
            border_style=self.theme.accent_color,
        )


class ConversationDisplay:
    """Conversation history display component."""

    def __init__(self, theme: Theme) -> None:
        """Initialize conversation display."""
        self.theme = theme

    def render_message(self, role: str, content: str) -> RenderableType:
        """Render a single message."""
        renderers = {
            "user": self._render_user_message,
            "assistant": self._render_assistant_message,
            "tool": self._render_tool_message,
        }
        renderer = renderers.get(role)
        if renderer:
            return renderer(content)
        return Panel(content, title=role)

    def _render_user_message(self, content: str) -> RenderableType:
        """Render user message."""
        return Panel(
            content,
            title=f"[bold {self.theme.user_color}]You[/]",
            border_style=self.theme.user_color,
        )

    def _render_assistant_message(self, content: str) -> RenderableType:
        """Render assistant message."""
        return Panel(
            Markdown(content),
            title=f"[bold {self.theme.assistant_color}]Assistant[/]",
            border_style=self.theme.assistant_color,
        )

    def _render_tool_message(self, content: str) -> RenderableType:
        """Render tool result message."""
        truncated = content[:200] + "..." if len(content) > 200 else content
        return Panel(
            truncated,
            title=f"[{self.theme.tool_color}]Tool Result[/]",
            border_style=self.theme.tool_color,
        )


class TodoPanel:
    """
    Renders the current todo list.

    Displays task progress with status indicators.
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

    def __init__(self, theme: Theme) -> None:
        """Initialize todo panel."""
        self.theme = theme

    def render(self, todo_list: "TodoList") -> RenderableType | None:
        """
        Render the todo list as a Rich panel.

        Args:
            todo_list: The todo list to render

        Returns:
            Panel or None if empty
        """
        if todo_list.is_empty:
            return None

        lines = []
        for item in todo_list.items:
            status_value = item.status.value
            symbol = self.STATUS_SYMBOLS.get(status_value, "?")
            color = self.STATUS_COLORS.get(status_value, "white")

            # Show active_form for in_progress, content otherwise
            text = item.active_form if status_value == "in_progress" else item.content

            lines.append(f"[{color}]{symbol} {text}[/]")

        completed, total = todo_list.progress
        title = f"Todo List ({completed}/{total})"

        return Panel(
            "\n".join(lines),
            title=f"[bold]{title}[/]",
            border_style="blue",
            padding=(0, 1),
        )

    def render_compact(self, todo_list: "TodoList") -> str:
        """
        Render a compact progress indicator.

        Args:
            todo_list: The todo list

        Returns:
            Progress string like "[████░░░░░░] 4/10"
        """
        completed, total = todo_list.progress
        if total == 0:
            return ""

        # Progress bar
        bar_width = 10
        filled = int((completed / total) * bar_width)
        empty = bar_width - filled

        bar = "█" * filled + "░" * empty
        return f"[{bar}] {completed}/{total}"
