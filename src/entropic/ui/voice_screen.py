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
from dataclasses import dataclass
from typing import TYPE_CHECKING

from textual import work
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Container, Horizontal
from textual.screen import Screen
from textual.widgets import Button, Label, Select, Static

from entropic.core.logging import get_logger
from entropic.ui.voice_widgets import (
    VoiceHeader,
    VoiceStatsPanel,
    VoiceStatusBar,
    VoiceTranscript,
    VoiceVisualizer,
)
from entropic.voice.client import VoiceCallbacks, VoiceClient
from entropic.voice.client import VoiceStats as ClientStats
from entropic.voice.controller import VoiceState

if TYPE_CHECKING:
    from entropic.config.schema import VoiceConfig

logger = get_logger("ui.voice_screen")


@dataclass
class VoiceScreenCallbacks:
    """Callbacks for voice screen lifecycle events."""

    on_enter: Callable[[], Awaitable[None]] | None = None
    on_exit: Callable[[], Awaitable[None]] | None = None


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
            input_val = input_select.value
            output_val = output_select.value
            input_idx = int(input_val) if isinstance(input_val, int) and input_val != -1 else None
            output_idx = (
                int(output_val) if isinstance(output_val, int) and output_val != -1 else None
            )
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
        layout: grid;
        grid-size: 1;
        grid-rows: 1 8 auto 1fr 5 3;
        height: 100%;
        width: 100%;
    }

    VoiceHeader {
        row-span: 1;
    }

    #visualizer-container {
        row-span: 1;
        height: 100%;
        width: 100%;
        padding: 0 1;
    }

    #settings-container {
        row-span: 1;
        height: auto;
        width: 100%;
        padding: 0 1;
    }

    #transcript-container {
        row-span: 1;
        height: 100%;
        width: 100%;
        padding: 0 1;
        overflow-y: auto;
    }

    #transcript {
        height: auto;
        width: 100%;
        border: round $primary;
        padding: 1;
    }

    #status-bar {
        row-span: 1;
        height: 100%;
        width: 100%;
    }

    .hidden {
        display: none;
        height: 0;
    }
    """

    BINDINGS = [
        Binding("escape", "exit_voice", "Exit", show=True, priority=True),
        Binding("space", "toggle_pause", "Pause/Resume", show=True),
        Binding("enter", "start_conversation", "Start", show=True),
        Binding("r", "reset_context", "Reset", show=True),
        Binding("b", "back_to_settings", "Back", show=True),
    ]

    def __init__(
        self,
        config: VoiceConfig,
        callbacks: VoiceScreenCallbacks | None = None,
        **kwargs: str | None,
    ) -> None:
        """
        Initialize voice screen.

        Args:
            config: Voice configuration
            callbacks: Lifecycle callbacks (on_enter, on_exit)
            **kwargs: Standard Textual Screen kwargs (name, id, classes)
        """
        super().__init__(**kwargs)
        self._config = config
        cbs = callbacks or VoiceScreenCallbacks()
        self._enter_callback = cbs.on_enter
        self._exit_callback = cbs.on_exit
        self._client: VoiceClient | None = None
        self._start_time: float = 0
        self._elapsed_timer: asyncio.Task[None] | None = None
        self._initialized: bool = False
        self._conversation_started: bool = False

    def compose(self) -> ComposeResult:
        """Compose screen layout."""
        yield VoiceHeader(title="Voice Mode")
        yield Container(VoiceVisualizer(id="visualizer"), id="visualizer-container")
        yield Container(VoiceSettingsPanel(id="settings-panel"), id="settings-container")
        yield Container(VoiceTranscript(id="transcript"), id="transcript-container")
        yield VoiceStatsPanel(id="stats-panel")
        yield VoiceStatusBar(id="status-bar")

    async def on_mount(self) -> None:
        """Handle screen mount - start initialization."""
        # Get server config
        host = "127.0.0.1"
        port = 8765
        if hasattr(self._config, "server"):
            host = self._config.server.host
            port = self._config.server.port

        # Create client
        self._client = VoiceClient(
            config=self._config,
            host=host,
            port=port,
        )

        # Set up callbacks
        self._client.set_callbacks(
            VoiceCallbacks(
                on_text=lambda t: self._on_transcript_update("assistant", t),
                on_state_change=self._on_client_state_change,
                on_stats_update=self._on_client_stats_update,
                on_input_level=self._on_input_level,
                on_output_level=self._on_output_level,
                on_error=self._on_error,
                on_loading_progress=self._on_loading_progress,
            )
        )

        # Start initialization (but don't start conversation)
        # Note: _run_initialization is a @work decorated method, calling it starts the worker
        _ = self._run_initialization()

    @work(exclusive=True)
    async def _run_initialization(self) -> None:
        """Initialize voice server and audio devices (but don't start conversation)."""
        widgets = await self._get_initialization_widgets()
        if widgets is None:
            return

        status_bar, settings_panel, transcript = widgets
        status_bar.set_state("initializing")

        try:
            await self._do_initialization(settings_panel, transcript)

            # Detect audio devices
            settings_panel.set_status("Detecting audio devices...")
            transcript.write("[dim]Detecting audio devices...[/]")
            devices = await self._detect_audio_devices()
            settings_panel.set_audio_devices(devices)
            transcript.write(f"[green]Found {len(devices)} audio devices[/]")

            # Ready for user to start
            self._initialized = True
            settings_panel.set_status("[green]Ready - Press Enter or click Start[/green]")
            settings_panel.set_ready(True)
            status_bar.set_state("ready")

            transcript.write("[green bold]Ready! Press Enter to start conversation.[/]")
            transcript.write("[dim]Shortcuts: Space=Pause, R=Reset, B=Back, Esc=Exit[/]")

        except Exception as e:
            logger.exception(f"Voice initialization error: {e}")
            try:
                settings_panel.set_status(f"[red]Error: {e}[/red]")
                transcript.write(f"[red bold]ERROR: {e}[/]")
            except Exception:
                pass
            self.notify(f"Voice mode error: {e}", severity="error")

    async def _get_initialization_widgets(
        self,
    ) -> tuple[VoiceStatusBar, VoiceSettingsPanel, VoiceTranscript] | None:
        """Get widgets needed for initialization. Returns None on failure."""
        if self._client is None:
            logger.error("Client is None in _run_initialization")
            return None
        await asyncio.sleep(0.1)  # Give widgets time to mount
        try:
            return (
                self.query_one("#status-bar", VoiceStatusBar),
                self.query_one("#settings-panel", VoiceSettingsPanel),
                self.query_one("#transcript", VoiceTranscript),
            )
        except Exception as e:
            logger.error(f"Failed to query widgets: {e}")
            return None

    async def _do_initialization(
        self, settings_panel: VoiceSettingsPanel, transcript: VoiceTranscript
    ) -> None:
        """Perform the initialization steps. Raises on failure."""
        assert self._client is not None
        transcript.write("[yellow]Starting voice mode initialization...[/]")

        if self._enter_callback:
            settings_panel.set_status("Unloading chat models...")
            transcript.write("[dim]Unloading chat models to free GPU memory...[/]")
            await self._enter_callback()

        settings_panel.set_status("Starting voice server...")
        transcript.write("[yellow]Starting voice server (models load in subprocess)...[/]")
        if not await self._client.start_server():
            settings_panel.set_status("[red]Server startup failed[/red]")
            transcript.write("[red bold]ERROR: Failed to start voice server[/]")
            self.notify("Failed to start voice server", severity="error")
            raise RuntimeError("Failed to start voice server")

        settings_panel.set_status("Connecting to voice server...")
        transcript.write("[dim]Connecting to voice server...[/]")
        if not await self._client.connect():
            settings_panel.set_status("[red]Connection failed[/red]")
            transcript.write("[red bold]ERROR: Failed to connect to voice server[/]")
            self.notify("Failed to connect to voice server", severity="error")
            raise RuntimeError("Failed to connect to voice server")

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

        if self._client is None:
            return

        self._conversation_started = True

        # Hide settings panel
        try:
            settings_container = self.query_one("#settings-container", Container)
            settings_container.add_class("hidden")
        except Exception:
            pass

        # Get selected devices (for future device selection support)
        settings_panel = self.query_one("#settings-panel", VoiceSettingsPanel)
        _input_device, _output_device = settings_panel.get_selected_devices()

        # Start conversation timer
        self._start_time = time.time()
        self._elapsed_timer = asyncio.create_task(self._update_elapsed())

        status_bar = self.query_one("#status-bar", VoiceStatusBar)
        status_bar.set_state("conversation")

        # Initialize stats panel as connected
        try:
            stats_panel = self.query_one("#stats-panel", VoiceStatsPanel)
            stats_panel.connected = True
        except Exception:
            pass

        # Start audio + networking
        await self._client.start()

    def on_button_pressed(self, event: Button.Pressed) -> None:
        """Handle button presses."""
        if event.button.id == "start-btn":
            self.run_worker(self.action_start_conversation())
        elif event.button.id == "generate-thinking-btn":
            self.run_worker(self._generate_thinking_audio())

    async def _generate_thinking_audio(self) -> None:
        """Generate thinking audio file using current voice."""
        if not self._initialized or self._client is None:
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
        """Handle voice state changes from controller (enum)."""
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
        except Exception:
            pass

    def _on_client_state_change(self, state: str) -> None:
        """Handle voice state changes from client (string)."""
        try:
            status_bar = self.query_one("#status-bar", VoiceStatusBar)
            # Don't override "ready" state from initialization
            if self._initialized and not self._conversation_started:
                return
            status_bar.set_state(state)
        except Exception:
            pass

    def _on_transcript_update(self, speaker: str, text: str) -> None:
        """Handle transcript updates."""
        logger.debug(f"Transcript update: {speaker}: {text[:50]}...")
        try:
            transcript = self.query_one("#transcript", VoiceTranscript)
            transcript.add_text(speaker, text)
        except Exception as e:
            logger.warning(f"Failed to update transcript: {e}")

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
        logger.info(f"Loading progress: {message}")
        try:
            transcript = self.query_one("#transcript", VoiceTranscript)
            transcript.add_text("system", message)
        except Exception as e:
            logger.warning(f"Failed to update loading progress: {e}")

    def _on_client_stats_update(self, stats: ClientStats) -> None:
        """Handle real-time stats updates from the client."""
        try:
            stats_panel = self.query_one("#stats-panel", VoiceStatsPanel)
            stats_panel.update_stats(
                server_fps=stats.server_fps,
                server_latency_ms=stats.server_latency_ms,
                frames_sent=stats.frames_sent,
                frames_received=stats.frames_received,
                connected=True,
            )
        except Exception:
            pass

    async def action_exit_voice(self) -> None:
        """Exit voice mode."""
        await self._cleanup()
        self.app.pop_screen()

    async def action_toggle_pause(self) -> None:
        """Toggle pause/resume."""
        if self._client is None:
            self.notify("Client not ready", timeout=1)
            return

        if not self._conversation_started:
            self.notify("Start conversation first (Enter)", timeout=1)
            return

        # Note: Pause/resume not fully implemented in client/server architecture yet
        self.notify("Pause/resume coming soon", timeout=2)

    async def action_back_to_settings(self) -> None:
        """Return to settings screen (stop conversation if running)."""
        if self._conversation_started:
            # Stop the audio streaming
            if self._client:
                await self._client.stop()

            # Cancel elapsed timer
            if self._elapsed_timer:
                self._elapsed_timer.cancel()
                try:
                    await self._elapsed_timer
                except asyncio.CancelledError:
                    pass
                self._elapsed_timer = None

            self._conversation_started = False

            # Reset server state for next conversation
            if self._client:
                await self._client.reset()

        # Show settings panel again
        try:
            settings_container = self.query_one("#settings-container", Container)
            settings_container.remove_class("hidden")
            status_bar = self.query_one("#status-bar", VoiceStatusBar)
            status_bar.set_state("ready")
        except Exception:
            pass

        self.notify("Returned to settings", timeout=1)

    async def action_reset_context(self) -> None:
        """Reset context and transcript."""
        if self._client is None:
            return

        await self._client.reset()

        try:
            transcript = self.query_one("#transcript", VoiceTranscript)
            transcript.clear_transcript()
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

        if self._client:
            await self._client.stop()
            await self._client.stop_server()
            self._client = None

        # Call on_exit to reload chat models
        if self._exit_callback:
            try:
                await self._exit_callback()
            except Exception as e:
                logger.warning(f"Error in on_exit callback: {e}")

    async def on_unmount(self) -> None:
        """Handle screen unmount."""
        await self._cleanup()
