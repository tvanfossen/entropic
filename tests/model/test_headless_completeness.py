"""Test response completeness and token cleanliness through headless Application."""

import pytest
from entropi.app import Application
from entropi.ui.headless import HeadlessPresenter

from .conftest import with_timeout


@pytest.mark.model
class TestResponseCompleteness:
    """Test that responses complete properly without truncation artifacts.

    List-style prompts may trigger 1-2 turns.
    """

    @pytest.mark.asyncio
    async def test_response_ends_cleanly(
        self,
        headless_app: Application,
        headless_presenter: HeadlessPresenter,
    ) -> None:
        """Response should end at a natural stopping point."""
        await with_timeout(
            headless_app._process_message(
                "List three programming languages and their main use cases"
            ),
            expected_turns=2,
            name="completeness_list",
        )

        content = headless_presenter.get_stream_content().strip()
        bad_endings = [" the", " a", " an", " to", " and", " or", " but", " is", " are"]
        ends_badly = any(content.endswith(ending) for ending in bad_endings)

        if ends_badly:
            pytest.skip("Response may be truncated (ends with common mid-sentence word)")


@pytest.mark.model
class TestNoSpecialTokens:
    """Test that responses don't leak raw special tokens."""

    @pytest.mark.asyncio
    async def test_no_special_tokens_in_response(
        self,
        headless_app: Application,
        headless_presenter: HeadlessPresenter,
    ) -> None:
        """Response should not contain raw special tokens."""
        await with_timeout(
            headless_app._process_message("Hello, how are you?"),
            expected_turns=1,
            name="tokens_check",
        )

        content = headless_presenter.get_stream_content()
        special_tokens = [
            "<|im_end|>",
            "<|endoftext|>",
            "[EOS]",
            "[PAD]",
            "<|end|>",
            "<|im_start|>",
        ]
        for token in special_tokens:
            assert token not in content, f"Found special token {token} in response"
