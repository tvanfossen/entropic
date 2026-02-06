"""Test error handling through headless Application."""

import pytest
from entropi.app import Application
from entropi.ui.headless import HeadlessPresenter


@pytest.mark.model
class TestErrorCapture:
    """Test that errors are handled gracefully in headless mode."""

    @pytest.mark.asyncio
    async def test_status_command_works(
        self,
        headless_app: Application,
        headless_presenter: HeadlessPresenter,
    ) -> None:
        """The /status command should execute without errors."""
        await headless_app._handle_user_input("/status")
        _ = headless_presenter  # Used by app fixture

    @pytest.mark.asyncio
    async def test_generation_error_captured(
        self,
        headless_app: Application,
        headless_presenter: HeadlessPresenter,
    ) -> None:
        """Generation errors should be captured gracefully."""
        _ = headless_presenter  # Used by app fixture

        original_engine = headless_app._engine
        headless_app._engine = None

        try:
            with pytest.raises(AssertionError):
                await headless_app._process_message("Test")
        finally:
            headless_app._engine = original_engine
