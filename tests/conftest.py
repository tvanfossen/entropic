"""Pytest configuration and fixtures.

@brief Root conftest — markers and basic fixtures only. No engine imports.
@version 2
"""

import tempfile
from pathlib import Path

import pytest


@pytest.fixture
def tmp_project_dir() -> Path:
    """Create a temporary project directory.

    @brief Yield a temp dir with .entropic/ structure.
    @version 1
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        project_dir = Path(tmpdir)
        entropic_dir = project_dir / ".entropic"
        entropic_dir.mkdir()
        yield project_dir


@pytest.fixture
def tmp_config_dir() -> Path:
    """Create a temporary config directory.

    @brief Yield a bare temp directory.
    @version 1
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        yield Path(tmpdir)


def pytest_configure(config):
    """Register custom markers.

    @brief Add unit/integration/model/slow markers.
    @version 1
    """
    config.addinivalue_line("markers", "unit: Fast tests without external dependencies")
    config.addinivalue_line("markers", "integration: Tests requiring external services")
    config.addinivalue_line("markers", "model: Tests requiring actual model inference")
    config.addinivalue_line("markers", "slow: Tests that take a long time to run")


def pytest_collection_modifyitems(config, items):
    """Auto-mark tests based on their location.

    @brief Apply markers based on test file path.
    @version 1
    """
    for item in items:
        if "unit" in str(item.fspath):
            item.add_marker(pytest.mark.unit)
        if "integration" in str(item.fspath):
            item.add_marker(pytest.mark.integration)
        if "model" in str(item.fspath):
            item.add_marker(pytest.mark.model)
