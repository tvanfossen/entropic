"""
Voice mode full-screen interface.

Provides the main voice conversation screen with visualizer,
transcript, and status displays. Two-phase operation:
1. Initialization: Load models, show settings
2. Conversation: Start after user confirms settings
"""

from __future__ import annotations

import asyncio
import time
from collections.abc import Awaitable, Callable
from typing import TYPE_CHECKING

from textual import work
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Container, Horizontal, Vertical
from textual.screen import Screen
from textual.widgets import Button, Label, Select, Static

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


class VoiceSettingsPanel(Static):
    """Settings panel shown before conversation starts."""

    DEFAULT_CSS = """
    VoiceSettingsPanel {
        width: 100%;
        height: auto;
        padding: 1 2;
        background: $surface;
        border: solid $primary;
    }

    VoiceSettingsPanel .settings-row {
        width: 100%;
        height: auto;
        margin-bottom: 1;
    }

    VoiceSettingsPanel Label {
        width: 20;
    }

    VoiceSettingsPanel Select {
        width: 1fr;
    }

    VoiceSettingsPanel .button-row {
        width: 100%;
        height: auto;
        margin-top: 1;
        align: center middle;
    }

    VoiceSettingsPanel Button {
        margin: 0 1;
    }

    VoiceSettingsPanel #start-btn {
        background: $success;
    }

    VoiceSettingsPanel #generate-thinking-btn {
        background: $warning;
    }
    """

    def __init__(
        self,
        name: str | None = None,
        id: str | None = None,
        classes: str | None = None,
    ) -> None:
        super().__init__(name=name, id=id, classes=classes)
        self._audio_devices: list[tuple[str, int]] = []
        self._input_device: int | None = None
        self._output_device: int | None = None

    def compose(self) -> ComposeResult:
        yield Label("[bold]Voice Settings[/bold]")

        with Horizontal(classes="settings-row"):
            yield Label("Input Device:")
            yield Select(
                [(("Loading...", -1))],
                id="input-device-select",
                allow_blank=False,
            )

        with Horizontal(classes="settings-row"):
            yield Label("Output Device:")
            yield Select(
                [(("Loading...", -1))],
                id="output-device-select",
                allow_blank=False,
            )

        with Horizontal(classes="settings-row"):
            yield Label("Status:")
            yield Label("Initializing...", id="init-status")

        with Horizontal(classes="button-row"):
            yield Button("Generate Thinking Audio", id="generate-thinking-btn", disabled=True)
            yield Button("Start Conversation", id="start-btn", variant="success", disabled=True)

    def set_audio_devices(
        self,
        devices: list[tuple[str, int, bool, bool]],  # (name, index, has_input, has_output)
    ) -> None:
        """Update audio device selectors."""
        input_devices = [(name, idx) for name, idx, has_in, _ in devices if has_in]
        output_devices = [(name, idx) for name, idx, _, has_out in devices if has_out]

        try:
            input_select = self.query_one("#input-device-select", Select)
            input_select.set_options(input_devices if input_devices else [("No devices", -1)])
            if input_devices:
                input_select.value = input_devices[0][1]

            output_select = self.query_one("#output-device-select", Select)
            output_select.set_options(output_devices if output_devices else [("No devices", -1)])
            if output_devices:
                output_select.value = output_devices[0][1]
        except Exception:
            pass

    def set_status(self, status: str) -> None:
        """Update status label."""
        try:
            label = self.query_one("#init-status", Label)
            label.update(status)
        except Exception:
            pass

    def set_ready(self, ready: bool) -> None:
        """Enable/disable start button."""
        try:
            start_btn = self.query_one("#start-btn", Button)
            start_btn.disabled = not ready
            thinking_btn = self.query_one("#generate-thinking-btn", Button)
            thinking_btn.disabled = not ready
        except Exception:
            pass

    def get_selected_devices(self) -> tuple[int | None, int | None]:
        """Get selected input and output device indices."""
        try:
            input_select = self.query_one("#input-device-select", Select)
            output_select = self.query_one("#output-device-select", Select)
            input_idx = input_select.value if input_select.value != -1 else None
            output_idx = output_select.value if output_select.value != -1 else None
            return input_idx, output_idx
        except Exception:
            return None, None


