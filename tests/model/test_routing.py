"""
Test model routing classification with real model inference.

These tests validate that the router model correctly classifies user prompts
into the appropriate model tiers (simple, code, normal, thinking).

Run with: pytest tests/model/test_routing.py -v
Skip with: pytest -m "not model"
"""

import pytest
from entropic.core.base import Message
from entropic.inference.orchestrator import ModelOrchestrator


@pytest.mark.model
class TestRoutingClassification:
    """Test task classification using router model."""

    # === SIMPLE tier tests (greetings) ===

    @pytest.mark.asyncio
    async def test_hello_routes_to_simple(self, orchestrator: ModelOrchestrator):
        """'Hello' should route to simple tier."""
        tier = await orchestrator.route([Message(role="user", content="Hello")])
        assert tier == "simple", f"Expected simple, got {tier}"

    @pytest.mark.asyncio
    async def test_hi_routes_to_simple(self, orchestrator: ModelOrchestrator):
        """'Hi' should route to simple tier."""
        tier = await orchestrator.route([Message(role="user", content="Hi")])
        assert tier == "simple", f"Expected simple, got {tier}"

    @pytest.mark.asyncio
    async def test_thanks_routes_to_simple(self, orchestrator: ModelOrchestrator):
        """'Thanks!' should route to simple tier."""
        tier = await orchestrator.route([Message(role="user", content="Thanks!")])
        assert tier == "simple", f"Expected simple, got {tier}"

    # === CODE tier tests ===

    @pytest.mark.asyncio
    async def test_write_code_routes_to_code(self, orchestrator: ModelOrchestrator):
        """Code writing request should route to code tier."""
        tier = await orchestrator.route(
            [Message(role="user", content="Write a Python function to calculate fibonacci numbers")]
        )
        assert tier == "code", f"Expected code, got {tier}"

    @pytest.mark.asyncio
    async def test_fix_bug_routes_to_code(self, orchestrator: ModelOrchestrator):
        """Bug fix request should route to code tier."""
        tier = await orchestrator.route(
            [Message(role="user", content="Fix the bug in the authentication module")]
        )
        assert tier == "code", f"Expected code, got {tier}"

    @pytest.mark.asyncio
    async def test_add_feature_routes_to_code(self, orchestrator: ModelOrchestrator):
        """Feature request should route to code tier."""
        tier = await orchestrator.route(
            [Message(role="user", content="Add a login button to the header component")]
        )
        assert tier == "code", f"Expected code, got {tier}"

    # === NORMAL (reasoning) tier tests ===

    @pytest.mark.asyncio
    async def test_explain_routes_to_reasoning(self, orchestrator: ModelOrchestrator):
        """Explanation request should route to normal (reasoning) tier."""
        tier = await orchestrator.route(
            [Message(role="user", content="Explain how HTTP cookies work")]
        )
        assert tier == "normal", f"Expected normal, got {tier}"

    @pytest.mark.asyncio
    async def test_question_routes_to_reasoning(self, orchestrator: ModelOrchestrator):
        """Question should route to normal (reasoning) tier."""
        tier = await orchestrator.route(
            [Message(role="user", content="What is the difference between TCP and UDP?")]
        )
        assert tier == "normal", f"Expected normal, got {tier}"

    # === THINKING (complex) tier tests ===

    @pytest.mark.asyncio
    async def test_complex_analysis_routes_to_thinking(
        self, orchestrator: ModelOrchestrator, models_available: dict
    ):
        """Complex analysis should route to thinking tier (if available)."""
        tier = await orchestrator.route(
            [
                Message(
                    role="user",
                    content="Analyze the trade-offs between microservices and monolithic "
                    "architecture for a high-traffic e-commerce platform, considering "
                    "scalability, maintainability, and team structure implications",
                )
            ]
        )

        if models_available.get("thinking", False):
            assert tier == "thinking", f"Expected thinking, got {tier}"
        else:
            # Falls back to normal if thinking not available
            assert tier == "normal", f"Expected normal fallback, got {tier}"


@pytest.mark.model
class TestClassificationSpeed:
    """Test that classification is fast (using small router model)."""

    @pytest.mark.asyncio
    async def test_classification_under_500ms(self, orchestrator: ModelOrchestrator):
        """Classification should complete in under 500ms."""
        import time

        messages = [Message(role="user", content="Hello, how are you?")]
        start = time.perf_counter()
        await orchestrator._classify_task(messages)
        elapsed_ms = (time.perf_counter() - start) * 1000

        # Router model (0.6B) should classify very quickly
        assert elapsed_ms < 500, f"Classification took {elapsed_ms:.0f}ms, expected <500ms"

    @pytest.mark.asyncio
    async def test_multiple_classifications_consistent(self, orchestrator: ModelOrchestrator):
        """Same prompt should classify consistently."""
        messages = [Message(role="user", content="Write a unit test for the login function")]
        results = []
        for _ in range(3):
            tier, _raw = await orchestrator._classify_task(messages)
            results.append(tier)

        # All results should be the same (deterministic with temp=0)
        assert all(r == results[0] for r in results), f"Inconsistent results: {results}"
        assert results[0] == "code", f"Expected code, got '{results[0]}'"
