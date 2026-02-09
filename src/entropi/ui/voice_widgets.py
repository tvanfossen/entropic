"""
Voice interface UI widgets.

Provides ASCII art visualization, transcript display, and status widgets
for the voice conversation mode.
"""

from __future__ import annotations

from enum import Enum, auto
from typing import ClassVar

from textual.reactive import reactive
from textual.timer import Timer
from textual.widgets import RichLog, Static


class AudioLevel(Enum):
    """Audio level categories for visualization."""

    SILENT = auto()
    LOW = auto()
    MEDIUM = auto()
    HIGH = auto()


class VoiceVisualizerFrames:
    """ASCII art animation frames for different audio levels."""

    # Idle state - subtle breathing animation
    IDLE: ClassVar[list[str]] = [
        "          .  .  .  .  .          ",
        "         .  .  .  .  .           ",
        "          .  .  .  .  .          ",
        "           .  .  .  .  .         ",
    ]

    # Low audio - gentle wave
    LOW: ClassVar[list[str]] = [
        "      _.-~          ~-._         ",
        "   .-~                  ~-.      ",
        "  /                        \\     ",
        " |                          |    ",
    ]

    # Medium audio - dynamic zigzag
    MEDIUM: ClassVar[list[str]] = [
        "    /\\    /\\    /\\    /\\    /\\   ",
        "   /  \\  /  \\  /  \\  /  \\  /  \\  ",
        "  /    \\/    \\/    \\/    \\/    \\ ",
        " /                              \\",
    ]

    # High audio - energetic blocks
    HIGH: ClassVar[list[str]] = [
        " ██▄▀█▄██▀▄█▀██▄▀█▄██▀▄█▀██▄▀█▄ ",
        " █▀▄██▀▄█▄▀██▄█▀▄██▀▄█▄▀██▄█▀▄█ ",
        " ▄█▀▄██▀▄█▄▀▄█▀▄██▀▄█▄▀▄█▀▄██▀▄ ",
        " █▄▀██▄▀█▀▄██▀█▄▀██▄▀█▀▄██▀█▄▀█ ",
    ]

    @classmethod
    def get_frames(cls, level: AudioLevel) -> list[str]:
        """Get animation frames for an audio level."""
        frames_map = {
            AudioLevel.SILENT: cls.IDLE,
            AudioLevel.LOW: cls.LOW,
            AudioLevel.MEDIUM: cls.MEDIUM,
            AudioLevel.HIGH: cls.HIGH,
        }
        return frames_map.get(level, cls.HIGH)


class VoiceVisualizer(Static):
    """
    Animated ASCII art visualizer for voice input/output.

    Responds to audio levels with different animation styles.
    """

    DEFAULT_CSS = """
    VoiceVisualizer {
        height: 6;
        width: 100%;
        text-align: center;
        border: round $primary;
        padding: 0 1;
    }

    VoiceVisualizer.input {
        border: round cyan;
    }

    VoiceVisualizer.output {
        border: round green;
    }
    """

    # Reactive properties
    level: reactive[float] = reactive(0.0)
    mode: reactive[str] = reactive("idle")  # "idle", "input", "output"

    def __init__(
        self,
        name: str | None = None,
        id: str | None = None,
        classes: str | None = None,
    ) -> None:
        """Initialize voice visualizer."""
        super().__init__(name=name, id=id, classes=classes)
        self._frame_index = 0
        self._timer: Timer | None = None

    def on_mount(self) -> None:
        """Start animation timer on mount."""
        self._timer = self.set_interval(0.1, self._advance_frame)

    def _advance_frame(self) -> None:
        """Advance animation frame."""
        self._frame_index = (self._frame_index + 1) % 4
        self._update_display()

    def _get_audio_level(self) -> AudioLevel:
        """Convert numeric level to category."""
        thresholds = [(0.05, AudioLevel.SILENT), (0.3, AudioLevel.LOW), (0.6, AudioLevel.MEDIUM)]
        for threshold, level in thresholds:
            if self.level < threshold:
                return level
        return AudioLevel.HIGH

    def _update_display(self) -> None:
        """Update the display with current animation frame."""
        level = self._get_audio_level()
        frames = VoiceVisualizerFrames.get_frames(level)
        frame = frames[self._frame_index % len(frames)]

        # Add color based on mode
        if self.mode == "input":
            frame = f"[cyan]{frame}[/]"
        elif self.mode == "output":
            frame = f"[green]{frame}[/]"
        else:
            frame = f"[dim]{frame}[/]"

        self.update(frame)

    def set_level(self, level: float, mode: str = "input") -> None:
        """
        Set audio level for visualization.

        Args:
            level: Audio level (0.0-1.0)
            mode: "input", "output", or "idle"
        """
        self.level = max(0.0, min(1.0, level))
        self.mode = mode

        # Update CSS class
        self.remove_class("input", "output")
        if mode in ("input", "output"):
            self.add_class(mode)


