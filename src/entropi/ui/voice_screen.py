"""
Voice mode full-screen interface.

Provides the main voice conversation screen with visualizer,
transcript, and status displays.
"""

from __future__ import annotations

import asyncio
import time
from collections.abc import Awaitable, Callable
from typing import TYPE_CHECKING

from textual import work
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Container, Vertical
from textual.screen import Screen
from textual.widgets import Static

from entropi.core.logging import get_logger
from entropi.ui.voice_widgets import (
    VoiceHeader,
    VoiceStatusBar,
    VoiceTranscript,
    VoiceVisualizer,
)
from entropi.voice.controller import PersonaPlexController, VoiceCallbacks, VoiceState

if TYPE_CHECKING:
    from entropi.config.schema import VoiceConfig

logger = get_logger("ui.voice_screen")


class VoiceScreen(Screen[None]):
    """
    Full-screen voice conversation interface.

    Manages the voice controller lifecycle and displays conversation
    state through visualizer and transcript widgets.
    """

    CSS = """
    VoiceScreen {
        layout: vertical;
    }

    #voice-content {
        height: 100%;
        width: 100%;
        padding: 1;
    }

    #visualizer-container {
        height: 8;
        width: 100%;
        margin-bottom: 1;
    }

    #transcript-container {
        height: 1fr;
        width: 100%;
    }
    """

    BINDINGS = [
        Binding("escape", "exit_voice", "Exit", show=True),
        Binding("space", "toggle_pause", "Pause/Resume", show=True),
        Binding("r", "reset_context", "Reset", show=True),
    ]

    def __init__(
        self,
        config: VoiceConfig,
        on_enter: Callable[[], Awaitable[None]] | None = None,
        on_exit: Callable[[], Awaitable[None]] | None = None,
        name: str | None = None,
        id: str | None = None,
        classes: str | None = None,
    ) -> None:
        """
        Initialize voice screen.

        Args:
            config: Voice configuration
            on_enter: Async callback when entering voice mode (e.g., unload chat models)
            on_exit: Async callback when exiting voice mode (e.g., reload chat models)
            name: Screen name
            id: Screen ID
            classes: CSS classes
        """
        super().__init__(name=name, id=id, classes=classes)
        self._config = config
        self._on_enter = on_enter
        self._on_exit = on_exit
        self._controller: PersonaPlexController | None = None
        self._start_time: float = 0
        self._elapsed_timer: asyncio.Task[None] | None = None

    def compose(self) -> ComposeResult:
        """Compose screen layout."""
        yield VoiceHeader(title="Voice Mode")

        with Vertical(id="voice-content"):
            with Container(id="visualizer-container"):
                yield VoiceVisualizer(id="visualizer")

            with Container(id="transcript-container"):
                yield VoiceTranscript(id="transcript")

        yield VoiceStatusBar(id="status-bar")

    async def on_mount(self) -> None:
        """Handle screen mount - initialize voice controller."""
        # Create callbacks
        callbacks = VoiceCallbacks(
            on_state_change=self._on_state_change,
            on_transcript_update=self._on_transcript_update,
            on_input_level=self._on_input_level,
            on_output_level=self._on_output_level,
            on_error=self._on_error,
            on_loading_progress=self._on_loading_progress,
        )

        # Create controller
        self._controller = PersonaPlexController(self._config, callbacks)

        # Start initialization
        self._run_initialization()

    @work(exclusive=True)
    async def _run_initialization(self) -> None:
        """Initialize and start voice conversation."""
        if self._controller is None:
            return

        status_bar = self.query_one("#status-bar", VoiceStatusBar)
        status_bar.set_state("initializing")

        try:
            # Call on_enter to free GPU memory (unload chat models)
            if self._on_enter:
                self.notify("Unloading chat models to free GPU memory...")
                await self._on_enter()

            success = await self._controller.initialize()
            if not success:
                self.notify("Failed to initialize voice mode", severity="error")
                self.app.pop_screen()
                return

            # Start conversation
            self._start_time = time.time()
            self._elapsed_timer = asyncio.create_task(self._update_elapsed())
            await self._controller.start_conversation()

        except Exception as e:
            logger.error(f"Voice initialization error: {e}")
            self.notify(f"Voice mode error: {e}", severity="error")
            self.app.pop_screen()

    async def _update_elapsed(self) -> None:
        """Update elapsed time display periodically."""
        while True:
            await asyncio.sleep(1)
            elapsed = time.time() - self._start_time
            try:
                status_bar = self.query_one("#status-bar", VoiceStatusBar)
                status_bar.set_elapsed(elapsed)
            except Exception:
                break

    def _on_state_change(self, state: VoiceState) -> None:
        """Handle voice state changes."""
        state_map = {
            VoiceState.IDLE: "idle",
            VoiceState.INITIALIZING: "initializing",
            VoiceState.CONVERSATION: "conversation",
            VoiceState.THINKING: "thinking",
            VoiceState.COMPACTING: "compacting",
            VoiceState.ERROR: "error",
        }

        try:
            status_bar = self.query_one("#status-bar", VoiceStatusBar)
            status_bar.set_state(state_map.get(state, "unknown"))

            if self._controller:
                status_bar.set_window(self._controller.window_number)
        except Exception:
            pass

    def _on_transcript_update(self, speaker: str, text: str) -> None:
        """Handle transcript updates."""
        try:
            transcript = self.query_one("#transcript", VoiceTranscript)
            transcript.add_text(speaker, text)
        except Exception:
            pass

    def _on_input_level(self, level: float) -> None:
        """Handle input audio level updates."""
        try:
            visualizer = self.query_one("#visualizer", VoiceVisualizer)
            visualizer.set_level(level, "input")
        except Exception:
            pass

    def _on_output_level(self, level: float) -> None:
        """Handle output audio level updates."""
        try:
            visualizer = self.query_one("#visualizer", VoiceVisualizer)
            visualizer.set_level(level, "output")
        except Exception:
            pass

    def _on_error(self, message: str) -> None:
        """Handle error messages."""
        self.notify(message, severity="error")

    def _on_loading_progress(self, message: str) -> None:
        """Handle loading progress updates."""
        try:
            # Update transcript area with loading status
            transcript = self.query_one("#transcript", VoiceTranscript)
            transcript.add_text("system", message)
            # Also show as notification for visibility
            self.notify(message, timeout=10)
        except Exception:
            pass

    async def action_exit_voice(self) -> None:
        """Exit voice mode."""
        await self._cleanup()
        self.app.pop_screen()

    async def action_toggle_pause(self) -> None:
        """Toggle pause/resume."""
        if self._controller is None:
            return

        if self._controller.is_paused:
            await self._controller.resume()
            self.notify("Resumed", timeout=1)
        else:
            await self._controller.pause()
            self.notify("Paused - Press Space to resume", timeout=2)

    async def action_reset_context(self) -> None:
        """Reset context and transcript."""
        if self._controller is None:
            return

        await self._controller.reset()

        try:
            transcript = self.query_one("#transcript", VoiceTranscript)
            transcript.clear()
        except Exception:
            pass

        self.notify("Context reset", timeout=1)

    async def _cleanup(self) -> None:
        """Clean up resources."""
        if self._elapsed_timer:
            self._elapsed_timer.cancel()
            try:
                await self._elapsed_timer
            except asyncio.CancelledError:
                pass

        if self._controller:
            await self._controller.stop()
            self._controller = None

        # Call on_exit to reload chat models
        if self._on_exit:
            try:
                await self._on_exit()
            except Exception as e:
                logger.warning(f"Error in on_exit callback: {e}")

    async def on_unmount(self) -> None:
        """Handle screen unmount."""
        await self._cleanup()
