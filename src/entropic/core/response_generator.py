"""Response generation subsystem for the agentic loop engine.

Handles model invocation, streaming, tier routing, system prompt
assembly, and pause/injection. Extracted from AgentEngine (P2-019).
"""

from __future__ import annotations

import asyncio
import json
from typing import TYPE_CHECKING, Any

from entropic.config.schema import EntropyConfig
from entropic.core.base import Message, ToolCall
from entropic.core.engine_types import (
    AgentState,
    EngineCallbacks,
    GenerationEvents,
    InterruptContext,
    InterruptMode,
    LoopConfig,
    LoopContext,
)
from entropic.core.logging import get_logger, get_model_logger
from entropic.inference.orchestrator import ModelOrchestrator

if TYPE_CHECKING:
    pass

logger = get_logger("core.response_generator")
model_logger = get_model_logger()


class ResponseGenerator:
    """Handles model response generation, tier routing, and pause/injection.

    Subsystem of AgentEngine. Holds references to shared dependencies
    and generates responses on behalf of the engine loop.
    """

    def __init__(
        self,
        orchestrator: ModelOrchestrator,
        config: EntropyConfig,
        loop_config: LoopConfig,
        callbacks: EngineCallbacks,
        events: GenerationEvents,
    ) -> None:
        """Initialize response generator.

        Args:
            orchestrator: Model orchestrator for tier routing and generation
            config: Application configuration (tier prompts, inject_model_context)
            loop_config: Loop configuration (stream_output flag)
            callbacks: Shared mutable callback container
            events: Threading events for interrupt and pause signaling
        """
        self._orchestrator = orchestrator
        self._config = config
        self._loop_config = loop_config
        self._callbacks = callbacks
        self._interrupt_event = events.interrupt
        self._pause_event = events.pause
        self._interrupt_context: InterruptContext | None = None

    # ------------------------------------------------------------------
    # Tier routing and system prompt assembly
    # ------------------------------------------------------------------

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
        allowed = self._orchestrator.get_allowed_tools(tier)
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
        adapter = self._orchestrator.get_adapter(tier)
        tier_config = self._config.models.tiers.get(tier.name) if hasattr(tier, "name") else None
        identity_fm = (
            self._orchestrator._prompt_manager.get_identity_frontmatter(tier.name)
            if hasattr(tier, "name")
            else None
        )
        enable_thinking = identity_fm.enable_thinking if identity_fm is not None else False

        system = ctx.base_system
        if tier_config and self._config.inject_model_context:
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
        tier = await self._orchestrator.route(ctx.messages)
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
        routing_result = self._orchestrator.last_routing_result
        if routing_result and self._callbacks.on_routing_complete:
            self._callbacks.on_routing_complete(routing_result)

    def _notify_tier_selected(self, tier_value: str) -> None:
        """Notify callback of tier selection (for handoff)."""
        if self._callbacks.on_tier_selected:
            self._callbacks.on_tier_selected(tier_value)

    # ------------------------------------------------------------------
    # Logging helpers
    # ------------------------------------------------------------------

    def _log_model_output(
        self,
        ctx: LoopContext,
        raw_content: str,
        cleaned_content: str,
        tool_calls: list[ToolCall],
        finish_reason: str,
    ) -> None:
        """Log raw and parsed model output to dedicated model log."""
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

    # ------------------------------------------------------------------
    # Response generation
    # ------------------------------------------------------------------

    async def generate_response(self, ctx: LoopContext) -> tuple[str, list[ToolCall]]:
        """Generate model response, streaming or not.

        Routes first (if no tier locked), emits routing callback,
        then generates with the resolved tier.
        """
        logger.debug(f"Generating response (stream={self._loop_config.stream_output})")

        # Route and lock tier before generation starts
        await self._lock_tier_if_needed(ctx)

        if self._loop_config.stream_output:
            return await self._generate_streaming(ctx)

        return await self._generate_non_streaming(ctx)

    def _get_identity_fm(self, ctx: LoopContext) -> Any:
        """Look up identity frontmatter for the locked tier."""
        if ctx.locked_tier is None or not hasattr(ctx.locked_tier, "name"):
            return None
        return self._orchestrator._prompt_manager.get_identity_frontmatter(ctx.locked_tier.name)

    async def _generate_streaming(self, ctx: LoopContext) -> tuple[str, list[ToolCall]]:
        """Generate response via streaming."""
        content = ""
        identity_fm = self._get_identity_fm(ctx)

        interrupted = False
        async for chunk in self._orchestrator.generate_stream(
            ctx.messages, tier=ctx.locked_tier, identity_fm=identity_fm
        ):
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

        cleaned_content, tool_calls = self._orchestrator.get_adapter().parse_tool_calls(content)
        self._log_model_output(
            ctx,
            raw_content=content,
            cleaned_content=cleaned_content,
            tool_calls=tool_calls,
            finish_reason="interrupted" if interrupted else self._orchestrator.last_finish_reason,
        )
        return cleaned_content, tool_calls

    async def _generate_non_streaming(self, ctx: LoopContext) -> tuple[str, list[ToolCall]]:
        """Generate response without streaming."""
        identity_fm = self._get_identity_fm(ctx)
        result = await self._orchestrator.generate(
            ctx.messages, tier=ctx.locked_tier, identity_fm=identity_fm
        )

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
        """Handle pause during generation.

        Prompts user for injection and either continues with context or resumes.
        """
        # Save partial state
        self._interrupt_context = InterruptContext(
            partial_content=partial_content,
            mode=InterruptMode.PAUSE,
        )
        # Update state: paused
        ctx.state = AgentState.PAUSED
        logger.info(f"State: {AgentState.PAUSED.name}")
        if self._callbacks.on_state_change:
            self._callbacks.on_state_change(AgentState.PAUSED)

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
        ctx.state = AgentState.EXECUTING
        logger.info(f"State: {AgentState.EXECUTING.name}")
        if self._callbacks.on_state_change:
            self._callbacks.on_state_change(AgentState.EXECUTING)
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
        ctx.state = AgentState.EXECUTING
        logger.info(f"State: {AgentState.EXECUTING.name}")
        if self._callbacks.on_state_change:
            self._callbacks.on_state_change(AgentState.EXECUTING)

        # Generate new response with injected context
        content = ""
        identity_fm = self._get_identity_fm(ctx)
        async for chunk in self._orchestrator.generate_stream(
            ctx.messages, tier=ctx.locked_tier, identity_fm=identity_fm
        ):
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

    # ------------------------------------------------------------------
    # Message creation
    # ------------------------------------------------------------------

    @staticmethod
    def create_assistant_message(content: str, tool_calls: list[ToolCall]) -> Message:
        """Create assistant message with tool calls.

        Note: Empty assistant messages can cause llama_decode errors with some models
        (especially Falcon H1R). When content is empty but tool calls exist, we preserve
        a representation of the tool calls to ensure valid ChatML formatting.
        """
        # Prevent empty assistant messages which can cause KV cache issues
        if not content.strip() and tool_calls:
            # Use proper <tool_call> format so model learns correct pattern
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
