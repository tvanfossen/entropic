"""
Model orchestrator for multi-model management.

Manages loading, routing, and lifecycle of multiple models.
"""

import asyncio
import time
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from entropi.config.schema import EntropyConfig, ModelConfig
from entropi.core.base import GenerationResult, Message, ModelBackend, ModelTier
from entropi.core.logging import get_logger
from entropi.inference.adapters import ChatAdapter
from entropi.inference.llama_cpp import LlamaCppBackend
from entropi.prompts import build_classification_prompt, load_tier_identity

logger = get_logger("inference.orchestrator")

BackendFactory = Callable[[ModelConfig, str], ModelBackend]


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

    def __init__(
        self,
        config: EntropyConfig,
        tiers: list[ModelTier] | None = None,
        backend_factory: BackendFactory | None = None,
    ) -> None:
        """
        Initialize orchestrator.

        Args:
            config: Application configuration
            tiers: Consumer-provided tier definitions. If None, tiers are
                   created from config + identity file frontmatter.
            backend_factory: Custom factory for creating model backends.
                If None, uses the default LlamaCppBackend factory.
        """
        self.config = config
        self._tiers: dict[ModelTier, ModelBackend] = {}
        self._router: ModelBackend | None = None
        self._lock = asyncio.Lock()
        self._thinking_mode: bool = config.thinking.enabled
        self._last_used_tier: ModelTier | None = None
        self._loaded_main_tier: ModelTier | None = None
        self._last_routed_tier: ModelTier | None = None
        self._last_routing_result: RoutingResult | None = None

        # Build tier list from config if not provided programmatically
        self._tier_list: list[ModelTier] = tiers or self._build_tiers_from_config()

        # Derive routing data from config
        self._tier_map = self._build_tier_map()
        self._classification_map = self._build_classification_map()
        self._handoff_rules = self._build_handoff_rules()

        # Backend creation — consumer can inject custom factory
        self._backend_factory: BackendFactory = backend_factory or self._default_backend_factory

    def _build_tiers_from_config(self) -> list[ModelTier]:
        """Build ModelTier instances from config + identity file frontmatter."""
        tiers: list[ModelTier] = []
        for name, tier_config in self.config.models.tiers.items():
            focus = tier_config.focus
            examples: list[str] = []

            if not focus:
                # Try identity file frontmatter
                identity_path = self._find_identity_file(name)
                if identity_path:
                    identity, _body = load_tier_identity(identity_path)
                    focus = identity.focus
                    examples = identity.examples

            if not focus:
                logger.warning(f"Tier '{name}' has no focus (config or identity file)")
                focus = [name]

            tiers.append(ModelTier(name, focus=focus, examples=examples))
        return tiers

    def _find_identity_file(self, tier_name: str) -> Path | None:
        """Find identity file for a tier in prompts_dir or bundled data.

        Checks prompts_dir first, then bundled. In strict mode
        (prompts_dir set + use_bundled_prompts=False), only checks
        prompts_dir — no bundled fallback.
        """
        filename = f"identity_{tier_name}.md"
        result = self._check_prompts_dir(filename)
        if result is not None or not self.config.use_bundled_prompts:
            return result
        default_path = Path(__file__).parent.parent / "data" / "prompts" / filename
        return default_path if default_path.exists() else None

    def _check_prompts_dir(self, filename: str) -> Path | None:
        """Check prompts_dir for a file, return path if found."""
        if not self.config.prompts_dir:
            return None
        user_path = self.config.prompts_dir / filename
        return user_path if user_path.exists() else None

    def _build_tier_map(self) -> dict[str, ModelTier]:
        """Build digit-to-tier mapping from config or auto-number."""
        routing = self.config.routing

        if routing.tier_map:
            # Explicit tier_map in config
            tier_by_name = {t.name: t for t in self._tier_list}
            result = {digit: tier_by_name[name] for digit, name in routing.tier_map.items()}
            logger.info("Using explicit tier_map: %s", {k: v.name for k, v in result.items()})
            return result

        # Auto-number tiers 1..N
        result = {str(i): tier for i, tier in enumerate(self._tier_list, 1)}
        logger.info("Auto-derived tier_map: %s", {k: v.name for k, v in result.items()})
        return result

    def _build_classification_map(self) -> dict[str, ModelTier]:
        """Build string digit → ModelTier map for classification parsing."""
        return self._tier_map

    def _build_handoff_rules(self) -> dict[ModelTier, frozenset[ModelTier]]:
        """Build handoff rules from config or default to all-to-all."""
        routing = self.config.routing
        tier_by_name = {t.name: t for t in self._tier_list}

        if routing.handoff_rules:
            return {
                tier_by_name[src]: frozenset(tier_by_name[t] for t in targets)
                for src, targets in routing.handoff_rules.items()
                if src in tier_by_name
            }

        # Default: all-to-all (every tier can hand off to every other)
        all_tiers = frozenset(self._tier_list)
        return {tier: all_tiers - {tier} for tier in self._tier_list}

    def _find_tier(self, name: str) -> ModelTier | None:
        """Find a tier by name from the tier list."""
        for tier in self._tier_list:
            if tier == name:
                return tier
        return None

    def _get_default_tier(self) -> ModelTier:
        """Get the default tier from config."""
        tier = self._find_tier(self.config.models.default)
        if tier is None:
            raise ValueError(f"Default tier '{self.config.models.default}' not found")
        return tier

    def _validate_tiers(self) -> None:
        """Validate tier config entries and identity files.

        Identity file validation only triggers when prompts_dir is set
        AND use_bundled_prompts is False — the consumer is fully managing
        their own prompts with no bundled fallback.
        """
        for tier in self._tier_list:
            if tier not in self._tiers:
                raise ValueError(f"ModelTier '{tier.name}' has no config entry in models.tiers")

        if self.config.prompts_dir and not self.config.use_bundled_prompts:
            missing = [t.name for t in self._tier_list if self._find_identity_file(t.name) is None]
            if missing:
                raise FileNotFoundError(
                    f"Missing identity files in {self.config.prompts_dir}: "
                    + ", ".join(f"identity_{n}.md" for n in missing)
                )

    async def initialize(self) -> None:
        """Initialize and load configured models."""
        logger.info("Initializing model orchestrator")

        models_config = self.config.models

        # Create backends for configured tiers
        for name, tier_config in models_config.tiers.items():
            tier = self._find_tier(name)
            if tier is None:
                logger.warning(f"Tier config '{name}' has no matching ModelTier")
                continue
            self._tiers[tier] = self._create_backend(tier_config, tier.name)

        # Create router backend (separate from tiers)
        if models_config.router:
            self._router = self._create_backend(models_config.router, "router")

        self._validate_tiers()

        if not self._tiers and not self._router:
            logger.warning("No models configured")
            return

        # Pre-load only ONE main model (default) to maximize VRAM
        default_tier = self._get_default_tier()
        if self._thinking_mode:
            thinking = self._find_tier("thinking")
            if thinking and thinking in self._tiers:
                default_tier = thinking

        if default_tier in self._tiers:
            await self._tiers[default_tier].load()
            self._loaded_main_tier = default_tier
            logger.info(f"Pre-loaded {default_tier.name} model (default)")

        # Pre-load router (small, always needed)
        if self._router:
            await self._router.load()
            logger.info("Pre-loaded ROUTER model")

    def _create_backend(self, model_config: ModelConfig, tier_name: str) -> ModelBackend:
        """Create a backend using the configured factory."""
        return self._backend_factory(model_config, tier_name)

    def _default_backend_factory(self, model_config: ModelConfig, tier_name: str) -> ModelBackend:
        """Default factory — creates LlamaCppBackend instances."""
        return LlamaCppBackend(
            config=model_config,
            tier=tier_name,
            prompts_dir=self.config.prompts_dir,
            use_bundled_prompts=self.config.use_bundled_prompts,
        )

    async def shutdown(self) -> None:
        """Shutdown and unload all models."""
        logger.info("Shutting down model orchestrator")

        for tier, model in self._tiers.items():
            if model.is_loaded:
                await model.unload()
                logger.info(f"Unloaded {tier.name} model")

        if self._router and self._router.is_loaded:
            await self._router.unload()
            logger.info("Unloaded ROUTER model")

    async def unload_all_models(self) -> None:
        """Unload all models to free VRAM.

        Used before loading other GPU-intensive components (e.g., voice mode).
        Call reload_default_models() to restore after.
        """
        import gc

        async with self._lock:
            for tier, model in self._tiers.items():
                if model.is_loaded:
                    await model.unload()
                    logger.info(f"Unloaded {tier.name} model for VRAM release")
            if self._router and self._router.is_loaded:
                await self._router.unload()
                logger.info("Unloaded ROUTER model for VRAM release")
            self._loaded_main_tier = None

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
        """Reload the default models after unload_all_models()."""
        async with self._lock:
            default_tier = self._get_default_tier()
            if self._thinking_mode:
                thinking = self._find_tier("thinking")
                if thinking and thinking in self._tiers:
                    default_tier = thinking

            if default_tier in self._tiers and not self._tiers[default_tier].is_loaded:
                await self._tiers[default_tier].load()
                self._loaded_main_tier = default_tier
                logger.info(f"Reloaded {default_tier.name} model")

            if self._router and not self._router.is_loaded:
                await self._router.load()
                logger.info("Reloaded ROUTER model")

    # Thinking mode methods

    def get_thinking_mode(self) -> bool:
        """Get current thinking mode state."""
        return self._thinking_mode

    async def set_thinking_mode(self, enabled: bool) -> bool:
        """Toggle thinking mode. Returns True if successful."""
        if enabled == self._thinking_mode:
            return True

        thinking_tier = self._find_tier("thinking")
        normal_tier = self._find_tier("normal")
        required = thinking_tier if enabled else normal_tier

        if required is None or required not in self._tiers:
            logger.warning(f"{'thinking' if enabled else 'normal'} model not configured")
            return False

        # At this point, `required` is proven non-None and in self._tiers
        async with self._lock:
            # Unload the opposite tier to free VRAM
            opposite = normal_tier if enabled else thinking_tier
            if opposite and opposite in self._tiers:
                model = self._tiers[opposite]
                if model.is_loaded:
                    await model.unload()
                    logger.info(f"Unloaded {opposite.name} model to free VRAM")

            await self._tiers[required].load()
            logger.info(f"Loaded {required.name} model")

            self._thinking_mode = enabled
            return True

    async def generate(
        self,
        messages: list[Message],
        tier: ModelTier | None = None,
        **kwargs: Any,
    ) -> GenerationResult:
        """Generate a response using the appropriate model."""
        if tier is None:
            tier = await self.route(messages)

        self._last_used_tier = tier
        model = await self._get_model(tier)

        if "max_tokens" not in kwargs:
            kwargs["max_tokens"] = model.config.max_output_tokens

        adapter_name = type(model.adapter).__name__
        logger.debug(f"Generating with {tier.name} model (adapter: {adapter_name})")
        result = await model.generate(messages, **kwargs)

        if result.content:
            cleaned_content, parsed_calls = model.adapter.parse_tool_calls(result.content)
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
        """Generate a streaming response."""
        if tier is None:
            tier = await self.route(messages)

        self._last_used_tier = tier
        model = await self._get_model(tier)

        if "max_tokens" not in kwargs:
            kwargs["max_tokens"] = model.config.max_output_tokens

        adapter_name = type(model.adapter).__name__
        logger.debug(f"Streaming with {tier.name} model (adapter: {adapter_name})")
        async for chunk in model.generate_stream(messages, **kwargs):
            yield chunk

    async def _get_model(self, tier: ModelTier) -> ModelBackend:
        """Get a model, loading if necessary.

        All generation tiers are main tiers (only one loaded at a time).
        Router is separate and always loaded.
        """
        tier = self._resolve_tier(tier)
        model = self._tiers[tier]

        return await self._get_main_tier_model(tier, model)

    def _resolve_tier(self, tier: ModelTier) -> ModelTier:
        """Resolve tier to an available model, using fallback if needed."""
        if tier in self._tiers:
            return tier
        fallback_name = self.config.routing.fallback_tier
        fallback = self._find_tier(fallback_name)
        if fallback and fallback in self._tiers:
            logger.debug(f"Falling back to {fallback_name} model")
            return fallback
        raise ValueError(f"No model available for tier {tier.name}")

    async def _get_main_tier_model(self, tier: ModelTier, model: ModelBackend) -> ModelBackend:
        """Get a main tier model with proper locking and swap handling."""
        async with self._lock:
            if model.is_loaded:
                self._update_swap_action("none")
                return model

            if reusable := self._find_reusable_model(tier, model):
                self._update_swap_action("reused")
                return reusable

            await self._unload_current_if_needed(model)

            logger.info(f"Loading {tier.name} model")
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
        current = self._tiers.get(self._loaded_main_tier)
        if current and current.is_loaded and current.config.path == model.config.path:
            logger.info(
                f"Reusing {self._loaded_main_tier.name} model for {tier.name} "
                f"(same file: {model.config.path.name})"
            )
            return current
        return None

    async def _unload_current_if_needed(self, model: ModelBackend) -> None:
        """Unload currently loaded model if it's a different file."""
        if not self._loaded_main_tier:
            return
        current = self._tiers.get(self._loaded_main_tier)
        if current and current.is_loaded and current.config.path != model.config.path:
            logger.info(f"Unloading {self._loaded_main_tier.name} model for swap")
            await current.unload()

    @property
    def tier_names(self) -> list[str]:
        """Get ordered list of tier names."""
        return [t.name for t in self._tier_list]

    @property
    def last_routing_result(self) -> RoutingResult | None:
        """Get the last routing result for display."""
        return self._last_routing_result

    async def route(self, messages: list[Message]) -> ModelTier:
        """Route request to appropriate model tier."""
        if not self.config.routing.enabled:
            default = self._get_default_tier()
            self._last_routing_result = RoutingResult(tier=default)
            return default

        start = time.perf_counter()
        previous_tier = self._last_routed_tier

        tier, raw_output = await self._classify_task(messages)

        # Thinking mode override: upgrade normal → thinking if forced on
        thinking_tier = self._find_tier("thinking")
        if (
            self._thinking_mode
            and tier == "normal"
            and thinking_tier is not None
            and thinking_tier in self._tiers
        ):
            tier = thinking_tier

        self._last_routing_result = RoutingResult(
            tier=tier,
            previous_tier=previous_tier,
            model_raw=raw_output,
        )

        await self._get_model(tier)

        routing_ms = (time.perf_counter() - start) * 1000
        self._last_routing_result.routing_ms = routing_ms
        self._last_routed_tier = tier

        swap = self._last_routing_result.swap_action
        logger.info(f"[ROUTER] {tier.name} | {swap} | {routing_ms:.0f}ms")
        return tier

    async def _classify_task(self, messages: list[Message]) -> tuple[ModelTier, str]:
        """Classify task type using router model."""
        if not self._router:
            logger.warning("[ROUTER] No router model configured, using default tier")
            return self._get_default_tier(), ""

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
        history_messages.reverse()

        classification_prompt = build_classification_prompt(
            self._tier_list, user_message, history=history_messages
        )

        # Router is always loaded separately, not in _tiers
        if not self._router.is_loaded:
            await self._router.load()

        result = await self._router.complete(
            classification_prompt,
            max_tokens=16,
            temperature=0.0,
        )

        raw = result.content.strip()
        tier, digit = self._parse_classification(raw)
        return tier, digit

    def _parse_classification(self, raw_output: str) -> tuple[ModelTier, str]:
        """Extract classification tier from raw completion output."""
        for char in raw_output:
            if char in self._classification_map:
                return self._classification_map[char], char
        logger.warning(
            f"[ROUTER] No valid digit in output: {raw_output!r}, "
            f"defaulting to {self.config.models.default}"
        )
        return self._get_default_tier(), ""

    def get_allowed_tools(self, tier: ModelTier) -> set[str] | None:
        """Get allowed tools for a tier, or None if all tools allowed."""
        model = self._tiers.get(tier)
        if model and model.config.allowed_tools is not None:
            return set(model.config.allowed_tools)
        return None

    def get_adapter(self, tier: ModelTier | None = None) -> ChatAdapter:
        """Get the chat adapter for a model tier."""
        if tier is None:
            tier = self._last_used_tier
            if tier is None:
                tier = self._get_default_tier()

        if tier in self._tiers:
            return self._tiers[tier].adapter

        from entropi.inference.adapters import get_adapter

        return get_adapter(
            "generic", tier.name, use_bundled_prompts=self.config.use_bundled_prompts
        )

    @property
    def last_used_tier(self) -> ModelTier | None:
        """Get the last used model tier."""
        return self._last_used_tier

    @property
    def last_finish_reason(self) -> str:
        """Get finish_reason from the last generation."""
        if self._last_used_tier and self._last_used_tier in self._tiers:
            return self._tiers[self._last_used_tier].last_finish_reason
        return "stop"

    def get_loaded_models(self) -> list[str]:
        """Get list of currently loaded model tiers."""
        loaded = [tier.name for tier, model in self._tiers.items() if model.is_loaded]
        if self._router and self._router.is_loaded:
            loaded.append("router")
        return loaded

    def get_available_models(self) -> list[str]:
        """Get list of configured (but not necessarily loaded) model tiers."""
        available = [tier.name for tier in self._tiers]
        if self._router:
            available.append("router")
        return available

    def can_handoff(self, from_tier: ModelTier | None, to_tier: ModelTier) -> bool:
        """Check if handoff between tiers is permitted."""
        if from_tier is None:
            return True
        if from_tier not in self._handoff_rules:
            return False
        return to_tier in self._handoff_rules[from_tier]

    def count_tokens(self, text: str, tier: ModelTier | None = None) -> int:
        """Count tokens in text."""
        if tier is None:
            tier = self._get_default_tier()

        if tier in self._tiers and self._tiers[tier].is_loaded:
            return self._tiers[tier].count_tokens(text)

        return len(text) // 4
