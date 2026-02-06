"""Model test fixtures for headless Application testing.

Architecture:
- Orchestrator is module-scoped: model loaded once per test file
- headless_app is function-scoped: fresh session per test, reuses model
- with_timeout computes timeout from expected turn count
"""

import asyncio
import time
from collections.abc import AsyncGenerator
from pathlib import Path

import pytest
from entropi.app import Application
from entropi.config.schema import EntropyConfig
from entropi.inference.orchestrator import ModelOrchestrator
from entropi.ui.headless import HeadlessPresenter

# Timing constants derived from observed performance
AVG_SECONDS_PER_TURN = 15
TURN_TIME_BUFFER = 1.5  # overestimate factor for variance


async def with_timeout(coro, expected_turns: int = 1, name: str = "operation"):
    """Run a coroutine with a timeout scaled to expected agent turns.

    Args:
        coro: Coroutine to run
        expected_turns: How many agent loop turns we expect
        name: Label for error messages

    Returns:
        Tuple of (result, elapsed_seconds)
    """
    timeout = expected_turns * AVG_SECONDS_PER_TURN * TURN_TIME_BUFFER
    start = time.perf_counter()
    try:
        async with asyncio.timeout(timeout):
            result = await coro
    except TimeoutError as e:
        elapsed = time.perf_counter() - start
        raise TimeoutError(
            f"Model test timeout: {name} took {elapsed:.1f}s, "
            f"expected <{timeout:.0f}s ({expected_turns} turns × "
            f"{AVG_SECONDS_PER_TURN}s × {TURN_TIME_BUFFER}x buffer)"
        ) from e
    elapsed = time.perf_counter() - start
    return result, elapsed


@pytest.fixture(scope="module")
async def shared_orchestrator(config: EntropyConfig, models_available: dict[str, bool]):
    """Module-scoped orchestrator. Model loaded once, shared across tests in a file."""
    if not any(models_available.values()):
        pytest.skip("No models available for testing")

    orch = ModelOrchestrator(config)
    await orch.initialize()

    yield orch

    await orch.shutdown()


@pytest.fixture
def headless_presenter() -> HeadlessPresenter:
    """Create a headless presenter for testing."""
    return HeadlessPresenter(auto_approve=True)


@pytest.fixture
async def headless_app(
    config: EntropyConfig,
    shared_orchestrator: ModelOrchestrator,
    headless_presenter: HeadlessPresenter,
    tmp_project_dir: Path,
) -> AsyncGenerator[Application, None]:
    """Create an Application with fresh session state, reusing loaded model.

    - Orchestrator (model) is shared across tests in the module
    - Session state (messages, conversation) is fresh per test
    """
    app = Application(
        config=config,
        project_dir=tmp_project_dir,
        presenter=headless_presenter,
        orchestrator=shared_orchestrator,
    )
    await app.initialize()

    yield app

    await app.shutdown()