class VoiceTranscript(RichLog):
    """
    Live transcript display with speaker attribution.

    Uses RichLog for proper scrolling and live updates.
    """

    DEFAULT_CSS = """
    VoiceTranscript {
        height: 100%;
        width: 100%;
        border: round $primary;
        background: $surface;
        scrollbar-gutter: stable;
    }
    """

    def __init__(
        self,
        max_lines: int = 100,
        name: str | None = None,
        id: str | None = None,
        classes: str | None = None,
    ) -> None:
        """
        Initialize transcript display.

        Args:
            max_lines: Maximum lines to keep in buffer
            name: Widget name
            id: Widget ID
            classes: CSS classes
        """
        super().__init__(
            max_lines=max_lines,
            highlight=True,
            markup=True,
            wrap=True,
            name=name,
            id=id,
            classes=classes,
        )
        self._current_speaker: str | None = None
        self._pending_text: str = ""

    def on_mount(self) -> None:
        """Initialize display on mount."""
        self.write("[dim]Initializing voice mode...[/]")

    def add_text(self, speaker: str, text: str) -> None:
        """
        Add text to transcript.

        Accumulates tokens and writes when hitting punctuation or reaching
        a threshold to avoid one-word-per-line display.

        Args:
            speaker: Speaker label ("user", "assistant", "system")
            text: Text to add
        """
        # Handle speaker change - flush pending text and start new
        if speaker != self._current_speaker:
            # Flush any pending text from previous speaker
            if self._pending_text and self._current_speaker:
                self._flush_pending()
            self._current_speaker = speaker
            self._pending_text = text
        else:
            # Accumulate text
            self._pending_text += text

        # Flush on sentence-ending punctuation or long enough text
        if any(p in text for p in ".!?") or len(self._pending_text) > 80:
            self._flush_pending()

    def _flush_pending(self) -> None:
        """Write pending text to display."""
        if not self._pending_text.strip():
            return

        if self._current_speaker == "user":
            color, label = "cyan", "You"
        elif self._current_speaker == "system":
            color, label = "yellow", "System"
        else:
            color, label = "green", "AI"

        self.write(f"[{color} bold]{label}:[/] {self._pending_text.strip()}")
        self._pending_text = ""

    def flush(self) -> None:
        """Flush any pending text to display."""
        self._flush_pending()

    def clear_transcript(self) -> None:
        """Clear transcript."""
        self._pending_text = ""
        super().clear()
        self._current_speaker = None
        self.write("[dim]Transcript cleared[/]")


