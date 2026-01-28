"""
Model orchestrator for multi-model management.

Manages loading, routing, and lifecycle of multiple models.
"""

import asyncio
from collections.abc import AsyncIterator
from enum import Enum
from pathlib import Path
from typing import Any

from entropi.config.schema import EntropyConfig, ModelConfig
from entropi.core.base import GenerationResult, Message, ModelBackend
from entropi.core.logging import get_logger
from entropi.inference.adapters import ChatAdapter
from entropi.inference.llama_cpp import LlamaCppBackend
from entropi.prompts import get_classification_prompt

logger = get_logger("inference.orchestrator")

# Grammar files directory
_GRAMMAR_DIR = Path(__file__).parent.parent / "data" / "grammars"


def load_grammar(name: str) -> str:
    """
    Load a GBNF grammar file by name.

    Args:
        name: Grammar name (e.g., "classification" loads "classification.gbnf")

    Returns:
        Grammar content string

    Raises:
        FileNotFoundError: If grammar file not found
    """
    path = _GRAMMAR_DIR / f"{name}.gbnf"
    if not path.exists():
        raise FileNotFoundError(f"Grammar file not found: {path}")

    # Read and strip comments for the actual grammar
    lines = path.read_text().splitlines()
    grammar_lines = [line for line in lines if line.strip() and not line.strip().startswith("#")]
    return "\n".join(grammar_lines)


class ModelTier(Enum):
    """Model tiers for task-specialized routing."""

    THINKING = "thinking"  # Deep reasoning (e.g., Qwen3-14B)
    NORMAL = "normal"  # General reasoning (e.g., Qwen3-8B)
    CODE = "code"  # Code generation (e.g., Qwen2.5-Coder-7B)
    SIMPLE = "simple"  # Simple responses (can share model with normal)
    ROUTER = "router"  # Classification only (small model, e.g., 0.5B)


# Load classification grammar from file
# Output: 1, 2, 3, or 4 (single-token for better accuracy)
CLASSIFICATION_GRAMMAR = load_grammar("classification")

# Map numeric output to category names
CLASSIFICATION_MAP = {
    "1": "simple",
    "2": "code",
    "3": "reasoning",
    "4": "complex",
}


