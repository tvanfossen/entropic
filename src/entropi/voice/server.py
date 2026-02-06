"""
Standalone PersonaPlex voice server.

Runs Moshi inference in a dedicated process, separate from the TUI,
to avoid event loop contention and latency issues.

Based on the PersonaPlex demo server architecture with WebSocket API.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

from entropi.core.logging import get_logger

logger = get_logger("voice.server")

# Add PersonaPlex submodule to path
VENDOR_PATH = Path(__file__).parent.parent.parent.parent / "vendor" / "personaplex" / "moshi"
if VENDOR_PATH.exists() and str(VENDOR_PATH) not in sys.path:
    sys.path.insert(0, str(VENDOR_PATH))

# HuggingFace repo and model files
HF_REPO = "nvidia/personaplex-7b-v1"
MIMI_NAME = "tokenizer-e351c8d8-checkpoint125.safetensors"
MOSHI_NAME = "model.safetensors"
TEXT_TOKENIZER_NAME = "tokenizer_spm_32k_3.model"

# Server defaults
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765
SAMPLE_RATE = 24000
FRAME_RATE = 12.5
FRAME_SIZE = int(SAMPLE_RATE / FRAME_RATE)  # 1920 samples per frame


@dataclass
class ServerStats:
    """Real-time server statistics."""

    frames_processed: int = 0
    total_inference_time: float = 0.0
    last_frame_time: float = 0.0
    start_time: float = field(default_factory=time.time)

    @property
    def fps(self) -> float:
        """Frames per second."""
        elapsed = time.time() - self.start_time
        if elapsed > 0:
            return self.frames_processed / elapsed
        return 0.0

    @property
    def avg_latency_ms(self) -> float:
        """Average inference latency in milliseconds."""
        if self.frames_processed > 0:
            return (self.total_inference_time / self.frames_processed) * 1000
        return 0.0

    def to_dict(self) -> dict[str, Any]:
        """Convert to dictionary for JSON serialization."""
        return {
            "frames_processed": self.frames_processed,
            "fps": round(self.fps, 1),
            "avg_latency_ms": round(self.avg_latency_ms, 1),
            "uptime_s": round(time.time() - self.start_time, 1),
        }


@dataclass
class ServerState:
    """Server state holding model components."""

    mimi: Any = None
    lm_gen: Any = None
    text_tokenizer: Any = None
    device: str = "cuda"
    stats: ServerStats = field(default_factory=ServerStats)
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)


def _download_model_files(voice_name: str = "NATF2") -> dict[str, Path]:
    """Download model files from HuggingFace."""
    from huggingface_hub import hf_hub_download

    logger.info("Downloading model files from HuggingFace...")

    mimi_path = Path(hf_hub_download(HF_REPO, MIMI_NAME))
    moshi_path = Path(hf_hub_download(HF_REPO, MOSHI_NAME))
    tokenizer_path = Path(hf_hub_download(HF_REPO, TEXT_TOKENIZER_NAME))

    # Download voice prompt
    voice_file = f"{voice_name}.pt"
    voice_path = Path(hf_hub_download(HF_REPO, f"voices/{voice_file}"))

    return {
        "mimi": mimi_path,
        "moshi": moshi_path,
        "tokenizer": tokenizer_path,
        "voice": voice_path,
    }


def _log_progress(message: str) -> None:
    """Log progress to both logger and stderr for subprocess visibility."""
    logger.info(message)
    print(message, file=sys.stderr, flush=True)


async def load_models(
    device: str = "cuda",
    quantize_int8: bool = True,
    context_window: int = 500,
    voice_name: str = "NATF2",
) -> ServerState:
    """Load PersonaPlex models."""
    import sentencepiece
    import torch
    from moshi.models import loaders
    from moshi.models.lm import LMGen

    state = ServerState(device=device)

    # Download model files
    _log_progress("Downloading model files...")
    model_paths = await asyncio.to_thread(_download_model_files, voice_name)

    # Load Mimi codec
    _log_progress("Loading Mimi audio codec...")
    state.mimi = await asyncio.to_thread(
        loaders.get_mimi,
        str(model_paths["mimi"]),
        device=device,
    )

    # Load LM model
    _log_progress(f"Loading Moshi LM (context={context_window}, int8={quantize_int8})...")
    lm_model = await asyncio.to_thread(
        loaders.get_moshi_lm,
        str(model_paths["moshi"]),
        device=device,
        dtype=torch.bfloat16,
        quantize_int8=quantize_int8,
        context=context_window,
    )

    # Create LMGen wrapper
    _log_progress("Initializing LM generator...")
    state.lm_gen = LMGen(
        lm_model,
        audio_silence_frame_cnt=int(0.5 * state.mimi.frame_rate),
        sample_rate=state.mimi.sample_rate,
        device=device,
        frame_rate=state.mimi.frame_rate,
    )

    # Load text tokenizer
    _log_progress("Loading tokenizer...")
    state.text_tokenizer = sentencepiece.SentencePieceProcessor(str(model_paths["tokenizer"]))

    # Load voice prompt
    _log_progress(f"Loading voice prompt: {voice_name}...")
    state.lm_gen.load_voice_prompt_embeddings(str(model_paths["voice"]))

    # Initialize streaming
    state.mimi.streaming_forever(1)
    state.lm_gen.streaming_forever(1)

    # Warmup
    _log_progress("Warming up model...")
    await asyncio.to_thread(_warmup, state)

    _log_progress("Models loaded and ready!")
    return state


def _warmup(state: ServerState) -> None:
    """Warmup model with dummy data."""
    import torch

    for _ in range(4):
        chunk = torch.zeros(1, 1, FRAME_SIZE, dtype=torch.float32, device=state.device)
        codes = state.mimi.encode(chunk)
        for c in range(codes.shape[-1]):
            tokens = state.lm_gen.step(codes[:, :, c : c + 1])
            if tokens is not None:
                _ = state.mimi.decode(tokens[:, 1:9])

    if state.device == "cuda":
        torch.cuda.synchronize()


def _process_audio_frame_sync(
    state: ServerState,
    audio_frame: np.ndarray,
) -> tuple[np.ndarray | None, str]:
    """
    Process a single audio frame through Moshi (synchronous, runs in thread).

    Args:
        state: Server state with models
        audio_frame: Input audio frame (float32, 1920 samples at 24kHz)

    Returns:
        Tuple of (output_audio, text_token)
    """
    import torch

    start_time = time.time()

    # Make a writable copy of the array (np.frombuffer returns read-only)
    audio_frame = audio_frame.copy()

    # Convert to tensor
    chunk = torch.from_numpy(audio_frame).to(device=state.device, dtype=torch.float32)
    chunk = chunk.unsqueeze(0).unsqueeze(0)  # Shape: (1, 1, frame_size)

    # Encode input audio
    codes = state.mimi.encode(chunk)

    output_audio = None
    text_output = ""

    # Process through LM
    for c in range(codes.shape[-1]):
        tokens = state.lm_gen.step(codes[:, :, c : c + 1])
        if tokens is None:
            continue

        # Decode output audio
        audio_tokens = tokens[:, 1:9]
        pcm = state.mimi.decode(audio_tokens)
        output_audio = pcm[0, 0].cpu().numpy()

        # Decode text token
        text_token = tokens[0, 0, 0].item()
        if text_token not in (0, 3):  # Not padding
            text = state.text_tokenizer.id_to_piece(text_token)
            text = text.replace("▁", " ")
            text_output += text

    # Update stats
    inference_time = time.time() - start_time
    state.stats.frames_processed += 1
    state.stats.total_inference_time += inference_time
    state.stats.last_frame_time = inference_time

    return output_audio, text_output


async def process_audio_frame(
    state: ServerState,
    audio_frame: np.ndarray,
) -> tuple[np.ndarray | None, str]:
    """
    Process a single audio frame through Moshi (non-blocking wrapper).

    Runs the blocking GPU operations in a thread pool to avoid blocking
    the event loop.
    """
    return await asyncio.to_thread(_process_audio_frame_sync, state, audio_frame)


class _WebSocketHandler:
    """Helper class to handle websocket connection state."""

    def __init__(self, websocket: Any, state: ServerState) -> None:
        self.websocket = websocket
        self.state = state
        self.input_buffer: asyncio.Queue[bytes | str] = asyncio.Queue(maxsize=10)
        self.output_buffer: asyncio.Queue[bytes | str] = asyncio.Queue(maxsize=10)
        self.close = False

    async def recv_loop(self) -> None:
        """Receive audio frames and control messages from client."""
        frame_count = 0
        try:
            _log_progress("Server recv_loop started")
            async for message in self.websocket:
                frame_count = await self._handle_recv_message(message, frame_count)
            _log_progress(f"Server recv_loop ended after {frame_count} frames")
        except Exception as e:
            if not self.close:
                _log_progress(f"Server recv_loop error: {e}")
        finally:
            logger.debug(f"recv_loop exiting, {frame_count} frames")
            self.close = True

    async def _handle_recv_message(self, message: bytes | str, frame_count: int) -> int:
        """Handle a single received message."""
        if isinstance(message, bytes):
            frame_count += 1
            if frame_count <= 3:
                _log_progress(f"Server received frame {frame_count}, size={len(message)}")
            try:
                await asyncio.wait_for(self.input_buffer.put(message), timeout=0.1)
            except TimeoutError:
                logger.debug("Input buffer full, dropping frame")
        elif isinstance(message, str):
            await _handle_control_message(message, self.state, self.websocket)
        return frame_count

    async def inference_loop(self) -> None:
        """Process audio frames through Moshi inference."""
        frame_count = 0
        output_count = 0
        try:
            _log_progress("Server inference_loop started")
            while not self.close:
                await asyncio.sleep(0.001)
                result = await self._process_next_frame(frame_count, output_count)
                if result:
                    frame_count, output_count = result
        except Exception as e:
            if not self.close:
                _log_progress(f"Server inference_loop error: {e}")
        finally:
            _log_progress(
                f"Server inference_loop exiting, processed {frame_count}, output {output_count}"
            )
            self.close = True

    async def _process_next_frame(
        self, frame_count: int, output_count: int
    ) -> tuple[int, int] | None:
        """Process next frame from input buffer."""
        audio_frame = self._get_valid_audio_frame()
        if audio_frame is None:
            return None
        frame_count += 1
        if frame_count <= 3:
            _log_progress(f"Server processing frame {frame_count}")
        output_count = await self._run_inference(audio_frame, output_count)
        return frame_count, output_count

    def _get_valid_audio_frame(self) -> Any | None:
        """Get valid audio frame from input buffer, or None if unavailable/invalid."""
        message = self._get_message_from_buffer()
        if message is None or not isinstance(message, bytes):
            return None
        audio_frame = np.frombuffer(message, dtype=np.float32)
        if len(audio_frame) != FRAME_SIZE:
            _log_progress(f"Server invalid frame size: {len(audio_frame)}, expected {FRAME_SIZE}")
            return None
        return audio_frame

    def _get_message_from_buffer(self) -> bytes | str | None:
        """Get message from input buffer, or None if empty."""
        try:
            return self.input_buffer.get_nowait()
        except asyncio.QueueEmpty:
            return None

    async def _run_inference(self, audio_frame: Any, output_count: int) -> int:
        """Run inference on audio frame."""
        try:
            async with self.state.lock:
                output_audio, text = await process_audio_frame(self.state, audio_frame)
        except Exception as e:
            _log_progress(f"Server inference error: {e}")
            return output_count
        output_count = self._queue_inference_output(output_audio, text, output_count)
        return output_count

    def _queue_inference_output(
        self, output_audio: Any, text: str | None, output_count: int
    ) -> int:
        """Queue inference output to output buffer."""
        if output_audio is not None:
            output_count += 1
            if output_count <= 3:
                _log_progress(f"Server queuing output {output_count}")
            try:
                self.output_buffer.put_nowait(output_audio.tobytes())
            except asyncio.QueueFull:
                logger.debug("Output buffer full, dropping frame")
        if text:
            try:
                self.output_buffer.put_nowait(json.dumps({"type": "text", "text": text}))
            except asyncio.QueueFull:
                pass
        if self.state.stats.frames_processed % 10 == 0:
            try:
                self.output_buffer.put_nowait(
                    json.dumps({"type": "stats", **self.state.stats.to_dict()})
                )
            except asyncio.QueueFull:
                pass
        return output_count

    async def send_loop(self) -> None:
        """Send audio frames and messages to client."""
        send_count = 0
        try:
            _log_progress("Server send_loop started")
            while not self.close:
                await asyncio.sleep(0.001)
                try:
                    message = self.output_buffer.get_nowait()
                    await self.websocket.send(message)
                    send_count += 1
                    if send_count <= 3:
                        _log_progress(f"Server sent message {send_count}")
                except asyncio.QueueEmpty:
                    continue
        except Exception as e:
            if not self.close:
                _log_progress(f"Server send_loop error: {e}")
        finally:
            _log_progress(f"Server send_loop exiting, sent {send_count}")
            self.close = True


async def handle_websocket(websocket: Any, state: ServerState) -> None:
    """
    Handle a WebSocket connection with 3-task pattern.

    Uses concurrent recv/process/send tasks matching PersonaPlex demo
    to prevent event loop blocking and maintain low latency.
    """
    import torch

    logger.info(f"Client connected: {websocket.remote_address}")
    async with state.lock:
        state.mimi.reset_streaming()
        state.lm_gen.reset_streaming()
        state.stats = ServerStats()
    await websocket.send(json.dumps({"type": "ready"}))

    handler = _WebSocketHandler(websocket, state)
    tasks = [
        asyncio.create_task(handler.recv_loop()),
        asyncio.create_task(handler.inference_loop()),
        asyncio.create_task(handler.send_loop()),
    ]
    try:
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    finally:
        handler.close = True
        for task in tasks:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        logger.info("Client disconnected")
        if torch.cuda.is_available():
            torch.cuda.empty_cache()


async def _handle_control_message(
    message: str,
    state: ServerState,
    websocket: Any,
) -> None:
    """Handle JSON control messages from client."""
    try:
        msg = json.loads(message)
        msg_type = msg.get("type")

        if msg_type == "set_prompt":
            # Update text prompt
            text_prompt = msg.get("text", "")
            if text_prompt:
                async with state.lock:
                    tokens = state.text_tokenizer.encode(f"<system> {text_prompt} <system>")
                    state.lm_gen.text_prompt_tokens = tokens
            logger.info(f"Updated text prompt: {text_prompt[:50]}...")

        elif msg_type == "reset":
            # Reset streaming state
            async with state.lock:
                state.mimi.reset_streaming()
                state.lm_gen.reset_streaming()
                state.stats = ServerStats()
            await websocket.send(json.dumps({"type": "reset_ack"}))

        elif msg_type == "get_stats":
            await websocket.send(json.dumps({"type": "stats", **state.stats.to_dict()}))

    except json.JSONDecodeError:
        logger.warning(f"Invalid JSON message: {message}")


async def run_server(
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    device: str = "cuda",
    quantize_int8: bool = True,
    context_window: int = 500,
    voice_name: str = "NATF2",
) -> None:
    """Run the voice server."""
    _log_progress(f"Voice server starting (device={device}, int8={quantize_int8})...")

    try:
        import websockets
    except ImportError:
        _log_progress("ERROR: websockets package required. Install with: pip install websockets")
        return

    # Load models
    try:
        state = await load_models(
            device=device,
            quantize_int8=quantize_int8,
            context_window=context_window,
            voice_name=voice_name,
        )
    except Exception as e:
        _log_progress(f"ERROR loading models: {e}")
        raise

    # Start server
    _log_progress(f"Starting WebSocket server on ws://{host}:{port}")

    async with websockets.serve(
        lambda ws: handle_websocket(ws, state),
        host,
        port,
    ):
        _log_progress("Voice server ready and accepting connections!")
        await asyncio.Future()  # Run forever


def main() -> None:
    """CLI entry point for voice server."""
    parser = argparse.ArgumentParser(description="Entropi Voice Server")
    parser.add_argument("--host", default=DEFAULT_HOST, help="Host to bind to")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="Port to bind to")
    parser.add_argument("--device", default="cuda", help="Device (cuda/cpu)")
    parser.add_argument("--no-int8", action="store_true", help="Disable INT8 quantization")
    parser.add_argument("--context", type=int, default=500, help="Context window size")
    parser.add_argument("--voice", default="NATF2", help="Voice prompt name")

    args = parser.parse_args()

    asyncio.run(
        run_server(
            host=args.host,
            port=args.port,
            device=args.device,
            quantize_int8=not args.no_int8,
            context_window=args.context,
            voice_name=args.voice,
        )
    )


if __name__ == "__main__":
    main()
