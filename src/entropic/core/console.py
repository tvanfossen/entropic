"""
Minimal engine console for headless output.

Replaces direct ``rich.Console`` usage in ``app.py`` so the engine
has zero dependency on ``rich``.  When the ``rich`` package is
available the console delegates to it for nicer formatting; otherwise
plain ``print()`` is used.

@brief Lightweight console abstraction for engine-side CLI output.
@version 1
"""

from __future__ import annotations

import contextlib
import sys
from typing import Any


class _NullStatus:
    """No-op context manager replacing ``rich.status.Status``."""

    def __init__(self, message: str) -> None:  # noqa: ARG002
        pass

    def update(self, message: str) -> None:  # noqa: ARG002
        """No-op status update."""

    def __enter__(self) -> _NullStatus:
        return self

    def __exit__(self, *_args: Any) -> None:
        pass


class EngineConsole:
    """Thin console wrapper used by the engine.

    @brief Print helper that optionally delegates to ``rich.Console``.
    @version 1

    Tries to import ``rich`` at construction time.  If unavailable,
    falls back to plain ``print()``.
    """

    def __init__(self) -> None:
        """Initialise, probing for ``rich`` availability."""
        self._rich_console: Any | None = None
        with contextlib.suppress(ImportError):
            from rich.console import Console

            self._rich_console = Console()

    def print(self, *args: Any, **kwargs: Any) -> None:
        """Print to stdout, using rich if available.

        @brief Print with optional rich formatting.
        @version 1
        """
        if self._rich_console is not None:
            self._rich_console.print(*args, **kwargs)
        else:
            # Strip rich markup for plain output
            text = " ".join(str(a) for a in args)
            end = kwargs.get("end", "\n")
            sys.stdout.write(text + end)
            sys.stdout.flush()

    def status(self, message: str) -> Any:
        """Return a status spinner context manager.

        @brief Loading-spinner context manager (rich or no-op).
        @version 1
        """
        if self._rich_console is not None:
            from rich.status import Status

            return Status(message, console=self._rich_console)
        return _NullStatus(message)
