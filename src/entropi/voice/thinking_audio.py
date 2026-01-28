"""
Thinking moment audio management.

Manages audio clips played during context compaction to maintain
conversation flow while the secondary LLM processes.
"""

from __future__ import annotations

import asyncio
import wave
from pathlib import Path
from typing import TYPE_CHECKING

import numpy as np

from entropi.core.logging import get_logger

if TYPE_CHECKING:
    from numpy.typing import NDArray

logger = get_logger("voice.thinking_audio")

# Target sample rate for PersonaPlex
TARGET_SAMPLE_RATE = 24000


def load_thinking_audio(
    path: Path,
    target_sample_rate: int = TARGET_SAMPLE_RATE,
) -> NDArray[np.float32]:
    """
    Load and resample a WAV file for thinking audio.

    Args:
        path: Path to WAV file
        target_sample_rate: Target sample rate for resampling

    Returns:
        Audio data as float32 array at target sample rate

    Raises:
        FileNotFoundError: If file doesn't exist
        ValueError: If file format is unsupported
    """
    if not path.exists():
        raise FileNotFoundError(f"Thinking audio not found: {path}")

    with wave.open(str(path), "rb") as wf:
        # Get audio parameters
        channels = wf.getnchannels()
        sample_width = wf.getsampwidth()
        source_rate = wf.getframerate()
        n_frames = wf.getnframes()

        # Read raw audio data
        raw_data = wf.readframes(n_frames)

    # Convert to numpy array based on sample width
    if sample_width == 1:
        audio = np.frombuffer(raw_data, dtype=np.uint8).astype(np.float32)
        audio = (audio - 128) / 128.0  # Convert to -1.0 to 1.0
    elif sample_width == 2:
        audio = np.frombuffer(raw_data, dtype=np.int16).astype(np.float32)
        audio = audio / 32768.0  # Convert to -1.0 to 1.0
    elif sample_width == 4:
        audio = np.frombuffer(raw_data, dtype=np.int32).astype(np.float32)
        audio = audio / 2147483648.0  # Convert to -1.0 to 1.0
    else:
        raise ValueError(f"Unsupported sample width: {sample_width}")

    # Convert stereo to mono if needed
    if channels == 2:
        audio = audio.reshape(-1, 2).mean(axis=1)
    elif channels > 2:
        raise ValueError(f"Unsupported channel count: {channels}")

    # Resample if needed
    if source_rate != target_sample_rate:
        audio = _resample(audio, source_rate, target_sample_rate)

    logger.debug(
        f"Loaded thinking audio: {path.name} "
        f"({len(audio) / target_sample_rate:.1f}s, {source_rate}Hz -> {target_sample_rate}Hz)"
    )

    return audio


def _resample(
    audio: NDArray[np.float32],
    source_rate: int,
    target_rate: int,
) -> NDArray[np.float32]:
    """
    Simple linear interpolation resampling.

    For higher quality, use scipy.signal.resample or librosa if available.

    Args:
        audio: Source audio data
        source_rate: Source sample rate
        target_rate: Target sample rate

    Returns:
        Resampled audio data
    """
    if source_rate == target_rate:
        return audio

    # Calculate new length
    duration = len(audio) / source_rate
    new_length = int(duration * target_rate)

    # Create interpolation indices
    old_indices = np.linspace(0, len(audio) - 1, new_length)
    new_audio = np.interp(old_indices, np.arange(len(audio)), audio)

    return new_audio.astype(np.float32)


class ThinkingAudioManager:
    """
    Manages thinking moment audio clips.

    Caches loaded audio clips and provides rotation for variety.
    """

    def __init__(
        self,
        audio_dir: Path,
        default_file: str = "thinking_moment.wav",
    ) -> None:
        """
        Initialize thinking audio manager.

        Args:
            audio_dir: Directory containing audio clips
            default_file: Default thinking audio filename
        """
        self._audio_dir = Path(audio_dir).expanduser()
        self._default_file = default_file
        self._cache: dict[str, NDArray[np.float32]] = {}
        self._clip_list: list[str] = []
        self._current_index = 0
        self._lock = asyncio.Lock()

    @property
    def audio_dir(self) -> Path:
        """Get audio directory path."""
        return self._audio_dir

    @property
    def default_path(self) -> Path:
        """Get default thinking audio path."""
        return self._audio_dir / self._default_file

    async def load_clips(self) -> int:
        """
        Load all available thinking audio clips.

        Returns:
            Number of clips loaded
        """
        async with self._lock:
            self._cache.clear()
            self._clip_list.clear()

            if not self._audio_dir.exists():
                logger.warning(f"Thinking audio directory not found: {self._audio_dir}")
                return 0

            # Load all WAV files in directory
            for wav_file in sorted(self._audio_dir.glob("thinking*.wav")):
                try:
                    audio = load_thinking_audio(wav_file)
                    self._cache[wav_file.name] = audio
                    self._clip_list.append(wav_file.name)
                except Exception as e:
                    logger.warning(f"Failed to load {wav_file.name}: {e}")

            # Also try to load default if not already loaded
            if self._default_file not in self._cache:
                default_path = self._audio_dir / self._default_file
                if default_path.exists():
                    try:
                        audio = load_thinking_audio(default_path)
                        self._cache[self._default_file] = audio
                        self._clip_list.insert(0, self._default_file)
                    except Exception as e:
                        logger.warning(f"Failed to load default: {e}")

            logger.info(f"Loaded {len(self._cache)} thinking audio clips")
            return len(self._cache)

    async def get_clip(self, rotate: bool = True) -> NDArray[np.float32] | None:
        """
        Get a thinking audio clip.

        Args:
            rotate: If True, rotate through available clips

        Returns:
            Audio data as float32 array, or None if no clips available
        """
        async with self._lock:
            if not self._clip_list:
                # Try to load clips if not yet loaded
                await self.load_clips()

            if not self._clip_list:
                return None

            if rotate and len(self._clip_list) > 1:
                # Rotate to next clip
                clip_name = self._clip_list[self._current_index]
                self._current_index = (self._current_index + 1) % len(self._clip_list)
            else:
                # Use default or first available
                clip_name = self._clip_list[0]

            return self._cache.get(clip_name)

    async def get_default(self) -> NDArray[np.float32] | None:
        """
        Get the default thinking audio clip.

        Returns:
            Audio data as float32 array, or None if not available
        """
        async with self._lock:
            if self._default_file in self._cache:
                return self._cache[self._default_file]

            # Try to load it
            default_path = self._audio_dir / self._default_file
            if default_path.exists():
                try:
                    audio = load_thinking_audio(default_path)
                    self._cache[self._default_file] = audio
                    if self._default_file not in self._clip_list:
                        self._clip_list.insert(0, self._default_file)
                    return audio
                except Exception as e:
                    logger.error(f"Failed to load default thinking audio: {e}")

            return None

    def has_clips(self) -> bool:
        """Check if any clips are loaded."""
        return len(self._cache) > 0

    def clip_count(self) -> int:
        """Get number of loaded clips."""
        return len(self._cache)
