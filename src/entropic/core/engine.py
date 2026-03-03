"""
Agentic loop engine.

Implements the core plan→act→observe→repeat cycle with
proper state management and termination conditions.
"""

from __future__ import annotations

import asyncio
import json
import threading
import time
from collections.abc import AsyncIterator
from typing import TYPE_CHECKING, Any

from entropic.config.schema import EntropyConfig
from entropic.core.base import Message, ToolCall
from entropic.core.compaction import CompactionManager, TokenCounter
from entropic.core.directives import DirectiveProcessor, DirectiveResult
from entropic.core.engine_types import (  # noqa: F401 — re-exported for consumers
    AgentState,
    EngineCallbacks,
    InterruptContext,
    InterruptMode,
    LoopConfig,
    LoopContext,
    LoopMetrics,
    ToolApproval,
)
from entropic.core.logging import get_logger, get_model_logger
from entropic.core.queue import MessageSource
from entropic.core.tool_executor import ToolExecutor, ToolExecutorHooks
from entropic.inference.orchestrator import ModelOrchestrator
from entropic.mcp.manager import ServerManager

if TYPE_CHECKING:
    from entropic.core.directives import (
        ClearSelfTodos,
        ContextAnchor,
        InjectContext,
        NotifyPresenter,
        PruneMessages,
        StopProcessing,
        TierChange,
    )

logger = get_logger("core.engine")
model_logger = get_model_logger()


