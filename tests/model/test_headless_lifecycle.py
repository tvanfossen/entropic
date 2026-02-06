"""Test generation lifecycle through headless Application."""

import pytest
from entropi.app import Application
from entropi.ui.headless import HeadlessPresenter

from .conftest import with_timeout


@pytest.mark.model
class TestGenerationLifecycle:
    """Test that generation starts, ends, and accumulates history correctly."""

    @pytest.mark.asyncio
    async def test_generation_starts_and_ends(
        self,
        headless_app: Application,
        headless_presenter: HeadlessPresenter,
    ) -> None:
        """Generation should properly start and end."""
        assert not headless_presenter._generating

        await with_timeout(
            headless_app._process_message("Hello"),
            expected_turns=1,
            name="lifecycle_hello",
        )

        assert not headless_presenter._generating

    @pytest.mark.asyncio
    async def test_multiple_messages_accumulate_history(
        self,
        headless_app: Application,
        headless_presenter: HeadlessPresenter,
    ) -> None:
        """Multiple messages should build conversation history."""
        await with_timeout(
            headless_app._process_message("Hello"),
            expected_turns=1,
            name="lifecycle_first",
        )
        first_response = headless_presenter.get_stream_content()
        headless_presenter.clear_captured()

        await with_timeout(
            headless_app._process_message("What did I just say?"),
            expected_turns=1,
            name="lifecycle_second",
        )
        second_response = headless_presenter.get_stream_content()

        assert len(first_response) > 0
        assert len(second_response) > 0
        assert len(headless_app._messages) >= 4
