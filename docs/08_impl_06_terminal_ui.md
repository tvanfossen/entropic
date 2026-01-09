# Implementation 06: Terminal UI

> Rich terminal interface with streaming, themes, and key bindings

**Prerequisites:** Implementation 05 complete  
**Estimated Time:** 2-3 hours with Claude Code  
**Checkpoint:** Interactive chat session works with streaming

---

## Objectives

1. Create terminal UI with Rich + Prompt Toolkit
2. Implement streaming output display
3. Add status bar with metrics
4. Create key bindings and history navigation
5. Build reusable UI components

---

## 1. Main Terminal UI

### File: `src/entropi/ui/terminal.py`

```python
"""
Main terminal UI using Rich and Prompt Toolkit.

Provides the interactive chat interface with streaming support.
"""
import asyncio
from typing import Callable

from prompt_toolkit import PromptSession
from prompt_toolkit.auto_suggest import AutoSuggestFromHistory
from prompt_toolkit.history import FileHistory
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.styles import Style
from rich.console import Console
from rich.live import Live
from rich.panel import Panel
from rich.markdown import Markdown

from entropi.config.schema import EntropyConfig
from entropi.core.base import Message
from entropi.core.engine import AgentState
from entropi.core.logging import get_logger
from entropi.ui.components import StatusBar, ToolCallDisplay, StreamingText
from entropi.ui.themes import get_theme

logger = get_logger("ui.terminal")


class TerminalUI:
    """
    Rich terminal interface for Entropi.

    Features:
    - Streaming output display
    - Status bar with metrics
    - Command history
    - Key bindings
    - Theming support
    """

    def __init__(
        self,
        config: EntropyConfig,
        history_file: str | None = None,
    ) -> None:
        """
        Initialize terminal UI.

        Args:
            config: Application configuration
            history_file: Path to command history file
        """
        self.config = config
        self.console = Console()
        self.theme = get_theme(config.ui.theme)

        # Prompt session with history
        history_path = history_file or str(config.config_dir / "history")
        self.session = PromptSession(
            history=FileHistory(history_path),
            auto_suggest=AutoSuggestFromHistory(),
            key_bindings=self._create_key_bindings(),
            style=self._create_prompt_style(),
        )

        # State
        self._current_agent_state = AgentState.IDLE
        self._token_count = 0
        self._is_generating = False

        # Callbacks
        self._on_interrupt: Callable[[], None] | None = None

    def _create_key_bindings(self) -> KeyBindings:
        """Create key bindings."""
        kb = KeyBindings()

        @kb.add("c-c")
        def _(event):
            """Handle Ctrl+C."""
            if self._is_generating:
                if self._on_interrupt:
                    self._on_interrupt()
            else:
                event.app.exit()

        @kb.add("c-l")
        def _(event):
            """Clear screen."""
            self.console.clear()

        @kb.add("c-t")
        def _(event):
            """Toggle thinking mode."""
            if self._on_thinking_toggle:
                self._on_thinking_toggle()

        return kb

    def set_thinking_toggle_callback(self, callback: Callable[[], None]) -> None:
        """Set callback for thinking mode toggle."""
        self._on_thinking_toggle = callback

    def _create_prompt_style(self) -> Style:
        """Create prompt style."""
        return Style.from_dict({
            "prompt": self.theme.prompt_color,
            "": self.theme.text_color,
        })

    def set_interrupt_callback(self, callback: Callable[[], None]) -> None:
        """Set callback for interrupt handling."""
        self._on_interrupt = callback

    async def get_input(self, prompt: str = "> ") -> str:
        """
        Get user input.

        Args:
            prompt: Prompt string

        Returns:
            User input
        """
        try:
            return await self.session.prompt_async(prompt)
        except EOFError:
            return "/exit"
        except KeyboardInterrupt:
            return ""

    def print_welcome(self, version: str, models: list[str]) -> None:
        """Print welcome message."""
        self.console.print()
        self.console.print(
            Panel(
                f"[bold {self.theme.accent_color}]Entropi[/] v{version}\n"
                f"[dim]Local AI Coding Assistant[/]\n\n"
                f"Models: {', '.join(models) or 'None loaded'}\n"
                f"Type [bold]/help[/] for commands, [bold]/exit[/] to quit",
                title="Welcome",
                border_style=self.theme.border_color,
            )
        )
        self.console.print()

    def print_user_message(self, message: str) -> None:
        """Print user message."""
        self.console.print(
            f"[bold {self.theme.user_color}]You:[/] {message}"
        )
        self.console.print()

    def print_assistant_message(self, message: str) -> None:
        """Print assistant message."""
        self.console.print(
            Panel(
                Markdown(message),
                title="[bold]Assistant[/]",
                title_align="left",
                border_style=self.theme.assistant_color,
                padding=(0, 1),
            )
        )
        self.console.print()

    def print_tool_call(self, name: str, arguments: dict) -> None:
        """Print tool call."""
        display = ToolCallDisplay(name, arguments, self.theme)
        self.console.print(display.render())

    def print_tool_result(self, name: str, result: str, is_error: bool = False) -> None:
        """Print tool result."""
        color = self.theme.error_color if is_error else self.theme.success_color
        truncated = result[:500] + "..." if len(result) > 500 else result

        self.console.print(
            Panel(
                truncated,
                title=f"[{color}]{'Error' if is_error else 'Result'}: {name}[/]",
                border_style=color,
                padding=(0, 1),
            )
        )

    async def stream_response(self, chunks: asyncio.Queue) -> str:
        """
        Stream response chunks to display.

        Args:
            chunks: Queue of response chunks

        Returns:
            Complete response
        """
        self._is_generating = True
        full_response = ""

        streaming_text = StreamingText(self.theme)

        with Live(
            streaming_text.render(""),
            console=self.console,
            refresh_per_second=30,
            transient=True,
        ) as live:
            while True:
                try:
                    chunk = await asyncio.wait_for(chunks.get(), timeout=0.1)
                    if chunk is None:
                        break
                    full_response += chunk
                    live.update(streaming_text.render(full_response))
                except asyncio.TimeoutError:
                    continue

        self._is_generating = False

        # Print final message
        if full_response:
            self.print_assistant_message(full_response)

        return full_response

    def print_status(
        self,
        model: str,
        vram_used: float,
        vram_total: float,
        tokens: int,
    ) -> None:
        """Print status bar."""
        status = StatusBar(
            model=model,
            vram_used=vram_used,
            vram_total=vram_total,
            tokens=tokens,
            theme=self.theme,
        )
        self.console.print(status.render())

    def print_error(self, message: str) -> None:
        """Print error message."""
        self.console.print(
            f"[bold {self.theme.error_color}]Error:[/] {message}"
        )

    def print_warning(self, message: str) -> None:
        """Print warning message."""
        self.console.print(
            f"[bold {self.theme.warning_color}]Warning:[/] {message}"
        )

    def print_info(self, message: str) -> None:
        """Print info message."""
        self.console.print(
            f"[{self.theme.info_color}]{message}[/]"
        )

    def clear(self) -> None:
        """Clear the terminal."""
        self.console.clear()

    def update_state(self, state: AgentState) -> None:
        """Update current agent state."""
        self._current_agent_state = state

    def update_token_count(self, count: int) -> None:
        """Update token count."""
        self._token_count = count
```

