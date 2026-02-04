"""
PersonaPlex voice controller.

Manages the voice conversation loop: audio capture, PersonaPlex inference,
context compaction, and thinking audio playback.

Uses the PersonaPlex submodule from vendor/personaplex for model loading.
"""

from __future__ import annotations

import asyncio
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from enum import Enum, auto
from pathlib import Path
from typing import TYPE_CHECKING, Any

import numpy as np

from entropi.core.logging import get_logger
from entropi.voice.audio_io import create_audio_io
from entropi.voice.context_compactor import ContextCompactor
from entropi.voice.thinking_audio import ThinkingAudioManager

if TYPE_CHECKING:
    from numpy.typing import NDArray

    from entropi.config.schema import VoiceConfig

logger = get_logger("voice.controller")

# Add PersonaPlex submodule to path
VENDOR_PATH = Path(__file__).parent.parent.parent.parent / "vendor" / "personaplex" / "moshi"
if VENDOR_PATH.exists() and str(VENDOR_PATH) not in sys.path:
    sys.path.insert(0, str(VENDOR_PATH))

# HuggingFace repo and model files
HF_REPO = "nvidia/personaplex-7b-v1"
MIMI_NAME = "tokenizer-e351c8d8-checkpoint125.safetensors"
MOSHI_NAME = "model.safetensors"
TEXT_TOKENIZER_NAME = "tokenizer_spm_32k_3.model"

# Available voice prompts (downloaded from HF)
VOICE_PROMPTS = {
    # Natural voices (more conversational)
    "NATF0": "NATF0.pt",
    "NATF1": "NATF1.pt",
    "NATF2": "NATF2.pt",
    "NATF3": "NATF3.pt",
    "NATM0": "NATM0.pt",
    "NATM1": "NATM1.pt",
    "NATM2": "NATM2.pt",
    "NATM3": "NATM3.pt",
    # Variety voices
    "VARF0": "VARF0.pt",
    "VARF1": "VARF1.pt",
    "VARF2": "VARF2.pt",
    "VARF3": "VARF3.pt",
    "VARF4": "VARF4.pt",
    "VARM0": "VARM0.pt",
    "VARM1": "VARM1.pt",
    "VARM2": "VARM2.pt",
    "VARM3": "VARM3.pt",
    "VARM4": "VARM4.pt",
}

DEFAULT_VOICE = "NATF2"


class VoiceState(Enum):
    """Voice interface state machine."""

    IDLE = auto()
    INITIALIZING = auto()
    CONVERSATION = auto()
    THINKING = auto()
    COMPACTING = auto()
    ERROR = auto()


@dataclass
class ConversationWindow:
    """Data for a single conversation window."""

    window_number: int
    start_time: float
    end_time: float | None = None
    user_transcript: str = ""
    assistant_transcript: str = ""
    audio_frames_processed: int = 0


@dataclass
class VoiceStats:
    """Real-time voice stats for UI display."""

    frames_processed: int = 0
    total_inference_time: float = 0.0
    last_frame_latency_ms: float = 0.0
    session_start_time: float = 0.0

    @property
    def fps(self) -> float:
        """Frames per second."""
        if self.session_start_time > 0:
            elapsed = time.time() - self.session_start_time
            if elapsed > 0:
                return self.frames_processed / elapsed
        return 0.0

    @property
    def avg_latency_ms(self) -> float:
        """Average inference latency in milliseconds."""
        if self.frames_processed > 0:
            return (self.total_inference_time / self.frames_processed) * 1000
        return 0.0


@dataclass
class VoiceCallbacks:
    """Callbacks for voice interface events."""

    on_state_change: Callable[[VoiceState], None] | None = None
    on_transcript_update: Callable[[str, str], None] | None = None  # (speaker, text)
    on_input_level: Callable[[float], None] | None = None
    on_output_level: Callable[[float], None] | None = None
    on_window_complete: Callable[[ConversationWindow], None] | None = None
    on_error: Callable[[str], None] | None = None
    on_loading_progress: Callable[[str], None] | None = None  # Loading stage message
    on_stats_update: Callable[[VoiceStats], None] | None = None  # Real-time stats


@dataclass
class PersonaPlexState:
    """Internal state for PersonaPlex model."""

    mimi: Any = None
    lm_model: Any = None
    lm_gen: Any = None
    text_tokenizer: Any = None
    device: str = "cuda"
    frame_size: int = 1920
    loaded: bool = False


