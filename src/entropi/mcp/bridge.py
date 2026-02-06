"""
MCP bridge for stdio-socket communication.

Bridges stdio (used by Claude Code) to Unix socket (used by Entropi).
This is spawned by Claude Code as a subprocess.

Usage:
    entropi --mcp-bridge
"""

from __future__ import annotations

import asyncio
import sys
import threading
from pathlib import Path

from entropi.core.logging import get_logger

logger = get_logger("mcp.bridge")

# Default socket path (project-relative for Docker compatibility)
DEFAULT_SOCKET_PATH = Path.cwd() / ".entropi" / "mcp.sock"


async def run_bridge(socket_path: Path | None = None) -> int:
    """
    Run the MCP bridge.

    Reads JSON-RPC messages from stdin, forwards to socket,
    and writes responses back to stdout.

    Args:
        socket_path: Path to Unix socket (default: .entropi/mcp.sock in cwd)

    Returns:
        Exit code (0 for success)
    """
    path = _resolve_socket_path(socket_path)
    connection = await _connect_to_socket(path)
    if connection is None:
        return 1

    reader, writer = connection
    exit_code = await _run_bridge_loop(reader, writer)
    logger.info("Bridge disconnected")
    return exit_code


def _resolve_socket_path(socket_path: Path | None) -> Path:
    """Resolve socket path to absolute path."""
    path = socket_path or DEFAULT_SOCKET_PATH
    if not path.is_absolute():
        path = Path.cwd() / path
    return path.resolve()


async def _connect_to_socket(
    path: Path,
) -> tuple[asyncio.StreamReader, asyncio.StreamWriter] | None:
    """Connect to socket. Returns (reader, writer) or None on error."""
    if not path.exists():
        logger.error(f"Socket not found: {path}")
        logger.error("Make sure Entropi is running first.")
        return None
    try:
        reader, writer = await asyncio.open_unix_connection(str(path))
        logger.info(f"Connected to Entropi at {path}")
        return reader, writer
    except Exception as e:
        logger.error(f"Failed to connect to socket: {e}")
        return None


async def _run_bridge_loop(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> int:
    """Run the bridge forwarding loop. Returns exit code."""
    stdin_task = asyncio.create_task(_forward_stdin_to_socket(writer))
    socket_task = asyncio.create_task(_forward_socket_to_stdout(reader))

    try:
        done, pending = await asyncio.wait(
            [stdin_task, socket_task], return_when=asyncio.FIRST_COMPLETED
        )
        for task in pending:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        for task in done:
            if task.exception():
                logger.error(f"Bridge error: {task.exception()}")
                return 1
        return 0
    finally:
        writer.close()
        await writer.wait_closed()


async def _forward_stdin_to_socket(writer: asyncio.StreamWriter) -> None:
    """Forward stdin to socket using a thread for blocking reads."""
    loop = asyncio.get_event_loop()
    queue: asyncio.Queue[bytes | None] = asyncio.Queue()

    def _read_stdin() -> None:
        """Read stdin in a thread (blocking)."""
        try:
            while True:
                line = sys.stdin.buffer.readline()
                if not line:
                    logger.info("stdin EOF in thread")
                    asyncio.run_coroutine_threadsafe(queue.put(None), loop)
                    break
                logger.info(f"stdin read: {len(line)} bytes")
                asyncio.run_coroutine_threadsafe(queue.put(line), loop)
        except Exception as e:
            logger.error(f"stdin reader error: {e}")
            asyncio.run_coroutine_threadsafe(queue.put(None), loop)

    # Start stdin reader thread
    thread = threading.Thread(target=_read_stdin, daemon=True)
    thread.start()
    logger.info("stdin reader thread started")

    while True:
        line = await queue.get()
        if line is None:
            logger.info("stdin queue received EOF")
            break

        logger.info(f"stdin -> socket: {len(line)} bytes")
        writer.write(line)
        await writer.drain()


async def _forward_socket_to_stdout(reader: asyncio.StreamReader) -> None:
    """Forward socket to stdout."""
    logger.info("socket reader initialized, waiting for responses...")

    while True:
        try:
            line = await reader.readline()
            if not line:
                logger.info("socket EOF")
                break

            logger.info(f"socket -> stdout: {len(line)} bytes")
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()
        except Exception as e:
            logger.error(f"socket reader error: {e}")
            break


def main(socket_path: str | None = None) -> int:
    """
    Main entry point for the bridge.

    Args:
        socket_path: Optional socket path override

    Returns:
        Exit code
    """
    path = Path(socket_path) if socket_path else None
    return asyncio.run(run_bridge(path))


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Entropi MCP Bridge")
    parser.add_argument(
        "--socket",
        type=str,
        help="Path to Unix socket",
        default=None,
    )
    args = parser.parse_args()

    sys.exit(main(args.socket))
