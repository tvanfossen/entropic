"""
Model orchestrator for multi-model management.

Manages loading, routing, and lifecycle of multiple models.
"""

import asyncio
from collections.abc import AsyncIterator
from enum import Enum
from typing import Any

from entropi.config.schema import EntropyConfig, ModelConfig
from entropi.core.base import GenerationResult, Message, ModelBackend
from entropi.core.logging import get_logger
from entropi.inference.adapters.qwen import QwenAdapter
from entropi.inference.llama_cpp import LlamaCppBackend

logger = get_logger("inference.orchestrator")


class ModelTier(Enum):
    """Model tiers for routing."""

    PRIMARY = "primary"
    WORKHORSE = "workhorse"
    FAST = "fast"
    MICRO = "micro"


class ModelOrchestrator:
    """
    Orchestrates multiple models for intelligent routing.

    Manages model lifecycle, handles routing decisions,
    and provides a unified interface for generation.
    """

    def __init__(self, config: EntropyConfig) -> None:
        """
        Initialize orchestrator.

        Args:
            config: Application configuration
        """
        self.config = config
        self._models: dict[ModelTier, ModelBackend] = {}
        self._adapter = QwenAdapter()
        self._lock = asyncio.Lock()

    async def initialize(self) -> None:
        """Initialize and load configured models."""
        logger.info("Initializing model orchestrator")

        # Create backends for configured models
        models_config = self.config.models

        if models_config.primary:
            self._models[ModelTier.PRIMARY] = self._create_backend(models_config.primary)

        if models_config.workhorse:
            self._models[ModelTier.WORKHORSE] = self._create_backend(models_config.workhorse)

        if models_config.fast:
            self._models[ModelTier.FAST] = self._create_backend(models_config.fast)

        if models_config.micro:
            self._models[ModelTier.MICRO] = self._create_backend(models_config.micro)

        if not self._models:
            logger.warning("No models configured")
            return

        # Pre-load default model
        default_tier = ModelTier(models_config.default)
        if default_tier in self._models:
            await self._models[default_tier].load()
            logger.info(f"Pre-loaded {default_tier.value} model")

        # Pre-load micro model for routing (if enabled and available)
        if (
            self.config.routing.enabled
            and ModelTier.MICRO in self._models
            and default_tier != ModelTier.MICRO
        ):
            await self._models[ModelTier.MICRO].load()
            logger.info("Pre-loaded micro model for routing")

    def _create_backend(self, model_config: ModelConfig) -> ModelBackend:
        """Create a backend for a model configuration."""
        return LlamaCppBackend(
            config=model_config,
            chat_format=self._adapter.chat_format,
        )

    async def shutdown(self) -> None:
        """Shutdown and unload all models."""
        logger.info("Shutting down model orchestrator")

        for tier, model in self._models.items():
            if model.is_loaded:
                await model.unload()
                logger.info(f"Unloaded {tier.value} model")

    async def generate(
        self,
        messages: list[Message],
        tier: ModelTier | None = None,
        **kwargs: Any,
    ) -> GenerationResult:
        """
        Generate a response using the appropriate model.

        Args:
            messages: Conversation messages
            tier: Specific model tier to use (auto-routes if None)
            **kwargs: Additional generation parameters

        Returns:
            Generation result
        """
        # Determine which model to use
        if tier is None:
            tier = await self._route(messages)

        # Ensure model is loaded
        model = await self._get_model(tier)

        logger.debug(f"Generating with {tier.value} model")
        result = await model.generate(messages, **kwargs)

        # Parse tool calls from content
        cleaned_content, tool_calls = self._adapter.parse_tool_calls(result.content)
        result.content = cleaned_content
        result.tool_calls = tool_calls

        return result

    async def generate_stream(
        self,
        messages: list[Message],
        tier: ModelTier | None = None,
        **kwargs: Any,
    ) -> AsyncIterator[str]:
        """
        Generate a streaming response.

        Args:
            messages: Conversation messages
            tier: Specific model tier to use
            **kwargs: Additional generation parameters

        Yields:
            Response chunks
        """
        if tier is None:
            tier = await self._route(messages)

        model = await self._get_model(tier)

        logger.debug(f"Streaming with {tier.value} model")
        async for chunk in model.generate_stream(messages, **kwargs):
            yield chunk

    async def _get_model(self, tier: ModelTier) -> ModelBackend:
        """
        Get a model, loading if necessary.

        Args:
            tier: Model tier

        Returns:
            Model backend

        Raises:
            ValueError: If tier not configured
        """
        if tier not in self._models:
            # Fallback to default
            fallback = ModelTier(self.config.routing.fallback_model)
            if fallback not in self._models:
                raise ValueError(f"No model available for tier {tier.value}")
            tier = fallback
            logger.debug(f"Falling back to {tier.value} model")

        model = self._models[tier]
        if not model.is_loaded:
            await model.load()

        return model

    async def _route(self, messages: list[Message]) -> ModelTier:
        """
        Route request to appropriate model tier.

        Args:
            messages: Conversation messages

        Returns:
            Model tier to use
        """
        if not self.config.routing.enabled:
            return ModelTier(self.config.models.default)

        # Get last user message for routing decision
        user_message = ""
        for msg in reversed(messages):
            if msg.role == "user":
                user_message = msg.content
                break

        # Try heuristic routing first
        if self.config.routing.use_heuristics:
            heuristic_result = self._route_heuristic(user_message)
            if heuristic_result is not None:
                logger.debug(f"Heuristic routing: {heuristic_result.value}")
                return heuristic_result

        # Fall back to model-based classification
        return await self._route_with_model(user_message)

    def _route_heuristic(self, message: str) -> ModelTier | None:
        """
        Route using heuristics (no model call).

        Args:
            message: User message

        Returns:
            Model tier or None if heuristics inconclusive
        """
        message_lower = message.lower()
        routing_config = self.config.routing

        # Short, simple questions -> FAST
        words = message.split()
        if len(words) <= routing_config.simple_query_max_tokens:
            simple_patterns = [
                "what is",
                "what's",
                "who is",
                "define",
                "explain",
                "how do i",
            ]
            if any(message_lower.startswith(p) for p in simple_patterns):
                return ModelTier.FAST

        # Complex keywords -> PRIMARY
        if any(kw in message_lower for kw in routing_config.complex_keywords):
            return ModelTier.PRIMARY

        # Inconclusive
        return None

    async def _route_with_model(self, message: str) -> ModelTier:
        """
        Route using micro model for classification.

        Args:
            message: User message

        Returns:
            Model tier
        """
        fallback = ModelTier(self.config.routing.fallback_model)
        if ModelTier.MICRO not in self._models:
            return fallback

        response = await self._classify_with_micro(message)
        return self._parse_routing_response(response, fallback)

    async def _classify_with_micro(self, message: str) -> str:
        """Classify request using micro model."""
        classification_prompt = f"""Classify this request into one of these categories:
- PRIMARY: Complex coding tasks, architecture, detailed review
- FAST: Simple questions, explanations, small tasks

Request: {message}

Respond with only the category name (PRIMARY or FAST):"""

        micro = await self._get_model(ModelTier.MICRO)
        result = await micro.generate(
            [Message(role="user", content=classification_prompt)],
            max_tokens=10,
            temperature=0.0,
        )
        return result.content.strip().upper()

    def _parse_routing_response(self, response: str, fallback: ModelTier) -> ModelTier:
        """Parse routing response to determine model tier."""
        logger.debug(f"Model-based routing result: {response}")
        tier_map = {"PRIMARY": ModelTier.PRIMARY, "FAST": ModelTier.FAST}
        for keyword, tier in tier_map.items():
            if keyword in response:
                return tier
        return fallback

    @property
    def adapter(self) -> QwenAdapter:
        """Get the chat adapter."""
        return self._adapter

    def get_loaded_models(self) -> list[str]:
        """Get list of currently loaded model tiers."""
        return [tier.value for tier, model in self._models.items() if model.is_loaded]

    def get_available_models(self) -> list[str]:
        """Get list of configured (but not necessarily loaded) model tiers."""
        return [tier.value for tier in self._models.keys()]

    def count_tokens(self, text: str, tier: ModelTier | None = None) -> int:
        """
        Count tokens in text.

        Args:
            text: Text to count
            tier: Model tier to use for counting

        Returns:
            Token count
        """
        if tier is None:
            tier = ModelTier(self.config.models.default)

        if tier in self._models and self._models[tier].is_loaded:
            return self._models[tier].count_tokens(text)

        # Estimate if no model loaded (~4 chars per token)
        return len(text) // 4
