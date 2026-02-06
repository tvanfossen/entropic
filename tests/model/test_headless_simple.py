"""Test simple greeting/acknowledgment responses through headless Application."""

import pytest
from entropi.app import Application
from entropi.ui.headless import HeadlessPresenter

from .conftest import with_timeout


@pytest.mark.model
class TestSimpleResponses:
    """Test SIMPLE tier responses for greetings and basic interactions."""

    @pytest.mark.asyncio
    async def test_hello_produces_response(
        self,
        headless_app: Application,
        headless_presenter: HeadlessPresenter,
    ) -> None:
        """'Hello' should produce a greeting in a single turn."""
        _, elapsed = await with_timeout(
            headless_app._process_message("Hello"),
            expected_turns=1,
            name="simple_hello",
        )

        content = headless_presenter.get_stream_content()
        assert len(content) > 0, "Expected streaming response"

        response_lower = content.lower()
        greeting_indicators = ["hello", "hi", "hey", "help", "assist"]
        has_greeting = any(word in response_lower for word in greeting_indicators)
        assert has_greeting, f"Expected greeting response, got: {content[:200]}"

    @pytest.mark.asyncio
    async def test_thanks_produces_acknowledgment(
        self,
        headless_app: Application,
        headless_presenter: HeadlessPresenter,
    ) -> None:
        """'Thanks!' should produce an acknowledgment in a single turn."""
        _, elapsed = await with_timeout(
            headless_app._process_message("Thanks!"),
            expected_turns=1,
            name="simple_thanks",
        )

        content = headless_presenter.get_stream_content()
        assert len(content) > 0, "Expected response"

        response_lower = content.lower()
        ack_words = ["welcome", "glad", "happy", "help", "anytime"]
        has_ack = any(word in response_lower for word in ack_words)
        assert has_ack, f"Expected acknowledgment, got: {content[:200]}"
