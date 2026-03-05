"""Context management subsystem for the agentic loop engine.

Handles context limit refresh, tool result pruning, context warning injection,
and compaction. Extracted from AgentEngine (P2-019).
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from entropic.config.schema import EntropyConfig
from entropic.core.base import Message
from entropic.core.compaction import CompactionManager
from entropic.core.engine_types import EngineCallbacks, LoopContext
from entropic.core.logging import get_logger
from entropic.inference.orchestrator import ModelOrchestrator

logger = get_logger("core.context_manager")


@dataclass
class ContextManagerHooks:
    """Engine-level hooks called during context management."""

    after_compaction: Callable[[LoopContext], None] | None = None
    """Called after context is compacted — used to reinject context anchors."""


class ContextManager:
    """Handles context management for the agentic loop.

    Subsystem of AgentEngine. Manages context limit refresh, tool result
    pruning, context warning injection, and compaction.
    """

    def __init__(
        self,
        config: EntropyConfig,
        orchestrator: ModelOrchestrator,
        compaction_manager: CompactionManager,
        callbacks: EngineCallbacks,
        hooks: ContextManagerHooks | None = None,
    ) -> None:
        """Initialize context manager.

        Args:
            config: Application configuration (tier configs for context limit)
            orchestrator: Model orchestrator (for last_used_tier in limit refresh)
            compaction_manager: Compaction manager with token counter
            callbacks: Shared mutable callback container
            hooks: Engine-level hooks for post-compaction actions
        """
        self._config = config
        self._orchestrator = orchestrator
        self._compaction_manager = compaction_manager
        self._callbacks = callbacks
        self._hooks = hooks or ContextManagerHooks()

    # ------------------------------------------------------------------
    # Context limit management
    # ------------------------------------------------------------------

    def refresh_context_limit(self) -> None:
        """Refresh context limit based on current model.

        When models swap (e.g., conversational → planner), the context limit
        may change. Updates the token counter to use the current model's
        context length.
        """
        last_tier = self._orchestrator.last_used_tier
        if last_tier is None:
            return

        tier_config = self._config.models.tiers.get(str(last_tier))
        if tier_config:
            new_max = tier_config.context_length
            if new_max != self._compaction_manager.counter.max_tokens:
                logger.debug(
                    f"Updating context limit: "
                    f"{self._compaction_manager.counter.max_tokens} -> {new_max}"
                )
                self._compaction_manager.counter.max_tokens = new_max

    # ------------------------------------------------------------------
    # Tool result pruning
    # ------------------------------------------------------------------

    def prune_tool_results(self, ctx: LoopContext, keep_recent: int) -> tuple[int, int]:
        """Replace old tool results with stubs. Returns (pruned_count, freed_chars)."""
        tool_result_indices = [i for i, msg in enumerate(ctx.messages) if msg.tool_results]

        to_prune = tool_result_indices[:-keep_recent] if keep_recent > 0 else tool_result_indices

        pruned_count = 0
        freed_chars = 0
        for idx in to_prune:
            msg = ctx.messages[idx]
            if msg.content.startswith("[Previous:"):
                continue

            tool_name = msg.metadata.get("tool_name", "unknown")
            char_count = len(msg.content)
            freed_chars += char_count

            stub = f"[Previous: {tool_name} result — {char_count} chars, pruned to save context]"
            ctx.messages[idx] = Message(
                role=msg.role,
                content=stub,
                metadata=msg.metadata,
            )
            pruned_count += 1

        if pruned_count > 0:
            self._compaction_manager.counter.clear_cache()

        return pruned_count, freed_chars

    def prune_old_tool_results(self, ctx: LoopContext) -> None:
        """Auto-prune tool results older than TTL iterations."""
        ttl = self._compaction_manager.config.tool_result_ttl
        current_iteration = ctx.metrics.iterations

        pruned = 0
        for i, msg in enumerate(ctx.messages):
            if not msg.tool_results:
                continue
            if msg.content.startswith("[Previous:"):
                continue

            added_at = msg.metadata.get("added_at_iteration")
            if added_at is None:
                continue

            if current_iteration - added_at >= ttl:
                tool_name = msg.metadata.get("tool_name", "unknown")
                char_count = len(msg.content)
                stub = (
                    f"[Previous: {tool_name} result — "
                    f"{char_count} chars, pruned to save context]"
                )
                ctx.messages[i] = Message(
                    role=msg.role,
                    content=stub,
                    metadata=msg.metadata,
                )
                pruned += 1

        if pruned > 0:
            self._compaction_manager.counter.clear_cache()
            logger.info(f"[AUTO-PRUNE] Pruned {pruned} tool results (TTL={ttl} iterations)")

    # ------------------------------------------------------------------
    # Context warning
    # ------------------------------------------------------------------

    def inject_context_warning(self, ctx: LoopContext) -> None:
        """Inject context usage warning if over threshold."""
        threshold = self._compaction_manager.config.warning_threshold_percent
        usage = self._compaction_manager.counter.usage_percent(ctx.messages)

        if usage < threshold:
            return

        last_warned = ctx.metadata.get("last_warning_iteration")
        if last_warned == ctx.metrics.iterations:
            return

        max_tokens = self._compaction_manager.counter.max_tokens
        current_tokens = self._compaction_manager.counter.count_messages(ctx.messages)
        pct = int(usage * 100)

        warning = (
            f"[CONTEXT WARNING] Context at {pct}% capacity ({current_tokens}/{max_tokens} tokens). "
            f"Capture findings with entropic.todo_write if needed, "
            f"then call entropic.prune_context."
        )
        ctx.messages.append(Message(role="user", content=warning))
        ctx.metadata["last_warning_iteration"] = ctx.metrics.iterations
        logger.info(f"[WARNING] Context at {pct}% — warning injected")

    # ------------------------------------------------------------------
    # Compaction
    # ------------------------------------------------------------------

    async def check_compaction(self, ctx: LoopContext, *, force: bool = False) -> None:
        """Check if compaction is needed and perform if so."""
        ctx.messages, result = await self._compaction_manager.check_and_compact(
            conversation_id=None,  # TODO: pass actual conversation ID
            messages=ctx.messages,
            force=force,
        )

        if result.compacted:
            logger.info(
                f"Compacted context: {result.old_token_count} -> {result.new_token_count} tokens"
            )
            if self._callbacks.on_compaction:
                self._callbacks.on_compaction(result)
            if self._hooks.after_compaction:
                self._hooks.after_compaction(ctx)