---

## 2. UI Components

### File: `src/entropi/ui/components.py`

```python
"""
Reusable UI components.

Provides building blocks for the terminal interface.
"""
from dataclasses import dataclass
from typing import Any

from rich.console import RenderableType
from rich.panel import Panel
from rich.table import Table
from rich.text import Text
from rich.markdown import Markdown

from entropi.ui.themes import Theme


@dataclass
class StatusBar:
    """Status bar component with thinking mode indicator."""

    model: str
    vram_used: float
    vram_total: float
    tokens: int
    theme: Theme
    thinking_enabled: bool = False
    current_task: str = ""  # "reasoning" or "code"

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

        # Thinking mode indicator
        if self.thinking_enabled:
            mode_indicator = "[bold cyan]🧠 Thinking[/]"
        else:
            mode_indicator = "[dim]⚡ Normal[/]"

        # Current task indicator
        if self.current_task == "code":
            task_indicator = "[yellow]</> Code[/]"
        elif self.current_task == "reasoning":
            task_indicator = "[blue]💭 Reasoning[/]"
        else:
            task_indicator = ""

        table = Table.grid(padding=1)
        table.add_column(justify="left")
        table.add_column(justify="center")
        table.add_column(justify="center")
        table.add_column(justify="right")

        table.add_row(
            f"{mode_indicator} │ [bold]Model:[/] {self.model}",
            f"[{vram_color}]VRAM: {self.vram_used:.1f}/{self.vram_total:.1f} GB[/]",
            task_indicator,
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
        args_text = "\n".join(
            f"  {k}: {v}" for k, v in self.arguments.items()
        )

        content = f"[bold]{self.name}[/]\n{args_text}" if args_text else f"[bold]{self.name}[/]"

        return Panel(
            content,
            title=f"[{self.theme.tool_color}]🔧 Tool Call[/]",
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
        cursor = "▌" if self._cursor_visible else " "
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
        if role == "user":
            return Panel(
                content,
                title=f"[bold {self.theme.user_color}]You[/]",
                border_style=self.theme.user_color,
            )
        elif role == "assistant":
            return Panel(
                Markdown(content),
                title=f"[bold {self.theme.assistant_color}]Assistant[/]",
                border_style=self.theme.assistant_color,
            )
        elif role == "tool":
            return Panel(
                content[:200] + "..." if len(content) > 200 else content,
                title=f"[{self.theme.tool_color}]Tool Result[/]",
                border_style=self.theme.tool_color,
            )
        else:
            return Panel(content, title=role)
```

