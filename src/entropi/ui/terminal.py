"""
Main terminal UI using Rich and Prompt Toolkit.

Provides the interactive chat interface with streaming support.
"""

import asyncio
from collections.abc import Callable
from typing import Any

from prompt_toolkit import PromptSession
from prompt_toolkit.auto_suggest import AutoSuggestFromHistory
from prompt_toolkit.history import FileHistory
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.styles import Style
from rich.console import Console
from rich.live import Live
from rich.markdown import Markdown
from rich.panel import Panel

from entropi.config.schema import EntropyConfig
from entropi.core.engine import AgentState
from entropi.core.logging import get_logger
from entropi.ui.components import StatusBar, StreamingText, ToolCallDisplay
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
        def _(event: Any) -> None:
            """Handle Ctrl+C."""
            if self._is_generating:
                if self._on_interrupt:
                    self._on_interrupt()
            else:
                event.app.exit()

        @kb.add("c-l")
        def _(event: Any) -> None:
            """Clear screen."""
            self.console.clear()

        return kb

    def _create_prompt_style(self) -> Style:
        """Create prompt style."""
        return Style.from_dict(
            {
                "prompt": self.theme.prompt_color,
                "": self.theme.text_color,
            }
        )

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
            result: str = await self.session.prompt_async(prompt)
            return result
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
        self.console.print(f"[bold {self.theme.user_color}]You:[/] {message}")
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

    def print_tool_call(self, name: str, arguments: dict[str, Any]) -> None:
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

    async def stream_response(self, chunks: "asyncio.Queue[str | None]") -> str:
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
                except TimeoutError:
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
        self.console.print(f"[bold {self.theme.error_color}]Error:[/] {message}")

    def print_warning(self, message: str) -> None:
        """Print warning message."""
        self.console.print(f"[bold {self.theme.warning_color}]Warning:[/] {message}")

    def print_info(self, message: str) -> None:
        """Print info message."""
        self.console.print(f"[{self.theme.info_color}]{message}[/]")

    def clear(self) -> None:
        """Clear the terminal."""
        self.console.clear()

    def update_state(self, state: AgentState) -> None:
        """Update current agent state."""
        self._current_agent_state = state

    def update_token_count(self, count: int) -> None:
        """Update token count."""
        self._token_count = count
