"""Pytest configuration and fixtures."""

import tempfile
from pathlib import Path

import pytest


@pytest.fixture
def tmp_project_dir() -> Path:
    """Create a temporary project directory."""
    with tempfile.TemporaryDirectory() as tmpdir:
        project_dir = Path(tmpdir)
        entropi_dir = project_dir / ".entropi"
        entropi_dir.mkdir()
        (entropi_dir / "commands").mkdir()
        yield project_dir


@pytest.fixture
def tmp_config_dir() -> Path:
    """Create a temporary config directory."""
    with tempfile.TemporaryDirectory() as tmpdir:
        yield Path(tmpdir)