class VoiceStatusBar(Static):
    """
    Status bar for voice mode.

    Shows current state, window number, elapsed time, and hints.
    """

    DEFAULT_CSS = """
    VoiceStatusBar {
        height: 3;
        width: 100%;
        background: $surface;
        padding: 1;
        border-top: solid $primary;
    }
    """

    # Reactive properties
    state: reactive[str] = reactive("idle")
    window: reactive[int] = reactive(0)
    elapsed: reactive[float] = reactive(0.0)

    def __init__(
        self,
        name: str | None = None,
        id: str | None = None,
        classes: str | None = None,
    ) -> None:
        """Initialize status bar."""
        super().__init__(name=name, id=id, classes=classes)

    def watch_state(self, state: str) -> None:
        """React to state changes."""
        self._update_display()

    def watch_window(self, window: int) -> None:
        """React to window changes."""
        self._update_display()

    def watch_elapsed(self, elapsed: float) -> None:
        """React to elapsed time changes."""
        self._update_display()

    def _update_display(self) -> None:
        """Update status bar display."""
        # State indicator
        state_colors = {
            "idle": ("dim", "Idle"),
            "initializing": ("yellow", "Loading..."),
            "ready": ("cyan", "Ready"),
            "conversation": ("green", "Listening"),
            "thinking": ("yellow", "Thinking..."),
            "compacting": ("cyan", "Processing"),
            "error": ("red", "Error"),
        }
        color, label = state_colors.get(self.state, ("dim", self.state))

        # Format elapsed time
        mins = int(self.elapsed // 60)
        secs = int(self.elapsed % 60)
        time_str = f"{mins}:{secs:02d}"

        # Build status line
        parts = [
            f"[{color}]{label}[/]",
            f"Window: {self.window}" if self.window > 0 else "",
            f"Time: {time_str}",
            "[dim]Esc[/]=Exit [dim]Space[/]=Pause [dim]R[/]=Reset [dim]B[/]=Back",
        ]

        self.update(" | ".join(p for p in parts if p))

    def set_state(self, state: str) -> None:
        """Set current state."""
        self.state = state

    def set_window(self, window: int) -> None:
        """Set current window number."""
        self.window = window

    def set_elapsed(self, elapsed: float) -> None:
        """Set elapsed time in seconds."""
        self.elapsed = elapsed

    def update_stats(self, window_number: int, frames_processed: int) -> None:
        """Update window statistics."""
        self.window = window_number
        self._update_display()


class VoiceHeader(Static):
    """Header for voice mode screen."""

    DEFAULT_CSS = """
    VoiceHeader {
        height: 1;
        width: 100%;
        text-align: center;
        background: $primary;
        color: $text;
    }
    """

    def __init__(
        self,
        title: str = "Voice Mode",
        name: str | None = None,
        id: str | None = None,
        classes: str | None = None,
    ) -> None:
        """Initialize header."""
        super().__init__(name=name, id=id, classes=classes)
        self.update(f"[bold]{title}[/] - Speak naturally")


class VoiceStatsPanel(Static):
    """
    Real-time stats panel for voice mode.

    Shows server FPS, latency, frames processed, and connection status.
    """

    DEFAULT_CSS = """
    VoiceStatsPanel {
        height: 5;
        width: 100%;
        background: $surface;
        padding: 0 1;
        border-top: solid $primary;
    }
    """

    # Reactive stats
    server_fps: reactive[float] = reactive(0.0)
    server_latency: reactive[float] = reactive(0.0)
    frames_sent: reactive[int] = reactive(0)
    frames_received: reactive[int] = reactive(0)
    connected: reactive[bool] = reactive(False)

    def __init__(
        self,
        name: str | None = None,
        id: str | None = None,
        classes: str | None = None,
    ) -> None:
        """Initialize stats panel."""
        super().__init__(name=name, id=id, classes=classes)

    def watch_server_fps(self, fps: float) -> None:
        self._update_display()

    def watch_server_latency(self, latency: float) -> None:
        self._update_display()

    def watch_frames_sent(self, frames: int) -> None:
        self._update_display()

    def watch_frames_received(self, frames: int) -> None:
        self._update_display()

    def watch_connected(self, connected: bool) -> None:
        self._update_display()

    def _update_display(self) -> None:
        """Update stats display."""
        if not self.connected:
            self.update("[dim]Disconnected[/]")
            return

        # Color-code latency
        if self.server_latency < 50:
            lat_color = "green"
        elif self.server_latency < 100:
            lat_color = "yellow"
        else:
            lat_color = "red"

        # Color-code FPS (target is 12.5 fps)
        if self.server_fps >= 12:
            fps_color = "green"
        elif self.server_fps >= 10:
            fps_color = "yellow"
        else:
            fps_color = "red"

        lines = [
            "[bold]Inference Stats[/]",
            f"  FPS: [{fps_color}]{self.server_fps:.1f}[/]  |  "
            f"Avg Latency: [{lat_color}]{self.server_latency:.0f}ms[/]",
            f"  Frames Processed: {self.frames_sent}  |  Status: [green]Running[/]",
        ]
        self.update("\n".join(lines))

    def update_stats(
        self,
        server_fps: float = 0.0,
        server_latency_ms: float = 0.0,
        frames_sent: int = 0,
        frames_received: int = 0,
        connected: bool = True,
    ) -> None:
        """Update all stats at once."""
        self.connected = connected
        self.server_fps = server_fps
        self.server_latency = server_latency_ms
        self.frames_sent = frames_sent
        self.frames_received = frames_received
