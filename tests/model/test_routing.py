"""
Test model routing classification with real model inference.

These tests validate that the router model correctly classifies user prompts
into the appropriate model tiers (SIMPLE, CODE, NORMAL, THINKING).

Run with: pytest tests/model/test_routing.py -v
Skip with: pytest -m "not model"
"""

import pytest
from entropi.core.base import Message
from entropi.inference.orchestrator import ModelOrchestrator, ModelTier


@pytest.mark.model
class TestRoutingClassification:
    """Test task classification using router model."""

    # === SIMPLE tier tests (greetings) ===

    @pytest.mark.asyncio
    async def test_hello_routes_to_simple(self, orchestrator: ModelOrchestrator):
        """'Hello' should route to SIMPLE tier."""
        tier = await orchestrator.route([Message(role="user", content="Hello")])
        assert tier == ModelTier.SIMPLE, f"Expected SIMPLE, got {tier}"

    @pytest.mark.asyncio
    async def test_hi_routes_to_simple(self, orchestrator: ModelOrchestrator):
        """'Hi' should route to SIMPLE tier."""
        tier = await orchestrator.route([Message(role="user", content="Hi")])
        assert tier == ModelTier.SIMPLE, f"Expected SIMPLE, got {tier}"

    @pytest.mark.asyncio
    async def test_thanks_routes_to_simple(self, orchestrator: ModelOrchestrator):
        """'Thanks!' should route to SIMPLE tier."""
        tier = await orchestrator.route([Message(role="user", content="Thanks!")])
        assert tier == ModelTier.SIMPLE, f"Expected SIMPLE, got {tier}"

    # === CODE tier tests ===

    @pytest.mark.asyncio
    async def test_write_code_routes_to_code(self, orchestrator: ModelOrchestrator):
        """Code writing request should route to CODE tier."""
        tier = await orchestrator.route(
            [Message(role="user", content="Write a Python function to calculate fibonacci numbers")]
        )
        assert tier == ModelTier.CODE, f"Expected CODE, got {tier}"

    @pytest.mark.asyncio
    async def test_fix_bug_routes_to_code(self, orchestrator: ModelOrchestrator):
        """Bug fix request should route to CODE tier."""
        tier = await orchestrator.route(
            [Message(role="user", content="Fix the bug in the authentication module")]
        )
        assert tier == ModelTier.CODE, f"Expected CODE, got {tier}"

    @pytest.mark.asyncio
    async def test_add_feature_routes_to_code(self, orchestrator: ModelOrchestrator):
        """Feature request should route to CODE tier."""
        tier = await orchestrator.route(
            [Message(role="user", content="Add a login button to the header component")]
        )
        assert tier == ModelTier.CODE, f"Expected CODE, got {tier}"

    # === NORMAL (reasoning) tier tests ===

    @pytest.mark.asyncio
    async def test_explain_routes_to_reasoning(self, orchestrator: ModelOrchestrator):
        """Explanation request should route to NORMAL (reasoning) tier."""
        tier = await orchestrator.route(
            [Message(role="user", content="Explain how HTTP cookies work")]
        )
        assert tier == ModelTier.NORMAL, f"Expected NORMAL, got {tier}"

    @pytest.mark.asyncio
    async def test_question_routes_to_reasoning(self, orchestrator: ModelOrchestrator):
        """Question should route to NORMAL (reasoning) tier."""
        tier = await orchestrator.route(
            [Message(role="user", content="What is the difference between TCP and UDP?")]
        )
        assert tier == ModelTier.NORMAL, f"Expected NORMAL, got {tier}"

    # === THINKING (complex) tier tests ===

    @pytest.mark.asyncio
    async def test_complex_analysis_routes_to_thinking(
        self, orchestrator: ModelOrchestrator, models_available: dict
    ):
        """Complex analysis should route to THINKING tier (if available)."""
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
            assert tier == ModelTier.THINKING, f"Expected THINKING, got {tier}"
        else:
            # Falls back to NORMAL if THINKING not available
            assert tier == ModelTier.NORMAL, f"Expected NORMAL fallback, got {tier}"


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
        assert results[0] == ModelTier.CODE, f"Expected ModelTier.CODE, got '{results[0]}'"
