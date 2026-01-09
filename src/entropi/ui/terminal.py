"""
Main terminal UI using Rich and Prompt Toolkit.

Provides the interactive chat interface with streaming support.
"""

import asyncio
from collections.abc import Callable, Coroutine
from typing import Any

from prompt_toolkit import PromptSession
from prompt_toolkit.auto_suggest import AutoSuggestFromHistory
from prompt_toolkit.history import InMemoryHistory
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.styles import Style
from rich.console import Console
from rich.live import Live
from rich.markdown import Markdown
from rich.panel import Panel

from entropi.config.schema import EntropyConfig
from entropi.core.compaction import CompactionResult
from entropi.core.engine import AgentState, ToolApproval
from entropi.core.logging import get_logger
from entropi.core.todos import TodoList
from entropi.ui.components import StatusBar, StreamingText, TodoPanel, ToolCallDisplay
from entropi.ui.themes import get_theme

logger = get_logger("ui.terminal")


class TerminalUI:
    """
    Rich terminal interface for Entropi.

    Features:
    - Streaming output display
    - Status bar with metrics
    - Key bindings
    - Theming support
    """

    def __init__(
        self,
        config: EntropyConfig,
    ) -> None:
        """
        Initialize terminal UI.

        Args:
            config: Application configuration
        """
        self.config = config
        self.console = Console()
        self.theme = get_theme(config.ui.theme)

        # Prompt session with in-memory history (no file created)
        self.session = PromptSession(
            history=InMemoryHistory(),
            auto_suggest=AutoSuggestFromHistory(),
            key_bindings=self._create_key_bindings(),
            style=self._create_prompt_style(),
        )

        # State
        self._current_agent_state = AgentState.IDLE
        self._token_count = 0
        self._is_generating = False
        self._auto_approve_all = False  # "yes always" mode

        # Callbacks
        self._on_interrupt: Callable[[], None] | None = None
        self._on_thinking_toggle: Callable[[], Coroutine[Any, Any, None]] | None = None

    def _create_key_bindings(self) -> KeyBindings:
        """Create key bindings."""
        kb = KeyBindings()

        @kb.add("c-c")
        def _(event: Any) -> None:
            """Handle Ctrl+C - raise KeyboardInterrupt for main loop to handle."""
            # Call interrupt callback if set (interrupts generation)
            if self._on_interrupt:
                self._on_interrupt()
            # Raise exception for main loop to handle double-Ctrl+C
            raise KeyboardInterrupt()

        @kb.add("escape")
        def _(event: Any) -> None:
            """Handle Escape - interrupt generation without raising exception."""
            if self._on_interrupt:
                self._on_interrupt()

        @kb.add("c-l")
        def _(event: Any) -> None:
            """Clear screen."""
            self.console.clear()

        @kb.add("c-t")
        def _(event: Any) -> None:
            """Toggle thinking mode."""
            if self._on_thinking_toggle:
                asyncio.create_task(self._on_thinking_toggle())

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

    def set_thinking_toggle_callback(
        self, callback: Callable[[], Coroutine[Any, Any, None]]
    ) -> None:
        """Set callback for thinking mode toggle."""
        self._on_thinking_toggle = callback

    async def get_input(self, prompt: str = "> ") -> str:
        """
        Get user input.

        Args:
            prompt: Prompt string

        Returns:
            User input

        Raises:
            KeyboardInterrupt: When user presses Ctrl+C
        """
        try:
            result: str = await self.session.prompt_async(prompt)
            return result
        except EOFError:
            return "/exit"
        # Let KeyboardInterrupt propagate for main loop to handle

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

    def print_tool_start(self, name: str, arguments: dict[str, Any]) -> None:
        """Print tool execution start status - Claude Code style."""
        # Format based on tool type for readability
        tool_base = name.split(".")[-1]  # e.g., "read_file" from "filesystem.read_file"

        if "read_file" in name:
            path = arguments.get("path", "")
            self.console.print(f"[cyan]⚡ Reading:[/] {path}")
        elif "write_file" in name:
            path = arguments.get("path", "")
            self.console.print(f"[cyan]⚡ Writing:[/] {path}")
        elif "edit_file" in name:
            path = arguments.get("path", "")
            self.console.print(f"[cyan]⚡ Editing:[/] {path}")
        elif "list_directory" in name:
            path = arguments.get("path", ".")
            self.console.print(f"[cyan]⚡ Listing:[/] {path}")
        elif "search_files" in name:
            pattern = arguments.get("pattern", "")
            self.console.print(f"[cyan]⚡ Searching:[/] {pattern}")
        elif "execute" in name:
            cmd = arguments.get("command", "")[:60]
            self.console.print(f"[cyan]⚡ Running:[/] {cmd}")
        elif "git" in name:
            self.console.print(f"[cyan]⚡ Git:[/] {tool_base}")
        elif "diagnostics" in name:
            path = arguments.get("path", "")
            self.console.print(f"[cyan]⚡ Checking:[/] {path}")
        else:
            # Generic format
            args_preview = ", ".join(f"{v}"[:20] for v in arguments.values())[:40]
            self.console.print(f"[cyan]⚡ {tool_base}:[/] {args_preview}")

    def print_tool_complete(self, name: str, result: str, duration_ms: float) -> None:
        """Print tool execution complete status - Claude Code style."""
        result_len = len(result)
        if result_len > 80:
            summary = f"({result_len} chars)"
        else:
            summary = result[:60].replace("\n", " ").strip()
            if len(result) > 60:
                summary += "..."
            summary = f"({summary})" if summary else ""

        self.console.print(
            f"[{self.theme.success_color}]✓[/] Done [dim]{duration_ms:.0f}ms {summary}[/]"
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
        thinking_mode: bool = False,
        context_used: int = 0,
        context_max: int = 0,
    ) -> None:
        """Print status bar."""
        status = StatusBar(
            model=model,
            vram_used=vram_used,
            vram_total=vram_total,
            tokens=tokens,
            theme=self.theme,
            thinking_mode=thinking_mode,
            context_used=context_used,
            context_max=context_max if context_max > 0 else 16384,
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

    async def prompt_tool_approval(
        self,
        name: str,
        arguments: dict[str, Any],
        is_sensitive: bool = False,
    ) -> ToolApproval | str:
        """
        Prompt user to approve a tool call.

        Args:
            name: Tool name
            arguments: Tool arguments
            is_sensitive: Whether this is a sensitive operation

        Returns:
            ToolApproval enum, or feedback string if denied with feedback
        """
        # Check if we're in session-wide auto-approve mode
        if self._auto_approve_all:
            return ToolApproval.ALLOW

        color = self.theme.warning_color if is_sensitive else self.theme.info_color
        icon = "!" if is_sensitive else "?"

        # Display the tool call
        self.console.print(
            f"\n[{color}][{icon}] Tool: {name}[/]"
        )

        # Show arguments (truncate long values like file content)
        for key, value in arguments.items():
            value_str = repr(value)
            if len(value_str) > 200:
                # Show first and last parts for context
                value_str = value_str[:100] + " ... " + value_str[-50:]
            self.console.print(f"    [dim]{key}:[/] {value_str}")

        # Prompt for approval with options
        self.console.print(
            f"\n[dim]Options: [bold]y[/]=yes once, [bold]a[/]=always allow (save), "
            f"[bold]n[/]=no, [bold]d[/]=always deny (save), or type feedback[/]"
        )

        try:
            response = await self.session.prompt_async(
                "[y/a/n/d/?] ",
                default="",
            )
            if response is None:
                return ToolApproval.DENY
            response_lower = response.lower().strip()

            if response_lower in ("y", "yes"):
                return ToolApproval.ALLOW
            elif response_lower in ("a", "always", "always allow"):
                self.console.print(
                    f"[{self.theme.info_color}]Saving permission to config...[/]"
                )
                return ToolApproval.ALWAYS_ALLOW
            elif response_lower in ("n", "no", ""):
                return ToolApproval.DENY
            elif response_lower in ("d", "deny", "always deny"):
                self.console.print(
                    f"[{self.theme.warning_color}]Saving deny rule to config...[/]"
                )
                return ToolApproval.ALWAYS_DENY
            elif response_lower in ("s", "session", "yes to all"):
                # Session-wide auto-approve (legacy behavior)
                self._auto_approve_all = True
                self.console.print(
                    f"[{self.theme.info_color}]Auto-approving all tools for this session[/]"
                )
                return ToolApproval.ALLOW
            else:
                # Treat as feedback for the model
                return response  # Return the feedback string

        except (EOFError, KeyboardInterrupt):
            return ToolApproval.DENY

    def reset_auto_approve(self) -> None:
        """Reset auto-approve mode (e.g., at start of new conversation)."""
        self._auto_approve_all = False

    def is_sensitive_tool(self, name: str, arguments: dict[str, Any]) -> bool:
        """
        Check if a tool call is sensitive and requires explicit approval.

        Args:
            name: Tool name
            arguments: Tool arguments

        Returns:
            True if sensitive
        """
        sensitive_tools = {
            "bash.execute",
            "filesystem.write_file",
            "filesystem.delete",
            "git.commit",
            "git.push",
        }

        # Check tool name
        if name in sensitive_tools:
            return True

        # Check for dangerous patterns in arguments
        dangerous_patterns = ["rm ", "sudo", "chmod", "chown", "> /", "| sh"]
        for value in arguments.values():
            if isinstance(value, str):
                for pattern in dangerous_patterns:
                    if pattern in value:
                        return True

        return False

    def print_todo_panel(self, todo_list: TodoList) -> None:
        """
        Print the todo panel.

        Args:
            todo_list: The todo list to render
        """
        panel = TodoPanel(self.theme)
        rendered = panel.render(todo_list)
        if rendered:
            self.console.print(rendered)

    def print_context_usage(self, used: int, max_tokens: int) -> None:
        """
        Print compact context usage indicator.

        Args:
            used: Tokens used
            max_tokens: Maximum tokens
        """
        if max_tokens <= 0:
            return

        percent = (used / max_tokens) * 100
        bar_width = 15
        filled = int((percent / 100) * bar_width)
        empty = bar_width - filled

        # Color based on usage
        if percent > 90:
            color = self.theme.error_color
        elif percent > 75:
            color = self.theme.warning_color
        else:
            color = "dim"

        bar = "█" * filled + "░" * empty
        self.console.print(
            f"[{color}]Context: [{bar}] {used:,}/{max_tokens:,} ({percent:.0f}%)[/]"
        )

    def print_compaction_notice(self, result: CompactionResult) -> None:
        """
        Print compaction notification.

        Args:
            result: Compaction result with before/after token counts
        """
        reduction = result.old_token_count - result.new_token_count
        reduction_pct = (reduction / result.old_token_count * 100) if result.old_token_count > 0 else 0

        self.console.print(
            Panel(
                f"Context compacted to free space\n"
                f"  [dim]Before:[/] {result.old_token_count:,} tokens\n"
                f"  [dim]After:[/]  {result.new_token_count:,} tokens\n"
                f"  [dim]Freed:[/]  {reduction:,} tokens ({reduction_pct:.0f}%)\n"
                f"  [dim]Kept:[/]   {result.preserved_messages} recent messages",
                title=f"[{self.theme.warning_color}]Context Compacted[/]",
                border_style=self.theme.warning_color,
                padding=(0, 1),
            )
        )
