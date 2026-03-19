"""
Logging infrastructure.

Provides consistent logging across all components with
file-based logging to keep the CLI clean.
"""

import logging
from pathlib import Path

from entropic.config.schema import LibraryConfig


def setup_logging(
    config: LibraryConfig,
    project_dir: Path | None = None,
    *,
    app_dir_name: str = ".entropic",
) -> logging.Logger:
    """
    Set up logging infrastructure.

    Logs are written to ``<project_dir>/<app_dir_name>/session.log``.
    Only warnings and errors are shown on console to keep CLI clean.

    Args:
        config: Application configuration
        project_dir: Project directory for log file location
        app_dir_name: Application directory name (default ``.entropic``).
            Consumer apps pass their own (e.g. ``.pychess``).

    Returns:
        Root logger
    """
    # Create logger
    logger = logging.getLogger("entropic")
    logger.setLevel(config.log_level)

    # Clear existing handlers
    logger.handlers.clear()

    # Console handler - only show WARNING and above to keep CLI clean
    console_handler = logging.StreamHandler()
    console_handler.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(message)s"))
    console_handler.setLevel(logging.WARNING)
    logger.addHandler(console_handler)

    # File handler - create in project's app directory
    log_dir = (project_dir or Path.cwd()) / app_dir_name
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


def setup_model_logger(
    project_dir: Path | None = None,
    *,
    app_dir_name: str = ".entropic",
) -> logging.Logger:
    """
    Set up dedicated model output logger.

    Writes raw model output to ``<project_dir>/<app_dir_name>/session_model.log``,
    separate from operational logs in session.log.

    Args:
        project_dir: Project directory for log file location
        app_dir_name: Application directory name (default ``.entropic``).
            Consumer apps pass their own (e.g. ``.pychess``).

    Returns:
        Model output logger
    """
    model_logger = logging.getLogger("entropic.model_output")
    model_logger.setLevel(logging.INFO)
    model_logger.handlers.clear()
    # Don't propagate to parent (entropic) logger — keeps session.log clean
    model_logger.propagate = False

    log_dir = (project_dir or Path.cwd()) / app_dir_name
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / "session_model.log"

    file_handler = logging.FileHandler(log_path, mode="w")
    file_handler.setLevel(logging.INFO)
    file_format = logging.Formatter(
        "%(asctime)s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    file_handler.setFormatter(file_format)
    model_logger.addHandler(file_handler)

    model_logger.info("Model output log started")
    return model_logger


def setup_display_logger(
    project_dir: Path | None = None,
    *,
    app_dir_name: str = ".entropic",
) -> logging.Logger:
    """
    Set up dedicated display mirror logger.

    Writes a text representation of TUI display events to
    ``<project_dir>/<app_dir_name>/session_display.log``,
    giving a readable transcript of what the user saw.

    Args:
        project_dir: Project directory for log file location
        app_dir_name: Application directory name (default ``.entropic``).
            Consumer apps pass their own (e.g. ``.pychess``).

    Returns:
        Display mirror logger
    """
    display_logger = logging.getLogger("entropic.display")
    display_logger.setLevel(logging.INFO)
    display_logger.handlers.clear()
    display_logger.propagate = False

    log_dir = (project_dir or Path.cwd()) / app_dir_name
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / "session_display.log"

    file_handler = logging.FileHandler(log_path, mode="w")
    file_handler.setLevel(logging.INFO)
    file_format = logging.Formatter(
        "%(asctime)s %(message)s",
        datefmt="%H:%M:%S",
    )
    file_handler.setFormatter(file_format)
    display_logger.addHandler(file_handler)

    display_logger.info("Display mirror log started")
    return display_logger


def get_display_logger() -> logging.Logger:
    """
    Get the dedicated display mirror logger.

    Returns:
        Display mirror logger (must call setup_display_logger first)
    """
    return logging.getLogger("entropic.display")


def get_logger(name: str) -> logging.Logger:
    """
    Get a logger for a specific component.

    Args:
        name: Component name (will be prefixed with 'entropic.')

    Returns:
        Logger instance
    """
    return logging.getLogger(f"entropic.{name}")


def get_model_logger() -> logging.Logger:
    """
    Get the dedicated model output logger.

    Returns:
        Model output logger (must call setup_model_logger first)
    """
    return logging.getLogger("entropic.model_output")
