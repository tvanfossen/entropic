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
    path = socket_path or DEFAULT_SOCKET_PATH

    # Resolve relative paths
    if not path.is_absolute():
        path = Path.cwd() / path
    path = path.resolve()

    if not path.exists():
        logger.error(f"Socket not found: {path}")
        logger.error("Make sure Entropi is running first.")
        return 1

    try:
        reader, writer = await asyncio.open_unix_connection(str(path))
        logger.info(f"Connected to Entropi at {path}")
    except Exception as e:
        logger.error(f"Failed to connect to socket: {e}")
        return 1

    # Create tasks for bidirectional forwarding
    stdin_task = asyncio.create_task(_forward_stdin_to_socket(writer))
    socket_task = asyncio.create_task(_forward_socket_to_stdout(reader))

    try:
        # Wait for either direction to complete (usually stdin EOF)
        done, pending = await asyncio.wait(
            [stdin_task, socket_task],
            return_when=asyncio.FIRST_COMPLETED,
        )

        # Cancel pending tasks
        for task in pending:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass

        # Check for errors
        for task in done:
            if task.exception():
                logger.error(f"Bridge error: {task.exception()}")
                return 1

    finally:
        writer.close()
        await writer.wait_closed()

    logger.info("Bridge disconnected")
    return 0


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
        line = await reader.readline()
        if not line:
            logger.info("socket EOF")
            break

        logger.debug(f"socket -> stdout: {line[:100]}...")
        sys.stdout.buffer.write(line)
        sys.stdout.buffer.flush()


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