class ModelOrchestrator:
    """
    Orchestrates multiple models for intelligent routing.

    Manages model lifecycle, handles routing decisions,
    and provides a unified interface for generation.
    """

    # Main model tiers that should be dynamically swapped (only one loaded at a time)
    MAIN_TIERS = frozenset({ModelTier.CODE, ModelTier.NORMAL, ModelTier.THINKING, ModelTier.SIMPLE})
    # Auxiliary tiers that stay loaded (small models)
    AUX_TIERS = frozenset({ModelTier.ROUTER})

    def __init__(self, config: EntropyConfig) -> None:
        """
        Initialize orchestrator.

        Args:
            config: Application configuration
        """
        self.config = config
        self._models: dict[ModelTier, LlamaCppBackend] = {}
        self._lock = asyncio.Lock()
        self._thinking_mode: bool = config.thinking.enabled
        self._last_used_tier: ModelTier | None = None
        self._loaded_main_tier: ModelTier | None = None  # Track which main model is loaded

    async def initialize(self) -> None:
        """Initialize and load configured models."""
        logger.info("Initializing model orchestrator")

        # Create backends for configured models
        models_config = self.config.models

        if models_config.thinking:
            self._models[ModelTier.THINKING] = self._create_backend(models_config.thinking)

        if models_config.normal:
            self._models[ModelTier.NORMAL] = self._create_backend(models_config.normal)

        if models_config.code:
            self._models[ModelTier.CODE] = self._create_backend(models_config.code)

        if models_config.simple:
            self._models[ModelTier.SIMPLE] = self._create_backend(models_config.simple)

        if models_config.router:
            self._models[ModelTier.ROUTER] = self._create_backend(models_config.router)

        if not self._models:
            logger.warning("No models configured")
            return

        # Pre-load only ONE main model (default) to maximize VRAM for context
        # Other main models are loaded on-demand via dynamic swapping
        default_tier = ModelTier(self.config.models.default)
        if self._thinking_mode and ModelTier.THINKING in self._models:
            default_tier = ModelTier.THINKING

        if default_tier in self._models:
            await self._models[default_tier].load()
            self._loaded_main_tier = default_tier
            logger.info(f"Pre-loaded {default_tier.value} model (default)")

        # Pre-load auxiliary models (small, always needed)
        if ModelTier.ROUTER in self._models:
            await self._models[ModelTier.ROUTER].load()
            logger.info("Pre-loaded ROUTER model")

    def _create_backend(self, model_config: ModelConfig) -> LlamaCppBackend:
        """Create a backend for a model configuration."""
        return LlamaCppBackend(
            config=model_config,
            prompts_dir=self.config.prompts_dir,
        )

    async def shutdown(self) -> None:
        """Shutdown and unload all models."""
        logger.info("Shutting down model orchestrator")

        for tier, model in self._models.items():
            if model.is_loaded:
                await model.unload()
                logger.info(f"Unloaded {tier.value} model")

    async def unload_all_models(self) -> None:
        """
        Unload all models to free VRAM.

        Used before loading other GPU-intensive components (e.g., voice mode).
        Call reload_default_models() to restore after.
        """
        import gc

        async with self._lock:
            for tier, model in self._models.items():
                if model.is_loaded:
                    await model.unload()
                    logger.info(f"Unloaded {tier.value} model for VRAM release")
            self._loaded_main_tier = None

        # Force garbage collection and CUDA cache clear
        gc.collect()
        try:
            import torch

            if torch.cuda.is_available():
                torch.cuda.empty_cache()
                torch.cuda.synchronize()
                logger.info("Cleared CUDA cache")
        except ImportError:
            pass

    async def reload_default_models(self) -> None:
        """
        Reload the default models after unload_all_models().

        Restores the same models that were loaded during initialize().
        """
        async with self._lock:
            # Determine default tier
            default_tier = ModelTier(self.config.models.default)
            if self._thinking_mode and ModelTier.THINKING in self._models:
                default_tier = ModelTier.THINKING

            # Load default main model
            if default_tier in self._models and not self._models[default_tier].is_loaded:
                await self._models[default_tier].load()
                self._loaded_main_tier = default_tier
                logger.info(f"Reloaded {default_tier.value} model")

            # Load router model
            if ModelTier.ROUTER in self._models and not self._models[ModelTier.ROUTER].is_loaded:
                await self._models[ModelTier.ROUTER].load()
                logger.info("Reloaded ROUTER model")

    # Thinking mode methods

    def get_thinking_mode(self) -> bool:
        """Get current thinking mode state."""
        return self._thinking_mode

    async def set_thinking_mode(self, enabled: bool) -> bool:
        """
        Toggle thinking mode.

        Args:
            enabled: Whether to enable thinking mode

        Returns:
            True if successful, False if model unavailable
        """
        if enabled == self._thinking_mode:
            return True

        async with self._lock:
            if enabled:
                # Enable thinking mode: load THINKING, optionally unload NORMAL
                if ModelTier.THINKING not in self._models:
                    logger.warning("THINKING model not configured")
                    return False

                # Unload NORMAL to free VRAM if needed
                if ModelTier.NORMAL in self._models and self._models[ModelTier.NORMAL].is_loaded:
                    await self._models[ModelTier.NORMAL].unload()
                    logger.info("Unloaded NORMAL model to free VRAM")

                # Load THINKING
                await self._models[ModelTier.THINKING].load()
                logger.info("Loaded THINKING model")
            else:
                # Disable thinking mode: load NORMAL, unload THINKING
                if ModelTier.NORMAL not in self._models:
                    logger.warning("NORMAL model not configured")
                    return False

                # Unload THINKING to free VRAM
                if (
                    ModelTier.THINKING in self._models
                    and self._models[ModelTier.THINKING].is_loaded
                ):
                    await self._models[ModelTier.THINKING].unload()
                    logger.info("Unloaded THINKING model")

                # Load NORMAL
                await self._models[ModelTier.NORMAL].load()
                logger.info("Loaded NORMAL model")

            self._thinking_mode = enabled
            return True

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

        # Track last used tier for adapter lookup
        self._last_used_tier = tier

        # Ensure model is loaded
        model = await self._get_model(tier)

        # Apply default max_tokens from config if not specified
        if "max_tokens" not in kwargs:
            kwargs["max_tokens"] = self.config.generation.max_tokens

        adapter_name = type(model.adapter).__name__
        logger.debug(f"Generating with {tier.value} model (adapter: {adapter_name})")
        result = await model.generate(messages, **kwargs)

        # Parse tool calls and clean content (removes <think> blocks, etc.)
        if result.content:
            cleaned_content, parsed_calls = model.adapter.parse_tool_calls(result.content)
            # Always use cleaned content (removes <think> blocks regardless of tool calls)
            result.content = cleaned_content
            if parsed_calls:
                result.tool_calls = parsed_calls
                logger.debug(f"Parsed tool calls from content: {[tc.name for tc in parsed_calls]}")

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

        # Track last used tier for adapter lookup
        self._last_used_tier = tier

        model = await self._get_model(tier)

        # Apply default max_tokens from config if not specified
        if "max_tokens" not in kwargs:
            kwargs["max_tokens"] = self.config.generation.max_tokens

        adapter_name = type(model.adapter).__name__
        logger.debug(f"Streaming with {tier.value} model (adapter: {adapter_name})")
        async for chunk in model.generate_stream(messages, **kwargs):
            yield chunk

    async def _get_model(self, tier: ModelTier) -> ModelBackend:
        """
        Get a model, loading if necessary.

        For main model tiers (CODE, NORMAL, THINKING), only one is loaded at a time.
        When switching between main tiers, the previous one is unloaded first.
        Auxiliary tiers (MICRO) stay loaded.

        Optimization: If two tiers point to the same model file, no swap needed.

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

        # Dynamic swapping for main tiers: unload current before loading new
        if tier in self.MAIN_TIERS and not model.is_loaded:
            async with self._lock:
                # Double-check after acquiring lock
                if not model.is_loaded:
                    # Check if a model with the same file path is already loaded
                    if self._loaded_main_tier:
                        current = self._models.get(self._loaded_main_tier)
                        if current and current.is_loaded:
                            if current.config.path == model.config.path:
                                # Same file already loaded - reuse it
                                if self._loaded_main_tier != tier:
                                    logger.info(
                                        f"Reusing {self._loaded_main_tier.value} model for {tier.value} "
                                        f"(same file: {model.config.path.name})"
                                    )
                                return current  # Return the already-loaded model
                            else:
                                # Different file - need to swap
                                logger.info(
                                    f"Unloading {self._loaded_main_tier.value} model for swap"
                                )
                                await current.unload()

                    # Load the requested model
                    logger.info(f"Loading {tier.value} model")
                    await model.load()
                    self._loaded_main_tier = tier

        elif not model.is_loaded:
            # Auxiliary tier - just load it
            await model.load()

        return model

    async def _route(self, messages: list[Message]) -> ModelTier:
        """
        Route request to appropriate model tier based on task type.

        Task routing:
        - SIMPLE (greetings, thanks) -> MICRO model (fast, no swap)
        - CODE (write/edit code) -> CODE model
        - REASONING (standard questions) -> NORMAL model (or THINKING if forced)
        - COMPLEX (deep analysis) -> THINKING model (auto, no toggle needed)

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

        # Classify task type
        task_type = await self._classify_task(user_message)
        logger.debug(f"Task classification: {task_type}")

        if task_type == "simple":
            return ModelTier.SIMPLE

        if task_type == "code":
            return ModelTier.CODE

        if task_type == "complex":
            # Complex tasks auto-route to THINKING (if available)
            if ModelTier.THINKING in self._models:
                return ModelTier.THINKING
            return ModelTier.NORMAL  # Fallback if no THINKING model

        # Standard reasoning: use THINKING only if user forced it on
        if self._thinking_mode and ModelTier.THINKING in self._models:
            return ModelTier.THINKING
        return ModelTier.NORMAL

    async def _classify_task(self, message: str) -> str:
        """
        Classify task type using the MICRO model.

        The MICRO model (0.5B) with GBNF grammar constraint is the single
        source of truth for task classification. It's fast (~10-50ms for
        one token) and more accurate than keyword heuristics.

        Args:
            message: User message

        Returns:
            Task type: 'simple', 'code', 'reasoning', or 'complex'
        """
        if ModelTier.ROUTER in self._models:
            return await self._classify_task_with_micro(message)

        # Fallback if MICRO model not configured
        return "reasoning"

    async def _classify_task_with_micro(self, message: str) -> str:
        """
        Classify task type using micro model with GBNF grammar constraint.

        Uses grammar to guarantee output is one of: 1, 2, 3, 4
        which maps to: simple, code, reasoning, complex

        Args:
            message: User message

        Returns:
            Task type: 'simple', 'code', 'reasoning', or 'complex'
        """
        # Load classification prompt from prompts directory (allows user customization)
        classification_prompt = get_classification_prompt(message, self.config.prompts_dir)

        micro = await self._get_model(ModelTier.ROUTER)
        result = await micro.generate(
            [Message(role="user", content=classification_prompt)],
            max_tokens=10,
            temperature=0.0,
            grammar=CLASSIFICATION_GRAMMAR,
        )

        # Grammar guarantees output is 1, 2, 3, or 4
        response = result.content.strip()
        category = CLASSIFICATION_MAP.get(response, "reasoning")  # Default to reasoning
        logger.debug(f"Micro model classification: {response} -> {category}")

        return category

    def get_adapter(self, tier: ModelTier | None = None) -> ChatAdapter:
        """
        Get the chat adapter for a model tier.

        Args:
            tier: Model tier (uses last used tier, then default if None)

        Returns:
            Chat adapter for the model
        """
        if tier is None:
            # Use last used tier if available, otherwise default
            tier = self._last_used_tier or ModelTier(self.config.models.default)

        if tier in self._models:
            return self._models[tier].adapter

        # Fallback to generic adapter
        from entropi.inference.adapters import get_adapter

        return get_adapter("generic")

    @property
    def last_used_tier(self) -> ModelTier | None:
        """Get the last used model tier (for tier locking in agentic loops)."""
        return self._last_used_tier

    @property
    def last_finish_reason(self) -> str:
        """Get finish_reason from the last generation."""
        if self._last_used_tier and self._last_used_tier in self._models:
            return self._models[self._last_used_tier].last_finish_reason
        return "stop"

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
