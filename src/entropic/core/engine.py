"""
Agentic loop engine.

Implements the core plan→act→observe→repeat cycle with
proper state management and termination conditions.
"""

from __future__ import annotations

import threading
import time
from collections.abc import AsyncIterator
from typing import TYPE_CHECKING, Any

from entropic.config.schema import EntropyConfig
from entropic.core.base import Message, ModelTier, ToolCall
from entropic.core.compaction import CompactionManager, TokenCounter
from entropic.core.context_manager import ContextManager, ContextManagerHooks
from entropic.core.directives import DirectiveProcessor, DirectiveResult
from entropic.core.engine_types import (  # noqa: F401 — re-exported for consumers
    AgentState,
    EngineCallbacks,
    GenerationEvents,
    InterruptContext,
    InterruptMode,
    LoopConfig,
    LoopContext,
    LoopMetrics,
    ToolApproval,
)
from entropic.core.logging import get_logger, get_model_logger
from entropic.core.queue import MessageSource
from entropic.core.response_generator import ResponseGenerator
from entropic.core.tool_executor import ToolExecutor, ToolExecutorHooks
from entropic.inference.orchestrator import ModelOrchestrator
from entropic.mcp.manager import ServerManager

if TYPE_CHECKING:
    from entropic.core.directives import (
        ClearSelfTodos,
        ContextAnchor,
        Delegate,
        InjectContext,
        NotifyPresenter,
        Pipeline,
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
        self._storage: Any = None  # Optional StorageBackend for delegation persistence

        # Use threading.Event for thread-safe cross-loop signaling
        # (Textual runs on a different event loop than the generation worker)
        self._interrupt_event = threading.Event()

        # Pause/inject support for interrupting generation
        self._pause_event = threading.Event()

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

        # Subsystem initialization:
        #   ResponseGenerator, ContextManager — EAGER: all deps available at __init__
        #   ToolExecutor — LAZY: server_manager is None at init, set async in run()
        self._response_generator = ResponseGenerator(
            orchestrator=self.orchestrator,
            config=self.config,
            loop_config=self.loop_config,
            callbacks=self._callbacks,
            events=GenerationEvents(
                interrupt=self._interrupt_event,
                pause=self._pause_event,
            ),
        )
        self._context_manager = ContextManager(
            config=self.config,
            orchestrator=self.orchestrator,
            compaction_manager=self._compaction_manager,
            callbacks=self._callbacks,
            hooks=ContextManagerHooks(
                after_compaction=self._reinject_context_anchors,
            ),
        )
        self._tool_executor: ToolExecutor | None = None

    def _register_directive_handlers(self) -> None:
        """Register handlers for all known directive types."""
        from entropic.core.directives import (
            ClearSelfTodos,
            ContextAnchor,
            Delegate,
            InjectContext,
            NotifyPresenter,
            Pipeline,
            PruneMessages,
            StopProcessing,
            TierChange,
        )

        self._directive_processor.register(StopProcessing, self._directive_stop_processing)
        self._directive_processor.register(TierChange, self._directive_tier_change)
        self._directive_processor.register(Delegate, self._directive_delegate)
        self._directive_processor.register(Pipeline, self._directive_pipeline)
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

    def _directive_delegate(
        self,
        ctx: LoopContext,
        directive: Delegate,
        result: DirectiveResult,
    ) -> None:
        """Handle delegate directive — spawn child inference loop.

        Validates depth, fires callbacks, runs DelegationManager, and
        injects the child's result back into the parent context.
        """
        logger.info(
            "[DIRECTIVE] delegate: target=%s task=%s max_turns=%s depth=%d",
            directive.target,
            directive.task[:80],
            directive.max_turns,
            ctx.delegation_depth,
        )

        # Store delegation request for async execution after directive processing
        ctx.metadata["pending_delegation"] = {
            "target": directive.target,
            "task": directive.task,
            "max_turns": directive.max_turns,
        }
        result.stop_processing = True

    def _directive_pipeline(
        self,
        ctx: LoopContext,
        directive: Pipeline,
        result: DirectiveResult,
    ) -> None:
        """Handle pipeline directive — store for async execution."""
        logger.info(
            "[DIRECTIVE] pipeline: stages=%s task=%s",
            directive.stages,
            directive.task[:80],
        )
        ctx.metadata["pending_pipeline"] = {
            "stages": directive.stages,
            "task": directive.task,
        }
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
            ctx.messages.append(
                Message(
                    role="user",
                    content=f"[SYSTEM] Delegation rejected: '{target_tier_str}' is not a valid identity tier.",
                )
            )
            return

        if not self.orchestrator.can_handoff(ctx.locked_tier, target_tier):
            current_name = str(ctx.locked_tier) if ctx.locked_tier else "none"
            is_self = ctx.locked_tier == target_tier
            if is_self:
                msg = (
                    f"[SYSTEM] Delegation rejected: you ARE the {current_name} identity. "
                    f"You cannot delegate to yourself. Execute the task directly "
                    f"using your available tools."
                )
            else:
                msg = (
                    f"[SYSTEM] Delegation rejected: {current_name} cannot delegate to "
                    f"{target_tier_str}. Use entropic.delegate with a permitted target."
                )
            logger.warning(
                f"[DIRECTIVE] Delegation not permitted: {current_name} -> {target_tier_str}"
            )
            ctx.messages.append(Message(role="user", content=msg))
            return

        current_tier = ctx.locked_tier
        ctx.locked_tier = target_tier
        result.tier_changed = True

        # Rebuild system prompt for new tier
        rg = self._response_generator
        ctx.messages[0] = Message(
            role="system",
            content=rg._build_formatted_system_prompt(target_tier, ctx),
        )

        current_name = str(current_tier) if current_tier else "none"

        # Inject chain context so the receiving identity knows why it was activated
        chain_note = (
            f"[SYSTEM] You were activated via {reason} from the "
            f"{current_name} identity. Review the conversation history "
            f"for context on the task in progress. Continue the work "
            f"using your available tools."
        )
        ctx.messages.append(Message(role="user", content=chain_note))

        model_logger.info(
            f"\n{'#' * 70}\n"
            f"[DELEGATE] {current_name} -> {target_tier_str} | reason: {reason}\n"
            f"{'#' * 70}"
        )
        rg._log_assembled_prompt(ctx, "delegate")
        rg._notify_tier_selected(target_tier_str)

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
        pruned_count, freed_chars = self._context_manager.prune_tool_results(
            ctx, directive.keep_recent
        )
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

    def set_storage(self, storage: Any) -> None:
        """Set the storage backend for delegation persistence."""
        self._storage = storage

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
            self._context_manager.refresh_context_limit(ctx)

            # Prune old tool results before compaction check
            self._context_manager.prune_old_tool_results(ctx)

            # Check for compaction before generating
            await self._context_manager.check_compaction(ctx)

            self._set_state(ctx, AgentState.EXECUTING)
            content, tool_calls = await self._response_generator.generate_response(ctx)

            logger.debug(
                f"Iteration {ctx.metrics.iterations}: "
                f"content_len={len(content)}, tool_calls={len(tool_calls)}, "
                f"content_preview={content[:100]!r}"
            )

            assistant_msg = ResponseGenerator.create_assistant_message(content, tool_calls)
            ctx.messages.append(assistant_msg)

            yield assistant_msg

            if tool_calls:
                ctx.effective_tool_calls = 0
                async for msg in self._process_tool_calls(ctx, tool_calls):
                    yield msg

                # Execute pending delegation/pipeline if directive was processed
                if "pending_delegation" in ctx.metadata:
                    async for msg in self._execute_pending_delegation(ctx):
                        yield msg
                    return
                if "pending_pipeline" in ctx.metadata:
                    async for msg in self._execute_pending_pipeline(ctx):
                        yield msg
                    return

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

        if await self._try_auto_chain(ctx, finish_reason, content):
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

    async def _execute_pending_delegation(self, ctx: LoopContext) -> AsyncIterator[Message]:
        """Execute a pending delegation request stored by _directive_delegate."""
        delegation = ctx.metadata.pop("pending_delegation")
        target = delegation["target"]
        task = delegation["task"]
        max_turns = delegation.get("max_turns")

        self._set_state(ctx, AgentState.DELEGATING)

        # Fire delegation start callback
        child_conv_id = str(__import__("uuid").uuid4())
        if self._callbacks.on_delegation_start:
            self._callbacks.on_delegation_start(child_conv_id, target, task)

        from entropic.core.delegation import DelegationManager

        manager = DelegationManager(self, storage=self._storage)
        result = await manager.execute_delegation(
            ctx,
            target,
            task,
            max_turns,
            parent_conversation_id=ctx.metadata.get("conversation_id"),
        )

        # Inject result into parent context
        status = "COMPLETE" if result.success else "FAILED"
        result_msg = Message(
            role="user",
            content=(
                f"[DELEGATION {status}: {target}] {result.summary}\n"
                f"(turns used: {result.turns_used})"
            ),
            metadata={"delegation_result": True, "target_tier": target},
        )
        ctx.messages.append(result_msg)
        yield result_msg

        # Fire delegation complete callback
        if self._callbacks.on_delegation_complete:
            self._callbacks.on_delegation_complete(
                child_conv_id, target, result.summary[:200], result.success
            )

        # Resume parent loop — parent processes the result
        self._set_state(ctx, AgentState.EXECUTING)

    async def _execute_pending_pipeline(self, ctx: LoopContext) -> AsyncIterator[Message]:
        """Execute a pending pipeline request stored by _directive_pipeline."""
        pipeline = ctx.metadata.pop("pending_pipeline")
        stages = pipeline["stages"]
        task = pipeline["task"]

        self._set_state(ctx, AgentState.DELEGATING)

        from entropic.core.delegation import DelegationManager

        manager = DelegationManager(self, storage=self._storage)
        result = await manager.execute_pipeline(
            ctx,
            stages,
            task,
            parent_conversation_id=ctx.metadata.get("conversation_id"),
        )

        # Inject final result into parent context
        status = "COMPLETE" if result.success else "FAILED"
        stage_str = " → ".join(stages)
        result_msg = Message(
            role="user",
            content=(
                f"[PIPELINE {status}: {stage_str}] {result.summary}\n"
                f"(turns used: {result.turns_used})"
            ),
            metadata={"pipeline_result": True, "stages": stages},
        )
        ctx.messages.append(result_msg)
        yield result_msg

        self._set_state(ctx, AgentState.EXECUTING)

    def _should_auto_chain(self, ctx: LoopContext, finish_reason: str, content: str = "") -> bool:
        """Check whether auto-chain should trigger for the current tier.

        Triggers when auto_chain is set AND either:
          - finish_reason == "length" (token budget exhausted)
          - finish_reason == "stop" AND response is complete (no pending work)

        The is_response_complete() guard prevents premature chaining when
        the model emits a partial response (e.g. "Let me think...") with
        stop but no tool calls.
        """
        if not ctx.locked_tier or not self.orchestrator.get_tier_param(
            ctx.locked_tier, "auto_chain"
        ):
            return False
        triggered = finish_reason == "length" or (
            finish_reason == "stop"
            and self.orchestrator.get_adapter().is_response_complete(content, [])
        )
        if triggered:
            trigger = "token budget" if finish_reason == "length" else "complete response"
            logger.info("[AUTO_CHAIN] %s trigger", trigger)
        return triggered

    async def _try_auto_chain(
        self, ctx: LoopContext, finish_reason: str, content: str = ""
    ) -> bool:
        """Attempt auto-chain on token exhaustion or completed response.

        At depth 0 (root): auto_chain = TierChange to lead (existing behavior).
        At depth > 0 (child): auto_chain = COMPLETE (return to parent).

        Returns True if chain fired, False otherwise.
        """
        if not self._should_auto_chain(ctx, finish_reason, content):
            return False

        # In child delegation: auto_chain means "I'm done, return to parent"
        if ctx.delegation_depth > 0:
            logger.info("[AUTO_CHAIN] child depth=%d — completing delegation", ctx.delegation_depth)
            self._set_state(ctx, AgentState.COMPLETE)
            return True

        return await self._execute_root_auto_chain(ctx)

    async def _execute_root_auto_chain(self, ctx: LoopContext) -> bool:
        """Execute auto_chain TierChange at root depth. Returns True if chain fired."""
        target = await self._resolve_auto_chain_target(ctx)
        if target is None:
            return False

        from entropic.core.directives import TierChange

        directive = TierChange(tier=target.name, reason="auto_chain")
        result = DirectiveResult()
        self._directive_tier_change(ctx, directive, result)
        return result.tier_changed

    async def _resolve_auto_chain_target(self, ctx: LoopContext) -> ModelTier | None:
        """Resolve the target tier for auto_chain at depth 0."""
        chain_target = self.orchestrator.get_tier_param(ctx.locked_tier, "auto_chain")

        # Resolve named target tier (auto_chain is a tier name string)
        target = None
        if isinstance(chain_target, str):
            target = self.orchestrator._find_tier(chain_target)

        if target is not None:
            return target

        # Named tier not found — fall back to route_among
        targets = self.orchestrator.get_handoff_targets(ctx.locked_tier)
        if not targets:
            logger.warning(
                "[AUTO_CHAIN] auto_chain=%s but tier not found and "
                "no handoff targets configured",
                chain_target,
            )
            return None
        return await self.orchestrator.route_among(targets)

    def _ensure_tool_executor(self) -> ToolExecutor:
        """Create or return the ToolExecutor subsystem.

        Created lazily because server_manager may be None at __init__ time
        (it's set up in run() on first call).
        """
        if self._tool_executor is None:
            if self.server_manager is None:
                raise RuntimeError(
                    "ToolExecutor requires server_manager. "
                    "Provide it at construction or let run() initialize a default."
                )
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
        await self._context_manager.check_compaction(ctx)
        self._context_manager.inject_context_warning(ctx)

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

    async def _handle_error(self, ctx: LoopContext, error: Exception) -> AsyncIterator[Message]:
        """Handle loop iteration error."""
        # Context overflow: recover via forced compaction instead of counting as error
        if isinstance(error, ValueError) and "exceed context window" in str(error):
            logger.warning(f"[RECOVERY] Context overflow detected, forcing compaction: {error}")
            await self._context_manager.check_compaction(ctx, force=True)
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

    def context_usage(self, messages: list[Message]) -> tuple[int, int]:
        """Return (tokens_used, max_tokens) for the given message list.

        Public accessor for context usage — avoids consumers reaching
        into _token_counter directly.
        """
        used = self._token_counter.count_messages(messages)
        return used, self._token_counter.max_tokens

    @property
    def is_paused(self) -> bool:
        """Check if generation is paused."""
        return self._pause_event.is_set()

    async def connect_server(
        self,
        name: str,
        command: str | None = None,
        args: list[str] | None = None,
        sse_url: str | None = None,
    ) -> list[str]:
        """Connect to an external MCP server at runtime.

        Delegates to ServerManager.connect_server(). ServerManager must be
        initialized before calling this method.

        Args:
            name: Unique server name.
            command: stdio command (mutually exclusive with sse_url).
            args: Arguments for the stdio command.
            sse_url: SSE endpoint URL (mutually exclusive with command).

        Returns:
            List of tool names registered from the server.

        Raises:
            RuntimeError: If ServerManager is not initialized.
            ValueError: If name conflicts with an existing server.
        """
        if self.server_manager is None:
            raise RuntimeError("ServerManager not initialized — call run() first")
        return await self.server_manager.connect_server(
            name=name, command=command, args=args, sse_url=sse_url
        )

    async def disconnect_server(self, name: str) -> None:
        """Disconnect and remove a runtime-registered MCP server.

        Args:
            name: Server name to disconnect.

        Raises:
            RuntimeError: If ServerManager is not initialized.
            KeyError: If server is not found.
        """
        if self.server_manager is None:
            raise RuntimeError("ServerManager not initialized — call run() first")
        await self.server_manager.disconnect_server(name)
