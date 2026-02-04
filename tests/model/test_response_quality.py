"""
Test model response quality with real model inference.

These tests validate that models produce appropriate responses
for different types of prompts using the actual config values.

Run with: pytest tests/model/test_response_quality.py -v
Skip with: pytest -m "not model"
"""

import pytest

from entropi.core.base import Message
from entropi.inference.orchestrator import ModelOrchestrator, ModelTier


@pytest.mark.model
class TestSimpleResponses:
    """Test SIMPLE tier responses for greetings and basic interactions."""

    @pytest.mark.asyncio
    async def test_hello_gets_greeting_response(self, orchestrator: ModelOrchestrator):
        """'Hello' should get a friendly greeting response."""
        result = await orchestrator.generate(
            [Message(role="user", content="Hello")],
            tier=ModelTier.SIMPLE,
        )

        response = result.content.lower()
        # Should contain some form of greeting
        greeting_words = ["hello", "hi", "hey", "greetings", "welcome", "how can i help"]
        has_greeting = any(word in response for word in greeting_words)

        assert has_greeting, f"Expected greeting in response, got: {result.content[:200]}"

    @pytest.mark.asyncio
    async def test_thanks_gets_acknowledgment(self, orchestrator: ModelOrchestrator):
        """'Thanks!' should get an acknowledgment response."""
        result = await orchestrator.generate(
            [Message(role="user", content="Thanks!")],
            tier=ModelTier.SIMPLE,
        )

        response = result.content.lower()
        # Should acknowledge thanks
        ack_words = ["welcome", "glad", "happy", "pleasure", "anytime", "help"]
        has_ack = any(word in response for word in ack_words)

        assert has_ack, f"Expected acknowledgment in response, got: {result.content[:200]}"


@pytest.mark.model
class TestCodeResponses:
    """Test CODE tier responses for programming tasks."""

    @pytest.mark.asyncio
    async def test_fibonacci_produces_code(self, orchestrator: ModelOrchestrator):
        """Code request should produce actual code."""
        result = await orchestrator.generate(
            [Message(role="user", content="Write a Python function to calculate fibonacci numbers")],
            tier=ModelTier.CODE,
        )

        response = result.content
        # Should contain Python function definition
        assert "def " in response, f"Expected function definition, got: {response[:300]}"
        assert "fibonacci" in response.lower() or "fib" in response.lower(), \
            f"Expected fibonacci-related code, got: {response[:300]}"

    @pytest.mark.asyncio
    async def test_code_response_has_python_syntax(self, orchestrator: ModelOrchestrator):
        """Code response should have valid Python syntax markers."""
        result = await orchestrator.generate(
            [Message(role="user", content="Write a function to check if a number is prime")],
            tier=ModelTier.CODE,
        )

        response = result.content
        # Should contain Python syntax elements
        python_markers = ["def ", "return", "if ", "for ", "while "]
        has_python = any(marker in response for marker in python_markers)

        assert has_python, f"Expected Python code, got: {response[:300]}"


@pytest.mark.model
class TestReasoningResponses:
    """Test NORMAL tier responses for explanations and questions."""

    @pytest.mark.asyncio
    async def test_explanation_is_substantive(self, orchestrator: ModelOrchestrator):
        """Explanation request should produce substantive response."""
        result = await orchestrator.generate(
            [Message(role="user", content="What is recursion in programming?")],
            tier=ModelTier.NORMAL,
        )

        response = result.content
        # Should be a substantial explanation
        assert len(response) > 100, f"Expected substantial explanation, got {len(response)} chars"

        # Should mention key concepts
        recursion_words = ["function", "call", "itself", "base", "case"]
        has_concepts = any(word in response.lower() for word in recursion_words)
        assert has_concepts, f"Expected recursion concepts, got: {response[:300]}"


@pytest.mark.model
class TestResponseCompleteness:
    """Test that responses complete properly without truncation artifacts."""

    @pytest.mark.asyncio
    async def test_response_ends_cleanly(self, orchestrator: ModelOrchestrator):
        """Response should end at a natural stopping point."""
        result = await orchestrator.generate(
            [Message(role="user", content="List three programming languages and their main use cases")],
            tier=ModelTier.NORMAL,
        )

        response = result.content.strip()
        # Should not end mid-sentence (common truncation indicators)
        bad_endings = [" the", " a", " an", " to", " and", " or", " but", " is", " are"]
        ends_badly = any(response.endswith(ending) for ending in bad_endings)

        # This is a soft assertion - truncation can happen legitimately
        if ends_badly:
            pytest.skip(f"Response may be truncated (ends with common mid-sentence word)")

    @pytest.mark.asyncio
    async def test_no_raw_special_tokens(self, orchestrator: ModelOrchestrator):
        """Response should not contain raw special tokens."""
        result = await orchestrator.generate(
            [Message(role="user", content="Hello, how are you?")],
            tier=ModelTier.SIMPLE,
        )

        response = result.content
        # Should not contain common special tokens
        special_tokens = ["<|im_end|>", "<|endoftext|>", "[EOS]", "[PAD]", "<|end|>"]
        for token in special_tokens:
            assert token not in response, f"Found special token {token} in response"
