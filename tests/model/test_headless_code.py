"""Test code generation responses through headless Application."""

import pytest
from entropi.app import Application
from entropi.ui.headless import HeadlessPresenter

from .conftest import with_timeout


@pytest.mark.model
class TestCodeResponses:
    """Test CODE tier responses for programming tasks.

    Code prompts non-deterministically trigger multiple agent turns: think → generate
    code → tool call to write/verify → confirmation. Expect up to ~7 turns when the
    model decides to use tools (vs. 1 turn for inline code).
    """

    @pytest.mark.asyncio
    async def test_code_request_produces_code(
        self,
        headless_app: Application,
        headless_presenter: HeadlessPresenter,
    ) -> None:
        """Code request should produce actual code."""
        await with_timeout(
            headless_app._process_message("Write a Python function to check if a number is even"),
            expected_turns=7,
            name="code_even",
        )

        content = headless_presenter.get_stream_content()
        assert len(content) > 0, "Expected code response"
        assert "def " in content, f"Expected function definition, got: {content[:300]}"

    @pytest.mark.asyncio
    async def test_code_response_has_python_syntax(
        self,
        headless_app: Application,
        headless_presenter: HeadlessPresenter,
    ) -> None:
        """Code response should have Python syntax markers."""
        await with_timeout(
            headless_app._process_message("Write a function that returns the sum of a list"),
            expected_turns=7,
            name="code_sum",
        )

        content = headless_presenter.get_stream_content()
        python_markers = ["def ", "return", "for ", "sum"]
        has_python = any(marker in content for marker in python_markers)
        assert has_python, f"Expected Python code, got: {content[:300]}"
