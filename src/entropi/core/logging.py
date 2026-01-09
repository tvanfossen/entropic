"""
Logging infrastructure.

Provides consistent logging across all components with
file-based logging to keep the CLI clean.
"""

import logging
from pathlib import Path

from rich.console import Console
from rich.logging import RichHandler

from entropi.config.schema import EntropyConfig


def setup_logging(
    config: EntropyConfig,
    project_dir: Path | None = None,
) -> logging.Logger:
    """
    Set up logging infrastructure.

    Logs are written to .entropi/session.log in the project directory.
    Only warnings and errors are shown on console to keep CLI clean.

    Args:
        config: Application configuration
        project_dir: Project directory for log file location

    Returns:
        Root logger
    """
    # Create logger
    logger = logging.getLogger("entropi")
    logger.setLevel(config.log_level)

    # Clear existing handlers
    logger.handlers.clear()

    # Console handler - only show WARNING and above to keep CLI clean
    console_handler = RichHandler(
        console=Console(stderr=True),
        show_time=True,
        show_path=False,
        rich_tracebacks=True,
    )
    console_handler.setLevel(logging.WARNING)
    console_format = logging.Formatter("%(message)s")
    console_handler.setFormatter(console_format)
    logger.addHandler(console_handler)

    # File handler - always create in project's .entropi/ directory
    log_dir = (project_dir or Path.cwd()) / ".entropi"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / "session.log"

    # Use mode='w' to overwrite each session
    file_handler = logging.FileHandler(log_path, mode="w")
    file_handler.setLevel(config.log_level)
    file_format = logging.Formatter(
        "%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    file_handler.setFormatter(file_format)
    logger.addHandler(file_handler)

    # Log startup info
    logger.info(f"Session started - logging to {log_path}")
    logger.info(f"Log level: {config.log_level}")

    return logger


def get_logger(name: str) -> logging.Logger:
    """
    Get a logger for a specific component.

    Args:
        name: Component name (will be prefixed with 'entropi.')

    Returns:
        Logger instance
    """
    return logging.getLogger(f"entropi.{name}")
