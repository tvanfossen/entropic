"""
Async audio I/O using sounddevice.

Provides buffered audio input/output for PersonaPlex integration
at 24kHz mono with 80ms frames (1920 samples).
"""

from __future__ import annotations

import asyncio
from collections.abc import Callable
from typing import TYPE_CHECKING

import numpy as np

from entropi.core.logging import get_logger

if TYPE_CHECKING:
    from numpy.typing import NDArray

logger = get_logger("voice.audio_io")

# Audio configuration
SAMPLE_RATE = 24000  # 24kHz for PersonaPlex
CHANNELS = 1  # Mono
FRAME_DURATION_MS = 80  # 80ms per frame
SAMPLES_PER_FRAME = int(SAMPLE_RATE * FRAME_DURATION_MS / 1000)  # 1920 samples
DTYPE = np.float32


class AudioIO:
    """
    Async audio I/O handler for voice interface.

    Provides non-blocking audio capture and playback using sounddevice,
    with frame buffering for PersonaPlex integration.
    """

    def __init__(
        self,
        sample_rate: int = SAMPLE_RATE,
        channels: int = CHANNELS,
        frame_size: int = SAMPLES_PER_FRAME,
        buffer_frames: int = 10,
    ) -> None:
        """
        Initialize audio I/O.

        Args:
            sample_rate: Audio sample rate in Hz
            channels: Number of audio channels
            frame_size: Samples per frame
            buffer_frames: Number of frames to buffer
        """
        self._sample_rate = sample_rate
        self._channels = channels
        self._frame_size = frame_size
        self._buffer_frames = buffer_frames

        # Import sounddevice lazily to allow graceful handling if unavailable
        self._sd: object | None = None

        # Buffers
        self._input_buffer: asyncio.Queue[NDArray[np.float32]] = asyncio.Queue(
            maxsize=buffer_frames
        )
        self._output_buffer: asyncio.Queue[NDArray[np.float32]] = asyncio.Queue(
            maxsize=buffer_frames
        )

        # Stream handles
        self._input_stream: object | None = None
        self._output_stream: object | None = None
        self._running = False

        # Callbacks for audio level monitoring
        self._on_input_level: Callable[[float], None] | None = None
        self._on_output_level: Callable[[float], None] | None = None

    def _ensure_sounddevice(self) -> object:
        """Import and return sounddevice module."""
        if self._sd is None:
            try:
                import sounddevice as sd

                self._sd = sd
            except ImportError as e:
                raise ImportError(
                    "sounddevice is required for voice mode. "
                    "Install with: pip install entropi[voice]"
                ) from e
        return self._sd

    @property
    def sample_rate(self) -> int:
        """Get sample rate."""
        return self._sample_rate

    @property
    def frame_size(self) -> int:
        """Get frame size in samples."""
        return self._frame_size

    @property
    def frame_duration_ms(self) -> float:
        """Get frame duration in milliseconds."""
        return self._frame_size / self._sample_rate * 1000

    def set_level_callbacks(
        self,
        on_input: Callable[[float], None] | None = None,
        on_output: Callable[[float], None] | None = None,
    ) -> None:
        """
        Set callbacks for audio level monitoring.

        Args:
            on_input: Callback for input level (0.0-1.0)
            on_output: Callback for output level (0.0-1.0)
        """
        self._on_input_level = on_input
        self._on_output_level = on_output

    async def start(self) -> None:
        """Start audio streams."""
        if self._running:
            return

        sd = self._ensure_sounddevice()
        self._running = True

        # Create input stream
        def input_callback(
            indata: NDArray[np.float32],
            frames: int,
            time_info: object,
            status: object,
        ) -> None:
            _ = frames, time_info
            if status:
                logger.warning(f"Input stream status: {status}")

            # Copy frame data
            frame = indata[:, 0].copy() if indata.ndim > 1 else indata.copy()

            # Calculate RMS level for visualization
            if self._on_input_level:
                rms = float(np.sqrt(np.mean(frame**2)))
                self._on_input_level(min(1.0, rms * 5))  # Scale for visibility

            # Add to buffer (non-blocking)
            try:
                self._input_buffer.put_nowait(frame)
            except asyncio.QueueFull:
                pass  # Drop frame if buffer full

        self._input_stream = sd.InputStream(
            samplerate=self._sample_rate,
            channels=self._channels,
            dtype=DTYPE,
            blocksize=self._frame_size,
            callback=input_callback,
        )
        self._input_stream.start()

        # Create output stream
        def output_callback(
            outdata: NDArray[np.float32],
            frames: int,
            time_info: object,
            status: object,
        ) -> None:
            _ = frames, time_info
            if status:
                logger.warning(f"Output stream status: {status}")

            try:
                frame = self._output_buffer.get_nowait()
                if outdata.ndim > 1:
                    outdata[:, 0] = frame[: len(outdata)]
                else:
                    outdata[:] = frame[: len(outdata)]

                # Calculate RMS level for visualization
                if self._on_output_level:
                    rms = float(np.sqrt(np.mean(frame**2)))
                    self._on_output_level(min(1.0, rms * 5))
            except asyncio.QueueEmpty:
                outdata.fill(0)  # Silence if no data

        self._output_stream = sd.OutputStream(
            samplerate=self._sample_rate,
            channels=self._channels,
            dtype=DTYPE,
            blocksize=self._frame_size,
            callback=output_callback,
        )
        self._output_stream.start()

        logger.info(
            f"Audio I/O started: {self._sample_rate}Hz, "
            f"{self._frame_size} samples/frame ({self.frame_duration_ms:.0f}ms)"
        )

    async def stop(self) -> None:
        """Stop audio streams."""
        if not self._running:
            return

        self._running = False

        if self._input_stream:
            self._input_stream.stop()
            self._input_stream.close()
            self._input_stream = None

        if self._output_stream:
            self._output_stream.stop()
            self._output_stream.close()
            self._output_stream = None

        # Clear buffers
        while not self._input_buffer.empty():
            try:
                self._input_buffer.get_nowait()
            except asyncio.QueueEmpty:
                break

        while not self._output_buffer.empty():
            try:
                self._output_buffer.get_nowait()
            except asyncio.QueueEmpty:
                break

        logger.info("Audio I/O stopped")

    async def read_frame(self, timeout: float = 0.2) -> NDArray[np.float32] | None:
        """
        Read an audio frame from input.

        Args:
            timeout: Maximum time to wait in seconds

        Returns:
            Audio frame as float32 array, or None if timeout
        """
        if not self._running:
            return None

        try:
            return await asyncio.wait_for(self._input_buffer.get(), timeout=timeout)
        except asyncio.TimeoutError:
            return None

    async def write_frame(self, frame: NDArray[np.float32]) -> bool:
        """
        Write an audio frame to output.

        Args:
            frame: Audio frame as float32 array

        Returns:
            True if written, False if buffer full
        """
        if not self._running:
            return False

        try:
            self._output_buffer.put_nowait(frame)
            return True
        except asyncio.QueueFull:
            return False

    async def play_clip(self, audio: NDArray[np.float32]) -> None:
        """
        Play an audio clip (blocking until complete).

        Args:
            audio: Audio data as float32 array
        """
        if not self._running:
            await self.start()

        # Split into frames and queue
        num_frames = len(audio) // self._frame_size
        for i in range(num_frames):
            start = i * self._frame_size
            end = start + self._frame_size
            frame = audio[start:end]
            await self.write_frame(frame)
            # Small delay to allow playback
            await asyncio.sleep(self.frame_duration_ms / 1000 * 0.9)

        # Handle remaining samples
        remainder = len(audio) % self._frame_size
        if remainder > 0:
            # Pad final frame with zeros
            final_frame = np.zeros(self._frame_size, dtype=DTYPE)
            final_frame[:remainder] = audio[-remainder:]
            await self.write_frame(final_frame)
            await asyncio.sleep(self.frame_duration_ms / 1000)

    def is_running(self) -> bool:
        """Check if audio I/O is running."""
        return self._running