---

## 3. Themes

### File: `src/entropi/ui/themes.py`

```python
"""
UI themes for terminal interface.

Provides consistent color schemes across the application.
"""
from dataclasses import dataclass
from typing import Literal


@dataclass(frozen=True)
class Theme:
    """Color theme definition."""

    name: str

    # Base colors
    text_color: str
    background_color: str
    border_color: str

    # Accent colors
    accent_color: str
    prompt_color: str

    # Role colors
    user_color: str
    assistant_color: str
    tool_color: str

    # Status colors
    success_color: str
    warning_color: str
    error_color: str
    info_color: str


# Built-in themes
DARK_THEME = Theme(
    name="dark",
    text_color="white",
    background_color="black",
    border_color="bright_black",
    accent_color="cyan",
    prompt_color="green",
    user_color="blue",
    assistant_color="green",
    tool_color="yellow",
    success_color="green",
    warning_color="yellow",
    error_color="red",
    info_color="cyan",
)

LIGHT_THEME = Theme(
    name="light",
    text_color="black",
    background_color="white",
    border_color="bright_black",
    accent_color="blue",
    prompt_color="blue",
    user_color="blue",
    assistant_color="green",
    tool_color="magenta",
    success_color="green",
    warning_color="yellow",
    error_color="red",
    info_color="blue",
)

THEMES = {
    "dark": DARK_THEME,
    "light": LIGHT_THEME,
}


def get_theme(name: Literal["dark", "light", "auto"]) -> Theme:
    """
    Get theme by name.

    Args:
        name: Theme name

    Returns:
        Theme instance
    """
    if name == "auto":
        # Could detect terminal theme here
        return DARK_THEME

    return THEMES.get(name, DARK_THEME)
```

---

## 4. Key Bindings

### File: `src/entropi/ui/keybindings.py`

