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

    # Handoff routing rules - which tiers can hand off to which
    HANDOFF_RULES: dict[ModelTier, frozenset[ModelTier]] = {
        ModelTier.SIMPLE: frozenset({ModelTier.NORMAL, ModelTier.CODE, ModelTier.THINKING}),
        ModelTier.NORMAL: frozenset({ModelTier.SIMPLE, ModelTier.CODE, ModelTier.THINKING}),
        ModelTier.CODE: frozenset({ModelTier.SIMPLE, ModelTier.NORMAL}),
        ModelTier.THINKING: frozenset({ModelTier.NORMAL, ModelTier.CODE}),  # Never to SIMPLE
    }

    # Task classification keyword patterns
    _CODE_KEYWORDS = (
        "write",
        "implement",
        "create",
        "add",
        "fix",
        "debug",
        "refactor",
        "edit",
        "modify",
        "update",
        "change",
        "build",
        "make",
        "function",
        "class",
        "method",
        "code",
        "program",
        "script",
        "api",
        "endpoint",
        "test",
        "unit test",
        "bug",
        "error",
        "feature",
    )
    _COMPLEX_KEYWORDS = (
        "analyze",
        "analyse",
        "compare",
        "evaluate",
        "assess",
        "review",
        "investigate",
        "examine",
        "trade-off",
        "tradeoff",
        "architecture",
        "design pattern",
        "pros and cons",
        "advantages and disadvantages",
    )
    _QUESTION_KEYWORDS = (
        "what is",
        "what are",
        "how does",
        "how do",
        "why is",
        "why does",
        "explain",
        "describe",
        "tell me about",
        "difference between",
        "when should",
        "can you explain",
    )
    _SIMPLE_PATTERNS = (
        "hello",
        "hi",
        "hey",
        "thanks",
        "thank you",
        "ok",
        "okay",
        "sure",
        "yes",
        "no",
        "bye",
        "goodbye",
        "good morning",
        "good night",
    )

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
            tier = await self._route(messages)

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
            tier = await self._route(messages)

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
                return model

            # Check for reusable already-loaded model with same path
            if reusable := self._find_reusable_model(tier, model):
                return reusable

            # Need to swap - unload current if different file
            await self._unload_current_if_needed(model)

            # Load the requested model
            logger.info(f"Loading {tier.value} model")
            await model.load()
            self._loaded_main_tier = tier
            return model

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

        # Classify task based on last user message
        user_message = self._get_last_user_message(messages)
        task_type = await self._classify_task(user_message)
        logger.info(f"[ROUTER] Task classification: {task_type}")

        return self._select_tier_for_task(task_type)

    def _get_last_user_message(self, messages: list[Message]) -> str:
        """Extract last user message for routing decision."""
        for msg in reversed(messages):
            if msg.role == "user":
                return msg.content
        return ""

    def _select_tier_for_task(self, task_type: str) -> ModelTier:
        """Select model tier for a task type."""
        # Direct mappings
        direct_tiers = {"simple": ModelTier.SIMPLE, "code": ModelTier.CODE}
        if tier := direct_tiers.get(task_type):
            return tier

        # Complex/reasoning tasks: check if THINKING should be used
        use_thinking = (
            task_type == "complex" or self._thinking_mode
        ) and ModelTier.THINKING in self._models
        return ModelTier.THINKING if use_thinking else ModelTier.NORMAL

    async def _classify_task(self, message: str) -> str:
        """
        Classify task type using keyword heuristics + model confirmation.

        The 0.6B router model alone isn't reliable for multi-class classification,
        so we use a hybrid approach:
        1. Keyword heuristics identify likely CODE/REASONING/COMPLEX tasks
        2. Model confirms SIMPLE classification (it's good at greeting detection)

        Args:
            message: User message

        Returns:
            Task type: 'simple', 'code', 'reasoning', or 'complex'
        """
        # Try keyword-based classification first
        if result := self._classify_by_keywords(message):
            return result

        # Fall back to model classification for ambiguous cases
        if ModelTier.ROUTER in self._models:
            return await self._classify_task_with_model(message)

        logger.info("[ROUTER] No keyword match, defaulting to reasoning")
        return "reasoning"

    def _classify_by_keywords(self, message: str) -> str | None:
        """Classify task by keyword matching. Returns None if no match."""
        msg_lower = message.lower()
        msg_stripped = msg_lower.strip().rstrip("!?.").strip()

        # Check for SIMPLE first (short messages that are just greetings)
        if msg_stripped in self._SIMPLE_PATTERNS or len(msg_stripped) <= 3:
            logger.info("[ROUTER] Keyword match: simple (greeting pattern)")
            return "simple"

        # Check keyword categories in order
        keyword_checks = [
            (self._CODE_KEYWORDS, "code"),
            (self._COMPLEX_KEYWORDS, "complex"),
            (self._QUESTION_KEYWORDS, "reasoning"),
        ]
        for keywords, task_type in keyword_checks:
            if matched_kw := self._find_keyword_match(msg_lower, keywords):
                logger.info(f"[ROUTER] Keyword match: {task_type} ('{matched_kw}')")
                return task_type
        return None

    def _find_keyword_match(self, text: str, keywords: tuple[str, ...]) -> str | None:
        """Find first matching keyword in text."""
        for kw in keywords:
            if kw in text:
                return kw
        return None

    async def _classify_task_with_model(self, message: str) -> str:
        """
        Classify task type using router model with GBNF grammar constraint.

        Used as fallback when keyword heuristics don't match.

        Args:
            message: User message

        Returns:
            Task type: 'simple', 'code', 'reasoning', or 'complex'
        """
        classification_prompt = get_classification_prompt(message, self.config.prompts_dir)

        micro = await self._get_model(ModelTier.ROUTER)
        result = await micro.generate(
            [Message(role="user", content=classification_prompt)],
            max_tokens=10,
            temperature=0.0,
            grammar=CLASSIFICATION_GRAMMAR,
        )

        response = result.content.strip()
        category = CLASSIFICATION_MAP.get(response, "reasoning")
        logger.info(f"[ROUTER] Model classification: '{response}' -> {category}")

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
