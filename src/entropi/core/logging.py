"""
Logging infrastructure.

Provides consistent logging across all components with
optional file output and rich console formatting.
"""

import logging
from pathlib import Path

from rich.console import Console
from rich.logging import RichHandler

from entropi.config.schema import EntropyConfig


def setup_logging(config: EntropyConfig) -> logging.Logger:
    """
    Set up logging infrastructure.

    Args:
        config: Application configuration

    Returns:
        Root logger
    """
    # Create logger
    logger = logging.getLogger("entropi")
    logger.setLevel(config.log_level)

    # Clear existing handlers
    logger.handlers.clear()

    # Console handler with Rich formatting
    console_handler = RichHandler(
        console=Console(stderr=True),
        show_time=True,
        show_path=False,
        rich_tracebacks=True,
    )
    console_handler.setLevel(config.log_level)
    console_format = logging.Formatter("%(message)s")
    console_handler.setFormatter(console_format)
    logger.addHandler(console_handler)

    # File handler (if configured)
    if config.log_file:
        log_path = Path(config.log_file).expanduser()
        log_path.parent.mkdir(parents=True, exist_ok=True)

        file_handler = logging.FileHandler(log_path)
        file_handler.setLevel(config.log_level)
        file_format = logging.Formatter("%(asctime)s - %(name)s - %(levelname)s - %(message)s")
        file_handler.setFormatter(file_format)
        logger.addHandler(file_handler)

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
