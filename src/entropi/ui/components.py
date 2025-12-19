"""
Reusable UI components.

Provides building blocks for the terminal interface.
"""

from dataclasses import dataclass
from typing import Any

from rich.console import RenderableType
from rich.markdown import Markdown
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

from entropi.ui.themes import Theme


@dataclass
class StatusBar:
    """Status bar component."""

    model: str
    vram_used: float
    vram_total: float
    tokens: int
    theme: Theme

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

        table = Table.grid(padding=1)
        table.add_column(justify="left")
        table.add_column(justify="center")
        table.add_column(justify="right")

        table.add_row(
            f"[bold]Model:[/] {self.model}",
            f"[{vram_color}]VRAM: {self.vram_used:.1f}/{self.vram_total:.1f} GB[/]",
            f"[dim]Tokens: {self.tokens:,}[/]",
        )

        return Panel(
            table,
            border_style=self.theme.border_color,
            padding=(0, 1),
        )


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
