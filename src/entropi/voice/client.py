"""
WebSocket client for PersonaPlex voice server.

Connects to the standalone voice server for low-latency inference,
keeping the TUI responsive while Moshi runs in a dedicated process.
"""

from __future__ import annotations

import asyncio
import json
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

import numpy as np

from entropi.core.logging import get_logger
from entropi.voice.audio_io import create_audio_io

if TYPE_CHECKING:
    from entropi.config.schema import VoiceConfig

logger = get_logger("voice.client")

# Server defaults
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765
SAMPLE_RATE = 24000
FRAME_SIZE = 1920  # 80ms at 24kHz


@dataclass
class VoiceStats:
    """Voice session statistics."""

    frames_sent: int = 0
    frames_received: int = 0
    server_fps: float = 0.0
    server_latency_ms: float = 0.0
    client_latency_ms: float = 0.0
    last_update: float = field(default_factory=time.time)

    def to_dict(self) -> dict[str, Any]:
        return {
            "frames_sent": self.frames_sent,
            "frames_received": self.frames_received,
            "server_fps": round(self.server_fps, 1),
            "server_latency_ms": round(self.server_latency_ms, 1),
            "client_latency_ms": round(self.client_latency_ms, 1),
        }


@dataclass
class VoiceCallbacks:
    """Callbacks for voice events."""

    on_text: Callable[[str], None] | None = None
    on_state_change: Callable[[str], None] | None = None
    on_stats_update: Callable[[VoiceStats], None] | None = None
    on_input_level: Callable[[float], None] | None = None
    on_output_level: Callable[[float], None] | None = None
    on_error: Callable[[str], None] | None = None
    on_loading_progress: Callable[[str], None] | None = None


