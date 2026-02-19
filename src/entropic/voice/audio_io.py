"""
Async audio I/O using sounddevice or PulseAudio subprocess.

Provides buffered audio input/output for PersonaPlex integration
at 24kHz mono with 80ms frames (1920 samples).

Handles resampling when hardware doesn't support 24kHz natively.

In Docker environments, uses parecord/paplay subprocess for reliable
PulseAudio access (PortAudio's PulseAudio backend has recording issues).
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
from collections.abc import Callable
from typing import TYPE_CHECKING, Any

import numpy as np

from entropic.core.logging import get_logger

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


def _resample_linear(
    audio: NDArray[np.float32], from_rate: int, to_rate: int
) -> NDArray[np.float32]:
    """
    Resample audio using linear interpolation (fallback method).

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


def _resample(audio: NDArray[np.float32], from_rate: int, to_rate: int) -> NDArray[np.float32]:
    """
    Resample audio using scipy's polyphase resampler (fast, high quality).

    Falls back to linear interpolation if scipy is unavailable.

    Args:
        audio: Input audio samples
        from_rate: Source sample rate
        to_rate: Target sample rate

    Returns:
        Resampled audio
    """
    if from_rate == to_rate:
        return audio

    try:
        from math import gcd

        from scipy.signal import resample_poly

        # Use polyphase resampling for efficiency
        # resample_poly(x, up, down) resamples by factor up/down
        g = gcd(from_rate, to_rate)
        up = to_rate // g
        down = from_rate // g

        resampled: NDArray[np.float32] = resample_poly(audio, up, down).astype(np.float32)
        return resampled

    except ImportError:
        # Fallback to linear interpolation if scipy unavailable
        logger.debug("scipy not available, using linear interpolation for resampling")
        return _resample_linear(audio, from_rate, to_rate)


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
        self._sd: Any = None

        # Buffers (contain frames at TARGET rate, i.e., 24kHz)
        self._input_buffer: asyncio.Queue[NDArray[np.float32]] = asyncio.Queue(
            maxsize=buffer_frames
        )
        self._output_buffer: asyncio.Queue[NDArray[np.float32]] = asyncio.Queue(
            maxsize=buffer_frames
        )

        # Stream handles
        self._input_stream: Any = None
        self._output_stream: Any = None
        self._running = False

        # Callbacks for audio level monitoring
        self._on_input_level: Callable[[float], None] | None = None
        self._on_output_level: Callable[[float], None] | None = None

    def _ensure_sounddevice(self) -> Any:
        """Import and return sounddevice module."""
        if self._sd is None:
            try:
                import sounddevice as sd

                self._sd = sd
            except ImportError as e:
                raise ImportError(
                    "sounddevice is required for voice mode. "
                    "Install with: pip install entropic[voice]"
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

    def _find_supported_sample_rate(self, sd: Any) -> int:
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
            device_info = sd.query_devices(kind="input")
            return int(device_info["default_samplerate"])
        except Exception:
            return 48000  # Common fallback

    async def start(self) -> None:
        """Start audio streams."""
        if self._running:
            return

        sd = self._ensure_sounddevice()
        self._configure_sample_rates(sd)
        self._running = True
        self._start_input_stream(sd)
        self._start_output_stream(sd)
        self._log_start_info()

    def _configure_sample_rates(self, sd: Any) -> None:
        """Configure sample rates and frame sizes."""
        self._device_sample_rate = self._find_supported_sample_rate(sd)
        self._needs_resampling = self._device_sample_rate != self._target_sample_rate
        self._device_frame_size = int(self._device_sample_rate * FRAME_DURATION_MS / 1000)
        if self._needs_resampling:
            logger.info(
                f"Device sample rate {self._device_sample_rate}Hz, "
                f"will resample to {self._target_sample_rate}Hz"
            )

    def _start_input_stream(self, sd: Any) -> None:
        """Create and start input stream."""
        self._input_stream = sd.InputStream(
            samplerate=self._device_sample_rate,
            channels=self._channels,
            dtype=DTYPE,
            blocksize=self._device_frame_size,
            callback=self._make_input_callback(),
        )
        self._input_stream.start()

    def _make_input_callback(self) -> Callable[..., None]:
        """Create input stream callback."""

        def callback(
            indata: NDArray[np.float32], frames: int, time_info: object, status: object
        ) -> None:
            _ = frames, time_info
            if status:
                logger.warning(f"Input stream status: {status}")
            frame = indata[:, 0].copy() if indata.ndim > 1 else indata.copy()
            if self._needs_resampling:
                frame = _resample(frame, self._device_sample_rate, self._target_sample_rate)
            self._report_input_level(frame)
            self._queue_input_frame(frame)

        return callback

    def _report_input_level(self, frame: NDArray[np.float32]) -> None:
        """Report input level to callback if set."""
        if self._on_input_level:
            rms = float(np.sqrt(np.mean(frame**2)))
            self._on_input_level(min(1.0, rms * 5))

    def _queue_input_frame(self, frame: NDArray[np.float32]) -> None:
        """Queue input frame, dropping if buffer full."""
        try:
            self._input_buffer.put_nowait(frame)
        except asyncio.QueueFull:
            pass

    def _start_output_stream(self, sd: Any) -> None:
        """Create and start output stream."""
        self._output_stream = sd.OutputStream(
            samplerate=self._device_sample_rate,
            channels=self._channels,
            dtype=DTYPE,
            blocksize=self._device_frame_size,
            callback=self._make_output_callback(),
        )
        self._output_stream.start()

    def _make_output_callback(self) -> Callable[..., None]:
        """Create output stream callback."""

        def callback(
            outdata: NDArray[np.float32], frames: int, time_info: object, status: object
        ) -> None:
            _ = frames, time_info
            if status:
                logger.warning(f"Output stream status: {status}")
            self._fill_output_data(outdata)

        return callback

    def _fill_output_data(self, outdata: NDArray[np.float32]) -> None:
        """Fill output buffer with audio data."""
        try:
            frame = self._output_buffer.get_nowait()
            if self._needs_resampling:
                frame = _resample(frame, self._target_sample_rate, self._device_sample_rate)
            if outdata.ndim > 1:
                outdata[:, 0] = frame[: len(outdata)]
            else:
                outdata[:] = frame[: len(outdata)]
            self._report_output_level(frame)
        except asyncio.QueueEmpty:
            outdata.fill(0)

    def _report_output_level(self, frame: NDArray[np.float32]) -> None:
        """Report output level to callback if set."""
        if self._on_output_level:
            rms = float(np.sqrt(np.mean(frame**2)))
            self._on_output_level(min(1.0, rms * 5))

    def _log_start_info(self) -> None:
        """Log audio I/O start info."""
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
        except TimeoutError:
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
            logger.warning(f"write_frame called but not running, frame size={len(frame)}")
            return False

        try:
            self._output_buffer.put_nowait(frame)
            logger.info(
                f"write_frame: queued {len(frame)} samples, queue size={self._output_buffer.qsize()}"
            )
            return True
        except asyncio.QueueFull:
            logger.warning(f"write_frame: buffer full, dropped {len(frame)} samples")
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
        num_frames = len(audio) // self._target_frame_size
        for i in range(num_frames):
            start = i * self._target_frame_size
            end = start + self._target_frame_size
            frame = audio[start:end]
            await self.write_frame(frame)
            # Small delay to allow playback
            await asyncio.sleep(self.frame_duration_ms / 1000 * 0.9)

        # Handle remaining samples
        remainder = len(audio) % self._target_frame_size
        if remainder > 0:
            # Pad final frame with zeros
            final_frame = np.zeros(self._target_frame_size, dtype=DTYPE)
            final_frame[:remainder] = audio[-remainder:]
            await self.write_frame(final_frame)
            await asyncio.sleep(self.frame_duration_ms / 1000)

    def is_running(self) -> bool:
        """Check if audio I/O is running."""
        return self._running


class PulseAudioIO:
    """
    Async audio I/O using PulseAudio subprocess (parecord/paplay).

    Used in Docker environments where PortAudio's PulseAudio backend
    has issues with recording. Uses parecord for input and paplay for
    output via subprocess pipes.
    """

    def __init__(
        self,
        target_sample_rate: int = TARGET_SAMPLE_RATE,
        channels: int = CHANNELS,
        frame_size: int = TARGET_FRAME_SIZE,
        buffer_frames: int = 10,
        input_gain: float = 15.0,
    ) -> None:
        """Initialize PulseAudio I/O.

        Args:
            input_gain: Gain applied to microphone input (default 15x to compensate
                for typically low hardware mic levels).
        """
        self._target_sample_rate = target_sample_rate
        self._channels = channels
        self._target_frame_size = frame_size
        self._buffer_frames = buffer_frames
        self._input_gain = input_gain

        # PulseAudio works at 48kHz, we resample to 24kHz
        self._device_sample_rate = 48000
        self._device_frame_size = int(48000 * FRAME_DURATION_MS / 1000)
        self._needs_resampling = True

        # Buffers (contain frames at TARGET rate, i.e., 24kHz)
        self._input_buffer: asyncio.Queue[NDArray[np.float32]] = asyncio.Queue(
            maxsize=buffer_frames
        )
        self._output_buffer: asyncio.Queue[NDArray[np.float32]] = asyncio.Queue(
            maxsize=buffer_frames
        )

        # Subprocess handles
        self._record_proc: subprocess.Popen[bytes] | None = None
        self._play_proc: subprocess.Popen[bytes] | None = None
        self._running = False

        # Background tasks
        self._input_task: asyncio.Task[None] | None = None
        self._output_task: asyncio.Task[None] | None = None

        # Callbacks for audio level monitoring
        self._on_input_level: Callable[[float], None] | None = None
        self._on_output_level: Callable[[float], None] | None = None

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
        """Set callbacks for audio level monitoring."""
        self._on_input_level = on_input
        self._on_output_level = on_output

    async def start(self) -> None:
        """Start audio streams via PulseAudio subprocess."""
        if self._running:
            return

        self._running = True

        # Start parecord subprocess for input
        # Output raw 16-bit signed LE samples to stdout
        self._record_proc = subprocess.Popen(
            [
                "parecord",
                "--raw",
                "--format=s16le",
                "--rate=48000",
                "--channels=1",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

        # Start paplay subprocess for output
        # Read raw 16-bit signed LE samples from stdin
        self._play_proc = subprocess.Popen(
            [
                "paplay",
                "--raw",
                "--format=s16le",
                "--rate=48000",
                "--channels=1",
            ],
            stdin=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

        # Start background tasks for reading/writing
        self._input_task = asyncio.create_task(self._read_input_loop())
        self._output_task = asyncio.create_task(self._write_output_loop())

        logger.info(f"PulseAudio I/O started: device=48000Hz, target={self._target_sample_rate}Hz")

    async def _read_input_loop(self) -> None:
        """Background task to read from parecord and buffer frames."""
        if not self._record_proc or not self._record_proc.stdout:
            logger.error("PulseAudio read loop: no record process or stdout")
            return

        bytes_per_frame = self._device_frame_size * 2
        loop = asyncio.get_event_loop()
        frame_count = 0
        logger.info(f"PulseAudio read loop started, expecting {bytes_per_frame} bytes per frame")

        while self._running and self._record_proc.poll() is None:
            try:
                raw_data = await loop.run_in_executor(
                    None, self._record_proc.stdout.read, bytes_per_frame
                )
                if not raw_data or len(raw_data) < bytes_per_frame:
                    continue
                frame = self._process_pulse_input_raw_data(raw_data)
                self._report_pulse_input_level(frame)
                frame_count = self._queue_pulse_input_frame(frame, frame_count)
            except Exception as e:
                if self._running:
                    logger.warning(f"PulseAudio input error: {e}")
                break

        logger.info(f"PulseAudio read loop exiting, captured {frame_count} frames")

    def _process_pulse_input_raw_data(self, raw_data: bytes) -> NDArray[np.float32]:
        """Convert raw audio data to processed frame."""
        samples = np.frombuffer(raw_data, dtype=np.int16).astype(np.float32)
        samples /= 32768.0
        frame = _resample(samples, 48000, self._target_sample_rate)
        return np.clip(frame * self._input_gain, -1.0, 1.0)

    def _report_pulse_input_level(self, frame: NDArray[np.float32]) -> None:
        """Report input level to callback if set."""
        if self._on_input_level:
            rms = float(np.sqrt(np.mean(frame**2)))
            self._on_input_level(min(1.0, rms * 5))

    def _queue_pulse_input_frame(self, frame: NDArray[np.float32], frame_count: int) -> int:
        """Queue input frame and return updated count."""
        frame_count += 1
        try:
            self._input_buffer.put_nowait(frame)
            if frame_count <= 3 or frame_count % 500 == 0:
                logger.info(
                    f"PulseAudio captured frame {frame_count}, queue size={self._input_buffer.qsize()}"
                )
        except asyncio.QueueFull:
            pass
        return frame_count

    async def _write_output_loop(self) -> None:
        """Background task to write frames to paplay."""
        if not self._play_proc or not self._play_proc.stdin:
            return

        loop = asyncio.get_event_loop()

        while self._running:
            try:
                # Get frame from buffer (with timeout to check running state)
                try:
                    frame = await asyncio.wait_for(self._output_buffer.get(), timeout=0.1)
                except TimeoutError:
                    continue

                # Calculate RMS level for visualization
                if self._on_output_level:
                    rms = float(np.sqrt(np.mean(frame**2)))
                    self._on_output_level(min(1.0, rms * 5))

                # Resample from 24kHz to 48kHz
                resampled = _resample(frame, self._target_sample_rate, 48000)

                # Convert float32 to s16le
                samples_int = (resampled * 32767).astype(np.int16)
                raw_data = samples_int.tobytes()

                # Write to subprocess (blocking, run in executor)
                await loop.run_in_executor(None, self._play_proc.stdin.write, raw_data)
                await loop.run_in_executor(None, self._play_proc.stdin.flush)

            except Exception as e:
                if self._running:
                    logger.warning(f"PulseAudio output error: {e}")
                break

    async def stop(self) -> None:
        """Stop audio streams."""
        if not self._running:
            return

        self._running = False
        await self._cancel_background_tasks()
        self._terminate_subprocesses()
        self._clear_buffers()
        logger.info("PulseAudio I/O stopped")

    async def _cancel_background_tasks(self) -> None:
        """Cancel and clean up background tasks."""
        await self._cancel_task(self._input_task)
        self._input_task = None
        await self._cancel_task(self._output_task)
        self._output_task = None

    async def _cancel_task(self, task: asyncio.Task[None] | None) -> None:
        """Cancel a single task safely."""
        if task:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass

    def _terminate_subprocesses(self) -> None:
        """Terminate subprocess handles."""
        self._terminate_record_proc()
        self._terminate_play_proc()

    def _terminate_record_proc(self) -> None:
        """Terminate record subprocess."""
        if self._record_proc:
            self._record_proc.terminate()
            self._record_proc.wait()
            self._record_proc = None

    def _terminate_play_proc(self) -> None:
        """Terminate play subprocess."""
        if self._play_proc:
            if self._play_proc.stdin:
                self._play_proc.stdin.close()
            self._play_proc.terminate()
            self._play_proc.wait()
            self._play_proc = None

    def _clear_buffers(self) -> None:
        """Clear audio buffers."""
        self._drain_queue(self._input_buffer)
        self._drain_queue(self._output_buffer)

    def _drain_queue(self, queue: asyncio.Queue[NDArray[np.float32]]) -> None:
        """Drain all items from a queue."""
        while not queue.empty():
            try:
                queue.get_nowait()
            except asyncio.QueueEmpty:
                break

    async def read_frame(self, timeout: float = 0.2) -> NDArray[np.float32] | None:
        """Read an audio frame from input."""
        if not self._running:
            return None

        try:
            return await asyncio.wait_for(self._input_buffer.get(), timeout=timeout)
        except TimeoutError:
            return None

    async def write_frame(self, frame: NDArray[np.float32]) -> bool:
        """Write an audio frame to output."""
        if not self._running:
            logger.warning(f"write_frame called but not running, frame size={len(frame)}")
            return False

        try:
            self._output_buffer.put_nowait(frame)
            logger.info(
                f"write_frame: queued {len(frame)} samples, "
                f"queue size={self._output_buffer.qsize()}"
            )
            return True
        except asyncio.QueueFull:
            logger.warning(f"write_frame: buffer full, dropped {len(frame)} samples")
            return False

    async def play_clip(self, audio: NDArray[np.float32]) -> None:
        """Play an audio clip (blocking until complete)."""
        if not self._running:
            await self.start()

        # Split into frames and queue
        num_frames = len(audio) // self._target_frame_size
        for i in range(num_frames):
            start = i * self._target_frame_size
            end = start + self._target_frame_size
            frame = audio[start:end]
            await self.write_frame(frame)
            await asyncio.sleep(self.frame_duration_ms / 1000 * 0.9)

        # Handle remaining samples
        remainder = len(audio) % self._target_frame_size
        if remainder > 0:
            final_frame = np.zeros(self._target_frame_size, dtype=DTYPE)
            final_frame[:remainder] = audio[-remainder:]
            await self.write_frame(final_frame)
            await asyncio.sleep(self.frame_duration_ms / 1000)

    def is_running(self) -> bool:
        """Check if audio I/O is running."""
        return self._running


def _should_use_pulseaudio() -> bool:
    """
    Check if we should use PulseAudio subprocess backend.

    Returns True if:
    - Running in Docker (/.dockerenv exists)
    - PulseAudio tools are available (parecord, paplay)
    - PULSE_SERVER environment variable is set
    """
    # Check for Docker environment
    in_docker = os.path.exists("/.dockerenv")

    # Check for PulseAudio tools
    has_parecord = shutil.which("parecord") is not None
    has_paplay = shutil.which("paplay") is not None

    # Check for PulseAudio server
    has_pulse_server = bool(os.environ.get("PULSE_SERVER"))

    use_pulse = in_docker and has_parecord and has_paplay and has_pulse_server

    if use_pulse:
        logger.info("Using PulseAudio subprocess backend for Docker environment")
    else:
        logger.info("Using sounddevice backend")

    return use_pulse


def create_audio_io(
    target_sample_rate: int = TARGET_SAMPLE_RATE,
    channels: int = CHANNELS,
    frame_size: int = TARGET_FRAME_SIZE,
    buffer_frames: int = 10,
) -> AudioIO | PulseAudioIO:
    """
    Create the appropriate AudioIO instance for the environment.

    In Docker with PulseAudio available, returns PulseAudioIO.
    Otherwise, returns the sounddevice-based AudioIO.
    """
    if _should_use_pulseaudio():
        return PulseAudioIO(
            target_sample_rate=target_sample_rate,
            channels=channels,
            frame_size=frame_size,
            buffer_frames=buffer_frames,
        )
    return AudioIO(
        target_sample_rate=target_sample_rate,
        channels=channels,
        frame_size=frame_size,
        buffer_frames=buffer_frames,
    )