class AgentEngine:
    """
    Core agent execution engine.

    Manages the agentic loop lifecycle including:
    - State transitions
    - Tool execution
    - Context management
    - Error recovery
    """

    def __init__(
        self,
        orchestrator: ModelOrchestrator,
        server_manager: ServerManager | None = None,
        config: EntropyConfig | None = None,
        loop_config: LoopConfig | None = None,
    ) -> None:
        """
        Initialize agent engine.

        Args:
            orchestrator: Model orchestrator
            server_manager: MCP server manager. If ``None``, a default
                manager with built-in servers is created on first ``run()``.
            config: Application configuration. Defaults to ``EntropyConfig()``.
            loop_config: Loop-specific configuration
        """
        self.orchestrator = orchestrator
        self.server_manager: ServerManager | None = server_manager
        self.config = config or EntropyConfig()
        self.loop_config = loop_config or LoopConfig()

        # Use threading.Event for thread-safe cross-loop signaling
        # (Textual runs on a different event loop than the generation worker)
        self._interrupt_event = threading.Event()

        # Pause/inject support for interrupting generation
        self._pause_event = threading.Event()
        self._interrupt_context: InterruptContext | None = None

        # Generic context anchors — keyed persistent messages in ctx.messages
        self._context_anchors: dict[str, str] = {}

        # Directive processor for tool-to-engine communication
        self._directive_processor = DirectiveProcessor()
        self._register_directive_handlers()

        # Compaction manager for context management
        max_tokens = self._get_max_context_tokens()
        self._token_counter = TokenCounter(max_tokens)
        self._compaction_manager = CompactionManager(
            config=self.config.compaction,
            token_counter=self._token_counter,
            orchestrator=orchestrator,
        )

        # Shared mutable callback container — all subsystems hold the same reference.
        # set_callbacks() updates fields in place so subsystems see changes.
        self._callbacks = EngineCallbacks()

        # ToolExecutor created lazily (needs server_manager, which may be None at init)
        self._tool_executor: ToolExecutor | None = None

    def _register_directive_handlers(self) -> None:
        """Register handlers for all known directive types."""
        from entropic.core.directives import (
            ClearSelfTodos,
            ContextAnchor,
            InjectContext,
            NotifyPresenter,
            PruneMessages,
            StopProcessing,
            TierChange,
        )

        self._directive_processor.register(StopProcessing, self._directive_stop_processing)
        self._directive_processor.register(TierChange, self._directive_tier_change)
        self._directive_processor.register(ClearSelfTodos, self._directive_clear_self_todos)
        self._directive_processor.register(InjectContext, self._directive_inject_context)
        self._directive_processor.register(PruneMessages, self._directive_prune_messages)
        self._directive_processor.register(ContextAnchor, self._directive_context_anchor)
        self._directive_processor.register(NotifyPresenter, self._directive_notify_presenter)

    def _directive_stop_processing(
        self,
        ctx: LoopContext,
        directive: StopProcessing,
        result: DirectiveResult,
    ) -> None:
        """Handle stop_processing directive."""
        logger.info("[DIRECTIVE] stop_processing — halting tool call loop")
        result.stop_processing = True

    def _directive_tier_change(
        self,
        ctx: LoopContext,
        directive: TierChange,
        result: DirectiveResult,
    ) -> None:
        """Handle tier_change directive — validate and execute tier switch."""
        target_tier_str = directive.tier
        reason = directive.reason

        target_tier = self.orchestrator._find_tier(target_tier_str)
        if target_tier is None:
            logger.error(f"[DIRECTIVE] Invalid tier in tier_change: {target_tier_str}")
            return

        if not self.orchestrator.can_handoff(ctx.locked_tier, target_tier):
            current_name = str(ctx.locked_tier) if ctx.locked_tier else "none"
            logger.warning(
                f"[DIRECTIVE] Handoff not permitted: {current_name} -> {target_tier_str}"
            )
            return

        current_tier = ctx.locked_tier
        ctx.locked_tier = target_tier
        result.tier_changed = True

        # Rebuild system prompt for new tier
        ctx.messages[0] = Message(
            role="system",
            content=self._build_formatted_system_prompt(target_tier, ctx),
        )

        current_name = str(current_tier) if current_tier else "none"
        model_logger.info(
            f"\n{'#' * 70}\n"
            f"[HANDOFF] {current_name} -> {target_tier_str} | reason: {reason}\n"
            f"{'#' * 70}"
        )
        self._log_assembled_prompt(ctx, "handoff")
        self._notify_tier_selected(target_tier_str)

        # Reinject context anchors for new tier's awareness
        self._reinject_context_anchors(ctx)

        logger.info(f"[DIRECTIVE] tier_change: {current_name} -> {target_tier_str}")

    def _directive_clear_self_todos(
        self,
        ctx: LoopContext,
        directive: ClearSelfTodos,
        result: DirectiveResult,
    ) -> None:
        """Handle clear_self_todos directive — not applicable in directive arch.

        In the directive architecture, the EntropicServer owns the TodoList
        and clears self-todos internally during handoff. This directive is
        a no-op on the engine side (the server already did the work).
        """
        logger.debug("[DIRECTIVE] clear_self_todos acknowledged")

    def _directive_inject_context(
        self,
        ctx: LoopContext,
        directive: InjectContext,
        result: DirectiveResult,
    ) -> None:
        """Handle inject_context directive — add message to context."""
        if directive.content:
            result.injected_messages.append(Message(role="user", content=directive.content))
            logger.info(f"[DIRECTIVE] inject_context: {directive.content[:80]}")

    def _directive_prune_messages(
        self,
        ctx: LoopContext,
        directive: PruneMessages,
        result: DirectiveResult,
    ) -> None:
        """Handle prune_messages directive — prune old tool results."""
        pruned_count, freed_chars = self._prune_tool_results(ctx, directive.keep_recent)
        logger.info(
            f"[DIRECTIVE] prune_messages: pruned {pruned_count} results "
            f"(~{freed_chars // 4} tokens freed)"
        )

    def _directive_context_anchor(
        self,
        ctx: LoopContext,
        directive: ContextAnchor,
        result: DirectiveResult,
    ) -> None:
        """Handle context_anchor directive — update keyed persistent message."""
        if not directive.content:
            self._context_anchors.pop(directive.key, None)
            ctx.messages = [
                m for m in ctx.messages if m.metadata.get("anchor_key") != directive.key
            ]
            logger.info(f"Removed context anchor: {directive.key}")
            return

        self._context_anchors[directive.key] = directive.content
        # Remove existing anchor with this key
        ctx.messages = [m for m in ctx.messages if m.metadata.get("anchor_key") != directive.key]
        # Append at end (recency bias)
        anchor = Message(
            role="user",
            content=directive.content,
            metadata={"is_context_anchor": True, "anchor_key": directive.key},
        )
        ctx.messages.append(anchor)
        logger.info(f"Updated context anchor: {directive.key}")

    def _directive_notify_presenter(
        self,
        ctx: LoopContext,
        directive: NotifyPresenter,
        result: DirectiveResult,
    ) -> None:
        """Handle notify_presenter directive — pass through to UI callback."""
        if self._callbacks.on_presenter_notify:
            self._callbacks.on_presenter_notify(directive.key, directive.data)

    def _get_max_context_tokens(self) -> int:
        """Get maximum context tokens from model config."""
        default_name = self.config.models.default
        tier_config = self.config.models.tiers.get(default_name)
        if tier_config:
            return tier_config.context_length
        return 16384

    def _refresh_context_limit(self) -> None:
        """Refresh context limit based on current model.

        When models swap (e.g., normal → thinking), the context limit
        may change. This method updates the token counter to use the
        current model's context length.
        """
        last_tier = self.orchestrator.last_used_tier
        if last_tier is None:
            return

        tier_config = self.config.models.tiers.get(str(last_tier))
        if tier_config:
            new_max = tier_config.context_length
            if new_max != self._token_counter.max_tokens:
                logger.debug(
                    f"Updating context limit: {self._token_counter.max_tokens} -> {new_max}"
                )
                self._token_counter.max_tokens = new_max

    def set_callbacks(self, callbacks: EngineCallbacks) -> None:
        """Set callback functions for loop events.

        Updates the shared EngineCallbacks instance in place so all
        subsystems (ToolExecutor, etc.) see the changes immediately.

        Args:
            callbacks: Callback configuration for engine events
        """
        from dataclasses import fields

        for f in fields(callbacks):
            setattr(self._callbacks, f.name, getattr(callbacks, f.name))

    async def _create_default_server_manager(self) -> ServerManager:
        """Create and initialize a default ServerManager from config."""
        tier_names = list(self.config.models.tiers.keys()) or None
        manager = ServerManager(self.config, tier_names=tier_names)
        await manager.initialize()
        logger.info("Created default ServerManager from config")
        return manager

    async def run(
        self,
        user_message: str,
        history: list[Message] | None = None,
        system_prompt: str | None = None,
        task_id: str | None = None,
        source: str = MessageSource.HUMAN,
    ) -> AsyncIterator[Message]:
        """
        Run the agentic loop.

        Args:
            user_message: User's input message
            history: Optional conversation history
            system_prompt: Optional system prompt override
            task_id: Optional task ID for external tracking (MCP integration)
            source: Message source (human, claude-code, system)

        Yields:
            Messages generated during the loop
        """
        # Lazy-init default ServerManager if none was provided
        if self.server_manager is None:
            self.server_manager = await self._create_default_server_manager()

        # Initialize context
        ctx = LoopContext()
        ctx.metrics.start_time = time.time()
        ctx.task_id = task_id
        ctx.source = source

        # Reset interrupt flag from any previous run
        self.reset_interrupt()
        self._pause_event.clear()

        # Build initial messages and get tools for system prompt
        tools = await self.server_manager.list_tools()
        logger.info(f"Tools available ({len(tools)}): {[t['name'] for t in tools]}")

        system = system_prompt or ""

        # Store on ctx for system prompt rebuild on tier change
        ctx.all_tools = tools
        ctx.base_system = system

        # Placeholder system message — rebuilt with tier-filtered tools in _lock_tier_if_needed
        ctx.messages = [Message(role="system", content=system)]

        if history:
            ctx.messages.extend(history)

        user_msg = Message(role="user", content=user_message)
        user_msg.metadata["source"] = "user"
        ctx.messages.append(user_msg)

        # Reinject context anchors from previous runs (survives across run() calls)
        self._reinject_context_anchors(ctx)

        # Log user prompt for debugging
        logger.info(f"\n{'=' * 60}\n[USER PROMPT]\n{'=' * 60}\n{user_message}\n{'=' * 60}")

        self._set_state(ctx, AgentState.PLANNING)

        try:
            async for message in self._loop(ctx):
                yield message
        finally:
            ctx.metrics.end_time = time.time()
            logger.info(
                f"Loop complete: {ctx.metrics.iterations} iterations, "
                f"{ctx.metrics.tool_calls} tool calls, "
                f"{ctx.metrics.duration_ms}ms"
            )

    async def _loop(
        self,
        ctx: LoopContext,
    ) -> AsyncIterator[Message]:
        """Main loop implementation."""
        while not self._should_stop(ctx):
            ctx.metrics.iterations += 1

            if self._interrupt_event.is_set():
                self._set_state(ctx, AgentState.INTERRUPTED)
                break

            async for msg in self._execute_iteration(ctx):
                yield msg

            if ctx.state == AgentState.ERROR:
                break

        # If we exited due to max iterations, warn the user
        if ctx.metrics.iterations >= self.loop_config.max_iterations:
            logger.warning("Loop ended due to max iterations")
            yield Message(
                role="assistant",
                content="[Note: Maximum iterations reached. The task may be incomplete.]",
            )

    async def _execute_iteration(self, ctx: LoopContext) -> AsyncIterator[Message]:
        """Execute a single loop iteration."""
        logger.info(
            f"[LOOP START] Iteration {ctx.metrics.iterations}/{self.loop_config.max_iterations} | "
            f"state={ctx.state.name} | messages={len(ctx.messages)} | errors={ctx.consecutive_errors}"
        )
        try:
            # Refresh context limit in case model changed
            self._refresh_context_limit()

            # Prune old tool results before compaction check
            self._prune_old_tool_results(ctx)

            # Check for compaction before generating
            await self._check_compaction(ctx)

            self._set_state(ctx, AgentState.EXECUTING)
            content, tool_calls = await self._generate_response(ctx)

            logger.debug(
                f"Iteration {ctx.metrics.iterations}: "
                f"content_len={len(content)}, tool_calls={len(tool_calls)}, "
                f"content_preview={content[:100]!r}"
            )

            assistant_msg = self._create_assistant_message(content, tool_calls)
            ctx.messages.append(assistant_msg)

            yield assistant_msg

            if tool_calls:
                ctx.effective_tool_calls = 0
                async for msg in self._process_tool_calls(ctx, tool_calls):
                    yield msg
                # All tool calls blocked/denied — fall through to standard decision
                if ctx.effective_tool_calls == 0:
                    await self._evaluate_no_tool_decision(ctx, content)
            else:
                await self._evaluate_no_tool_decision(ctx, content)

        except Exception as e:
            logger.exception(f"Loop iteration error: {e}")
            async for msg in self._handle_error(ctx, e):
                yield msg

    async def _evaluate_no_tool_decision(self, ctx: LoopContext, content: str) -> None:
        """Evaluate loop decision when no tool calls were effective.

        Called when either:
          - Model produced no tool calls
          - Model produced tool calls but ALL were blocked/denied
        """
        finish_reason = self.orchestrator.last_finish_reason

        if await self._try_auto_chain(ctx, finish_reason):
            logger.info("[LOOP DECISION] auto_chain fired, continuing with new tier")
            return

        if finish_reason == "length":
            logger.info("[LOOP DECISION] finish_reason=length, continuing loop")
            return

        adapter = self.orchestrator.get_adapter()
        is_complete = adapter.is_response_complete(content, [])
        logger.info(f"[LOOP DECISION] tool_calls=0, is_response_complete={is_complete}")
        if is_complete:
            self._set_state(ctx, AgentState.COMPLETE)

    def _log_model_output(
        self,
        ctx: LoopContext,
        raw_content: str,
        cleaned_content: str,
        tool_calls: list[ToolCall],
        finish_reason: str,
    ) -> None:
        """Log raw and parsed model output to dedicated model log."""
        # Detailed raw + parsed output to dedicated model log
        model_logger.info(
            f"\n{'=' * 70}\n"
            f"[TURN {ctx.metrics.iterations}] finish_reason={finish_reason}\n"
            f"{'=' * 70}\n"
            f"--- RAW OUTPUT ---\n"
            f"{raw_content}\n"
            f"--- PARSED ---\n"
            f"cleaned_content_len={len(cleaned_content)}\n"
            f"tool_calls={len(tool_calls)}: {[tc.name for tc in tool_calls]}\n"
            f"{'=' * 70}"
        )
        # Summary only in session.log
        logger.info(
            f"[MODEL OUTPUT] Turn {ctx.metrics.iterations} | "
            f"finish_reason={finish_reason} | "
            f"raw_len={len(raw_content)} | "
            f"cleaned_len={len(cleaned_content)} | "
            f"tool_calls={len(tool_calls)}"
        )

    def _log_assembled_prompt(self, ctx: LoopContext, event: str) -> None:
        """Log complete assembled prompt. Called at routing and handoff only."""
        tier_value = ctx.locked_tier.name if ctx.locked_tier else "none"
        model_logger.info(
            f"\n{'~' * 70}\n"
            f"[PROMPT] event={event} tier={tier_value} "
            f"messages={len(ctx.messages)}\n"
            f"{'~' * 70}"
        )
        for i, msg in enumerate(ctx.messages):
            model_logger.info(f"  [{i}] {msg.role} ({len(msg.content)} chars):\n{msg.content}")
        model_logger.info(f"{'~' * 70}")

    def _filter_tools_for_tier(
        self, tools: list[dict[str, Any]], tier: Any
    ) -> list[dict[str, Any]]:
        """Filter tool list based on tier's allowed_tools config.

        Args:
            tools: Full tool list
            tier: ModelTier to filter for

        Returns:
            Filtered tool list (unchanged if tier allows all tools)
        """
        allowed = self.orchestrator.get_allowed_tools(tier)
        if allowed is None:
            return tools
        return [t for t in tools if t["name"] in allowed]

    def _should_auto_chain(self, ctx: LoopContext, finish_reason: str) -> bool:
        """Check whether auto-chain should trigger for the current tier.

        Triggers when auto_chain=True AND either:
          - finish_reason == "length" (token budget exhausted)
          - finish_reason == "stop" AND tier has grammar configured
        """
        tier_config = self.config.models.tiers.get(ctx.locked_tier.name if ctx.locked_tier else "")
        if not tier_config or not tier_config.auto_chain:
            return False
        is_grammar_completion = finish_reason == "stop" and tier_config.grammar is not None
        if is_grammar_completion:
            logger.info("[AUTO_CHAIN] Grammar completion trigger (grammar + stop)")
        return finish_reason == "length" or is_grammar_completion

    async def _try_auto_chain(self, ctx: LoopContext, finish_reason: str) -> bool:
        """Attempt auto-chain handoff on token exhaustion or grammar completion.

        Returns True if chain fired (tier changed), False otherwise.
        """
        if not self._should_auto_chain(ctx, finish_reason):
            return False

        targets = self.orchestrator.get_handoff_targets(ctx.locked_tier)
        if not targets:
            logger.warning("[AUTO_CHAIN] auto_chain=True but no handoff targets configured")
            return False

        target = await self.orchestrator.route_among(targets)

        from entropic.core.directives import TierChange

        directive = TierChange(tier=target.name, reason="auto_chain")
        result = DirectiveResult()
        self._directive_tier_change(ctx, directive, result)
        return result.tier_changed

    def _build_formatted_system_prompt(self, tier: Any, ctx: LoopContext) -> str:
        """Build system prompt formatted for a specific tier's adapter.

        Args:
            tier: ModelTier to build prompt for
            ctx: Loop context with base_system and all_tools

        Returns:
            Formatted system prompt string
        """
        filtered = self._filter_tools_for_tier(ctx.all_tools, tier)
        adapter = self.orchestrator.get_adapter(tier)
        tier_config = self.config.models.tiers.get(tier.name) if hasattr(tier, "name") else None
        enable_thinking = tier_config.enable_thinking if tier_config else True

        system = ctx.base_system
        if tier_config and self.config.inject_model_context:
            system = self._inject_model_context(system, tier, tier_config)

        return adapter.format_system_prompt(system, filtered, enable_thinking=enable_thinking)

    def _inject_model_context(self, system: str, tier: Any, tier_config: Any) -> str:
        """Append model configuration context to the system prompt.

        Gives the model accurate self-knowledge about its own tier,
        model file, and configuration — derived from actual config,
        not hardcoded documentation.
        """
        model_name = (
            tier_config.path.name if hasattr(tier_config.path, "name") else str(tier_config.path)
        )
        tier_name = tier.name if hasattr(tier, "name") else str(tier)
        lines = [
            "\n\n## Model Context (auto-injected from config)",
            f"- **Active tier**: {tier_name}",
            f"- **Model file**: {model_name}",
            f"- **Adapter**: {tier_config.adapter}",
            f"- **Context length**: {tier_config.context_length:,} tokens",
            f"- **GPU layers**: {tier_config.gpu_layers}",
        ]
        return system + "\n".join(lines)

    async def _lock_tier_if_needed(self, ctx: LoopContext) -> None:
        """Route and lock tier before first generation, emitting callbacks."""
        if ctx.locked_tier is not None:
            return
        # Route to determine tier before generation starts
        tier = await self.orchestrator.route(ctx.messages)
        ctx.locked_tier = tier
        logger.debug(f"Locked tier for loop: {ctx.locked_tier}")

        # Rebuild system prompt for the routed tier's adapter and tool filter
        formatted = self._build_formatted_system_prompt(tier, ctx)
        ctx.messages[0] = Message(role="system", content=formatted)
        logger.info(f"System prompt size: ~{len(formatted) // 4} tokens")

        # Log routing decision and full assembled prompt to model log
        model_logger.info(f"\n{'#' * 70}\n[ROUTED] tier={tier.name}\n{'#' * 70}")
        self._log_assembled_prompt(ctx, "routed")

        if self._callbacks.on_tier_selected:
            self._callbacks.on_tier_selected(tier.name)
        routing_result = self.orchestrator.last_routing_result
        if routing_result and self._callbacks.on_routing_complete:
            self._callbacks.on_routing_complete(routing_result)

    def _notify_tier_selected(self, tier_value: str) -> None:
        """Notify callback of tier selection (for handoff)."""
        if self._callbacks.on_tier_selected:
            self._callbacks.on_tier_selected(tier_value)

    async def _generate_response(self, ctx: LoopContext) -> tuple[str, list[ToolCall]]:
        """Generate model response, streaming or not.

        Routes first (if no tier locked), emits routing callback,
        then generates with the resolved tier.
        """
        logger.debug(f"Generating response (stream={self.loop_config.stream_output})")

        # Route and lock tier before generation starts
        await self._lock_tier_if_needed(ctx)

        if self.loop_config.stream_output:
            return await self._generate_streaming(ctx)

        return await self._generate_non_streaming(ctx)

    async def _generate_streaming(self, ctx: LoopContext) -> tuple[str, list[ToolCall]]:
        """Generate response via streaming."""
        content = ""

        interrupted = False
        async for chunk in self.orchestrator.generate_stream(ctx.messages, tier=ctx.locked_tier):
            if self._interrupt_event.is_set():
                interrupted = True
                break

            if self._pause_event.is_set():
                logger.info("Generation paused by user")
                content = await self._handle_pause(ctx, content)
                if self._interrupt_event.is_set():
                    interrupted = True
                    break

            content += chunk

            if self._callbacks.on_stream_chunk:
                self._callbacks.on_stream_chunk(chunk)

        if interrupted:
            logger.info(f"Stream interrupted after {len(content)} chars")

        cleaned_content, tool_calls = self.orchestrator.get_adapter().parse_tool_calls(content)
        self._log_model_output(
            ctx,
            raw_content=content,
            cleaned_content=cleaned_content,
            tool_calls=tool_calls,
            finish_reason="interrupted" if interrupted else self.orchestrator.last_finish_reason,
        )
        return cleaned_content, tool_calls

    async def _generate_non_streaming(self, ctx: LoopContext) -> tuple[str, list[ToolCall]]:
        """Generate response without streaming."""
        result = await self.orchestrator.generate(ctx.messages, tier=ctx.locked_tier)

        self._log_model_output(
            ctx,
            raw_content=result.raw_content or result.content,
            cleaned_content=result.content,
            tool_calls=result.tool_calls,
            finish_reason=result.finish_reason,
        )

        ctx.metrics.tokens_used += result.token_count
        return result.content, result.tool_calls

    async def _handle_pause(self, ctx: LoopContext, partial_content: str) -> str:
        """
        Handle pause during generation.

        Prompts user for injection and either continues with context or resumes.
        """
        # Save partial state
        self._interrupt_context = InterruptContext(
            partial_content=partial_content,
            mode=InterruptMode.PAUSE,
        )
        self._set_state(ctx, AgentState.PAUSED)

        # Call the pause prompt callback to get user input
        injection = None
        if self._callbacks.on_pause_prompt:
            result = self._callbacks.on_pause_prompt(partial_content)
            if asyncio.iscoroutine(result):
                injection = await result
            else:
                injection = result

        # Clear pause event
        self._pause_event.clear()

        if injection is None:
            # User cancelled - set interrupt
            logger.info("Pause cancelled by user")
            self._interrupt_event.set()
            return partial_content

        if injection.strip():
            # User provided injection - continue with new context
            logger.info(f"Continuing with injection: {injection[:50]}...")
            return await self._continue_with_injection(ctx, partial_content, injection)

        # Empty injection - just resume
        logger.info("Resuming without injection")
        self._set_state(ctx, AgentState.EXECUTING)
        return partial_content

    async def _continue_with_injection(
        self,
        ctx: LoopContext,
        partial_content: str,
        injection: str,
    ) -> str:
        """Continue generation with injected user context."""
        # Add partial response to context (if any meaningful content)
        if partial_content.strip():
            ctx.messages.append(
                Message(
                    role="assistant",
                    content=partial_content + "\n\n[Generation paused by user]",
                )
            )

        # Add user injection
        ctx.messages.append(
            Message(
                role="user",
                content=f"[User interjection]: {injection}\n\nPlease continue with this in mind.",
            )
        )

        # Clear pause state and resume
        self._set_state(ctx, AgentState.EXECUTING)

        # Generate new response with injected context
        content = ""
        async for chunk in self.orchestrator.generate_stream(ctx.messages, tier=ctx.locked_tier):
            if self._interrupt_event.is_set():
                break

            if self._pause_event.is_set():
                logger.info("Generation paused again during continuation")
                content = await self._handle_pause(ctx, content)
                if self._interrupt_event.is_set():
                    break

            content += chunk
            if self._callbacks.on_stream_chunk:
                self._callbacks.on_stream_chunk(chunk)

        return content

    def _create_assistant_message(self, content: str, tool_calls: list[ToolCall]) -> Message:
        """Create assistant message with tool calls.

        Note: Empty assistant messages can cause llama_decode errors with some models
        (especially Falcon H1R). When content is empty but tool calls exist, we preserve
        a representation of the tool calls to ensure valid ChatML formatting.
        """
        # Prevent empty assistant messages which can cause KV cache issues
        if not content.strip() and tool_calls:
            # Use proper <tool_call> format so model learns correct pattern
            # (prevents feedback loop where model mimics [Calling: ...] format)
            content = "\n".join(
                [
                    f'<tool_call>{{"name": "{tc.name}", "arguments": {json.dumps(tc.arguments)}}}</tool_call>'
                    for tc in tool_calls
                ]
            )

        return Message(
            role="assistant",
            content=content,
            tool_calls=[
                {"id": tc.id, "name": tc.name, "arguments": tc.arguments} for tc in tool_calls
            ],
        )

    def _ensure_tool_executor(self) -> ToolExecutor:
        """Create or return the ToolExecutor subsystem.

        Created lazily because server_manager may be None at __init__ time
        (it's set up in run() on first call).
        """
        if self._tool_executor is None:
            assert self.server_manager is not None
            self._tool_executor = ToolExecutor(
                server_manager=self.server_manager,
                orchestrator=self.orchestrator,
                loop_config=self.loop_config,
                callbacks=self._callbacks,
                hooks=ToolExecutorHooks(
                    after_tool=self._after_tool_hook,
                    process_directives=self._process_directives,
                ),
            )
        return self._tool_executor

    async def _after_tool_hook(self, ctx: LoopContext) -> None:
        """Post-tool hook: compaction check + context warning injection."""
        await self._check_compaction(ctx)
        self._inject_context_warning(ctx)

    async def _process_tool_calls(
        self, ctx: LoopContext, tool_calls: list[ToolCall]
    ) -> AsyncIterator[Message]:
        """Delegate tool call processing to ToolExecutor."""
        executor = self._ensure_tool_executor()
        async for msg in executor.process_tool_calls(ctx, tool_calls):
            yield msg

    def _process_directives(self, ctx: LoopContext, directives: list[Any]) -> DirectiveResult:
        """Process directives from a tool result.

        Args:
            ctx: Current loop context
            directives: Native Directive objects from ToolResult

        Returns:
            Aggregate directive result
        """
        result = self._directive_processor.process(ctx, directives)

        # Append any injected messages to context
        for injected in result.injected_messages:
            ctx.messages.append(injected)

        return result

    def _prune_tool_results(self, ctx: LoopContext, keep_recent: int) -> tuple[int, int]:
        """Replace old tool results with stubs. Returns (pruned_count, freed_chars)."""
        # Find all tool result messages (newest first for keep_recent logic)
        tool_result_indices = [i for i, msg in enumerate(ctx.messages) if msg.tool_results]

        # Keep the most recent N
        to_prune = tool_result_indices[:-keep_recent] if keep_recent > 0 else tool_result_indices

        pruned_count = 0
        freed_chars = 0
        for idx in to_prune:
            msg = ctx.messages[idx]
            # Skip already-pruned messages
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

    def _prune_old_tool_results(self, ctx: LoopContext) -> None:
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
                    f"[Previous: {tool_name} result — {char_count} chars, pruned to save context]"
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

    def _inject_context_warning(self, ctx: LoopContext) -> None:
        """Inject context usage warning if over threshold."""
        threshold = self._compaction_manager.config.warning_threshold_percent
        usage = self._compaction_manager.counter.usage_percent(ctx.messages)

        if usage < threshold:
            return

        # Don't repeat warning in same iteration
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

    async def _handle_error(self, ctx: LoopContext, error: Exception) -> AsyncIterator[Message]:
        """Handle loop iteration error."""
        # Context overflow: recover via forced compaction instead of counting as error
        if isinstance(error, ValueError) and "exceed context window" in str(error):
            logger.warning(f"[RECOVERY] Context overflow detected, forcing compaction: {error}")
            await self._check_compaction(ctx, force=True)
            return

        logger.error(f"Loop error: {error}")
        ctx.consecutive_errors += 1
        ctx.metrics.errors += 1

        if ctx.consecutive_errors >= self.loop_config.max_consecutive_errors:
            self._set_state(ctx, AgentState.ERROR)
            yield Message(
                role="assistant",
                content=f"I encountered repeated errors and cannot continue: {error}",
            )

    def _should_stop(self, ctx: LoopContext) -> bool:
        """Check if loop should stop."""
        # Check termination states
        if ctx.state in (AgentState.COMPLETE, AgentState.ERROR, AgentState.INTERRUPTED):
            return True

        # Check limits with logging
        if ctx.metrics.iterations >= self.loop_config.max_iterations:
            logger.warning("Max iterations reached")
        elif ctx.consecutive_duplicate_attempts >= 3:
            logger.warning("Stopping loop: model stuck in duplicate tool calls")
        else:
            return False

        return True

    def _set_state(self, ctx: LoopContext, state: AgentState) -> None:
        """Set agent state and notify callback."""
        ctx.state = state
        logger.info(f"State: {state.name}")
        if self._callbacks.on_state_change:
            self._callbacks.on_state_change(state)

    async def _check_compaction(self, ctx: LoopContext, *, force: bool = False) -> None:
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
            # Notify via callback
            if self._callbacks.on_compaction:
                self._callbacks.on_compaction(result)
            # Reinject context anchors so model retains awareness
            self._reinject_context_anchors(ctx)

    def _reinject_context_anchors(self, ctx: LoopContext) -> None:
        """Reinject all cached context anchors into ctx.messages.

        Called after compaction, tier change, or at run() start to ensure
        anchored state survives context mutations. Each anchor is a single
        keyed message — zero context growth per anchor.
        """
        from entropic.core.directives import ContextAnchor

        for key, content in self._context_anchors.items():
            self._directive_context_anchor(
                ctx, ContextAnchor(key=key, content=content), DirectiveResult()
            )

    def interrupt(self) -> None:
        """Interrupt the running loop (hard cancel)."""
        logger.info("Engine interrupted by user")
        self._interrupt_event.set()
        # Also clear any pending pause
        self._pause_event.clear()

    def reset_interrupt(self) -> None:
        """Reset interrupt flag."""
        self._interrupt_event.clear()

    def pause(self) -> None:
        """Pause generation (Escape key) to allow injection."""
        logger.info("Engine paused by user")
        self._pause_event.set()

    def inject(self, content: str) -> None:
        """Inject user content and resume generation.

        Note: Currently injection happens via _on_pause_prompt callback.
        This method is reserved for future direct injection support.
        """
        logger.debug(f"Injecting content: {content[:50]}...")
        # Injection now handled via _on_pause_prompt callback in _handle_pause()

    def resume(self) -> None:
        """Resume generation without injection.

        Note: Currently resume happens via _on_pause_prompt returning empty string.
        This method is reserved for future direct resume support.
        """
        logger.debug("Resume without injection")
        # Resume now handled via _on_pause_prompt callback in _handle_pause()

    def cancel_pause(self) -> None:
        """Cancel pause and interrupt completely."""
        logger.info("Pause cancelled, interrupting")
        self._pause_event.clear()
        self._interrupt_event.set()

    @property
    def is_paused(self) -> bool:
        """Check if generation is paused."""
        return self._pause_event.is_set()