class VoiceClient:
    """
    WebSocket client for voice server.

    Manages connection to the voice server, audio I/O, and callbacks.
    Runs audio processing in background tasks to keep the TUI responsive.
    """

    def __init__(
        self,
        config: VoiceConfig,
        host: str = DEFAULT_HOST,
        port: int = DEFAULT_PORT,
    ) -> None:
        self._config = config
        self._host = host
        self._port = port

        self._websocket: Any = None
        self._audio_io = create_audio_io()
        self._running = False
        self._connected = False

        self._callbacks = VoiceCallbacks()
        self._stats = VoiceStats()

        # Background tasks
        self._send_task: asyncio.Task[None] | None = None
        self._recv_task: asyncio.Task[None] | None = None
        self._progress_task: asyncio.Task[None] | None = None

        # Server process (if we start it ourselves)
        self._server_proc: subprocess.Popen[bytes] | None = None
        self._startup_timeout: int = (
            600  # Default 10 min for model loading, can be overridden by config
        )

    @property
    def stats(self) -> VoiceStats:
        return self._stats

    @property
    def is_connected(self) -> bool:
        return self._connected

    def set_callbacks(self, callbacks: VoiceCallbacks) -> None:
        """Set event callbacks."""
        self._callbacks = callbacks

        # Set audio level callbacks
        self._audio_io.set_level_callbacks(
            on_input=callbacks.on_input_level,
            on_output=callbacks.on_output_level,
        )

    async def start_server(self) -> bool:
        """Start the voice server as a subprocess with progress monitoring."""
        if self._server_proc is not None:
            return True

        try:
            self._configure_startup_timeout()
            self._start_server_process()
            self._progress_task = asyncio.create_task(self._monitor_server_progress())
            return await self._wait_for_server_ready()
        except Exception as e:
            logger.error(f"Failed to start voice server: {e}")
            if self._callbacks.on_error:
                self._callbacks.on_error(str(e))
            return False

    def _configure_startup_timeout(self) -> None:
        """Configure startup timeout from config if available."""
        if hasattr(self._config, "server") and hasattr(
            self._config.server, "startup_timeout_seconds"
        ):
            self._startup_timeout = self._config.server.startup_timeout_seconds

    def _start_server_process(self) -> None:
        """Build command and start the server subprocess."""
        cmd = [
            sys.executable,
            "-m",
            "entropi.voice.server",
            "--host",
            self._host,
            "--port",
            str(self._port),
            "--device",
            self._config.runtime.device,
            "--context",
            str(self._config.runtime.context_window),
            "--voice",
            self._config.voice_prompt.voice_name,
        ]
        if self._config.runtime.quantization != "int8":
            cmd.append("--no-int8")

        logger.info(f"Starting voice server: {' '.join(cmd)}")
        if self._callbacks.on_loading_progress:
            self._callbacks.on_loading_progress("Starting voice server subprocess...")

        self._server_proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    async def _wait_for_server_ready(self) -> bool:
        """Wait for server to become ready. Returns True on success."""
        check_iterations = self._startup_timeout * 2  # Check every 0.5s
        for _ in range(check_iterations):
            await asyncio.sleep(0.5)

            if self._check_server_died():
                return False

            if await self._check_server_ready():
                logger.info("Voice server started successfully")
                if self._callbacks.on_loading_progress:
                    self._callbacks.on_loading_progress("Voice server ready!")
                return True

        logger.error(f"Voice server failed to start within {self._startup_timeout}s timeout")
        if self._callbacks.on_error:
            self._callbacks.on_error(f"Server startup timeout ({self._startup_timeout}s)")
        return False

    def _check_server_died(self) -> bool:
        """Check if server process has died. Returns True if dead."""
        if self._server_proc is None or self._server_proc.poll() is None:
            return False

        returncode = self._server_proc.returncode
        logger.error(f"Voice server process exited with code {returncode}")
        self._log_server_stderr()
        if self._callbacks.on_error:
            self._callbacks.on_error(f"Server exited with code {returncode}")
        return True

    def _log_server_stderr(self) -> None:
        """Log server stderr if available."""
        try:
            if self._server_proc and self._server_proc.stderr:
                stderr_output = self._server_proc.stderr.read().decode()
                if stderr_output:
                    logger.error(f"Server stderr: {stderr_output[:500]}")
                    if self._callbacks.on_loading_progress:
                        self._callbacks.on_loading_progress(f"Server error: {stderr_output[:200]}")
        except Exception:
            pass

    async def _monitor_server_progress(self) -> None:
        """Parse server stderr for loading progress messages."""
        if not self._server_proc or not self._server_proc.stderr:
            return

        loop = asyncio.get_event_loop()
        try:
            while self._server_proc and self._server_proc.poll() is None:
                line = await loop.run_in_executor(None, self._server_proc.stderr.readline)
                if not line:
                    await asyncio.sleep(0.1)
                    continue
                self._process_server_line(line)
        except asyncio.CancelledError:
            pass
        except Exception as e:
            logger.debug(f"Progress monitor error: {e}")

    def _process_server_line(self, line: bytes) -> None:
        """Process a single line from server stderr."""
        try:
            decoded = line.decode().strip()
            if decoded:
                self._maybe_report_progress(decoded)
                logger.debug(f"Server: {decoded}")
        except Exception as e:
            logger.debug(f"Failed to decode server output: {e}")

    def _maybe_report_progress(self, message: str) -> None:
        """Report progress message if it's a loading-related message."""
        if not self._callbacks.on_loading_progress:
            return
        lower = message.lower()
        if self._is_debug_output(lower):
            return
        if self._is_loading_message(lower):
            self._callbacks.on_loading_progress(message)

    def _is_debug_output(self, lower: str) -> bool:
        """Check if message is debug/traceback output to skip."""
        if lower.startswith("file ") or lower.startswith("traceback"):
            return True
        return "site-packages" in lower or "line " in lower

    def _is_loading_message(self, lower: str) -> bool:
        """Check if message is a loading-related progress message."""
        keywords = [
            "loading",
            "downloading",
            "initializing",
            "warming",
            "models loaded",
            "ready",
            "starting",
        ]
        return any(kw in lower for kw in keywords)

    async def _check_server_ready(self) -> bool:
        """Check if server is ready to accept connections."""
        try:
            import websockets

            async with websockets.connect(
                f"ws://{self._host}:{self._port}",
                open_timeout=1,
            ) as ws:
                # Wait for ready message
                msg = await asyncio.wait_for(ws.recv(), timeout=1)
                data = json.loads(msg)
                return bool(data.get("type") == "ready")
        except Exception:
            return False

    async def connect(self) -> bool:
        """Connect to the voice server."""
        try:
            import websockets
        except ImportError:
            logger.error("websockets package required")
            return False

        success = await self._do_connect(websockets)
        if success and self._callbacks.on_state_change:
            self._callbacks.on_state_change("connected")
        return success

    async def _do_connect(self, websockets: Any) -> bool:
        """Perform the actual connection. Returns True on success."""
        try:
            self._websocket = await websockets.connect(
                f"ws://{self._host}:{self._port}", max_size=None
            )
            msg = await asyncio.wait_for(self._websocket.recv(), timeout=5)
            data = json.loads(msg)

            if data.get("type") != "ready":
                logger.error(f"Unexpected server message: {data}")
                return False

            self._connected = True
            logger.info("Connected to voice server")
            return True
        except Exception as e:
            logger.error(f"Failed to connect to voice server: {e}")
            if self._callbacks.on_error:
                self._callbacks.on_error(str(e))
            return False

    async def start(self) -> bool:
        """Start voice session (audio + networking)."""
        if self._running:
            return True

        if not self._connected:
            if not await self.connect():
                return False

        self._running = True

        # Start audio I/O
        await self._audio_io.start()

        # Start background tasks
        self._send_task = asyncio.create_task(self._send_loop())
        self._recv_task = asyncio.create_task(self._recv_loop())

        if self._callbacks.on_state_change:
            self._callbacks.on_state_change("running")

        logger.info("Voice session started")
        return True

    async def _send_loop(self) -> None:
        """Background task to send audio frames to server."""
        logger.info("Client send_loop started")
        frames_sent = 0
        while self._running and self._websocket:
            try:
                # Read audio frame
                frame = await self._audio_io.read_frame(timeout=0.1)
                if frame is None:
                    continue

                # Send to server as raw bytes
                start_time = time.time()
                await self._websocket.send(frame.tobytes())
                self._stats.frames_sent += 1
                frames_sent += 1
                self._stats.client_latency_ms = (time.time() - start_time) * 1000

                if frames_sent <= 3 or frames_sent % 100 == 0:
                    logger.info(f"Client sent frame {frames_sent}, size={len(frame)}")

            except asyncio.CancelledError:
                break
            except Exception as e:
                if self._running:
                    logger.warning(f"Send error: {e}")
                break
        logger.info(f"Client send_loop exiting, sent {frames_sent} frames")

    async def _recv_loop(self) -> None:
        """Background task to receive from server."""
        logger.info("Client recv_loop started")
        frames_received = 0
        messages_received = 0
        while self._running and self._websocket:
            try:
                message = await asyncio.wait_for(self._websocket.recv(), timeout=0.5)
                if isinstance(message, bytes):
                    frames_received = await self._handle_audio_message(message, frames_received)
                elif isinstance(message, str):
                    self._handle_json_message(message)
                    messages_received += 1
            except TimeoutError:
                continue
            except asyncio.CancelledError:
                break
            except Exception as e:
                if self._running:
                    logger.warning(f"Receive error: {e}")
                break
        logger.info(
            f"Client recv_loop exiting, received {frames_received} audio, {messages_received} messages"
        )

    async def _handle_audio_message(self, message: bytes, frames_received: int) -> int:
        """Handle incoming audio frame."""
        audio = np.frombuffer(message, dtype=np.float32)
        await self._audio_io.write_frame(audio)
        self._stats.frames_received += 1
        frames_received += 1
        if frames_received <= 3 or frames_received % 100 == 0:
            logger.info(f"Client received audio frame {frames_received}, size={len(audio)}")
        return frames_received

    def _handle_json_message(self, message: str) -> None:
        """Handle incoming JSON message."""
        data = json.loads(message)
        msg_type = data.get("type")
        if msg_type == "text":
            self._handle_text_message(data)
        elif msg_type == "stats":
            self._handle_stats_message(data)

    def _handle_text_message(self, data: dict[str, Any]) -> None:
        """Handle text message from server."""
        text = data.get("text", "")
        if text and self._callbacks.on_text:
            self._callbacks.on_text(text)
            logger.info(f"Client received text: {text[:50]}")

    def _handle_stats_message(self, data: dict[str, Any]) -> None:
        """Handle stats message from server."""
        self._stats.server_fps = data.get("fps", 0)
        self._stats.server_latency_ms = data.get("avg_latency_ms", 0)
        if self._callbacks.on_stats_update:
            self._callbacks.on_stats_update(self._stats)

    async def stop(self) -> None:
        """Stop voice session."""
        if not self._running:
            return

        self._running = False

        # Cancel background tasks
        for task in [self._send_task, self._recv_task]:
            if task:
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass

        self._send_task = None
        self._recv_task = None

        # Stop audio
        await self._audio_io.stop()

        # Close websocket
        if self._websocket:
            await self._websocket.close()
            self._websocket = None

        self._connected = False

        if self._callbacks.on_state_change:
            self._callbacks.on_state_change("stopped")

        logger.info("Voice session stopped")

    async def stop_server(self) -> None:
        """Stop the voice server subprocess."""
        # Cancel progress monitor task
        if self._progress_task:
            self._progress_task.cancel()
            try:
                await self._progress_task
            except asyncio.CancelledError:
                pass
            self._progress_task = None

        if self._server_proc:
            self._server_proc.terminate()
            try:
                self._server_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._server_proc.kill()
            self._server_proc = None
            logger.info("Voice server stopped")

    async def set_prompt(self, text: str) -> None:
        """Set the text prompt on the server."""
        if self._websocket and self._connected:
            await self._websocket.send(
                json.dumps(
                    {
                        "type": "set_prompt",
                        "text": text,
                    }
                )
            )

    async def reset(self) -> None:
        """Reset the conversation state."""
        if self._websocket and self._connected:
            await self._websocket.send(json.dumps({"type": "reset"}))
            self._stats = VoiceStats()

    async def __aenter__(self) -> VoiceClient:
        await self.start()
        return self

    async def __aexit__(self, *args: Any) -> None:
        await self.stop()