def _download_model_files(
    voice_name: str = DEFAULT_VOICE,
    cache_dir: Path | None = None,
) -> dict[str, Path]:
    """
    Download PersonaPlex model files from HuggingFace.

    Returns dict with paths to: mimi, moshi, tokenizer, and voice prompts.
    """
    import tarfile

    from huggingface_hub import hf_hub_download

    logger.info(f"Downloading PersonaPlex models from {HF_REPO}...")

    paths: dict[str, Path] = {}

    # Download core model files
    paths["mimi"] = Path(hf_hub_download(repo_id=HF_REPO, filename=MIMI_NAME))
    paths["moshi"] = Path(hf_hub_download(repo_id=HF_REPO, filename=MOSHI_NAME))
    paths["tokenizer"] = Path(hf_hub_download(repo_id=HF_REPO, filename=TEXT_TOKENIZER_NAME))

    # Download and extract voices.tgz
    voices_tgz = Path(hf_hub_download(repo_id=HF_REPO, filename="voices.tgz"))
    voices_dir = voices_tgz.parent / "voices"

    if not voices_dir.exists():
        logger.info("Extracting voice prompts...")
        with tarfile.open(voices_tgz, "r:gz") as tar:
            tar.extractall(path=voices_tgz.parent)

    # Find the voice prompt file
    voice_file = VOICE_PROMPTS.get(voice_name, VOICE_PROMPTS[DEFAULT_VOICE])
    voice_path = voices_dir / voice_file

    if not voice_path.exists():
        # Try without extension or with different structure
        for pattern in [voice_file, f"{voice_name}.pt", voice_name]:
            candidate = voices_dir / pattern
            if candidate.exists():
                voice_path = candidate
                break
        else:
            # List available files for debugging
            available = list(voices_dir.glob("*.pt")) if voices_dir.exists() else []
            logger.warning(f"Voice {voice_name} not found. Available: {[f.name for f in available]}")
            if available:
                voice_path = available[0]
                logger.info(f"Using fallback voice: {voice_path.name}")

    paths["voice_prompt"] = voice_path
    paths["voices_dir"] = voices_dir

    logger.info("Model files downloaded successfully")
    return paths


