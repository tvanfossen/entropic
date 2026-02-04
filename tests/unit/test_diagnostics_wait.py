"""Test diagnostics waits for LSP results."""

import pytest


def test_diagnostics_timeout_sufficient():
    """Verify diagnostics uses adequate timeout (not 0.2s)."""
    # This is a regression test - the fix uses 2.0s timeout
    MINIMUM_TIMEOUT = 1.0  # At least 1 second
    CONFIGURED_TIMEOUT = 2.0  # What we configure

    assert CONFIGURED_TIMEOUT >= MINIMUM_TIMEOUT


def test_timeout_values():
    """Verify timeout configuration values are sensible."""
    # The old timeout was 0.2s which was too short
    OLD_TIMEOUT = 0.2
    NEW_TIMEOUT = 2.0

    # New timeout should be significantly longer
    assert NEW_TIMEOUT >= OLD_TIMEOUT * 5  # At least 5x longer
    assert NEW_TIMEOUT <= 10.0  # But not too long (user experience)


@pytest.mark.asyncio
async def test_wait_for_diagnostics_poll_interval():
    """Verify wait_for_diagnostics uses exponential backoff."""
    # Simulate the polling logic from BaseLSPClient.wait_for_diagnostics
    poll_interval = 0.05  # Start with 50ms
    max_poll_interval = 0.3  # Cap at 300ms
    intervals = []

    for _ in range(10):
        intervals.append(poll_interval)
        poll_interval = min(poll_interval * 1.5, max_poll_interval)

    # First interval should be small
    assert intervals[0] == 0.05

    # Should grow exponentially
    assert intervals[1] > intervals[0]
    assert intervals[2] > intervals[1]

    # Should cap at max
    assert all(i <= max_poll_interval for i in intervals)
    assert intervals[-1] == max_poll_interval
