"""
Application orchestrator.

Coordinates all components and manages the application lifecycle.
"""

from pathlib import Path

from rich.console import Console

from entropi.config.schema import EntropyConfig
from entropi.core.base import Message
from entropi.core.logging import get_logger
from entropi.inference.orchestrator import ModelOrchestrator


class Application:
    """Main application orchestrator."""

    def __init__(
        self,
        config: EntropyConfig,
        project_dir: Path | None = None,
    ) -> None:
        """
        Initialize application.

        Args:
            config: Application configuration
            project_dir: Project directory
        """
        self.config = config
        self.project_dir = project_dir or Path.cwd()
        self.logger = get_logger("app")
        self.console = Console()

        # Components (initialized lazily)
        self._orchestrator: ModelOrchestrator | None = None
        self._mcp_manager = None  # TODO: Implementation 03
        self._storage = None  # TODO: Implementation 07
        self._ui = None  # TODO: Implementation 06

    async def initialize(self) -> None:
        """Initialize all components."""
        self.logger.info("Initializing Entropi...")

        # Initialize model orchestrator
        self._orchestrator = ModelOrchestrator(self.config)
        await self._orchestrator.initialize()

        # TODO: Initialize other components in later phases
        # - MCP manager (Implementation 03)
        # - Storage backend (Implementation 07)
        # - Terminal UI (Implementation 06)

        self.logger.info("Entropi initialized")

    async def shutdown(self) -> None:
        """Shutdown all components."""
        self.logger.info("Shutting down...")

        # Shutdown in reverse order
        if self._orchestrator:
            await self._orchestrator.shutdown()

        self.logger.info("Shutdown complete")

    async def run(self) -> None:
        """Run the interactive application."""
        try:
            await self.initialize()

            self.console.print("[bold green]Entropi[/bold green] initialized!")
            self.console.print(f"Project: {self.project_dir}")
            self.console.print(f"Config: {self.config.config_dir}")

            if self._orchestrator:
                loaded = self._orchestrator.get_loaded_models()
                available = self._orchestrator.get_available_models()
                if loaded:
                    self.console.print(f"Loaded models: {', '.join(loaded)}")
                elif available:
                    self.console.print(f"Available models: {', '.join(available)}")
                else:
                    self.console.print("[yellow]No models configured[/yellow]")

            self.console.print("\n[yellow]Interactive mode not yet implemented.[/yellow]")
            self.console.print("Use 'entropi ask \"your question\"' for now.")

        except KeyboardInterrupt:
            self.console.print("\n[yellow]Interrupted[/yellow]")
        finally:
            await self.shutdown()

    async def single_turn(self, message: str, stream: bool = True) -> None:
        """
        Process a single message and exit.

        Args:
            message: User message
            stream: Whether to stream output
        """
        try:
            await self.initialize()

            # Check if models are available
            if not self._orchestrator or not self._orchestrator.get_available_models():
                self.console.print(f"[dim]You: {message}[/dim]")
                self.console.print("\n[yellow]No models configured.[/yellow]")
                self.console.print(
                    "[dim]Configure models in ~/.entropi/config.yaml or "
                    ".entropi/config.yaml[/dim]"
                )
                return

            self.console.print(f"[dim]You: {message}[/dim]\n")

            messages = [Message(role="user", content=message)]

            if stream:
                self.console.print("[bold]Assistant:[/bold] ", end="")
                async for chunk in self._orchestrator.generate_stream(messages):
                    self.console.print(chunk, end="")
                self.console.print()
            else:
                result = await self._orchestrator.generate(messages)
                self.console.print(f"[bold]Assistant:[/bold] {result.content}")
                self.console.print(
                    f"\n[dim]Tokens: {result.token_count}, "
                    f"Time: {result.generation_time_ms}ms[/dim]"
                )

        finally:
            await self.shutdown()