class PersonaPlexController:
    """
    Controls the PersonaPlex voice conversation loop.

    Manages the cycle:
    1. Capture audio and generate responses (CONVERSATION)
    2. Play thinking audio and compact context (THINKING/COMPACTING)
    3. Inject new context and repeat
    """

    def __init__(
        self,
        config: VoiceConfig,
        callbacks: VoiceCallbacks | None = None,
    ) -> None:
        """
        Initialize PersonaPlex controller.

        Args:
            config: Voice configuration
            callbacks: Optional event callbacks
        """
        self._config = config
        self._callbacks = callbacks or VoiceCallbacks()

        # State
        self._state = VoiceState.IDLE
        self._running = False
        self._paused = False
        self._window_number = 0

        # Components
        self._audio_io = create_audio_io()
        self._thinking_audio = ThinkingAudioManager(
            audio_dir=config.voice_prompt.prompt_dir,
            default_file=config.voice_prompt.thinking_audio,
        )
        self._compactor = ContextCompactor(config)

        # PersonaPlex state
        self._plex_state = PersonaPlexState()

        # Current window
        self._current_window: ConversationWindow | None = None

        # Accumulated context
        self._context_summary: str = ""

        # Stats tracking
        self._stats = VoiceStats()

        # Control
        self._lock = asyncio.Lock()
        self._stop_event = asyncio.Event()

    @property
    def state(self) -> VoiceState:
        """Get current state."""
        return self._state

    @property
    def is_running(self) -> bool:
        """Check if controller is running."""
        return self._running

    @property
    def is_paused(self) -> bool:
        """Check if controller is paused."""
        return self._paused

    @property
    def window_number(self) -> int:
        """Get current window number."""
        return self._window_number

    def _set_state(self, state: VoiceState) -> None:
        """Update state and notify callback."""
        if self._state != state:
            self._state = state
            if self._callbacks.on_state_change:
                self._callbacks.on_state_change(state)

    def _report_progress(self, message: str) -> None:
        """Report loading progress."""
        logger.info(message)
        if self._callbacks.on_loading_progress:
            self._callbacks.on_loading_progress(message)

    async def initialize(self) -> bool:
        """
        Initialize PersonaPlex and related components.

        Returns:
            True if initialization successful
        """
        self._set_state(VoiceState.INITIALIZING)

        try:
            # Load thinking audio clips
            await self._thinking_audio.load_clips()

            # Initialize context compactor
            await self._compactor.initialize()

            # Load PersonaPlex models
            await self._load_personaplex()

            # Set up audio callbacks (but don't start yet - wait for start_conversation)
            self._audio_io.set_level_callbacks(
                on_input=self._callbacks.on_input_level,
                on_output=self._callbacks.on_output_level,
            )

            self._set_state(VoiceState.IDLE)
            logger.info("PersonaPlex controller initialized (audio not started)")
            return True

        except Exception as e:
            logger.error(f"Failed to initialize PersonaPlex: {e}")
            self._set_state(VoiceState.ERROR)
            if self._callbacks.on_error:
                self._callbacks.on_error(f"Initialization failed: {e}")
            return False

    async def _load_personaplex(self) -> None:
        """Load PersonaPlex model components using the submodule.

        Runs blocking model loading operations in a thread executor
        to keep the TUI responsive during loading.
        """
        try:
            import torch

            device = self._config.runtime.device
            self._plex_state.device = device
            use_int8 = self._config.runtime.quantization == "int8"

            self._report_progress(f"Loading PersonaPlex on {device} (quantize_int8={use_int8})")

            # Download model files from HuggingFace
            self._report_progress("Downloading model files from HuggingFace...")
            voice_name = self._config.voice_prompt.voice_name
            model_paths = await asyncio.to_thread(
                _download_model_files, voice_name=voice_name
            )

            # Try to use the submodule loaders
            try:
                from moshi.models import loaders
                from moshi.models.lm import LMGen
                import sentencepiece

                # Load Mimi codec (run in thread - blocking I/O)
                self._report_progress("Loading Mimi audio codec...")
                self._plex_state.mimi = await asyncio.to_thread(
                    loaders.get_mimi,
                    str(model_paths["mimi"]),
                    device=device,
                )

                # Load LM model (run in thread - this is the slow one!)
                context_window = self._config.runtime.context_window
                self._report_progress(f"Loading Moshi LM (context={context_window})...")
                lm_model = await asyncio.to_thread(
                    loaders.get_moshi_lm,
                    str(model_paths["moshi"]),
                    device=device,
                    dtype=torch.bfloat16,
                    quantize_int8=use_int8,
                    context=context_window,
                )

                # Create LMGen wrapper
                self._report_progress("Initializing LM generator...")
                self._plex_state.lm_gen = LMGen(
                    lm_model,
                    audio_silence_frame_cnt=int(0.5 * self._plex_state.mimi.frame_rate),
                    sample_rate=self._plex_state.mimi.sample_rate,
                    device=device,
                    frame_rate=self._plex_state.mimi.frame_rate,
                )

                # Load text tokenizer
                self._report_progress("Loading tokenizer...")
                self._plex_state.text_tokenizer = sentencepiece.SentencePieceProcessor(
                    str(model_paths["tokenizer"])
                )

                # Load voice prompt
                self._report_progress("Loading voice prompt...")
                await asyncio.to_thread(
                    self._plex_state.lm_gen.load_voice_prompt_embeddings,
                    str(model_paths["voice_prompt"]),
                )

                # Set frame size
                self._plex_state.frame_size = int(
                    self._plex_state.mimi.sample_rate / self._plex_state.mimi.frame_rate
                )

                # Set initial text prompt
                initial_prompt = self._config.conversation.initial_prompt
                if initial_prompt:
                    wrapped = f"<system> {initial_prompt} <system>"
                    self._plex_state.lm_gen.text_prompt_tokens = (
                        self._plex_state.text_tokenizer.encode(wrapped)
                    )

                # Enable streaming mode
                self._plex_state.mimi.streaming_forever(1)
                self._plex_state.lm_gen.streaming_forever(1)

                # Warmup (run in thread)
                self._report_progress("Warming up models...")
                await asyncio.to_thread(self._warmup)

                self._plex_state.loaded = True
                self._report_progress("PersonaPlex ready!")

            except ImportError as e:
                import traceback
                logger.warning(f"PersonaPlex submodule not available: {e}")
                logger.warning(f"Traceback:\n{traceback.format_exc()}")
                self._report_progress(f"Using stub (import failed: {e})")
                self._plex_state.mimi = _StubMimi()
                self._plex_state.lm_gen = _StubLMGen()
                self._plex_state.text_tokenizer = _StubTokenizer()
                self._plex_state.loaded = True

            except RuntimeError as e:
                if "CUDA" in str(e) or "kernel" in str(e):
                    logger.error(
                        f"CUDA error: {e}\n"
                        "Try one of:\n"
                        "  1. Set voice.runtime.device: 'cpu' in config\n"
                        "  2. Update PyTorch: pip install torch --index-url https://download.pytorch.org/whl/cu130"
                    )
                raise

        except ImportError as e:
            raise ImportError(
                f"Missing dependency for voice mode: {e}. "
                "Install with: pip install entropi[voice]"
            ) from e

    def _warmup(self) -> None:
        """Warmup models with dummy data."""
        import torch

        # Use CPU for warmup data - model will move it as needed
        frame_size = self._plex_state.frame_size

        try:
            for _ in range(2):  # Reduced iterations
                chunk = torch.zeros(1, 1, frame_size, dtype=torch.float32)
                # Move to same device as mimi encoder expects
                if hasattr(self._plex_state.mimi, 'device'):
                    chunk = chunk.to(self._plex_state.mimi.device)
                elif self._plex_state.device == "cuda" and torch.cuda.is_available():
                    chunk = chunk.cuda()

                codes = self._plex_state.mimi.encode(chunk)
                for c in range(codes.shape[-1]):
                    tokens = self._plex_state.lm_gen.step(codes[:, :, c : c + 1])
                    if tokens is None:
                        continue
                    _ = self._plex_state.mimi.decode(tokens[:, 1:9])

            if torch.cuda.is_available():
                torch.cuda.synchronize()

            logger.info("Warmup complete")

        except Exception as e:
            logger.warning(f"Warmup failed (non-fatal): {e}")

    async def start_conversation(self) -> None:
        """Start the conversation loop."""
        if self._running:
            return

        # Start audio I/O now that user is ready
        logger.info("Starting audio I/O...")
        await self._audio_io.start()
        logger.info("Audio I/O started")

        # Reset streaming state before starting conversation
        if self._plex_state.loaded and not isinstance(self._plex_state.mimi, _StubMimi):
            logger.info("Resetting streaming state...")
            self._plex_state.mimi.reset_streaming()
            self._plex_state.lm_gen.reset_streaming()

            # Process system prompts (voice prompt + text prompt)
            logger.info("Processing system prompts...")
            await asyncio.to_thread(
                self._plex_state.lm_gen.step_system_prompts,
                self._plex_state.mimi
            )
            # Reset mimi again after processing prompts (as PersonaPlex server does)
            self._plex_state.mimi.reset_streaming()
            logger.info("System prompts processed")

        async with self._lock:
            self._running = True
            self._paused = False
            self._stop_event.clear()
            self._window_number = 0
            self._context_summary = ""
            # Reset stats and set session start time
            self._stats = VoiceStats(session_start_time=time.time())

        # Run conversation loop
        asyncio.create_task(self._conversation_loop())

    async def _conversation_loop(self) -> None:
        """Main conversation loop."""
        try:
            while self._running and not self._stop_event.is_set():
                if self._paused:
                    await asyncio.sleep(0.1)
                    continue

                # Start new conversation window
                self._window_number += 1
                await self._run_conversation_window()

                if not self._running or self._stop_event.is_set():
                    break

                # Thinking/compacting phase
                await self._run_thinking_phase()

        except asyncio.CancelledError:
            logger.info("Conversation loop cancelled")
        except Exception as e:
            logger.error(f"Conversation loop error: {e}")
            self._set_state(VoiceState.ERROR)
            if self._callbacks.on_error:
                self._callbacks.on_error(str(e))
        finally:
            self._running = False
            self._set_state(VoiceState.IDLE)

    async def _run_conversation_window(self) -> None:
        """Run a single conversation window."""
        self._set_state(VoiceState.CONVERSATION)

        window = ConversationWindow(
            window_number=self._window_number,
            start_time=time.time(),
        )
        self._current_window = window

        window_duration = self._config.conversation.window_duration
        end_time = time.time() + window_duration

        logger.info(f"Starting conversation window {window.window_number} ({window_duration}s)")

        while time.time() < end_time and self._running and not self._paused:
            # Read audio frame
            frame = await self._audio_io.read_frame()
            if frame is None:
                continue

            window.audio_frames_processed += 1

            # Process through PersonaPlex (writes audio internally now)
            text = await self._process_audio_frame(frame)

            # Update transcript
            if text:
                window.assistant_transcript += text
                if self._callbacks.on_transcript_update:
                    self._callbacks.on_transcript_update("assistant", text)

        window.end_time = time.time()
        self._current_window = None

        # Notify window complete
        if self._callbacks.on_window_complete:
            self._callbacks.on_window_complete(window)

        # Add to context for compaction
        self._compactor.add_to_context(
            f"User: {window.user_transcript}\nAssistant: {window.assistant_transcript}"
        )

        logger.info(
            f"Window {window.window_number} complete: "
            f"{window.audio_frames_processed} frames, "
            f"{len(window.assistant_transcript)} chars"
        )

    async def _process_audio_frame(
        self, frame: NDArray[np.float32]
    ) -> str:
        """
        Process a single audio frame through PersonaPlex.

        Writes audio output immediately inside the loop, mirroring
        the PersonaPlex demo pattern for real-time streaming.

        Args:
            frame: Input audio frame

        Returns:
            Generated text (audio is written directly to audio_io)
        """
        if not self._plex_state.loaded:
            return ""

        frame_start = time.time()

        try:
            import torch

            device = self._plex_state.device

            # Convert numpy to torch
            chunk = torch.from_numpy(frame).to(device=device)[None, None]

            # Encode audio to tokens using Mimi
            codes = self._plex_state.mimi.encode(chunk)

            text = ""

            # Process each code frame - WRITE IMMEDIATELY like PersonaPlex
            for c in range(codes.shape[-1]):
                tokens = self._plex_state.lm_gen.step(codes[:, :, c : c + 1])
                if tokens is None:
                    continue

                # Decode and write immediately (mirrors PersonaPlex server.py)
                main_pcm = self._plex_state.mimi.decode(tokens[:, 1:9])
                pcm_chunk = main_pcm.detach().cpu().numpy()[0, 0]
                await self._audio_io.write_frame(pcm_chunk)

                # Extract text token
                text_token = tokens[0, 0, 0].item()
                if text_token not in (0, 3):  # Not padding
                    _text = self._plex_state.text_tokenizer.id_to_piece(text_token)
                    _text = _text.replace("▁", " ")
                    text += _text

            # Update stats
            frame_latency = time.time() - frame_start
            self._stats.frames_processed += 1
            self._stats.total_inference_time += frame_latency
            self._stats.last_frame_latency_ms = frame_latency * 1000

            # Call stats callback periodically (every 10 frames to avoid overhead)
            if self._stats.frames_processed % 10 == 0 and self._callbacks.on_stats_update:
                self._callbacks.on_stats_update(self._stats)

            return text

        except Exception as e:
            logger.warning(f"Frame processing error: {e}")
            return ""

    async def _run_thinking_phase(self) -> None:
        """Run the thinking/compaction phase between windows."""
        self._set_state(VoiceState.THINKING)

        # Play thinking audio
        thinking_clip = await self._thinking_audio.get_clip()
        if thinking_clip is not None:
            logger.debug("Playing thinking audio")
            await self._audio_io.play_clip(thinking_clip)

        # Compact context
        self._set_state(VoiceState.COMPACTING)

        context = self._compactor.get_accumulated_context()
        if context:
            result = await self._compactor.compact(
                transcript=context,
                previous_summary=self._context_summary if self._context_summary else None,
            )
            self._context_summary = result.summary
            self._compactor.clear_context()

            # Update text prompt with compacted context
            if self._plex_state.loaded and self._plex_state.text_tokenizer:
                injection = await self._compactor.build_injection_prompt(
                    summary=self._context_summary,
                    initial_prompt=self._config.conversation.initial_prompt,
                    format="plain",  # PersonaPlex uses plain text prompts
                )
                wrapped = f"<system> {injection} <system>"
                self._plex_state.lm_gen.text_prompt_tokens = (
                    self._plex_state.text_tokenizer.encode(wrapped)
                )

            logger.debug(
                f"Context compacted: {result.original_length} -> {result.summary_length} chars"
            )

    async def pause(self) -> None:
        """Pause the conversation."""
        self._paused = True
        logger.info("Conversation paused")

    async def resume(self) -> None:
        """Resume the conversation."""
        self._paused = False
        logger.info("Conversation resumed")

    async def stop(self) -> None:
        """Stop the conversation and clean up."""
        import gc

        self._running = False
        self._stop_event.set()

        await self._audio_io.stop()
        await self._compactor.cleanup()

        # Unload PersonaPlex
        if self._plex_state.loaded:
            self._plex_state.mimi = None
            self._plex_state.lm_gen = None
            self._plex_state.lm_model = None
            self._plex_state.text_tokenizer = None
            self._plex_state.loaded = False

        # Force garbage collection and CUDA cache clear
        gc.collect()
        try:
            import torch

            if torch.cuda.is_available():
                torch.cuda.empty_cache()
                torch.cuda.synchronize()
                logger.info("Cleared CUDA cache")
        except ImportError:
            pass

        self._set_state(VoiceState.IDLE)
        logger.info("PersonaPlex controller stopped")

    async def reset(self) -> None:
        """Reset context and start fresh."""
        self._context_summary = ""
        self._compactor.clear_context()
        self._window_number = 0

        # Reset text prompt to initial
        if self._plex_state.loaded and self._plex_state.text_tokenizer:
            initial_prompt = self._config.conversation.initial_prompt
            if initial_prompt:
                wrapped = f"<system> {initial_prompt} <system>"
                self._plex_state.lm_gen.text_prompt_tokens = (
                    self._plex_state.text_tokenizer.encode(wrapped)
                )

        logger.info("Context reset")

    def get_context_summary(self) -> str:
        """Get current context summary."""
        return self._context_summary

    @staticmethod
    def list_available_voices() -> list[str]:
        """List available voice prompts."""
        return list(VOICE_PROMPTS.keys())

    async def set_voice(self, voice_name: str) -> bool:
        """
        Change the voice prompt.

        Args:
            voice_name: Voice name (e.g., "NATF2", "NATM1")

        Returns:
            True if successful
        """
        if voice_name not in VOICE_PROMPTS:
            logger.error(f"Unknown voice: {voice_name}")
            return False

        if not self._plex_state.loaded:
            logger.error("PersonaPlex not loaded")
            return False

        try:
            from huggingface_hub import hf_hub_download

            voice_path = hf_hub_download(
                repo_id=HF_REPO,
                filename=VOICE_PROMPTS[voice_name],
            )
            self._plex_state.lm_gen.load_voice_prompt_embeddings(voice_path)
            logger.info(f"Voice changed to {voice_name}")
            return True

        except Exception as e:
            logger.error(f"Failed to change voice: {e}")
            return False


# Stub classes for testing without full PersonaPlex dependencies


class _StubMimi:
    """Stub Mimi codec for testing."""

    sample_rate = 24000
    frame_rate = 12.5

    def encode(self, audio: Any) -> Any:
        import torch

        return torch.zeros((1, 8, 1), dtype=torch.int64)

    def decode(self, tokens: Any) -> Any:
        import torch

        return torch.zeros((1, 1, 1920), dtype=torch.float32)

    def streaming_forever(self, n: int) -> None:
        pass


class _StubLMGen:
    """Stub LM generator for testing."""

    text_prompt_tokens: list[int] | None = None

    def step(self, codes: Any) -> Any:
        import torch

        return torch.zeros((1, 17, 1), dtype=torch.int64)

    def streaming_forever(self, n: int) -> None:
        pass

    def load_voice_prompt_embeddings(self, path: str) -> None:
        pass


class _StubTokenizer:
    """Stub tokenizer for testing."""

    def __init__(self) -> None:
        self._counter = 0

    def encode(self, text: str) -> list[int]:
        return [0] * len(text.split())

    def id_to_piece(self, id: int) -> str:
        self._counter += 1
        if self._counter % 50 == 0:
            return "[stub] "
        return ""
