"""
Model orchestrator for multi-model management.

Manages loading, routing, and lifecycle of multiple models.
"""

import asyncio
import math
import time
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from entropic.config.schema import EntropyConfig, ModelConfig
from entropic.core.base import GenerationResult, Message, ModelBackend, ModelTier
from entropic.core.logging import get_logger
from entropic.inference.adapters import ChatAdapter
from entropic.inference.adapters.base import get_adapter
from entropic.inference.llama_cpp import LlamaCppBackend
from entropic.prompts import build_classification_prompt
from entropic.prompts.manager import PromptManager

logger = get_logger("inference.orchestrator")

BackendFactory = Callable[[ModelConfig, str], ModelBackend]


def _logprob_to_confidence(logprobs: list[dict[str, Any]] | None) -> float:
    """Convert first token's log-probability to a 0–1 confidence score."""
    if not logprobs:
        return 0.0
    lp = logprobs[0].get("logprob", 0.0)
    return math.exp(lp)


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
        self._model_pool: dict[Path, ModelBackend] = {}  # resolved_path → backend (dedup)
        self._adapters: dict[ModelTier, ChatAdapter] = {}  # per-tier adapter (not per-backend)
        self._router: ModelBackend | None = None
        self._lock = asyncio.Lock()
        self._last_used_tier: ModelTier | None = None
        self._loaded_main_tier: ModelTier | None = None
        self._last_routed_tier: ModelTier | None = None
        self._last_routing_result: RoutingResult | None = None

        # Recent tier history (most recent last, capped at 5)
        self._tier_history: list[str] = []

        # Grammar file cache (path → contents)
        self._grammar_cache: dict[Path, str] = {}

        # Central prompt loading — must exist before _build_tiers_from_config
        self._prompt_manager = PromptManager.from_config(config, quiet=True)

        # Build tier list from config if not provided programmatically
        self._tier_list: list[ModelTier] = tiers or self._build_tiers_from_config()

        # Derive routing data from config
        self._tier_map = self._build_tier_map()
        self._handoff_rules = self._build_handoff_rules()

        # Backend creation — consumer can inject custom factory
        self._backend_factory: BackendFactory = backend_factory or self._default_backend_factory

    def _build_tiers_from_config(self) -> list[ModelTier]:
        """Build ModelTier instances from config + identity frontmatter.

        Only routable tiers are included — tiers with ``routable=False``
        are excluded from router classification (chain-only or infrastructure).
        """
        tiers: list[ModelTier] = []
        for name, tier_config in self.config.models.tiers.items():
            # Resolve routable: config override → identity frontmatter → True
            fm = self._prompt_manager.get_identity_frontmatter(name)
            config_routable = tier_config.routable
            fm_routable = fm.routable if fm else True
            routable = config_routable if config_routable is not None else fm_routable
            if not routable:
                logger.debug("Tier '%s' is non-routable, skipping router classification", name)
                continue

            focus: list[str] = fm.focus if fm else []
            examples: list[str] = fm.examples if fm else []

            if not focus:
                logger.warning(f"Tier '{name}' has no focus (identity file missing or empty)")
                focus = [name]

            tiers.append(ModelTier(name, focus=focus, examples=examples))
        return tiers

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
        mapping = {k: v.name for k, v in result.items()}
        logger.warning(
            "tier_map auto-derived from tier order: %s. "
            "Reordering tiers in config will change routing. "
            "Set routing.tier_map explicitly for stable routing.",
            mapping,
        )
        return result

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
        """Validate tier config entries exist for all tier definitions."""
        for tier in self._tier_list:
            if tier not in self._tiers:
                raise ValueError(f"ModelTier '{tier.name}' has no config entry in models.tiers")

    async def initialize(self) -> None:
        """Initialize and load configured models.

        Creates ONE backend per unique model file (not per tier). Multiple
        tiers sharing the same .gguf file share a single backend instance,
        avoiding duplicate VRAM compute buffer allocation (~1.1GB each).
        """
        logger.info("Initializing model orchestrator")

        models_config = self.config.models

        # Create deduplicated backends: one per unique model path
        for name, tier_config in models_config.tiers.items():
            tier = self._find_tier(name)
            if tier is None:
                logger.warning(f"Tier config '{name}' has no matching ModelTier")
                continue

            resolved_path = tier_config.path.resolve()
            if resolved_path not in self._model_pool:
                self._model_pool[resolved_path] = self._create_backend(tier_config, name)
            self._tiers[tier] = self._model_pool[resolved_path]

            # Per-tier adapter (identity-specific, independent of shared backend)
            self._adapters[tier] = get_adapter(
                tier_config.adapter, name, prompt_manager=self._prompt_manager
            )

        unique_count = len(self._model_pool)
        tier_count = len(self._tiers)
        logger.info(f"Created {unique_count} unique backend(s) for {tier_count} tier(s)")

        # Create router backend (separate from tiers, speed-optimized)
        if models_config.router:
            self._router = self._backend_factory(models_config.router, "router")

        self._validate_tiers()

        if not self._tiers and not self._router:
            logger.warning("No models configured")
            return

        # Determine the default main tier to activate
        default_tier = self._get_default_tier()

        await self._keep_warm_tiers(default_tier)

        # Activate the default main tier (COLD/WARM → ACTIVE)
        if default_tier in self._tiers:
            await self._tiers[default_tier].load()
            self._loaded_main_tier = default_tier
            logger.info(f"Activated {default_tier.name} model (default)")

        # Router stays ACTIVE permanently (small model, always needed)
        if self._router:
            await self._router.load()
            logger.info("Activated ROUTER model")

    async def _keep_warm_tiers(self, default_tier: ModelTier) -> None:
        """Warm non-default tiers with keep_warm=True (CPU preload).

        Tracks already-warmed backends to avoid warming a shared backend
        multiple times when several tiers reference the same model file.
        """
        warmed_backends: set[int] = set()  # id() of already-warmed backends
        default_backend = self._tiers.get(default_tier)

        for name, tier_config in self.config.models.tiers.items():
            tier = self._find_tier(name)
            if not (tier and tier in self._tiers and tier_config.keep_warm):
                continue
            backend = self._tiers[tier]
            # Skip default tier's backend (it gets load() not warm())
            # Skip already-warmed backends (shared by another tier)
            if backend is default_backend or id(backend) in warmed_backends:
                continue
            logger.info(f"Warming {name} model (keep_warm=True)")
            await backend.warm()
            warmed_backends.add(id(backend))

    def _create_backend(self, model_config: ModelConfig, tier_name: str) -> ModelBackend:
        """Create a backend using the configured factory."""
        return self._backend_factory(model_config, tier_name)

    def _default_backend_factory(self, model_config: ModelConfig, tier_name: str) -> ModelBackend:
        """Default factory — creates LlamaCppBackend instances."""
        return LlamaCppBackend(
            config=model_config,
            tier=tier_name,
            prompt_manager=self._prompt_manager,
        )

    def _resolve_grammar(self, tier: ModelTier, identity_fm: Any = None) -> str | None:
        """Resolve grammar string for a tier.

        Resolution order:
          1. Config override (``config.models.tiers[name].grammar``) — user override
          2. Identity frontmatter (``identity_fm.grammar``) — bundled default
          3. None — no grammar constraint

        Frontmatter grammar paths are relative to the bundled data directory.
        """
        # 1. Config override
        tier_config = self.config.models.tiers.get(tier.name)
        config_grammar = getattr(tier_config, "grammar", None) if tier_config else None
        if config_grammar is not None:
            return self._load_grammar_file(config_grammar.expanduser().resolve())

        # 2. Identity frontmatter
        fm_grammar = getattr(identity_fm, "grammar", None) if identity_fm else None
        if fm_grammar is not None:
            import entropic

            data_dir = Path(entropic.__file__).parent / "data"
            return self._load_grammar_file((data_dir / fm_grammar).resolve())

        return None

    def _load_grammar_file(self, resolved: Path) -> str | None:
        """Load and cache a grammar file by resolved path."""
        if resolved not in self._grammar_cache:
            if not resolved.exists():
                logger.warning("Grammar file not found: %s", resolved)
                return None
            self._grammar_cache[resolved] = resolved.read_text()
            logger.info(
                "Loaded grammar: %s (%d chars)",
                resolved.name,
                len(self._grammar_cache[resolved]),
            )
        return self._grammar_cache[resolved]

    async def shutdown(self) -> None:
        """Shutdown and unload all models.

        Iterates unique backends (model pool), not per-tier mapping,
        to avoid double-unloading shared backends.
        """
        logger.info("Shutting down model orchestrator")

        for path, model in self._model_pool.items():
            if model.is_loaded:
                await model.unload()
                logger.info(f"Unloaded model: {path.name}")

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
            for path, model in self._model_pool.items():
                if model.is_loaded:
                    await model.unload()
                    logger.info(f"Unloaded {path.name} for VRAM release")
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

            if default_tier in self._tiers and not self._tiers[default_tier].is_loaded:
                await self._tiers[default_tier].load()
                self._loaded_main_tier = default_tier
                logger.info(f"Reloaded {default_tier.name} model")

            if self._router and not self._router.is_loaded:
                await self._router.load()
                logger.info("Reloaded ROUTER model")

    def _apply_tier_defaults(
        self, tier: ModelTier, kwargs: dict[str, Any], identity_fm: Any = None
    ) -> None:
        """Inject tier-specific defaults into generation kwargs.

        Reads inference params from identity frontmatter if available,
        otherwise uses conservative fallbacks. Only sets values not
        already present in kwargs, so explicit caller overrides are preserved.

        Args:
            tier: The model tier being used
            kwargs: Generation kwargs dict (mutated in-place)
            identity_fm: IdentityFrontmatter for the tier, or None
        """
        if identity_fm is not None:
            defaults: dict[str, Any] = {
                "max_tokens": identity_fm.max_output_tokens,
                "temperature": identity_fm.temperature,
                "repeat_penalty": identity_fm.repeat_penalty,
            }
            if "chat_template_kwargs" not in kwargs:
                kwargs["chat_template_kwargs"] = {"enable_thinking": identity_fm.enable_thinking}
        else:
            # Fallback for custom tiers without a bundled identity
            defaults = {"max_tokens": 1024, "temperature": 0.7, "repeat_penalty": 1.1}

        for key, value in defaults.items():
            if key not in kwargs:
                kwargs[key] = value

        if "grammar" not in kwargs:
            grammar_str = self._resolve_grammar(tier, identity_fm)
            if grammar_str:
                kwargs["grammar"] = grammar_str

    async def generate(
        self,
        messages: list[Message],
        tier: ModelTier | None = None,
        identity_fm: Any = None,
        **kwargs: Any,
    ) -> GenerationResult:
        """Generate a response using the appropriate model."""
        t_start = time.perf_counter()

        t_route = time.perf_counter()
        if tier is None:
            tier = await self.route(messages)
        routing_ms = (time.perf_counter() - t_route) * 1000

        self._last_used_tier = tier

        t_swap = time.perf_counter()
        model = await self._get_model(tier)
        swap_ms = (time.perf_counter() - t_swap) * 1000

        self._apply_tier_defaults(tier, kwargs, identity_fm)

        adapter = self._adapters.get(tier)
        adapter_name = type(adapter).__name__ if adapter else "unknown"
        logger.debug(f"Generating with {tier.name} model (adapter: {adapter_name})")
        result = await model.generate(messages, **kwargs)

        if result.content and adapter:
            result.raw_content = result.content
            cleaned_content, parsed_calls = adapter.parse_tool_calls(result.content)
            result.content = cleaned_content
            if parsed_calls:
                result.tool_calls = parsed_calls
                logger.debug(f"Parsed tool calls from content: {[tc.name for tc in parsed_calls]}")

        result.routing_ms = routing_ms
        result.swap_ms = swap_ms
        result.total_ms = (time.perf_counter() - t_start) * 1000
        return result

    async def generate_stream(
        self,
        messages: list[Message],
        tier: ModelTier | None = None,
        identity_fm: Any = None,
        **kwargs: Any,
    ) -> AsyncIterator[str]:
        """Generate a streaming response."""
        if tier is None:
            tier = await self.route(messages)

        self._last_used_tier = tier
        model = await self._get_model(tier)
        self._apply_tier_defaults(tier, kwargs, identity_fm)

        adapter = self._adapters.get(tier)
        adapter_name = type(adapter).__name__ if adapter else "unknown"
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

            await self._deactivate_current_if_needed(model)

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
        """Find an already-loaded model that can be reused.

        With backend deduplication, tiers sharing the same model file share
        the same backend instance. If the requested backend is already loaded
        (identity: ``model is current``), it can be reused directly.
        """
        if not self._loaded_main_tier:
            return None
        current = self._tiers.get(self._loaded_main_tier)
        if current and current.is_loaded and current is model:
            logger.info(
                f"Reusing {self._loaded_main_tier.name} backend for {tier.name} "
                f"(shared backend: {model.config.path.name})"
            )
            return current
        return None

    async def _deactivate_current_if_needed(self, model: ModelBackend) -> None:
        """Swap out the currently loaded model to make room for a new one.

        The target state is config-driven:
        - ``keep_warm=True`` → ACTIVE → WARM (keeps CPU pages locked,
          fast reactivation, but costs ~1.1GB CUDA compute buffer)
        - ``keep_warm=False`` → ACTIVE → COLD (full unload, frees
          all VRAM and RAM — slower to reload but zero residual cost)

        Only called when the incoming model is a *different backend* from
        the current one. Same-backend reuse goes through
        ``_find_reusable_model`` instead.
        """
        if not self._loaded_main_tier:
            return
        current = self._tiers.get(self._loaded_main_tier)
        if current and current.is_loaded and current is not model:
            keep_warm = current.config.keep_warm
            if keep_warm:
                logger.info(
                    f"Deactivating {self._loaded_main_tier.name} model "
                    f"for swap (keep_warm=True, keeping WARM)"
                )
                await current.deactivate()
            else:
                logger.info(
                    f"Unloading {self._loaded_main_tier.name} model "
                    f"for swap (keep_warm=False, going COLD)"
                )
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

        self._last_routing_result = RoutingResult(
            tier=tier,
            previous_tier=previous_tier,
            model_raw=raw_output,
        )

        await self._get_model(tier)

        routing_ms = (time.perf_counter() - start) * 1000
        self._last_routing_result.routing_ms = routing_ms
        self._last_routed_tier = tier
        self._tier_history.append(tier.name)
        if len(self._tier_history) > 5:
            self._tier_history = self._tier_history[-5:]

        swap = self._last_routing_result.swap_action
        logger.info(f"[ROUTER] {tier.name} | {swap} | {routing_ms:.0f}ms")
        return tier

    @staticmethod
    def _extract_latest_user_message(messages: list[Message]) -> str:
        """Extract the latest user message for classification.

        The router only needs the current user message — no conversation
        history.  Passing history risks context overflow (tool results in
        user-role messages can be enormous) and adds noise to a 0.6B
        classifier that works best with minimal, focused input.
        """
        for msg in reversed(messages):
            if msg.role == "user":
                return msg.content
        return ""

    async def _classify_task(self, messages: list[Message]) -> tuple[ModelTier, str]:
        """Classify task type using router model.

        Uses raw text completion (no chat template) with the digit-based
        classification prompt. The prompt ends with ``"user message" -> ``
        and the model continues with the tier digit.
        """
        if not self._router:
            logger.warning("[ROUTER] No router model configured, using default tier")
            return self._get_default_tier(), ""

        user_message = self._extract_latest_user_message(messages)
        classification_prompt = build_classification_prompt(
            self._tier_list,
            user_message,
            recent_tiers=self._tier_history or None,
        )

        if not self._router.is_loaded:
            await self._router.load()

        result = await self._router.complete(
            classification_prompt,
            max_tokens=1,
            temperature=0.0,
        )
        raw = result.content.strip()
        tier, digit = self._parse_classification(raw)

        self._log_classification(tier, digit, result.logprobs)
        return tier, digit

    @staticmethod
    def _log_classification(
        tier: ModelTier, digit: str, logprobs: list[dict[str, Any]] | None
    ) -> None:
        """Log classification result with optional confidence."""
        if logprobs:
            confidence = _logprob_to_confidence(logprobs)
            logger.info(
                "[ROUTER] classified as %s (digit=%s, confidence=%.0f%%)",
                tier.name,
                digit,
                confidence * 100,
            )
        else:
            logger.info("[ROUTER] classified as %s (digit=%s)", tier.name, digit)

    def _parse_classification(self, raw_output: str) -> tuple[ModelTier, str]:
        """Extract classification tier from raw completion output."""
        for char in raw_output:
            if char in self._tier_map:
                return self._tier_map[char], char
        logger.warning(
            f"[ROUTER] No valid digit in output: {raw_output!r}, "
            f"defaulting to {self.config.models.default}"
        )
        return self._get_default_tier(), ""

    def get_tier_param(self, tier: ModelTier, attr: str, default: Any = None) -> Any:
        """Resolve a tier parameter: config override → identity frontmatter → default.

        Single resolution path for ALL tier-level params that can appear
        in both config (TierConfig) and identity frontmatter. Config wins
        when explicitly set (not None); otherwise frontmatter is consulted.
        """
        tier_config = self.config.models.tiers.get(tier.name)
        config_val = getattr(tier_config, attr, None) if tier_config else None
        if config_val is not None:
            return config_val

        fm = self._prompt_manager.get_identity_frontmatter(tier.name)
        fm_val = getattr(fm, attr, None) if fm else None
        if fm_val is not None:
            return fm_val

        return default

    @property
    def tier_history(self) -> list[str]:
        """Recent tier activations (most recent last, max 5)."""
        return list(self._tier_history)

    def get_allowed_tools(self, tier: ModelTier) -> set[str] | None:
        """Get allowed tools for a tier, or None if all tools allowed."""
        result = self.get_tier_param(tier, "allowed_tools")
        return set(result) if result is not None else None

    def get_adapter(self, tier: ModelTier | None = None) -> ChatAdapter:
        """Get the chat adapter for a model tier."""
        if tier is None:
            tier = self._last_used_tier
            if tier is None:
                tier = self._get_default_tier()

        if tier in self._adapters:
            return self._adapters[tier]

        from entropic.inference.adapters.base import get_adapter as _get_adapter

        return _get_adapter("generic", tier.name, prompt_manager=self._prompt_manager)

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

    def get_handoff_targets(self, tier: ModelTier) -> list[ModelTier]:
        """Get allowed handoff targets for a tier."""
        if tier not in self._handoff_rules:
            return []
        return list(self._handoff_rules[tier])

    async def route_among(self, candidates: list[ModelTier]) -> ModelTier:
        """Route among a subset of tiers. Single candidate returns directly."""
        if len(candidates) == 1:
            return candidates[0]
        # Multiple candidates: use router to re-classify among them
        # For now, return first candidate (router integration deferred to
        # when multi-target auto-chain is actually needed)
        logger.info(
            f"[ROUTE_AMONG] Multiple candidates: {[t.name for t in candidates]}, "
            f"selecting first: {candidates[0].name}"
        )
        return candidates[0]

    def count_tokens(self, text: str, tier: ModelTier | None = None) -> int:
        """Count tokens in text."""
        if tier is None:
            tier = self._get_default_tier()

        if tier in self._tiers and self._tiers[tier].is_loaded:
            return self._tiers[tier].count_tokens(text)

        return len(text) // 4
