"""
Model orchestrator for multi-model management.

Manages loading, routing, and lifecycle of multiple models.
"""

import asyncio
import time
from collections.abc import AsyncIterator
from dataclasses import dataclass
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


# Map numeric output to model tiers directly
CLASSIFICATION_MAP: dict[str, "ModelTier"] = {
    "1": ModelTier.SIMPLE,
    "2": ModelTier.CODE,
    "3": ModelTier.NORMAL,
    "4": ModelTier.THINKING,
}


@dataclass
class RoutingResult:
    """Metadata from a routing decision."""

    tier: ModelTier
    previous_tier: ModelTier | None = None
    model_raw: str = ""  # raw model output (e.g. "2")
    swap_action: str = "none"  # "none", "reused", "loaded"
    routing_ms: float = 0.0  # total routing time


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

    # Handoff routing rules - which tiers can hand off to which
    HANDOFF_RULES: dict[ModelTier, frozenset[ModelTier]] = {
        ModelTier.SIMPLE: frozenset({ModelTier.NORMAL, ModelTier.CODE, ModelTier.THINKING}),
        ModelTier.NORMAL: frozenset({ModelTier.SIMPLE, ModelTier.CODE, ModelTier.THINKING}),
        ModelTier.CODE: frozenset({ModelTier.SIMPLE, ModelTier.NORMAL}),
        ModelTier.THINKING: frozenset({ModelTier.NORMAL, ModelTier.CODE}),  # Never to SIMPLE
    }

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
        self._last_routed_tier: ModelTier | None = None  # Track last router-selected tier
        self._last_routing_result: RoutingResult | None = None

    async def initialize(self) -> None:
        """Initialize and load configured models."""
        logger.info("Initializing model orchestrator")

        # Create backends for configured models
        models_config = self.config.models

        if models_config.thinking:
            self._models[ModelTier.THINKING] = self._create_backend(
                models_config.thinking, ModelTier.THINKING
            )

        if models_config.normal:
            self._models[ModelTier.NORMAL] = self._create_backend(
                models_config.normal, ModelTier.NORMAL
            )

        if models_config.code:
            self._models[ModelTier.CODE] = self._create_backend(models_config.code, ModelTier.CODE)

        if models_config.simple:
            self._models[ModelTier.SIMPLE] = self._create_backend(
                models_config.simple, ModelTier.SIMPLE
            )

        if models_config.router:
            self._models[ModelTier.ROUTER] = self._create_backend(
                models_config.router, ModelTier.ROUTER
            )

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

    def _create_backend(self, model_config: ModelConfig, tier: ModelTier) -> LlamaCppBackend:
        """Create a backend for a model configuration."""
        return LlamaCppBackend(
            config=model_config,
            tier=tier.value,
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

        # Pre-validate required model exists
        required_tier = ModelTier.THINKING if enabled else ModelTier.NORMAL
        if required_tier not in self._models:
            logger.warning(f"{required_tier.name} model not configured")
            return False

        async with self._lock:
            if enabled:
                # Enable thinking mode: load THINKING, optionally unload NORMAL
                if ModelTier.NORMAL in self._models and self._models[ModelTier.NORMAL].is_loaded:
                    await self._models[ModelTier.NORMAL].unload()
                    logger.info("Unloaded NORMAL model to free VRAM")

                await self._models[ModelTier.THINKING].load()
                logger.info("Loaded THINKING model")
            else:
                # Disable thinking mode: load NORMAL, unload THINKING
                if (
                    ModelTier.THINKING in self._models
                    and self._models[ModelTier.THINKING].is_loaded
                ):
                    await self._models[ModelTier.THINKING].unload()
                    logger.info("Unloaded THINKING model")

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
            tier = await self.route(messages)

        # Track last used tier for adapter lookup
        self._last_used_tier = tier

        # Ensure model is loaded
        model = await self._get_model(tier)

        # Use model's context_length if max_tokens not specified
        if "max_tokens" not in kwargs:
            kwargs["max_tokens"] = model.config.max_output_tokens

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
            tier = await self.route(messages)

        # Track last used tier for adapter lookup
        self._last_used_tier = tier

        model = await self._get_model(tier)

        # Use model's context_length if max_tokens not specified
        if "max_tokens" not in kwargs:
            kwargs["max_tokens"] = model.config.max_output_tokens

        adapter_name = type(model.adapter).__name__
        logger.debug(f"Streaming with {tier.value} model (adapter: {adapter_name})")
        async for chunk in model.generate_stream(messages, **kwargs):
            yield chunk

    async def _get_model(self, tier: ModelTier) -> ModelBackend:
        """
        Get a model, loading if necessary.

        For main model tiers (CODE, NORMAL, THINKING), only one is loaded at a time.
        When switching between main tiers, the previous one is unloaded first.
        Auxiliary tiers (ROUTER) stay loaded.

        Optimization: If two tiers point to the same model file, no swap needed.

        Args:
            tier: Model tier

        Returns:
            Model backend

        Raises:
            ValueError: If tier not configured
        """
        tier = self._resolve_tier(tier)
        model = self._models[tier]

        # Main tiers: require lock for all operations to prevent TOCTOU race
        if tier in self.MAIN_TIERS:
            return await self._get_main_tier_model(tier, model)

        # Auxiliary tier - load without lock (small models, always loaded)
        if not model.is_loaded:
            await model.load()
        return model

    def _resolve_tier(self, tier: ModelTier) -> ModelTier:
        """Resolve tier to an available model, using fallback if needed."""
        if tier in self._models:
            return tier
        fallback = ModelTier(self.config.routing.fallback_model)
        if fallback not in self._models:
            raise ValueError(f"No model available for tier {tier.value}")
        logger.debug(f"Falling back to {fallback.value} model")
        return fallback

    async def _get_main_tier_model(self, tier: ModelTier, model: ModelBackend) -> ModelBackend:
        """Get a main tier model with proper locking and swap handling."""
        async with self._lock:
            if model.is_loaded:
                self._update_swap_action("none")
                return model

            # Check for reusable already-loaded model with same path
            if reusable := self._find_reusable_model(tier, model):
                self._update_swap_action("reused")
                return reusable

            # Need to swap - unload current if different file
            await self._unload_current_if_needed(model)

            # Load the requested model
            logger.info(f"Loading {tier.value} model")
            await model.load()
            self._loaded_main_tier = tier
            self._update_swap_action("loaded")
            return model

    def _update_swap_action(self, action: str) -> None:
        """Update the swap action on the last routing result."""
        if self._last_routing_result:
            self._last_routing_result.swap_action = action

    def _find_reusable_model(self, tier: ModelTier, model: ModelBackend) -> ModelBackend | None:
        """Find an already-loaded model that can be reused (same file path)."""
        if not self._loaded_main_tier:
            return None
        current = self._models.get(self._loaded_main_tier)
        if current and current.is_loaded and current.config.path == model.config.path:
            logger.info(
                f"Reusing {self._loaded_main_tier.value} model for {tier.value} "
                f"(same file: {model.config.path.name})"
            )
            return current
        return None

    async def _unload_current_if_needed(self, model: ModelBackend) -> None:
        """Unload currently loaded model if it's a different file."""
        if not self._loaded_main_tier:
            return
        current = self._models.get(self._loaded_main_tier)
        if current and current.is_loaded and current.config.path != model.config.path:
            logger.info(f"Unloading {self._loaded_main_tier.value} model for swap")
            await current.unload()

    @property
    def last_routing_result(self) -> RoutingResult | None:
        """Get the last routing result for display."""
        return self._last_routing_result

    async def route(self, messages: list[Message]) -> ModelTier:
        """
        Route request to appropriate model tier.

        Uses the router model for classification. Falls back to default
        tier if no router is configured.

        Args:
            messages: Conversation messages

        Returns:
            Model tier to use
        """
        if not self.config.routing.enabled:
            default = ModelTier(self.config.models.default)
            self._last_routing_result = RoutingResult(tier=default)
            return default

        start = time.perf_counter()
        previous_tier = self._last_routed_tier

        tier, raw_output = await self._classify_task(messages)

        # Thinking mode override: upgrade NORMAL → THINKING if forced on
        if self._thinking_mode and tier == ModelTier.NORMAL and ModelTier.THINKING in self._models:
            tier = ModelTier.THINKING

        # Create result early so _get_model can populate swap_action
        self._last_routing_result = RoutingResult(
            tier=tier,
            previous_tier=previous_tier,
            model_raw=raw_output,
        )

        # Prepare the model (load/swap) — populates swap_action
        await self._get_model(tier)

        routing_ms = (time.perf_counter() - start) * 1000
        self._last_routing_result.routing_ms = routing_ms
        self._last_routed_tier = tier

        swap = self._last_routing_result.swap_action
        logger.info(f"[ROUTER] {tier.value} | {swap} | {routing_ms:.0f}ms")
        return tier

    async def _classify_task(self, messages: list[Message]) -> tuple[ModelTier, str]:
        """
        Classify task type using router model.

        Args:
            messages: Full conversation messages

        Returns:
            Tuple of (ModelTier, raw model output string)
        """
        if ModelTier.ROUTER not in self._models:
            logger.warning("[ROUTER] No router model configured, using default tier")
            default = ModelTier(self.config.models.default)
            return default, ""

        # Extract last user message and history for context
        user_message = ""
        history_messages: list[str] = []
        for msg in reversed(messages):
            if msg.role == "user":
                if not user_message:
                    user_message = msg.content
                else:
                    history_messages.append(msg.content)
                    if len(history_messages) >= 5:
                        break

        history_messages.reverse()  # Chronological order

        classification_prompt = get_classification_prompt(
            user_message, self.config.prompts_dir, history=history_messages
        )

        router = await self._get_model(ModelTier.ROUTER)
        result = await router.complete(
            classification_prompt,
            max_tokens=16,
            temperature=0.0,
        )

        raw = result.content.strip()
        tier, digit = self._parse_classification(raw)
        return tier, digit

    def _parse_classification(self, raw_output: str) -> tuple[ModelTier, str]:
        """Extract classification tier from raw completion output.

        Scans for the first valid digit (1-4) in the model output.
        The model continues the ``"message" -> N`` pattern, so the
        first classification digit is the answer.

        Returns:
            Tuple of (tier, digit string) for display purposes.
        """
        for char in raw_output:
            if char in CLASSIFICATION_MAP:
                return CLASSIFICATION_MAP[char], char
        logger.warning(f"[ROUTER] No valid digit in output: {raw_output!r}, defaulting to NORMAL")
        return ModelTier.NORMAL, ""

    def get_allowed_tools(self, tier: ModelTier) -> set[str] | None:
        """Get allowed tools for a tier, or None if all tools allowed.

        Args:
            tier: Model tier to check

        Returns:
            Set of allowed tool names, or None if no filtering
        """
        model = self._models.get(tier)
        if model and model.config.allowed_tools is not None:
            return set(model.config.allowed_tools)
        return None

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

        return get_adapter("generic", tier.value)

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

    def can_handoff(self, from_tier: ModelTier | None, to_tier: ModelTier) -> bool:
        """
        Check if handoff between tiers is permitted.

        Args:
            from_tier: Current tier (None if not yet locked)
            to_tier: Target tier

        Returns:
            True if handoff is allowed, False otherwise
        """
        if from_tier is None:
            return True  # No current tier, any target is valid
        if from_tier not in self.HANDOFF_RULES:
            return False  # Unknown source tier
        return to_tier in self.HANDOFF_RULES[from_tier]

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