```python
"""
Key binding definitions.

Centralizes all keyboard shortcuts and their actions.
"""
from dataclasses import dataclass
from typing import Callable, Any

from prompt_toolkit.key_binding import KeyBindings, KeyPressEvent


@dataclass
class KeyBinding:
    """Key binding definition."""

    keys: str
    description: str
    action: str


# Standard key bindings
STANDARD_BINDINGS = [
    KeyBinding("ctrl-c", "Interrupt generation / Exit", "interrupt"),
    KeyBinding("ctrl-d", "Exit", "exit"),
    KeyBinding("ctrl-l", "Clear screen", "clear"),
    KeyBinding("ctrl-r", "Search history", "search_history"),
    KeyBinding("up", "Previous command", "history_prev"),
    KeyBinding("down", "Next command", "history_next"),
    KeyBinding("tab", "Autocomplete", "autocomplete"),
]


def create_key_bindings(
    on_interrupt: Callable[[], None] | None = None,
    on_exit: Callable[[], None] | None = None,
    on_clear: Callable[[], None] | None = None,
) -> KeyBindings:
    """
    Create key bindings with callbacks.

    Args:
        on_interrupt: Interrupt callback
        on_exit: Exit callback
        on_clear: Clear screen callback

    Returns:
        KeyBindings instance
    """
    kb = KeyBindings()

    @kb.add("c-c")
    def handle_ctrl_c(event: KeyPressEvent) -> None:
        """Handle Ctrl+C."""
        if on_interrupt:
            on_interrupt()

    @kb.add("c-d")
    def handle_ctrl_d(event: KeyPressEvent) -> None:
        """Handle Ctrl+D."""
        if on_exit:
            on_exit()
        event.app.exit()

    @kb.add("c-l")
    def handle_ctrl_l(event: KeyPressEvent) -> None:
        """Handle Ctrl+L."""
        if on_clear:
            on_clear()

    return kb


def get_binding_help() -> list[tuple[str, str]]:
    """Get list of key bindings for help display."""
    return [(b.keys, b.description) for b in STANDARD_BINDINGS]
```

---

## 5. Tests

### File: `tests/unit/test_ui.py`

```python
"""Tests for UI components."""
import pytest

from entropi.ui.themes import get_theme, DARK_THEME, LIGHT_THEME
from entropi.ui.components import StatusBar, ToolCallDisplay


class TestThemes:
    """Tests for theme system."""

    def test_get_dark_theme(self) -> None:
        """Test getting dark theme."""
        theme = get_theme("dark")
        assert theme == DARK_THEME
        assert theme.name == "dark"

    def test_get_light_theme(self) -> None:
        """Test getting light theme."""
        theme = get_theme("light")
        assert theme == LIGHT_THEME

    def test_auto_defaults_to_dark(self) -> None:
        """Test auto theme defaults to dark."""
        theme = get_theme("auto")
        assert theme == DARK_THEME

    def test_invalid_theme_defaults(self) -> None:
        """Test invalid theme name defaults to dark."""
        theme = get_theme("nonexistent")
        assert theme == DARK_THEME


class TestComponents:
    """Tests for UI components."""

    def test_status_bar_renders(self) -> None:
        """Test status bar rendering."""
        theme = get_theme("dark")
        status = StatusBar(
            model="14B",
            vram_used=10.5,
            vram_total=16.0,
            tokens=1234,
            theme=theme,
        )
        rendered = status.render()
        assert rendered is not None

    def test_tool_call_display(self) -> None:
        """Test tool call display."""
        theme = get_theme("dark")
        display = ToolCallDisplay(
            name="read_file",
            arguments={"path": "test.py"},
            theme=theme,
        )
        rendered = display.render()
        assert rendered is not None
```

---

## Checkpoint: Verification

```bash
# Run tests
pytest tests/unit/test_ui.py -v

# Test interactive mode
entropi
# Should show welcome message and prompt
# Type a message and see streaming response
# Press Ctrl+C to exit
```

**Success Criteria:**
- [ ] Theme tests pass
- [ ] Component tests pass
- [ ] Interactive mode launches
- [ ] Streaming output displays correctly
- [ ] Ctrl+C interrupts generation

---

## Next Phase

Proceed to **Implementation 07: Storage** to implement conversation persistence.
