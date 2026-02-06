"""Test reasoning/explanation responses through headless Application."""

import pytest
from entropi.app import Application
from entropi.ui.headless import HeadlessPresenter

from .conftest import with_timeout


@pytest.mark.model
class TestReasoningResponses:
    """Test NORMAL tier responses for explanations and questions.

    Reasoning prompts typically complete in 1-2 turns (explain, maybe look up context).
    """

    @pytest.mark.asyncio
    async def test_explanation_is_substantive(
        self,
        headless_app: Application,
        headless_presenter: HeadlessPresenter,
    ) -> None:
        """Explanation request should produce substantive response."""
        await with_timeout(
            headless_app._process_message("What is recursion in programming?"),
            expected_turns=2,
            name="reasoning_recursion",
        )

        content = headless_presenter.get_stream_content()
        assert len(content) > 100, f"Expected substantial explanation, got {len(content)} chars"

        recursion_words = ["function", "call", "itself", "base", "case"]
        has_concepts = any(word in content.lower() for word in recursion_words)
        assert has_concepts, f"Expected recursion concepts, got: {content[:300]}"
