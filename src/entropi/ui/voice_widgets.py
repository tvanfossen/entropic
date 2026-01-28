"""
Voice interface UI widgets.

Provides ASCII art visualization, transcript display, and status widgets
for the voice conversation mode.
"""

from __future__ import annotations

from collections import deque
from enum import Enum, auto
from typing import ClassVar

from textual.app import ComposeResult
from textual.reactive import reactive
from textual.timer import Timer
from textual.widgets import Static


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
        if level == AudioLevel.SILENT:
            return cls.IDLE
        elif level == AudioLevel.LOW:
            return cls.LOW
        elif level == AudioLevel.MEDIUM:
            return cls.MEDIUM
        else:
            return cls.HIGH


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
        self._timer = self.set_interval(0.1, self._animate)

    def _animate(self) -> None:
        """Advance animation frame."""
        self._frame_index = (self._frame_index + 1) % 4
        self._update_display()

    def _get_audio_level(self) -> AudioLevel:
        """Convert numeric level to category."""
        if self.level < 0.05:
            return AudioLevel.SILENT
        elif self.level < 0.3:
            return AudioLevel.LOW
        elif self.level < 0.6:
            return AudioLevel.MEDIUM
        else:
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


class VoiceTranscript(Static):
    """
    Live transcript display with speaker attribution.

    Shows recent conversation with speaker labels and scrolling.
    """

    DEFAULT_CSS = """
    VoiceTranscript {
        height: 100%;
        width: 100%;
        border: round $surface;
        padding: 0 1;
        overflow-y: auto;
    }
    """

    def __init__(
        self,
        max_lines: int = 20,
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
        super().__init__(name=name, id=id, classes=classes)
        self._max_lines = max_lines
        self._lines: deque[tuple[str, str]] = deque(maxlen=max_lines)
        self._current_speaker: str | None = None
        self._current_text: str = ""

    def add_text(self, speaker: str, text: str) -> None:
        """
        Add text to transcript.

        Args:
            speaker: Speaker label ("user" or "assistant")
            text: Text to add
        """
        if speaker != self._current_speaker:
            # Flush current line
            if self._current_text:
                self._lines.append((self._current_speaker or "unknown", self._current_text))
            self._current_speaker = speaker
            self._current_text = text
        else:
            self._current_text += text

        self._update_display()

    def _update_display(self) -> None:
        """Update the display with current transcript."""
        lines = []

        def get_speaker_style(speaker: str) -> tuple[str, str]:
            """Get color and label for speaker."""
            if speaker == "user":
                return "cyan", "You"
            elif speaker == "system":
                return "yellow", "System"
            else:
                return "green", "AI"

        for speaker, text in self._lines:
            color, label = get_speaker_style(speaker)
            # Truncate long lines
            if len(text) > 60:
                text = text[:57] + "..."
            lines.append(f"[{color}]{label}:[/] {text}")

        # Add current in-progress line
        if self._current_text:
            color, label = get_speaker_style(self._current_speaker or "assistant")
            text = self._current_text
            if len(text) > 60:
                text = text[:57] + "..."
            lines.append(f"[{color}]{label}:[/] {text}[dim]|[/]")

        self.update("\n".join(lines) if lines else "[dim]Listening...[/]")

    def clear(self) -> None:
        """Clear transcript."""
        self._lines.clear()
        self._current_speaker = None
        self._current_text = ""
        self._update_display()


class VoiceStatusBar(Static):
    """
    Status bar for voice mode.

    Shows current state, window number, elapsed time, and hints.
    """

    DEFAULT_CSS = """
    VoiceStatusBar {
        height: 1;
        width: 100%;
        background: $surface;
        padding: 0 1;
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
            "[dim]Esc[/]=Exit [dim]Space[/]=Pause [dim]R[/]=Reset",
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


class VoiceHeader(Static):
    """Header for voice mode screen."""

    DEFAULT_CSS = """
    VoiceHeader {
        height: 3;
        width: 100%;
        text-align: center;
        background: $primary;
        color: $text;
        padding: 1;
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
        self._title = title

    def compose(self) -> ComposeResult:
        """Compose header content."""
        yield Static(f"[bold]{self._title}[/] - Speak naturally")
