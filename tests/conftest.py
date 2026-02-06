"""Pytest configuration and fixtures."""

import os
import tempfile
from pathlib import Path

import pytest
from entropi.config.loader import ConfigLoader
from entropi.config.schema import EntropyConfig
from entropi.inference.orchestrator import ModelOrchestrator, ModelTier

# =============================================================================
# Basic Fixtures
# =============================================================================


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


# =============================================================================
# Model Fixtures (for @pytest.mark.model tests)
# =============================================================================


def _check_model_available(model_path: str) -> bool:
    """Check if a model file exists."""
    expanded = os.path.expanduser(model_path)
    return os.path.exists(expanded)


@pytest.fixture(scope="session")
def config() -> EntropyConfig:
    """Load the entropi configuration from project config files."""
    loader = ConfigLoader()
    return loader.load()


@pytest.fixture(scope="session")
def models_available(config: EntropyConfig) -> dict[str, bool]:
    """Check which models are available on this system."""
    available = {}

    if config.models.router:
        available["router"] = _check_model_available(str(config.models.router.path))
    if config.models.simple:
        available["simple"] = _check_model_available(str(config.models.simple.path))
    if config.models.normal:
        available["normal"] = _check_model_available(str(config.models.normal.path))
    if config.models.code:
        available["code"] = _check_model_available(str(config.models.code.path))
    if config.models.thinking:
        available["thinking"] = _check_model_available(str(config.models.thinking.path))

    return available


@pytest.fixture(scope="session")
def router_available(models_available: dict[str, bool]) -> bool:
    """Check if router model is available."""
    return models_available.get("router", False)


@pytest.fixture(scope="module")
async def orchestrator(config: EntropyConfig, models_available: dict[str, bool]):
    """
    Create and initialize a model orchestrator.

    This is a module-scoped fixture to avoid loading models multiple times.
    Models are loaded once per test module and shared across tests.
    """
    # Skip if no models available
    if not any(models_available.values()):
        pytest.skip("No models available for testing")

    orch = ModelOrchestrator(config)
    await orch.initialize()

    yield orch

    await orch.shutdown()


@pytest.fixture
async def router_model(orchestrator: ModelOrchestrator, router_available: bool):
    """Get the router model for classification tests."""
    if not router_available:
        pytest.skip("Router model not available")

    return await orchestrator._get_model(ModelTier.ROUTER)


# =============================================================================
# Pytest Hooks
# =============================================================================


def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line("markers", "unit: Fast tests without external dependencies")
    config.addinivalue_line("markers", "integration: Tests requiring external services")
    config.addinivalue_line("markers", "model: Tests requiring actual model inference")
    config.addinivalue_line("markers", "slow: Tests that take a long time to run")


def pytest_collection_modifyitems(config, items):
    """Auto-mark tests based on their location."""
    for item in items:
        # Auto-mark tests in tests/unit as unit tests
        if "unit" in str(item.fspath):
            item.add_marker(pytest.mark.unit)

        # Auto-mark tests in tests/integration as integration tests
        if "integration" in str(item.fspath):
            item.add_marker(pytest.mark.integration)

        # Auto-mark tests in tests/model as model tests with timeout
        if "model" in str(item.fspath):
            item.add_marker(pytest.mark.model)
