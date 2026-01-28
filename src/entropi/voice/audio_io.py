"""
Async audio I/O using sounddevice.

Provides buffered audio input/output for PersonaPlex integration
at 24kHz mono with 80ms frames (1920 samples).

Handles resampling when hardware doesn't support 24kHz natively.
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

# Audio configuration - target for PersonaPlex
TARGET_SAMPLE_RATE = 24000  # 24kHz for PersonaPlex
CHANNELS = 1  # Mono
FRAME_DURATION_MS = 80  # 80ms per frame
TARGET_FRAME_SIZE = int(TARGET_SAMPLE_RATE * FRAME_DURATION_MS / 1000)  # 1920 samples
DTYPE = np.float32

# Common sample rates to try (in order of preference)
SUPPORTED_SAMPLE_RATES = [24000, 48000, 44100, 96000, 16000]


def _resample(audio: NDArray[np.float32], from_rate: int, to_rate: int) -> NDArray[np.float32]:
    """
    Resample audio using linear interpolation.

    Args:
        audio: Input audio samples
        from_rate: Source sample rate
        to_rate: Target sample rate

    Returns:
        Resampled audio
    """
    if from_rate == to_rate:
        return audio

    # Calculate output length
    duration = len(audio) / from_rate
    out_len = int(duration * to_rate)

    # Create time arrays
    in_time = np.linspace(0, duration, len(audio), endpoint=False)
    out_time = np.linspace(0, duration, out_len, endpoint=False)

    # Interpolate
    return np.interp(out_time, in_time, audio).astype(np.float32)


class AudioIO:
    """
    Async audio I/O handler for voice interface.

    Provides non-blocking audio capture and playback using sounddevice,
    with frame buffering for PersonaPlex integration.

    Automatically handles resampling when hardware doesn't support 24kHz.
    """

    def __init__(
        self,
        target_sample_rate: int = TARGET_SAMPLE_RATE,
        channels: int = CHANNELS,
        frame_size: int = TARGET_FRAME_SIZE,
        buffer_frames: int = 10,
    ) -> None:
        """
        Initialize audio I/O.

        Args:
            target_sample_rate: Target sample rate for PersonaPlex (24kHz)
            channels: Number of audio channels
            frame_size: Samples per frame at target rate
            buffer_frames: Number of frames to buffer
        """
        self._target_sample_rate = target_sample_rate
        self._channels = channels
        self._target_frame_size = frame_size
        self._buffer_frames = buffer_frames

        # Actual device sample rate (determined at start)
        self._device_sample_rate: int = target_sample_rate
        self._device_frame_size: int = frame_size
        self._needs_resampling: bool = False

        # Import sounddevice lazily to allow graceful handling if unavailable
        self._sd: object | None = None

        # Buffers (contain frames at TARGET rate, i.e., 24kHz)
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
        """Get target sample rate (24kHz for PersonaPlex)."""
        return self._target_sample_rate

    @property
    def device_sample_rate(self) -> int:
        """Get actual device sample rate."""
        return self._device_sample_rate

    @property
    def frame_size(self) -> int:
        """Get frame size in samples at target rate."""
        return self._target_frame_size

    @property
    def frame_duration_ms(self) -> float:
        """Get frame duration in milliseconds."""
        return self._target_frame_size / self._target_sample_rate * 1000

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

    def _find_supported_sample_rate(self, sd: object) -> int:
        """Find a sample rate supported by the default audio device."""
        for rate in SUPPORTED_SAMPLE_RATES:
            try:
                sd.check_input_settings(samplerate=rate)
                sd.check_output_settings(samplerate=rate)
                return rate
            except Exception:
                continue

        # Fallback to device default
        try:
            device_info = sd.query_devices(kind='input')
            return int(device_info['default_samplerate'])
        except Exception:
            return 48000  # Common fallback

    async def start(self) -> None:
        """Start audio streams."""
        if self._running:
            return

        sd = self._ensure_sounddevice()

        # Find supported sample rate
        self._device_sample_rate = self._find_supported_sample_rate(sd)
        self._needs_resampling = self._device_sample_rate != self._target_sample_rate

        # Calculate device frame size to match target frame duration
        self._device_frame_size = int(
            self._device_sample_rate * FRAME_DURATION_MS / 1000
        )

        if self._needs_resampling:
            logger.info(
                f"Device sample rate {self._device_sample_rate}Hz, "
                f"will resample to {self._target_sample_rate}Hz"
            )

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

            # Resample to target rate if needed
            if self._needs_resampling:
                frame = _resample(frame, self._device_sample_rate, self._target_sample_rate)

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
            samplerate=self._device_sample_rate,
            channels=self._channels,
            dtype=DTYPE,
            blocksize=self._device_frame_size,
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

                # Resample from target rate to device rate if needed
                if self._needs_resampling:
                    frame = _resample(frame, self._target_sample_rate, self._device_sample_rate)

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
            samplerate=self._device_sample_rate,
            channels=self._channels,
            dtype=DTYPE,
            blocksize=self._device_frame_size,
            callback=output_callback,
        )
        self._output_stream.start()

        logger.info(
            f"Audio I/O started: device={self._device_sample_rate}Hz, "
            f"target={self._target_sample_rate}Hz, "
            f"resampling={self._needs_resampling}"
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
