"""Tool execution subsystem for the agentic loop engine.

Handles tool call processing, approval, duplicate detection,
and error recovery. Extracted from AgentEngine (P2-019).
"""

from __future__ import annotations

import asyncio
import json
import time
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass
from typing import Any

from entropic.config.loader import save_permission
from entropic.core.base import Message, ToolCall, ToolResult
from entropic.core.engine_types import (
    AgentState,
    EngineCallbacks,
    LoopConfig,
    LoopContext,
    ToolApproval,
)
from entropic.core.logging import get_logger
from entropic.inference.orchestrator import ModelOrchestrator
from entropic.mcp.manager import PermissionDeniedError, ServerManager

logger = get_logger("core.tool_executor")


@dataclass
class ToolExecutorHooks:
    """Engine-level hooks called during tool processing."""

    after_tool: Callable[[LoopContext], Any] | None = None
    """Async hook called after each tool execution (compaction, context warning)."""

    process_directives: Callable[[LoopContext, list[Any]], Any] | None = None
    """Hook to process directives from tool results. Returns DirectiveResult."""


class ToolExecutor:
    """Handles tool call execution, approval, and duplicate detection.

    Subsystem of AgentEngine. Holds references to shared dependencies
    and processes tool calls on behalf of the engine loop.
    """

    def __init__(
        self,
        server_manager: ServerManager,
        orchestrator: ModelOrchestrator,
        loop_config: LoopConfig,
        callbacks: EngineCallbacks,
        hooks: ToolExecutorHooks | None = None,
    ) -> None:
        """Initialize tool executor.

        Args:
            server_manager: MCP server manager for tool execution
            orchestrator: Model orchestrator for adapter access
            loop_config: Loop configuration (limits, auto-approve)
            callbacks: Shared mutable callback container
            hooks: Engine-level hooks for post-tool actions and directives
        """
        self._server_manager = server_manager
        self._orchestrator = orchestrator
        self._loop_config = loop_config
        self._callbacks = callbacks
        self._hooks = hooks or ToolExecutorHooks()

    def _set_state(self, ctx: LoopContext, state: AgentState) -> None:
        """Set agent state and notify callback."""
        ctx.state = state
        logger.info(f"State: {state.name}")
        if self._callbacks.on_state_change:
            self._callbacks.on_state_change(state)

    @staticmethod
    def _sort_tool_calls(tool_calls: list[ToolCall]) -> list[ToolCall]:
        """Sort tool calls so entropic.handoff is always last.

        Handoff fires ``stop_processing`` which drops subsequent calls,
        so it must be terminal in the batch.  Preserves relative order
        of all other calls (stable sort).
        """
        return sorted(tool_calls, key=lambda tc: tc.name == "entropic.handoff")

    async def process_tool_calls(
        self, ctx: LoopContext, tool_calls: list[ToolCall]
    ) -> AsyncIterator[Message]:
        """Process tool calls and yield result messages."""
        logger.info(f"Processing {len(tool_calls)} tool calls")
        self._set_state(ctx, AgentState.WAITING_TOOL)
        limited_calls = self._sort_tool_calls(
            tool_calls[: self._loop_config.max_tool_calls_per_turn]
        )

        for i, tool_call in enumerate(limited_calls):
            # Check for duplicate tool calls
            duplicate_result = self._check_duplicate_tool_call(ctx, tool_call)
            if duplicate_result:
                dup_msg = self._handle_duplicate(ctx, tool_call, duplicate_result)
                yield dup_msg
                if ctx.consecutive_duplicate_attempts >= 3:
                    return
                continue

            # Successful non-duplicate tool call - reset counter
            ctx.consecutive_duplicate_attempts = 0

            logger.debug(f"Executing tool {i + 1}/{len(limited_calls)}: {tool_call.name}")
            msg, tool_result = await self._execute_tool(ctx, tool_call)
            # Tag tool results with iteration for auto-pruning
            if tool_result:
                ctx.effective_tool_calls += 1
                msg.metadata["added_at_iteration"] = ctx.metrics.iterations
                msg.metadata["tool_name"] = tool_call.name
            ctx.messages.append(msg)
            yield msg

            self._record_tool_call(ctx, tool_call, msg)
            await self._run_post_tool_hooks(ctx, tool_result)

            if self._should_stop_on_directive(ctx, tool_result):
                return

        ctx.consecutive_errors = 0

    def _handle_duplicate(
        self, ctx: LoopContext, tool_call: ToolCall, previous_result: str
    ) -> Message:
        """Handle a duplicate tool call. Returns message to yield."""
        ctx.consecutive_duplicate_attempts += 1
        logger.warning(
            f"Duplicate tool call #{ctx.consecutive_duplicate_attempts}: {tool_call.name}"
        )

        if ctx.consecutive_duplicate_attempts >= 3:
            logger.error(
                f"Model stuck in loop - {ctx.consecutive_duplicate_attempts} "
                f"consecutive duplicate tool calls"
            )
            msg = Message(
                role="user",
                content="STOP: You have called the same tool 3 times with identical arguments. "
                "This indicates you are stuck. Please try a completely different approach "
                "or respond to the user explaining what's blocking you.",
            )
            ctx.messages.append(msg)
            logger.info(f"[FEEDBACK] Circuit breaker triggered: {msg.content}")
            return msg

        msg = self._create_duplicate_message(tool_call, previous_result)
        ctx.messages.append(msg)
        logger.info(f"[FEEDBACK] Duplicate message sent: {msg.content[:100]}...")
        return msg

    async def _run_post_tool_hooks(self, ctx: LoopContext, tool_result: Any) -> None:
        """Run post-tool engine hooks (compaction, context warning)."""
        if self._hooks.after_tool:
            result = self._hooks.after_tool(ctx)
            if asyncio.iscoroutine(result):
                await result

    def _should_stop_on_directive(self, ctx: LoopContext, tool_result: Any) -> bool:
        """Check if directives require stopping tool processing."""
        if not (tool_result and tool_result.directives and self._hooks.process_directives):
            return False
        directive_result = self._hooks.process_directives(ctx, tool_result.directives)
        if directive_result.stop_processing:
            logger.info("[DIRECTIVE] stop_processing — halting tool call loop")
            return True
        return False

    def _get_tool_call_key(self, tool_call: ToolCall) -> str:
        """Generate a unique key for a tool call based on name and arguments."""
        args_str = json.dumps(tool_call.arguments, sort_keys=True, default=str)
        return f"{tool_call.name}:{args_str}"

    def _check_duplicate_tool_call(self, ctx: LoopContext, tool_call: ToolCall) -> str | None:
        """Check if this tool call is a duplicate. Returns previous result if duplicate."""
        # Delegate to server's skip_duplicate_check for tools with side effects
        if self._server_manager.skip_duplicate_check(tool_call):
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

        # Enforce tier-level allowed_tools (defense against hallucinated calls)
        if rejection := self._check_tier_allowed(ctx, tool_call):
            return rejection, None

        # Execute the tool (all tools, including entropic.*, go through ServerManager)
        if self._callbacks.on_tool_start:
            self._callbacks.on_tool_start(tool_call)
        return await self._do_execute_tool(ctx, tool_call)

    def _check_tier_allowed(self, ctx: LoopContext, tool_call: ToolCall) -> Message | None:
        """Reject tool calls not in the current tier's allowed_tools.

        Returns a feedback message if rejected, None if allowed.
        """
        if ctx.locked_tier is None:
            return None
        allowed = self._orchestrator.get_allowed_tools(ctx.locked_tier)
        if allowed is None or tool_call.name in allowed:
            return None
        tier_name = getattr(ctx.locked_tier, "name", str(ctx.locked_tier))
        logger.warning(
            "[TOOL BLOCKED] %s not in %s tier's allowed_tools", tool_call.name, tier_name
        )
        reason = (
            f"Tool `{tool_call.name}` is not available in the {tier_name} tier. "
            f"Available tools: {sorted(allowed)}"
        )
        return self._create_denied_message(tool_call, reason)

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
            result = await self._server_manager.execute(tool_call)
            ctx.metrics.tool_calls += 1
            duration_ms = (time.time() - start_time) * 1000
            self._log_tool_success(ctx, tool_call, result.result, duration_ms)
            msg = self._orchestrator.get_adapter().format_tool_result(tool_call, result.result)
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
        if self._callbacks.on_tool_complete:
            self._callbacks.on_tool_complete(tool_call, result, duration_ms)
        if ctx.task_id and self._callbacks.on_tool_record:
            self._callbacks.on_tool_record(ctx.task_id, tool_call, "success", result, duration_ms)

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
        raw_error = str(error)

        # Log raw error (always unfiltered for diagnostics)
        if is_permission:
            logger.warning(f"Tool permission denied: {tool_call.name} - {raw_error}")
            display_msg = f"Permission denied: {raw_error}"
        else:
            logger.error(f"Tool execution error: {tool_call.name} - {raw_error}")
            display_msg = f"Error: {raw_error}"

        if self._callbacks.on_tool_complete:
            self._callbacks.on_tool_complete(tool_call, display_msg, duration_ms)
        if ctx.task_id and self._callbacks.on_tool_record:
            self._callbacks.on_tool_record(ctx.task_id, tool_call, "error", raw_error, duration_ms)

        # Sanitize before sending to model — consumer controls what leaks
        sanitizer = self._callbacks.error_sanitizer
        model_error = sanitizer(raw_error) if sanitizer else raw_error

        msg = (
            self._create_denied_message(tool_call, model_error)
            if is_permission
            else self._create_error_message(tool_call, model_error)
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
        if self._loop_config.auto_approve_tools:
            return True
        if self._server_manager.is_explicitly_allowed(tool_call):
            return True
        return self._callbacks.on_tool_call is None  # No callback = headless mode

    async def _get_approval_result(self, tool_call: ToolCall) -> bool | str | ToolApproval:
        """Get approval result from callback."""
        assert self._callbacks.on_tool_call is not None
        result = self._callbacks.on_tool_call(tool_call)
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
            self._server_manager.add_permission(pattern, allow=allowed)

        return allowed

    def _get_permission_pattern(self, tool_call: ToolCall) -> str:
        """Generate permission pattern via server class inheritance.

        Delegates to ServerManager which routes to the server
        class's get_permission_pattern() for proper granularity.
        """
        return self._server_manager.get_permission_pattern(
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
