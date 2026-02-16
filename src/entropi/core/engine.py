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
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import TYPE_CHECKING, Any

from entropi.config.loader import save_permission
from entropi.config.schema import EntropyConfig
from entropi.core.base import Message, ToolCall, ToolResult
from entropi.core.compaction import CompactionManager, CompactionResult, TokenCounter
from entropi.core.context import ContextBuilder
from entropi.core.directives import DirectiveProcessor, DirectiveResult
from entropi.core.logging import get_logger, get_model_logger
from entropi.core.queue import MessageSource
from entropi.inference.orchestrator import ModelOrchestrator, RoutingResult
from entropi.mcp.manager import PermissionDeniedError, ServerManager

if TYPE_CHECKING:
    from entropi.core.directives import (
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


class AgentState(Enum):
    """Agent execution states."""

    IDLE = auto()
    PLANNING = auto()
    EXECUTING = auto()
    WAITING_TOOL = auto()
    VERIFYING = auto()
    COMPLETE = auto()
    ERROR = auto()
    INTERRUPTED = auto()
    PAUSED = auto()  # Generation paused, awaiting user input


class InterruptMode(Enum):
    """How to handle generation interrupt."""

    CANCEL = "cancel"  # Discard partial response, stop
    PAUSE = "pause"  # Keep partial response, await input
    INJECT = "inject"  # Keep partial, inject context, continue


class ToolApproval(Enum):
    """Tool approval responses from user."""

    DENY = "deny"  # Deny this once
    ALLOW = "allow"  # Allow this once
    ALWAYS_DENY = "always_deny"  # Deny and save to config
    ALWAYS_ALLOW = "always_allow"  # Allow and save to config


@dataclass
class LoopConfig:
    """Configuration for the agentic loop."""

    max_iterations: int = 15
    max_consecutive_errors: int = 3
    max_tool_calls_per_turn: int = 10
    idle_timeout_seconds: int = 300
    require_plan_for_complex: bool = True
    stream_output: bool = True
    auto_approve_tools: bool = False


@dataclass
class LoopMetrics:
    """Metrics collected during loop execution."""

    iterations: int = 0
    tool_calls: int = 0
    tokens_used: int = 0
    errors: int = 0
    start_time: float = 0.0
    end_time: float = 0.0

    @property
    def duration_ms(self) -> int:
        """Get duration in milliseconds."""
        return int((self.end_time - self.start_time) * 1000)


@dataclass
class LoopContext:
    """Context maintained during loop execution."""

    messages: list[Message] = field(default_factory=list)
    pending_tool_calls: list[ToolCall] = field(default_factory=list)
    state: AgentState = AgentState.IDLE
    metrics: LoopMetrics = field(default_factory=LoopMetrics)
    consecutive_errors: int = 0
    # Track recent tool calls to prevent duplicates (key: "name:args_hash")
    recent_tool_calls: dict[str, str] = field(default_factory=dict)
    # Track consecutive duplicate attempts to detect stuck model
    consecutive_duplicate_attempts: int = 0
    # Flag indicating we have tool results that should be presented
    has_pending_tool_results: bool = False
    # Lock model tier for the entire loop to prevent mid-task switching
    locked_tier: Any = None  # ModelTier or None
    # External task tracking (for MCP integration)
    task_id: str | None = None  # Associated task ID if from external source
    source: str = MessageSource.HUMAN  # Message source (human, claude-code)
    # Stored for system prompt rebuild on tier change
    all_tools: list[dict[str, Any]] = field(default_factory=list)
    base_system: str = ""
    # General-purpose metadata for runtime state (e.g., warning tracking)
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass
class InterruptContext:
    """Context for interrupted/paused generation."""

    partial_content: str = ""
    partial_tool_calls: list[ToolCall] = field(default_factory=list)
    injection: str | None = None
    mode: InterruptMode = InterruptMode.PAUSE


@dataclass
class EngineCallbacks:
    """Callback configuration for engine events."""

    on_state_change: Callable[[AgentState], None] | None = None
    on_tool_call: Callable[[ToolCall], Any] | None = None
    on_stream_chunk: Callable[[str], None] | None = None
    on_tool_start: Callable[[ToolCall], None] | None = None
    on_tool_complete: Callable[[ToolCall, str, float], None] | None = None
    on_presenter_notify: Callable[[str, dict[str, Any]], None] | None = None
    on_compaction: Callable[[CompactionResult], None] | None = None
    on_pause_prompt: Callable[[str], Any] | None = None
    on_tool_record: Callable[[str, ToolCall, str, str | None, float], None] | None = None
    on_tier_selected: Callable[[str], None] | None = None
    on_routing_complete: Callable[[RoutingResult], None] | None = None


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
        server_manager: ServerManager,
        config: EntropyConfig,
        loop_config: LoopConfig | None = None,
    ) -> None:
        """
        Initialize agent engine.

        Args:
            orchestrator: Model orchestrator
            server_manager: MCP server manager
            config: Application configuration
            loop_config: Loop-specific configuration
        """
        self.orchestrator = orchestrator
        self.server_manager = server_manager
        self.config = config
        self.loop_config = loop_config or LoopConfig()

        self._context_builder = ContextBuilder(config)
        # Use threading.Event for thread-safe cross-loop signaling
        # (Textual runs on a different event loop than the generation worker)
        self._interrupt_event = threading.Event()

        # Pause/inject support for interrupting generation
        self._pause_event = threading.Event()
        self._interrupt_context: InterruptContext | None = None

        # Callback for pause/inject prompting
        self._on_pause_prompt: Callable[[str], Any] | None = None

        # Generic context anchors — keyed persistent messages in ctx.messages
        self._context_anchors: dict[str, str] = {}

        # Directive processor for tool-to-engine communication
        self._directive_processor = DirectiveProcessor()
        self._register_directive_handlers()

        # Compaction manager for context management
        max_tokens = self._get_max_context_tokens()
        self._token_counter = TokenCounter(max_tokens)
        self._compaction_manager = CompactionManager(
            config=config.compaction,
            token_counter=self._token_counter,
            orchestrator=orchestrator,
        )

        # Callbacks
        self._on_state_change: Callable[[AgentState], None] | None = None
        self._on_tool_approval: Callable[[ToolCall], Any] | None = None  # Can be sync or async
        self._on_stream_chunk: Callable[[str], None] | None = None
        self._on_tool_start: Callable[[ToolCall], None] | None = None
        self._on_tool_complete: Callable[[ToolCall, str, float], None] | None = None
        self._on_presenter_notify: Callable[[str, dict[str, Any]], None] | None = None
        self._on_compaction: Callable[[CompactionResult], None] | None = None
        # Task tracking callback for external MCP integration
        self._on_tool_record: Callable[[str, ToolCall, str, str | None, float], None] | None = None
        self._on_tier_selected: Callable[[str], None] | None = None
        self._on_routing_complete: Callable[[RoutingResult], None] | None = None

    def _register_directive_handlers(self) -> None:
        """Register handlers for all known directive types."""
        from entropi.core.directives import (
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
        from entropi.inference.orchestrator import ModelTier

        target_tier_str = directive.tier
        reason = directive.reason

        try:
            target_tier = ModelTier(target_tier_str)
        except ValueError:
            logger.error(f"[DIRECTIVE] Invalid tier in tier_change: {target_tier_str}")
            return

        if not self.orchestrator.can_handoff(ctx.locked_tier, target_tier):
            current_name = ctx.locked_tier.value if ctx.locked_tier else "none"
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

        current_name = current_tier.value if current_tier else "none"
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

        In the directive architecture, the EntropiServer owns the TodoList
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
        if self._on_presenter_notify:
            self._on_presenter_notify(directive.key, directive.data)

    def _get_max_context_tokens(self) -> int:
        """Get maximum context tokens from model config."""
        # Try to get from the default model config
        default_model = self.config.models.default
        model_config = getattr(self.config.models, default_model, None)
        if model_config and hasattr(model_config, "context_length"):
            return int(model_config.context_length)
        # Fallback default
        return 16384

    def _refresh_context_limit(self) -> None:
        """Refresh context limit based on current model.

        When models swap (e.g., normal → thinking), the context limit
        may change. This method updates the token counter to use the
        current model's context length.
        """
        # Get context length from last used tier's model config
        last_tier = self.orchestrator.last_used_tier
        if last_tier is None:
            return

        # Map tier to config attribute name
        tier_to_config = {
            "thinking": self.config.models.thinking,
            "normal": self.config.models.normal,
            "code": self.config.models.code,
            "simple": self.config.models.simple,
        }

        model_config = tier_to_config.get(last_tier.value)
        if model_config and hasattr(model_config, "context_length"):
            new_max = model_config.context_length
            if new_max != self._token_counter.max_tokens:
                logger.debug(
                    f"Updating context limit: {self._token_counter.max_tokens} -> {new_max}"
                )
                self._token_counter.max_tokens = new_max

    def set_callbacks(self, callbacks: EngineCallbacks) -> None:
        """Set callback functions for loop events.

        Args:
            callbacks: Callback configuration for engine events
        """
        self._on_state_change = callbacks.on_state_change
        self._on_tool_approval = callbacks.on_tool_call
        self._on_stream_chunk = callbacks.on_stream_chunk
        self._on_tool_start = callbacks.on_tool_start
        self._on_tool_complete = callbacks.on_tool_complete
        self._on_presenter_notify = callbacks.on_presenter_notify
        self._on_compaction = callbacks.on_compaction
        self._on_pause_prompt = callbacks.on_pause_prompt
        self._on_tool_record = callbacks.on_tool_record
        self._on_tier_selected = callbacks.on_tier_selected
        self._on_routing_complete = callbacks.on_routing_complete

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

        # Build base system prompt (before adapter formatting)
        system = self._context_builder.build_system_prompt(system_prompt or "")

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
                async for msg in self._process_tool_calls(ctx, tool_calls):
                    yield msg
            else:
                # Check if generation was cut off due to token limit
                finish_reason = self.orchestrator.last_finish_reason
                if finish_reason == "length":
                    logger.info("[LOOP DECISION] finish_reason=length, continuing loop")
                    # Don't mark complete, let loop continue
                else:
                    # No tool calls - check if response is complete using adapter
                    adapter = self.orchestrator.get_adapter()
                    is_complete = adapter.is_response_complete(content, tool_calls)
                    logger.info(f"[LOOP DECISION] tool_calls=0, is_response_complete={is_complete}")
                    if is_complete:
                        self._set_state(ctx, AgentState.COMPLETE)
                    # else: continue loop (model may still be thinking)

        except Exception as e:
            logger.exception(f"Loop iteration error: {e}")
            async for msg in self._handle_error(ctx, e):
                yield msg

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
        tier_value = ctx.locked_tier.value if ctx.locked_tier else "none"
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
        return adapter.format_system_prompt(ctx.base_system, filtered)

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
        model_logger.info(f"\n{'#' * 70}\n" f"[ROUTED] tier={tier.value}\n" f"{'#' * 70}")
        self._log_assembled_prompt(ctx, "routed")

        if self._on_tier_selected:
            self._on_tier_selected(tier.value)
        routing_result = self.orchestrator.last_routing_result
        if routing_result and self._on_routing_complete:
            self._on_routing_complete(routing_result)

    def _notify_tier_selected(self, tier_value: str) -> None:
        """Notify callback of tier selection (for handoff)."""
        if self._on_tier_selected:
            self._on_tier_selected(tier_value)

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

            if self._on_stream_chunk:
                self._on_stream_chunk(chunk)

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
            raw_content=result.content,
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
        if self._on_pause_prompt:
            result = self._on_pause_prompt(partial_content)
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
            if self._on_stream_chunk:
                self._on_stream_chunk(chunk)

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

    async def _process_tool_calls(
        self, ctx: LoopContext, tool_calls: list[ToolCall]
    ) -> AsyncIterator[Message]:
        """Process tool calls and yield result messages."""
        logger.info(f"Processing {len(tool_calls)} tool calls")
        self._set_state(ctx, AgentState.WAITING_TOOL)
        limited_calls = tool_calls[: self.loop_config.max_tool_calls_per_turn]

        for i, tool_call in enumerate(limited_calls):
            # Check for duplicate tool calls
            duplicate_result = self._check_duplicate_tool_call(ctx, tool_call)
            if duplicate_result:
                ctx.consecutive_duplicate_attempts += 1
                logger.warning(
                    f"Duplicate tool call #{ctx.consecutive_duplicate_attempts}: {tool_call.name}"
                )

                # Circuit breaker: if model is stuck in loop, force break
                if ctx.consecutive_duplicate_attempts >= 3:
                    logger.error(
                        f"Model stuck in loop - {ctx.consecutive_duplicate_attempts} "
                        f"consecutive duplicate tool calls"
                    )
                    break_msg = Message(
                        role="user",
                        content="STOP: You have called the same tool 3 times with identical arguments. "
                        "This indicates you are stuck. Please try a completely different approach "
                        "or respond to the user explaining what's blocking you.",
                    )
                    ctx.messages.append(break_msg)
                    logger.info(f"[FEEDBACK] Circuit breaker triggered: {break_msg.content}")
                    yield break_msg
                    return  # Exit tool processing entirely

                msg = self._create_duplicate_message(tool_call, duplicate_result)
                ctx.messages.append(msg)
                logger.info(f"[FEEDBACK] Duplicate message sent: {msg.content[:100]}...")
                yield msg
                continue

            # Successful non-duplicate tool call - reset counter
            ctx.consecutive_duplicate_attempts = 0

            logger.debug(f"Executing tool {i + 1}/{len(limited_calls)}: {tool_call.name}")
            msg, tool_result = await self._execute_tool(ctx, tool_call)
            # Tag tool results with iteration for auto-pruning
            if tool_result:
                msg.metadata["added_at_iteration"] = ctx.metrics.iterations
                msg.metadata["tool_name"] = tool_call.name
            ctx.messages.append(msg)
            yield msg

            # Track this tool call for duplicate detection
            self._record_tool_call(ctx, tool_call, msg)

            # Check compaction after each tool result to catch large results
            await self._check_compaction(ctx)

            # Warn if context usage is high after this tool result
            self._inject_context_warning(ctx)

            # Process directives from tool result (tier changes, pruning, etc.)
            if tool_result and tool_result.directives:
                directive_result = self._process_directives(ctx, tool_result.directives)
                if directive_result.stop_processing:
                    logger.info("[DIRECTIVE] stop_processing — halting tool call loop")
                    return

        ctx.consecutive_errors = 0

    def _get_tool_call_key(self, tool_call: ToolCall) -> str:
        """Generate a unique key for a tool call based on name and arguments."""
        args_str = json.dumps(tool_call.arguments, sort_keys=True, default=str)
        return f"{tool_call.name}:{args_str}"

    def _check_duplicate_tool_call(self, ctx: LoopContext, tool_call: ToolCall) -> str | None:
        """Check if this tool call is a duplicate. Returns previous result if duplicate."""
        # Delegate to server's skip_duplicate_check for tools with side effects
        if self.server_manager.skip_duplicate_check(tool_call):
            return None

        key = self._get_tool_call_key(tool_call)
        return ctx.recent_tool_calls.get(key)

    def _record_tool_call(self, ctx: LoopContext, tool_call: ToolCall, result_msg: Message) -> None:
        """Record a tool call for duplicate detection."""
        key = self._get_tool_call_key(tool_call)
        # Store the result content for reuse
        ctx.recent_tool_calls[key] = result_msg.content

    def _create_duplicate_message(self, tool_call: ToolCall, previous_result: str) -> Message:
        """Create a message indicating a duplicate tool call was skipped.

        Uses role="user" because llama-cpp doesn't render role="tool" properly,
        which would cause the model to never see the feedback and loop forever.
        """
        content = f"""Tool `{tool_call.name}` was already called with the same arguments.

Previous result:
{previous_result}

Do NOT call this tool again. Use the previous result above."""

        return Message(role="user", content=content)

    async def _execute_tool(
        self, ctx: LoopContext, tool_call: ToolCall
    ) -> tuple[Message, ToolResult | None]:
        """Execute a single tool call.

        All tools go through the same path: approval → start callback →
        execute via ServerManager → complete callback. No tool-specific
        handlers in the engine.

        Returns:
            Tuple of (message for context, ToolResult with directives or None if denied)
        """
        logger.info(f"[TOOL CALL] {tool_call.name}")
        logger.info(f"[TOOL ARGS] {json.dumps(tool_call.arguments, indent=2, default=str)}")

        # Check approval (returns denial message if not approved)
        if denial_msg := await self._check_tool_approval(tool_call):
            return denial_msg, None

        # Execute the tool (all tools, including entropi.*, go through ServerManager)
        if self._on_tool_start:
            self._on_tool_start(tool_call)
        return await self._do_execute_tool(ctx, tool_call)

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

    async def _check_tool_approval(self, tool_call: ToolCall) -> Message | None:
        """Check tool approval. Returns denial message if not approved, None if approved."""
        approval_result = await self._is_tool_approved(tool_call)
        if approval_result is True:
            return None
        # Denied - format reason based on whether feedback was provided
        if isinstance(approval_result, str):
            logger.warning(f"Tool call denied with feedback: {tool_call.name}")
            reason = f"Permission denied by user. Feedback: {approval_result}"
        else:
            logger.warning(f"Tool call denied by user: {tool_call.name}")
            reason = "Permission denied by user"
        denied_msg = self._create_denied_message(tool_call, reason)
        logger.info(f"[FEEDBACK] Denied message sent: {denied_msg.content[:100]}...")
        return denied_msg

    async def _do_execute_tool(
        self, ctx: LoopContext, tool_call: ToolCall
    ) -> tuple[Message, ToolResult]:
        """Execute tool and return both the formatted message and raw result.

        The Message goes into ctx.messages for model context.
        The ToolResult carries directives for engine-level side effects.
        """
        start_time = time.time()
        try:
            logger.debug(f"Executing tool via server manager: {tool_call.name}")
            result = await self.server_manager.execute(tool_call)
            ctx.metrics.tool_calls += 1
            duration_ms = (time.time() - start_time) * 1000
            self._log_tool_success(ctx, tool_call, result.result, duration_ms)
            msg = self.orchestrator.get_adapter().format_tool_result(tool_call, result.result)
            return msg, result
        except PermissionDeniedError as e:
            duration_ms = (time.time() - start_time) * 1000
            msg = self._handle_tool_error(ctx, tool_call, e, duration_ms, is_permission=True)
            return msg, ToolResult(
                call_id=tool_call.id,
                name=tool_call.name,
                result=str(e),
                is_error=True,
            )
        except Exception as e:
            duration_ms = (time.time() - start_time) * 1000
            msg = self._handle_tool_error(ctx, tool_call, e, duration_ms, is_permission=False)
            return msg, ToolResult(
                call_id=tool_call.id,
                name=tool_call.name,
                result=str(e),
                is_error=True,
            )

    def _log_tool_success(
        self, ctx: LoopContext, tool_call: ToolCall, result: str, duration_ms: float
    ) -> None:
        """Log and notify about successful tool execution."""
        result_str = str(result)
        logger.debug(f"Tool result ({duration_ms:.0f}ms):\n{result_str}")
        logger.info(
            f"\n{'=' * 60}\n[TOOL RESULT: {tool_call.name}]\n{'=' * 60}\n{result_str}\n{'=' * 60}"
        )
        logger.info(
            f"[TOOL COMPLETE] {tool_call.name} -> {len(result_str)} chars result ({duration_ms:.0f}ms)"
        )
        if self._on_tool_complete:
            self._on_tool_complete(tool_call, result, duration_ms)
        if ctx.task_id and self._on_tool_record:
            self._on_tool_record(ctx.task_id, tool_call, "success", result, duration_ms)

    def _handle_tool_error(
        self,
        ctx: LoopContext,
        tool_call: ToolCall,
        error: Exception,
        duration_ms: float,
        *,
        is_permission: bool,
    ) -> Message:
        """Handle tool execution error and return appropriate message."""
        error_str = str(error)
        if is_permission:
            logger.warning(f"Tool permission denied: {tool_call.name} - {error_str}")
            display_msg = f"Permission denied: {error_str}"
        else:
            logger.error(f"Tool execution error: {tool_call.name} - {error_str}")
            display_msg = f"Error: {error_str}"

        if self._on_tool_complete:
            self._on_tool_complete(tool_call, display_msg, duration_ms)
        if ctx.task_id and self._on_tool_record:
            self._on_tool_record(ctx.task_id, tool_call, "error", error_str, duration_ms)

        msg = (
            self._create_denied_message(tool_call, error_str)
            if is_permission
            else self._create_error_message(tool_call, error_str)
        )
        logger.info(f"[FEEDBACK] {'Permission denied' if is_permission else 'Error'} message sent")
        return msg

    async def _is_tool_approved(self, tool_call: ToolCall) -> bool | str:
        """Check if tool call is approved.

        Approval flow:
        1. If auto_approve is set, always approve
        2. If tool is explicitly in allow list, skip prompting
        3. Otherwise, prompt user via callback
        4. If user selects "Always allow/deny", save to config

        Returns:
            True if approved, False if denied, or str with feedback if denied with feedback.
        """
        # Early approval checks (no prompting needed)
        if self._should_auto_approve(tool_call):
            return True

        # Prompt user via callback and convert result
        result = await self._get_approval_result(tool_call)
        return self._convert_approval_result(tool_call, result)

    def _convert_approval_result(
        self, tool_call: ToolCall, result: bool | str | ToolApproval
    ) -> bool | str:
        """Convert callback result to approval decision."""
        if isinstance(result, str):
            return result  # Feedback string as denial reason
        if isinstance(result, ToolApproval):
            return self._handle_approval_result(tool_call, result)
        return bool(result)  # Legacy bool support

    def _should_auto_approve(self, tool_call: ToolCall) -> bool:
        """Check if tool should be auto-approved without prompting."""
        if self.loop_config.auto_approve_tools:
            return True
        if self.server_manager.is_explicitly_allowed(tool_call):
            return True
        return self._on_tool_approval is None  # No callback = headless mode

    async def _get_approval_result(self, tool_call: ToolCall) -> bool | str | ToolApproval:
        """Get approval result from callback."""
        assert self._on_tool_approval is not None
        result = self._on_tool_approval(tool_call)
        if asyncio.iscoroutine(result):
            result = await result
        return result

    def _handle_approval_result(self, tool_call: ToolCall, approval: ToolApproval) -> bool:
        """Handle ToolApproval result and save to config if needed."""
        allowed = approval in (ToolApproval.ALLOW, ToolApproval.ALWAYS_ALLOW)

        # Save permanent permissions
        if approval in (ToolApproval.ALWAYS_ALLOW, ToolApproval.ALWAYS_DENY):
            pattern = self._get_permission_pattern(tool_call)
            logger.info(f"Saving 'always {'allow' if allowed else 'deny'}' permission: {pattern}")
            save_permission(pattern, allow=allowed)
            self.server_manager.add_permission(pattern, allow=allowed)

        return allowed

    def _get_permission_pattern(self, tool_call: ToolCall) -> str:
        """Generate permission pattern via server class inheritance.

        Delegates to ServerManager which routes to the server
        class's get_permission_pattern() for proper granularity.
        """
        return self.server_manager.get_permission_pattern(
            tool_call.name,
            tool_call.arguments,
        )

    def _create_denied_message(self, tool_call: ToolCall, reason: str) -> Message:
        """Create a permission denied message.

        Uses role="user" because llama-cpp doesn't render role="tool" properly,
        which would cause the model to never see the feedback and loop forever.
        """
        content = f"""Tool `{tool_call.name}` was denied: {reason}

Try a different approach or ask the user for clarification."""

        return Message(role="user", content=content)

    def _create_error_message(self, tool_call: ToolCall, error: str) -> Message:
        """Create a structured error message for the model to recover from.

        Uses role="user" because llama-cpp doesn't render role="tool" properly,
        which would cause the model to never see the feedback and loop forever.
        """
        content = f"""Tool `{tool_call.name}` failed with error: {error}

RECOVERY:
- Check arguments are correct
- Try a different approach
- Do NOT retry with the same arguments"""

        return Message(role="user", content=content)

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
            f"Capture findings with entropi.todo_write if needed, "
            f"then call entropi.prune_context."
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
        if self._on_state_change:
            self._on_state_change(state)

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
            if self._on_compaction:
                self._on_compaction(result)
            # Reinject context anchors so model retains awareness
            self._reinject_context_anchors(ctx)

    def _reinject_context_anchors(self, ctx: LoopContext) -> None:
        """Reinject all cached context anchors into ctx.messages.

        Called after compaction, tier change, or at run() start to ensure
        anchored state survives context mutations. Each anchor is a single
        keyed message — zero context growth per anchor.
        """
        from entropi.core.directives import ContextAnchor

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