class VoiceScreen(Screen[None]):
    """
    Full-screen voice conversation interface.

    Two-phase operation:
    1. Initialization phase: Load models, detect devices, show settings
    2. Conversation phase: After user starts, run conversation loop
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

    #settings-container {
        height: auto;
        width: 100%;
        margin-bottom: 1;
    }

    #transcript-container {
        height: 1fr;
        width: 100%;
    }

    .hidden {
        display: none;
    }
    """

    BINDINGS = [
        Binding("escape", "exit_voice", "Exit", show=True),
        Binding("space", "toggle_pause", "Pause/Resume", show=True),
        Binding("enter", "start_conversation", "Start", show=True),
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
        self._initialized: bool = False
        self._conversation_started: bool = False

    def compose(self) -> ComposeResult:
        """Compose screen layout."""
        yield VoiceHeader(title="Voice Mode")

        with Vertical(id="voice-content"):
            with Container(id="visualizer-container"):
                yield VoiceVisualizer(id="visualizer")

            with Container(id="settings-container"):
                yield VoiceSettingsPanel(id="settings-panel")

            with Container(id="transcript-container"):
                yield VoiceTranscript(id="transcript")

        yield VoiceStatusBar(id="status-bar")

    async def on_mount(self) -> None:
        """Handle screen mount - start initialization."""
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

        # Start initialization (but don't start conversation)
        self._run_initialization()

    @work(exclusive=True)
    async def _run_initialization(self) -> None:
        """Initialize models and audio devices (but don't start conversation)."""
        if self._controller is None:
            return

        status_bar = self.query_one("#status-bar", VoiceStatusBar)
        status_bar.set_state("initializing")
        settings_panel = self.query_one("#settings-panel", VoiceSettingsPanel)

        try:
            # Call on_enter to free GPU memory (unload chat models)
            if self._on_enter:
                settings_panel.set_status("Unloading chat models...")
                await self._on_enter()

            # Initialize controller (loads models)
            settings_panel.set_status("Loading PersonaPlex models...")
            success = await self._controller.initialize()
            if not success:
                settings_panel.set_status("[red]Initialization failed[/red]")
                self.notify("Failed to initialize voice mode", severity="error")
                return

            # Detect audio devices
            settings_panel.set_status("Detecting audio devices...")
            devices = await self._detect_audio_devices()
            settings_panel.set_audio_devices(devices)

            # Ready for user to start
            self._initialized = True
            settings_panel.set_status("[green]Ready - Press Enter or click Start[/green]")
            settings_panel.set_ready(True)
            status_bar.set_state("ready")

            self._on_loading_progress("Models loaded. Configure settings and press Enter to start.")

        except Exception as e:
            logger.error(f"Voice initialization error: {e}")
            settings_panel.set_status(f"[red]Error: {e}[/red]")
            self.notify(f"Voice mode error: {e}", severity="error")

    async def _detect_audio_devices(self) -> list[tuple[str, int, bool, bool]]:
        """Detect available audio devices."""

        def _query_devices() -> list[tuple[str, int, bool, bool]]:
            """Query audio devices in a dedicated thread (PortAudio is thread-sensitive)."""
            import sys
            import traceback

            devices = []
            try:
                logger.info("Detecting audio devices...")
                logger.info(f"sys.path: {sys.path[:5]}")  # First 5 entries
                import sounddevice as sd

                logger.info(f"sounddevice v{sd.__version__} from {sd.__file__}")
                device_list = sd.query_devices()
                logger.info(f"Found {len(device_list)} audio devices")

                for i, dev in enumerate(device_list):
                    name = dev["name"]
                    has_input = dev["max_input_channels"] > 0
                    has_output = dev["max_output_channels"] > 0
                    devices.append((name, i, has_input, has_output))
            except Exception as e:
                logger.error(f"Audio device detection failed: {e}")
                logger.error(f"Traceback:\n{traceback.format_exc()}")
            return devices

        # Run in thread to avoid PortAudio thread context issues
        return await asyncio.to_thread(_query_devices)

    async def action_start_conversation(self) -> None:
        """Start the conversation after user confirms settings."""
        if not self._initialized or self._conversation_started:
            return

        if self._controller is None:
            return

        self._conversation_started = True

        # Hide settings panel
        try:
            settings_container = self.query_one("#settings-container", Container)
            settings_container.add_class("hidden")
        except Exception:
            pass

        # Get selected devices
        settings_panel = self.query_one("#settings-panel", VoiceSettingsPanel)
        input_device, output_device = settings_panel.get_selected_devices()

        # TODO: Pass selected devices to audio_io

        # Start conversation
        self._start_time = time.time()
        self._elapsed_timer = asyncio.create_task(self._update_elapsed())

        status_bar = self.query_one("#status-bar", VoiceStatusBar)
        status_bar.set_state("conversation")

        await self._controller.start_conversation()

    def on_button_pressed(self, event: Button.Pressed) -> None:
        """Handle button presses."""
        if event.button.id == "start-btn":
            self.run_worker(self.action_start_conversation())
        elif event.button.id == "generate-thinking-btn":
            self.run_worker(self._generate_thinking_audio())

    async def _generate_thinking_audio(self) -> None:
        """Generate thinking audio file using current voice."""
        if not self._initialized or self._controller is None:
            return

        self.notify("Generating thinking audio...", timeout=5)
        # TODO: Implement thinking audio generation
        self.notify("Thinking audio generation not yet implemented", severity="warning")

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
            # Don't override "ready" state from initialization
            if self._initialized and not self._conversation_started:
                return
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
            transcript = self.query_one("#transcript", VoiceTranscript)
            transcript.add_text("system", message)
        except Exception:
            pass

    async def action_exit_voice(self) -> None:
        """Exit voice mode."""
        await self._cleanup()
        self.app.pop_screen()

    async def action_toggle_pause(self) -> None:
        """Toggle pause/resume."""
        if self._controller is None or not self._conversation_started:
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
